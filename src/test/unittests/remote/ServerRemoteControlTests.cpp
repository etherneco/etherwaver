/*
 * Etherwaver -- remote control unit tests: server integration (input
 * path, permissions, session loss, host switching, loop protection).
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#define BARRIER_TEST_ENV

#include "server/Server.h"
#include "server/BaseClientProxy.h"
#include "server/PrimaryClient.h"
#include "server/RemoteControlArbiter.h"
#include "server/Config.h"
#include "barrier/option_types.h"

#include "test/global/gtest.h"
#include "test/global/TestEventQueue.h"

#include <memory>

using namespace barrier::remote;
typedef barrier::remote::Event RemoteEvent;

namespace {

struct Recorded {
    std::string m_what;
    SInt32 m_a, m_b;
    bool operator==(const Recorded& o) const { return m_what == o.m_what && m_a == o.m_a && m_b == o.m_b; }
};

struct StateMsg {
    int m_state, m_reason;
    UInt32 m_epoch;
    std::string m_controller, m_target;
    SInt32 m_w, m_h;
};

class RcFakeClient : public BaseClientProxy {
public:
    RcFakeClient(const std::string& name, bool rc = true) :
        BaseClientProxy(name), m_rc(rc), m_entered(false) { }

    void* getEventTarget() const { return NULL; }
    bool getClipboard(ClipboardID, IClipboard*) const { return false; }
    void getShape(SInt32& x, SInt32& y, SInt32& w, SInt32& h) const { x = 0; y = 0; w = 1280; h = 720; }
    void getScreens(std::vector<ClientScreenInfo>&) const { }
    void getCursorPos(SInt32& x, SInt32& y) const { x = 10; y = 10; }
    void enter(SInt32, SInt32, UInt32, KeyModifierMask, bool) { m_entered = true; }
    bool leave() { m_entered = false; return true; }
    void setClipboard(ClipboardID, const IClipboard*) {}
    void grabClipboard(ClipboardID) {}
    void setClipboardDirty(ClipboardID, bool) {}
    void keyDown(KeyID id, KeyModifierMask, KeyButton b) { m_input.push_back({ "kd", (SInt32)id, (SInt32)b }); }
    void keyRepeat(KeyID id, KeyModifierMask, SInt32, KeyButton b) { m_input.push_back({ "kr", (SInt32)id, (SInt32)b }); }
    void keyUp(KeyID id, KeyModifierMask, KeyButton b) { m_input.push_back({ "ku", (SInt32)id, (SInt32)b }); }
    void mouseDown(ButtonID id) { m_input.push_back({ "md", (SInt32)id, 0 }); }
    void mouseUp(ButtonID id) { m_input.push_back({ "mu", (SInt32)id, 0 }); }
    void mouseMove(SInt32 x, SInt32 y) { m_input.push_back({ "mm", x, y }); }
    void mouseRelativeMove(SInt32 x, SInt32 y) { m_input.push_back({ "mr", x, y }); }
    void mouseWheel(SInt32 x, SInt32 y) { m_input.push_back({ "mw", x, y }); }
    void screensaver(bool) {}
    void resetOptions() {}
    void setOptions(const OptionsList&) {}
    void sendDragInfo(UInt32, const char*, size_t) {}
    void fileChunkSending(UInt8, char*, size_t) {}
    barrier::IStream* getStream() const { return NULL; }

    bool supportsRemoteControl() const { return m_rc; }
    void sendRemoteControlState(int state, int reason, UInt32 epoch, SInt32, SInt32,
                                SInt32 w, SInt32 h, UInt8, const std::string& controller,
                                const std::string& target)
    {
        m_states.push_back({ state, reason, epoch, controller, target, w, h });
    }

    bool m_rc;
    bool m_entered;
    std::vector<Recorded> m_input;
    std::vector<StateMsg> m_states;
};

class RcFakePrimary : public PrimaryClient {
public:
    RcFakePrimary(const std::string& name) : PrimaryClient(), m_name(name) { }

    std::string getName() const { return m_name; }
    void* getEventTarget() const { return NULL; }
    bool getClipboard(ClipboardID, IClipboard*) const { return false; }
    void getShape(SInt32& x, SInt32& y, SInt32& w, SInt32& h) const { x = 0; y = 0; w = 1920; h = 1080; }
    void getScreens(std::vector<ClientScreenInfo>&) const { }
    void getCursorPos(SInt32& x, SInt32& y) const { x = 960; y = 540; }
    void enter(SInt32, SInt32, UInt32, KeyModifierMask, bool) { }
    bool leave() { return true; }
    void setClipboard(ClipboardID, const IClipboard*) {}
    void grabClipboard(ClipboardID) {}
    void setClipboardDirty(ClipboardID, bool) {}
    void keyDown(KeyID, KeyModifierMask, KeyButton) {}
    void keyRepeat(KeyID, KeyModifierMask, SInt32, KeyButton) {}
    void keyUp(KeyID, KeyModifierMask, KeyButton) {}
    void mouseDown(ButtonID) {}
    void mouseUp(ButtonID) {}
    void mouseMove(SInt32, SInt32) {}
    void mouseRelativeMove(SInt32, SInt32) {}
    void mouseWheel(SInt32, SInt32) {}
    void screensaver(bool) {}
    void resetOptions() {}
    void setOptions(const OptionsList&) {}
    void sendDragInfo(UInt32, const char*, size_t) {}
    void fileChunkSending(UInt8, char*, size_t) {}
    KeyModifierMask getToggleMask() const { return 0; }
    bool isLockedToScreen() const { return false; }
    barrier::IStream* getStream() const { return NULL; }

    // the server's own screen: record what would be synthesized
    void injectKeyDown(KeyID id, KeyModifierMask, KeyButton b) { m_input.push_back({ "kd", (SInt32)id, (SInt32)b }); }
    void injectKeyRepeat(KeyID id, KeyModifierMask, SInt32, KeyButton b) { m_input.push_back({ "kr", (SInt32)id, (SInt32)b }); }
    void injectKeyUp(KeyButton b) { m_input.push_back({ "ku", 0, (SInt32)b }); }
    void injectMouseButton(ButtonID id, bool press) { m_input.push_back({ press ? "md" : "mu", (SInt32)id, 0 }); }
    void injectMouseMove(SInt32 x, SInt32 y) { m_input.push_back({ "mm", x, y }); }
    void injectMouseRelativeMove(SInt32 x, SInt32 y) { m_input.push_back({ "mr", x, y }); }
    void injectMouseWheel(SInt32 x, SInt32 y) { m_input.push_back({ "mw", x, y }); }

    std::string m_name;
    std::vector<Recorded> m_input;
};

class ServerRemoteControlTest : public ::testing::Test {
protected:
    ServerRemoteControlTest() :
        m_config(&m_events),
        m_rack("rack"),
        m_laptop("laptop"),
        m_desk("desk"),
        m_viewer("viewer")
    {
        m_config.addScreen("rack");
        m_config.addScreen("laptop");
        m_config.addScreen("desk");
        m_config.addScreen("viewer");
        m_config.addOption("laptop", kOptionRemoteControl, 1);
        m_config.addOption("desk", kOptionRemoteControl, 1);

        m_server.setEventsForTest(&m_events);
        m_server.setConfigForTest(&m_config);
        m_server.setPrimaryClientForTest(&m_rack);
        m_server.addClientForTest("rack", &m_rack);
        m_server.addClientForTest("laptop", &m_laptop);
        m_server.addClientForTest("desk", &m_desk);
        m_server.addClientForTest("viewer", &m_viewer);
        m_server.setActive(&m_rack);
        m_server.enableRemoteControlForTest();
        m_server.setRemoteControlClockForTest(100.0);
    }

    UInt32 grantLaptop()
    {
        m_server.onRemoteControlRequest(&m_laptop, false, "");
        EXPECT_FALSE(m_laptop.m_states.empty());
        if (m_laptop.m_states.empty()) {
            return 0;
        }
        EXPECT_EQ(kStateGranted, m_laptop.m_states.back().m_state);
        return m_laptop.m_states.back().m_epoch;
    }

    TestEventQueue  m_events;
    Config          m_config;
    RcFakePrimary   m_rack;
    RcFakeClient    m_laptop;
    RcFakeClient    m_desk;
    RcFakeClient    m_viewer;
    Server          m_server;
};

} // namespace

TEST_F(ServerRemoteControlTest, grantedClientInputReachesServerScreen)
{
    const UInt32 epoch = grantLaptop();
    ASSERT_NE(0u, epoch);
    EXPECT_EQ("rack", m_laptop.m_states.back().m_target);
    EXPECT_EQ(1920, m_laptop.m_states.back().m_w) << "controlled host geometry for coordinate mapping";
    EXPECT_EQ("laptop", m_server.getRemoteController());

    // other remote-control clients learn who controls
    ASSERT_FALSE(m_desk.m_states.empty());
    EXPECT_EQ(kStateControlledByOther, m_desk.m_states.back().m_state);
    EXPECT_EQ("laptop", m_desk.m_states.back().m_controller);

    m_server.onRemoteInput(&m_laptop, epoch, RemoteEvent::keyDown('a', 0, 38));
    m_server.onRemoteInput(&m_laptop, epoch, RemoteEvent::keyUp('a', 0, 38));
    m_server.onRemoteInput(&m_laptop, epoch, RemoteEvent::buttonDown(kButtonLeft));
    m_server.onRemoteInput(&m_laptop, epoch, RemoteEvent::moveRel(5, -3));
    m_server.onRemoteInput(&m_laptop, epoch, RemoteEvent::buttonUp(kButtonLeft));
    m_server.onRemoteInput(&m_laptop, epoch, RemoteEvent::moveAbs(5000, -10));
    m_server.onRemoteInput(&m_laptop, epoch, RemoteEvent::wheel(-120, 240));

    std::vector<Recorded> expected = {
        { "kd", 'a', 38 }, { "ku", 0, 38 }, { "md", kButtonLeft, 0 }, { "mr", 5, -3 },
        { "mu", kButtonLeft, 0 }, { "mm", 1919, 0 }, { "mw", -120, 240 } };
    EXPECT_EQ(expected, m_rack.m_input);
    EXPECT_TRUE(m_laptop.m_input.empty()) << "never fed back to the controller";
}

TEST_F(ServerRemoteControlTest, viewOnlyClientIsRefused)
{
    m_server.onRemoteControlRequest(&m_viewer, false, "");
    ASSERT_FALSE(m_viewer.m_states.empty());
    EXPECT_EQ(kStateDenied, m_viewer.m_states.back().m_state);
    EXPECT_EQ(kReasonNotPermitted, m_viewer.m_states.back().m_reason);

    m_server.onRemoteInput(&m_viewer, 0, RemoteEvent::keyDown('a', 0, 38));
    EXPECT_TRUE(m_rack.m_input.empty());
}

TEST_F(ServerRemoteControlTest, globalOptionGrantsAllAndScreenOptionOverrides)
{
    m_config.addOption("", kOptionRemoteControl, 1);
    m_config.removeOption("desk", kOptionRemoteControl);
    m_config.addOption("desk", kOptionRemoteControl, 0);
    m_server.onRemoteControlRequest(&m_desk, false, "");
    EXPECT_EQ(kReasonNotPermitted, m_desk.m_states.back().m_reason);
    m_server.onRemoteControlRequest(&m_viewer, false, "");
    EXPECT_EQ(kStateGranted, m_viewer.m_states.back().m_state);
}

TEST_F(ServerRemoteControlTest, staleEpochIsNotInjected)
{
    const UInt32 epoch = grantLaptop();
    m_server.onRemoteInput(&m_laptop, epoch + 7, RemoteEvent::keyDown('a', 0, 38));
    EXPECT_TRUE(m_rack.m_input.empty());
}

TEST_F(ServerRemoteControlTest, conflictingClientsAndTakeover)
{
    const UInt32 epoch = grantLaptop();
    m_server.onRemoteInput(&m_laptop, epoch, RemoteEvent::keyDown(kKeyControl_L, 0, 37));

    m_server.onRemoteControlRequest(&m_desk, false, "");
    EXPECT_EQ(kStateDenied, m_desk.m_states.back().m_state);
    EXPECT_EQ(kReasonBusy, m_desk.m_states.back().m_reason);
    EXPECT_EQ("laptop", m_desk.m_states.back().m_controller);

    const size_t before = m_laptop.m_states.size();
    m_server.onRemoteControlRequest(&m_desk, true, "");
    EXPECT_EQ(kStateGranted, m_desk.m_states.back().m_state);
    ASSERT_GT(m_laptop.m_states.size(), before);
    EXPECT_EQ(kStateRevoked, m_laptop.m_states[before].m_state);
    EXPECT_EQ(kReasonTakenOver, m_laptop.m_states[before].m_reason);
    EXPECT_EQ(kStateControlledByOther, m_laptop.m_states.back().m_state);
    EXPECT_EQ("desk", m_laptop.m_states.back().m_controller);
    // laptop's Ctrl was released on the server before desk took over
    ASSERT_EQ(2u, m_rack.m_input.size());
    EXPECT_EQ("ku", m_rack.m_input[1].m_what);
    EXPECT_EQ(37, m_rack.m_input[1].m_b);

    // the laptop can no longer type
    m_server.onRemoteInput(&m_laptop, epoch, RemoteEvent::keyDown('x', 0, 53));
    EXPECT_EQ(2u, m_rack.m_input.size());
}

TEST_F(ServerRemoteControlTest, controllerDisconnectReleasesHeldInput)
{
    const UInt32 epoch = grantLaptop();
    m_server.onRemoteInput(&m_laptop, epoch, RemoteEvent::keyDown(kKeyShift_L, 0, 50));
    m_server.onRemoteInput(&m_laptop, epoch, RemoteEvent::buttonDown(kButtonLeft));
    EXPECT_TRUE(m_server.getRemoteControlForTest()->held().anyButtonDown());

    m_server.removeClientForTest(&m_laptop);
    ASSERT_EQ(4u, m_rack.m_input.size());
    EXPECT_EQ("mu", m_rack.m_input[2].m_what);
    EXPECT_EQ("ku", m_rack.m_input[3].m_what);
    EXPECT_EQ("", m_server.getRemoteController());
    EXPECT_EQ(kStateIdle, m_desk.m_states.back().m_state);
}

TEST_F(ServerRemoteControlTest, crashedClientTimesOut)
{
    const UInt32 epoch = grantLaptop();
    m_server.onRemoteInput(&m_laptop, epoch, RemoteEvent::keyDown(kKeyAlt_L, 0, 64));
    m_server.remoteControlTickForTest(102.0);
    EXPECT_EQ(1u, m_rack.m_input.size());

    // no message for longer than the lease: Alt is released
    m_server.remoteControlTickForTest(103.5);
    ASSERT_EQ(2u, m_rack.m_input.size());
    EXPECT_EQ("ku", m_rack.m_input[1].m_what);

    m_server.remoteControlTickForTest(120.0);
    EXPECT_EQ(kStateRevoked, m_laptop.m_states.back().m_state);
    EXPECT_EQ(kReasonTimeout, m_laptop.m_states.back().m_reason);
    EXPECT_EQ("", m_server.getRemoteController());
}

TEST_F(ServerRemoteControlTest, releaseByControllerFreesTheHost)
{
    const UInt32 epoch = grantLaptop();
    m_server.onRemoteInput(&m_laptop, epoch, RemoteEvent::keyDown(kKeyControl_L, 0, 37));
    m_server.onRemoteControlRelease(&m_laptop, epoch);
    EXPECT_EQ(kStateReleased, m_laptop.m_states.back().m_state);
    ASSERT_EQ(2u, m_rack.m_input.size());
    EXPECT_EQ("ku", m_rack.m_input[1].m_what);
    EXPECT_EQ("", m_server.getRemoteController());
}

TEST_F(ServerRemoteControlTest, switchingHostsReleasesOnOldHostAndRoutesToNew)
{
    const UInt32 epoch = grantLaptop();
    m_server.onRemoteInput(&m_laptop, epoch, RemoteEvent::keyDown(kKeyShift_L, 0, 50));

    m_server.onRemoteControlRequest(&m_laptop, false, "desk");
    ASSERT_EQ(kStateGranted, m_laptop.m_states.back().m_state);
    EXPECT_EQ("desk", m_laptop.m_states.back().m_target);
    EXPECT_EQ(&m_desk, m_server.getActiveClientForTest());
    EXPECT_TRUE(m_desk.m_entered);
    // shift released on the rack before leaving it
    ASSERT_EQ(2u, m_rack.m_input.size());
    EXPECT_EQ("ku", m_rack.m_input[1].m_what);

    const UInt32 epoch2 = m_laptop.m_states.back().m_epoch;
    m_server.onRemoteInput(&m_laptop, epoch2, RemoteEvent::keyDown('z', 0, 52));
    m_server.onRemoteInput(&m_laptop, epoch2, RemoteEvent::wheel(120, -120));
    std::vector<Recorded> expected = { { "kd", 'z', 52 }, { "mw", 0, -120 }, { "mw", 120, 0 } };
    EXPECT_EQ(expected, m_desk.m_input);
    EXPECT_EQ(2u, m_rack.m_input.size());

    // and back to the server
    m_server.onRemoteControlRequest(&m_laptop, false, "");
    EXPECT_EQ(&m_rack, m_server.getActiveClientForTest());
    ASSERT_FALSE(m_desk.m_input.empty());
    EXPECT_EQ("ku", m_desk.m_input.back().m_what) << "z released on desk";
}

TEST_F(ServerRemoteControlTest, clientCannotControlItselfAndServerNeverSwitchesToController)
{
    m_server.onRemoteControlRequest(&m_laptop, false, "laptop");
    EXPECT_EQ(kReasonTargetIsSelf, m_laptop.m_states.back().m_reason);

    m_server.onRemoteControlRequest(&m_laptop, false, "nosuchhost");
    EXPECT_EQ(kReasonUnknownTarget, m_laptop.m_states.back().m_reason);

    grantLaptop();
    m_server.jumpToScreenForTest(&m_laptop);
    EXPECT_EQ(&m_rack, m_server.getActiveClientForTest()) << "no input loop back to the controller";
    EXPECT_FALSE(m_laptop.m_entered);

    // other hosts are still reachable (existing KVM switching)
    m_server.jumpToScreenForTest(&m_desk);
    EXPECT_EQ(&m_desk, m_server.getActiveClientForTest());
    EXPECT_EQ("desk", m_laptop.m_states.back().m_target);
    EXPECT_EQ(kReasonTargetChanged, m_laptop.m_states.back().m_reason);
}

TEST_F(ServerRemoteControlTest, controlledHostDisconnectFallsBackToServer)
{
    grantLaptop();
    m_server.onRemoteControlRequest(&m_laptop, false, "desk");
    const UInt32 epoch = m_laptop.m_states.back().m_epoch;
    m_server.onRemoteInput(&m_laptop, epoch, RemoteEvent::keyDown('q', 0, 24));

    m_server.removeClientForTest(&m_desk);
    // removeClient alone does not move focus in the test harness; the
    // held state of the vanished host must be gone though
    ASSERT_NE(nullptr, m_server.getRemoteControlForTest());
    EXPECT_TRUE(m_server.getRemoteControlForTest()->held().empty());
}
