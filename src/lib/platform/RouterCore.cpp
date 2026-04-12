#include "platform/RouterCore.h"

#include "base/Log.h"

#if defined(__linux__)

#include <chrono>

#include <linux/input.h>
#include <linux/input-event-codes.h>

namespace {

long long monotonic_ms()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

UhidEdgeTransitionService::Config make_edge_config(const RouterCore::Config& config)
{
    UhidEdgeTransitionService::Config edgeConfig;
    edgeConfig.m_leftThreshold = config.m_edgeThreshold;
    edgeConfig.m_rightThreshold = config.m_edgeThreshold;
    edgeConfig.m_topThreshold = config.m_edgeThreshold;
    edgeConfig.m_bottomThreshold = config.m_edgeThreshold;
    edgeConfig.m_debugLogging = config.m_debugEvents;
    return edgeConfig;
}

} // namespace

EtherwaverBridge::EtherwaverBridge(bool debugEvents)
    : m_debugEvents(debugEvents)
    , m_running(false)
{
}

EtherwaverBridge::~EtherwaverBridge()
{
    stop();
}

void
EtherwaverBridge::start()
{
    if (m_running.exchange(true)) {
        return;
    }

    m_thread = std::thread(&EtherwaverBridge::workerLoop, this);
}

void
EtherwaverBridge::stop()
{
    if (!m_running.exchange(false)) {
        return;
    }

    m_cv.notify_all();
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void
EtherwaverBridge::switchSystem(RemoteTarget target)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    QueuedEvent queued;
    queued.m_isSwitch = true;
    queued.m_target = target;
    queued.m_event = NULL;
    m_queue.push(queued);
    m_cv.notify_one();
}

void
EtherwaverBridge::sendInput(const input_event& event, const std::string& devicePath)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    QueuedEvent queued;
    queued.m_isSwitch = false;
    queued.m_target = RemoteTarget::None;
    queued.m_event = new input_event(event);
    queued.m_devicePath = devicePath;
    m_queue.push(queued);
    m_cv.notify_one();
}

void
EtherwaverBridge::workerLoop()
{
    while (m_running.load()) {
        QueuedEvent queued;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this]() { return !m_queue.empty() || !m_running.load(); });
            if (!m_running.load() && m_queue.empty()) {
                break;
            }
            queued = m_queue.front();
            m_queue.pop();
        }

        if (queued.m_isSwitch) {
            LOG((CLOG_NOTE "etherwaver-bridge: active remote target=%s", targetName(queued.m_target)));
            continue;
        }

        if (m_debugEvents && queued.m_event != NULL) {
            LOG((CLOG_DEBUG1
                "etherwaver-bridge: queued remote event path=%s type=%u code=%u value=%d",
                queued.m_devicePath.c_str(),
                queued.m_event->type,
                queued.m_event->code,
                queued.m_event->value));
        }
        delete queued.m_event;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    while (!m_queue.empty()) {
        delete m_queue.front().m_event;
        m_queue.pop();
    }
}

const char*
EtherwaverBridge::targetName(RemoteTarget target) const
{
    switch (target) {
    case RemoteTarget::Left: return "left";
    case RemoteTarget::Right: return "right";
    case RemoteTarget::Up: return "up";
    case RemoteTarget::Down: return "down";
    case RemoteTarget::None: break;
    }
    return "none";
}

RouterCore::Config::Config()
    : m_uhidName("Etherwaver")
    , m_screenWidth(1920)
    , m_screenHeight(1080)
    , m_edgeThreshold(40)
    , m_debugEvents(false)
{
}

RouterCore::FailsafeTracker::FailsafeTracker()
{
}

bool
RouterCore::FailsafeTracker::matchesCtrl(unsigned short code) const
{
    return (code == KEY_LEFTCTRL || code == KEY_RIGHTCTRL);
}

void
RouterCore::FailsafeTracker::record(std::deque<long long>& history, long long nowMs, size_t requiredCount, long long windowMs)
{
    history.push_back(nowMs);
    while (!history.empty() && (nowMs - history.front()) > windowMs) {
        history.pop_front();
    }
    while (history.size() > requiredCount) {
        history.pop_front();
    }
}

bool
RouterCore::FailsafeTracker::observe(const input_event& event)
{
    if (event.type != EV_KEY || event.value != 1) {
        return false;
    }

    const long long nowMs = monotonic_ms();
    if (matchesCtrl(event.code)) {
        record(m_ctrlPresses, nowMs, 3, 1000);
        if (m_ctrlPresses.size() == 3 && (m_ctrlPresses.back() - m_ctrlPresses.front()) <= 1000) {
            m_ctrlPresses.clear();
            return true;
        }
    }

    if (event.code == KEY_F12) {
        record(m_f12Presses, nowMs, 2, 1000);
        if (m_f12Presses.size() == 2 && (m_f12Presses.back() - m_f12Presses.front()) <= 1000) {
            m_f12Presses.clear();
            return true;
        }
    }

    return false;
}

RouterCore::RouterCore(const Config& config, const std::function<void(bool)>& grabControlCallback)
    : m_config(config)
    , m_grabControlCallback(grabControlCallback)
    , m_edgeService(make_edge_config(config))
    , m_bridge(config.m_debugEvents)
    , m_activeSystem(ActiveSystem::Local)
    , m_suspended(false)
    , m_pendingDx(0)
    , m_pendingDy(0)
    , m_pendingWheelX(0)
    , m_pendingWheelY(0)
{
    m_edgeService.setScreenGeometry(0, 0, config.m_screenWidth, config.m_screenHeight);
    m_edgeService.setTransitionHandler(this);
}

RouterCore::~RouterCore()
{
    stop();
}

bool
RouterCore::start()
{
    m_bridge.start();
    const std::string keyboardName = m_config.m_uhidName + " Keyboard";
    const std::string mouseName = m_config.m_uhidName + " Mouse";

    if (!m_keyboard.start(keyboardName)) {
        return false;
    }
    if (!m_mouse.start(mouseName)) {
        m_keyboard.stop();
        return false;
    }

    LOG((CLOG_NOTE
        "router-core: started screen=%dx%d threshold=%d",
        m_config.m_screenWidth,
        m_config.m_screenHeight,
        m_config.m_edgeThreshold));
    return true;
}

void
RouterCore::stop()
{
    flushPendingRelative();
    releaseLocalState();
    m_mouse.stop();
    m_keyboard.stop();
    m_bridge.stop();
}

bool
RouterCore::routingSuspended() const
{
    return m_suspended;
}

bool
RouterCore::isMouseButton(unsigned short code) const
{
    return (code >= BTN_MOUSE && code <= BTN_TASK);
}

RouterCore::ActiveSystem
RouterCore::remoteForDirection(Direction direction) const
{
    switch (direction) {
    case kLeft: return ActiveSystem::RemoteLeft;
    case kRight: return ActiveSystem::RemoteRight;
    case kTop: return ActiveSystem::RemoteUp;
    case kBottom: return ActiveSystem::RemoteDown;
    }
    return ActiveSystem::Local;
}

IUhidEdgeTransitionHandler::Direction
RouterCore::opposite(Direction direction) const
{
    switch (direction) {
    case kLeft: return kRight;
    case kRight: return kLeft;
    case kTop: return kBottom;
    case kBottom: return kTop;
    }
    return kLeft;
}

EtherwaverBridge::RemoteTarget
RouterCore::bridgeTarget(ActiveSystem target) const
{
    switch (target) {
    case ActiveSystem::RemoteLeft: return EtherwaverBridge::RemoteTarget::Left;
    case ActiveSystem::RemoteRight: return EtherwaverBridge::RemoteTarget::Right;
    case ActiveSystem::RemoteUp: return EtherwaverBridge::RemoteTarget::Up;
    case ActiveSystem::RemoteDown: return EtherwaverBridge::RemoteTarget::Down;
    case ActiveSystem::Local: break;
    }
    return EtherwaverBridge::RemoteTarget::None;
}

const char*
RouterCore::systemName(ActiveSystem system) const
{
    switch (system) {
    case ActiveSystem::Local: return "local";
    case ActiveSystem::RemoteLeft: return "remote-left";
    case ActiveSystem::RemoteRight: return "remote-right";
    case ActiveSystem::RemoteUp: return "remote-up";
    case ActiveSystem::RemoteDown: return "remote-down";
    }
    return "unknown";
}

void
RouterCore::routeEvent(const input_event& event, const std::string& devicePath)
{
    if (m_failsafeTracker.observe(event)) {
        activateFailsafeUnlock();
        return;
    }

    if (m_suspended) {
        return;
    }

    if (m_config.m_debugEvents) {
        LOG((CLOG_DEBUG1
            "router-core: event path=%s type=%u code=%u value=%d target=%s",
            devicePath.c_str(),
            event.type,
            event.code,
            event.value,
            systemName(m_activeSystem)));
    }

    if (event.type == EV_REL) {
        switch (event.code) {
        case REL_X:
            m_pendingDx += event.value;
            break;
        case REL_Y:
            m_pendingDy += event.value;
            break;
        case REL_HWHEEL:
            m_pendingWheelX += event.value;
            break;
        case REL_WHEEL:
            m_pendingWheelY += event.value;
            break;
        default:
            if (m_activeSystem != ActiveSystem::Local) {
                m_bridge.sendInput(event, devicePath);
            }
            break;
        }

        if (m_activeSystem != ActiveSystem::Local) {
            m_bridge.sendInput(event, devicePath);
        }
        return;
    }

    if (event.type == EV_SYN && event.code == SYN_REPORT) {
        flushPendingRelative();
        if (m_activeSystem != ActiveSystem::Local) {
            m_bridge.sendInput(event, devicePath);
        }
        return;
    }

    if (m_activeSystem != ActiveSystem::Local) {
        m_bridge.sendInput(event, devicePath);
        return;
    }

    if (event.type == EV_KEY) {
        if (isMouseButton(event.code)) {
            m_mouse.handleButtonEvent(event.code, event.value);
        }
        else {
            m_keyboard.handleKeyEvent(event.code, event.value != 0 ? 1 : 0);
        }
    }
}

void
RouterCore::onTransition(Direction direction)
{
    switchSystem(direction);
}

void
RouterCore::switchSystem(Direction direction)
{
    const ActiveSystem previous = m_activeSystem;
    if (m_activeSystem == ActiveSystem::Local) {
        m_activeSystem = remoteForDirection(direction);
    }
    else {
        const ActiveSystem inbound = remoteForDirection(opposite(direction));
        if (m_activeSystem == inbound) {
            m_activeSystem = ActiveSystem::Local;
        }
        else {
            m_activeSystem = remoteForDirection(direction);
        }
    }

    flushPendingRelative();
    releaseLocalState();
    m_bridge.switchSystem(bridgeTarget(m_activeSystem));
    LOG((CLOG_NOTE
        "router-core: switched from %s to %s",
        systemName(previous),
        systemName(m_activeSystem)));
}

void
RouterCore::injectAltTab()
{
    if (m_suspended) {
        return;
    }

    m_keyboard.pressChord({KEY_LEFTALT, KEY_TAB});
    m_keyboard.releaseKey(KEY_TAB);
    m_keyboard.releaseKey(KEY_LEFTALT);
}

void
RouterCore::injectSuper()
{
    if (m_suspended) {
        return;
    }

    m_keyboard.tapKey(KEY_LEFTMETA);
}

void
RouterCore::flushPendingRelative()
{
    if (m_pendingDx != 0 || m_pendingDy != 0) {
        m_edgeService.onRelativeMouseMotion(m_pendingDx, m_pendingDy);
        if (m_activeSystem == ActiveSystem::Local) {
            m_mouse.moveRelative(m_pendingDx, m_pendingDy);
        }
    }

    if ((m_pendingWheelX != 0 || m_pendingWheelY != 0) && m_activeSystem == ActiveSystem::Local) {
        m_mouse.wheel(m_pendingWheelX, m_pendingWheelY);
    }

    m_pendingDx = 0;
    m_pendingDy = 0;
    m_pendingWheelX = 0;
    m_pendingWheelY = 0;
}

void
RouterCore::releaseLocalState()
{
    m_keyboard.clear();
    m_mouse.clear();
}

void
RouterCore::activateFailsafeUnlock()
{
    if (m_suspended) {
        return;
    }

    m_suspended = true;
    flushPendingRelative();
    releaseLocalState();
    if (m_grabControlCallback) {
        m_grabControlCallback(false);
    }
    LOG((CLOG_WARN "router-core: failsafe unlock triggered, all grabs released"));
}

#endif
