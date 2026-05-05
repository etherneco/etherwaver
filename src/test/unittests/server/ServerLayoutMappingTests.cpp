#define BARRIER_TEST_ENV

#include "server/Server.h"
#include "server/BaseClientProxy.h"
#include "core/layout/ScreenManager.h"

#include "test/global/gtest.h"

namespace {

using etherwaver::layout::Screen;
using etherwaver::layout::ScreenManager;
using etherwaver::server::findLayoutScreenForPositionForTest;
using etherwaver::server::getJumpCursorPosForLayoutScreenForTest;
using etherwaver::server::resolveObjectLayoutDestinationForTest;
using etherwaver::server::selectClientScreenForLayoutScreenForTest;

class FakeClientProxy : public BaseClientProxy {
public:
    FakeClientProxy(const std::string& name,
                    const std::vector<ClientScreenInfo>& screens,
                    SInt32 x, SInt32 y, SInt32 w, SInt32 h)
        : BaseClientProxy(name)
        , m_screens(screens)
        , m_x(x)
        , m_y(y)
        , m_w(w)
        , m_h(h)
    {
    }

    void* getEventTarget() const { return NULL; }
    bool getClipboard(ClipboardID, IClipboard*) const { return false; }
    void getShape(SInt32& x, SInt32& y, SInt32& w, SInt32& h) const
    {
        x = m_x;
        y = m_y;
        w = m_w;
        h = m_h;
    }
    void getScreens(std::vector<ClientScreenInfo>& screens) const { screens = m_screens; }
    void getCursorPos(SInt32& x, SInt32& y) const { x = m_x; y = m_y; }
    void enter(SInt32, SInt32, UInt32, KeyModifierMask, bool) {}
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
    barrier::IStream* getStream() const { return NULL; }

private:
    std::vector<ClientScreenInfo> m_screens;
    SInt32 m_x;
    SInt32 m_y;
    SInt32 m_w;
    SInt32 m_h;
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
    client.setJumpCursorPos(4300, 1200);

    SInt32 x = 0;
    SInt32 y = 0;
    getJumpCursorPosForLayoutScreenForTest(layout, &client, layoutScreens[0], x, y);

    EXPECT_EQ(1600, x);
    EXPECT_EQ(900, y);
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

} // namespace
