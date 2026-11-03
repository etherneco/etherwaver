/*
 * Etherwaver -- remote control unit tests: server side arbitration.
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "server/RemoteControlArbiter.h"

#include "test/global/gtest.h"

#include <set>

using namespace barrier::remote;
typedef barrier::remote::Event RemoteEvent;

namespace {

RemoteControlArbiter
makeArbiter(std::set<std::string> permitted = { "laptop", "desk" })
{
    return RemoteControlArbiter([permitted](const std::string& name) {
        return permitted.count(name) != 0;
    }, 10);
}

} // namespace

TEST(RemoteControlArbiterTests, grantsPermittedClientAndDeniesViewer)
{
    RemoteControlArbiter a = makeArbiter();
    RemoteControlArbiter::Decision d = a.request("viewer", "", false, true, false, 0.0);
    EXPECT_EQ(kStateDenied, d.m_state);
    EXPECT_EQ(kReasonNotPermitted, d.m_reason);
    EXPECT_FALSE(a.hasController());

    d = a.request("laptop", "rack", false, true, false, 0.0);
    EXPECT_EQ(kStateGranted, d.m_state);
    EXPECT_NE(0u, d.m_epoch);
    EXPECT_EQ("laptop", a.controller());
    EXPECT_EQ("rack", a.target());
}

TEST(RemoteControlArbiterTests, refusesSelfAndUnknownTarget)
{
    RemoteControlArbiter a = makeArbiter();
    EXPECT_EQ(kReasonTargetIsSelf, a.request("laptop", "laptop", false, true, true, 0).m_reason);
    EXPECT_EQ(kReasonUnknownTarget, a.request("laptop", "nope", false, false, false, 0).m_reason);
    EXPECT_FALSE(a.hasController());
}

TEST(RemoteControlArbiterTests, onlyOneControllerWithoutTakeover)
{
    RemoteControlArbiter a = makeArbiter();
    const UInt32 epoch = a.request("laptop", "", false, true, false, 0).m_epoch;
    RemoteControlArbiter::Decision d = a.request("desk", "", false, true, false, 0);
    EXPECT_EQ(kStateDenied, d.m_state);
    EXPECT_EQ(kReasonBusy, d.m_reason);
    EXPECT_EQ("laptop", a.controller());

    // input of the second client never mixes in
    RemoteEvent e = RemoteEvent::keyDown('x', 0, 53);
    EXPECT_EQ(RemoteControlArbiter::kDropNotController, a.input("desk", epoch, e, 0));
}

TEST(RemoteControlArbiterTests, takeoverReleasesPreviousControllersInput)
{
    RemoteControlArbiter a = makeArbiter();
    const UInt32 first = a.request("laptop", "", false, true, false, 0).m_epoch;
    RemoteEvent ctrl = RemoteEvent::keyDown(kKeyControl_L, 0, 37);
    RemoteEvent btn  = RemoteEvent::buttonDown(kButtonLeft);
    ASSERT_EQ(RemoteControlArbiter::kAccept, a.input("laptop", first, ctrl, 0));
    ASSERT_EQ(RemoteControlArbiter::kAccept, a.input("laptop", first, btn, 0));

    RemoteControlArbiter::Decision d = a.request("desk", "", true, true, false, 1);
    EXPECT_EQ(kStateGranted, d.m_state);
    EXPECT_EQ("laptop", d.m_previousController);
    ASSERT_EQ(2u, d.m_release.size());
    EXPECT_EQ(RemoteEvent::buttonUp(kButtonLeft), d.m_release[0]);
    EXPECT_EQ(kEventKeyUp, d.m_release[1].m_kind);
    EXPECT_NE(first, d.m_epoch);

    // the old controller's in-flight input is now stale
    RemoteEvent late = RemoteEvent::keyUp(kKeyControl_L, 0, 37);
    EXPECT_EQ(RemoteControlArbiter::kDropNotController, a.input("laptop", first, late, 1));
}

TEST(RemoteControlArbiterTests, takeoverCanBeDisabled)
{
    RemoteControlArbiter a = makeArbiter();
    RemoteControlArbiter::Policy p;
    p.m_allowTakeover = false;
    a.setPolicy(p);
    a.request("laptop", "", false, true, false, 0);
    EXPECT_EQ(kReasonBusy, a.request("desk", "", true, true, false, 0).m_reason);
    EXPECT_EQ("laptop", a.controller());
}

TEST(RemoteControlArbiterTests, staleEpochAfterRegrantIsDropped)
{
    RemoteControlArbiter a = makeArbiter();
    const UInt32 first = a.request("laptop", "", false, true, false, 0).m_epoch;
    std::vector<RemoteEvent> release;
    ASSERT_TRUE(a.release("laptop", first, release));
    const UInt32 second = a.request("laptop", "", false, true, false, 1).m_epoch;
    ASSERT_NE(first, second);

    // events queued before the reconnect must not be replayed
    RemoteEvent old = RemoteEvent::keyDown('a', 0, 38);
    EXPECT_EQ(RemoteControlArbiter::kDropStaleEpoch, a.input("laptop", first, old, 1));
    RemoteEvent fresh = RemoteEvent::keyDown('a', 0, 38);
    EXPECT_EQ(RemoteControlArbiter::kAccept, a.input("laptop", second, fresh, 1));
}

TEST(RemoteControlArbiterTests, releaseWithWrongEpochIsIgnored)
{
    RemoteControlArbiter a = makeArbiter();
    const UInt32 epoch = a.request("laptop", "", false, true, false, 0).m_epoch;
    std::vector<RemoteEvent> release;
    EXPECT_FALSE(a.release("laptop", epoch + 1, release));
    EXPECT_FALSE(a.release("desk", epoch, release));
    EXPECT_TRUE(a.hasController());
    EXPECT_TRUE(a.release("laptop", epoch, release));
    EXPECT_FALSE(a.hasController());
}

TEST(RemoteControlArbiterTests, duplicatePressBecomesRepeatAndUnmatchedReleaseIsDropped)
{
    RemoteControlArbiter a = makeArbiter();
    const UInt32 epoch = a.request("laptop", "", false, true, false, 0).m_epoch;
    RemoteEvent down = RemoteEvent::keyDown('a', 0, 38);
    ASSERT_EQ(RemoteControlArbiter::kAccept, a.input("laptop", epoch, down, 0));
    RemoteEvent again = RemoteEvent::keyDown('a', 0, 38);
    ASSERT_EQ(RemoteControlArbiter::kAccept, a.input("laptop", epoch, again, 0));
    EXPECT_EQ(kEventKeyRepeat, again.m_kind);
    EXPECT_EQ(1, again.m_d);

    RemoteEvent up = RemoteEvent::keyUp('a', 0, 38);
    EXPECT_EQ(RemoteControlArbiter::kAccept, a.input("laptop", epoch, up, 0));
    RemoteEvent up2 = RemoteEvent::keyUp('a', 0, 38);
    EXPECT_EQ(RemoteControlArbiter::kDropUnmatched, a.input("laptop", epoch, up2, 0));
    RemoteEvent repeat = RemoteEvent::keyRepeat('a', 0, 38, 1);
    EXPECT_EQ(RemoteControlArbiter::kDropUnmatched, a.input("laptop", epoch, repeat, 0));
    RemoteEvent bogus(kEventKeyDown, kKeyNone, 0, 38);
    EXPECT_EQ(RemoteControlArbiter::kDropInvalid, a.input("laptop", epoch, bogus, 0));
}

TEST(RemoteControlArbiterTests, disconnectReleasesEverythingHeld)
{
    RemoteControlArbiter a = makeArbiter();
    const UInt32 epoch = a.request("laptop", "", false, true, false, 0).m_epoch;
    RemoteEvent e1 = RemoteEvent::keyDown(kKeyShift_L, 0, 50);
    RemoteEvent e2 = RemoteEvent::keyDown(kKeyControl_L, KeyModifierShift, 37);
    RemoteEvent e3 = RemoteEvent::keyDown(kKeyAlt_L, KeyModifierShift | KeyModifierControl, 64);
    RemoteEvent e4 = RemoteEvent::buttonDown(kButtonLeft);
    a.input("laptop", epoch, e1, 0);
    a.input("laptop", epoch, e2, 0);
    a.input("laptop", epoch, e3, 0);
    a.input("laptop", epoch, e4, 0);

    std::vector<RemoteEvent> release;
    EXPECT_FALSE(a.clientGone("desk", release));
    EXPECT_TRUE(release.empty());
    EXPECT_TRUE(a.clientGone("laptop", release));
    ASSERT_EQ(4u, release.size());
    EXPECT_EQ(RemoteEvent::buttonUp(kButtonLeft), release[0]);
    std::set<int> keys;
    for (size_t i = 1; i < release.size(); ++i) {
        EXPECT_EQ(kEventKeyUp, release[i].m_kind);
        keys.insert(release[i].m_c);
    }
    EXPECT_EQ(std::set<int>({ 37, 50, 64 }), keys) << "no Ctrl, Alt or Shift left pressed";
    EXPECT_FALSE(a.hasController());
    EXPECT_TRUE(a.held().empty());
}

TEST(RemoteControlArbiterTests, leaseTimeoutReleasesThenRevokes)
{
    RemoteControlArbiter a = makeArbiter();
    const UInt32 epoch = a.request("laptop", "", false, true, false, 100.0).m_epoch;
    RemoteEvent down = RemoteEvent::keyDown(kKeyControl_L, 0, 37);
    a.input("laptop", epoch, down, 100.0);

    std::vector<RemoteEvent> release;
    EXPECT_EQ(RemoteControlArbiter::kTimeoutNone, a.checkTimeout(102.0, release));
    EXPECT_TRUE(release.empty());

    // a heartbeat renews the lease
    RemoteEvent hb(kEventHeartbeat);
    EXPECT_EQ(RemoteControlArbiter::kAcceptHeartbeat, a.input("laptop", epoch, hb, 102.5));
    EXPECT_EQ(RemoteControlArbiter::kTimeoutNone, a.checkTimeout(105.0, release));

    // silent for more than the release timeout: held input is released
    EXPECT_EQ(RemoteControlArbiter::kTimeoutReleased, a.checkTimeout(105.6, release));
    ASSERT_EQ(1u, release.size());
    EXPECT_EQ(kEventKeyUp, release[0].m_kind);
    EXPECT_TRUE(a.hasController()) << "grant survives a short outage";
    release.clear();
    EXPECT_EQ(RemoteControlArbiter::kTimeoutNone, a.checkTimeout(106.0, release));

    // silent for longer: the grant is revoked
    EXPECT_EQ(RemoteControlArbiter::kTimeoutRevoked, a.checkTimeout(118.0, release));
    EXPECT_FALSE(a.hasController());
}

TEST(RemoteControlArbiterTests, permissionCheckedOnEveryRequest)
{
    bool allowed = true;
    RemoteControlArbiter a([&allowed](const std::string&) { return allowed; }, 1);
    EXPECT_EQ(kStateGranted, a.request("laptop", "", false, true, false, 0).m_state);
    std::vector<RemoteEvent> release;
    a.revoke(release);
    allowed = false;
    EXPECT_EQ(kReasonNotPermitted, a.request("laptop", "", false, true, false, 0).m_reason);
}
