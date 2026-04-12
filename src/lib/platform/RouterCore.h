#pragma once

#include "platform/UhidEdgeTransitionService.h"
#include "platform/UhidInputDevices.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

struct input_event;

class EtherwaverBridge {
public:
    enum class RemoteTarget {
        None,
        Left,
        Right,
        Up,
        Down
    };

    explicit EtherwaverBridge(bool debugEvents);
    ~EtherwaverBridge();

    void start();
    void stop();
    void switchSystem(RemoteTarget target);
    void sendInput(const input_event& event, const std::string& devicePath);

private:
    struct QueuedEvent {
        bool m_isSwitch;
        RemoteTarget m_target;
        input_event* m_event;
        std::string m_devicePath;
    };

    void workerLoop();
    const char* targetName(RemoteTarget target) const;

private:
    bool m_debugEvents;
    std::atomic<bool> m_running;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::queue<QueuedEvent> m_queue;
};

class RouterCore : public IUhidEdgeTransitionHandler {
public:
    struct Config {
        std::string m_uhidName;
        int m_screenWidth;
        int m_screenHeight;
        int m_edgeThreshold;
        bool m_debugEvents;

        Config();
    };

    explicit RouterCore(const Config& config, const std::function<void(bool)>& grabControlCallback);
    ~RouterCore();

    bool start();
    void stop();

    void routeEvent(const input_event& event, const std::string& devicePath);
    void onTransition(Direction direction) override;

    void switchSystem(Direction direction);
    void injectAltTab();
    void injectSuper();
    bool routingSuspended() const;

private:
    enum class ActiveSystem {
        Local,
        RemoteLeft,
        RemoteRight,
        RemoteUp,
        RemoteDown
    };

    class FailsafeTracker {
    public:
        FailsafeTracker();
        bool observe(const input_event& event);

    private:
        bool matchesCtrl(unsigned short code) const;
        void record(std::deque<long long>& history, long long nowMs, size_t requiredCount, long long windowMs);

    private:
        std::deque<long long> m_ctrlPresses;
        std::deque<long long> m_f12Presses;
    };

    bool isMouseButton(unsigned short code) const;
    ActiveSystem remoteForDirection(Direction direction) const;
    Direction opposite(Direction direction) const;
    EtherwaverBridge::RemoteTarget bridgeTarget(ActiveSystem target) const;
    const char* systemName(ActiveSystem system) const;
    void flushPendingRelative();
    void releaseLocalState();
    void activateFailsafeUnlock();

private:
    Config m_config;
    std::function<void(bool)> m_grabControlCallback;
    UhidKeyboardDevice m_keyboard;
    UhidMouseDevice m_mouse;
    UhidEdgeTransitionService m_edgeService;
    EtherwaverBridge m_bridge;
    FailsafeTracker m_failsafeTracker;
    ActiveSystem m_activeSystem;
    bool m_suspended;
    int m_pendingDx;
    int m_pendingDy;
    int m_pendingWheelX;
    int m_pendingWheelY;
};
