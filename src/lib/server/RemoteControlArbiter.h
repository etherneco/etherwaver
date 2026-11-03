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

#pragma once

#include "barrier/RemoteInput.h"

#include <functional>
#include <string>
#include <vector>

//! Decides which client may forward its local input to the server
/*!
Pure state machine (no I/O, no clock) so it can be unit tested.  The
server feeds it requests, input and the current time; it answers what to
do.  Exactly one client controls at a time; mixing the input of several
clients is never allowed.

Lease handling: every accepted message from the controller renews the
lease.  After \c releaseTimeout seconds without a message everything the
controller holds is released (the connection may be dead); after
\c revokeTimeout seconds the grant itself is revoked.
*/
class RemoteControlArbiter {
public:
    struct Policy {
        Policy() : m_allowTakeover(true), m_releaseTimeout(3.0), m_revokeTimeout(15.0) { }
        bool            m_allowTakeover;
        double          m_releaseTimeout;
        double          m_revokeTimeout;
    };

    struct Decision {
        Decision() : m_state(barrier::remote::kStateIdle),
            m_reason(barrier::remote::kReasonNone), m_epoch(0) { }
        int             m_state;          // barrier::remote::ControlState
        int             m_reason;         // barrier::remote::ControlReason
        UInt32          m_epoch;
        std::string     m_previousController; // set when a takeover happened
        std::vector<barrier::remote::Event> m_release; // to inject before switching
    };

    enum InputVerdict {
        kAccept,            // inject \c event (possibly rewritten)
        kAcceptHeartbeat,   // lease renewed, nothing to inject
        kDropNotController, // sender does not hold control
        kDropStaleEpoch,    // event belongs to an old grant
        kDropInvalid,       // malformed event
        kDropUnmatched      // release without press, already handled
    };

    //! \p isPermitted tells whether a client may control at all
    //! (viewers are refused).  \p initialEpoch seeds the epoch counter.
    RemoteControlArbiter(std::function<bool(const std::string&)> isPermitted,
                         UInt32 initialEpoch = 1);

    void                setPolicy(const Policy& policy) { m_policy = policy; }
    const Policy&       policy() const { return m_policy; }
    void                setPermissionCheck(std::function<bool(const std::string&)> f)
                        { m_isPermitted = f; }

    //! Handle a control request.  \p targetExists / \p targetIsSelf are
    //! resolved by the server.
    Decision            request(const std::string& client, const std::string& target,
                            bool takeover, bool targetExists, bool targetIsSelf,
                            double now);

    //! Controller releases control.  Returns false (and does nothing)
    //! if \p client is not the controller or \p epoch is stale.
    bool                release(const std::string& client, UInt32 epoch,
                            std::vector<barrier::remote::Event>& toRelease);

    //! Connection of \p client is gone.  Returns true if it was the
    //! controller; \p toRelease receives what must be released.
    bool                clientGone(const std::string& client,
                            std::vector<barrier::remote::Event>& toRelease);

    //! Revoke control (server side decision), e.g. permission removed.
    bool                revoke(std::vector<barrier::remote::Event>& toRelease);

    //! Validate and track an input event.  May rewrite \p event: a
    //! repeated press of a held key becomes a repeat.
    InputVerdict        input(const std::string& client, UInt32 epoch,
                            barrier::remote::Event& event, double now);

    enum TimeoutAction { kTimeoutNone, kTimeoutReleased, kTimeoutRevoked };
    //! Check the lease.  On kTimeoutReleased / kTimeoutRevoked
    //! \p toRelease receives the events to inject.
    TimeoutAction       checkTimeout(double now,
                            std::vector<barrier::remote::Event>& toRelease);

    //! Release everything held (e.g. the controlled host changes)
    std::vector<barrier::remote::Event> releaseHeld() { return m_held.releaseAll(); }

    //! Controlled host changed (screen switch).  Held state must have
    //! been released before.
    void                setTarget(const std::string& target) { m_target = target; }

    bool                hasController() const { return !m_controller.empty(); }
    const std::string&  controller() const { return m_controller; }
    const std::string&  target() const { return m_target; }
    UInt32              epoch() const { return m_epoch; }
    const barrier::remote::HeldInputState& held() const { return m_held; }
    bool                isController(const std::string& client) const
                        { return !m_controller.empty() && m_controller == client; }

    // diagnostics (counters only, never content)
    std::uint64_t              acceptedCount() const { return m_accepted; }
    std::uint64_t              droppedCount() const { return m_dropped; }

private:
    void                endControl(std::vector<barrier::remote::Event>& toRelease);

private:
    std::function<bool(const std::string&)> m_isPermitted;
    Policy              m_policy;
    std::string         m_controller;
    std::string         m_target;
    UInt32              m_epoch;
    UInt32              m_nextEpoch;
    double              m_lastActivity;
    bool                m_heldReleasedByTimeout;
    barrier::remote::HeldInputState m_held;
    std::uint64_t              m_accepted;
    std::uint64_t              m_dropped;
};
