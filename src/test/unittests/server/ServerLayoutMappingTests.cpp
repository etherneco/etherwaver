#define BARRIER_TEST_ENV

#include "server/Server.h"
#include "core/layout/ScreenManager.h"

#include "test/global/gtest.h"

namespace {

using etherwaver::layout::Screen;
using etherwaver::layout::ScreenManager;
using etherwaver::server::findLayoutScreenForPositionForTest;
using etherwaver::server::selectClientScreenForLayoutScreenForTest;

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

} // namespace
