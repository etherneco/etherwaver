#pragma once

#include "platform/InputGrabber.h"
#include "platform/RouterCore.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <linux/input.h>

class LinuxInputRouterDaemon {
public:
    struct Config {
        std::string m_uhidName;
        int m_screenWidth;
        int m_screenHeight;
        int m_edgeThreshold;
        int m_rescanIntervalMs;
        std::string m_logLevel;
        bool m_debugEvents;

        Config();
    };

    LinuxInputRouterDaemon();
    ~LinuxInputRouterDaemon();

    int run(int argc, char** argv);

private:
    struct DeviceRecord {
        std::string m_path;
        std::string m_name;
        int m_fd;
        bool m_keyboard;
        bool m_pointer;
        bool m_touchpad;
        bool m_grabbed;
    };

    bool parseArgs(int argc, char** argv);
    void printUsage(const char* argv0) const;
    bool start();
    void stop();
    void inputLoop();
    void routerLoop();
    void refreshDevices();
    void attachDevice(const std::string& path);
    void detachDevice(int fd);
    bool classifyDevice(int fd, DeviceRecord& record) const;
    std::string deviceName(int fd) const;
    bool shouldIgnoreDeviceName(const std::string& name) const;
    void setExclusiveRouting(bool exclusive);
    void enqueue(const input_event& event, const std::string& devicePath);
    bool readDeviceEvents(DeviceRecord& device);
    int wakeFd() const;

private:
    struct RoutedEvent {
        input_event m_event;
        std::string m_devicePath;
    };

    Config m_config;
    std::unique_ptr<RouterCore> m_router;
    InputGrabber m_grabber;
    std::atomic<bool> m_running;
    int m_epollFd;
    int m_wakePipe[2];
    std::thread m_inputThread;
    std::thread m_routerThread;
    std::mutex m_devicesMutex;
    std::map<int, DeviceRecord> m_devices;
    std::mutex m_queueMutex;
    std::condition_variable m_queueCv;
    std::deque<RoutedEvent> m_queue;
};
