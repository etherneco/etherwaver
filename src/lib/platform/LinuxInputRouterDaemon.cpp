#include "platform/LinuxInputRouterDaemon.h"

#include "base/Log.h"

#if defined(__linux__)

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <set>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

volatile sig_atomic_t g_stopRequested = 0;
int g_wakeWriteFd = -1;

void signal_handler(int)
{
    g_stopRequested = 1;
    if (g_wakeWriteFd >= 0) {
        const char byte = 'x';
        write(g_wakeWriteFd, &byte, sizeof(byte));
    }
}

template<size_t N>
bool test_bit(const std::array<unsigned long, N>& bits, unsigned int bit)
{
    const size_t index = bit / (sizeof(unsigned long) * 8);
    const size_t offset = bit % (sizeof(unsigned long) * 8);
    return (index < N) && ((bits[index] & (1UL << offset)) != 0);
}

template<size_t N>
bool fetch_bits(int fd, unsigned long request, std::array<unsigned long, N>& bits)
{
    bits.fill(0);
    return (ioctl(fd, request, bits.data()) >= 0);
}

} // namespace

LinuxInputRouterDaemon::Config::Config()
    : m_uhidName("Etherwaver")
    , m_screenWidth(1920)
    , m_screenHeight(1080)
    , m_edgeThreshold(40)
    , m_rescanIntervalMs(1000)
    , m_logLevel("INFO")
    , m_debugEvents(false)
{
}

LinuxInputRouterDaemon::LinuxInputRouterDaemon()
    : m_running(false)
    , m_epollFd(-1)
    , m_wakePipe{-1, -1}
{
}

LinuxInputRouterDaemon::~LinuxInputRouterDaemon()
{
    stop();
}

int
LinuxInputRouterDaemon::run(int argc, char** argv)
{
    if (!parseArgs(argc, argv)) {
        return 1;
    }

    CLOG->setFilter(m_config.m_logLevel.c_str());

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (!start()) {
        stop();
        return 1;
    }

    if (m_inputThread.joinable()) {
        m_inputThread.join();
    }

    stop();
    return 0;
}

bool
LinuxInputRouterDaemon::parseArgs(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return false;
        }
        if (arg == "--debug-events") {
            m_config.m_debugEvents = true;
            m_config.m_logLevel = "DEBUG1";
            continue;
        }
        if (arg == "--uhid-name" && i + 1 < argc) {
            m_config.m_uhidName = argv[++i];
            continue;
        }
        if (arg == "--screen-width" && i + 1 < argc) {
            m_config.m_screenWidth = atoi(argv[++i]);
            continue;
        }
        if (arg == "--screen-height" && i + 1 < argc) {
            m_config.m_screenHeight = atoi(argv[++i]);
            continue;
        }
        if (arg == "--edge-threshold" && i + 1 < argc) {
            m_config.m_edgeThreshold = atoi(argv[++i]);
            continue;
        }
        if (arg == "--log-level" && i + 1 < argc) {
            m_config.m_logLevel = argv[++i];
            continue;
        }

        fprintf(stderr, "Unknown argument: %s\n", arg.c_str());
        printUsage(argv[0]);
        return false;
    }

    return true;
}

void
LinuxInputRouterDaemon::printUsage(const char* argv0) const
{
    fprintf(stdout,
        "Usage: %s [options]\n"
        "  --uhid-name NAME       Base name for virtual keyboard/mouse\n"
        "  --screen-width N       Virtual local screen width (default 1920)\n"
        "  --screen-height N      Virtual local screen height (default 1080)\n"
        "  --edge-threshold N     Edge trigger threshold in pixels (default 40)\n"
        "  --log-level LEVEL      ERROR|WARNING|NOTE|INFO|DEBUG|DEBUG1...\n"
        "  --debug-events         Verbose event logging\n",
        argv0);
}

bool
LinuxInputRouterDaemon::start()
{
    if (m_running.exchange(true)) {
        return true;
    }

    if (pipe(m_wakePipe) != 0) {
        LOG((CLOG_ERR "waverd: pipe failed (%s)", strerror(errno)));
        return false;
    }
    fcntl(m_wakePipe[0], F_SETFL, O_NONBLOCK);
    fcntl(m_wakePipe[1], F_SETFL, O_NONBLOCK);

    m_epollFd = epoll_create1(EPOLL_CLOEXEC);
    if (m_epollFd < 0) {
        LOG((CLOG_ERR "waverd: epoll_create1 failed (%s)", strerror(errno)));
        return false;
    }

    struct epoll_event wakeEvent;
    memset(&wakeEvent, 0, sizeof(wakeEvent));
    wakeEvent.events = EPOLLIN;
    wakeEvent.data.fd = m_wakePipe[0];
    epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_wakePipe[0], &wakeEvent);
    g_wakeWriteFd = m_wakePipe[1];

    RouterCore::Config routerConfig;
    routerConfig.m_uhidName = m_config.m_uhidName;
    routerConfig.m_screenWidth = m_config.m_screenWidth;
    routerConfig.m_screenHeight = m_config.m_screenHeight;
    routerConfig.m_edgeThreshold = m_config.m_edgeThreshold;
    routerConfig.m_debugEvents = m_config.m_debugEvents;
    m_router.reset(new RouterCore(routerConfig, [this](bool exclusive) { setExclusiveRouting(exclusive); }));

    if (!m_router->start()) {
        LOG((CLOG_ERR "waverd: failed to start router core"));
        return false;
    }

    refreshDevices();
    m_routerThread = std::thread(&LinuxInputRouterDaemon::routerLoop, this);
    m_inputThread = std::thread(&LinuxInputRouterDaemon::inputLoop, this);
    LOG((CLOG_NOTE "waverd: started"));
    return true;
}

void
LinuxInputRouterDaemon::stop()
{
    if (!m_running.exchange(false)) {
        return;
    }

    if (m_wakePipe[1] >= 0) {
        const char byte = 'q';
        write(m_wakePipe[1], &byte, sizeof(byte));
    }

    if (m_inputThread.joinable()) {
        m_inputThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queueCv.notify_all();
    }
    if (m_routerThread.joinable()) {
        m_routerThread.join();
    }

    {
        std::lock_guard<std::mutex> lock(m_devicesMutex);
        while (!m_devices.empty()) {
            detachDevice(m_devices.begin()->first);
        }
    }

    if (m_router) {
        m_router->stop();
        m_router.reset();
    }

    if (m_epollFd >= 0) {
        close(m_epollFd);
        m_epollFd = -1;
    }
    if (m_wakePipe[0] >= 0) {
        close(m_wakePipe[0]);
        m_wakePipe[0] = -1;
    }
    if (m_wakePipe[1] >= 0) {
        close(m_wakePipe[1]);
        m_wakePipe[1] = -1;
    }
    g_wakeWriteFd = -1;
}

void
LinuxInputRouterDaemon::inputLoop()
{
    std::array<struct epoll_event, 32> events;
    while (m_running.load() && !g_stopRequested) {
        const int count = epoll_wait(m_epollFd, events.data(), static_cast<int>(events.size()), m_config.m_rescanIntervalMs);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG((CLOG_ERR "waverd: epoll_wait failed (%s)", strerror(errno)));
            break;
        }

        for (int i = 0; i < count; ++i) {
            const int fd = events[i].data.fd;
            if (fd == m_wakePipe[0]) {
                char buffer[32];
                while (read(fd, buffer, sizeof(buffer)) > 0) {
                }
                continue;
            }

            std::lock_guard<std::mutex> lock(m_devicesMutex);
            std::map<int, DeviceRecord>::iterator it = m_devices.find(fd);
            if (it == m_devices.end()) {
                continue;
            }

            if ((events[i].events & (EPOLLERR | EPOLLHUP)) != 0 || !readDeviceEvents(it->second)) {
                detachDevice(fd);
            }
        }

        refreshDevices();
    }
}

void
LinuxInputRouterDaemon::routerLoop()
{
    while (m_running.load() || !m_queue.empty()) {
        RoutedEvent queued;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_queueCv.wait(lock, [this]() { return !m_queue.empty() || !m_running.load(); });
            if (m_queue.empty()) {
                continue;
            }
            queued = m_queue.front();
            m_queue.pop_front();
        }

        if (m_router) {
            m_router->routeEvent(queued.m_event, queued.m_devicePath);
        }
    }
}

void
LinuxInputRouterDaemon::refreshDevices()
{
    DIR* dir = opendir("/dev/input");
    if (dir == NULL) {
        LOG((CLOG_WARN "waverd: failed to open /dev/input (%s)", strerror(errno)));
        return;
    }

    std::set<std::string> seen;
    for (struct dirent* entry = readdir(dir); entry != NULL; entry = readdir(dir)) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        const std::string path = std::string("/dev/input/") + entry->d_name;
        seen.insert(path);
        bool known = false;
        {
            std::lock_guard<std::mutex> lock(m_devicesMutex);
            for (const auto& pair : m_devices) {
                if (pair.second.m_path == path) {
                    known = true;
                    break;
                }
            }
        }
        if (!known) {
            attachDevice(path);
        }
    }
    closedir(dir);

    std::lock_guard<std::mutex> lock(m_devicesMutex);
    std::vector<int> removeFds;
    for (const auto& pair : m_devices) {
        if (seen.find(pair.second.m_path) == seen.end()) {
            removeFds.push_back(pair.first);
        }
    }
    for (int fd : removeFds) {
        detachDevice(fd);
    }
}

void
LinuxInputRouterDaemon::attachDevice(const std::string& path)
{
    const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        LOG((CLOG_WARN "waverd: open %s failed (%s)", path.c_str(), strerror(errno)));
        return;
    }

    DeviceRecord record;
    record.m_path = path;
    record.m_fd = fd;
    record.m_grabbed = false;
    if (!classifyDevice(fd, record)) {
        close(fd);
        return;
    }

    if (shouldIgnoreDeviceName(record.m_name)) {
        close(fd);
        return;
    }

    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
    event.data.fd = fd;
    if (epoll_ctl(m_epollFd, EPOLL_CTL_ADD, fd, &event) != 0) {
        LOG((CLOG_WARN "waverd: epoll add %s failed (%s)", path.c_str(), strerror(errno)));
        close(fd);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_devicesMutex);
        m_devices[fd] = record;
        if (m_grabber.exclusive()) {
            m_devices[fd].m_grabbed = m_grabber.apply(fd, path, true);
        }
    }

    LOG((CLOG_NOTE
        "waverd: attached %s name=%s keyboard=%s pointer=%s touchpad=%s",
        path.c_str(),
        record.m_name.c_str(),
        record.m_keyboard ? "yes" : "no",
        record.m_pointer ? "yes" : "no",
        record.m_touchpad ? "yes" : "no"));
}

void
LinuxInputRouterDaemon::detachDevice(int fd)
{
    std::map<int, DeviceRecord>::iterator it = m_devices.find(fd);
    if (it == m_devices.end()) {
        return;
    }

    epoll_ctl(m_epollFd, EPOLL_CTL_DEL, fd, NULL);
    if (it->second.m_grabbed) {
        m_grabber.apply(fd, it->second.m_path, false);
    }
    LOG((CLOG_NOTE "waverd: detached %s name=%s", it->second.m_path.c_str(), it->second.m_name.c_str()));
    close(fd);
    m_devices.erase(it);
}

bool
LinuxInputRouterDaemon::classifyDevice(int fd, DeviceRecord& record) const
{
    record.m_name = deviceName(fd);
    record.m_keyboard = false;
    record.m_pointer = false;
    record.m_touchpad = false;

    std::array<unsigned long, (EV_MAX / (sizeof(unsigned long) * 8)) + 1> evBits;
    std::array<unsigned long, (KEY_MAX / (sizeof(unsigned long) * 8)) + 1> keyBits;
    std::array<unsigned long, (REL_MAX / (sizeof(unsigned long) * 8)) + 1> relBits;
    std::array<unsigned long, (ABS_MAX / (sizeof(unsigned long) * 8)) + 1> absBits;

    if (!fetch_bits(fd, EVIOCGBIT(0, sizeof(evBits)), evBits)) {
        return false;
    }

    const bool hasKeys = test_bit(evBits, EV_KEY) && fetch_bits(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits);
    const bool hasRel = test_bit(evBits, EV_REL) && fetch_bits(fd, EVIOCGBIT(EV_REL, sizeof(relBits)), relBits);
    const bool hasAbs = test_bit(evBits, EV_ABS) && fetch_bits(fd, EVIOCGBIT(EV_ABS, sizeof(absBits)), absBits);

    if (hasKeys) {
        record.m_keyboard =
            test_bit(keyBits, KEY_A) ||
            test_bit(keyBits, KEY_SPACE) ||
            test_bit(keyBits, KEY_ENTER);

        record.m_pointer =
            (test_bit(keyBits, BTN_LEFT) || test_bit(keyBits, BTN_RIGHT) || test_bit(keyBits, BTN_MIDDLE));

        record.m_touchpad =
            hasAbs &&
            test_bit(absBits, ABS_X) &&
            test_bit(absBits, ABS_Y) &&
            (test_bit(keyBits, BTN_TOOL_FINGER) || test_bit(keyBits, BTN_TOUCH));
    }

    if (hasRel && test_bit(relBits, REL_X) && test_bit(relBits, REL_Y)) {
        record.m_pointer = true;
    }

    return (record.m_keyboard || record.m_pointer || record.m_touchpad);
}

std::string
LinuxInputRouterDaemon::deviceName(int fd) const
{
    char name[256];
    memset(name, 0, sizeof(name));
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
        return "unknown";
    }
    return name;
}

bool
LinuxInputRouterDaemon::shouldIgnoreDeviceName(const std::string& name) const
{
    const std::string keyboardName = m_config.m_uhidName + " Keyboard";
    const std::string mouseName = m_config.m_uhidName + " Mouse";
    return (name == keyboardName || name == mouseName);
}

void
LinuxInputRouterDaemon::setExclusiveRouting(bool exclusive)
{
    std::lock_guard<std::mutex> lock(m_devicesMutex);
    m_grabber.setExclusive(exclusive);
    for (auto& pair : m_devices) {
        if (pair.second.m_grabbed == exclusive) {
            continue;
        }
        const bool success = m_grabber.apply(pair.second.m_fd, pair.second.m_path, exclusive);
        pair.second.m_grabbed = exclusive && success;
    }
}

bool
LinuxInputRouterDaemon::readDeviceEvents(DeviceRecord& device)
{
    while (true) {
        input_event event;
        const ssize_t n = read(device.m_fd, &event, sizeof(event));
        if (n == static_cast<ssize_t>(sizeof(event))) {
            enqueue(event, device.m_path);
            continue;
        }

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return true;
        }

        return false;
    }
}

void
LinuxInputRouterDaemon::enqueue(const input_event& event, const std::string& devicePath)
{
    std::lock_guard<std::mutex> lock(m_queueMutex);
    RoutedEvent queued;
    queued.m_event = event;
    queued.m_devicePath = devicePath;
    m_queue.push_back(queued);
    m_queueCv.notify_one();
}

#endif
