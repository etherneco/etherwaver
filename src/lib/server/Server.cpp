/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) 2012-2016 Symless Ltd.
 * Copyright (C) 2002 Chris Schoeneman
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "server/Server.h"

#include "server/ClientProxy.h"
#include "server/ClientProxyUnknown.h"
#include "server/PrimaryClient.h"
#include "server/ClientListener.h"
#include "barrier/FileChunk.h"
#include "barrier/IPlatformScreen.h"
#include "barrier/DropHelper.h"
#include "barrier/option_types.h"
#include "barrier/protocol_types.h"
#include "barrier/XScreen.h"
#include "barrier/XBarrier.h"
#include "barrier/StreamChunker.h"
#include "barrier/KeyState.h"
#include "barrier/Screen.h"
#include "barrier/PacketStreamFilter.h"
#include "net/TCPSocket.h"
#include "net/IDataSocket.h"
#include "net/IListenSocket.h"
#include "arch/IArchNetwork.h"
#include "net/XSocket.h"
#include "mt/Thread.h"
#include "arch/Arch.h"
#include "base/IEventQueue.h"
#include "base/Log.h"
#include "base/TMethodEventJob.h"
#include "core/layout/LayoutLoader.h"

#include "net/IDataSocket.h"
#include "arch/IArchNetwork.h"

#include <cstring>
#include <string>
#include <typeinfo>
#include <cstdlib>
#include <sstream>
#include <fstream>
#include <ctime>
#include <stdexcept>
#include <cctype>

namespace {

static int
clampInt(int value, int minValue, int maxValue)
{
    if (value < minValue) {
        return minValue;
    }
    if (value > maxValue) {
        return maxValue;
    }
    return value;
}

static int
toGlobalCoordinate(int localValue, int localOrigin, int localSpan,
                   int screenOrigin, int screenSpan)
{
    if (localSpan <= 0 || screenSpan <= 0) {
        return screenOrigin;
    }

    const int offset = localValue - localOrigin;
    return screenOrigin + (offset * screenSpan) / localSpan;
}

static int
toClientCoordinate(int globalValue, int screenOrigin, int screenSpan,
                   int clientOrigin, int clientSpan)
{
    if (screenSpan <= 0 || clientSpan <= 0) {
        return clientOrigin;
    }

    const int offset = globalValue - screenOrigin;
    return clientOrigin + (offset * clientSpan) / screenSpan;
}

static int
mapInclusiveCoordinate(int value, int srcMin, int srcMax, int dstMin, int dstMax)
{
    if (srcMax <= srcMin) {
        return dstMin;
    }
    if (dstMax <= dstMin) {
        return dstMin;
    }

    const int srcOffset = value - srcMin;
    const int srcSpan = srcMax - srcMin;
    const int dstSpan = dstMax - dstMin;
    return dstMin + (srcOffset * dstSpan) / srcSpan;
}

static int
applyTransitionInset(int value, int minValue, int maxValue, EDirection direction)
{
    static const int kTransitionInset = 24;
    if (maxValue <= minValue) {
        return minValue;
    }

    switch (direction) {
    case kLeft:
        return std::min(value, maxValue - kTransitionInset);

    case kRight:
        return std::max(value, minValue + kTransitionInset);

    case kTop:
        return std::min(value, maxValue - kTransitionInset);

    case kBottom:
        return std::max(value, minValue + kTransitionInset);

    case kNoDirection:
        return value;
    }

    return value;
}

static IUhidEdgeTransitionHandler::Direction
toUhidDirection(EDirection dir)
{
    switch (dir) {
    case kLeft:
        return IUhidEdgeTransitionHandler::kLeft;

    case kRight:
        return IUhidEdgeTransitionHandler::kRight;

    case kTop:
        return IUhidEdgeTransitionHandler::kTop;

    case kBottom:
        return IUhidEdgeTransitionHandler::kBottom;

    case kNoDirection:
        break;
    }

    return IUhidEdgeTransitionHandler::kLeft;
}

static EDirection
fromUhidDirection(IUhidEdgeTransitionHandler::Direction dir)
{
    switch (dir) {
    case IUhidEdgeTransitionHandler::kLeft:
        return kLeft;

    case IUhidEdgeTransitionHandler::kRight:
        return kRight;

    case IUhidEdgeTransitionHandler::kTop:
        return kTop;

    case IUhidEdgeTransitionHandler::kBottom:
        return kBottom;
    }

    return kNoDirection;
}

static EDirection
oppositeDirection(EDirection dir)
{
    switch (dir) {
    case kLeft:
        return kRight;

    case kRight:
        return kLeft;

    case kTop:
        return kBottom;

    case kBottom:
        return kTop;

    case kNoDirection:
        return kNoDirection;
    }

    return kNoDirection;
}

static bool
getHostLayoutBounds(const etherwaver::layout::ScreenManager& layout,
                    const std::string& hostId,
                    int& minX, int& minY, int& maxX, int& maxY)
{
    const std::vector<etherwaver::layout::Screen>& screens = layout.getScreens();
    bool found = false;
    for (std::vector<etherwaver::layout::Screen>::const_iterator it = screens.begin();
         it != screens.end(); ++it) {
        if (it->m_hostId != hostId) {
            continue;
        }

        const int right = it->m_x + it->m_width;
        const int bottom = it->m_y + it->m_height;
        if (!found) {
            minX = it->m_x;
            minY = it->m_y;
            maxX = right;
            maxY = bottom;
            found = true;
            continue;
        }

        minX = std::min<int>(minX, it->m_x);
        minY = std::min<int>(minY, it->m_y);
        maxX = std::max<int>(maxX, right);
        maxY = std::max<int>(maxY, bottom);
    }

    return found;
}

static const etherwaver::layout::Screen*
findLayoutScreenForPosition(const etherwaver::layout::ScreenManager& layout,
                            const std::string& hostId,
                            SInt32 screenX, SInt32 screenY, SInt32 screenW, SInt32 screenH,
                            SInt32 cursorX, SInt32 cursorY)
{
    if (screenW <= 0 || screenH <= 0) {
        return NULL;
    }

    int hostMinX = 0;
    int hostMinY = 0;
    int hostMaxX = 0;
    int hostMaxY = 0;
    if (!getHostLayoutBounds(layout, hostId, hostMinX, hostMinY, hostMaxX, hostMaxY)) {
        return NULL;
    }

    const int hostWidth = std::max<int>(1, hostMaxX - hostMinX);
    const int hostHeight = std::max<int>(1, hostMaxY - hostMinY);
    const int globalX = hostMinX + ((cursorX - screenX) * hostWidth) / screenW;
    const int globalY = hostMinY + ((cursorY - screenY) * hostHeight) / screenH;
    const etherwaver::layout::Screen* screen = layout.findScreenAt(globalX, globalY);
    if (screen != NULL && screen->m_hostId == hostId) {
        return screen;
    }

    return NULL;
}

static bool
getClientScreenForCursor(const BaseClientProxy* client,
                         SInt32 cursorX, SInt32 cursorY,
                         SInt32& screenX, SInt32& screenY,
                         SInt32& screenW, SInt32& screenH)
{
    if (client == NULL) {
        return false;
    }

    std::vector<ClientScreenInfo> screens;
    client->getScreens(screens);
    if (screens.empty()) {
        client->getShape(screenX, screenY, screenW, screenH);
        return screenW > 0 && screenH > 0;
    }

    const ClientScreenInfo* bestScreen = NULL;
    for (std::vector<ClientScreenInfo>::const_iterator it = screens.begin();
         it != screens.end(); ++it) {
        if (it->m_w <= 0 || it->m_h <= 0) {
            continue;
        }

        if (cursorX >= it->m_x && cursorX < it->m_x + it->m_w &&
            cursorY >= it->m_y && cursorY < it->m_y + it->m_h) {
            bestScreen = &(*it);
            break;
        }
    }

    if (bestScreen == NULL) {
        // Fall back to the nearest physical screen so switching still works
        // when the cursor is just outside monitor bounds.
        SInt32 bestDistance = 0;
        bool haveDistance = false;
        for (std::vector<ClientScreenInfo>::const_iterator it = screens.begin();
             it != screens.end(); ++it) {
            if (it->m_w <= 0 || it->m_h <= 0) {
                continue;
            }

            const SInt32 dx =
                (cursorX < it->m_x) ? (it->m_x - cursorX) :
                (cursorX >= it->m_x + it->m_w) ? (cursorX - (it->m_x + it->m_w - 1)) : 0;
            const SInt32 dy =
                (cursorY < it->m_y) ? (it->m_y - cursorY) :
                (cursorY >= it->m_y + it->m_h) ? (cursorY - (it->m_y + it->m_h - 1)) : 0;
            const SInt32 distance = dx + dy;
            if (!haveDistance || distance < bestDistance) {
                bestDistance = distance;
                bestScreen = &(*it);
                haveDistance = true;
            }
        }
    }

    if (bestScreen == NULL) {
        client->getShape(screenX, screenY, screenW, screenH);
        return screenW > 0 && screenH > 0;
    }

    screenX = bestScreen->m_x;
    screenY = bestScreen->m_y;
    screenW = bestScreen->m_w;
    screenH = bestScreen->m_h;
    return true;
}

static bool
isScreenIndexSuffix(const std::string& name, std::string& baseName)
{
    const std::string::size_type dash = name.find_last_of('-');
    if (dash == std::string::npos || dash == 0 || dash + 1 >= name.size()) {
        return false;
    }

    for (std::string::size_type i = dash + 1; i < name.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(name[i]))) {
            return false;
        }
    }

    baseName = name.substr(0, dash);
    return !baseName.empty();
}

static std::string
baseHostName(const std::string& name)
{
    std::string candidate = name;
    std::string baseName;
    while (isScreenIndexSuffix(candidate, baseName)) {
        candidate = baseName;
    }
    return candidate;
}

static std::string
resolveLayoutHostId(const etherwaver::layout::ScreenManager& layout, const std::string& hostId)
{
    if (layout.getFirstScreenForHost(hostId) != NULL) {
        return hostId;
    }

    const std::string baseName = baseHostName(hostId);
    if (baseName != hostId && layout.getFirstScreenForHost(baseName) != NULL) {
        return baseName;
    }

    return hostId;
}

static std::string
resolveClientHostId(const std::map<std::string, BaseClientProxy*>& clients,
                    const std::string& primaryHostId,
                    const std::string& hostId)
{
    if (hostId.empty()) {
        return std::string();
    }

    if (hostId == primaryHostId || baseHostName(primaryHostId) == baseHostName(hostId)) {
        return primaryHostId;
    }

    if (clients.find(hostId) != clients.end()) {
        return hostId;
    }

    const std::string targetBaseHostId = baseHostName(hostId);
    std::string resolvedHostId;
    for (std::map<std::string, BaseClientProxy*>::const_iterator it = clients.begin();
         it != clients.end(); ++it) {
        if (baseHostName(it->first) != targetBaseHostId) {
            continue;
        }

        if (!resolvedHostId.empty()) {
            return hostId;
        }
        resolvedHostId = it->first;
    }

    return resolvedHostId.empty() ? hostId : resolvedHostId;
}

static const char*
safeDirectionName(EDirection dir)
{
    return (dir == kNoDirection) ? "none" : Config::dirName(dir);
}

static std::string
resolveScreenOrHostName(const Config& config, const std::string& name)
{
    if (config.isScreen(name)) {
        return config.getCanonicalName(name);
    }

    for (Config::const_iterator it = config.begin(); it != config.end(); ++it) {
        std::string baseName;
        if (isScreenIndexSuffix(*it, baseName) && baseName == name) {
            return *it;
        }
    }

    return std::string();
}

static bool
matchesScreenOrHostName(const Config& config, const std::string& name)
{
    return !resolveScreenOrHostName(config, name).empty();
}

} // namespace

class Server::UhidTransitionHandler : public IUhidEdgeTransitionHandler {
public:
    explicit UhidTransitionHandler(Server* server)
        : m_server(server)
    {
    }

    void onTransition(Direction direction) override
    {
        if (m_server != NULL) {
            m_server->onTransition(direction);
        }
    }

private:
    Server* m_server;
};
//
// Server
//

Server::Server(
		Config& config,
		PrimaryClient* primaryClient,
		barrier::Screen* screen,
		IEventQueue* events,
		ServerArgs const& args) :
	m_mock(false),
	m_primaryClient(primaryClient),
	m_active(primaryClient),
	m_seqNum(0),
	m_xDelta(0),
	m_yDelta(0),
	m_xDelta2(0),
	m_yDelta2(0),
	m_config(&config),
	m_inputFilter(config.getInputFilter()),
	m_activeSaver(NULL),
	m_switchDir(kNoDirection),
	m_switchScreen(NULL),
	m_switchWaitDelay(0.0),
	m_switchWaitTimer(NULL),
	m_switchTwoTapDelay(0.0),
	m_switchTwoTapEngaged(false),
	m_switchTwoTapArmed(false),
	m_switchTwoTapZone(3),
	m_switchNeedsShift(false),
	m_switchNeedsControl(false),
	m_switchNeedsAlt(false),
	m_relativeMoves(false),
	m_keyboardBroadcasting(false),
	m_lockedToScreen(false),
	m_screen(screen),
	m_events(events),
	m_sendFileThread(NULL),
	m_writeToDropDirThread(NULL),
	m_ignoreFileTransfer(false),
	m_enableClipboard(true),
	m_sendDragInfoThread(NULL),
	m_waitDragInfoThread(true),
	m_args(args),
	m_activeLayoutScreenId(primaryClient != NULL ? primaryClient->getName() : std::string()),
	m_httpListener(NULL),
	m_running(false)
    , m_uhidTransitionHandler(new UhidTransitionHandler(this))
    , m_uhidTransitionTriggered(false)
    , m_recentSwitchTimer(true)
    , m_recentSwitchArmed(false)
    , m_recentSwitchSource(NULL)
    , m_recentSwitchDestination(NULL)
    , m_recentSwitchDirection(kNoDirection)
{
	// must have a primary client and it must have a canonical name
	assert(m_primaryClient != NULL);
	assert(matchesScreenOrHostName(config, primaryClient->getName()));
	assert(m_screen != NULL);

    UhidEdgeTransitionService::Config uhidConfig;
    uhidConfig.m_debugLogging = false;
    uhidConfig.m_enableTopBottom = true;
    uhidConfig.m_requiredConsecutiveEvents = 4;
    m_uhidEdgeTransitionService = UhidEdgeTransitionService(uhidConfig);
    m_uhidEdgeTransitionService.setTransitionHandler(m_uhidTransitionHandler.get());

    std::string primaryName = getName(primaryClient);

	// clear clipboards
	for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
		ClipboardInfo& clipboard   = m_clipboards[id];
		clipboard.m_clipboardOwner  = primaryName;
		clipboard.m_clipboardSeqNum = m_seqNum;
		if (clipboard.m_clipboard.open(0)) {
			clipboard.m_clipboard.empty();
			clipboard.m_clipboard.close();
		}
		clipboard.m_clipboardData   = clipboard.m_clipboard.marshall();
	}

	// install event handlers
	m_events->adoptHandler(Event::kTimer, this,
							new TMethodEventJob<Server>(this,
								&Server::handleSwitchWaitTimeout));
	m_events->adoptHandler(m_events->forIKeyState().keyDown(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleKeyDownEvent));
	m_events->adoptHandler(m_events->forIKeyState().keyUp(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleKeyUpEvent));
	m_events->adoptHandler(m_events->forIKeyState().keyRepeat(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleKeyRepeatEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().buttonDown(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleButtonDownEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().buttonUp(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleButtonUpEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().motionOnPrimary(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleMotionPrimaryEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().motionOnSecondary(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleMotionSecondaryEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().wheel(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleWheelEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().screensaverActivated(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleScreensaverActivatedEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().screensaverDeactivated(),
							m_primaryClient->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleScreensaverDeactivatedEvent));
	m_events->adoptHandler(m_events->forServer().switchToScreen(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleSwitchToScreenEvent));
  m_events->adoptHandler(m_events->forServer().toggleScreen(),
              m_inputFilter,
              new TMethodEventJob<Server>(this,
                &Server::handleToggleScreenEvent));
	m_events->adoptHandler(m_events->forServer().switchInDirection(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleSwitchInDirectionEvent));
	m_events->adoptHandler(m_events->forServer().keyboardBroadcast(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleKeyboardBroadcastEvent));
	m_events->adoptHandler(m_events->forServer().lockCursorToScreen(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleLockCursorToScreenEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().fakeInputBegin(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleFakeInputBeginEvent));
	m_events->adoptHandler(m_events->forIPrimaryScreen().fakeInputEnd(),
							m_inputFilter,
							new TMethodEventJob<Server>(this,
								&Server::handleFakeInputEndEvent));

	if (m_args.m_enableDragDrop) {
		m_events->adoptHandler(m_events->forFile().fileChunkSending(),
								this,
								new TMethodEventJob<Server>(this,
									&Server::handleFileChunkSendingEvent));
		m_events->adoptHandler(m_events->forFile().fileRecieveCompleted(),
								this,
								new TMethodEventJob<Server>(this,
									&Server::handleFileRecieveCompletedEvent));
	}

	// add connection
	addClient(m_primaryClient);

	// set initial configuration
	setConfig(config);

	// enable primary client
	m_primaryClient->enable();
	m_inputFilter->setPrimaryClient(m_primaryClient);

	// Determine if scroll lock is already set. If so, lock the cursor to the primary screen
	if (m_primaryClient->getToggleMask() & KeyModifierScrollLock) {
		LOG((CLOG_NOTE "Scroll Lock is on, locking cursor to screen"));
		m_lockedToScreen = true;
	}

// Initialize current host to server's own name
    m_currentHost = m_primaryClient->getName();

    // Start the HTTP endpoint
    try {
        NetworkAddress addr("0.0.0.0", 24802);  // Listen on all interfaces, port 24801
        addr.resolve();

    
	m_httpListener = ARCH->newSocket(IArchNetwork::kINET, IArchNetwork::kSTREAM);
        ARCH->bindSocket(m_httpListener, addr.getAddress());
        ARCH->listenOnSocket(m_httpListener);  // Backlog 5

        m_running = true;
        m_httpThread = std::thread(&Server::httpLoop, this);

    } catch (XBase& e) {
        LOG((CLOG_ERR "Failed to start HTTP endpoint: %s", e.what()));
    }


}

Server::~Server()
{
	if (m_mock) {
		return;
	}

	// remove event handlers and timers
	m_events->removeHandler(m_events->forIKeyState().keyDown(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIKeyState().keyUp(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIKeyState().keyRepeat(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIPrimaryScreen().buttonDown(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIPrimaryScreen().buttonUp(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIPrimaryScreen().motionOnPrimary(),
							m_primaryClient->getEventTarget());
	m_events->removeHandler(m_events->forIPrimaryScreen().motionOnSecondary(),
							m_primaryClient->getEventTarget());
	m_events->removeHandler(m_events->forIPrimaryScreen().wheel(),
							m_primaryClient->getEventTarget());
	m_events->removeHandler(m_events->forIPrimaryScreen().screensaverActivated(),
							m_primaryClient->getEventTarget());
	m_events->removeHandler(m_events->forIPrimaryScreen().screensaverDeactivated(),
							m_primaryClient->getEventTarget());
	m_events->removeHandler(m_events->forIPrimaryScreen().fakeInputBegin(),
							m_inputFilter);
	m_events->removeHandler(m_events->forIPrimaryScreen().fakeInputEnd(),
							m_inputFilter);
	m_events->removeHandler(Event::kTimer, this);
	stopSwitch();

	// force immediate disconnection of secondary clients
	disconnect();
	for (OldClients::iterator index = m_oldClients.begin();
							index != m_oldClients.end(); ++index) {
		BaseClientProxy* client = index->first;
		m_events->deleteTimer(index->second);
		m_events->removeHandler(Event::kTimer, client);
		m_events->removeHandler(m_events->forClientProxy().disconnected(), client);
		delete client;
	}

	// remove input filter
	m_inputFilter->setPrimaryClient(NULL);

	// disable and disconnect primary client
	m_primaryClient->disable();
	removeClient(m_primaryClient);

    if (m_httpListener) {
        m_running = false;
        ARCH->closeSocket(m_httpListener);
        m_httpThread.join();
    }
}

#ifdef BARRIER_TEST_ENV
Server::Server() :
    m_mock(true),
    m_primaryClient(NULL),
    m_active(NULL),
    m_seqNum(0),
    m_x(0),
    m_y(0),
    m_xDelta(0),
    m_yDelta(0),
    m_xDelta2(0),
    m_yDelta2(0),
    m_config(NULL),
    m_inputFilter(NULL),
    m_activeSaver(NULL),
    m_xSaver(0),
    m_ySaver(0),
    m_switchDir(kNoDirection),
    m_switchScreen(NULL),
    m_switchWaitDelay(0.0),
    m_switchWaitTimer(NULL),
    m_switchWaitX(0),
    m_switchWaitY(0),
    m_switchTwoTapDelay(0.0),
    m_switchTwoTapEngaged(false),
    m_switchTwoTapArmed(false),
    m_switchTwoTapZone(0),
    m_switchNeedsShift(false),
    m_switchNeedsControl(false),
    m_switchNeedsAlt(false),
    m_relativeMoves(false),
    m_keyboardBroadcasting(false),
    m_lockedToScreen(false),
    m_screen(NULL),
    m_events(NULL),
    m_expectedFileSize(0),
    m_sendFileThread(NULL),
    m_writeToDropDirThread(NULL),
    m_ignoreFileTransfer(false),
    m_enableClipboard(false),
    m_sendDragInfoThread(NULL),
    m_waitDragInfoThread(false),
    m_clientListener(NULL),
    m_httpListener(NULL),
    m_running(false),
    m_uhidTransitionHandler(),
    m_uhidEdgeTransitionService(UhidEdgeTransitionService::Config()),
    m_uhidTransitionTriggered(false),
    m_recentSwitchTimer(true),
    m_recentSwitchArmed(false),
    m_recentSwitchSource(NULL),
    m_recentSwitchDestination(NULL),
    m_recentSwitchDirection(kNoDirection)
{
}
#endif

bool
Server::setConfig(const Config& config)
{
	// refuse configuration if it doesn't include the primary screen
	if (!matchesScreenOrHostName(config, m_primaryClient->getName())) {
		return false;
	}

	// close clients that are connected but being dropped from the
	// configuration.
	closeClients(config);

	// cut over
	processOptions();

	// add ScrollLock as a hotkey to lock to the screen.  this was a
	// built-in feature in earlier releases and is now supported via
	// the user configurable hotkey mechanism.  if the user has already
	// registered ScrollLock for something else then that will win but
	// we will unfortunately generate a warning.  if the user has
	// configured a LockCursorToScreenAction then we don't add
	// ScrollLock as a hotkey.
	if (!m_config->hasLockToScreenAction()) {
		IPlatformScreen::KeyInfo* key =
			IPlatformScreen::KeyInfo::alloc(kKeyScrollLock, 0, 0, 0);
		InputFilter::Rule rule(new InputFilter::KeystrokeCondition(m_events, key));
		rule.adoptAction(new InputFilter::LockCursorToScreenAction(m_events), true);
		m_inputFilter->addFilterRule(rule);
	}

	// tell primary screen about reconfiguration
	reloadScreenLayout();
    refreshPrimaryUhidGeometry();
	m_primaryClient->reconfigure(getActivePrimarySides());

	// tell all (connected) clients about current options
	for (ClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		BaseClientProxy* client = index->second;
		sendOptions(client);
	}

	return true;
}

void
Server::adoptClient(BaseClientProxy* client)
{
	assert(client != NULL);

    const std::string clientName = getName(client);

	// watch for client disconnection
	m_events->adoptHandler(m_events->forClientProxy().disconnected(), client,
							new TMethodEventJob<Server>(this,
								&Server::handleClientDisconnected, client));

	// name must resolve to a configured screen or a known object-layout host
	if (clientName.empty()) {
		LOG((CLOG_WARN "unrecognised client name \"%s\", check server config", client->getName().c_str()));
		closeClient(client, kMsgEUnknown);
		return;
	}

	// add client to client list
	if (!addClient(client)) {
		// can only have one screen with a given name at any given time
		LOG((CLOG_WARN "a client with name \"%s\" is already connected", clientName.c_str()));
		closeClient(client, kMsgEBusy);
		return;
	}
	LOG((CLOG_NOTE "client \"%s\" has connected", clientName.c_str()));
	reloadScreenLayout();

	// send configuration options to client
	sendOptions(client);

	// activate screen saver on new client if active on the primary screen
	if (m_activeSaver != NULL) {
		client->screensaver(true);
	}

	// send notification
	Server::ScreenConnectedInfo* info =
		new Server::ScreenConnectedInfo(clientName);
	m_events->addEvent(Event(m_events->forServer().connected(),
								m_primaryClient->getEventTarget(), info));
}

void
Server::disconnect()
{
	// close all secondary clients
	if (m_clients.size() > 1 || !m_oldClients.empty()) {
		Config emptyConfig(m_events);
		closeClients(emptyConfig);
	}
	else {
		m_events->addEvent(Event(m_events->forServer().disconnected(), this));
	}
}

UInt32
Server::getNumClients() const
{
	return (SInt32)m_clients.size();
}

void
Server::getClients(std::vector<std::string>& list) const
{
	list.clear();
	for (ClientList::const_iterator index = m_clients.begin();
							index != m_clients.end(); ++index) {
		list.push_back(index->first);
	}
}

std::string Server::getName(const BaseClientProxy* client) const
{
    for (ClientList::const_iterator it = m_clients.begin(); it != m_clients.end(); ++it) {
        if (it->second == client) {
            return it->first;
        }
    }

    const std::string rawName = client->getName();
    std::string name = resolveScreenOrHostName(*m_config, rawName);
    if (!name.empty()) {
        return name;
    }

    std::string baseName;
    if (isScreenIndexSuffix(rawName, baseName)) {
        name = resolveScreenOrHostName(*m_config, baseName);
        if (!name.empty()) {
            return name;
        }

        if (usingObjectLayout() &&
            m_screenLayout.getFirstScreenForHost(baseName) != NULL) {
            return baseName;
        }
    }

    if (usingObjectLayout() &&
        m_screenLayout.getFirstScreenForHost(rawName) != NULL) {
        return rawName;
    }

    return std::string();
}

std::string
Server::getLayoutPath() const
{
    if (m_args.m_configFile.empty()) {
        return "etherwaver-layout.json";
    }

    const std::string configPath = m_args.m_configFile;
    const std::string::size_type slash = configPath.find_last_of("/\\");
    if (slash == std::string::npos) {
        return "etherwaver-layout.json";
    }
    return configPath.substr(0, slash + 1) + "etherwaver-layout.json";
}

bool
Server::usingObjectLayout() const
{
    return !m_screenLayout.empty();
}

const etherwaver::layout::Screen*
Server::getActiveLayoutScreen() const
{
    if (!usingObjectLayout()) {
        return NULL;
    }

    SInt32 ax = 0;
    SInt32 ay = 0;
    SInt32 aw = 0;
    SInt32 ah = 0;
    getClientScreenForCursor(m_active, m_x, m_y, ax, ay, aw, ah);
    const std::string activeHostId = resolveLayoutHostId(m_screenLayout, getName(m_active));
    const etherwaver::layout::Screen* positionScreen =
        findLayoutScreenForPosition(m_screenLayout, activeHostId, ax, ay, aw, ah, m_x, m_y);
    if (positionScreen != NULL) {
        return positionScreen;
    }

    const etherwaver::layout::Screen* screen =
        m_screenLayout.getScreen(m_activeLayoutScreenId);
    if (screen != NULL) {
        return screen;
    }

    return getLayoutScreenForHost(getName(m_active));
}

const etherwaver::layout::Screen*
Server::getLayoutScreenForHost(const std::string& hostId) const
{
    if (!usingObjectLayout()) {
        return NULL;
    }

    const std::string resolvedHostId = resolveLayoutHostId(m_screenLayout, hostId);
    const std::string primaryScreenId = resolvedHostId + ":screen0";
    const std::vector<etherwaver::layout::Screen>& screens = m_screenLayout.getScreens();
    for (std::vector<etherwaver::layout::Screen>::const_iterator it = screens.begin();
         it != screens.end(); ++it) {
        if (it->m_hostId != resolvedHostId) {
            continue;
        }
        if (it->m_id == primaryScreenId || it->m_name == "screen0" ||
            it->m_name == primaryScreenId) {
            return &(*it);
        }
    }

    return m_screenLayout.getFirstScreenForHost(resolvedHostId);
}

BaseClientProxy*
Server::getClientForLayoutScreen(const etherwaver::layout::Screen& screen) const
{
    const std::string layoutHostId = resolveLayoutHostId(m_screenLayout, screen.m_hostId);
    const std::string runtimeHostId =
        resolveClientHostId(m_clients, getName(m_primaryClient), layoutHostId);
    if (runtimeHostId == getName(m_primaryClient)) {
        return m_primaryClient;
    }

    ClientList::const_iterator it = m_clients.find(runtimeHostId);
    if (it == m_clients.end()) {
        return NULL;
    }
    return it->second;
}

bool
Server::switchToScreenName(const std::string& screenName)
{
    if (screenName.empty()) {
        return false;
    }

    if (usingObjectLayout()) {
        const etherwaver::layout::Screen* screen = m_screenLayout.getScreen(screenName);
        if (screen != NULL) {
            BaseClientProxy* client = getClientForLayoutScreen(*screen);
            if (client != NULL) {
                SInt32 x = 0;
                SInt32 y = 0;
                client->getJumpCursorPos(x, y);
                switchScreen(client, x, y, false, screen->m_id);
                return true;
            }

            const std::string runtimeHostId =
                resolveClientHostId(m_clients, getName(m_primaryClient), screen->m_hostId);
            ClientList::const_iterator host = m_clients.find(runtimeHostId);
            if (host != m_clients.end()) {
                jumpToScreen(host->second);
                return true;
            }
        }
    }

    ClientList::const_iterator client = m_clients.find(screenName);
    if (client != m_clients.end()) {
        jumpToScreen(client->second);
        return true;
    }

    return false;
}

void
Server::reloadScreenLayout()
{
    const etherwaver::layout::ScreenManager previousLayout = m_screenLayout;
    const std::string previousActiveScreenId = m_activeLayoutScreenId;
    std::map<std::string, etherwaver::layout::HostGeometry> hostGeometries;
    std::map<std::string, std::vector<ClientScreenInfo> > hostScreens;
    const std::string primaryHostId = getName(m_primaryClient);
    const std::string primaryBaseHostId = baseHostName(primaryHostId);
    const std::string layoutPath = getLayoutPath();

    {
        SInt32 x = 0;
        SInt32 y = 0;
        SInt32 width = 0;
        SInt32 height = 0;
        std::vector<ClientScreenInfo> screens;
        m_primaryClient->getShape(x, y, width, height);
        hostGeometries[primaryHostId] = etherwaver::layout::HostGeometry(x, y, width, height);
        m_primaryClient->getScreens(screens);
        hostScreens[primaryHostId] = screens;

        if (primaryBaseHostId != primaryHostId) {
            hostGeometries[primaryBaseHostId] =
                etherwaver::layout::HostGeometry(x, y, width, height);
            hostScreens[primaryBaseHostId] = screens;
        }
    }

    for (ClientList::const_iterator it = m_clients.begin(); it != m_clients.end(); ++it) {
        SInt32 x = 0;
        SInt32 y = 0;
        SInt32 width = 0;
        SInt32 height = 0;
        it->second->getShape(x, y, width, height);
        hostGeometries[it->first] = etherwaver::layout::HostGeometry(x, y, width, height);
        it->second->getScreens(hostScreens[it->first]);
    }

    const bool hasLayoutFile = std::ifstream(layoutPath.c_str()).good();
    if (!hasLayoutFile && !previousLayout.empty()) {
        LOG((CLOG_WARN
            "object-layout reload skipped missingLayoutFile=%s keepingPreviousLayout=yes",
            layoutPath.c_str()));
        m_screenLayout = previousLayout;
    }
    else {
        try {
            m_screenLayout = etherwaver::layout::LayoutLoader::loadLayout(
                layoutPath, *m_config, hostGeometries, hostScreens, primaryHostId);
        }
        catch (const std::exception& e) {
            if (!previousLayout.empty()) {
                LOG((CLOG_WARN
                    "failed to load object layout: %s; keeping previous layout",
                    e.what()));
                m_screenLayout = previousLayout;
            }
            else {
                LOG((CLOG_WARN "failed to load object layout: %s", e.what()));
                m_screenLayout.setScreens(std::vector<etherwaver::layout::Screen>());
            }
        }
    }

    {
        const std::vector<etherwaver::layout::Screen>& screens = m_screenLayout.getScreens();
        for (std::vector<etherwaver::layout::Screen>::const_iterator it = screens.begin();
             it != screens.end(); ++it) {
            LOG((CLOG_INFO
                "object-layout loaded screen id=%s host=%s name=%s rect=%d,%d %dx%d links(L=%s R=%s U=%s D=%s)",
                it->m_id.c_str(),
                it->m_hostId.c_str(),
                it->m_name.c_str(),
                it->m_x,
                it->m_y,
                it->m_width,
                it->m_height,
                it->m_leftLink.empty() ? "<none>" : it->m_leftLink.c_str(),
                it->m_rightLink.empty() ? "<none>" : it->m_rightLink.c_str(),
                it->m_topLink.empty() ? "<none>" : it->m_topLink.c_str(),
                it->m_bottomLink.empty() ? "<none>" : it->m_bottomLink.c_str()));
        }
    }

    const etherwaver::layout::Screen* activeScreen =
        m_screenLayout.getScreen(previousActiveScreenId);
    if (activeScreen != NULL && activeScreen->m_hostId != getName(m_active)) {
        activeScreen = NULL;
    }
    if (activeScreen == NULL) {
        activeScreen = getLayoutScreenForHost(getName(m_active));
    }
    if (activeScreen != NULL) {
        m_activeLayoutScreenId = activeScreen->m_id;
    }

    refreshPrimaryUhidGeometry();
}

bool
Server::trySwitchUsingObjectLayout(SInt32 x, SInt32 y, bool absoluteMotion)
{
    BaseClientProxy* const sourceClient = m_active;
    const etherwaver::layout::Screen* sourceScreen = getActiveLayoutScreen();
    if (sourceScreen == NULL) {
        LOG((CLOG_INFO
            "object-layout switch aborted activeHost=%s reason=no-source-screen x=%d y=%d absolute=%s",
            getName(m_active).c_str(), x, y, absoluteMotion ? "yes" : "no"));
        return false;
    }

    SInt32 ax, ay, aw, ah;
    getClientScreenForCursor(m_active, x, y, ax, ay, aw, ah);

    // Map the cursor from this physical sub-screen's local space into the
    // layout coordinate space of sourceScreen.
    //
    // We map [ax .. ax+aw] → [sourceScreen->m_x .. m_x+m_width] directly.
    // Using the host-wide span (hostMinX/hostWidth) would be wrong for
    // multi-monitor hosts: a cursor at the left edge of a right-hand monitor
    // would map to hostMinX, falsely triggering kLeft.
    EDirection direction = kNoDirection;
    int globalX = toGlobalCoordinate(x, ax, aw,
                                     sourceScreen->m_x, sourceScreen->m_width);
    int globalY = toGlobalCoordinate(y, ay, ah,
                                     sourceScreen->m_y, sourceScreen->m_height);
    if (x < ax || globalX < sourceScreen->m_x) {
        direction = kLeft;
    }
    else if (x >= ax + aw || globalX >= sourceScreen->m_x + sourceScreen->m_width) {
        direction = kRight;
    }
    else if (y < ay || globalY < sourceScreen->m_y) {
        direction = kTop;
    }
    else if (y >= ay + ah || globalY >= sourceScreen->m_y + sourceScreen->m_height) {
        direction = kBottom;
    }

    // When the cursor has already crossed a local edge, force the global
    // coordinate just outside the source screen so directional links resolve.
    if (direction == kLeft && globalX >= sourceScreen->m_x) {
        globalX = sourceScreen->m_x - 1;
    }
    else if (direction == kRight &&
             globalX < sourceScreen->m_x + sourceScreen->m_width) {
        globalX = sourceScreen->m_x + sourceScreen->m_width;
    }
    else if (direction == kTop && globalY >= sourceScreen->m_y) {
        globalY = sourceScreen->m_y - 1;
    }
    else if (direction == kBottom &&
             globalY < sourceScreen->m_y + sourceScreen->m_height) {
        globalY = sourceScreen->m_y + sourceScreen->m_height;
    }

    const etherwaver::layout::Screen* destinationScreen = m_screenLayout.findScreenAt(globalX, globalY);
    const etherwaver::layout::Screen* resolvedDestination = destinationScreen;
    const etherwaver::layout::Screen* directionalDestination = NULL;
    if ((resolvedDestination == NULL || resolvedDestination->m_id == sourceScreen->m_id) &&
        direction != kNoDirection) {
        directionalDestination = m_screenLayout.findScreenInDirection(sourceScreen->m_id, direction);
        resolvedDestination = directionalDestination;
    }

    if (resolvedDestination == NULL || resolvedDestination->m_id == sourceScreen->m_id) {
        LOG((CLOG_INFO
            "object-layout switch rejected sourceScreen=%s direction=%s directDestination=%s directionalDestination=%s resolvedDestination=%s reason=no-valid-destination",
            sourceScreen->m_id.c_str(),
            safeDirectionName(direction),
            (destinationScreen != NULL ? destinationScreen->m_id.c_str() : "<none>"),
            (directionalDestination != NULL ? directionalDestination->m_id.c_str() : "<none>"),
            (resolvedDestination != NULL ? resolvedDestination->m_id.c_str() : "<none>")));
        noSwitch(clampInt(x, ax, ax + aw - 1), clampInt(y, ay, ay + ah - 1));
        return false;
    }

    BaseClientProxy* destinationClient = getClientForLayoutScreen(*resolvedDestination);
    if (destinationClient == NULL) {
        LOG((CLOG_INFO
            "object-layout switch rejected sourceScreen=%s resolvedDestination=%s reason=no-destination-client",
            sourceScreen->m_id.c_str(),
            resolvedDestination->m_id.c_str()));
        return false;
    }

    const SInt32 xActive = clampInt(x, ax, ax + aw - 1);
    const SInt32 yActive = clampInt(y, ay, ay + ah - 1);

    SInt32 dx = 0;
    SInt32 dy = 0;
    destinationClient->getShape(dx, dy, aw, ah);

    SInt32 targetX = toClientCoordinate(globalX, resolvedDestination->m_x, resolvedDestination->m_width,
                                        dx, aw);
    SInt32 targetY = toClientCoordinate(globalY, resolvedDestination->m_y, resolvedDestination->m_height,
                                        dy, ah);
    targetX = clampInt(targetX, dx, dx + aw - 1);
    targetY = clampInt(targetY, dy, dy + ah - 1);
    targetX = clampInt(applyTransitionInset(targetX, dx, dx + aw - 1, direction),
                       dx, dx + aw - 1);
    targetY = clampInt(applyTransitionInset(targetY, dy, dy + ah - 1, direction),
                       dy, dy + ah - 1);

    if (!isSwitchOkay(destinationClient, direction, targetX, targetY, xActive, yActive)) {
        LOG((CLOG_INFO
            "object-layout switch blocked sourceScreen=%s destination=%s client=%s target=%d,%d",
            sourceScreen->m_id.c_str(),
            resolvedDestination->m_id.c_str(),
            getName(destinationClient).c_str(),
            targetX, targetY));
        return false;
    }

    LOG((CLOG_INFO
        "object-layout switch accepted sourceScreen=%s destination=%s client=%s target=%d,%d",
        sourceScreen->m_id.c_str(),
        resolvedDestination->m_id.c_str(),
        getName(destinationClient).c_str(),
        targetX, targetY));

    rememberRecentObjectLayoutSwitch(sourceClient, destinationClient, direction);
    switchScreen(destinationClient, targetX, targetY, false, resolvedDestination->m_id);
    if (!absoluteMotion) {
        m_x = targetX;
        m_y = targetY;
    }
    return true;
}

void
Server::refreshPrimaryUhidGeometry()
{
    SInt32 ax = 0;
    SInt32 ay = 0;
    SInt32 aw = 0;
    SInt32 ah = 0;
    m_primaryClient->getShape(ax, ay, aw, ah);
    m_uhidEdgeTransitionService.setScreenGeometry(ax, ay, aw, ah);
}

bool
Server::trySwitchUsingUhidDirection(IUhidEdgeTransitionHandler::Direction direction)
{
    if (!usingObjectLayout() || m_active != m_primaryClient) {
        return false;
    }

    SInt32 ax = 0;
    SInt32 ay = 0;
    SInt32 aw = 0;
    SInt32 ah = 0;
    m_active->getShape(ax, ay, aw, ah);
    if (aw <= 0 || ah <= 0) {
        return false;
    }

    const SInt32 virtualX = m_uhidEdgeTransitionService.virtualX();
    const SInt32 virtualY = m_uhidEdgeTransitionService.virtualY();
    SInt32 edgeX = clampInt(virtualX, ax, ax + aw - 1);
    SInt32 edgeY = clampInt(virtualY, ay, ay + ah - 1);

    switch (direction) {
    case IUhidEdgeTransitionHandler::kLeft:
        edgeX = ax - 1;
        break;

    case IUhidEdgeTransitionHandler::kRight:
        edgeX = ax + aw;
        break;

    case IUhidEdgeTransitionHandler::kTop:
        edgeY = ay - 1;
        break;

    case IUhidEdgeTransitionHandler::kBottom:
        edgeY = ay + ah;
        break;
    }

    LOG((CLOG_INFO
        "uhid-edge primary attempt activeHost=%s direction=%s virtualPos=%d,%d edgePos=%d,%d",
        getName(m_active).c_str(),
        safeDirectionName(fromUhidDirection(direction)),
        virtualX,
        virtualY,
        edgeX,
        edgeY));

    return trySwitchUsingObjectLayout(edgeX, edgeY, true);
}

void
Server::onTransition(IUhidEdgeTransitionHandler::Direction direction)
{
    m_uhidTransitionTriggered = trySwitchUsingUhidDirection(direction);
}

void
Server::rememberRecentObjectLayoutSwitch(BaseClientProxy* src,
                                         BaseClientProxy* dst,
                                         EDirection direction)
{
    m_recentSwitchSource = src;
    m_recentSwitchDestination = dst;
    m_recentSwitchDirection = direction;
    m_recentSwitchTimer.reset();
    m_recentSwitchArmed = true;
}

bool
Server::isRecentReverseSwitch(BaseClientProxy* newScreen, EDirection direction) const
{
    static const double kRecentReverseSwitchCooldown = 0.35;

    if (!m_recentSwitchArmed || m_active == NULL) {
        return false;
    }

    if (m_recentSwitchTimer.getTime() > kRecentReverseSwitchCooldown) {
        return false;
    }

    return (m_active == m_recentSwitchDestination &&
            newScreen == m_recentSwitchSource &&
            direction == oppositeDirection(m_recentSwitchDirection));
}

UInt32
Server::getActivePrimarySides() const
{
	UInt32 sides = 0;
	if (!isLockedToScreenServer()) {
		if (hasAnyNeighbor(m_primaryClient, kLeft)) {
			sides |= kLeftMask;
		}
		if (hasAnyNeighbor(m_primaryClient, kRight)) {
			sides |= kRightMask;
		}
		if (hasAnyNeighbor(m_primaryClient, kTop)) {
			sides |= kTopMask;
		}
		if (hasAnyNeighbor(m_primaryClient, kBottom)) {
			sides |= kBottomMask;
		}
	}
	return sides;
}

bool
Server::isLockedToScreenServer() const
{
	// locked if scroll-lock is toggled on
	return m_lockedToScreen;
}

bool
Server::isLockedToScreen() const
{
	// locked if we say we're locked
	if (isLockedToScreenServer()) {
		return true;
	}

	// locked if primary says we're locked
	if (m_primaryClient->isLockedToScreen()) {
		return true;
	}

	// not locked
	return false;
}

SInt32
Server::getJumpZoneSize(BaseClientProxy* client) const
{
	if (client == m_primaryClient) {
		return m_primaryClient->getJumpZoneSize();
	}
	else {
		return 0;
	}
}

void
Server::switchScreen(BaseClientProxy* dst,
				SInt32 x, SInt32 y, bool forScreensaver,
				const std::string& layoutScreenId)
{
	assert(dst != NULL);

#ifndef NDEBUG
	{
		SInt32 dx, dy, dw, dh;
		dst->getShape(dx, dy, dw, dh);
		assert(x >= dx && y >= dy && x < dx + dw && y < dy + dh);
	}
#endif
	assert(m_active != NULL);

	LOG((CLOG_INFO "switch from \"%s\" to \"%s\" at %d,%d", getName(m_active).c_str(), getName(dst).c_str(), x, y));

	// stop waiting to switch
	stopSwitch();

	// record new position
	m_x       = x;
	m_y       = y;
	m_xDelta  = 0;
	m_yDelta  = 0;
	m_xDelta2 = 0;
	m_yDelta2 = 0;

	// wrapping means leaving the active screen and entering it again.
	// since that's a waste of time we skip that and just warp the
	// mouse.
	if (m_active != dst) {
		// leave active screen
		if (!m_active->leave()) {
			// cannot leave screen
			LOG((CLOG_WARN "can't leave screen"));
			return;
		}

		// update the primary client's clipboards if we're leaving the
		// primary screen.
		if (m_active == m_primaryClient && m_enableClipboard) {
			for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
				ClipboardInfo& clipboard = m_clipboards[id];
				if (clipboard.m_clipboardOwner == getName(m_primaryClient)) {
					onClipboardChanged(m_primaryClient,
						id, clipboard.m_clipboardSeqNum);
				}
			}
		}

		// cut over
		m_active = dst;
		{ 
			std::lock_guard<std::mutex> lock(m_mutex); 
			m_currentHost = dst->getName(); 
			m_current_ip.clear();
		}
		if (!layoutScreenId.empty()) {
			m_activeLayoutScreenId = layoutScreenId;
		}
		else if (usingObjectLayout()) {
			const etherwaver::layout::Screen* screen = getLayoutScreenForHost(getName(dst));
			if (screen != NULL) {
				m_activeLayoutScreenId = screen->m_id;
			}
		}

		// increment enter sequence number
		++m_seqNum;

		// enter new screen
		m_active->enter(x, y, m_seqNum,
								m_primaryClient->getToggleMask(),
								forScreensaver);

		if (m_enableClipboard) {
			// send the clipboard data to new active screen
			for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
				m_active->setClipboard(id, &m_clipboards[id].m_clipboard);
			}
		}

		Server::SwitchToScreenInfo* info =
			Server::SwitchToScreenInfo::alloc(m_active->getName());
		m_events->addEvent(Event(m_events->forServer().screenSwitched(), this, info));
	}
	else {
		if (!layoutScreenId.empty()) {
			m_activeLayoutScreenId = layoutScreenId;
		}
		m_active->mouseMove(x, y);
	}
}

void
Server::jumpToScreen(BaseClientProxy* newScreen)
{
	assert(newScreen != NULL);

	// record the current cursor position on the active screen
	m_active->setJumpCursorPos(m_x, m_y);

	// get the last cursor position on the target screen
	SInt32 x, y;
	newScreen->getJumpCursorPos(x, y);

	std::string layoutScreenId;
	if (usingObjectLayout()) {
		const etherwaver::layout::Screen* screen = getLayoutScreenForHost(getName(newScreen));
		if (screen != NULL) {
			layoutScreenId = screen->m_id;
		}
	}

	switchScreen(newScreen, x, y, false, layoutScreenId);
}

float
Server::mapToFraction(BaseClientProxy* client,
				EDirection dir, SInt32 x, SInt32 y) const
{
	SInt32 sx, sy, sw, sh;
	client->getShape(sx, sy, sw, sh);
	switch (dir) {
	case kLeft:
	case kRight:
		return static_cast<float>(y - sy + 0.5f) / static_cast<float>(sh);

	case kTop:
	case kBottom:
		return static_cast<float>(x - sx + 0.5f) / static_cast<float>(sw);

	case kNoDirection:
		assert(0 && "bad direction");
		break;
	}
	return 0.0f;
}

void
Server::mapToPixel(BaseClientProxy* client,
				EDirection dir, float f, SInt32& x, SInt32& y) const
{
	SInt32 sx, sy, sw, sh;
	client->getShape(sx, sy, sw, sh);
	switch (dir) {
	case kLeft:
	case kRight:
		y = static_cast<SInt32>(f * sh) + sy;
		break;

	case kTop:
	case kBottom:
		x = static_cast<SInt32>(f * sw) + sx;
		break;

	case kNoDirection:
		assert(0 && "bad direction");
		break;
	}
}

bool
Server::hasAnyNeighbor(BaseClientProxy* client, EDirection dir) const
{
	assert(client != NULL);

	if (usingObjectLayout()) {
		const etherwaver::layout::Screen* screen = getActiveLayoutScreen();
		if (screen == NULL || screen->m_hostId != getName(client)) {
			screen = getLayoutScreenForHost(getName(client));
		}
		return (screen != NULL && m_screenLayout.hasAdjacentScreen(screen->m_id, dir));
	}

	return m_config->hasNeighbor(getName(client), dir);
}

BaseClientProxy*
Server::getNeighbor(BaseClientProxy* src,
				EDirection dir, SInt32& x, SInt32& y) const
{
	// note -- must be locked on entry

	assert(src != NULL);

	if (usingObjectLayout()) {
		const etherwaver::layout::Screen* sourceScreen = getActiveLayoutScreen();
		if (sourceScreen == NULL || sourceScreen->m_hostId != getName(src)) {
			sourceScreen = getLayoutScreenForHost(getName(src));
		}
		if (sourceScreen == NULL) {
			return NULL;
		}

		const etherwaver::layout::Screen* destinationScreen =
			m_screenLayout.findScreenInDirection(sourceScreen->m_id, dir);
		if (destinationScreen == NULL) {
			return NULL;
		}

		BaseClientProxy* dst = getClientForLayoutScreen(*destinationScreen);
		if (dst == NULL) {
			return NULL;
		}

		SInt32 sx, sy, sw, sh;
		SInt32 dx, dy, dw, dh;
		src->getShape(sx, sy, sw, sh);
		dst->getShape(dx, dy, dw, dh);

		const int globalX = toGlobalCoordinate(x, sx, sw, sourceScreen->m_x, sourceScreen->m_width);
		const int globalY = toGlobalCoordinate(y, sy, sh, sourceScreen->m_y, sourceScreen->m_height);
		x = clampInt(toClientCoordinate(globalX, destinationScreen->m_x, destinationScreen->m_width, dx, dw),
		             dx, dx + dw - 1);
		y = clampInt(toClientCoordinate(globalY, destinationScreen->m_y, destinationScreen->m_height, dy, dh),
		             dy, dy + dh - 1);
		return dst;
	}

	// get source screen name
    std::string srcName = getName(src);
	assert(!srcName.empty());
	LOG((CLOG_DEBUG2 "find neighbor on %s of \"%s\"", Config::dirName(dir), srcName.c_str()));

	// convert position to fraction
	float t = mapToFraction(src, dir, x, y);

	// search for the closest neighbor that exists in direction dir
	float tTmp;
	for (;;) {
        std::string dstName(m_config->getNeighbor(srcName, dir, t, &tTmp));

		// if nothing in that direction then return NULL. if the
		// destination is the source then we can make no more
		// progress in this direction.  since we haven't found a
		// connected neighbor we return NULL.
		if (dstName.empty()) {
			LOG((CLOG_DEBUG2 "no neighbor on %s of \"%s\"", Config::dirName(dir), srcName.c_str()));
			return NULL;
		}

		// look up neighbor cell.  if the screen is connected and
		// ready then we can stop.
		ClientList::const_iterator index = m_clients.find(dstName);
		if (index != m_clients.end()) {
			LOG((CLOG_DEBUG2 "\"%s\" is on %s of \"%s\" at %f", dstName.c_str(), Config::dirName(dir), srcName.c_str(), t));
			mapToPixel(index->second, dir, tTmp, x, y);
			return index->second;
		}

		// skip over unconnected screen
		LOG((CLOG_DEBUG2 "ignored \"%s\" on %s of \"%s\"", dstName.c_str(), Config::dirName(dir), srcName.c_str()));
		srcName = dstName;

		// use position on skipped screen
		t = tTmp;
	}
}

BaseClientProxy*
Server::mapToNeighbor(BaseClientProxy* src,
				EDirection srcSide, SInt32& x, SInt32& y) const
{
	// note -- must be locked on entry

	assert(src != NULL);

	if (usingObjectLayout()) {
		return getNeighbor(src, srcSide, x, y);
	}

	// get the first neighbor
	BaseClientProxy* dst = getNeighbor(src, srcSide, x, y);
	if (dst == NULL) {
		return NULL;
	}

	// get the source screen's size
	SInt32 dx, dy, dw, dh;
	BaseClientProxy* lastGoodScreen = src;
	lastGoodScreen->getShape(dx, dy, dw, dh);

	// find destination screen, adjusting x or y (but not both).  the
	// searches are done in a sort of canonical screen space where
	// the upper-left corner is 0,0 for each screen.  we adjust from
	// actual to canonical position on entry to and from canonical to
	// actual on exit from the search.
	switch (srcSide) {
	case kLeft:
		x -= dx;
		while (dst != NULL) {
			lastGoodScreen = dst;
			lastGoodScreen->getShape(dx, dy, dw, dh);
			x += dw;
			if (x >= 0) {
				break;
			}
			LOG((CLOG_DEBUG2 "skipping over screen %s", getName(dst).c_str()));
			dst = getNeighbor(lastGoodScreen, srcSide, x, y);
		}
		assert(lastGoodScreen != NULL);
		x += dx;
		break;

	case kRight:
		x -= dx;
		while (dst != NULL) {
			x -= dw;
			lastGoodScreen = dst;
			lastGoodScreen->getShape(dx, dy, dw, dh);
			if (x < dw) {
				break;
			}
			LOG((CLOG_DEBUG2 "skipping over screen %s", getName(dst).c_str()));
			dst = getNeighbor(lastGoodScreen, srcSide, x, y);
		}
		assert(lastGoodScreen != NULL);
		x += dx;
		break;

	case kTop:
		y -= dy;
		while (dst != NULL) {
			lastGoodScreen = dst;
			lastGoodScreen->getShape(dx, dy, dw, dh);
			y += dh;
			if (y >= 0) {
				break;
			}
			LOG((CLOG_DEBUG2 "skipping over screen %s", getName(dst).c_str()));
			dst = getNeighbor(lastGoodScreen, srcSide, x, y);
		}
		assert(lastGoodScreen != NULL);
		y += dy;
		break;

	case kBottom:
		y -= dy;
		while (dst != NULL) {
			y -= dh;
			lastGoodScreen = dst;
			lastGoodScreen->getShape(dx, dy, dw, dh);
			if (y < dh) {
				break;
			}
			LOG((CLOG_DEBUG2 "skipping over screen %s", getName(dst).c_str()));
			dst = getNeighbor(lastGoodScreen, srcSide, x, y);
		}
		assert(lastGoodScreen != NULL);
		y += dy;
		break;

	case kNoDirection:
		assert(0 && "bad direction");
		return NULL;
	}

	// save destination screen
	assert(lastGoodScreen != NULL);
	dst = lastGoodScreen;

	// if entering primary screen then be sure to move in far enough
	// to avoid the jump zone.  if entering a side that doesn't have
	// a neighbor (i.e. an asymmetrical side) then we don't need to
	// move inwards because that side can't provoke a jump.
	avoidJumpZone(dst, srcSide, x, y);

	return dst;
}

void
Server::avoidJumpZone(BaseClientProxy* dst,
				EDirection dir, SInt32& x, SInt32& y) const
{
	// we only need to avoid jump zones on the primary screen
	if (dst != m_primaryClient) {
		return;
	}

	if (usingObjectLayout()) {
		const etherwaver::layout::Screen* screen = getActiveLayoutScreen();
		if (screen == NULL) {
			return;
		}

		SInt32 dx, dy, dw, dh;
		dst->getShape(dx, dy, dw, dh);
		SInt32 z = getJumpZoneSize(dst);

		switch (dir) {
		case kLeft:
			if (m_screenLayout.hasAdjacentScreen(screen->m_id, kRight) &&
				x > dx + dw - 1 - z) {
				x = dx + dw - 1 - z;
			}
			break;
		case kRight:
			if (m_screenLayout.hasAdjacentScreen(screen->m_id, kLeft) &&
				x < dx + z) {
				x = dx + z;
			}
			break;
		case kTop:
			if (m_screenLayout.hasAdjacentScreen(screen->m_id, kBottom) &&
				y > dy + dh - 1 - z) {
				y = dy + dh - 1 - z;
			}
			break;
		case kBottom:
			if (m_screenLayout.hasAdjacentScreen(screen->m_id, kTop) &&
				y < dy + z) {
				y = dy + z;
			}
			break;
		default:
			break;
		}
		return;
	}

    const std::string dstName(getName(dst));
	SInt32 dx, dy, dw, dh;
	dst->getShape(dx, dy, dw, dh);
	float t = mapToFraction(dst, dir, x, y);
	SInt32 z = getJumpZoneSize(dst);

	// move in far enough to avoid the jump zone.  if entering a side
	// that doesn't have a neighbor (i.e. an asymmetrical side) then we
	// don't need to move inwards because that side can't provoke a jump.
	switch (dir) {
	case kLeft:
		if (!m_config->getNeighbor(dstName, kRight, t, NULL).empty() &&
			x > dx + dw - 1 - z)
			x = dx + dw - 1 - z;
		break;

	case kRight:
		if (!m_config->getNeighbor(dstName, kLeft, t, NULL).empty() &&
			x < dx + z)
			x = dx + z;
		break;

	case kTop:
		if (!m_config->getNeighbor(dstName, kBottom, t, NULL).empty() &&
			y > dy + dh - 1 - z)
			y = dy + dh - 1 - z;
		break;

	case kBottom:
		if (!m_config->getNeighbor(dstName, kTop, t, NULL).empty() &&
			y < dy + z)
			y = dy + z;
		break;

	case kNoDirection:
		assert(0 && "bad direction");
	}
}

bool
Server::isSwitchOkay(BaseClientProxy* newScreen,
				EDirection dir, SInt32 x, SInt32 y,
				SInt32 xActive, SInt32 yActive)
{
	LOG((CLOG_DEBUG1 "try to leave \"%s\" on %s", getName(m_active).c_str(), Config::dirName(dir)));

	// is there a neighbor?
	if (newScreen == NULL) {
		// there's no neighbor.  we don't want to switch and we don't
		// want to try to switch later.
		LOG((CLOG_DEBUG1 "no neighbor %s", Config::dirName(dir)));
		stopSwitch();
		return false;
	}

    if (isRecentReverseSwitch(newScreen, dir)) {
        LOG((CLOG_DEBUG1 "blocked recent reverse switch"));
        return false;
    }

	// should we switch or not?
	bool preventSwitch = false;
	bool allowSwitch   = false;

	// note if the switch direction has changed.  save the new
	// direction and screen if so.
	bool isNewDirection  = (dir != m_switchDir);
	if (isNewDirection || m_switchScreen == NULL) {
		m_switchDir    = dir;
		m_switchScreen = newScreen;
	}

	// is this a double tap and do we care?
	if (!allowSwitch && m_switchTwoTapDelay > 0.0) {
		if (isNewDirection ||
			!isSwitchTwoTapStarted() || !shouldSwitchTwoTap()) {
			// tapping a different or new edge or second tap not
			// fast enough.  prepare for second tap.
			preventSwitch = true;
			startSwitchTwoTap();
		}
		else {
			// got second tap
			allowSwitch = true;
		}
	}

	// if waiting before a switch then prepare to switch later
	if (!allowSwitch && m_switchWaitDelay > 0.0) {
		if (isNewDirection || !isSwitchWaitStarted()) {
			startSwitchWait(x, y);
		}
		preventSwitch = true;
	}

	// are we in a locked corner?  first check if screen has the option set
	// and, if not, check the global options.
	const Config::ScreenOptions* options =
						m_config->getOptions(getName(m_active));
	if (options == NULL || options->count(kOptionScreenSwitchCorners) == 0) {
		options = m_config->getOptions("");
	}
	if (options != NULL && options->count(kOptionScreenSwitchCorners) > 0) {
		// get corner mask and size
		Config::ScreenOptions::const_iterator i =
			options->find(kOptionScreenSwitchCorners);
		UInt32 corners = static_cast<UInt32>(i->second);
		i = options->find(kOptionScreenSwitchCornerSize);
		SInt32 size = 0;
		if (i != options->end()) {
			size = i->second;
		}

		// see if we're in a locked corner
		if ((getCorner(m_active, xActive, yActive, size) & corners) != 0) {
			// yep, no switching
			LOG((CLOG_DEBUG1 "locked in corner"));
			preventSwitch = true;
			stopSwitch();
		}
	}

	// ignore if mouse is locked to screen and don't try to switch later
	if (!preventSwitch && isLockedToScreen()) {
		LOG((CLOG_DEBUG1 "locked to screen"));
		preventSwitch = true;
		stopSwitch();
	}

	// check for optional needed modifiers
	KeyModifierMask mods = this->m_primaryClient->getToggleMask();

	if (!preventSwitch && (
			(this->m_switchNeedsShift && ((mods & KeyModifierShift) != KeyModifierShift)) ||
			(this->m_switchNeedsControl && ((mods & KeyModifierControl) != KeyModifierControl)) ||
			(this->m_switchNeedsAlt && ((mods & KeyModifierAlt) != KeyModifierAlt))
		)) {
		LOG((CLOG_DEBUG1 "need modifiers to switch"));
		preventSwitch = true;
		stopSwitch();
	}

	return !preventSwitch;
}

void
Server::noSwitch(SInt32 x, SInt32 y)
{
	armSwitchTwoTap(x, y);
	stopSwitchWait();
}

void
Server::stopSwitch()
{
	if (m_switchScreen != NULL) {
		m_switchScreen = NULL;
		m_switchDir    = kNoDirection;
		stopSwitchTwoTap();
		stopSwitchWait();
	}
}

void
Server::startSwitchTwoTap()
{
	m_switchTwoTapEngaged = true;
	m_switchTwoTapArmed   = false;
	m_switchTwoTapTimer.reset();
	LOG((CLOG_DEBUG1 "waiting for second tap"));
}

void
Server::armSwitchTwoTap(SInt32 x, SInt32 y)
{
	if (m_switchTwoTapEngaged) {
		if (m_switchTwoTapTimer.getTime() > m_switchTwoTapDelay) {
			// second tap took too long.  disengage.
			stopSwitchTwoTap();
		}
		else if (!m_switchTwoTapArmed) {
			// still time for a double tap.  see if we left the tap
			// zone and, if so, arm the two tap.
			SInt32 ax, ay, aw, ah;
			m_active->getShape(ax, ay, aw, ah);
			SInt32 tapZone = m_primaryClient->getJumpZoneSize();
			if (tapZone < m_switchTwoTapZone) {
				tapZone = m_switchTwoTapZone;
			}
			if (x >= ax + tapZone && x < ax + aw - tapZone &&
				y >= ay + tapZone && y < ay + ah - tapZone) {
				// win32 can generate bogus mouse events that appear to
				// move in the opposite direction that the mouse actually
				// moved.  try to ignore that crap here.
				switch (m_switchDir) {
				case kLeft:
					m_switchTwoTapArmed = (m_xDelta > 0 && m_xDelta2 > 0);
					break;

				case kRight:
					m_switchTwoTapArmed = (m_xDelta < 0 && m_xDelta2 < 0);
					break;

				case kTop:
					m_switchTwoTapArmed = (m_yDelta > 0 && m_yDelta2 > 0);
					break;

				case kBottom:
					m_switchTwoTapArmed = (m_yDelta < 0 && m_yDelta2 < 0);
					break;

				default:
					break;
				}
			}
		}
	}
}

void
Server::stopSwitchTwoTap()
{
	m_switchTwoTapEngaged = false;
	m_switchTwoTapArmed   = false;
}

bool
Server::isSwitchTwoTapStarted() const
{
	return m_switchTwoTapEngaged;
}

bool
Server::shouldSwitchTwoTap() const
{
	// this is the second tap if two-tap is armed and this tap
	// came fast enough
	return (m_switchTwoTapArmed &&
			m_switchTwoTapTimer.getTime() <= m_switchTwoTapDelay);
}

void
Server::startSwitchWait(SInt32 x, SInt32 y)
{
	stopSwitchWait();
	m_switchWaitX     = x;
	m_switchWaitY     = y;
	m_switchWaitTimer = m_events->newOneShotTimer(m_switchWaitDelay, this);
	LOG((CLOG_DEBUG1 "waiting to switch"));
}

void
Server::stopSwitchWait()
{
	if (m_switchWaitTimer != NULL) {
		m_events->deleteTimer(m_switchWaitTimer);
		m_switchWaitTimer = NULL;
	}
}

bool
Server::isSwitchWaitStarted() const
{
	return (m_switchWaitTimer != NULL);
}

UInt32
Server::getCorner(BaseClientProxy* client,
				SInt32 x, SInt32 y, SInt32 size) const
{
	assert(client != NULL);

	// get client screen shape
	SInt32 ax, ay, aw, ah;
	client->getShape(ax, ay, aw, ah);

	// check for x,y on the left or right
	SInt32 xSide;
	if (x <= ax) {
		xSide = -1;
	}
	else if (x >= ax + aw - 1) {
		xSide = 1;
	}
	else {
		xSide = 0;
	}

	// check for x,y on the top or bottom
	SInt32 ySide;
	if (y <= ay) {
		ySide = -1;
	}
	else if (y >= ay + ah - 1) {
		ySide = 1;
	}
	else {
		ySide = 0;
	}

	// if against the left or right then check if y is within size
	if (xSide != 0) {
		if (y < ay + size) {
			return (xSide < 0) ? kTopLeftMask : kTopRightMask;
		}
		else if (y >= ay + ah - size) {
			return (xSide < 0) ? kBottomLeftMask : kBottomRightMask;
		}
	}

	// if against the left or right then check if y is within size
	if (ySide != 0) {
		if (x < ax + size) {
			return (ySide < 0) ? kTopLeftMask : kBottomLeftMask;
		}
		else if (x >= ax + aw - size) {
			return (ySide < 0) ? kTopRightMask : kBottomRightMask;
		}
	}

	return kNoCornerMask;
}

void
Server::stopRelativeMoves()
{
	if (m_relativeMoves && m_active != m_primaryClient) {
		// warp to the center of the active client so we know where we are
		SInt32 ax, ay, aw, ah;
		m_active->getShape(ax, ay, aw, ah);
		m_x       = ax + (aw >> 1);
		m_y       = ay + (ah >> 1);
		m_xDelta  = 0;
		m_yDelta  = 0;
		m_xDelta2 = 0;
		m_yDelta2 = 0;
		LOG((CLOG_DEBUG2 "synchronize move on %s by %d,%d", getName(m_active).c_str(), m_x, m_y));
		m_active->mouseMove(m_x, m_y);
	}
}

void
Server::sendOptions(BaseClientProxy* client) const
{
	OptionsList optionsList;

	// look up options for client
	const Config::ScreenOptions* options =
						m_config->getOptions(getName(client));
	if (options != NULL) {
		// convert options to a more convenient form for sending
		optionsList.reserve(2 * options->size());
		for (Config::ScreenOptions::const_iterator index = options->begin();
									index != options->end(); ++index) {
			optionsList.push_back(index->first);
			optionsList.push_back(static_cast<UInt32>(index->second));
		}
	}

	// look up global options
	options = m_config->getOptions("");
	if (options != NULL) {
		// convert options to a more convenient form for sending
		optionsList.reserve(optionsList.size() + 2 * options->size());
		for (Config::ScreenOptions::const_iterator index = options->begin();
									index != options->end(); ++index) {
			optionsList.push_back(index->first);
			optionsList.push_back(static_cast<UInt32>(index->second));
		}
	}

	// send the options
	client->resetOptions();
	client->setOptions(optionsList);
}

void
Server::processOptions()
{
	const Config::ScreenOptions* options = m_config->getOptions("");
	if (options == NULL) {
		return;
	}

	m_switchNeedsShift = false;		// it seems if I don't add these
	m_switchNeedsControl = false;	// lines, the 'reload config' option
	m_switchNeedsAlt = false;		// doesn't work correct.

	bool newRelativeMoves = m_relativeMoves;
	for (Config::ScreenOptions::const_iterator index = options->begin();
								index != options->end(); ++index) {
		const OptionID id       = index->first;
		const OptionValue value = index->second;
		if (id == kOptionScreenSwitchDelay) {
			m_switchWaitDelay = 1.0e-3 * static_cast<double>(value);
			if (m_switchWaitDelay < 0.0) {
				m_switchWaitDelay = 0.0;
			}
			stopSwitchWait();
		}
		else if (id == kOptionScreenSwitchTwoTap) {
			m_switchTwoTapDelay = 1.0e-3 * static_cast<double>(value);
			if (m_switchTwoTapDelay < 0.0) {
				m_switchTwoTapDelay = 0.0;
			}
			stopSwitchTwoTap();
		}
		else if (id == kOptionScreenSwitchNeedsControl) {
			m_switchNeedsControl = (value != 0);
		}
		else if (id == kOptionScreenSwitchNeedsShift) {
			m_switchNeedsShift = (value != 0);
		}
		else if (id == kOptionScreenSwitchNeedsAlt) {
			m_switchNeedsAlt = (value != 0);
		}
		else if (id == kOptionRelativeMouseMoves) {
			newRelativeMoves = (value != 0);
		}
		else if (id == kOptionClipboardSharing) {
			m_enableClipboard = (value != 0);

			if (m_enableClipboard == false) {
				LOG((CLOG_NOTE "clipboard sharing is disabled"));
			}
		}
	}
	if (m_relativeMoves && !newRelativeMoves) {
		stopRelativeMoves();
	}
	m_relativeMoves = newRelativeMoves;
}

void
Server::handleShapeChanged(const Event&, void* vclient)
{
	// ignore events from unknown clients
	BaseClientProxy* client = static_cast<BaseClientProxy*>(vclient);
	if (m_clientSet.count(client) == 0) {
		return;
	}

	LOG((CLOG_DEBUG "screen \"%s\" shape changed", getName(client).c_str()));

	// update jump coordinate
	SInt32 x, y;
	client->getCursorPos(x, y);
	client->setJumpCursorPos(x, y);

	// update the mouse coordinates
	if (client == m_active) {
		m_x = x;
		m_y = y;
	}

	reloadScreenLayout();

	// handle resolution change to primary screen
	if (client == m_primaryClient) {
		if (client == m_active) {
			onMouseMovePrimary(m_x, m_y);
		}
		else {
			onMouseMoveSecondary(0, 0);
		}
	}
}

void
Server::handleClipboardGrabbed(const Event& event, void* vclient)
{
	if (!m_enableClipboard) {
		return;
	}

	// ignore events from unknown clients
	BaseClientProxy* grabber = static_cast<BaseClientProxy*>(vclient);
	if (m_clientSet.count(grabber) == 0) {
		return;
	}
	const IScreen::ClipboardInfo* info =
		static_cast<const IScreen::ClipboardInfo*>(event.getData());

	// ignore grab if sequence number is old.  always allow primary
	// screen to grab.
	ClipboardInfo& clipboard = m_clipboards[info->m_id];
	if (grabber != m_primaryClient &&
		info->m_sequenceNumber < clipboard.m_clipboardSeqNum) {
		LOG((CLOG_INFO "ignored screen \"%s\" grab of clipboard %d", getName(grabber).c_str(), info->m_id));
		return;
	}

	// mark screen as owning clipboard
	LOG((CLOG_INFO "screen \"%s\" grabbed clipboard %d from \"%s\"", getName(grabber).c_str(), info->m_id, clipboard.m_clipboardOwner.c_str()));
	clipboard.m_clipboardOwner  = getName(grabber);
	clipboard.m_clipboardSeqNum = info->m_sequenceNumber;

	// clear the clipboard data (since it's not known at this point)
	if (clipboard.m_clipboard.open(0)) {
		clipboard.m_clipboard.empty();
		clipboard.m_clipboard.close();
	}
	clipboard.m_clipboardData = clipboard.m_clipboard.marshall();

	// tell all other screens to take ownership of clipboard.  tell the
	// grabber that it's clipboard isn't dirty.
	for (ClientList::iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		BaseClientProxy* client = index->second;
		if (client == grabber) {
			client->setClipboardDirty(info->m_id, false);
		}
		else {
			client->grabClipboard(info->m_id);
		}
	}
}

void
Server::handleClipboardChanged(const Event& event, void* vclient)
{
	// ignore events from unknown clients
	BaseClientProxy* sender = static_cast<BaseClientProxy*>(vclient);
	if (m_clientSet.count(sender) == 0) {
		return;
	}
	const IScreen::ClipboardInfo* info =
		static_cast<const IScreen::ClipboardInfo*>(event.getData());
	onClipboardChanged(sender, info->m_id, info->m_sequenceNumber);
}

void
Server::handleKeyDownEvent(const Event& event, void*)
{
	IPlatformScreen::KeyInfo* info =
		static_cast<IPlatformScreen::KeyInfo*>(event.getData());
	onKeyDown(info->m_key, info->m_mask, info->m_button, info->m_screens);
}

void
Server::handleKeyUpEvent(const Event& event, void*)
{
	IPlatformScreen::KeyInfo* info =
		 static_cast<IPlatformScreen::KeyInfo*>(event.getData());
	onKeyUp(info->m_key, info->m_mask, info->m_button, info->m_screens);
}

void
Server::handleKeyRepeatEvent(const Event& event, void*)
{
	IPlatformScreen::KeyInfo* info =
		static_cast<IPlatformScreen::KeyInfo*>(event.getData());
	onKeyRepeat(info->m_key, info->m_mask, info->m_count, info->m_button);
}

void
Server::handleButtonDownEvent(const Event& event, void*)
{
	IPlatformScreen::ButtonInfo* info =
		static_cast<IPlatformScreen::ButtonInfo*>(event.getData());
	onMouseDown(info->m_button);
}

void
Server::handleButtonUpEvent(const Event& event, void*)
{
	IPlatformScreen::ButtonInfo* info =
		static_cast<IPlatformScreen::ButtonInfo*>(event.getData());
	onMouseUp(info->m_button);
}

void
Server::handleMotionPrimaryEvent(const Event& event, void*)
{
	IPlatformScreen::MotionInfo* info =
		static_cast<IPlatformScreen::MotionInfo*>(event.getData());
	onMouseMovePrimary(info->m_x, info->m_y);
}

void
Server::handleMotionSecondaryEvent(const Event& event, void*)
{
	IPlatformScreen::MotionInfo* info =
		static_cast<IPlatformScreen::MotionInfo*>(event.getData());
	onMouseMoveSecondary(info->m_x, info->m_y);
}

void
Server::handleWheelEvent(const Event& event, void*)
{
	IPlatformScreen::WheelInfo* info =
		static_cast<IPlatformScreen::WheelInfo*>(event.getData());
	onMouseWheel(info->m_xDelta, info->m_yDelta);
}

void
Server::handleScreensaverActivatedEvent(const Event&, void*)
{
	onScreensaver(true);
}

void
Server::handleScreensaverDeactivatedEvent(const Event&, void*)
{
	onScreensaver(false);
}

void
Server::handleSwitchWaitTimeout(const Event&, void*)
{
	// ignore if mouse is locked to screen
	if (isLockedToScreen()) {
		LOG((CLOG_DEBUG1 "locked to screen"));
		stopSwitch();
		return;
	}

	LOG((CLOG_INFO
		"switch-wait timeout activeHost=%s targetHost=%s target=%d,%d objectLayout=%s",
		getName(m_active).c_str(),
		(m_switchScreen != NULL ? getName(m_switchScreen).c_str() : "<none>"),
		m_switchWaitX, m_switchWaitY,
		usingObjectLayout() ? "yes" : "no"));

	// switch screen
	switchScreen(m_switchScreen, m_switchWaitX, m_switchWaitY, false);
}

void
Server::handleClientDisconnected(const Event&, void* vclient)
{
	// client has disconnected.  it might be an old client or an
	// active client.  we don't care so just handle it both ways.
	BaseClientProxy* client = static_cast<BaseClientProxy*>(vclient);
	removeActiveClient(client);
	removeOldClient(client);

	delete client;
}

void
Server::handleClientCloseTimeout(const Event&, void* vclient)
{
	// client took too long to disconnect.  just dump it.
	BaseClientProxy* client = static_cast<BaseClientProxy*>(vclient);
	LOG((CLOG_NOTE "forced disconnection of client \"%s\"", getName(client).c_str()));
	removeOldClient(client);

	delete client;
}

void
Server::handleSwitchToScreenEvent(const Event& event, void*)
{
	SwitchToScreenInfo* info =
		static_cast<SwitchToScreenInfo*>(event.getData());

	if (!switchToScreenName(info->m_screen)) {
		LOG((CLOG_DEBUG1 "screen \"%s\" not active", info->m_screen));
	}
}

void
Server::handleToggleScreenEvent(const Event& event, void*)
{
  if (usingObjectLayout()) {
    const etherwaver::layout::Screen* next = m_screenLayout.getNextScreen(m_activeLayoutScreenId);
    if (next != NULL) {
      BaseClientProxy* client = getClientForLayoutScreen(*next);
      if (client != NULL) {
        SInt32 x = 0;
        SInt32 y = 0;
        client->getJumpCursorPos(x, y);
        switchScreen(client, x, y, false, next->m_id);
      }
    }
    return;
  }

  std::string current = getName(m_active);
  ClientList::const_iterator index = m_clients.find(current);
  if (index == m_clients.end()) {
    LOG((CLOG_DEBUG1 "screen \"%s\" not active", current.c_str()));
  }
  else {
    ++index;
    if (index == m_clients.end()) {
      index = m_clients.begin();
    }
    jumpToScreen(index->second);
  }
}


void
Server::handleSwitchInDirectionEvent(const Event& event, void*)
{
	SwitchInDirectionInfo* info =
		static_cast<SwitchInDirectionInfo*>(event.getData());

	if (usingObjectLayout()) {
		const etherwaver::layout::Screen* activeScreen = getActiveLayoutScreen();
		if (activeScreen != NULL) {
			const etherwaver::layout::Screen* next =
				m_screenLayout.findScreenInDirection(activeScreen->m_id, info->m_direction);
			if (next != NULL) {
				BaseClientProxy* client = getClientForLayoutScreen(*next);
				if (client != NULL) {
					SInt32 x = 0;
					SInt32 y = 0;
					client->getJumpCursorPos(x, y);
					switchScreen(client, x, y, false, next->m_id);
				}
			}
		}
		return;
	}

	// jump to screen in chosen direction from center of this screen
	SInt32 x = m_x, y = m_y;
	BaseClientProxy* newScreen =
		getNeighbor(m_active, info->m_direction, x, y);
	if (newScreen == NULL) {
		LOG((CLOG_DEBUG1 "no neighbor %s", Config::dirName(info->m_direction)));
	}
	else {
		jumpToScreen(newScreen);
	}
}

void
Server::handleKeyboardBroadcastEvent(const Event& event, void*)
{
	KeyboardBroadcastInfo* info = (KeyboardBroadcastInfo*)event.getData();

	// choose new state
	bool newState;
	switch (info->m_state) {
	case KeyboardBroadcastInfo::kOff:
		newState = false;
		break;

	default:
	case KeyboardBroadcastInfo::kOn:
		newState = true;
		break;

	case KeyboardBroadcastInfo::kToggle:
		newState = !m_keyboardBroadcasting;
		break;
	}

	// enter new state
	if (newState != m_keyboardBroadcasting ||
		info->m_screens != m_keyboardBroadcastingScreens) {
		m_keyboardBroadcasting        = newState;
		m_keyboardBroadcastingScreens = info->m_screens;
		LOG((CLOG_DEBUG "keyboard broadcasting %s: %s", m_keyboardBroadcasting ? "on" : "off", m_keyboardBroadcastingScreens.c_str()));
	}
}

void
Server::handleLockCursorToScreenEvent(const Event& event, void*)
{
	LockCursorToScreenInfo* info = (LockCursorToScreenInfo*)event.getData();

	// choose new state
	bool newState;
	switch (info->m_state) {
	case LockCursorToScreenInfo::kOff:
		newState = false;
		break;

	default:
	case LockCursorToScreenInfo::kOn:
		newState = true;
		break;

	case LockCursorToScreenInfo::kToggle:
		newState = !m_lockedToScreen;
		break;
	}

	// enter new state
	if (newState != m_lockedToScreen) {
		m_lockedToScreen = newState;
		LOG((CLOG_NOTE "cursor %s current screen", m_lockedToScreen ? "locked to" : "unlocked from"));

		m_primaryClient->reconfigure(getActivePrimarySides());
		if (!isLockedToScreenServer()) {
			stopRelativeMoves();
		}
	}
}

void
Server::handleFakeInputBeginEvent(const Event&, void*)
{
	m_primaryClient->fakeInputBegin();
}

void
Server::handleFakeInputEndEvent(const Event&, void*)
{
	m_primaryClient->fakeInputEnd();
}

void
Server::handleFileChunkSendingEvent(const Event& event, void*)
{
	onFileChunkSending(event.getData());
}

void
Server::handleFileRecieveCompletedEvent(const Event& event, void*)
{
	onFileRecieveCompleted();
}

void
Server::onClipboardChanged(BaseClientProxy* sender,
				ClipboardID id, UInt32 seqNum)
{
	ClipboardInfo& clipboard = m_clipboards[id];

	// ignore update if sequence number is old
	if (seqNum < clipboard.m_clipboardSeqNum) {
		LOG((CLOG_INFO "ignored screen \"%s\" update of clipboard %d (missequenced)", getName(sender).c_str(), id));
		return;
	}

	// should be the expected client
	assert(sender == m_clients.find(clipboard.m_clipboardOwner)->second);

	// get data
	sender->getClipboard(id, &clipboard.m_clipboard);

	// ignore if data hasn't changed
    std::string data = clipboard.m_clipboard.marshall();
	if (data == clipboard.m_clipboardData) {
		LOG((CLOG_DEBUG "ignored screen \"%s\" update of clipboard %d (unchanged)", clipboard.m_clipboardOwner.c_str(), id));
		return;
	}

	// got new data
	LOG((CLOG_INFO "screen \"%s\" updated clipboard %d", clipboard.m_clipboardOwner.c_str(), id));
	clipboard.m_clipboardData = data;

	// tell all clients except the sender that the clipboard is dirty
	for (ClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		BaseClientProxy* client = index->second;
		client->setClipboardDirty(id, client != sender);
	}

	// send the new clipboard to the active screen
	m_active->setClipboard(id, &clipboard.m_clipboard);
}

void
Server::onScreensaver(bool activated)
{
	LOG((CLOG_DEBUG "onScreenSaver %s", activated ? "activated" : "deactivated"));

	if (activated) {
		// save current screen and position
		m_activeSaver = m_active;
		m_xSaver      = m_x;
		m_ySaver      = m_y;

		// jump to primary screen
		if (m_active != m_primaryClient) {
			switchScreen(m_primaryClient, 0, 0, true);
		}
	}
	else {
		// jump back to previous screen and position.  we must check
		// that the position is still valid since the screen may have
		// changed resolutions while the screen saver was running.
		if (m_activeSaver != NULL && m_activeSaver != m_primaryClient) {
			// check position
			BaseClientProxy* screen = m_activeSaver;
			SInt32 x, y, w, h;
			screen->getShape(x, y, w, h);
			SInt32 zoneSize = getJumpZoneSize(screen);
			if (m_xSaver < x + zoneSize) {
				m_xSaver = x + zoneSize;
			}
			else if (m_xSaver >= x + w - zoneSize) {
				m_xSaver = x + w - zoneSize - 1;
			}
			if (m_ySaver < y + zoneSize) {
				m_ySaver = y + zoneSize;
			}
			else if (m_ySaver >= y + h - zoneSize) {
				m_ySaver = y + h - zoneSize - 1;
			}

			// jump
			switchScreen(screen, m_xSaver, m_ySaver, false);
		}

		// reset state
		m_activeSaver = NULL;
	}

	// send message to all clients
	for (ClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		BaseClientProxy* client = index->second;
		client->screensaver(activated);
	}
}

void
Server::onKeyDown(KeyID id, KeyModifierMask mask, KeyButton button,
				const char* screens)
{
	LOG((CLOG_DEBUG1 "onKeyDown id=%d mask=0x%04x button=0x%04x", id, mask, button));
	assert(m_active != NULL);

	// relay
	if (!m_keyboardBroadcasting && IKeyState::KeyInfo::isDefault(screens)) {
		m_active->keyDown(id, mask, button);
	}
	else {
		if (!screens && m_keyboardBroadcasting) {
			screens = m_keyboardBroadcastingScreens.c_str();
			if (IKeyState::KeyInfo::isDefault(screens)) {
				screens = "*";
			}
		}
		for (ClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
			if (IKeyState::KeyInfo::contains(screens, index->first)) {
				index->second->keyDown(id, mask, button);
			}
		}
	}
}

void
Server::onKeyUp(KeyID id, KeyModifierMask mask, KeyButton button,
				const char* screens)
{
	LOG((CLOG_DEBUG1 "onKeyUp id=%d mask=0x%04x button=0x%04x", id, mask, button));
	assert(m_active != NULL);

	// relay
	if (!m_keyboardBroadcasting && IKeyState::KeyInfo::isDefault(screens)) {
		m_active->keyUp(id, mask, button);
	}
	else {
		if (!screens && m_keyboardBroadcasting) {
			screens = m_keyboardBroadcastingScreens.c_str();
			if (IKeyState::KeyInfo::isDefault(screens)) {
				screens = "*";
			}
		}
		for (ClientList::const_iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
			if (IKeyState::KeyInfo::contains(screens, index->first)) {
				index->second->keyUp(id, mask, button);
			}
		}
	}
}

void
Server::onKeyRepeat(KeyID id, KeyModifierMask mask,
				SInt32 count, KeyButton button)
{
	LOG((CLOG_DEBUG1 "onKeyRepeat id=%d mask=0x%04x count=%d button=0x%04x", id, mask, count, button));
	assert(m_active != NULL);

	// relay
	m_active->keyRepeat(id, mask, count, button);
}

void
Server::onMouseDown(ButtonID id)
{
	LOG((CLOG_DEBUG1 "onMouseDown id=%d", id));
	assert(m_active != NULL);

	// relay
	m_active->mouseDown(id);

	// reset this variable back to default value true
	m_waitDragInfoThread = true;
}

void
Server::onMouseUp(ButtonID id)
{
	LOG((CLOG_DEBUG1 "onMouseUp id=%d", id));
	assert(m_active != NULL);

	// relay
	m_active->mouseUp(id);

	if (m_ignoreFileTransfer) {
		m_ignoreFileTransfer = false;
		return;
	}

	if (m_args.m_enableDragDrop) {
		if (!m_screen->isOnScreen()) {
            std::string& file = m_screen->getDraggingFilename();
			if (!file.empty()) {
				sendFileToClient(file.c_str());
			}
		}

		// always clear dragging filename
		m_screen->clearDraggingFilename();
	}
}

bool
Server::onMouseMovePrimary(SInt32 x, SInt32 y)
{
	LOG((CLOG_DEBUG4 "onMouseMovePrimary %d,%d", x, y));

	// mouse move on primary (server's) screen
	if (m_active != m_primaryClient) {
		// stale event -- we're actually on a secondary screen
		return false;
	}

	// save last delta
	m_xDelta2 = m_xDelta;
	m_yDelta2 = m_yDelta;

	// save current delta
	m_xDelta  = x - m_x;
	m_yDelta  = y - m_y;

	// save position
	m_x       = x;
	m_y       = y;

	if (usingObjectLayout()) {
        refreshPrimaryUhidGeometry();
        m_uhidEdgeTransitionService.updateSystemCursorSample(x, y);
        m_uhidTransitionTriggered = false;
        m_uhidEdgeTransitionService.onRelativeMouseMotion(m_xDelta, m_yDelta);
        if (m_uhidTransitionTriggered) {
            return true;
        }

		SInt32 ax, ay, aw, ah;
		m_active->getShape(ax, ay, aw, ah);
		SInt32 zoneSize = std::max<SInt32>(2, getJumpZoneSize(m_active));
		const etherwaver::layout::Screen* sourceScreen = getActiveLayoutScreen();
		if (sourceScreen == NULL) {
			noSwitch(x, y);
			return false;
		}

		const std::string sourceHostId = resolveLayoutHostId(m_screenLayout, sourceScreen->m_hostId);
		int hostMinX = sourceScreen->m_x;
		int hostMinY = sourceScreen->m_y;
		int hostMaxX = sourceScreen->m_x + sourceScreen->m_width;
		int hostMaxY = sourceScreen->m_y + sourceScreen->m_height;
		if (!getHostLayoutBounds(m_screenLayout, sourceHostId, hostMinX, hostMinY, hostMaxX, hostMaxY)) {
			hostMinX = sourceScreen->m_x;
			hostMinY = sourceScreen->m_y;
			hostMaxX = sourceScreen->m_x + sourceScreen->m_width;
			hostMaxY = sourceScreen->m_y + sourceScreen->m_height;
		}

		const int hostWidth = std::max<int>(1, hostMaxX - hostMinX);
		const int hostHeight = std::max<int>(1, hostMaxY - hostMinY);
		const int hostRight = hostMinX + hostWidth - 1;
		const int hostBottom = hostMinY + hostHeight - 1;
		const int localRight = ax + aw - 1;
		const int localBottom = ay + ah - 1;
		const SInt32 screenLeft =
			mapInclusiveCoordinate(sourceScreen->m_x, hostMinX, hostRight, ax, localRight);
		const SInt32 screenRight =
			mapInclusiveCoordinate(sourceScreen->m_x + sourceScreen->m_width - 1,
								   hostMinX, hostRight, ax, localRight);
		const SInt32 screenTop =
			mapInclusiveCoordinate(sourceScreen->m_y, hostMinY, hostBottom, ay, localBottom);
		const SInt32 screenBottom =
			mapInclusiveCoordinate(sourceScreen->m_y + sourceScreen->m_height - 1,
								   hostMinY, hostBottom, ay, localBottom);

		EDirection dirh = kNoDirection, dirv = kNoDirection;
		SInt32 xh = x, yv = y;
		if (x < screenLeft + zoneSize) {
			xh  -= zoneSize;
			dirh = kLeft;
		}
		else if (x >= screenRight - zoneSize + 1) {
			xh  += zoneSize;
			dirh = kRight;
		}
		if (y < screenTop + zoneSize) {
			yv  -= zoneSize;
			dirv = kTop;
		}
		else if (y >= screenBottom - zoneSize + 1) {
			yv  += zoneSize;
			dirv = kBottom;
		}

		if (dirh == kNoDirection && dirv == kNoDirection) {
			LOG((CLOG_INFO
				"object-layout primary no-edge activeHost=%s pos=%d,%d sysPos=%d,%d hostScreen=%s localScreen=%d,%d..%d,%d desktop=%d,%d %dx%d zone=%d",
				getName(m_active).c_str(),
                m_uhidEdgeTransitionService.virtualX(),
                m_uhidEdgeTransitionService.virtualY(),
                x, y, sourceScreen->m_id.c_str(),
				screenLeft, screenTop, screenRight, screenBottom,
				ax, ay, aw, ah, zoneSize));
			noSwitch(x, y);
			return false;
		}

		LOG((CLOG_INFO
			"object-layout primary edge activeHost=%s pos=%d,%d sysPos=%d,%d zone=%d dirh=%s dirv=%s xh=%d yv=%d",
			getName(m_active).c_str(),
            m_uhidEdgeTransitionService.virtualX(),
            m_uhidEdgeTransitionService.virtualY(),
            x, y, zoneSize,
			safeDirectionName(dirh), safeDirectionName(dirv), xh, yv));

		EDirection dirs[] = {dirh, dirv};
		SInt32 xs[] = {xh, x}, ys[] = {y, yv};
		for (int i = 0; i < 2; ++i) {
			if (dirs[i] == kNoDirection) {
				continue;
			}
			LOG((CLOG_INFO
				"object-layout primary attempt activeHost=%s direction=%s x=%d y=%d",
				getName(m_active).c_str(), safeDirectionName(dirs[i]), xs[i], ys[i]));
			if (trySwitchUsingObjectLayout(xs[i], ys[i], true)) {
				return true;
			}
		}

		LOG((CLOG_INFO
			"object-layout primary attempts-failed activeHost=%s pos=%d,%d",
			getName(m_active).c_str(), x, y));
		return false;
	}

	// get screen shape
	SInt32 ax, ay, aw, ah;
	m_active->getShape(ax, ay, aw, ah);
	SInt32 zoneSize = getJumpZoneSize(m_active);

	// clamp position to screen
	SInt32 xc = x, yc = y;
	if (xc < ax + zoneSize) {
		xc = ax;
	}
	else if (xc >= ax + aw - zoneSize) {
		xc = ax + aw - 1;
	}
	if (yc < ay + zoneSize) {
		yc = ay;
	}
	else if (yc >= ay + ah - zoneSize) {
		yc = ay + ah - 1;
	}

	// see if we should change screens
	// when the cursor is in a corner, there may be a screen either
	// horizontally or vertically.  check both directions.
	EDirection dirh = kNoDirection, dirv = kNoDirection;
	SInt32 xh = x, yv = y;
	if (x < ax + zoneSize) {
		xh  -= zoneSize;
		dirh = kLeft;
	}
	else if (x >= ax + aw - zoneSize) {
		xh  += zoneSize;
		dirh = kRight;
	}
	if (y < ay + zoneSize) {
		yv  -= zoneSize;
		dirv = kTop;
	}
	else if (y >= ay + ah - zoneSize) {
		yv  += zoneSize;
		dirv = kBottom;
	}
	if (dirh == kNoDirection && dirv == kNoDirection) {
		// still on local screen
		noSwitch(x, y);
		return false;
	}

	// check both horizontally and vertically
	EDirection dirs[] = {dirh, dirv};
	SInt32 xs[] = {xh, x}, ys[] = {y, yv};
	for (int i = 0; i < 2; ++i) {
		EDirection dir = dirs[i];
		if (dir == kNoDirection) {
			continue;
		}
		x = xs[i], y = ys[i];

		// get jump destination
		BaseClientProxy* newScreen = mapToNeighbor(m_active, dir, x, y);

		// should we switch or not?
		if (isSwitchOkay(newScreen, dir, x, y, xc, yc)) {
			if (m_args.m_enableDragDrop
				&& m_screen->isDraggingStarted()
				&& m_active != newScreen
				&& m_waitDragInfoThread) {
				if (m_sendDragInfoThread == NULL) {
                    m_sendDragInfoThread = new Thread([this, newScreen]()
                                                      { send_drag_info_thread(newScreen); });
				}

				return false;
			}

			// switch screen
			switchScreen(newScreen, x, y, false);
			m_waitDragInfoThread = true;
			return true;
		}
	}

	return false;
}

void Server::send_drag_info_thread(BaseClientProxy* newScreen)
{
	m_dragFileList.clear();
    std::string& dragFileList = m_screen->getDraggingFilename();
	if (!dragFileList.empty()) {
		DragInformation di;
		di.setFilename(dragFileList);
		m_dragFileList.push_back(di);
	}

#if defined(__APPLE__)
	// on mac it seems that after faking a LMB up, system would signal back
	// to barrier a mouse up event, which doesn't happen on windows. as a
	// result, barrier would send dragging file to client twice. This variable
	// is used to ignore the first file sending.
	m_ignoreFileTransfer = true;
#endif

	// send drag file info to client if there is any
	if (m_dragFileList.size() > 0) {
		sendDragInfo(newScreen);
		m_dragFileList.clear();
	}
	m_waitDragInfoThread = false;
	m_sendDragInfoThread = NULL;
}

void
Server::sendDragInfo(BaseClientProxy* newScreen)
{
    std::string infoString;
	UInt32 fileCount = DragInformation::setupDragInfo(m_dragFileList, infoString);

	if (fileCount > 0) {
		char* info = NULL;
		size_t size = infoString.size();
		info = new char[size];
		memcpy(info, infoString.c_str(), size);

		LOG((CLOG_DEBUG2 "sending drag information to client"));
		LOG((CLOG_DEBUG3 "dragging file list: %s", info));
		LOG((CLOG_DEBUG3 "dragging file list string size: %i", size));
		newScreen->sendDragInfo(fileCount, info, size);
	}
}

void
Server::onMouseMoveSecondary(SInt32 dx, SInt32 dy)
{
	LOG((CLOG_DEBUG2 "onMouseMoveSecondary %+d,%+d", dx, dy));

	// mouse move on secondary (client's) screen
	assert(m_active != NULL);
	if (m_active == m_primaryClient) {
		// stale event -- we're actually on the primary screen
		return;
	}

	// if doing relative motion on secondary screens and we're locked
	// to the screen (which activates relative moves) then send a
	// relative mouse motion.  when we're doing this we pretend as if
	// the mouse isn't actually moving because we're expecting some
	// program on the secondary screen to warp the mouse on us, so we
	// have no idea where it really is.
	if (m_relativeMoves && isLockedToScreenServer()) {
		LOG((CLOG_DEBUG2 "relative move on %s by %d,%d", getName(m_active).c_str(), dx, dy));
		m_active->mouseRelativeMove(dx, dy);
		return;
	}

	// save old position
	const SInt32 xOld = m_x;
	const SInt32 yOld = m_y;

	// save last delta
	m_xDelta2 = m_xDelta;
	m_yDelta2 = m_yDelta;

	// save current delta
	m_xDelta  = dx;
	m_yDelta  = dy;

	// accumulate motion
	m_x      += dx;
	m_y      += dy;

	if (usingObjectLayout() && trySwitchUsingObjectLayout(m_x, m_y, false)) {
		return;
	}

	// get screen shape
	SInt32 ax, ay, aw, ah;
	m_active->getShape(ax, ay, aw, ah);

	// find direction of neighbor and get the neighbor
	bool jump = true;
	BaseClientProxy* newScreen;
	do {
		// clamp position to screen
		SInt32 xc = m_x, yc = m_y;
		if (xc < ax) {
			xc = ax;
		}
		else if (xc >= ax + aw) {
			xc = ax + aw - 1;
		}
		if (yc < ay) {
			yc = ay;
		}
		else if (yc >= ay + ah) {
			yc = ay + ah - 1;
		}

		EDirection dir;
		if (m_x < ax) {
			dir = kLeft;
		}
		else if (m_x > ax + aw - 1) {
			dir = kRight;
		}
		else if (m_y < ay) {
			dir = kTop;
		}
		else if (m_y > ay + ah - 1) {
			dir = kBottom;
		}
		else {
			// we haven't left the screen
			newScreen = m_active;
			jump      = false;

			// if waiting and mouse is not on the border we're waiting
			// on then stop waiting.  also if it's not on the border
			// then arm the double tap.
			if (m_switchScreen != NULL) {
				bool clearWait;
				SInt32 zoneSize = m_primaryClient->getJumpZoneSize();
				switch (m_switchDir) {
				case kLeft:
					clearWait = (m_x >= ax + zoneSize);
					break;

				case kRight:
					clearWait = (m_x <= ax + aw - 1 - zoneSize);
					break;

				case kTop:
					clearWait = (m_y >= ay + zoneSize);
					break;

				case kBottom:
					clearWait = (m_y <= ay + ah - 1 + zoneSize);
					break;

				default:
					clearWait = false;
					break;
				}
				if (clearWait) {
					// still on local screen
					noSwitch(m_x, m_y);
				}
			}

			// skip rest of block
			break;
		}

		// try to switch screen.  get the neighbor.
		newScreen = mapToNeighbor(m_active, dir, m_x, m_y);

		// see if we should switch
		if (!isSwitchOkay(newScreen, dir, m_x, m_y, xc, yc)) {
			newScreen = m_active;
			jump      = false;
		}
	} while (false);

	if (jump) {
		if (m_sendFileThread != NULL) {
			StreamChunker::interruptFile();
			m_sendFileThread = NULL;
		}

		SInt32 newX = m_x;
		SInt32 newY = m_y;

		// switch screens
		switchScreen(newScreen, newX, newY, false);
	}
	else {
		// same screen.  clamp mouse to edge.
		m_x = xOld + dx;
		m_y = yOld + dy;
		if (m_x < ax) {
			m_x = ax;
			LOG((CLOG_DEBUG2 "clamp to left of \"%s\"", getName(m_active).c_str()));
		}
		else if (m_x > ax + aw - 1) {
			m_x = ax + aw - 1;
			LOG((CLOG_DEBUG2 "clamp to right of \"%s\"", getName(m_active).c_str()));
		}
		if (m_y < ay) {
			m_y = ay;
			LOG((CLOG_DEBUG2 "clamp to top of \"%s\"", getName(m_active).c_str()));
		}
		else if (m_y > ay + ah - 1) {
			m_y = ay + ah - 1;
			LOG((CLOG_DEBUG2 "clamp to bottom of \"%s\"", getName(m_active).c_str()));
		}

		// warp cursor if it moved.
		if (m_x != xOld || m_y != yOld) {
			LOG((CLOG_DEBUG2 "move on %s to %d,%d", getName(m_active).c_str(), m_x, m_y));
			m_active->mouseMove(m_x, m_y);
		}
	}
}

void
Server::onMouseWheel(SInt32 xDelta, SInt32 yDelta)
{
	LOG((CLOG_DEBUG1 "onMouseWheel %+d,%+d", xDelta, yDelta));
	assert(m_active != NULL);

	// relay
	m_active->mouseWheel(xDelta, yDelta);
}

void
Server::onFileChunkSending(const void* data)
{
	FileChunk* chunk = static_cast<FileChunk*>(const_cast<void*>(data));

	LOG((CLOG_DEBUG1 "sending file chunk"));
	assert(m_active != NULL);

	// relay
	m_active->fileChunkSending(chunk->m_chunk[0], &chunk->m_chunk[1], chunk->m_dataSize);
}

void
Server::onFileRecieveCompleted()
{
	if (isReceivedFileSizeValid()) {
        m_writeToDropDirThread = new Thread([this]() { write_to_drop_dir_thread(); });
	}
}

void Server::write_to_drop_dir_thread()
{
	LOG((CLOG_DEBUG "starting write to drop dir thread"));

	while (m_screen->isFakeDraggingStarted()) {
		ARCH->sleep(.1f);
	}

	DropHelper::writeToDir(m_screen->getDropTarget(), m_fakeDragFileList,
					m_receivedFileData);
}

bool
Server::addClient(BaseClientProxy* client)
{
    std::string name = getName(client);
	if (m_clients.count(name) != 0) {
		return false;
	}

	// add event handlers
	m_events->adoptHandler(m_events->forIScreen().shapeChanged(),
							client->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleShapeChanged, client));
	m_events->adoptHandler(m_events->forClipboard().clipboardGrabbed(),
							client->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleClipboardGrabbed, client));
	m_events->adoptHandler(m_events->forClipboard().clipboardChanged(),
							client->getEventTarget(),
							new TMethodEventJob<Server>(this,
								&Server::handleClipboardChanged, client));

	// add to list
	m_clientSet.insert(client);
	m_clients.insert(std::make_pair(name, client));

	// initialize client data
	SInt32 x, y;
	client->getCursorPos(x, y);
	client->setJumpCursorPos(x, y);

	// tell primary client about the active sides
	reloadScreenLayout();
	m_primaryClient->reconfigure(getActivePrimarySides());

	return true;
}

bool
Server::removeClient(BaseClientProxy* client)
{
	// return false if not in list
	ClientSet::iterator i = m_clientSet.find(client);
	if (i == m_clientSet.end()) {
		return false;
	}

	// remove event handlers
	m_events->removeHandler(m_events->forIScreen().shapeChanged(),
							client->getEventTarget());
	m_events->removeHandler(m_events->forClipboard().clipboardGrabbed(),
							client->getEventTarget());
	m_events->removeHandler(m_events->forClipboard().clipboardChanged(),
							client->getEventTarget());

	// remove from list
	m_clients.erase(getName(client));
	m_clientSet.erase(i);
	reloadScreenLayout();

	return true;
}

void
Server::closeClient(BaseClientProxy* client, const char* msg)
{
	assert(client != m_primaryClient);
	assert(msg != NULL);

	// send message to client.  this message should cause the client
	// to disconnect.  we add this client to the closed client list
	// and install a timer to remove the client if it doesn't respond
	// quickly enough.  we also remove the client from the active
	// client list since we're not going to listen to it anymore.
	// note that this method also works on clients that are not in
	// the m_clients list.  adoptClient() may call us with such a
	// client.
	LOG((CLOG_NOTE "disconnecting client \"%s\"", getName(client).c_str()));

	// send message
	// FIXME -- avoid type cast (kinda hard, though)
	((ClientProxy*)client)->close(msg);

	// install timer.  wait timeout seconds for client to close.
	double timeout = 5.0;
	EventQueueTimer* timer = m_events->newOneShotTimer(timeout, NULL);
	m_events->adoptHandler(Event::kTimer, timer,
							new TMethodEventJob<Server>(this,
								&Server::handleClientCloseTimeout, client));

	// move client to closing list
	removeClient(client);
	m_oldClients.insert(std::make_pair(client, timer));

	// if this client is the active screen then we have to
	// jump off of it
	forceLeaveClient(client);
}

void
Server::closeClients(const Config& config)
{
	// collect the clients that are connected but are being dropped
	// from the configuration (or who's canonical name is changing).
	typedef std::set<BaseClientProxy*> RemovedClients;
	RemovedClients removed;
	for (ClientList::iterator index = m_clients.begin();
								index != m_clients.end(); ++index) {
		if (!config.isCanonicalName(index->first)) {
			removed.insert(index->second);
		}
	}

	// don't close the primary client
	removed.erase(m_primaryClient);

	// now close them.  we collect the list then close in two steps
	// because closeClient() modifies the collection we iterate over.
	for (RemovedClients::iterator index = removed.begin();
								index != removed.end(); ++index) {
		closeClient(*index, kMsgCClose);
	}
}

void
Server::removeActiveClient(BaseClientProxy* client)
{
	if (removeClient(client)) {
		forceLeaveClient(client);
		m_events->removeHandler(m_events->forClientProxy().disconnected(), client);
		if (m_clients.size() == 1 && m_oldClients.empty()) {
			m_events->addEvent(Event(m_events->forServer().disconnected(), this));
		}
	}
}

void
Server::removeOldClient(BaseClientProxy* client)
{
	OldClients::iterator i = m_oldClients.find(client);
	if (i != m_oldClients.end()) {
		m_events->removeHandler(m_events->forClientProxy().disconnected(), client);
		m_events->removeHandler(Event::kTimer, i->second);
		m_events->deleteTimer(i->second);
		m_oldClients.erase(i);
		if (m_clients.size() == 1 && m_oldClients.empty()) {
			m_events->addEvent(Event(m_events->forServer().disconnected(), this));
		}
	}
}

void
Server::forceLeaveClient(BaseClientProxy* client)
{
	BaseClientProxy* active =
		(m_activeSaver != NULL) ? m_activeSaver : m_active;
	if (active == client) {
		// record new position (center of primary screen)
		m_primaryClient->getCursorCenter(m_x, m_y);

		// stop waiting to switch to this client
		if (active == m_switchScreen) {
			stopSwitch();
		}

		// don't notify active screen since it has probably already
		// disconnected.
		LOG((CLOG_INFO "jump from \"%s\" to \"%s\" at %d,%d", getName(active).c_str(), getName(m_primaryClient).c_str(), m_x, m_y));

		// cut over
		m_active = m_primaryClient;
		const etherwaver::layout::Screen* primaryScreen =
			getLayoutScreenForHost(getName(m_primaryClient));
		if (primaryScreen != NULL) {
			m_activeLayoutScreenId = primaryScreen->m_id;
		}

		// enter new screen (unless we already have because of the
		// screen saver)
		if (m_activeSaver == NULL) {
			m_primaryClient->enter(m_x, m_y, m_seqNum,
								m_primaryClient->getToggleMask(), false);
		}

		Server::SwitchToScreenInfo* info =
			Server::SwitchToScreenInfo::alloc(m_active->getName());
		m_events->addEvent(Event(m_events->forServer().screenSwitched(), this, info));
	}

	// if this screen had the cursor when the screen saver activated
	// then we can't switch back to it when the screen saver
	// deactivates.
	if (m_activeSaver == client) {
		m_activeSaver = NULL;
	}

	// tell primary client about the active sides
	m_primaryClient->reconfigure(getActivePrimarySides());
}


//
// Server::ClipboardInfo
//

Server::ClipboardInfo::ClipboardInfo() :
	m_clipboard(),
	m_clipboardData(),
	m_clipboardOwner(),
	m_clipboardSeqNum(0)
{
	// do nothing
}


//
// Server::LockCursorToScreenInfo
//

Server::LockCursorToScreenInfo*
Server::LockCursorToScreenInfo::alloc(State state)
{
	LockCursorToScreenInfo* info =
		(LockCursorToScreenInfo*)malloc(sizeof(LockCursorToScreenInfo));
	info->m_state = state;
	return info;
}


//
// Server::SwitchToScreenInfo
//

Server::SwitchToScreenInfo*
Server::SwitchToScreenInfo::alloc(const std::string& screen)
{
	SwitchToScreenInfo* info =
		(SwitchToScreenInfo*)malloc(sizeof(SwitchToScreenInfo) +
								screen.size());
	strcpy(info->m_screen, screen.c_str());
	return info;
}


//
// Server::SwitchInDirectionInfo
//

Server::SwitchInDirectionInfo*
Server::SwitchInDirectionInfo::alloc(EDirection direction)
{
	SwitchInDirectionInfo* info =
		(SwitchInDirectionInfo*)malloc(sizeof(SwitchInDirectionInfo));
	info->m_direction = direction;
	return info;
}

//
// Server::KeyboardBroadcastInfo
//

Server::KeyboardBroadcastInfo*
Server::KeyboardBroadcastInfo::alloc(State state)
{
	KeyboardBroadcastInfo* info =
		(KeyboardBroadcastInfo*)malloc(sizeof(KeyboardBroadcastInfo));
	info->m_state      = state;
	info->m_screens[0] = '\0';
	return info;
}

Server::KeyboardBroadcastInfo*
Server::KeyboardBroadcastInfo::alloc(State state, const std::string& screens)
{
	KeyboardBroadcastInfo* info =
		(KeyboardBroadcastInfo*)malloc(sizeof(KeyboardBroadcastInfo) +
								screens.size());
	info->m_state = state;
	strcpy(info->m_screens, screens.c_str());
	return info;
}

bool
Server::isReceivedFileSizeValid()
{
	return m_expectedFileSize == m_receivedFileData.size();
}

void
Server::sendFileToClient(const char* filename)
{
	if (m_sendFileThread != NULL) {
		StreamChunker::interruptFile();
	}

    m_sendFileThread = new Thread([this, filename]() { send_file_thread(filename); });
}

void Server::send_file_thread(const char* filename)
{
	try {
		LOG((CLOG_DEBUG "sending file to client, filename=%s", filename));
		StreamChunker::sendFile(filename, m_events, this);
	}
	catch (std::runtime_error &error) {
		LOG((CLOG_ERR "failed sending file chunks, error: %s", error.what()));
	}

	m_sendFileThread = NULL;
}

void
Server::dragInfoReceived(UInt32 fileNum, std::string content)
{
	if (!m_args.m_enableDragDrop) {
		LOG((CLOG_DEBUG "drag drop not enabled, ignoring drag info."));
		return;
	}

	DragInformation::parseDragInfo(m_fakeDragFileList, fileNum, content);

	m_screen->startDraggingFiles(m_fakeDragFileList);
}

void Server::httpLoop()
{
    while (m_running) {
        ArchSocket socket = ARCH->acceptSocket(m_httpListener, nullptr);

        if (!socket) {
            // prevent 100% CPU when no clients connect
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        try {
            std::string request;
            char requestBuf[4096];
            size_t bytesRead = ARCH->readSocket(socket, requestBuf, sizeof(requestBuf));
            if (bytesRead == 0) {
                ARCH->closeSocket(socket);
                continue;
            }
            request.append(requestBuf, bytesRead);

            size_t headerEnd = request.find("\r\n\r\n");
            size_t headerSize = 4;
            if (headerEnd == std::string::npos) {
                headerEnd = request.find("\n\n");
                headerSize = 2;
            }

            while (headerEnd == std::string::npos && request.size() < 1024 * 1024) {
                bytesRead = ARCH->readSocket(socket, requestBuf, sizeof(requestBuf));
                if (bytesRead == 0) {
                    break;
                }
                request.append(requestBuf, bytesRead);
                headerEnd = request.find("\r\n\r\n");
                headerSize = 4;
                if (headerEnd == std::string::npos) {
                    headerEnd = request.find("\n\n");
                    headerSize = 2;
                }
            }

            std::string method;
            std::string path;
            std::string requestLine;
            std::string headers;
            std::string body;

            if (headerEnd != std::string::npos) {
                headers = request.substr(0, headerEnd);
                body = request.substr(headerEnd + headerSize);
            }
            else {
                headers = request;
            }

            size_t lineEnd = headers.find("\r\n");
            if (lineEnd == std::string::npos) {
                lineEnd = headers.find('\n');
            }
            if (lineEnd != std::string::npos) {
                requestLine = headers.substr(0, lineEnd);
                std::istringstream iss(requestLine);
                iss >> method >> path;
            }

            size_t contentLength = 0;
            {
                std::istringstream hs(headers);
                std::string line;
                // skip request line
                std::getline(hs, line);
                while (std::getline(hs, line)) {
                    if (!line.empty() && line[line.size() - 1] == '\r') {
                        line.erase(line.size() - 1);
                    }
                    std::string lowerLine = line;
                    for (size_t i = 0; i < lowerLine.size(); ++i) {
                        lowerLine[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(lowerLine[i])));
                    }
                    if (lowerLine.compare(0, 15, "content-length:") == 0) {
                        std::string lenStr = line.substr(15);
                        size_t firstNotSpace = lenStr.find_first_not_of(" \t");
                        if (firstNotSpace != std::string::npos) {
                            lenStr = lenStr.substr(firstNotSpace);
                            contentLength = static_cast<size_t>(std::strtoul(lenStr.c_str(), NULL, 10));
                        }
                        break;
                    }
                }
            }

            while (body.size() < contentLength && request.size() < 2 * 1024 * 1024) {
                bytesRead = ARCH->readSocket(socket, requestBuf, sizeof(requestBuf));
                if (bytesRead == 0) {
                    break;
                }
                body.append(requestBuf, bytesRead);
            }

            if (contentLength < body.size()) {
                body.resize(contentLength);
            }

            // remove query string if present
            size_t queryPos = path.find('?');
            if (queryPos != std::string::npos) {
                path = path.substr(0, queryPos);
            }

            const std::string switchPrefix = "/set/screen/";
            const bool isConfigRequest = (path == "/get/config" || path == "/config");
            const bool isSetConfigRequest = (path == "/set/config" && method == "POST");
            bool isSwitchRequest = (path.compare(0, switchPrefix.size(), switchPrefix) == 0);
            std::string responseBody;
            std::string contentType;

            if (isSwitchRequest) {
                std::string requestedScreen = path.substr(switchPrefix.size());
                bool found = false;

                if (!requestedScreen.empty()) {
                    std::string canonical = m_config->getCanonicalName(requestedScreen);
                    if (!canonical.empty()) {
                        requestedScreen = canonical;
                    }

                    found = switchToScreenName(requestedScreen);
                }

                responseBody = found ? "ok" : "false";
                contentType = "text/plain";
            }
            else if (isSetConfigRequest) {
                bool ok = false;
                try {
                    Config validatedConfig(m_events);
                    std::istringstream testStream(body);
                    testStream >> validatedConfig;

                    if (matchesScreenOrHostName(validatedConfig, m_primaryClient->getName())) {
                        std::string configPath = m_args.m_configFile.empty() ? "http_set_config.conf" : m_args.m_configFile;
                        std::ofstream configOut(configPath.c_str(), std::ios::binary | std::ios::trunc);
                        if (configOut.is_open()) {
                            configOut.write(body.data(), static_cast<std::streamsize>(body.size()));
                            configOut.close();

                            if (!configOut.fail()) {
                                std::istringstream applyStream(body);
                                applyStream >> *m_config;
                                ok = setConfig(*m_config);
                            }
                        }
                    }
                }
                catch (...) {
                    ok = false;
                }

                responseBody = ok ? "ok" : "false";
                contentType = "text/plain";
            }
            else if (isConfigRequest) {
                std::ostringstream out;
                out << *m_config;
                responseBody = out.str();
                contentType = "text/plain";
            }
            else {
                std::string current;
                std::string currentIp;

                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    current = m_currentHost;
                    currentIp = m_current_ip;
                }

                responseBody = "{\"server\": {\"current\":\"" + current + "\", \"ip\":\"" + currentIp + "\"}}";
                contentType = "application/json";
            }

            std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: " + contentType + "\r\n"
                "Content-Length: " + std::to_string(responseBody.size()) + "\r\n"
                "Connection: close\r\n\r\n" + responseBody;

            ARCH->writeSocket(socket,
                              (const UInt8*)response.data(),
                              response.size());
        }
        catch (...) {
            // Ignore errors
        }

        ARCH->closeSocket(socket);
    }
}
