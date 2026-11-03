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

#include "client/Client.h"

#include "client/IInputBackend.h"
#include "client/ServerProxy.h"
#include "client/InputBackendFactory.h"
#include "client/RemoteControlApi.h"
#include "barrier/ILocalInputCapture.h"
#include "barrier/Screen.h"
#include "barrier/FileChunk.h"
#include "barrier/DropHelper.h"
#include "barrier/PacketStreamFilter.h"
#include "barrier/ProtocolUtil.h"
#include "barrier/protocol_types.h"
#include "barrier/XBarrier.h"
#include "barrier/StreamChunker.h"
#include "barrier/IPlatformScreen.h"
#include "mt/Thread.h"
#include "net/TCPSocket.h"
#include "net/IDataSocket.h"
#include "net/ISocketFactory.h"
#include "net/SecureSocket.h"
#include "arch/Arch.h"
#include "base/Log.h"
#include "base/IEventQueue.h"
#include "base/TMethodEventJob.h"

#include <cstring>
#include <cstdlib>
#include <sstream>
#include <stdexcept>
#include <fstream>
#include <future>
#include <chrono>

//
// Client
//

Client::Client(IEventQueue* events, const std::string& name, const NetworkAddress& address,
               ISocketFactory* socketFactory,
               barrier::Screen* screen,
               ClientArgs const& args) :
    m_mock(false),
    m_name(name),
    m_serverAddress(address),
    m_socketFactory(socketFactory),
    m_screen(screen),
    m_stream(NULL),
    m_timer(NULL),
    m_server(NULL),
    m_ready(false),
    m_active(false),
    m_suspended(false),
    m_connectOnResume(false),
    m_events(events),
    m_sendFileThread(NULL),
    m_writeToDropDirThread(NULL),
    m_socket(NULL),
    m_useSecureNetwork(args.m_enableCrypto),
    m_args(args),
    m_enableClipboard(true),
    m_inputBackend(createInputBackend(screen, args)),
    m_serverMinorVersion(0),
    m_remoteControlWakeType(Event::kUnknown),
    m_remoteControlTimer(NULL),
    m_loggedIgnoredInput(false)
{
    assert(m_socketFactory != NULL);
    assert(m_screen        != NULL);

    // remote control: forward this computer's keyboard and mouse
    barrier::remote::Hotkey releaseHotkey = barrier::remote::Hotkey::defaultRelease();
    if (!m_args.m_remoteControlReleaseHotkey.empty()) {
        std::string error;
        if (!barrier::remote::Hotkey::parse(m_args.m_remoteControlReleaseHotkey,
                                            releaseHotkey, &error)) {
            LOG((CLOG_WARN "invalid --remote-control-release-hotkey (%s), using %s",
                 error.c_str(), barrier::remote::Hotkey::defaultRelease().toString().c_str()));
            releaseHotkey = barrier::remote::Hotkey::defaultRelease();
        }
    }
    m_remoteControl.reset(new RemoteControlController(this, releaseHotkey));
    m_events->registerTypeOnce(m_remoteControlWakeType, "Client::remoteControlWake");
    m_events->adoptHandler(m_remoteControlWakeType, m_remoteControl.get(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleRemoteControlWake));
    m_remoteControl->setWakeup([this]() { postRemoteControlWake(); });
    m_remoteControlTimer = m_events->newTimer(1.0, m_remoteControl.get());
    m_events->adoptHandler(Event::kTimer, m_remoteControl.get(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleRemoteControlTimer));

    // register suspend/resume event handlers
    m_events->adoptHandler(m_events->forIScreen().suspend(),
                            getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleSuspend));
    m_events->adoptHandler(m_events->forIScreen().resume(),
                            getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleResume));

    if (m_args.m_enableDragDrop) {
        m_events->adoptHandler(m_events->forFile().fileChunkSending(),
                                this,
                                new TMethodEventJob<Client>(this,
                                    &Client::handleFileChunkSending));
        m_events->adoptHandler(m_events->forFile().fileRecieveCompleted(),
                                this,
                                new TMethodEventJob<Client>(this,
                                    &Client::handleFileRecieveCompleted));
    }

}

Client::~Client()
{
    if (m_mock) {
        return;
    }

    m_events->removeHandler(m_events->forIScreen().suspend(),
                              getEventTarget());
    m_events->removeHandler(m_events->forIScreen().resume(),
                              getEventTarget());

    // give the input back before anything else goes away
    if (m_remoteControlApi) {
        m_remoteControlApi->stop();
    }
    m_remoteControl->stop("shutdown", "client is shutting down: keyboard and mouse released");
    m_remoteControl->setCapture(std::unique_ptr<ILocalInputCapture>());
    m_events->removeHandler(Event::kTimer, m_remoteControl.get());
    m_events->deleteTimer(m_remoteControlTimer);
    m_events->removeHandler(m_remoteControlWakeType, m_remoteControl.get());

    cleanupTimer();
    cleanupScreen();
    cleanupConnecting();
    cleanupConnection();
    delete m_socketFactory;
}

void
Client::connect()
{
    if (m_stream != NULL) {
        return;
    }
    if (m_suspended) {
        m_connectOnResume = true;
        return;
    }

    auto security_level = ConnectionSecurityLevel::PLAINTEXT;
    if (m_useSecureNetwork) {
        // client always authenticates server
        security_level = ConnectionSecurityLevel::ENCRYPTED_AUTHENTICATED;
    }

    try {
        // resolve the server hostname.  do this every time we connect
        // in case we couldn't resolve the address earlier or the address
        // has changed (which can happen frequently if this is a laptop
        // being shuttled between various networks).  patch by Brent
        // Priddy.
        m_serverAddress.resolve();

        // m_serverAddress will be null if the hostname address is not reolved
        if (m_serverAddress.getAddress() != NULL) {
          // to help users troubleshoot, show server host name (issue: 60)
          LOG((CLOG_NOTE "connecting to '%s': %s:%i",
          m_serverAddress.getHostname().c_str(),
          ARCH->addrToString(m_serverAddress.getAddress()).c_str(),
          m_serverAddress.getPort()));
        }

        // create the socket
        IDataSocket* socket = m_socketFactory->create(ARCH->getAddrFamily(m_serverAddress.getAddress()),
                                                      security_level);
        m_socket = dynamic_cast<TCPSocket*>(socket);

        // filter socket messages, including a packetizing filter
        m_stream = socket;
        m_stream = new PacketStreamFilter(m_events, m_stream, true);

        // connect
        LOG((CLOG_DEBUG1 "connecting to server"));
        setupConnecting();
        setupTimer();
        socket->connect(m_serverAddress);
    }
    catch (XBase& e) {
        cleanupTimer();
        cleanupConnecting();
        cleanupStream();
        LOG((CLOG_DEBUG1 "connection failed"));
        sendConnectionFailedEvent(e.what());
        return;
    }
}

void
Client::disconnect(const char* msg)
{
    m_connectOnResume = false;
    cleanupTimer();
    cleanupScreen();
    cleanupConnecting();
    cleanupConnection();
    if (msg != NULL) {
        sendConnectionFailedEvent(msg);
    }
    else {
        sendEvent(m_events->forClient().disconnected(), NULL);
    }
}

void
Client::handshakeComplete()
{
    m_ready = true;
    m_screen->enable();
    m_remoteControl->onConnected();
    sendEvent(m_events->forClient().connected(), NULL);
}

bool
Client::isConnected() const
{
    return (m_server != NULL);
}

bool
Client::isConnecting() const
{
    return (m_timer != NULL);
}

NetworkAddress
Client::getServerAddress() const
{
    return m_serverAddress;
}

void*
Client::getEventTarget() const
{
    return m_screen->getEventTarget();
}

bool
Client::getClipboard(ClipboardID id, IClipboard* clipboard) const
{
    return m_screen->getClipboard(id, clipboard);
}

void
Client::getShape(SInt32& x, SInt32& y, SInt32& w, SInt32& h) const
{
    m_screen->getShape(x, y, w, h);
}

void
Client::getScreens(std::vector<ClientScreenInfo>& screens) const
{
    m_screen->getScreens(screens);
}

void
Client::getCursorPos(SInt32& x, SInt32& y) const
{
    if (!m_inputBackend->getCursorPos(x, y)) {
        m_screen->getCursorPos(x, y);
    }
}

void
Client::enter(SInt32 xAbs, SInt32 yAbs, UInt32, KeyModifierMask mask, bool)
{
    LOG((CLOG_INFO "client enter at %d,%d backendManagesCursor=%s moveAfterEnter=%s",
        xAbs, yAbs,
        m_inputBackend->managesCursorVisibility() ? "yes" : "no",
        m_inputBackend->movesCursorAfterScreenEnter() ? "yes" : "no"));

    m_active = true;
    if (m_inputBackend->movesCursorAfterScreenEnter()) {
        m_screen->enter(mask);
        m_inputBackend->enter(xAbs, yAbs);
    }
    else if (m_inputBackend->managesCursorVisibility()) {
        m_screen->enter(mask);
        m_inputBackend->enter(xAbs, yAbs);
    }
    else {
        m_inputBackend->enter(xAbs, yAbs);
        m_screen->enter(mask);
    }

    if (m_sendFileThread != NULL) {
        StreamChunker::interruptFile();
        m_sendFileThread = NULL;
    }
}

bool
Client::leave()
{
    m_active = false;

    if (m_inputBackend->managesCursorVisibility()) {
        m_screen->leave();
        m_inputBackend->leave();
    }
    else {
        m_inputBackend->leave();
        m_screen->leave();
    }

    if (m_enableClipboard) {
        // send clipboards that we own and that have changed
        for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
            if (m_ownClipboard[id]) {
                sendClipboard(id);
            }
        }
    }

    return true;
}

void
Client::setClipboard(ClipboardID id, const IClipboard* clipboard)
{
     m_screen->setClipboard(id, clipboard);
    m_ownClipboard[id]  = false;
    m_sentClipboard[id] = false;
}

void
Client::grabClipboard(ClipboardID id)
{
    m_screen->grabClipboard(id);
    m_ownClipboard[id]  = false;
    m_sentClipboard[id] = false;
}

void
Client::setClipboardDirty(ClipboardID, bool)
{
    assert(0 && "shouldn't be called");
}

void
Client::keyDown(KeyID id, KeyModifierMask mask, KeyButton button)
{
    if (m_remoteControl->isForwarding()) {
        // this computer is forwarding its own input; never apply input
        // coming back from the server (protects against loops)
        if (!m_loggedIgnoredInput) {
            LOG((CLOG_DEBUG "ignoring server input while controlling remotely"));
            m_loggedIgnoredInput = true;
        }
        return;
    }
    m_inputBackend->keyDown(id, mask, button);
}

void
Client::keyRepeat(KeyID id, KeyModifierMask mask,
                SInt32 count, KeyButton button)
{
    if (m_remoteControl->isForwarding()) {
        // this computer is forwarding its own input; never apply input
        // coming back from the server (protects against loops)
        if (!m_loggedIgnoredInput) {
            LOG((CLOG_DEBUG "ignoring server input while controlling remotely"));
            m_loggedIgnoredInput = true;
        }
        return;
    }
    m_inputBackend->keyRepeat(id, mask, count, button);
}

void
Client::keyUp(KeyID id, KeyModifierMask mask, KeyButton button)
{
    if (m_remoteControl->isForwarding()) {
        // this computer is forwarding its own input; never apply input
        // coming back from the server (protects against loops)
        if (!m_loggedIgnoredInput) {
            LOG((CLOG_DEBUG "ignoring server input while controlling remotely"));
            m_loggedIgnoredInput = true;
        }
        return;
    }
    m_inputBackend->keyUp(id, mask, button);
}

void
Client::mouseDown(ButtonID id)
{
    if (m_remoteControl->isForwarding()) {
        // this computer is forwarding its own input; never apply input
        // coming back from the server (protects against loops)
        if (!m_loggedIgnoredInput) {
            LOG((CLOG_DEBUG "ignoring server input while controlling remotely"));
            m_loggedIgnoredInput = true;
        }
        return;
    }
    m_inputBackend->mouseDown(id);
}

void
Client::mouseUp(ButtonID id)
{
    if (m_remoteControl->isForwarding()) {
        // this computer is forwarding its own input; never apply input
        // coming back from the server (protects against loops)
        if (!m_loggedIgnoredInput) {
            LOG((CLOG_DEBUG "ignoring server input while controlling remotely"));
            m_loggedIgnoredInput = true;
        }
        return;
    }
    m_inputBackend->mouseUp(id);
}

void
Client::mouseMove(SInt32 x, SInt32 y)
{
    if (m_remoteControl->isForwarding()) {
        // this computer is forwarding its own input; never apply input
        // coming back from the server (protects against loops)
        if (!m_loggedIgnoredInput) {
            LOG((CLOG_DEBUG "ignoring server input while controlling remotely"));
            m_loggedIgnoredInput = true;
        }
        return;
    }
    m_inputBackend->mouseMove(x, y);
}

void
Client::mouseRelativeMove(SInt32 dx, SInt32 dy)
{
    if (m_remoteControl->isForwarding()) {
        // this computer is forwarding its own input; never apply input
        // coming back from the server (protects against loops)
        if (!m_loggedIgnoredInput) {
            LOG((CLOG_DEBUG "ignoring server input while controlling remotely"));
            m_loggedIgnoredInput = true;
        }
        return;
    }
    m_inputBackend->mouseRelativeMove(dx, dy);
}

void
Client::mouseWheel(SInt32 xDelta, SInt32 yDelta)
{
    if (m_remoteControl->isForwarding()) {
        // this computer is forwarding its own input; never apply input
        // coming back from the server (protects against loops)
        if (!m_loggedIgnoredInput) {
            LOG((CLOG_DEBUG "ignoring server input while controlling remotely"));
            m_loggedIgnoredInput = true;
        }
        return;
    }
    m_inputBackend->mouseWheel(xDelta, yDelta);
}

void
Client::screensaver(bool activate)
{
     m_screen->screensaver(activate);
}

void
Client::resetOptions()
{
    m_screen->resetOptions();
}

void
Client::setOptions(const OptionsList& options)
{
    for (OptionsList::const_iterator index = options.begin();
         index != options.end(); ++index) {
        const OptionID id       = *index;
        if (id == kOptionClipboardSharing) {
            index++;
            if (*index == static_cast<OptionValue>(false)) {
                LOG((CLOG_NOTE "clipboard sharing is disabled"));
            }
            m_enableClipboard = *index;

            break;
        }
    }

    m_screen->setOptions(options);
}

std::string
Client::getName() const
{
    return m_name;
}

void
Client::sendClipboard(ClipboardID id)
{
    // note -- m_mutex must be locked on entry
    assert(m_screen != NULL);
    assert(m_server != NULL);

    // get clipboard data.  set the clipboard time to the last
    // clipboard time before getting the data from the screen
    // as the screen may detect an unchanged clipboard and
    // avoid copying the data.
    Clipboard clipboard;
    if (clipboard.open(m_timeClipboard[id])) {
        clipboard.close();
    }
    m_screen->getClipboard(id, &clipboard);

    // check time
    if (m_timeClipboard[id] == 0 ||
        clipboard.getTime() != m_timeClipboard[id]) {
        // save new time
        m_timeClipboard[id] = clipboard.getTime();

        // marshall the data
        std::string data = clipboard.marshall();

        // save and send data if different or not yet sent
        if (!m_sentClipboard[id] || data != m_dataClipboard[id]) {
            m_sentClipboard[id] = true;
            m_dataClipboard[id] = data;
            m_server->onClipboardChanged(id, &clipboard);
        }
    }
}

void
Client::sendEvent(Event::Type type, void* data)
{
    m_events->addEvent(Event(type, getEventTarget(), data));
}

void
Client::sendConnectionFailedEvent(const char* msg)
{
    FailInfo* info = new FailInfo(msg);
    info->m_retry = true;
    Event event(m_events->forClient().connectionFailed(), getEventTarget(), info, Event::kDontFreeData);
    m_events->addEvent(event);
}

void
Client::sendFileChunk(const void* data)
{
    FileChunk* chunk = static_cast<FileChunk*>(const_cast<void*>(data));
    LOG((CLOG_DEBUG1 "send file chunk"));
    assert(m_server != NULL);

    // relay
    m_server->fileChunkSending(chunk->m_chunk[0], &chunk->m_chunk[1], chunk->m_dataSize);
}

void
Client::setupConnecting()
{
    assert(m_stream != NULL);

    if (m_args.m_enableCrypto) {
        m_events->adoptHandler(m_events->forIDataSocket().secureConnected(),
                    m_stream->getEventTarget(),
                        new TMethodEventJob<Client>(this,
                                &Client::handleConnected));
    }
    else {
        m_events->adoptHandler(m_events->forIDataSocket().connected(),
                    m_stream->getEventTarget(),
                        new TMethodEventJob<Client>(this,
                                &Client::handleConnected));
    }

    m_events->adoptHandler(m_events->forIDataSocket().connectionFailed(),
                            m_stream->getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleConnectionFailed));
}

void
Client::setupConnection()
{
    assert(m_stream != NULL);

    m_events->adoptHandler(m_events->forISocket().disconnected(),
                            m_stream->getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleDisconnected));
    m_events->adoptHandler(m_events->forIStream().inputReady(),
                            m_stream->getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleHello));
    m_events->adoptHandler(m_events->forIStream().outputError(),
                            m_stream->getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleOutputError));
    m_events->adoptHandler(m_events->forIStream().inputShutdown(),
                            m_stream->getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleDisconnected));
    m_events->adoptHandler(m_events->forIStream().outputShutdown(),
                            m_stream->getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleDisconnected));

    m_events->adoptHandler(m_events->forISocket().stopRetry(),
                           m_stream->getEventTarget(),
                           new TMethodEventJob<Client>(this, &Client::handleStopRetry));
}

void
Client::setupScreen()
{
    assert(m_server == NULL);

    m_ready  = false;
    m_server = new ServerProxy(this, m_stream, m_events);
    m_events->adoptHandler(m_events->forIScreen().shapeChanged(),
                            getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleShapeChanged));
    m_events->adoptHandler(m_events->forClipboard().clipboardGrabbed(),
                            getEventTarget(),
                            new TMethodEventJob<Client>(this,
                                &Client::handleClipboardGrabbed));
}

void
Client::setupTimer()
{
    assert(m_timer == NULL);

    m_timer = m_events->newOneShotTimer(15.0, NULL);
    m_events->adoptHandler(Event::kTimer, m_timer,
                            new TMethodEventJob<Client>(this,
                                &Client::handleConnectTimeout));
}

void
Client::cleanupConnecting()
{
    if (m_stream != NULL) {
        m_events->removeHandler(m_events->forIDataSocket().connected(),
                            m_stream->getEventTarget());
        m_events->removeHandler(m_events->forIDataSocket().connectionFailed(),
                            m_stream->getEventTarget());
    }
}

void
Client::cleanupConnection()
{
    if (m_stream != NULL) {
        m_events->removeHandler(m_events->forIStream().inputReady(),
                            m_stream->getEventTarget());
        m_events->removeHandler(m_events->forIStream().outputError(),
                            m_stream->getEventTarget());
        m_events->removeHandler(m_events->forIStream().inputShutdown(),
                            m_stream->getEventTarget());
        m_events->removeHandler(m_events->forIStream().outputShutdown(),
                            m_stream->getEventTarget());
        m_events->removeHandler(m_events->forISocket().disconnected(),
                            m_stream->getEventTarget());
        m_events->removeHandler(m_events->forISocket().stopRetry(),
                                m_stream->getEventTarget());
        cleanupStream();
    }
}

void
Client::cleanupScreen()
{
    if (m_server != NULL) {
        // connection is going away: stop forwarding immediately.  the
        // controller must see the connection as gone (m_ready false) so
        // it does not try to send over it.
        const bool wasReady = m_ready;
        m_ready = false;
        if (m_remoteControl) {
            m_remoteControl->onDisconnected();
        }
        m_ready = wasReady;
        if (m_ready) {
            m_screen->disable();
            m_ready = false;
        }
        m_events->removeHandler(m_events->forIScreen().shapeChanged(),
                            getEventTarget());
        m_events->removeHandler(m_events->forClipboard().clipboardGrabbed(),
                            getEventTarget());
        delete m_server;
        m_server = NULL;
    }
}

void
Client::cleanupTimer()
{
    if (m_timer != NULL) {
        m_events->removeHandler(Event::kTimer, m_timer);
        m_events->deleteTimer(m_timer);
        m_timer = NULL;
    }
}

void
Client::cleanupStream()
{
    delete m_stream;
    m_stream = NULL;
}

void
Client::handleConnected(const Event&, void*)
{
    LOG((CLOG_DEBUG1 "connected;  wait for hello"));
    cleanupConnecting();
    setupConnection();

    // reset clipboard state
    for (ClipboardID id = 0; id < kClipboardEnd; ++id) {
        m_ownClipboard[id]  = false;
        m_sentClipboard[id] = false;
        m_timeClipboard[id] = 0;
    }
}

void
Client::handleConnectionFailed(const Event& event, void*)
{
    IDataSocket::ConnectionFailedInfo* info =
        static_cast<IDataSocket::ConnectionFailedInfo*>(event.getData());

    cleanupTimer();
    cleanupConnecting();
    cleanupStream();
    LOG((CLOG_DEBUG1 "connection failed"));
    sendConnectionFailedEvent(info->m_what.c_str());
    delete info;
}

void
Client::handleConnectTimeout(const Event&, void*)
{
    cleanupTimer();
    cleanupConnecting();
    cleanupConnection();
    cleanupStream();
    LOG((CLOG_DEBUG1 "connection timed out"));
    sendConnectionFailedEvent("Timed out");
}

void
Client::handleOutputError(const Event&, void*)
{
    cleanupTimer();
    cleanupScreen();
    cleanupConnection();
    LOG((CLOG_WARN "error sending to server"));
    sendEvent(m_events->forClient().disconnected(), NULL);
}

void
Client::handleDisconnected(const Event&, void*)
{
    cleanupTimer();
    cleanupScreen();
    cleanupConnection();
    LOG((CLOG_DEBUG1 "disconnected"));
    sendEvent(m_events->forClient().disconnected(), NULL);
}

void
Client::handleShapeChanged(const Event&, void*)
{
    LOG((CLOG_DEBUG "resolution changed"));
    m_server->onInfoChanged();
}

void
Client::handleClipboardGrabbed(const Event& event, void*)
{
    if (!m_enableClipboard) {
        return;
    }

    const IScreen::ClipboardInfo* info =
        static_cast<const IScreen::ClipboardInfo*>(event.getData());

    // grab ownership
    m_server->onGrabClipboard(info->m_id);

    // we now own the clipboard and it has not been sent to the server
    m_ownClipboard[info->m_id]  = true;
    m_sentClipboard[info->m_id] = false;
    m_timeClipboard[info->m_id] = 0;

    // if we're not the active screen then send the clipboard now,
    // otherwise we'll wait until we leave.
    if (!m_active) {
        sendClipboard(info->m_id);
    }
}

void
Client::handleHello(const Event&, void*)
{
    SInt16 major, minor;
    if (!ProtocolUtil::readf(m_stream, kMsgHello, &major, &minor)) {
        sendConnectionFailedEvent("Protocol error from server, check encryption settings");
        cleanupTimer();
        cleanupConnection();
        return;
    }

    // check versions
    LOG((CLOG_DEBUG1 "got hello version %d.%d", major, minor));
    if (major < kProtocolMajorVersion ||
        (major == kProtocolMajorVersion && minor < kProtocolMinimumServerMinorVersion)) {
        sendConnectionFailedEvent(XIncompatibleClient(major, minor).what());
        cleanupTimer();
        cleanupConnection();
        return;
    }

    // say hello back with the highest version both sides speak, so a
    // newer client keeps working with an older server (the server picks
    // its client proxy from this number).
    SInt16 replyMinor = kProtocolMinorVersion;
    if (major == kProtocolMajorVersion && minor < kProtocolMinorVersion) {
        replyMinor = minor;
    }
    m_serverMinorVersion = replyMinor;
    LOG((CLOG_DEBUG1 "say hello version %d.%d", kProtocolMajorVersion, replyMinor));
    ProtocolUtil::writef(m_stream, kMsgHelloBack,
                            kProtocolMajorVersion,
                            replyMinor, &m_name);

    // now connected but waiting to complete handshake
    setupScreen();
    cleanupTimer();

    // make sure we process any remaining messages later.  we won't
    // receive another event for already pending messages so we fake
    // one.
    if (m_stream->isReady()) {
        m_events->addEvent(Event(m_events->forIStream().inputReady(),
                            m_stream->getEventTarget()));
    }
}

void
Client::handleSuspend(const Event&, void*)
{
    m_remoteControl->onSuspend();
    LOG((CLOG_INFO "suspend"));
    m_suspended       = true;
    bool wasConnected = isConnected();
    disconnect(NULL);
    m_connectOnResume = wasConnected;
}

void
Client::handleResume(const Event&, void*)
{
    LOG((CLOG_INFO "resume"));
    m_suspended = false;
    if (m_connectOnResume) {
        m_connectOnResume = false;
        connect();
    }
}

void
Client::handleFileChunkSending(const Event& event, void*)
{
    sendFileChunk(event.getData());
}

void
Client::handleFileRecieveCompleted(const Event& event, void*)
{
    onFileRecieveCompleted();
}

void
Client::onFileRecieveCompleted()
{
    if (isReceivedFileSizeValid()) {
        m_writeToDropDirThread = new Thread([this](){ write_to_drop_dir_thread(); });
    }
}

void
Client::handleStopRetry(const Event&, void*)
{
    m_args.m_restartable = false;
}

void Client::write_to_drop_dir_thread()
{
    LOG((CLOG_DEBUG "starting write to drop dir thread"));

    while (m_screen->isFakeDraggingStarted()) {
        ARCH->sleep(.1f);
    }

    DropHelper::writeToDir(m_screen->getDropTarget(), m_dragFileList,
                    m_receivedFileData);
}

void
Client::dragInfoReceived(UInt32 fileNum, std::string data)
{
    // TODO: fix duplicate function from CServer
    if (!m_args.m_enableDragDrop) {
        LOG((CLOG_DEBUG "drag drop not enabled, ignoring drag info."));
        return;
    }

    DragInformation::parseDragInfo(m_dragFileList, fileNum, data);

    m_screen->startDraggingFiles(m_dragFileList);
}

bool
Client::isReceivedFileSizeValid()
{
    return m_expectedFileSize == m_receivedFileData.size();
}

void
Client::sendFileToServer(const char* filename)
{
    if (m_sendFileThread != NULL) {
        StreamChunker::interruptFile();
    }

    m_sendFileThread = new Thread([this, filename]() { send_file_thread(filename); });
}

void Client::send_file_thread(const char* filename)
{
    try {
        StreamChunker::sendFile(filename, m_events, this);
    }
    catch (std::runtime_error& error) {
        LOG((CLOG_ERR "failed sending file chunks: %s", error.what()));
    }

    m_sendFileThread = NULL;
}

void
Client::sendDragInfo(UInt32 fileCount, std::string& info, size_t size)
{
    m_server->sendDragInfo(fileCount, info.c_str(), size);
}

//
// remote control
//

void
Client::setLocalInputCapture(std::unique_ptr<ILocalInputCapture> capture)
{
    if (capture) {
        std::string why;
        if (capture->isAvailable(why)) {
            LOG((CLOG_INFO "remote control: local input capture backend \"%s\" ready",
                 capture->name().c_str()));
        }
        else {
            LOG((CLOG_INFO "remote control: capture backend \"%s\" unavailable: %s",
                 capture->name().c_str(), why.c_str()));
        }
    }
    m_remoteControl->setCapture(std::move(capture));

    if (!m_remoteControlApi && m_args.m_remoteControlApiPort > 0) {
        m_remoteControlApi.reset(new RemoteControlApi(m_args.m_remoteControlApiPort,
            [this](const std::string& method, const std::string& path,
                   const std::map<std::string, std::string>& query) {
                return callRemoteControlApiOnMainThread(method, path, query);
            }));
        std::string error;
        if (!m_remoteControlApi->start(error)) {
            LOG((CLOG_WARN "remote control API not available on port %d: %s",
                 m_args.m_remoteControlApiPort, error.c_str()));
            m_remoteControlApi.reset();
        }
    }
}

void
Client::postRemoteControlWake()
{
    // any thread
    m_events->addEvent(Event(m_remoteControlWakeType, m_remoteControl.get()));
}

std::string
Client::callRemoteControlApiOnMainThread(const std::string& method, const std::string& path,
                                         const std::map<std::string, std::string>& query)
{
    // API thread: hand the command to the main thread and wait
    std::shared_ptr<std::promise<std::string> > promise(new std::promise<std::string>());
    std::future<std::string> future = promise->get_future();
    {
        std::lock_guard<std::mutex> lock(m_mainTasksMutex);
        m_mainTasks.push_back([this, promise, method, path, query]() {
            promise->set_value(executeRemoteControlCommand(method, path, query));
        });
    }
    postRemoteControlWake();
    if (future.wait_for(std::chrono::seconds(3)) != std::future_status::ready) {
        return "{\"ok\":false,\"error\":\"client busy\"}";
    }
    return future.get();
}

void
Client::handleRemoteControlWake(const Event&, void*)
{
    std::vector<std::function<void()> > tasks;
    {
        std::lock_guard<std::mutex> lock(m_mainTasksMutex);
        tasks.swap(m_mainTasks);
    }
    for (size_t i = 0; i < tasks.size(); ++i) {
        tasks[i]();
    }
    m_remoteControl->processPending();
    if (!m_remoteControl->isForwarding()) {
        m_loggedIgnoredInput = false;
    }
}

void
Client::handleRemoteControlTimer(const Event&, void*)
{
    m_remoteControl->tick();
}

std::string
Client::executeRemoteControlCommand(const std::string& method, const std::string& path,
                                    const std::map<std::string, std::string>& query)
{
    std::string error;
    bool ok = true;
    std::map<std::string, std::string>::const_iterator i;
    if (path == "/remote-control" || path == "/remote-control/status") {
        // status only
    }
    else if (method != "POST") {
        return "{\"ok\":false,\"error\":\"use POST\"}";
    }
    else if (path == "/remote-control/start") {
        ILocalInputCapture::Mode mode = ILocalInputCapture::kModeCapture;
        i = query.find("mode");
        if (i != query.end() && i->second == "window") {
            mode = ILocalInputCapture::kModeWindow;
        }
        std::string target;
        i = query.find("target");
        if (i != query.end()) {
            target = i->second;
        }
        i = query.find("takeover");
        const bool takeover = (i != query.end() && (i->second == "1" || i->second == "true"));
        ok = m_remoteControl->start(mode, target, takeover, error);
    }
    else if (path == "/remote-control/stop") {
        m_remoteControl->stop("user", "released by the user: keyboard and mouse returned to this computer");
    }
    else if (path == "/remote-control/ctrl-alt-del") {
        ok = m_remoteControl->sendCtrlAltDel(error);
    }
    else {
        return "{\"ok\":false,\"error\":\"unknown command\"}";
    }

    std::string out = std::string("{\"ok\":") + (ok ? "true" : "false");
    if (!ok) {
        std::string escaped;
        for (size_t k = 0; k < error.size(); ++k) {
            if (error[k] == '"' || error[k] == '\\') escaped += '\\';
            escaped += error[k];
        }
        out += ",\"error\":\"" + escaped + "\"";
    }
    out += ",\"status\":" +
           RemoteControlController::statusToJson(m_remoteControl->status()) + "}";
    return out;
}

void
Client::onRemoteControlState(int state, int reason, UInt32 epoch,
                             SInt32 x, SInt32 y, SInt32 w, SInt32 h,
                             UInt8 capabilities, const std::string& controller,
                             const std::string& target)
{
    m_remoteControl->onServerState(state, reason, epoch, x, y, w, h, capabilities,
                                   controller, target);
}

bool
Client::isConnectedToServer() const
{
    return m_server != NULL && m_ready;
}

bool
Client::isRemoteControlSupported() const
{
    return isConnectedToServer() &&
           m_serverMinorVersion >= kProtocolRemoteControlMinorVersion;
}

void
Client::sendRemoteControlRequest(bool takeover, const std::string& target)
{
    if (isRemoteControlSupported()) {
        m_server->requestRemoteControl(takeover, target);
    }
}

void
Client::sendRemoteControlRelease(UInt32 epoch)
{
    if (isRemoteControlSupported()) {
        m_server->releaseRemoteControl(epoch);
    }
}

void
Client::sendRemoteInput(UInt32 epoch, const barrier::remote::Event& event)
{
    if (isRemoteControlSupported()) {
        m_server->sendRemoteInput(epoch, event);
    }
}

std::string
Client::serverName() const
{
    return m_serverAddress.getHostname();
}
