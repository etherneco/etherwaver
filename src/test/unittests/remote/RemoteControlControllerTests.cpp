/*
 * Etherwaver -- remote control unit tests: client state machine.
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "client/RemoteControlController.h"

#include "test/global/gtest.h"

using namespace barrier::remote;
typedef barrier::remote::Event RemoteEvent;

namespace {

class FakeTransport : public IRemoteControlTransport {
public:
    bool isConnectedToServer() const { return m_connected; }
    bool isRemoteControlSupported() const { return m_connected && m_supports; }
    void sendRemoteControlRequest(bool takeover, const std::string& target)
    {
        m_requests.push_back(target + (takeover ? "!" : ""));
    }
    void sendRemoteControlRelease(UInt32 epoch) { m_releases.push_back(epoch); }
    void sendRemoteInput(UInt32 epoch, const RemoteEvent& event)
    {
        m_sentEpochs.push_back(epoch);
        if (event.m_kind == kEventHeartbeat) {
            ++m_heartbeats;
        }
        else {
            m_sent.push_back(event);
        }
    }
    std::string serverName() const { return "rack"; }

    bool m_connected = true;
    bool m_supports = true;
    std::vector<std::string> m_requests;
    std::vector<UInt32> m_releases;
    std::vector<RemoteEvent> m_sent;
    std::vector<UInt32> m_sentEpochs;
    int m_heartbeats = 0;
};

class FakeCapture : public ILocalInputCapture {
public:
    std::string name() const { return "fake"; }
    bool isAvailable(std::string& why) const { why = m_why; return m_available; }
    bool supportsMode(Mode mode) const { return mode == kModeCapture || m_window; }
    bool start(Mode mode, const Config& config, Sink* sink, std::string& error)
    {
        if (m_failStart) {
            error = "grab refused";
            return false;
        }
        m_mode = mode;
        m_config = config;
        m_sink = sink;
        m_capturing = true;
        ++m_starts;
        return true;
    }
    void stop() { if (m_capturing) ++m_stops; m_capturing = false; }
    void updateTarget(const Config& config) { m_config = config; ++m_updates; }
    bool isCapturing() const { return m_capturing; }

    bool m_available = true;
    bool m_window = true;
    bool m_failStart = false;
    std::string m_why;
    bool m_capturing = false;
    int m_starts = 0, m_stops = 0, m_updates = 0;
    Mode m_mode = kModeCapture;
    Config m_config;
    Sink* m_sink = nullptr;
};

class ControllerTest : public ::testing::Test {
protected:
    ControllerTest() : m_controller(&m_transport, Hotkey::defaultRelease())
    {
        m_capture = new FakeCapture;
        m_controller.setCapture(std::unique_ptr<ILocalInputCapture>(m_capture));
        m_controller.setWakeup([this]() { ++m_wakes; });
    }

    UInt32 startAndGrant(ILocalInputCapture::Mode mode = ILocalInputCapture::kModeCapture)
    {
        std::string error;
        EXPECT_TRUE(m_controller.start(mode, "", false, error)) << error;
        EXPECT_EQ(RemoteControlController::kRequesting, m_controller.state());
        EXPECT_EQ(0, m_capture->m_starts) << "nothing is captured before the grant";
        m_controller.onServerState(kStateGranted, kReasonNone, 77, 0, 0, 1920, 1080,
                                   kCapAbsolute | kCapRelative, "laptop", "rack");
        return 77;
    }

    // what the capture thread would deliver, then the main thread drains
    void capture(const RemoteEvent& e)
    {
        m_capture->m_sink->onCaptureEvent(e);
    }

    FakeTransport m_transport;
    FakeCapture* m_capture;
    RemoteControlController m_controller;
    int m_wakes = 0;
};

} // namespace

TEST_F(ControllerTest, startsOnlyOnExplicitRequestAndNeverOnConnect)
{
    m_controller.onConnected();
    EXPECT_EQ(RemoteControlController::kLocal, m_controller.state());
    EXPECT_TRUE(m_transport.m_requests.empty());
    EXPECT_FALSE(m_controller.isForwarding());
}

TEST_F(ControllerTest, grantStartsCaptureAndForwardsInOrder)
{
    const UInt32 epoch = startAndGrant();
    EXPECT_EQ(RemoteControlController::kRemote, m_controller.state());
    EXPECT_EQ(1, m_capture->m_starts);
    EXPECT_EQ("rack", m_capture->m_config.m_targetName);
    EXPECT_EQ("Ctrl+Alt+Shift+Q", m_capture->m_config.m_releaseHotkey.toString());
    EXPECT_TRUE(m_controller.isForwarding()) << "server input is ignored while forwarding";

    capture(RemoteEvent::keyDown(kKeyShift_L, 0, 50));
    capture(RemoteEvent::keyDown('A', KeyModifierShift, 38));
    capture(RemoteEvent::keyUp('A', KeyModifierShift, 38));
    capture(RemoteEvent::keyUp(kKeyShift_L, KeyModifierShift, 50));
    capture(RemoteEvent::moveRel(1, 1));
    capture(RemoteEvent::moveRel(2, 2));
    EXPECT_GT(m_wakes, 0);
    m_controller.processPending();

    ASSERT_EQ(5u, m_transport.m_sent.size());
    EXPECT_EQ(kEventKeyDown, m_transport.m_sent[0].m_kind);
    EXPECT_EQ(kEventKeyDown, m_transport.m_sent[1].m_kind);
    EXPECT_EQ(kEventKeyUp, m_transport.m_sent[2].m_kind);
    EXPECT_EQ(kEventKeyUp, m_transport.m_sent[3].m_kind);
    EXPECT_EQ(RemoteEvent::moveRel(3, 3), m_transport.m_sent[4]) << "motion coalesced";
    for (size_t i = 0; i < m_transport.m_sentEpochs.size(); ++i) {
        EXPECT_EQ(epoch, m_transport.m_sentEpochs[i]);
    }
}

TEST_F(ControllerTest, releaseHotkeyEndsCaptureAndReleasesRemoteKeys)
{
    const UInt32 epoch = startAndGrant();
    // user presses Ctrl+Alt+Shift (forwarded) then Q (swallowed by capture)
    capture(RemoteEvent::keyDown(kKeyControl_L, 0, 37));
    capture(RemoteEvent::keyDown(kKeyAlt_L, KeyModifierControl, 64));
    capture(RemoteEvent::keyDown(kKeyShift_L, KeyModifierControl | KeyModifierAlt, 50));
    capture(RemoteEvent::buttonDown(kButtonLeft));
    m_capture->m_sink->onCaptureReleaseHotkey();
    m_controller.processPending();

    EXPECT_EQ(RemoteControlController::kLocal, m_controller.state());
    EXPECT_FALSE(m_capture->m_capturing) << "local cursor and input restored";
    EXPECT_EQ(1, m_capture->m_stops);
    // 4 presses then 4 releases, then the grant is released
    ASSERT_EQ(8u, m_transport.m_sent.size());
    EXPECT_EQ(RemoteEvent::buttonUp(kButtonLeft), m_transport.m_sent[4]);
    for (size_t i = 5; i < 8; ++i) {
        EXPECT_EQ(kEventKeyUp, m_transport.m_sent[i].m_kind);
    }
    ASSERT_EQ(1u, m_transport.m_releases.size());
    EXPECT_EQ(epoch, m_transport.m_releases[0]);
    EXPECT_EQ("hotkey", m_controller.status().m_lastEvent);
    EXPECT_FALSE(m_controller.isForwarding());
}

TEST_F(ControllerTest, releaseButtonEndsCapture)
{
    startAndGrant();
    capture(RemoteEvent::keyDown('a', 0, 38));
    m_controller.processPending();
    m_controller.stop("user", "released");
    EXPECT_EQ(RemoteControlController::kLocal, m_controller.state());
    EXPECT_FALSE(m_capture->m_capturing);
    ASSERT_EQ(2u, m_transport.m_sent.size());
    EXPECT_EQ(kEventKeyUp, m_transport.m_sent[1].m_kind);
    EXPECT_EQ(1u, m_transport.m_releases.size());
}

TEST_F(ControllerTest, connectionLossStopsCaptureWithoutSending)
{
    startAndGrant();
    capture(RemoteEvent::keyDown(kKeyControl_L, 0, 37));
    m_controller.processPending();
    const size_t sent = m_transport.m_sent.size();

    m_transport.m_connected = false;
    m_controller.onDisconnected();
    EXPECT_EQ(RemoteControlController::kLocal, m_controller.state());
    EXPECT_FALSE(m_capture->m_capturing);
    EXPECT_EQ(sent, m_transport.m_sent.size());
    EXPECT_EQ("connection-lost", m_controller.status().m_lastEvent);

    // reconnect: nothing pending is replayed and capture does not resume
    capture(RemoteEvent::keyDown('a', 0, 38));
    m_transport.m_connected = true;
    m_controller.onConnected();
    m_controller.processPending();
    m_controller.tick();
    EXPECT_EQ(sent, m_transport.m_sent.size());
    EXPECT_EQ(RemoteControlController::kLocal, m_controller.state());
    EXPECT_EQ(0, m_transport.m_heartbeats - 0) << "no heartbeat after the session ended";
}

TEST_F(ControllerTest, suspendReleasesInput)
{
    startAndGrant();
    m_controller.onSuspend();
    EXPECT_EQ(RemoteControlController::kLocal, m_controller.state());
    EXPECT_FALSE(m_capture->m_capturing);
    EXPECT_EQ(1u, m_transport.m_releases.size());
}

TEST_F(ControllerTest, captureLossEndsRemoteControl)
{
    startAndGrant();
    m_capture->m_sink->onCaptureLost("system sleep detected");
    m_controller.processPending();
    EXPECT_EQ(RemoteControlController::kLocal, m_controller.state());
    EXPECT_EQ("capture-lost", m_controller.status().m_lastEvent);
}

TEST_F(ControllerTest, revokeAndTakeoverByServerStopCapture)
{
    startAndGrant();
    m_controller.onServerState(kStateRevoked, kReasonTakenOver, 0, 0, 0, 0, 0, 0, "desk", "rack");
    EXPECT_EQ(RemoteControlController::kLocal, m_controller.state());
    EXPECT_FALSE(m_capture->m_capturing);
    EXPECT_TRUE(m_transport.m_releases.empty()) << "server already ended the grant";
    EXPECT_EQ("desk", m_controller.status().m_controller);
}

TEST_F(ControllerTest, deniedViewOnly)
{
    std::string error;
    ASSERT_TRUE(m_controller.start(ILocalInputCapture::kModeCapture, "", false, error));
    m_controller.onServerState(kStateDenied, kReasonNotPermitted, 0, 0, 0, 0, 0, 0, "", "");
    EXPECT_EQ(RemoteControlController::kLocal, m_controller.state());
    EXPECT_TRUE(m_controller.status().m_viewOnly);
    EXPECT_EQ(0, m_capture->m_starts);
}

TEST_F(ControllerTest, missingPermissionOrServerSupportIsReported)
{
    std::string error;
    m_capture->m_available = false;
    m_capture->m_why = "no permission to read /dev/input/event*";
    EXPECT_FALSE(m_controller.start(ILocalInputCapture::kModeCapture, "", false, error));
    EXPECT_NE(std::string::npos, error.find("/dev/input"));
    EXPECT_EQ("capture-unavailable", m_controller.status().m_lastEvent);

    m_capture->m_available = true;
    m_transport.m_supports = false;
    EXPECT_FALSE(m_controller.start(ILocalInputCapture::kModeCapture, "", false, error));
    EXPECT_EQ("server-unsupported", m_controller.status().m_lastEvent);

    m_transport.m_connected = false;
    EXPECT_FALSE(m_controller.start(ILocalInputCapture::kModeCapture, "", false, error));
    EXPECT_TRUE(m_transport.m_requests.empty());
}

TEST_F(ControllerTest, failedGrabReleasesTheGrant)
{
    m_capture->m_failStart = true;
    startAndGrant();
    EXPECT_EQ(RemoteControlController::kLocal, m_controller.state());
    ASSERT_EQ(1u, m_transport.m_releases.size());
    EXPECT_EQ(77u, m_transport.m_releases[0]);
    EXPECT_EQ("capture-failed", m_controller.status().m_lastEvent);
}

TEST_F(ControllerTest, lateGrantAfterCancelIsReturned)
{
    std::string error;
    ASSERT_TRUE(m_controller.start(ILocalInputCapture::kModeCapture, "", false, error));
    m_controller.stop("user", "cancelled");
    m_controller.onServerState(kStateGranted, kReasonNone, 9, 0, 0, 100, 100, 0, "laptop", "rack");
    EXPECT_EQ(RemoteControlController::kLocal, m_controller.state());
    EXPECT_EQ(0, m_capture->m_starts);
    ASSERT_EQ(1u, m_transport.m_releases.size());
    EXPECT_EQ(9u, m_transport.m_releases[0]);
}

TEST_F(ControllerTest, windowModeSuspendsOnFocusLoss)
{
    startAndGrant(ILocalInputCapture::kModeWindow);
    EXPECT_EQ(RemoteControlController::kSuspended, m_controller.state())
        << "window mode waits for the window focus";
    capture(RemoteEvent::keyDown('a', 0, 38));
    m_controller.processPending();
    EXPECT_TRUE(m_transport.m_sent.empty()) << "nothing forwarded without focus";

    m_capture->m_sink->onCaptureFocus(true);
    m_controller.processPending();
    EXPECT_EQ(RemoteControlController::kRemote, m_controller.state());
    capture(RemoteEvent::moveAbs(100, 200));
    capture(RemoteEvent::keyDown(kKeyControl_L, 0, 37));
    m_capture->m_sink->onCaptureFocus(false);
    m_controller.processPending();
    EXPECT_EQ(RemoteControlController::kSuspended, m_controller.state());
    ASSERT_EQ(3u, m_transport.m_sent.size());
    EXPECT_EQ(RemoteEvent::moveAbs(100, 200), m_transport.m_sent[0]);
    EXPECT_EQ(kEventKeyUp, m_transport.m_sent[2].m_kind) << "ctrl released on focus loss";

    // the lease is kept alive while paused
    m_controller.tick();
    EXPECT_EQ(1, m_transport.m_heartbeats);
}

TEST_F(ControllerTest, unmatchedReleasesAreNotForwarded)
{
    startAndGrant();
    // the key that triggered the start was pressed before the grab
    capture(RemoteEvent::keyUp(kKeyReturn, 0, 36));
    capture(RemoteEvent::buttonUp(kButtonLeft));
    m_controller.processPending();
    EXPECT_TRUE(m_transport.m_sent.empty());
}

TEST_F(ControllerTest, targetChangeResyncsHeldState)
{
    startAndGrant();
    capture(RemoteEvent::keyDown(kKeyShift_L, 0, 50));
    m_controller.processPending();
    m_controller.onServerState(kStateGranted, kReasonTargetChanged, 78, 0, 0, 1280, 720, 0,
                               "laptop", "desk");
    EXPECT_EQ(78u, m_controller.epoch());
    EXPECT_EQ(1, m_capture->m_updates);
    EXPECT_EQ("desk", m_capture->m_config.m_targetName);
    EXPECT_EQ(1280, m_capture->m_config.m_targetW);
    // server already released shift on the old host
    capture(RemoteEvent::keyUp(kKeyShift_L, KeyModifierShift, 50));
    m_controller.processPending();
    EXPECT_EQ(1u, m_transport.m_sent.size());
}

TEST_F(ControllerTest, queueOverflowStopsSafely)
{
    startAndGrant();
    // more key presses than the queue holds, without draining
    for (int i = 0; i < 2000; ++i) {
        capture(RemoteEvent::keyDown('a', 0, (KeyButton)(i % 400 + 8)));
        capture(RemoteEvent::keyUp('a', 0, (KeyButton)(i % 400 + 8)));
    }
    m_controller.processPending();
    EXPECT_EQ(RemoteControlController::kLocal, m_controller.state());
    EXPECT_EQ("queue-overflow", m_controller.status().m_lastEvent);
    EXPECT_FALSE(m_capture->m_capturing);
}

TEST_F(ControllerTest, ctrlAltDelIsAnExplicitBalancedAction)
{
    std::string error;
    EXPECT_FALSE(m_controller.sendCtrlAltDel(error));
    startAndGrant();
    ASSERT_TRUE(m_controller.sendCtrlAltDel(error));
    ASSERT_EQ(6u, m_transport.m_sent.size());
    int downs = 0, ups = 0;
    for (size_t i = 0; i < 6; ++i) {
        downs += m_transport.m_sent[i].m_kind == kEventKeyDown;
        ups   += m_transport.m_sent[i].m_kind == kEventKeyUp;
    }
    EXPECT_EQ(3, downs);
    EXPECT_EQ(3, ups);
    EXPECT_EQ((SInt32)kKeyDelete, m_transport.m_sent[2].m_a);
}

TEST_F(ControllerTest, statusJsonHasNoKeyContent)
{
    startAndGrant();
    capture(RemoteEvent::keyDown('p', 0, 33));
    m_controller.processPending();
    const std::string json = RemoteControlController::statusToJson(m_controller.status());
    EXPECT_NE(std::string::npos, json.find("\"state\":\"remote\""));
    EXPECT_NE(std::string::npos, json.find("\"releaseHotkey\":\"Ctrl+Alt+Shift+Q\""));
    EXPECT_NE(std::string::npos, json.find("\"forwarded\":1"));
    EXPECT_EQ(std::string::npos, json.find("\"p\""));
}
