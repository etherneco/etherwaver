#define BARRIER_TEST_ENV

#include "core/layout/LayoutLoader.h"
#include "core/layout/ScreenManager.h"
#include "server/Config.h"

#include "test/global/gtest.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include <unistd.h>

namespace {

using etherwaver::layout::HostGeometry;
using etherwaver::layout::LayoutLoader;
using etherwaver::layout::Screen;
using etherwaver::layout::ScreenManager;

std::string
writeLayoutFile(const std::string& body)
{
    char path[] = "/tmp/etherwaver-layout-test-XXXXXX";
    const int fd = mkstemp(path);
    EXPECT_NE(-1, fd);
    if (fd != -1) {
        close(fd);
    }

    std::ofstream out(path);
    out << body;
    out.close();
    return path;
}

} // namespace

TEST(ScreenManagerTests, findScreenInDirection_allowsChainAcrossHostsAndMonitors)
{
    std::vector<Screen> screens;
    screens.push_back(Screen("host1:monitor1", "host1", "monitor1",
                             0, 0, 1920, 1080));
    screens.push_back(Screen("host2:monitor1", "host2", "monitor1",
                             1920, 0, 1920, 1080));
    screens.push_back(Screen("host1:monitor2", "host1", "monitor2",
                             3840, 0, 1920, 1080));

    ScreenManager manager;
    manager.setScreens(screens);

    const Screen* host2Monitor1 =
        manager.findScreenInDirection("host1:monitor1", kRight);
    ASSERT_NE(static_cast<const Screen*>(NULL), host2Monitor1);
    EXPECT_EQ("host2:monitor1", host2Monitor1->m_id);
    EXPECT_EQ("host2", host2Monitor1->m_hostId);

    const Screen* host1Monitor2 =
        manager.findScreenInDirection("host2:monitor1", kRight);
    ASSERT_NE(static_cast<const Screen*>(NULL), host1Monitor2);
    EXPECT_EQ("host1:monitor2", host1Monitor2->m_id);
    EXPECT_EQ("host1", host1Monitor2->m_hostId);

    const Screen* backToHost2 =
        manager.findScreenInDirection("host1:monitor2", kLeft);
    ASSERT_NE(static_cast<const Screen*>(NULL), backToHost2);
    EXPECT_EQ("host2:monitor1", backToHost2->m_id);
}

TEST(ScreenManagerTests, loadLayout_preservesSavedMonitorLinks)
{
    const std::string path = writeLayoutFile(
        "{\"screens\":["
        "{\"id\":\"host1:monitor1\",\"host\":\"host1\",\"name\":\"monitor1\","
        "\"x\":0,\"y\":0,\"width\":1920,\"height\":1080,"
        "\"links\":{\"right\":\"host2:monitor1\"}},"
        "{\"id\":\"host2:monitor1\",\"host\":\"host2\",\"name\":\"monitor1\","
        "\"x\":1920,\"y\":0,\"width\":1920,\"height\":1080,"
        "\"links\":{\"left\":\"host1:monitor1\",\"right\":\"host1:monitor2\"}},"
        "{\"id\":\"host1:monitor2\",\"host\":\"host1\",\"name\":\"monitor2\","
        "\"x\":3840,\"y\":0,\"width\":1920,\"height\":1080,"
        "\"links\":{\"left\":\"host2:monitor1\"}}"
        "]}");

    Config config;
    std::map<std::string, HostGeometry> hostGeometries;
    std::map<std::string, std::vector<ClientScreenInfo> > hostScreens;

    std::ifstream saved(path.c_str());
    char first = '\0';
    saved.get(first);
    ASSERT_EQ('{', first);

    ScreenManager manager =
        LayoutLoader::loadLayout(path, config, hostGeometries, hostScreens, "host1");

    std::remove(path.c_str());

    const Screen* host2Monitor1 =
        manager.findScreenInDirection("host1:monitor1", kRight);
    ASSERT_NE(static_cast<const Screen*>(NULL), host2Monitor1);
    EXPECT_EQ("host2:monitor1", host2Monitor1->m_id);

    const Screen* host1Monitor2 =
        manager.findScreenInDirection("host2:monitor1", kRight);
    ASSERT_NE(static_cast<const Screen*>(NULL), host1Monitor2);
    EXPECT_EQ("host1:monitor2", host1Monitor2->m_id);
    EXPECT_EQ("host1", manager.getHostForScreen("host1:monitor2"));
}
