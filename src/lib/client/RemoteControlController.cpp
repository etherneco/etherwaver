/*
 * Etherwaver -- remote control of a server from a client's local input.
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "client/RemoteControlController.h"

#include "base/Log.h"

#include <sstream>

using namespace barrier::remote;
typedef barrier::remote::Event RemoteEvent;

namespace {

// synthetic key buttons for the explicit ctrl+alt+del action; they lie
// above every real keycode (X11/evdev use 8..255, Windows scancodes
// <= 0x1ff are masked by the receiver) so they never clash.
const KeyButton kCadControlButton = 0x1F0;
const KeyButton kCadAltButton     = 0x1F1;
const KeyButton kCadDeleteButton  = 0x1F2;

std::string
jsonString(const std::string& value)
{
    std::string out = "\"";
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        }
        else if (c == '\n') {
            out += "\\n";
        }
        else if (static_cast<unsigned char>(c) < 0x20) {
            out += ' ';
        }
        else {
            out += c;
        }
    }
    return out + "\"";
}

} // namespace

RemoteControlController::RemoteControlController(IRemoteControlTransport* transport,
                                                 const Hotkey& releaseHotkey) :
    m_transport(transport),
    m_releaseHotkey(releaseHotkey.isValid() ? releaseHotkey : Hotkey::defaultRelease()),
    m_state(kLocal),
    m_mode(ILocalInputCapture::kModeCapture),
    m_epoch(0),
    m_targetX(0), m_targetY(0), m_targetW(0), m_targetH(0),
    m_capabilities(0),
    m_viewOnly(false),
    m_inbox(1024),
    m_inboxOverflow(false),
    m_releaseHotkeyPressed(false),
    m_wakePending(false),
    m_forwarded(0)
{
}

RemoteControlController::~RemoteControlController()
{
    if (m_capture) {
        m_capture->stop();
    }
}

void
RemoteControlController::setCapture(std::unique_ptr<ILocalInputCapture> capture)
{
    if (m_capture) {
        m_capture->stop();
    }
    m_capture = std::move(capture);
    notify();
}

const char*
RemoteControlController::stateName(State state)
{
    switch (state) {
    case kLocal:      return "local";
    case kRequesting: return "requesting";
    case kRemote:     return "remote";
    case kSuspended:  return "suspended";
    }
    return "local";
}

void
RemoteControlController::setState(State state)
{
    if (m_state != state) {
        LOG((CLOG_NOTE "remote control: %s -> %s", stateName(m_state), stateName(state)));
        m_state = state;
    }
}

void
RemoteControlController::setMessage(const std::string& code, const std::string& message)
{
    m_lastEvent   = code;
    m_lastMessage = message;
    if (!message.empty()) {
        LOG((CLOG_NOTE "remote control: %s", message.c_str()));
    }
}

ILocalInputCapture::Config
RemoteControlController::captureConfig() const
{
    ILocalInputCapture::Config config;
    config.m_releaseHotkey = m_releaseHotkey;
    config.m_targetName    = m_target.empty() ? m_requestedTarget : m_target;
    config.m_targetX       = m_targetX;
    config.m_targetY       = m_targetY;
    config.m_targetW       = m_targetW;
    config.m_targetH       = m_targetH;
    return config;
}

bool
RemoteControlController::start(ILocalInputCapture::Mode mode, const std::string& target,
                               bool takeover, std::string& error)
{
    if (m_state != kLocal) {
        error = "remote control is already active";
        return false;
    }
    if (!m_transport->isConnectedToServer()) {
        error = "not connected to a server";
        setMessage("not-connected", error);
        notify();
        return false;
    }
    if (!m_transport->isRemoteControlSupported()) {
        error = "the server does not support remote control (needs Etherwaver protocol 1.8)";
        setMessage("server-unsupported", error);
        notify();
        return false;
    }
    std::string why;
    if (!m_capture || !m_capture->isAvailable(why)) {
        error = "local input capture is not available: " +
                (m_capture ? why : std::string("not implemented on this platform"));
        setMessage("capture-unavailable", error);
        notify();
        return false;
    }
    if (!m_capture->supportsMode(mode)) {
        error = std::string("the ") + m_capture->name() + " capture backend does not support " +
                (mode == ILocalInputCapture::kModeWindow ? "window mode" : "full capture");
        setMessage("mode-unsupported", error);
        notify();
        return false;
    }

    m_mode            = mode;
    m_requestedTarget = target;
    m_viewOnly        = false;
    m_sentHeld.clear();
    clearInbox();
    setState(kRequesting);
    setMessage("requesting", std::string());
    m_transport->sendRemoteControlRequest(takeover, target);
    notify();
    return true;
}

void
RemoteControlController::clearInbox()
{
    std::lock_guard<std::mutex> lock(m_inboxMutex);
    m_inbox.clear();
    m_inboxOverflow        = false;
    m_releaseHotkeyPressed = false;
    m_captureLost.clear();
    m_focusChanges.clear();
}

void
RemoteControlController::releaseRemoteHeld()
{
    std::vector<RemoteEvent> release = m_sentHeld.releaseAll();
    if (m_epoch == 0 || !m_transport->isConnectedToServer()) {
        return;
    }
    for (size_t i = 0; i < release.size(); ++i) {
        m_transport->sendRemoteInput(m_epoch, release[i]);
    }
}

void
RemoteControlController::endLocally(const std::string& code, const std::string& message,
                                    bool sendRelease)
{
    // 1. give the input back to this computer first
    if (m_capture) {
        m_capture->stop();
    }
    // 2. release what the remote host still has pressed, then the grant
    if (sendRelease && m_transport->isConnectedToServer()) {
        releaseRemoteHeld();
        if (m_epoch != 0) {
            m_transport->sendRemoteControlRelease(m_epoch);
        }
    }
    m_sentHeld.clear();
    clearInbox();
    m_epoch = 0;
    setState(kLocal);
    setMessage(code, message);
    notify();
}

void
RemoteControlController::stop(const std::string& code, const std::string& message)
{
    if (m_state == kLocal) {
        return;
    }
    // when still requesting, a late grant is answered with a release in
    // onServerState()
    endLocally(code, message, true);
}

bool
RemoteControlController::sendCtrlAltDel(std::string& error)
{
    if (m_state != kRemote && m_state != kSuspended) {
        error = "remote control is not active";
        return false;
    }
    const KeyModifierMask none = 0;
    m_transport->sendRemoteInput(m_epoch, RemoteEvent::keyDown(kKeyControl_L, none, kCadControlButton));
    m_transport->sendRemoteInput(m_epoch, RemoteEvent::keyDown(kKeyAlt_L, KeyModifierControl, kCadAltButton));
    m_transport->sendRemoteInput(m_epoch, RemoteEvent::keyDown(kKeyDelete,
        KeyModifierControl | KeyModifierAlt, kCadDeleteButton));
    m_transport->sendRemoteInput(m_epoch, RemoteEvent::keyUp(kKeyDelete,
        KeyModifierControl | KeyModifierAlt, kCadDeleteButton));
    m_transport->sendRemoteInput(m_epoch, RemoteEvent::keyUp(kKeyAlt_L, KeyModifierControl | KeyModifierAlt,
        kCadAltButton));
    m_transport->sendRemoteInput(m_epoch, RemoteEvent::keyUp(kKeyControl_L, KeyModifierControl, kCadControlButton));
    return true;
}

void
RemoteControlController::onConnected()
{
    // reconnecting never resumes remote control on its own
    m_epoch = 0;
    m_viewOnly = false;
    m_controller.clear();
    notify();
}

void
RemoteControlController::onDisconnected()
{
    if (m_state != kLocal) {
        // the server releases its side when it notices the lost
        // connection; locally we stop immediately.
        endLocally("connection-lost",
                   "connection to the server lost: keyboard and mouse returned to this computer",
                   false);
    }
    m_controller.clear();
    m_target.clear();
    notify();
}

void
RemoteControlController::onSuspend()
{
    if (m_state != kLocal) {
        stop("suspend", "this computer is going to sleep or locking: keyboard and mouse released");
    }
}

void
RemoteControlController::onServerState(int state, int reason, UInt32 epoch,
                                       SInt32 x, SInt32 y, SInt32 w, SInt32 h,
                                       UInt8 capabilities, const std::string& controller,
                                       const std::string& target)
{
    m_controller   = controller;
    m_capabilities = capabilities;

    switch (state) {
    case kStateGranted:
        m_target  = target;
        m_targetX = x; m_targetY = y; m_targetW = w; m_targetH = h;
        if (m_state == kRequesting) {
            m_epoch = epoch;
            std::string error;
            if (!m_capture || !m_capture->start(m_mode, captureConfig(), this, error)) {
                endLocally("capture-failed", "could not capture local input: " + error, true);
                return;
            }
            setState(m_mode == ILocalInputCapture::kModeWindow ? kSuspended : kRemote);
            setMessage("granted", "keyboard and mouse now control " + m_target +
                       ". Press " + m_releaseHotkey.toString() + " to return to this computer.");
        }
        else if (m_state == kRemote || m_state == kSuspended) {
            if (epoch != m_epoch || reason == kReasonTargetChanged) {
                // the server released what was held on the previous host
                m_sentHeld.clear();
            }
            m_epoch = epoch;
            if (m_capture) {
                m_capture->updateTarget(captureConfig());
            }
            if (reason == kReasonTargetChanged) {
                setMessage("target-changed", "keyboard and mouse now control " + m_target);
            }
        }
        else if (m_state == kLocal && epoch != 0) {
            // a grant we no longer want (stopped while requesting)
            m_transport->sendRemoteControlRelease(epoch);
        }
        break;

    case kStateDenied:
        m_viewOnly = (reason == kReasonNotPermitted);
        if (m_state != kLocal) {
            endLocally("denied", std::string("remote control refused: ") + controlReasonText(reason),
                       false);
            return;
        }
        break;

    case kStateRevoked:
        if (m_state != kLocal) {
            endLocally("revoked", std::string("remote control ended by the server: ") +
                       controlReasonText(reason), false);
            return;
        }
        break;

    case kStateReleased:
        if (m_state != kLocal) {
            endLocally("released", "remote control released", false);
            return;
        }
        break;

    case kStateControlledByOther:
        if (m_state == kRemote || m_state == kSuspended) {
            endLocally("revoked", "another computer took over control", false);
            return;
        }
        if (reason == kReasonProtocol && m_state == kLocal) {
            // server dropped stale input; nothing to do
        }
        m_target = target;
        break;

    case kStateIdle:
    default:
        if (m_state == kRemote || m_state == kSuspended) {
            endLocally("revoked", "remote control ended by the server", false);
            return;
        }
        m_target.clear();
        break;
    }
    notify();
}

void
RemoteControlController::onCaptureEvent(const RemoteEvent& event)
{
    {
        std::lock_guard<std::mutex> lock(m_inboxMutex);
        if (m_inbox.push(event) == InputQueue::kOverflow) {
            m_inboxOverflow = true;
        }
    }
    wake();
}

void
RemoteControlController::onCaptureReleaseHotkey()
{
    {
        std::lock_guard<std::mutex> lock(m_inboxMutex);
        m_releaseHotkeyPressed = true;
    }
    wake();
}

void
RemoteControlController::onCaptureLost(const std::string& reason)
{
    {
        std::lock_guard<std::mutex> lock(m_inboxMutex);
        m_captureLost = reason.empty() ? std::string("capture interrupted") : reason;
    }
    wake();
}

void
RemoteControlController::onCaptureFocus(bool active)
{
    {
        std::lock_guard<std::mutex> lock(m_inboxMutex);
        m_focusChanges.push_back(active);
    }
    wake();
}

void
RemoteControlController::wake()
{
    if (!m_wakePending.exchange(true) && m_wakeup) {
        m_wakeup();
    }
}

void
RemoteControlController::forward(const RemoteEvent& in)
{
    RemoteEvent event = in;
    switch (event.m_kind) {
    case kEventKeyDown:
        if (!m_sentHeld.pressKey((KeyID)event.m_a, (KeyModifierMask)event.m_b,
                                 (KeyButton)event.m_c)) {
            event = RemoteEvent::keyRepeat((KeyID)event.m_a, (KeyModifierMask)event.m_b,
                                           (KeyButton)event.m_c, 1);
        }
        break;
    case kEventKeyRepeat:
        if (!m_sentHeld.isKeyDown((KeyButton)event.m_c)) {
            return;
        }
        break;
    case kEventKeyUp:
        // a release whose press happened before capture started
        if (!m_sentHeld.releaseKey((KeyButton)event.m_c)) {
            return;
        }
        break;
    case kEventButtonDown:
        if (!m_sentHeld.pressButton((ButtonID)event.m_a)) {
            return;
        }
        break;
    case kEventButtonUp:
        if (!m_sentHeld.releaseButton((ButtonID)event.m_a)) {
            return;
        }
        break;
    default:
        break;
    }
    m_transport->sendRemoteInput(m_epoch, event);
    ++m_forwarded;
}

void
RemoteControlController::processPending()
{
    m_wakePending = false;

    std::vector<RemoteEvent> events;
    bool overflow, hotkey;
    std::string lost;
    std::vector<bool> focus;
    {
        std::lock_guard<std::mutex> lock(m_inboxMutex);
        RemoteEvent e;
        while (m_inbox.pop(e)) {
            events.push_back(e);
        }
        overflow = m_inboxOverflow;
        hotkey   = m_releaseHotkeyPressed;
        lost     = m_captureLost;
        focus.swap(m_focusChanges);
        m_inboxOverflow = false;
        m_releaseHotkeyPressed = false;
        m_captureLost.clear();
    }

    if (m_state != kRemote && m_state != kSuspended) {
        return;
    }
    if (!m_transport->isConnectedToServer()) {
        onDisconnected();
        return;
    }

    // events captured before the release hotkey still go out in order
    for (size_t i = 0; i < events.size(); ++i) {
        if (m_state == kRemote) {
            forward(events[i]);
        }
    }

    for (size_t i = 0; i < focus.size(); ++i) {
        if (!focus[i] && m_state == kRemote) {
            // window mode: focus lost -> nothing stays pressed remotely
            releaseRemoteHeld();
            setState(kSuspended);
            setMessage("focus-lost", "session window lost the focus: forwarding paused");
        }
        else if (focus[i] && m_state == kSuspended) {
            setState(kRemote);
            setMessage("focus", "keyboard and mouse now control " + m_target +
                       ". Press " + m_releaseHotkey.toString() + " to return to this computer.");
        }
    }

    if (overflow) {
        stop("queue-overflow", "input queue overflowed: remote control stopped, input released");
        return;
    }
    if (hotkey) {
        stop("hotkey", "release hotkey " + m_releaseHotkey.toString() +
             " pressed: keyboard and mouse returned to this computer");
        return;
    }
    if (!lost.empty()) {
        stop("capture-lost", "local capture ended (" + lost +
             "): keyboard and mouse returned to this computer");
        return;
    }
    if (!focus.empty()) {
        notify();
    }
}

void
RemoteControlController::tick()
{
    if ((m_state == kRemote || m_state == kSuspended) && m_epoch != 0) {
        if (!m_transport->isConnectedToServer()) {
            onDisconnected();
            return;
        }
        m_transport->sendRemoteInput(m_epoch, RemoteEvent(kEventHeartbeat));
    }
    notify();
}

RemoteControlController::Status
RemoteControlController::status() const
{
    Status s;
    s.m_state            = m_state;
    s.m_mode             = m_mode;
    s.m_connected        = m_transport->isConnectedToServer();
    s.m_serverSupports   = m_transport->isRemoteControlSupported();
    s.m_viewOnly         = m_viewOnly;
    s.m_target           = m_target.empty() ? m_requestedTarget : m_target;
    s.m_controller       = m_controller;
    s.m_server           = m_transport->serverName();
    s.m_releaseHotkey    = m_releaseHotkey.toString();
    s.m_lastEvent        = m_lastEvent;
    s.m_lastMessage      = m_lastMessage;
    s.m_epoch            = m_epoch;
    s.m_forwarded        = m_forwarded;
    s.m_ctrlAltDelNative = (m_capabilities & kCapCtrlAltDel) != 0;
    if (m_capture) {
        s.m_captureBackend   = m_capture->name();
        s.m_captureAvailable = m_capture->isAvailable(s.m_captureUnavailableReason);
        s.m_windowModeSupported = m_capture->supportsMode(ILocalInputCapture::kModeWindow);
        s.m_captureModeSupported = m_capture->supportsMode(ILocalInputCapture::kModeCapture);
    }
    else {
        s.m_captureBackend = "none";
        s.m_captureUnavailableReason = "local input capture is not implemented on this platform";
    }
    {
        std::lock_guard<std::mutex> lock(m_inboxMutex);
        s.m_coalesced = m_inbox.coalescedCount();
        s.m_dropped   = m_inbox.droppedCount();
    }
    return s;
}

std::string
RemoteControlController::statusToJson(const Status& s)
{
    std::ostringstream out;
    out << "{"
        << "\"state\":" << jsonString(stateName(s.m_state))
        << ",\"mode\":" << jsonString(s.m_mode == ILocalInputCapture::kModeWindow ? "window" : "capture")
        << ",\"connected\":" << (s.m_connected ? "true" : "false")
        << ",\"serverSupports\":" << (s.m_serverSupports ? "true" : "false")
        << ",\"viewOnly\":" << (s.m_viewOnly ? "true" : "false")
        << ",\"captureBackend\":" << jsonString(s.m_captureBackend)
        << ",\"captureAvailable\":" << (s.m_captureAvailable ? "true" : "false")
        << ",\"captureUnavailableReason\":" << jsonString(s.m_captureUnavailableReason)
        << ",\"windowModeSupported\":" << (s.m_windowModeSupported ? "true" : "false")
        << ",\"captureModeSupported\":" << (s.m_captureModeSupported ? "true" : "false")
        << ",\"server\":" << jsonString(s.m_server)
        << ",\"target\":" << jsonString(s.m_target)
        << ",\"controller\":" << jsonString(s.m_controller)
        << ",\"releaseHotkey\":" << jsonString(s.m_releaseHotkey)
        << ",\"ctrlAltDelNative\":" << (s.m_ctrlAltDelNative ? "true" : "false")
        << ",\"lastEvent\":" << jsonString(s.m_lastEvent)
        << ",\"lastMessage\":" << jsonString(s.m_lastMessage)
        << ",\"epoch\":" << s.m_epoch
        << ",\"forwarded\":" << s.m_forwarded
        << ",\"coalesced\":" << s.m_coalesced
        << ",\"dropped\":" << s.m_dropped
        << "}";
    return out.str();
}

void
RemoteControlController::notify()
{
    if (m_statusListener) {
        m_statusListener(status());
    }
}
