#define BARRIER_TEST_ENV

#include "server/Server.h"
#include "server/BaseClientProxy.h"
#include "server/PrimaryClient.h"
#include "core/layout/ScreenManager.h"

#include "test/global/gtest.h"
#include "test/global/TestEventQueue.h"

#include <chrono>
#include <thread>

namespace {

using etherwaver::layout::Screen;
using etherwaver::layout::ScreenManager;
using etherwaver::server::findLayoutScreenForPositionForTest;
using etherwaver::server::getJumpCursorPosForLayoutScreenForTest;
using etherwaver::server::resolveObjectLayoutDestinationForTest;
using etherwaver::server::resolveObjectLayoutTargetForTest;
using etherwaver::server::selectClientScreenForLayoutScreenForTest;

class FakeClientProxy : public BaseClientProxy {
public:
    FakeClientProxy(const std::string& name,
                    const std::vector<ClientScreenInfo>& screens,
                    SInt32 x, SInt32 y, SInt32 w, SInt32 h)
        : BaseClientProxy(name)
        , m_screens(screens)
        , m_shapeX(x)
        , m_shapeY(y)
        , m_cursorX(x)
        , m_cursorY(y)
        , m_w(w)
        , m_h(h)
        , m_entered(false)
        , m_lastEnterX(0)
        , m_lastEnterY(0)
    {
    }

    void* getEventTarget() const { return NULL; }
    bool getClipboard(ClipboardID, IClipboard*) const { return false; }
    void getShape(SInt32& x, SInt32& y, SInt32& w, SInt32& h) const
    {
        x = m_shapeX;
        y = m_shapeY;
        w = m_w;
        h = m_h;
    }
    void getScreens(std::vector<ClientScreenInfo>& screens) const { screens = m_screens; }
    void getCursorPos(SInt32& x, SInt32& y) const { x = m_cursorX; y = m_cursorY; }
    void enter(SInt32 x, SInt32 y, UInt32, KeyModifierMask, bool)
    {
        m_entered = true;
        m_lastEnterX = x;
        m_lastEnterY = y;
        m_cursorX = x;
        m_cursorY = y;
    }
    bool leave() { return true; }
    void setClipboard(ClipboardID, const IClipboard*) {}
    void grabClipboard(ClipboardID) {}
    void setClipboardDirty(ClipboardID, bool) {}
    void keyDown(KeyID, KeyModifierMask, KeyButton) {}
    void keyRepeat(KeyID, KeyModifierMask, SInt32, KeyButton) {}
    void keyUp(KeyID, KeyModifierMask, KeyButton) {}
    void mouseDown(ButtonID) {}
    void mouseUp(ButtonID) {}
    void mouseMove(SInt32 x, SInt32 y)
    {
        m_mouseMoves.push_back(std::make_pair(x, y));
        m_cursorX = x;
        m_cursorY = y;
    }
    void mouseRelativeMove(SInt32, SInt32) {}
    void mouseWheel(SInt32, SInt32) {}
    void screensaver(bool) {}
    void resetOptions() {}
    void setOptions(const OptionsList&) {}
    void sendDragInfo(UInt32, const char*, size_t) {}
    void fileChunkSending(UInt8, char*, size_t) {}
    barrier::IStream* getStream() const { return NULL; }

    bool m_entered;
    SInt32 m_lastEnterX;
    SInt32 m_lastEnterY;
    std::vector<std::pair<SInt32, SInt32> > m_mouseMoves;

private:
    std::vector<ClientScreenInfo> m_screens;
    SInt32 m_shapeX;
    SInt32 m_shapeY;
    mutable SInt32 m_cursorX;
    mutable SInt32 m_cursorY;
    SInt32 m_w;
    SInt32 m_h;
};

class FakePrimaryClient : public PrimaryClient {
public:
    FakePrimaryClient(const std::string& name,
                      const std::vector<ClientScreenInfo>& screens,
                      SInt32 x, SInt32 y, SInt32 w, SInt32 h)
        : PrimaryClient()
        , m_name(name)
        , m_screens(screens)
        , m_shapeX(x)
        , m_shapeY(y)
        , m_cursorX(x)
        , m_cursorY(y)
        , m_w(w)
        , m_h(h)
        , m_entered(false)
        , m_lastEnterX(0)
        , m_lastEnterY(0)
    {
    }

    std::string getName() const { return m_name; }
    void* getEventTarget() const { return NULL; }
    bool getClipboard(ClipboardID, IClipboard*) const { return false; }
    void getShape(SInt32& x, SInt32& y, SInt32& w, SInt32& h) const
    {
        x = m_shapeX;
        y = m_shapeY;
        w = m_w;
        h = m_h;
    }
    void getScreens(std::vector<ClientScreenInfo>& screens) const { screens = m_screens; }
    void getCursorPos(SInt32& x, SInt32& y) const { x = m_cursorX; y = m_cursorY; }
    void enter(SInt32 x, SInt32 y, UInt32, KeyModifierMask, bool)
    {
        m_entered = true;
        m_lastEnterX = x;
        m_lastEnterY = y;
        m_cursorX = x;
        m_cursorY = y;
    }
    bool leave() { return true; }
    void setClipboard(ClipboardID, const IClipboard*) {}
    void grabClipboard(ClipboardID) {}
    void setClipboardDirty(ClipboardID, bool) {}
    void keyDown(KeyID, KeyModifierMask, KeyButton) {}
    void keyRepeat(KeyID, KeyModifierMask, SInt32, KeyButton) {}
    void keyUp(KeyID, KeyModifierMask, KeyButton) {}
    void mouseDown(ButtonID) {}
    void mouseUp(ButtonID) {}
    void mouseMove(SInt32 x, SInt32 y)
    {
        m_mouseMoves.push_back(std::make_pair(x, y));
        m_cursorX = x;
        m_cursorY = y;
    }
    void mouseRelativeMove(SInt32, SInt32) {}
    void mouseWheel(SInt32, SInt32) {}
    void screensaver(bool) {}
    void resetOptions() {}
    void setOptions(const OptionsList&) {}
    void sendDragInfo(UInt32, const char*, size_t) {}
    void fileChunkSending(UInt8, char*, size_t) {}
    KeyModifierMask getToggleMask() const { return 0; }
    barrier::IStream* getStream() const { return NULL; }

    std::string m_name;
    std::vector<ClientScreenInfo> m_screens;
    SInt32 m_shapeX;
    SInt32 m_shapeY;
    mutable SInt32 m_cursorX;
    mutable SInt32 m_cursorY;
    SInt32 m_w;
    SInt32 m_h;
    bool m_entered;
    SInt32 m_lastEnterX;
    SInt32 m_lastEnterY;
    std::vector<std::pair<SInt32, SInt32> > m_mouseMoves;
};

TEST(ServerLayoutMappingTests, selectClientScreenForLayoutPrefersGeometryWhenNamesAreSwapped)
{
    ScreenManager layout;
    std::vector<Screen> layoutScreens;
    layoutScreens.push_back(Screen("mamre:mamre-2", "mamre", "mamre-2",
                                   518, 42, 240, 140));
    layoutScreens.push_back(Screen("mamre:mamre-1", "mamre", "mamre-1",
                                   62, 57, 240, 140));
    layoutScreens.push_back(Screen("Siloe:Siloe-1", "Siloe", "Siloe-1",
                                   293, 43, 240, 140));
    layout.setScreens(layoutScreens);

    std::vector<ClientScreenInfo> clientScreens;
    clientScreens.push_back(ClientScreenInfo("mamre-1", 3520, 0, 1920, 3240));
    clientScreens.push_back(ClientScreenInfo("mamre-2", 0, 0, 3200, 1800));

    SInt32 x = 0;
    SInt32 y = 0;
    SInt32 w = 0;
    SInt32 h = 0;

    ASSERT_TRUE(selectClientScreenForLayoutScreenForTest(
        layout, clientScreens, layoutScreens[0], x, y, w, h));
    EXPECT_EQ(3520, x);
    EXPECT_EQ(0, y);
    EXPECT_EQ(1920, w);
    EXPECT_EQ(3240, h);

    ASSERT_TRUE(selectClientScreenForLayoutScreenForTest(
        layout, clientScreens, layoutScreens[1], x, y, w, h));
    EXPECT_EQ(0, x);
    EXPECT_EQ(0, y);
    EXPECT_EQ(3200, w);
    EXPECT_EQ(1800, h);
}

TEST(ServerLayoutMappingTests, selectClientScreenForLayoutUsesNameWhenOnlyOneScreenExists)
{
    ScreenManager layout;
    std::vector<Screen> layoutScreens;
    layoutScreens.push_back(Screen("mamre:mamre-2", "mamre", "mamre-2",
                                   518, 42, 240, 140));
    layout.setScreens(layoutScreens);

    std::vector<ClientScreenInfo> clientScreens;
    clientScreens.push_back(ClientScreenInfo("mamre-2", 0, 0, 3200, 1800));

    SInt32 x = 0;
    SInt32 y = 0;
    SInt32 w = 0;
    SInt32 h = 0;

    ASSERT_TRUE(selectClientScreenForLayoutScreenForTest(
        layout, clientScreens, layoutScreens[0], x, y, w, h));
    EXPECT_EQ(0, x);
    EXPECT_EQ(0, y);
    EXPECT_EQ(3200, w);
    EXPECT_EQ(1800, h);
}

TEST(ServerLayoutMappingTests, selectClientScreenForCrossHostLinkPrefersExactMonitorName)
{
    ScreenManager layout;
    std::vector<Screen> layoutScreens;
    layoutScreens.push_back(Screen("KANAAN:KANAAN-1", "KANAAN", "KANAAN-1",
                                   71, 32, 240, 140));
    layoutScreens.back().m_rightLink = "Siloe-1";
    layoutScreens.push_back(Screen("Siloe:Siloe-1", "Siloe", "Siloe-1",
                                   293, 43, 240, 140));
    layoutScreens.back().m_leftLink = "KANAAN-1";
    layoutScreens.back().m_rightLink = "mamre-2";
    layoutScreens.push_back(Screen("mamre:mamre-2", "mamre", "mamre-2",
                                   518, 42, 240, 140));
    layoutScreens.back().m_leftLink = "Siloe-1";
    layoutScreens.push_back(Screen("mamre:mamre-1", "mamre", "mamre-1",
                                   522, 167, 240, 140));
    layout.setScreens(layoutScreens);

    std::vector<ClientScreenInfo> clientScreens;
    clientScreens.push_back(ClientScreenInfo("mamre-1", 0, 0, 1920, 1080));
    clientScreens.push_back(ClientScreenInfo("mamre-2", 1920, 0, 1920, 1080));

    SInt32 x = 0;
    SInt32 y = 0;
    SInt32 w = 0;
    SInt32 h = 0;

    ASSERT_TRUE(selectClientScreenForLayoutScreenForTest(
        layout, clientScreens, layoutScreens[2], x, y, w, h));
    EXPECT_EQ(1920, x);
    EXPECT_EQ(0, y);
    EXPECT_EQ(1920, w);
    EXPECT_EQ(1080, h);
}

TEST(ServerLayoutMappingTests, jumpPositionForLogicalScreenMovesIntoTargetMonitorWhenSavedPositionIsOnAnotherMonitor)
{
    ScreenManager layout;
    std::vector<Screen> layoutScreens;
    layoutScreens.push_back(Screen("mamre:mamre-2", "mamre", "mamre-2",
                                   518, 42, 240, 140));
    layoutScreens.push_back(Screen("mamre:mamre-1", "mamre", "mamre-1",
                                   62, 57, 240, 140));
    layout.setScreens(layoutScreens);

    std::vector<ClientScreenInfo> clientScreens;
    clientScreens.push_back(ClientScreenInfo("mamre-1", 3520, 0, 1920, 3240));
    clientScreens.push_back(ClientScreenInfo("mamre-2", 0, 0, 3200, 1800));

    FakeClientProxy client("mamre", clientScreens, 0, 0, 5440, 3240);
    client.setJumpCursorPos(100, 1200);

    SInt32 x = 0;
    SInt32 y = 0;
    getJumpCursorPosForLayoutScreenForTest(layout, &client, layoutScreens[0], x, y);

    EXPECT_EQ(4480, x);
    EXPECT_EQ(1620, y);
}

TEST(ServerLayoutMappingTests, findLayoutScreenForPositionCanMisidentifyScreenWhenMonitorsAreRemapped)
{
    ScreenManager layout;
    std::vector<Screen> layoutScreens;
    layoutScreens.push_back(Screen("mamre:mamre-2", "mamre", "mamre-2",
                                   518, 42, 240, 140));
    layoutScreens.push_back(Screen("mamre:mamre-1", "mamre", "mamre-1",
                                   62, 57, 240, 140));
    layout.setScreens(layoutScreens);

    const Screen* resolved = findLayoutScreenForPositionForTest(
        layout, "mamre",
        3520, 0, 1920, 3240,
        3520, 1200);

    ASSERT_NE(static_cast<const Screen*>(NULL), resolved);
    EXPECT_EQ("mamre:mamre-1", resolved->m_id);
}

TEST(ServerLayoutMappingTests, remoteHostShouldNotInferLogicalScreenFromCursorPosition)
{
    ScreenManager layout;
    std::vector<Screen> layoutScreens;
    layoutScreens.push_back(Screen("mamre:mamre-2", "mamre", "mamre-2",
                                   518, 42, 240, 140));
    layoutScreens.push_back(Screen("mamre:mamre-1", "mamre", "mamre-1",
                                   62, 57, 240, 140));
    layout.setScreens(layoutScreens);

    const Screen* resolved = findLayoutScreenForPositionForTest(
        layout, "mamre",
        3520, 0, 1920, 3240,
        3520, 1200);

    ASSERT_NE(static_cast<const Screen*>(NULL), resolved);
    ASSERT_EQ("mamre:mamre-1", resolved->m_id);

    // This captures the bug we see in production logs: position-based
    // inference on a remote host can disagree with the logical screen that
    // was explicitly selected during the last transition.
    const std::string rememberedLogicalScreen = "mamre:mamre-2";
    EXPECT_NE(rememberedLogicalScreen, resolved->m_id);
}

TEST(ServerLayoutMappingTests, resolveObjectLayoutDestinationCanLoseReturnDirectionAfterCrossingIntoAdjacentRemoteMonitor)
{
    ScreenManager layout;
    std::vector<Screen> layoutScreens;
    layoutScreens.push_back(Screen("mamre:mamre-1", "mamre", "mamre-1",
                                   0, 0, 100, 100));
    layoutScreens.back().m_rightLink = "Siloe-1";
    layoutScreens.push_back(Screen("Siloe:Siloe-1", "Siloe", "Siloe-1",
                                   100, 0, 100, 100));
    layoutScreens.back().m_leftLink = "mamre-1";
    layoutScreens.back().m_rightLink = "mamre-2";
    layoutScreens.push_back(Screen("mamre:mamre-2", "mamre", "mamre-2",
                                   200, 0, 100, 100));
    layoutScreens.back().m_leftLink = "Siloe-1";
    layout.setScreens(layoutScreens);

    EDirection direction = kNoDirection;
    int globalX = 0;
    int globalY = 0;

    const Screen* resolved = resolveObjectLayoutDestinationForTest(
        layout,
        layoutScreens[2],   // active logical screen is the remote screen on the right
        100, 0, 100, 100,   // the physical monitor that corresponds to mamre:mamre-2
        0, 0, 100, 100,     // but the cursor is already being resolved against the left monitor
        99, 50,             // just past the left edge of the active monitor
        direction,
        globalX, globalY);

    EXPECT_EQ(kNoDirection, direction);
    ASSERT_NE(static_cast<const Screen*>(NULL), resolved);
    EXPECT_EQ("mamre:mamre-2", resolved->m_id);
}

TEST(ServerLayoutMappingTests, resolveObjectLayoutDestinationReturnsToPrimaryWhenUsingSourceMonitorGeometry)
{
    ScreenManager layout;
    std::vector<Screen> layoutScreens;
    layoutScreens.push_back(Screen("mamre:mamre-1", "mamre", "mamre-1",
                                   0, 0, 100, 100));
    layoutScreens.back().m_rightLink = "Siloe-1";
    layoutScreens.push_back(Screen("Siloe:Siloe-1", "Siloe", "Siloe-1",
                                   100, 0, 100, 100));
    layoutScreens.back().m_leftLink = "mamre-1";
    layoutScreens.back().m_rightLink = "mamre-2";
    layoutScreens.push_back(Screen("mamre:mamre-2", "mamre", "mamre-2",
                                   200, 0, 100, 100));
    layoutScreens.back().m_leftLink = "Siloe-1";
    layout.setScreens(layoutScreens);

    EDirection direction = kNoDirection;
    int globalX = 0;
    int globalY = 0;

    const Screen* resolved = resolveObjectLayoutDestinationForTest(
        layout,
        layoutScreens[2],   // active logical screen is the right-hand remote screen
        100, 0, 100, 100,   // source monitor geometry for mamre:mamre-2
        100, 0, 100, 100,   // use the same source geometry for direction mapping
        99, 50,             // cursor has just crossed the left edge of mamre:mamre-2
        direction,
        globalX, globalY);

    EXPECT_EQ(kLeft, direction);
    ASSERT_NE(static_cast<const Screen*>(NULL), resolved);
    EXPECT_EQ("Siloe:Siloe-1", resolved->m_id);
}

TEST(ServerLayoutMappingTests, directObjectLayoutDestinationRequiresExplicitLink)
{
    ScreenManager layout;
    std::vector<Screen> layoutScreens;
    layoutScreens.push_back(Screen("siloe:siloe-1", "siloe", "siloe-1",
                                   0, 0, 100, 100));
    layoutScreens.push_back(Screen("mamre:mamre-1", "mamre", "mamre-1",
                                   0, 0, 100, 100));
    layout.setScreens(layoutScreens);

    EDirection direction = kNoDirection;
    int globalX = 0;
    int globalY = 0;

    const Screen* resolved = resolveObjectLayoutDestinationForTest(
        layout,
        layoutScreens[1],
        0, 0, 100, 100,
        0, 0, 100, 100,
        50, 50,
        direction,
        globalX, globalY);

    EXPECT_EQ(kNoDirection, direction);
    EXPECT_EQ(static_cast<const Screen*>(NULL), resolved);

    layoutScreens[1].m_rightLink = "siloe-1";
    layout.setScreens(layoutScreens);

    resolved = resolveObjectLayoutDestinationForTest(
        layout,
        layoutScreens[1],
        0, 0, 100, 100,
        0, 0, 100, 100,
        50, 50,
        direction,
        globalX, globalY);

    EXPECT_EQ(kNoDirection, direction);
    ASSERT_NE(static_cast<const Screen*>(NULL), resolved);
    EXPECT_EQ("siloe:siloe-1", resolved->m_id);
}

TEST(ServerLayoutMappingTests, leftEdgeFromSiloeMapsToStackedMamreMonitor)
{
    ScreenManager layout;
    std::vector<Screen> layoutScreens;
    layoutScreens.push_back(Screen("mamre:mamre-1", "mamre", "mamre-1",
                                   0, 0, 1920, 1080));
    layoutScreens.back().m_rightLink = "Siloe-1";
    layoutScreens.push_back(Screen("Siloe:Siloe-1", "Siloe", "Siloe-1",
                                   1920, 0, 1920, 1080));
    layoutScreens.back().m_leftLink = "mamre-1";
    layoutScreens.back().m_rightLink = "mamre-2";
    layoutScreens.push_back(Screen("mamre:mamre-2", "mamre", "mamre-2",
                                   3840, 0, 1920, 1080));
    layoutScreens.back().m_leftLink = "Siloe-1";
    layout.setScreens(layoutScreens);

    std::vector<ClientScreenInfo> mamreScreens;
    mamreScreens.push_back(ClientScreenInfo("mamre-1", 0, 2160, 3840, 2160));
    mamreScreens.push_back(ClientScreenInfo("mamre-2", 0, 0, 3840, 2160));

    EDirection direction = kNoDirection;
    SInt32 targetX = 0;
    SInt32 targetY = 0;
    const Screen* resolved = resolveObjectLayoutTargetForTest(
        layout,
        layoutScreens[1],   // Siloe-1: 0,0 1920x1080 on the server host
        mamreScreens,
        0, 0, 1920, 1080,
        -1, 100,
        direction,
        targetX, targetY);

    EXPECT_EQ(kLeft, direction);
    ASSERT_NE(static_cast<const Screen*>(NULL), resolved);
    EXPECT_EQ("mamre:mamre-1", resolved->m_id);
    EXPECT_EQ(3815, targetX);
    EXPECT_EQ(2360, targetY);

    resolved = resolveObjectLayoutTargetForTest(
        layout,
        layoutScreens[1],
        mamreScreens,
        0, 0, 1920, 1080,
        1920, 100,
        direction,
        targetX, targetY);

    EXPECT_EQ(kRight, direction);
    ASSERT_NE(static_cast<const Screen*>(NULL), resolved);
    EXPECT_EQ("mamre:mamre-2", resolved->m_id);
    EXPECT_EQ(24, targetX);
    EXPECT_EQ(200, targetY);
}

TEST(ServerLayoutMappingTests, rightEdgeFromSiloeUsesLeftEdgeOfLinkedMamreMonitor)
{
    ScreenManager layout;
    std::vector<Screen> layoutScreens;
    layoutScreens.push_back(Screen("mamre:mamre-1", "mamre", "mamre-1",
                                   0, 0, 1920, 1080));
    layoutScreens.back().m_rightLink = "Siloe-1";
    layoutScreens.push_back(Screen("Siloe:Siloe-1", "Siloe", "Siloe-1",
                                   1920, 0, 1920, 1080));
    layoutScreens.back().m_leftLink = "mamre-1";
    layoutScreens.back().m_rightLink = "mamre-2";
    layoutScreens.push_back(Screen("mamre:mamre-2", "mamre", "mamre-2",
                                   0, 0, 1920, 1080));
    layoutScreens.back().m_leftLink = "Siloe-1";
    layout.setScreens(layoutScreens);

    std::vector<ClientScreenInfo> mamreScreens;
    mamreScreens.push_back(ClientScreenInfo("mamre-2", 0, 0, 1920, 1080));

    EDirection direction = kNoDirection;
    SInt32 targetX = 0;
    SInt32 targetY = 0;
    const Screen* resolved = resolveObjectLayoutTargetForTest(
        layout,
        layoutScreens[1],
        mamreScreens,
        0, 0, 1920, 1080,
        1920, 200,
        direction,
        targetX, targetY);

    EXPECT_EQ(kRight, direction);
    ASSERT_NE(static_cast<const Screen*>(NULL), resolved);
    EXPECT_EQ("mamre:mamre-2", resolved->m_id);
    EXPECT_EQ(24, targetX);
    EXPECT_EQ(200, targetY);
}

TEST(ServerLayoutMappingTests, rightEdgeFromStackedMamreBottomMonitorMapsToSiloe)
{
    ScreenManager layout;
    std::vector<Screen> layoutScreens;
    layoutScreens.push_back(Screen("mamre:mamre-1", "mamre", "mamre-1",
                                   0, 0, 1920, 1080));
    layoutScreens.back().m_rightLink = "Siloe-1";
    layoutScreens.push_back(Screen("Siloe:Siloe-1", "Siloe", "Siloe-1",
                                   1920, 0, 1920, 1080));
    layoutScreens.back().m_leftLink = "mamre-1";
    layoutScreens.back().m_rightLink = "mamre-2";
    layoutScreens.push_back(Screen("mamre:mamre-2", "mamre", "mamre-2",
                                   3840, 0, 1920, 1080));
    layoutScreens.back().m_leftLink = "Siloe-1";
    layout.setScreens(layoutScreens);

    std::vector<ClientScreenInfo> siloeScreens;
    siloeScreens.push_back(ClientScreenInfo("Siloe-1", 0, 0, 1920, 1080));

    EDirection direction = kNoDirection;
    SInt32 targetX = 0;
    SInt32 targetY = 0;
    const Screen* resolved = resolveObjectLayoutTargetForTest(
        layout,
        layoutScreens[0],
        siloeScreens,
        0, 2160, 3840, 2160,
        3840, 2360,
        direction,
        targetX, targetY);

    EXPECT_EQ(kRight, direction);
    ASSERT_NE(static_cast<const Screen*>(NULL), resolved);
    EXPECT_EQ("Siloe:Siloe-1", resolved->m_id);
    EXPECT_EQ(24, targetX);
    EXPECT_EQ(100, targetY);
}

TEST(ServerLayoutMappingTests, leftEdgeFromStackedMamreTopMonitorMapsBackToSiloe)
{
    ScreenManager layout;
    std::vector<Screen> layoutScreens;
    layoutScreens.push_back(Screen("mamre:mamre-1", "mamre", "mamre-1",
                                   0, 0, 1920, 1080));
    layoutScreens.back().m_rightLink = "Siloe-1";
    layoutScreens.push_back(Screen("Siloe:Siloe-1", "Siloe", "Siloe-1",
                                   1920, 0, 1920, 1080));
    layoutScreens.back().m_leftLink = "mamre-1";
    layoutScreens.back().m_rightLink = "mamre-2";
    layoutScreens.push_back(Screen("mamre:mamre-2", "mamre", "mamre-2",
                                   3840, 0, 1920, 1080));
    layoutScreens.back().m_leftLink = "Siloe-1";
    layout.setScreens(layoutScreens);

    std::vector<ClientScreenInfo> siloeScreens;
    siloeScreens.push_back(ClientScreenInfo("Siloe-1", 0, 0, 1920, 1080));

    EDirection direction = kNoDirection;
    SInt32 targetX = 0;
    SInt32 targetY = 0;
    const Screen* resolved = resolveObjectLayoutTargetForTest(
        layout,
        layoutScreens[2],
        siloeScreens,
        0, 0, 3840, 2160,
        -1, 1018,
        direction,
        targetX, targetY);

    EXPECT_EQ(kLeft, direction);
    ASSERT_NE(static_cast<const Screen*>(NULL), resolved);
    EXPECT_EQ("Siloe:Siloe-1", resolved->m_id);
    EXPECT_EQ(1895, targetX);
    EXPECT_EQ(509, targetY);
}

TEST(ServerLayoutMappingTests, objectLayoutServerSimulationSwitchesAcrossStackedRemoteMonitors)
{
    TestEventQueue events;
    Config config(&events);
    config.addScreen("Siloe");
    config.addScreen("mamre");

    ScreenManager layout;
    std::vector<Screen> layoutScreens;
    layoutScreens.push_back(Screen("mamre:mamre-1", "mamre", "mamre-1",
                                   0, 0, 1920, 1080));
    layoutScreens.back().m_rightLink = "Siloe-1";
    layoutScreens.push_back(Screen("Siloe:Siloe-1", "Siloe", "Siloe-1",
                                   1920, 0, 1920, 1080));
    layoutScreens.back().m_leftLink = "mamre-1";
    layoutScreens.back().m_rightLink = "mamre-2";
    layoutScreens.push_back(Screen("mamre:mamre-2", "mamre", "mamre-2",
                                   3840, 0, 1920, 1080));
    layoutScreens.back().m_leftLink = "Siloe-1";
    layout.setScreens(layoutScreens);

    std::vector<ClientScreenInfo> siloeScreens;
    siloeScreens.push_back(ClientScreenInfo("Siloe-1", 0, 0, 1920, 1080));
    FakePrimaryClient siloe("Siloe", siloeScreens, 0, 0, 1920, 1080);

    std::vector<ClientScreenInfo> mamreScreens;
    mamreScreens.push_back(ClientScreenInfo("mamre-1", 0, 2160, 3840, 2160));
    mamreScreens.push_back(ClientScreenInfo("mamre-2", 0, 0, 3840, 2160));
    FakeClientProxy mamre("mamre", mamreScreens, 0, 0, 3840, 4320);

    Server server;
    server.setEventsForTest(&events);
    server.setConfigForTest(&config);
    server.setPrimaryClientForTest(&siloe);
    server.addClientForTest("Siloe", &siloe);
    server.addClientForTest("mamre", &mamre);
    server.setScreenLayoutForTest(layout);
    server.setActive(&mamre);
    server.setActiveLayoutScreenIdForTest("mamre:mamre-1");
    server.setCursorPosForTest(3839, 2360);

    ASSERT_TRUE(server.trySwitchUsingObjectLayoutForTest(3840, 2360, false));
    EXPECT_EQ(&siloe, server.getActiveClientForTest());
    EXPECT_EQ("Siloe:Siloe-1", server.getActiveLayoutScreenIdForTest());
    EXPECT_TRUE(siloe.m_entered);
    EXPECT_EQ(24, siloe.m_lastEnterX);
    EXPECT_EQ(100, siloe.m_lastEnterY);

    ASSERT_TRUE(server.trySwitchUsingObjectLayoutForTest(1920, 509, true));
    EXPECT_EQ(&mamre, server.getActiveClientForTest());
    EXPECT_EQ("mamre:mamre-2", server.getActiveLayoutScreenIdForTest());
    EXPECT_TRUE(mamre.m_entered);
    EXPECT_EQ(24, mamre.m_lastEnterX);
    EXPECT_EQ(1018, mamre.m_lastEnterY);

    EXPECT_FALSE(server.trySwitchUsingObjectLayoutForTest(-1, 1018, false));
    EXPECT_EQ(&mamre, server.getActiveClientForTest());
    EXPECT_EQ("mamre:mamre-2", server.getActiveLayoutScreenIdForTest());

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    EXPECT_FALSE(server.trySwitchUsingObjectLayoutForTest(-1, 1018, false));
    EXPECT_EQ(&mamre, server.getActiveClientForTest());
    EXPECT_EQ("mamre:mamre-2", server.getActiveLayoutScreenIdForTest());

    server.onMouseMoveSecondaryForTest(120, 0);
    ASSERT_EQ(&mamre, server.getActiveClientForTest());

    server.onMouseMoveSecondaryForTest(-200, 0);
    EXPECT_EQ(&siloe, server.getActiveClientForTest());
    EXPECT_EQ("Siloe:Siloe-1", server.getActiveLayoutScreenIdForTest());
    EXPECT_EQ(1895, siloe.m_lastEnterX);
    EXPECT_EQ(509, siloe.m_lastEnterY);
}

TEST(ServerLayoutMappingTests, primaryMovementAwayFromReturnEdgeClearsRecentReverseGuard)
{
    TestEventQueue events;
    Config config(&events);
    config.addScreen("Siloe");
    config.addScreen("mamre");

    ScreenManager layout;
    std::vector<Screen> layoutScreens;
    layoutScreens.push_back(Screen("Siloe:Siloe-1", "Siloe", "Siloe-1",
                                   0, 0, 1920, 1080));
    layoutScreens.back().m_rightLink = "mamre-1";
    layoutScreens.push_back(Screen("mamre:mamre-1", "mamre", "mamre-1",
                                   1920, 0, 3840, 2160));
    layoutScreens.back().m_leftLink = "Siloe-1";
    layout.setScreens(layoutScreens);

    std::vector<ClientScreenInfo> siloeScreens;
    siloeScreens.push_back(ClientScreenInfo("Siloe-1", 0, 0, 1920, 1080));
    FakePrimaryClient siloe("Siloe", siloeScreens, 0, 0, 1920, 1080);

    std::vector<ClientScreenInfo> mamreScreens;
    mamreScreens.push_back(ClientScreenInfo("mamre-1", 0, 0, 3840, 2160));
    FakeClientProxy mamre("mamre", mamreScreens, 0, 0, 3840, 2160);

    Server server;
    server.setEventsForTest(&events);
    server.setConfigForTest(&config);
    server.setPrimaryClientForTest(&siloe);
    server.addClientForTest("Siloe", &siloe);
    server.addClientForTest("mamre", &mamre);
    server.setScreenLayoutForTest(layout);
    server.setActive(&siloe);
    server.setActiveLayoutScreenIdForTest("Siloe:Siloe-1");
    server.setCursorPosForTest(1919, 540);

    ASSERT_TRUE(server.trySwitchUsingObjectLayoutForTest(1920, 540, true));
    EXPECT_EQ(&mamre, server.getActiveClientForTest());
    EXPECT_EQ("mamre:mamre-1", server.getActiveLayoutScreenIdForTest());

    server.onMouseMoveSecondaryForTest(120, 0);
    ASSERT_EQ(&mamre, server.getActiveClientForTest());

    server.onMouseMoveSecondaryForTest(-200, 0);
    EXPECT_EQ(&siloe, server.getActiveClientForTest());
    EXPECT_EQ("Siloe:Siloe-1", server.getActiveLayoutScreenIdForTest());
    EXPECT_EQ(1895, siloe.m_lastEnterX);
    EXPECT_EQ(540, siloe.m_lastEnterY);

    EXPECT_FALSE(server.trySwitchUsingObjectLayoutForTest(1920, 540, true));
    EXPECT_EQ(&siloe, server.getActiveClientForTest());

    server.onMouseMovePrimaryForTest(1700, 540);

    EXPECT_TRUE(server.trySwitchUsingObjectLayoutForTest(1920, 540, true));
    EXPECT_EQ(&mamre, server.getActiveClientForTest());
    EXPECT_EQ("mamre:mamre-1", server.getActiveLayoutScreenIdForTest());
}

TEST(ServerLayoutMappingTests, sameClientLogicalSwitchSendsEnterSoClientRefreshesMonitorBounds)
{
    TestEventQueue events;
    Config config(&events);
    config.addScreen("Siloe");
    config.addScreen("mamre");

    ScreenManager layout;
    std::vector<Screen> layoutScreens;
    layoutScreens.push_back(Screen("mamre:mamre-1", "mamre", "mamre-1",
                                   0, 0, 1920, 1080));
    layoutScreens.back().m_rightLink = "mamre-2";
    layoutScreens.push_back(Screen("mamre:mamre-2", "mamre", "mamre-2",
                                   1920, 0, 1920, 1080));
    layoutScreens.back().m_leftLink = "mamre-1";
    layout.setScreens(layoutScreens);

    std::vector<ClientScreenInfo> siloeScreens;
    siloeScreens.push_back(ClientScreenInfo("Siloe-1", 0, 0, 1920, 1080));
    FakePrimaryClient siloe("Siloe", siloeScreens, 0, 0, 1920, 1080);

    std::vector<ClientScreenInfo> mamreScreens;
    mamreScreens.push_back(ClientScreenInfo("mamre-1", 0, 0, 1920, 1080));
    mamreScreens.push_back(ClientScreenInfo("mamre-2", 1920, 0, 1920, 1080));
    FakeClientProxy mamre("mamre", mamreScreens, 100, 100, 3840, 1080);

    Server server;
    server.setEventsForTest(&events);
    server.setConfigForTest(&config);
    server.setPrimaryClientForTest(&siloe);
    server.addClientForTest("Siloe", &siloe);
    server.addClientForTest("mamre", &mamre);
    server.setScreenLayoutForTest(layout);
    server.setActive(&mamre);
    server.setActiveLayoutScreenIdForTest("mamre:mamre-1");
    server.setCursorPosForTest(1919, 100);

    ASSERT_TRUE(server.trySwitchUsingObjectLayoutForTest(1920, 100, false));
    EXPECT_EQ(&mamre, server.getActiveClientForTest());
    EXPECT_EQ("mamre:mamre-2", server.getActiveLayoutScreenIdForTest());
    EXPECT_TRUE(mamre.m_entered);
    EXPECT_EQ(1944, mamre.m_lastEnterX);
    EXPECT_EQ(100, mamre.m_lastEnterY);
    EXPECT_TRUE(mamre.m_mouseMoves.empty());
}

TEST(ServerLayoutMappingTests, sameClientLogicalScreensWithoutLinkAreClampedToSourceMonitor)
{
    TestEventQueue events;
    Config config(&events);
    config.addScreen("Siloe");
    config.addScreen("mamre");

    ScreenManager layout;
    std::vector<Screen> layoutScreens;
    layoutScreens.push_back(Screen("mamre:mamre-1", "mamre", "mamre-1",
                                   0, 0, 1920, 1080));
    layoutScreens.push_back(Screen("mamre:mamre-2", "mamre", "mamre-2",
                                   1920, 0, 1920, 1080));
    layout.setScreens(layoutScreens);

    std::vector<ClientScreenInfo> siloeScreens;
    siloeScreens.push_back(ClientScreenInfo("Siloe-1", 0, 0, 1920, 1080));
    FakePrimaryClient siloe("Siloe", siloeScreens, 0, 0, 1920, 1080);

    std::vector<ClientScreenInfo> mamreScreens;
    mamreScreens.push_back(ClientScreenInfo("mamre-1", 0, 0, 1920, 1080));
    mamreScreens.push_back(ClientScreenInfo("mamre-2", 1920, 0, 1920, 1080));
    FakeClientProxy mamre("mamre", mamreScreens, 100, 100, 3840, 1080);

    Server server;
    server.setEventsForTest(&events);
    server.setConfigForTest(&config);
    server.setPrimaryClientForTest(&siloe);
    server.addClientForTest("Siloe", &siloe);
    server.addClientForTest("mamre", &mamre);
    server.setScreenLayoutForTest(layout);
    server.setActive(&mamre);
    server.setActiveLayoutScreenIdForTest("mamre:mamre-1");
    server.setCursorPosForTest(1919, 100);

    EXPECT_FALSE(server.trySwitchUsingObjectLayoutForTest(1920, 100, false));
    EXPECT_EQ(&mamre, server.getActiveClientForTest());
    EXPECT_EQ("mamre:mamre-1", server.getActiveLayoutScreenIdForTest());
    EXPECT_FALSE(mamre.m_entered);
    ASSERT_FALSE(mamre.m_mouseMoves.empty());
    EXPECT_EQ(1919, mamre.m_mouseMoves.back().first);
    EXPECT_EQ(100, mamre.m_mouseMoves.back().second);
}

TEST(ServerLayoutMappingTests, objectLayoutSecondaryMotionInsideLogicalScreenMovesCursor)
{
    TestEventQueue events;
    Config config(&events);
    config.addScreen("Siloe");
    config.addScreen("KANAAN");

    ScreenManager layout;
    std::vector<Screen> layoutScreens;
    layoutScreens.push_back(Screen("KANAAN:KANAAN-1", "KANAAN", "KANAAN-1",
                                   0, 0, 1920, 1080));
    layoutScreens.back().m_rightLink = "Siloe-1";
    layoutScreens.push_back(Screen("Siloe:Siloe-1", "Siloe", "Siloe-1",
                                   1920, 0, 1920, 1080));
    layoutScreens.back().m_leftLink = "KANAAN-1";
    layout.setScreens(layoutScreens);

    std::vector<ClientScreenInfo> siloeScreens;
    siloeScreens.push_back(ClientScreenInfo("Siloe-1", 0, 0, 1920, 1080));
    FakePrimaryClient siloe("Siloe", siloeScreens, 0, 0, 1920, 1080);

    std::vector<ClientScreenInfo> kanaanScreens;
    kanaanScreens.push_back(ClientScreenInfo("KANAAN-1", 0, 0, 1920, 1080));
    FakeClientProxy kanaan("KANAAN", kanaanScreens, 100, 100, 1920, 1080);

    Server server;
    server.setEventsForTest(&events);
    server.setConfigForTest(&config);
    server.setPrimaryClientForTest(&siloe);
    server.addClientForTest("Siloe", &siloe);
    server.addClientForTest("KANAAN", &kanaan);
    server.setScreenLayoutForTest(layout);
    server.setActive(&kanaan);
    server.setActiveLayoutScreenIdForTest("KANAAN:KANAAN-1");
    server.setCursorPosForTest(100, 100);

    server.onMouseMoveSecondaryForTest(10, 5);

    ASSERT_EQ(1u, kanaan.m_mouseMoves.size());
    EXPECT_EQ(110, kanaan.m_mouseMoves.back().first);
    EXPECT_EQ(105, kanaan.m_mouseMoves.back().second);
    EXPECT_EQ(&kanaan, server.getActiveClientForTest());
    EXPECT_EQ("KANAAN:KANAAN-1", server.getActiveLayoutScreenIdForTest());
}

} // namespace
