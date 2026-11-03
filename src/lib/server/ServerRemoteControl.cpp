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

//
// Server side of remote control (protocol 1.8).
//
// A client forwards its local keyboard and mouse.  Accepted events are
// injected through the regular input path: if the server's own screen is
// active they are synthesized there (PrimaryClient::inject*), otherwise
// they are relayed to the active client exactly like the server's own
// physical input (onKeyDown(), onMouseMoveSecondary(), ...), so screen
// switching, keyboard broadcasting and input filters keep working.
//
// Safety rules implemented here:
//  - only one controller; a client never controls itself, and the server
//    never switches onto the controlling client (no input loop);
//  - input carries the epoch of the grant, stale epochs are dropped;
//  - everything the controller holds is released when it releases
//    control, disconnects, times out, is taken over, or the controlled
//    host changes;
//  - key content is never logged.
//

#include "server/Server.h"

#include "server/RemoteControlArbiter.h"
#include "server/PrimaryClient.h"
#include "server/BaseClientProxy.h"
#include "server/Config.h"
#include "barrier/option_types.h"
#include "barrier/Screen.h"
#include "base/IEventQueue.h"
#include "base/TMethodEventJob.h"
#include "base/Log.h"
#include "arch/Arch.h"

#include <algorithm>
#include <sstream>

using namespace barrier::remote;
typedef barrier::remote::Event RemoteEvent;

namespace {

std::string
jsonEscapeRc(const std::string& value)
{
    std::string out;
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        }
        else if (static_cast<unsigned char>(c) < 0x20) {
            out += ' ';
        }
        else {
            out += c;
        }
    }
    return out;
}

} // namespace

void
Server::initRemoteControl()
{
    m_remoteControl.reset(new RemoteControlArbiter(
        [this](const std::string& name) { return isRemoteControlPermitted(name); },
        static_cast<UInt32>(ARCH->time() * 1000.0) | 1));
    applyRemoteControlOptions();

    if (m_events != NULL) {
        m_remoteControlTimer = m_events->newTimer(0.5, m_remoteControl.get());
        m_events->adoptHandler(::Event::kTimer, m_remoteControl.get(),
                               new TMethodEventJob<Server>(this,
                                   &Server::handleRemoteControlTimer));
    }
    updateRemoteControlSnapshot();
}

void
Server::shutdownRemoteControl()
{
    if (!m_remoteControl) {
        return;
    }
    std::vector<RemoteEvent> release;
    std::string controller = m_remoteControl->controller();
    if (m_remoteControl->revoke(release)) {
        injectRemoteEvents(release);
        BaseClientProxy* client = findClientByName(controller);
        if (client != NULL) {
            client->sendRemoteControlState(kStateRevoked, kReasonServerShutdown,
                                           0, 0, 0, 0, 0, 0, std::string(), std::string());
        }
    }
    if (m_remoteControlTimer != NULL && m_events != NULL) {
        m_events->removeHandler(::Event::kTimer, m_remoteControl.get());
        m_events->deleteTimer(m_remoteControlTimer);
        m_remoteControlTimer = NULL;
    }
}

void
Server::resetRemoteControlForTest()
{
    // used by unit tests: an arbiter without timers or event queue
    m_remoteControl.reset(new RemoteControlArbiter(
        [this](const std::string& name) { return isRemoteControlPermitted(name); }, 100));
    if (m_config != NULL) {
        applyRemoteControlOptions();
    }
}

void
Server::applyRemoteControlOptions()
{
    if (!m_remoteControl || m_config == NULL) {
        return;
    }
    RemoteControlArbiter::Policy policy;
    const Config::ScreenOptions* options = m_config->getOptions("");
    if (options != NULL) {
        Config::ScreenOptions::const_iterator i = options->find(kOptionRemoteControlTakeover);
        if (i != options->end()) {
            policy.m_allowTakeover = (i->second != 0);
        }
        i = options->find(kOptionRemoteControlTimeout);
        if (i != options->end() && i->second > 0) {
            policy.m_releaseTimeout = i->second / 1000.0;
            policy.m_revokeTimeout  = std::max(policy.m_releaseTimeout * 5.0, 15.0);
        }
    }
    m_remoteControl->setPolicy(policy);

    // permissions may have been removed by the new configuration
    if (m_remoteControl->hasController() &&
        !isRemoteControlPermitted(m_remoteControl->controller())) {
        LOG((CLOG_NOTE "remote control: permission of \"%s\" removed by configuration",
             m_remoteControl->controller().c_str()));
        BaseClientProxy* client = findClientByName(m_remoteControl->controller());
        std::vector<RemoteEvent> release;
        m_remoteControl->revoke(release);
        injectRemoteEvents(release);
        if (client != NULL) {
            sendRemoteControlStateTo(client, kStateRevoked, kReasonNotPermitted);
        }
        broadcastRemoteControlState(client);
    }
}

bool
Server::isRemoteControlPermitted(const std::string& name) const
{
    if (m_config == NULL || name.empty()) {
        return false;
    }
    // the server's own screen never "controls" through the network
    if (m_primaryClient != NULL && name == getName(m_primaryClient)) {
        return false;
    }
    // per-screen option wins over the global default
    const Config::ScreenOptions* options = m_config->getOptions(name);
    if (options != NULL) {
        Config::ScreenOptions::const_iterator i = options->find(kOptionRemoteControl);
        if (i != options->end()) {
            return i->second != 0;
        }
    }
    options = m_config->getOptions("");
    if (options != NULL) {
        Config::ScreenOptions::const_iterator i = options->find(kOptionRemoteControl);
        if (i != options->end()) {
            return i->second != 0;
        }
    }
    // secure default: viewing/being controlled only
    return false;
}

BaseClientProxy*
Server::findClientByName(const std::string& name) const
{
    if (name.empty()) {
        return NULL;
    }
    ClientList::const_iterator i = m_clients.find(name);
    if (i != m_clients.end()) {
        return i->second;
    }
    if (m_config != NULL) {
        const std::string canonical = m_config->getCanonicalName(name);
        if (!canonical.empty()) {
            i = m_clients.find(canonical);
            if (i != m_clients.end()) {
                return i->second;
            }
        }
    }
    for (i = m_clients.begin(); i != m_clients.end(); ++i) {
        if (i->second != NULL && i->second->getName() == name) {
            return i->second;
        }
    }
    return NULL;
}

bool
Server::isRemoteController(const BaseClientProxy* client) const
{
    return m_remoteControl && client != NULL &&
           m_remoteControl->isController(getName(client));
}

std::string
Server::getRemoteController() const
{
    return m_remoteControl ? m_remoteControl->controller() : std::string();
}

double
Server::remoteControlNow() const
{
    if (m_remoteControlTestClock >= 0.0) {
        return m_remoteControlTestClock;
    }
    return ARCH->time();
}

UInt8
Server::remoteControlCapabilities() const
{
    UInt8 caps = kCapAbsolute | kCapRelative | kCapHorizontalWheel;
#if WINAPI_MSWINDOWS
    if (m_active == m_primaryClient) {
        caps |= kCapCtrlAltDel;
    }
#endif
    return caps;
}

void
Server::sendRemoteControlStateTo(BaseClientProxy* client, int state, int reason)
{
    if (client == NULL || client == m_primaryClient || !client->supportsRemoteControl()) {
        return;
    }
    SInt32 x = 0, y = 0, w = 0, h = 0;
    std::string target;
    UInt32 epoch = 0;
    if (m_remoteControl && m_remoteControl->hasController()) {
        target = m_remoteControl->target();
        if (m_active != NULL) {
            m_active->getShape(x, y, w, h);
        }
        if (m_remoteControl->isController(getName(client))) {
            epoch = m_remoteControl->epoch();
        }
    }
    const std::string controller = m_remoteControl ? m_remoteControl->controller() : std::string();
    client->sendRemoteControlState(state, reason, epoch, x, y, w, h,
                                   remoteControlCapabilities(), controller, target);
}

void
Server::broadcastRemoteControlState(BaseClientProxy* except)
{
    updateRemoteControlSnapshot();
    if (!m_remoteControl) {
        return;
    }
    for (ClientList::const_iterator i = m_clients.begin(); i != m_clients.end(); ++i) {
        BaseClientProxy* client = i->second;
        if (client == except || client == m_primaryClient) {
            continue;
        }
        if (!m_remoteControl->hasController()) {
            sendRemoteControlStateTo(client, kStateIdle, kReasonNone);
        }
        else if (m_remoteControl->isController(i->first)) {
            sendRemoteControlStateTo(client, kStateGranted, kReasonNone);
        }
        else {
            sendRemoteControlStateTo(client, kStateControlledByOther, kReasonNone);
        }
    }
}

void
Server::updateRemoteControlSnapshot()
{
    std::ostringstream out;
    out << "{\"controller\":\"";
    if (m_remoteControl && m_remoteControl->hasController()) {
        out << jsonEscapeRc(m_remoteControl->controller()) << "\",\"target\":\""
            << jsonEscapeRc(m_remoteControl->target()) << "\",\"epoch\":"
            << m_remoteControl->epoch() << ",\"heldKeys\":"
            << m_remoteControl->held().keyCount() << ",\"heldButtons\":"
            << m_remoteControl->held().buttonCount();
    }
    else {
        out << "\",\"target\":\"\",\"epoch\":0,\"heldKeys\":0,\"heldButtons\":0";
    }
    if (m_remoteControl) {
        out << ",\"accepted\":" << m_remoteControl->acceptedCount()
            << ",\"dropped\":" << m_remoteControl->droppedCount();
    }
    out << "}";
    std::lock_guard<std::mutex> lock(m_mutex);
    m_remoteControlSnapshot = out.str();
}

void
Server::onRemoteControlRequest(BaseClientProxy* client, bool takeover, const std::string& target)
{
    if (!m_remoteControl || client == NULL) {
        return;
    }
    const std::string name = getName(client);

    BaseClientProxy* dst = target.empty() ? m_primaryClient : findClientByName(target);
    const bool targetExists = (dst != NULL);
    const bool targetIsSelf = (dst == client);
    const std::string targetName = targetExists ? getName(dst) : target;

    RemoteControlArbiter::Decision d =
        m_remoteControl->request(name, targetName, takeover, targetExists, targetIsSelf,
                                 remoteControlNow());

    if (d.m_state != kStateGranted) {
        LOG((CLOG_NOTE "remote control: refused \"%s\" (%s)", name.c_str(),
             controlReasonText(d.m_reason)));
        sendRemoteControlStateTo(client, d.m_state, d.m_reason);
        return;
    }

    // release whatever the previous grant held, on the host it was held on
    injectRemoteEvents(d.m_release);

    if (!d.m_previousController.empty()) {
        LOG((CLOG_NOTE "remote control: \"%s\" took over from \"%s\"",
             name.c_str(), d.m_previousController.c_str()));
        BaseClientProxy* previous = findClientByName(d.m_previousController);
        if (previous != NULL) {
            sendRemoteControlStateTo(previous, kStateRevoked, kReasonTakenOver);
        }
    }

    // move the focus to the requested host
    if (m_active != dst) {
        jumpToScreen(dst);
    }
    // jumpToScreen() may refuse (e.g. screen saver); report the truth
    m_remoteControl->setTarget(getName(m_active));

    LOG((CLOG_NOTE "remote control: \"%s\" now controls \"%s\" (epoch %u)",
         name.c_str(), m_remoteControl->target().c_str(), m_remoteControl->epoch()));
    sendRemoteControlStateTo(client, kStateGranted, kReasonNone);
    broadcastRemoteControlState(client);
}

void
Server::onRemoteControlRelease(BaseClientProxy* client, UInt32 epoch)
{
    if (!m_remoteControl || client == NULL) {
        return;
    }
    std::vector<RemoteEvent> release;
    if (!m_remoteControl->release(getName(client), epoch, release)) {
        // stale release (e.g. after a takeover): nothing to do
        return;
    }
    injectRemoteEvents(release);
    LOG((CLOG_NOTE "remote control: \"%s\" released control", getName(client).c_str()));
    sendRemoteControlStateTo(client, kStateReleased, kReasonNone);
    broadcastRemoteControlState(client);
}

void
Server::onRemoteInput(BaseClientProxy* client, UInt32 epoch, RemoteEvent event)
{
    if (!m_remoteControl || client == NULL) {
        return;
    }
    const RemoteControlArbiter::InputVerdict verdict =
        m_remoteControl->input(getName(client), epoch, event, remoteControlNow());

    switch (verdict) {
    case RemoteControlArbiter::kAccept:
        injectRemoteEvent(event);
        break;

    case RemoteControlArbiter::kAcceptHeartbeat:
        break;

    case RemoteControlArbiter::kDropNotController:
    case RemoteControlArbiter::kDropStaleEpoch:
        // input from a client without (current) control.  tell it the
        // truth once in a while so it stops capturing.
        if ((m_remoteControl->droppedCount() % 64) == 1) {
            LOG((CLOG_DEBUG "remote control: dropped input from \"%s\" (no current grant)",
                 getName(client).c_str()));
            sendRemoteControlStateTo(client,
                m_remoteControl->hasController() ? kStateControlledByOther : kStateIdle,
                kReasonProtocol);
        }
        break;

    case RemoteControlArbiter::kDropInvalid:
        LOG((CLOG_WARN "remote control: invalid input event from \"%s\"",
             getName(client).c_str()));
        break;

    case RemoteControlArbiter::kDropUnmatched:
        break;
    }
}

void
Server::injectRemoteEvents(const std::vector<RemoteEvent>& events)
{
    for (size_t i = 0; i < events.size(); ++i) {
        injectRemoteEvent(events[i]);
    }
}

void
Server::injectRemoteEvent(const RemoteEvent& event)
{
    if (m_active == NULL || m_primaryClient == NULL) {
        return;
    }
    if (isRemoteController(m_active)) {
        // never feed a controller its own input (would loop)
        return;
    }

    m_remoteInputDispatch = true;
    if (m_active == m_primaryClient) {
        // synthesize on the server's own screen.  the platform reports
        // the resulting motion like physical motion, so edge switching
        // to other hosts keeps working.
        switch (event.m_kind) {
        case kEventKeyDown:
            m_primaryClient->injectKeyDown((KeyID)event.m_a, (KeyModifierMask)event.m_b,
                                           (KeyButton)event.m_c);
            break;
        case kEventKeyRepeat:
            m_primaryClient->injectKeyRepeat((KeyID)event.m_a, (KeyModifierMask)event.m_b,
                                             event.m_d, (KeyButton)event.m_c);
            break;
        case kEventKeyUp:
            m_primaryClient->injectKeyUp((KeyButton)event.m_c);
            break;
        case kEventButtonDown:
            m_primaryClient->injectMouseButton((ButtonID)event.m_a, true);
            break;
        case kEventButtonUp:
            m_primaryClient->injectMouseButton((ButtonID)event.m_a, false);
            break;
        case kEventMoveAbs: {
            SInt32 x, y, w, h;
            m_primaryClient->getShape(x, y, w, h);
            const SInt32 cx = std::max(x, std::min<SInt32>(event.m_a, x + w - 1));
            const SInt32 cy = std::max(y, std::min<SInt32>(event.m_b, y + h - 1));
            m_primaryClient->injectMouseMove(cx, cy);
            break;
        }
        case kEventMoveRel:
            m_primaryClient->injectMouseRelativeMove(event.m_a, event.m_b);
            break;
        case kEventWheel:
            m_primaryClient->injectMouseWheel(event.m_a, event.m_b);
            break;
        default:
            break;
        }
    }
    else {
        // relay to the active client through the regular path
        switch (event.m_kind) {
        case kEventKeyDown:
            onKeyDown((KeyID)event.m_a, (KeyModifierMask)event.m_b, (KeyButton)event.m_c, NULL);
            break;
        case kEventKeyRepeat:
            onKeyRepeat((KeyID)event.m_a, (KeyModifierMask)event.m_b, event.m_d,
                        (KeyButton)event.m_c);
            break;
        case kEventKeyUp:
            onKeyUp((KeyID)event.m_a, (KeyModifierMask)event.m_b, (KeyButton)event.m_c, NULL);
            break;
        case kEventButtonDown:
            onMouseDown((ButtonID)event.m_a);
            break;
        case kEventButtonUp:
            onMouseUp((ButtonID)event.m_a);
            break;
        case kEventMoveAbs: {
            SInt32 x, y, w, h;
            m_active->getShape(x, y, w, h);
            m_x = std::max(x, std::min<SInt32>(event.m_a, x + w - 1));
            m_y = std::max(y, std::min<SInt32>(event.m_b, y + h - 1));
            m_active->mouseMove(m_x, m_y);
            break;
        }
        case kEventMoveRel:
            onMouseMoveSecondary(event.m_a, event.m_b);
            break;
        case kEventWheel:
            if (event.m_b != 0) {
                onMouseWheel(0, event.m_b);
            }
            if (event.m_a != 0) {
                onMouseWheel(event.m_a, 0);
            }
            break;
        default:
            break;
        }
    }
    m_remoteInputDispatch = false;
}

void
Server::remoteControlBeforeSwitch()
{
    // the controlled host is about to change: release everything the
    // controller holds on the current host first.
    if (m_remoteControl && m_remoteControl->hasController() &&
        !m_remoteControl->held().empty()) {
        injectRemoteEvents(m_remoteControl->releaseHeld());
    }
}

void
Server::remoteControlTargetChanged()
{
    if (!m_remoteControl || !m_remoteControl->hasController() || m_active == NULL) {
        return;
    }
    const std::string target = getName(m_active);
    if (target == m_remoteControl->target()) {
        return;
    }
    m_remoteControl->setTarget(target);
    LOG((CLOG_NOTE "remote control: controlled host is now \"%s\"", target.c_str()));
    BaseClientProxy* controller = findClientByName(m_remoteControl->controller());
    sendRemoteControlStateTo(controller, kStateGranted, kReasonTargetChanged);
    broadcastRemoteControlState(controller);
}

void
Server::remoteControlClientRemoved(BaseClientProxy* client)
{
    if (!m_remoteControl || client == NULL) {
        return;
    }
    const std::string name = getName(client);
    std::vector<RemoteEvent> release;
    if (m_remoteControl->clientGone(name, release)) {
        LOG((CLOG_NOTE "remote control: controller \"%s\" disconnected, releasing %d held inputs",
             name.c_str(), (int)release.size()));
        injectRemoteEvents(release);
        broadcastRemoteControlState(client);
    }
    else if (m_remoteControl->hasController() && client == m_active) {
        // the controlled host went away; its held state is gone with it
        m_remoteControl->releaseHeld();
    }
}

void
Server::handleRemoteControlTimer(const ::Event&, void*)
{
    remoteControlTick(remoteControlNow());
}

void
Server::remoteControlTick(double now)
{
    if (!m_remoteControl) {
        return;
    }
    const std::string controller = m_remoteControl->controller();
    std::vector<RemoteEvent> release;
    switch (m_remoteControl->checkTimeout(now, release)) {
    case RemoteControlArbiter::kTimeoutReleased:
        LOG((CLOG_NOTE "remote control: no input from \"%s\" within the lease, released %d held inputs",
             controller.c_str(), (int)release.size()));
        injectRemoteEvents(release);
        updateRemoteControlSnapshot();
        break;

    case RemoteControlArbiter::kTimeoutRevoked: {
        LOG((CLOG_NOTE "remote control: lease of \"%s\" expired, control revoked", controller.c_str()));
        injectRemoteEvents(release);
        BaseClientProxy* client = findClientByName(controller);
        sendRemoteControlStateTo(client, kStateRevoked, kReasonTimeout);
        broadcastRemoteControlState(client);
        break;
    }

    default:
        break;
    }
}
