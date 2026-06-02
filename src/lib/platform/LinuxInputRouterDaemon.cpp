#include "platform/LinuxInputRouterDaemon.h"

#include "base/Log.h"
#include "config.h"

#if defined(__linux__)

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <set>
#include <map>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/socket.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <X11/Xlib.h>
#endif

namespace {

volatile sig_atomic_t g_stopRequested = 0;
int g_wakeWriteFd = -1;

struct CursorInputDevice {
    int m_fd;
    std::string m_path;
    std::string m_name;
};

struct CursorDeviceScanStats {
    CursorDeviceScanStats()
        : m_candidates(0)
        , m_openDenied(0)
        , m_openFailed(0)
        , m_ignored(0)
        , m_notPointer(0)
        , m_attached(0)
    {
    }

    int m_candidates;
    int m_openDenied;
    int m_openFailed;
    int m_ignored;
    int m_notPointer;
    int m_attached;
};

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

std::string inputDeviceName(int fd)
{
    char name[256];
    memset(name, 0, sizeof(name));
    if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
        return "unknown";
    }
    return name;
}

bool shouldIgnoreCursorDeviceName(const std::string& name)
{
    return name.find("EtherWaver") != std::string::npos ||
           name.find("Etherwaver") != std::string::npos ||
           name.find("Barrier") != std::string::npos;
}

bool isRelativePointerDevice(int fd)
{
    std::array<unsigned long, (EV_MAX / (sizeof(unsigned long) * 8)) + 1> evBits;
    std::array<unsigned long, (REL_MAX / (sizeof(unsigned long) * 8)) + 1> relBits;
    std::array<unsigned long, (KEY_MAX / (sizeof(unsigned long) * 8)) + 1> keyBits;

    if (!fetch_bits(fd, EVIOCGBIT(0, sizeof(evBits)), evBits)) {
        return false;
    }

    if (!test_bit(evBits, EV_REL) ||
        !fetch_bits(fd, EVIOCGBIT(EV_REL, sizeof(relBits)), relBits) ||
        !test_bit(relBits, REL_X) ||
        !test_bit(relBits, REL_Y)) {
        return false;
    }

    if (!test_bit(evBits, EV_KEY) ||
        !fetch_bits(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits)) {
        return true;
    }

    return test_bit(keyBits, BTN_LEFT) ||
           test_bit(keyBits, BTN_RIGHT) ||
           test_bit(keyBits, BTN_MIDDLE);
}

void closeCursorDevices(std::map<int, CursorInputDevice>& devices, int epollFd)
{
    for (std::map<int, CursorInputDevice>::iterator it = devices.begin(); it != devices.end(); ++it) {
        if (epollFd >= 0) {
            epoll_ctl(epollFd, EPOLL_CTL_DEL, it->first, NULL);
        }
        close(it->first);
    }
    devices.clear();
}

void refreshCursorDevices(std::map<int, CursorInputDevice>& devices,
                          int epollFd,
                          CursorDeviceScanStats& stats)
{
    stats = CursorDeviceScanStats();

    DIR* dir = opendir("/dev/input");
    if (dir == NULL) {
        return;
    }

    std::set<std::string> seen;
    for (struct dirent* entry = readdir(dir); entry != NULL; entry = readdir(dir)) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        const std::string path = std::string("/dev/input/") + entry->d_name;
        ++stats.m_candidates;
        seen.insert(path);

        bool known = false;
        for (std::map<int, CursorInputDevice>::const_iterator it = devices.begin(); it != devices.end(); ++it) {
            if (it->second.m_path == path) {
                known = true;
                break;
            }
        }
        if (known) {
            continue;
        }

        const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if (errno == EACCES || errno == EPERM) {
                ++stats.m_openDenied;
            }
            else {
                ++stats.m_openFailed;
            }
            continue;
        }

        const std::string name = inputDeviceName(fd);
        if (shouldIgnoreCursorDeviceName(name)) {
            ++stats.m_ignored;
            close(fd);
            continue;
        }

        if (!isRelativePointerDevice(fd)) {
            ++stats.m_notPointer;
            close(fd);
            continue;
        }

        struct epoll_event event;
        memset(&event, 0, sizeof(event));
        event.events = EPOLLIN | EPOLLERR | EPOLLHUP;
        event.data.fd = fd;
        if (epoll_ctl(epollFd, EPOLL_CTL_ADD, fd, &event) != 0) {
            close(fd);
            continue;
        }

        CursorInputDevice device;
        device.m_fd = fd;
        device.m_path = path;
        device.m_name = name;
        devices[fd] = device;
        ++stats.m_attached;
        LOG((CLOG_NOTE "cursorposd: attached %s name=%s", path.c_str(), name.c_str()));
    }
    closedir(dir);

    std::vector<int> removeFds;
    for (std::map<int, CursorInputDevice>::const_iterator it = devices.begin(); it != devices.end(); ++it) {
        if (seen.find(it->second.m_path) == seen.end()) {
            removeFds.push_back(it->first);
        }
    }

    for (std::vector<int>::const_iterator it = removeFds.begin(); it != removeFds.end(); ++it) {
        epoll_ctl(epollFd, EPOLL_CTL_DEL, *it, NULL);
        close(*it);
        devices.erase(*it);
    }
}

bool queryX11Cursor(int& x, int& y)
{
#if defined(__linux__)
    Display* display = XOpenDisplay(NULL);
    if (display == NULL) {
        return false;
    }

    Window root = DefaultRootWindow(display);
    Window rootReturn = None;
    Window childReturn = None;
    int windowX = 0;
    int windowY = 0;
    unsigned int mask = 0;
    const Bool ok = XQueryPointer(display, root, &rootReturn, &childReturn,
                                  &x, &y, &windowX, &windowY, &mask);
    XCloseDisplay(display);
    return ok;
#else
    return false;
#endif
}

bool readCommandOutput(const char* command, std::string& output)
{
    FILE* pipe = popen(command, "r");
    if (pipe == NULL) {
        return false;
    }

    output.clear();
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
        output += buffer;
    }

    const int status = pclose(pipe);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 && !output.empty();
}

bool environmentFlagEnabled(const char* name)
{
    const char* value = getenv(name);
    return value != NULL &&
           value[0] != '\0' &&
           strcmp(value, "0") != 0 &&
           strcmp(value, "false") != 0 &&
           strcmp(value, "FALSE") != 0;
}

bool isWaylandSession()
{
    const char* waylandDisplay = getenv("WAYLAND_DISPLAY");
    return waylandDisplay != NULL && waylandDisplay[0] != '\0';
}

bool parseCursorPosition(const std::string& text, int& x, int& y)
{
    const char* cursor = text.c_str();
    char* end = NULL;

    while (*cursor != '\0' && !isdigit(*cursor) && *cursor != '-') {
        ++cursor;
    }
    if (*cursor == '\0') {
        return false;
    }

    const long parsedX = strtol(cursor, &end, 10);
    if (end == cursor) {
        return false;
    }

    cursor = end;
    while (*cursor != '\0' && !isdigit(*cursor) && *cursor != '-') {
        ++cursor;
    }
    if (*cursor == '\0') {
        return false;
    }

    const long parsedY = strtol(cursor, &end, 10);
    if (end == cursor) {
        return false;
    }

    x = static_cast<int>(parsedX);
    y = static_cast<int>(parsedY);
    return true;
}

bool parseCursorOrigin(const std::string& text, int& x, int& y)
{
    char* end = NULL;
    const long parsedX = strtol(text.c_str(), &end, 10);
    if (end == text.c_str() || (*end != ',' && *end != 'x' && *end != ':')) {
        return false;
    }

    const char* yStart = end + 1;
    const long parsedY = strtol(yStart, &end, 10);
    if (end == yStart || *end != '\0') {
        return false;
    }

    x = static_cast<int>(parsedX);
    y = static_cast<int>(parsedY);
    return true;
}

bool queryHyprlandSocket(const std::string& socketPath, int& x, int& y)
{
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        return false;
    }

    struct timeval timeout;
    timeout.tv_sec = 0;
    timeout.tv_usec = 250000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (socketPath.size() >= sizeof(address.sun_path)) {
        close(fd);
        return false;
    }
    strncpy(address.sun_path, socketPath.c_str(), sizeof(address.sun_path) - 1);

    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(fd);
        return false;
    }

    const char command[] = "cursorpos";
    if (write(fd, command, sizeof(command) - 1) != static_cast<ssize_t>(sizeof(command) - 1)) {
        close(fd);
        return false;
    }

    std::string response;
    char buffer[256];
    while (true) {
        const ssize_t n = read(fd, buffer, sizeof(buffer));
        if (n > 0) {
            response.append(buffer, static_cast<size_t>(n));
            continue;
        }
        break;
    }

    close(fd);
    return parseCursorPosition(response, x, y);
}

bool queryHyprlandCursor(int& x, int& y)
{
    const char* signature = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (signature == NULL || signature[0] == '\0') {
        return false;
    }

    const char* runtimeDir = getenv("XDG_RUNTIME_DIR");
    if (runtimeDir != NULL && runtimeDir[0] != '\0') {
        const std::string socketPath =
            std::string(runtimeDir) + "/hypr/" + signature + "/.socket.sock";
        if (queryHyprlandSocket(socketPath, x, y)) {
            return true;
        }
    }

    const std::string legacySocketPath =
        std::string("/tmp/hypr/") + signature + "/.socket.sock";
    return queryHyprlandSocket(legacySocketPath, x, y);
}

bool queryGnomeShellCursor(int& x, int& y)
{
    std::string output;
    if (!readCommandOutput(
            "gdbus call --session "
            "--dest org.gnome.Shell "
            "--object-path /org/gnome/Shell "
            "--method org.gnome.Shell.Eval "
            "'JSON.stringify(global.get_pointer())' 2>/dev/null",
            output)) {
        return false;
    }

    const size_t marker = output.find("[");
    if (marker == std::string::npos) {
        return false;
    }

    return parseCursorPosition(output.substr(marker), x, y);
}

bool setGnomeShellCursor(int x, int y, int& actualX, int& actualY, bool& hasActual)
{
    char command[512];
    snprintf(command, sizeof(command),
        "gdbus call --session "
        "--dest org.gnome.Shell "
        "--object-path /org/gnome/Shell "
        "--method org.gnome.Shell.Eval "
        "'imports.gi.Clutter.get_default_backend().get_default_seat().warp_pointer(%d, %d); "
        "JSON.stringify(global.get_pointer())' 2>/dev/null",
        x,
        y);

    std::string output;
    hasActual = false;
    if (!readCommandOutput(command, output)) {
        return false;
    }

    const size_t marker = output.find("[");
    if (marker != std::string::npos) {
        hasActual = parseCursorPosition(output.substr(marker), actualX, actualY);
    }

    return true;
}

bool queryCompositorCursor(int& x, int& y, std::string& source)
{
    if (queryGnomeShellCursor(x, y)) {
        source = "gnome-shell-eval";
        return true;
    }

    if (queryHyprlandCursor(x, y)) {
        source = "hyprland";
        return true;
    }

    if ((!isWaylandSession() || environmentFlagEnabled("ETHERWAVER_ALLOW_XWAYLAND_CURSOR")) &&
        queryX11Cursor(x, y)) {
        source = "x11";
        return true;
    }

    source.clear();
    return false;
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
    , m_cursorPositionServer(false)
    , m_cursorPositionSocket("/tmp/etherwaver-cursor.sock")
    , m_cursorPositionOriginSet(false)
    , m_cursorPositionOriginX(0)
    , m_cursorPositionOriginY(0)
    , m_setCursorPosition(false)
    , m_setCursorPositionX(0)
    , m_setCursorPositionY(0)
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
    signal(SIGPIPE, SIG_IGN);

    if (m_config.m_setCursorPosition) {
        int actualX = 0;
        int actualY = 0;
        bool hasActual = false;
        if (!setGnomeShellCursor(
                m_config.m_setCursorPositionX,
                m_config.m_setCursorPositionY,
                actualX,
                actualY,
                hasActual)) {
            LOG((CLOG_ERR "cursorposd: failed to set cursor position to %d,%d",
                m_config.m_setCursorPositionX,
                m_config.m_setCursorPositionY));
            return 1;
        }

        if (hasActual) {
            LOG((CLOG_NOTE "cursorposd: set cursor position requested=%d,%d actual=%d,%d",
                m_config.m_setCursorPositionX,
                m_config.m_setCursorPositionY,
                actualX,
                actualY));
        }
        else {
            LOG((CLOG_NOTE "cursorposd: set cursor position requested=%d,%d",
                m_config.m_setCursorPositionX,
                m_config.m_setCursorPositionY));
        }
        return 0;
    }

    if (m_config.m_cursorPositionServer) {
        return runCursorPositionServer();
    }

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
        if (arg == "--cursor-position-server") {
            m_config.m_cursorPositionServer = true;
            continue;
        }
        if (arg == "--cursor-position-socket" && i + 1 < argc) {
            m_config.m_cursorPositionSocket = argv[++i];
            continue;
        }
        if (arg == "--cursor-position-origin" && i + 1 < argc) {
            if (!parseCursorOrigin(argv[++i],
                    m_config.m_cursorPositionOriginX,
                    m_config.m_cursorPositionOriginY)) {
                fprintf(stderr, "Invalid cursor origin, expected X,Y: %s\n", argv[i]);
                return false;
            }
            m_config.m_cursorPositionOriginSet = true;
            continue;
        }
        if ((arg == "--set-cursor-position" || arg == "--cursor-position-set") && i + 1 < argc) {
            if (!parseCursorOrigin(argv[++i],
                    m_config.m_setCursorPositionX,
                    m_config.m_setCursorPositionY)) {
                fprintf(stderr, "Invalid cursor position, expected X,Y: %s\n", argv[i]);
                return false;
            }
            m_config.m_setCursorPosition = true;
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
        "  --debug-events         Verbose event logging\n"
        "  --cursor-position-server        Run local cursor position server\n"
        "  --cursor-position-socket PATH   Socket path for cursor position server\n"
        "  --cursor-position-origin X,Y    Seed evdev cursor tracking at X,Y\n"
        "  --set-cursor-position X,Y       Move GNOME Shell cursor to X,Y using unsafe-mode Eval\n",
        argv0);
}

int
LinuxInputRouterDaemon::runCursorPositionServer()
{
    const int serverFd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (serverFd < 0) {
        LOG((CLOG_ERR "cursorposd: socket failed (%s)", strerror(errno)));
        return 1;
    }

    sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (m_config.m_cursorPositionSocket.size() >= sizeof(address.sun_path)) {
        LOG((CLOG_ERR "cursorposd: socket path too long"));
        close(serverFd);
        return 1;
    }
    strncpy(address.sun_path, m_config.m_cursorPositionSocket.c_str(), sizeof(address.sun_path) - 1);

    unlink(address.sun_path);
    if (bind(serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        LOG((CLOG_ERR "cursorposd: bind %s failed (%s)", address.sun_path, strerror(errno)));
        close(serverFd);
        return 1;
    }

    if (listen(serverFd, 8) != 0) {
        LOG((CLOG_ERR "cursorposd: listen failed (%s)", strerror(errno)));
        close(serverFd);
        unlink(address.sun_path);
        return 1;
    }

    int epollFd = epoll_create1(EPOLL_CLOEXEC);
    if (epollFd < 0) {
        LOG((CLOG_ERR "cursorposd: epoll_create1 failed (%s)", strerror(errno)));
        close(serverFd);
        unlink(address.sun_path);
        return 1;
    }

    struct epoll_event serverEvent;
    memset(&serverEvent, 0, sizeof(serverEvent));
    serverEvent.events = EPOLLIN;
    serverEvent.data.fd = serverFd;
    if (epoll_ctl(epollFd, EPOLL_CTL_ADD, serverFd, &serverEvent) != 0) {
        LOG((CLOG_ERR "cursorposd: epoll add server failed (%s)", strerror(errno)));
        close(epollFd);
        close(serverFd);
        unlink(address.sun_path);
        return 1;
    }

    int cursorX = 0;
    int cursorY = 0;
    std::string cursorSource;
    bool hasCursor = queryCompositorCursor(cursorX, cursorY, cursorSource);
    if (!hasCursor && m_config.m_cursorPositionOriginSet) {
        cursorX = m_config.m_cursorPositionOriginX;
        cursorY = m_config.m_cursorPositionOriginY;
        cursorSource = "evdev-origin";
        hasCursor = true;
    }
    std::map<int, CursorInputDevice> cursorDevices;
    CursorDeviceScanStats scanStats;
    refreshCursorDevices(cursorDevices, epollFd, scanStats);

    LOG((CLOG_NOTE
        "cursorposd: listening on %s devices=%lu candidates=%d denied=%d failed=%d ignored=%d notPointer=%d attached=%d initial=%s %d,%d",
        address.sun_path,
        static_cast<unsigned long>(cursorDevices.size()),
        scanStats.m_candidates,
        scanStats.m_openDenied,
        scanStats.m_openFailed,
        scanStats.m_ignored,
        scanStats.m_notPointer,
        scanStats.m_attached,
        hasCursor ? cursorSource.c_str() : "no",
        cursorX,
        cursorY));

    std::array<struct epoll_event, 32> events;
    while (!g_stopRequested) {
        const int ready = epoll_wait(epollFd, events.data(), static_cast<int>(events.size()), 1000);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOG((CLOG_ERR "cursorposd: epoll_wait failed (%s)", strerror(errno)));
            break;
        }

        if (ready == 0) {
            refreshCursorDevices(cursorDevices, epollFd, scanStats);
            continue;
        }

        for (int i = 0; i < ready; ++i) {
            const int fd = events[i].data.fd;
            if (fd == serverFd) {
                const int clientFd = accept4(serverFd, NULL, NULL, SOCK_CLOEXEC);
                if (clientFd < 0) {
                    continue;
                }

                std::string compositorSource;
                if (queryCompositorCursor(cursorX, cursorY, compositorSource)) {
                    cursorSource = compositorSource;
                    hasCursor = true;
                }
                else if (!hasCursor && m_config.m_cursorPositionOriginSet) {
                    cursorX = m_config.m_cursorPositionOriginX;
                    cursorY = m_config.m_cursorPositionOriginY;
                    cursorSource = "evdev-origin";
                    hasCursor = true;
                }
                else if (!hasCursor) {
                    cursorSource = "unavailable";
                }

                char response[200];
                snprintf(response, sizeof(response),
                    "%s %d %d %s devices=%lu candidates=%d denied=%d failed=%d ignored=%d notPointer=%d attached=%d\n",
                    hasCursor ? "OK" : "ERR",
                    cursorX,
                    cursorY,
                    hasCursor ? cursorSource.c_str() : "cursor unavailable",
                    static_cast<unsigned long>(cursorDevices.size()),
                    scanStats.m_candidates,
                    scanStats.m_openDenied,
                    scanStats.m_openFailed,
                    scanStats.m_ignored,
                    scanStats.m_notPointer,
                    scanStats.m_attached);
                write(clientFd, response, strlen(response));
                close(clientFd);
                continue;
            }

            std::map<int, CursorInputDevice>::iterator deviceIt = cursorDevices.find(fd);
            if (deviceIt == cursorDevices.end()) {
                continue;
            }

            if ((events[i].events & (EPOLLERR | EPOLLHUP)) != 0) {
                epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
                cursorDevices.erase(deviceIt);
                continue;
            }

            while (true) {
                input_event event;
                const ssize_t n = read(fd, &event, sizeof(event));
                if (n == static_cast<ssize_t>(sizeof(event))) {
                    if (event.type == EV_REL) {
                        if (event.code == REL_X) {
                            cursorX += event.value;
                            hasCursor = true;
                            if (cursorSource.empty() || cursorSource == "unavailable" ||
                                cursorSource == "evdev-origin") {
                                cursorSource = "evdev";
                            }
                        }
                        else if (event.code == REL_Y) {
                            cursorY += event.value;
                            hasCursor = true;
                            if (cursorSource.empty() || cursorSource == "unavailable" ||
                                cursorSource == "evdev-origin") {
                                cursorSource = "evdev";
                            }
                        }
                    }
                    continue;
                }

                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    break;
                }

                epoll_ctl(epollFd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
                cursorDevices.erase(deviceIt);
                break;
            }
        }
    }

    closeCursorDevices(cursorDevices, epollFd);
    close(epollFd);
    close(serverFd);
    unlink(address.sun_path);
    return 0;
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
