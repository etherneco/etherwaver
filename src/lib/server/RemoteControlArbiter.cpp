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

#include "server/RemoteControlArbiter.h"

using namespace barrier::remote;
typedef barrier::remote::Event RemoteEvent;

RemoteControlArbiter::RemoteControlArbiter(
        std::function<bool(const std::string&)> isPermitted,
        UInt32 initialEpoch) :
    m_isPermitted(isPermitted),
    m_epoch(0),
    m_nextEpoch(initialEpoch == 0 ? 1 : initialEpoch),
    m_lastActivity(0.0),
    m_heldReleasedByTimeout(false),
    m_accepted(0),
    m_dropped(0)
{
}

RemoteControlArbiter::Decision
RemoteControlArbiter::request(const std::string& client, const std::string& target,
                              bool takeover, bool targetExists, bool targetIsSelf,
                              double now)
{
    Decision d;
    if (!m_isPermitted || !m_isPermitted(client)) {
        d.m_state  = kStateDenied;
        d.m_reason = kReasonNotPermitted;
        return d;
    }
    if (targetIsSelf) {
        d.m_state  = kStateDenied;
        d.m_reason = kReasonTargetIsSelf;
        return d;
    }
    if (!targetExists) {
        d.m_state  = kStateDenied;
        d.m_reason = kReasonUnknownTarget;
        return d;
    }

    if (hasController() && m_controller != client) {
        if (!takeover || !m_policy.m_allowTakeover) {
            d.m_state  = kStateDenied;
            d.m_reason = kReasonBusy;
            return d;
        }
        // takeover: the previous controller loses control, its held
        // input is released before the new controller starts.
        d.m_previousController = m_controller;
        endControl(d.m_release);
    }
    else if (hasController() && m_controller == client) {
        // re-request by the controller (e.g. host change): keep the
        // grant but release what was held on the previous target.
        std::vector<RemoteEvent> released = m_held.releaseAll();
        d.m_release.insert(d.m_release.end(), released.begin(), released.end());
    }

    m_controller = client;
    m_target     = target;
    m_epoch      = m_nextEpoch++;
    if (m_nextEpoch == 0) {
        m_nextEpoch = 1;
    }
    m_lastActivity          = now;
    m_heldReleasedByTimeout = false;

    d.m_state  = kStateGranted;
    d.m_reason = d.m_previousController.empty() ? kReasonNone : kReasonTakenOver;
    d.m_epoch  = m_epoch;
    return d;
}

void
RemoteControlArbiter::endControl(std::vector<RemoteEvent>& toRelease)
{
    std::vector<RemoteEvent> released = m_held.releaseAll();
    toRelease.insert(toRelease.end(), released.begin(), released.end());
    m_controller.clear();
    m_target.clear();
    m_epoch = 0;
    m_heldReleasedByTimeout = false;
}

bool
RemoteControlArbiter::release(const std::string& client, UInt32 epoch,
                              std::vector<RemoteEvent>& toRelease)
{
    if (!isController(client) || epoch != m_epoch) {
        return false;
    }
    endControl(toRelease);
    return true;
}

bool
RemoteControlArbiter::clientGone(const std::string& client, std::vector<RemoteEvent>& toRelease)
{
    if (!isController(client)) {
        return false;
    }
    endControl(toRelease);
    return true;
}

bool
RemoteControlArbiter::revoke(std::vector<RemoteEvent>& toRelease)
{
    if (!hasController()) {
        return false;
    }
    endControl(toRelease);
    return true;
}

RemoteControlArbiter::InputVerdict
RemoteControlArbiter::input(const std::string& client, UInt32 epoch, RemoteEvent& event, double now)
{
    if (!isController(client)) {
        ++m_dropped;
        return kDropNotController;
    }
    if (epoch != m_epoch) {
        ++m_dropped;
        return kDropStaleEpoch;
    }
    if (!isValidEvent(event.m_kind, event.m_a, event.m_b, event.m_c, event.m_d)) {
        ++m_dropped;
        return kDropInvalid;
    }

    m_lastActivity = now;
    m_heldReleasedByTimeout = false;

    switch (event.m_kind) {
    case kEventHeartbeat:
        return kAcceptHeartbeat;

    case kEventKeyDown:
        if (!m_held.pressKey((KeyID)event.m_a, (KeyModifierMask)event.m_b,
                             (KeyButton)event.m_c)) {
            // already down: an autorepeat sent as press
            event = RemoteEvent::keyRepeat((KeyID)event.m_a, (KeyModifierMask)event.m_b,
                                     (KeyButton)event.m_c, 1);
        }
        break;

    case kEventKeyRepeat:
        if (!m_held.isKeyDown((KeyButton)event.m_c)) {
            ++m_dropped;
            return kDropUnmatched;
        }
        break;

    case kEventKeyUp:
        if (!m_held.releaseKey((KeyButton)event.m_c)) {
            ++m_dropped;
            return kDropUnmatched;
        }
        break;

    case kEventButtonDown:
        if (!m_held.pressButton((ButtonID)event.m_a)) {
            ++m_dropped;
            return kDropUnmatched;
        }
        break;

    case kEventButtonUp:
        if (!m_held.releaseButton((ButtonID)event.m_a)) {
            ++m_dropped;
            return kDropUnmatched;
        }
        break;

    default:
        break;
    }

    ++m_accepted;
    return kAccept;
}

RemoteControlArbiter::TimeoutAction
RemoteControlArbiter::checkTimeout(double now, std::vector<RemoteEvent>& toRelease)
{
    if (!hasController()) {
        return kTimeoutNone;
    }
    const double idle = now - m_lastActivity;
    if (m_policy.m_revokeTimeout > 0 && idle >= m_policy.m_revokeTimeout) {
        endControl(toRelease);
        return kTimeoutRevoked;
    }
    if (m_policy.m_releaseTimeout > 0 && idle >= m_policy.m_releaseTimeout &&
        !m_heldReleasedByTimeout) {
        m_heldReleasedByTimeout = true;
        if (!m_held.empty()) {
            std::vector<RemoteEvent> released = m_held.releaseAll();
            toRelease.insert(toRelease.end(), released.begin(), released.end());
            return kTimeoutReleased;
        }
    }
    return kTimeoutNone;
}
