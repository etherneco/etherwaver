/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "client/InputBackendFactory.h"

#include "client/IInputBackend.h"
#include "client/SoftCursorPositioner.h"
#include "barrier/ClientArgs.h"
#include "barrier/Screen.h"
#include "base/Log.h"
#include "platform/UhidServer.h"

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__linux__)
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace {

std::string uhidDebugStatusPath()
{
#if defined(__linux__)
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "/tmp/etherwaver-uhid-cursor-%ld.status", static_cast<long>(getuid()));
    return buffer;
#else
    return "";
#endif
}

std::string debugBoundsPidPath()
{
#if defined(__linux__)
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "/tmp/etherwaver-debug-bounds-%ld.pid", static_cast<long>(getuid()));
    return buffer;
#else
    return "";
#endif
}

void writeUhidDebugStatusLine(const char* line)
{
    const std::string path = uhidDebugStatusPath();
    if (path.empty()) {
        return;
    }

    FILE* file = fopen(path.c_str(), "w");
    if (file == NULL) {
        return;
    }

    fprintf(file, "%s\n", line != NULL ? line : "unavailable");
    fclose(file);
}

bool debugBoundsOverlayEnabled()
{
    const char* value = getenv("ETHERWAVER_DEBUG_BOUNDS_OVERLAY");
    if (value == NULL || value[0] == '\0') {
        return true;
    }

    return strcmp(value, "0") != 0 &&
           strcmp(value, "false") != 0 &&
           strcmp(value, "FALSE") != 0;
}

std::string executableDir()
{
#if defined(__linux__)
    char buffer[4096];
    const ssize_t length = readlink("/proc/self/exe", buffer, sizeof(buffer) - 1);
    if (length <= 0) {
        return "";
    }

    buffer[length] = '\0';
    std::string path(buffer);
    const std::string::size_type slash = path.find_last_of('/');
    return slash == std::string::npos ? "" : path.substr(0, slash);
#else
    return "";
#endif
}

std::string debugBoundsHelperPath()
{
    const char* configured = getenv("ETHERWAVER_DEBUG_BOUNDS_HELPER");
    if (configured != NULL && configured[0] != '\0') {
        return configured;
    }

    const std::string dir = executableDir();
    if (!dir.empty()) {
        return dir + "/etherwaver-debug-bounds";
    }

    return "etherwaver-debug-bounds";
}

void preserveEnvironmentValue(const char* name, std::string& value)
{
    const char* current = getenv(name);
    value = current != NULL ? current : "";
}

#if defined(__linux__)
void restoreEnvironmentValue(const char* name, const std::string& value)
{
    if (!value.empty()) {
        setenv(name, value.c_str(), 1);
    }
}
#endif

void useCleanDebugBoundsEnvironment()
{
#if defined(__linux__)
    std::string home;
    std::string user;
    std::string logname;
    std::string display;
    std::string xauthority;
    std::string xdgRuntimeDir;
    std::string dbusSessionBusAddress;

    preserveEnvironmentValue("HOME", home);
    preserveEnvironmentValue("USER", user);
    preserveEnvironmentValue("LOGNAME", logname);
    preserveEnvironmentValue("DISPLAY", display);
    preserveEnvironmentValue("XAUTHORITY", xauthority);
    preserveEnvironmentValue("XDG_RUNTIME_DIR", xdgRuntimeDir);
    preserveEnvironmentValue("DBUS_SESSION_BUS_ADDRESS", dbusSessionBusAddress);

    clearenv();
    setenv("PATH", "/usr/bin:/bin", 1);
    setenv("QT_QPA_PLATFORM", "xcb", 1);

    restoreEnvironmentValue("HOME", home);
    restoreEnvironmentValue("USER", user);
    restoreEnvironmentValue("LOGNAME", logname);
    restoreEnvironmentValue("DISPLAY", display);
    restoreEnvironmentValue("XAUTHORITY", xauthority);
    restoreEnvironmentValue("XDG_RUNTIME_DIR", xdgRuntimeDir);
    restoreEnvironmentValue("DBUS_SESSION_BUS_ADDRESS", dbusSessionBusAddress);
#endif
}

void killDebugBoundsHelper()
{
#if defined(__linux__)
    const std::string path = debugBoundsPidPath();
    if (path.empty()) {
        return;
    }

    FILE* file = fopen(path.c_str(), "r");
    if (file == NULL) {
        return;
    }

    long pid = 0;
    const int parsed = fscanf(file, "%ld", &pid);
    fclose(file);
    unlink(path.c_str());
    if (parsed == 1 && pid > 1) {
        kill(static_cast<pid_t>(pid), SIGTERM);
    }
#endif
}

#if defined(__linux__)
void writeDebugBoundsPid(pid_t pid)
{
    const std::string path = debugBoundsPidPath();
    if (path.empty()) {
        return;
    }

    FILE* file = fopen(path.c_str(), "w");
    if (file == NULL) {
        return;
    }

    fprintf(file, "%ld\n", static_cast<long>(pid));
    fclose(file);
}
#endif

void startDebugBoundsHelper(SInt32 x, SInt32 y, SInt32 w, SInt32 h)
{
#if defined(__linux__)
    if (!debugBoundsOverlayEnabled() || w <= 0 || h <= 0) {
        return;
    }

    killDebugBoundsHelper();

    char xArg[32];
    char yArg[32];
    char wArg[32];
    char hArg[32];
    snprintf(xArg, sizeof(xArg), "%d", x);
    snprintf(yArg, sizeof(yArg), "%d", y);
    snprintf(wArg, sizeof(wArg), "%d", w);
    snprintf(hArg, sizeof(hArg), "%d", h);

    const std::string helper = debugBoundsHelperPath();
    const pid_t pid = fork();
    if (pid < 0) {
        LOG((CLOG_WARN "debug-bounds: failed to fork helper"));
        return;
    }

    if (pid == 0) {
        useCleanDebugBoundsEnvironment();
        execl(helper.c_str(), helper.c_str(), xArg, yArg, wArg, hArg, static_cast<char*>(NULL));
        execlp("etherwaver-debug-bounds", "etherwaver-debug-bounds",
               xArg, yArg, wArg, hArg, static_cast<char*>(NULL));
        _exit(127);
    }

    writeDebugBoundsPid(pid);
    LOG((CLOG_INFO "debug-bounds: started helper pid=%ld bounds=%d,%d %dx%d",
        static_cast<long>(pid), x, y, w, h));
#else
    (void)x;
    (void)y;
    (void)w;
    (void)h;
#endif
}

void showDebugBoundsOverlay(SInt32 x, SInt32 y, SInt32 w, SInt32 h)
{
    startDebugBoundsHelper(x, y, w, h);
}

void hideDebugBoundsOverlay()
{
    killDebugBoundsHelper();
}

class ScreenInputBackend : public IInputBackend {
public:
    explicit ScreenInputBackend(barrier::Screen* screen)
        : m_screen(screen)
    {
        assert(m_screen != NULL);
    }

    void enter(SInt32 xAbs, SInt32 yAbs) override
    {
        m_screen->mouseMove(xAbs, yAbs);
    }

    void leave() override
    {
    }

    bool managesCursorVisibility() const override
    {
        return false;
    }

    bool movesCursorAfterScreenEnter() const override
    {
        return true;
    }

    void keyDown(KeyID id, KeyModifierMask mask, KeyButton button) override
    {
        m_screen->keyDown(id, mask, button);
    }

    void keyRepeat(KeyID id, KeyModifierMask mask, SInt32 count, KeyButton button) override
    {
        m_screen->keyRepeat(id, mask, count, button);
    }

    void keyUp(KeyID id, KeyModifierMask mask, KeyButton button) override
    {
        m_screen->keyUp(id, mask, button);
    }

    void mouseDown(ButtonID id) override
    {
        m_screen->mouseDown(id);
    }

    void mouseUp(ButtonID id) override
    {
        m_screen->mouseUp(id);
    }

    void mouseMove(SInt32 xAbs, SInt32 yAbs) override
    {
        m_screen->mouseMove(xAbs, yAbs);
    }

    void mouseRelativeMove(SInt32 dx, SInt32 dy) override
    {
        m_screen->mouseRelativeMove(dx, dy);
    }

    void mouseWheel(SInt32 xDelta, SInt32 yDelta) override
    {
        m_screen->mouseWheel(xDelta, yDelta);
    }

private:
    barrier::Screen* m_screen;
};

class ScreenCursorPositionProvider : public ICursorPositionProvider {
public:
    explicit ScreenCursorPositionProvider(barrier::Screen* screen)
        : m_screen(screen)
    {
        assert(m_screen != NULL);
    }

    void getCursorPos(SInt32& x, SInt32& y) override
    {
        m_screen->getCursorPos(x, y);
    }

private:
    barrier::Screen* m_screen;
};

class UhidCursorMotionSink : public ICursorMotionSink {
public:
    explicit UhidCursorMotionSink(UhidServer* server)
        : m_server(server)
    {
        assert(m_server != NULL);
    }

    void primeAbsolutePosition(SInt32 x, SInt32 y) override
    {
        m_server->primeAbsolutePosition(x, y);
    }

    void mouseMoveAbsolute(SInt32 x, SInt32 y) override
    {
        m_server->mouseMoveAbsolute(x, y);
    }

private:
    UhidServer* m_server;
};

class UhidInputBackend : public IInputBackend {
public:
    UhidInputBackend(barrier::Screen* screen, const String& deviceName)
        : m_screen(screen)
        , m_started(false)
        , m_uhidServer(new UhidServer())
        , m_cursorPositionProvider(new ScreenCursorPositionProvider(screen))
        , m_cursorMotionSink(new UhidCursorMotionSink(m_uhidServer.get()))
        , m_hasActiveBounds(false)
        , m_activeX(0)
        , m_activeY(0)
        , m_activeW(0)
        , m_activeH(0)
        , m_cursorX(0)
        , m_cursorY(0)
        , m_hasTrackedCursorPos(false)
    {
        assert(m_screen != NULL);
        m_started = m_uhidServer->start(deviceName);
        writeDebugStatus(m_started ? "started" : "start-failed");
    }

    bool started() const
    {
        return m_started;
    }

    void enter(SInt32 xAbs, SInt32 yAbs) override
    {
        const SInt32 prevX = m_cursorX;
        const SInt32 prevY = m_cursorY;
        const bool useSelfTracked = m_hasTrackedCursorPos;

        m_uhidServer->clearInputState();
        m_hasActiveBounds = false;
        refreshScreens();
        updateActiveBoundsForPoint(xAbs, yAbs);
        clampToActiveBounds(xAbs, yAbs);

        if (useSelfTracked) {
            // Under Wayland, XQueryPointer (used by softSetCursorPos) returns stale data
            // because the cursor is moved via UHID HID events — XWayland only sees cursor
            // position when the pointer is over an XWayland surface. Using our own tracked
            // position as the baseline gives the correct relative delta to the target.
            m_uhidServer->primeAbsolutePosition(prevX, prevY);
            m_uhidServer->mouseMoveAbsolute(xAbs, yAbs);
            m_cursorX = xAbs;
            m_cursorY = yAbs;
            LOG((CLOG_DEBUG2
                "uhid: soft set cursor reason=enter current=%d,%d target=%d,%d delta=%d,%d bounds=%d,%d %dx%d",
                prevX, prevY, xAbs, yAbs, xAbs - prevX, yAbs - prevY,
                m_activeX, m_activeY, m_activeW, m_activeH));
        } else {
            // First enter since startup: XQueryPointer is still accurate because no
            // UHID events have been sent yet. Use native platform warp first
            // (XTestFakeMotionEvent) — works on X11 and Wayland with unsafe mode
            // enabled. softSetCursorPos then reads back the actual position and
            // issues a corrective UHID delta for any remaining error.
            m_screen->mouseMove(xAbs, yAbs);
            softSetCursorPos(xAbs, yAbs, "enter");
            m_hasTrackedCursorPos = true;
        }

        LOG((CLOG_INFO "uhid: enter cursor at %d,%d bounds=%d,%d %dx%d",
            xAbs, yAbs, m_activeX, m_activeY, m_activeW, m_activeH));
        if (m_hasActiveBounds) {
            showDebugBoundsOverlay(m_activeX, m_activeY, m_activeW, m_activeH);
        }
        writeDebugStatus("enter");
    }

    void leave() override
    {
        m_uhidServer->clearInputState();
        m_hasActiveBounds = false;
        m_screens.clear();
        // Intentionally keep m_cursorX/m_cursorY: enter() uses them on re-entry
        // to compute the correct relative-motion delta without querying XQueryPointer.
        hideDebugBoundsOverlay();
        writeDebugStatus("leave");
    }

    bool managesCursorVisibility() const override
    {
        return true;
    }

    bool movesCursorAfterScreenEnter() const override
    {
        return false;
    }

    void keyDown(KeyID id, KeyModifierMask mask, KeyButton) override
    {
        m_uhidServer->keyDown(id, mask);
    }

    void keyRepeat(KeyID id, KeyModifierMask mask, SInt32 count, KeyButton) override
    {
        m_uhidServer->keyRepeat(id, mask, count);
    }

    void keyUp(KeyID id, KeyModifierMask mask, KeyButton) override
    {
        m_uhidServer->keyUp(id, mask);
    }

    void mouseDown(ButtonID id) override
    {
        m_uhidServer->mouseDown(id);
    }

    void mouseUp(ButtonID id) override
    {
        m_uhidServer->mouseUp(id);
    }

    void mouseMove(SInt32 xAbs, SInt32 yAbs) override
    {
        const SInt32 requestedX = xAbs;
        const SInt32 requestedY = yAbs;
        clampToActiveBounds(xAbs, yAbs);
        if (requestedX != xAbs || requestedY != yAbs) {
            LOG((CLOG_DEBUG2
                "uhid: clamp absolute cursor requested=%d,%d clamped=%d,%d bounds=%d,%d %dx%d",
                requestedX, requestedY, xAbs, yAbs,
                m_activeX, m_activeY, m_activeW, m_activeH));
        }
        m_cursorX = xAbs;
        m_cursorY = yAbs;
        m_uhidServer->mouseMoveAbsolute(xAbs, yAbs);
        writeDebugStatus("absolute");
    }

    void mouseRelativeMove(SInt32 dx, SInt32 dy) override
    {
        SInt32 xAbs = m_cursorX + dx;
        SInt32 yAbs = m_cursorY + dy;
        const SInt32 requestedX = xAbs;
        const SInt32 requestedY = yAbs;
        clampToActiveBounds(xAbs, yAbs);
        if (requestedX != xAbs || requestedY != yAbs) {
            LOG((CLOG_DEBUG2
                "uhid: clamp relative cursor delta=%d,%d requested=%d,%d clamped=%d,%d bounds=%d,%d %dx%d",
                dx, dy, requestedX, requestedY, xAbs, yAbs,
                m_activeX, m_activeY, m_activeW, m_activeH));
        }
        m_cursorX = xAbs;
        m_cursorY = yAbs;
        m_uhidServer->mouseMoveAbsolute(xAbs, yAbs);
        writeDebugStatus("relative");
    }

    void mouseWheel(SInt32 xDelta, SInt32 yDelta) override
    {
        m_uhidServer->mouseWheel(xDelta, yDelta);
    }

private:
    bool contains(SInt32 x, SInt32 y,
                  SInt32 rx, SInt32 ry, SInt32 rw, SInt32 rh) const
    {
        return (rw > 0 && rh > 0 &&
                x >= rx && y >= ry &&
                x < rx + rw && y < ry + rh);
    }

    bool isCurrentBounds(SInt32 x, SInt32 y, SInt32 w, SInt32 h) const
    {
        return (m_hasActiveBounds &&
                m_activeX == x && m_activeY == y &&
                m_activeW == w && m_activeH == h);
    }

    void fallbackToShapeBounds()
    {
        if (!m_screens.empty()) {
            LOG((CLOG_WARN
                "uhid: refused host-wide fallback while monitor list is available count=%lu",
                static_cast<unsigned long>(m_screens.size())));
            return;
        }

        SInt32 x = 0;
        SInt32 y = 0;
        SInt32 w = 0;
        SInt32 h = 0;
        m_screen->getShape(x, y, w, h);
        if (w > 0 && h > 0) {
            LOG((CLOG_WARN
                "uhid: falling back to host shape bounds=%d,%d %dx%d; monitor edge blocking needs physical monitor detection",
                x, y, w, h));
            setActiveBounds(x, y, w, h);
        }
    }

    void setActiveBounds(SInt32 x, SInt32 y, SInt32 w, SInt32 h)
    {
        if (w <= 0 || h <= 0) {
            return;
        }

        if (!isCurrentBounds(x, y, w, h)) {
            LOG((CLOG_INFO "uhid: active monitor bounds=%d,%d %dx%d",
                x, y, w, h));
        }

        m_activeX = x;
        m_activeY = y;
        m_activeW = w;
        m_activeH = h;
        m_hasActiveBounds = true;
    }

    void getCursorPos(SInt32& x, SInt32& y)
    {
        m_cursorPositionProvider->getCursorPos(x, y);
        LOG((CLOG_INFO "uhid: current cursor position x=%d y=%d", x, y));
        m_cursorX = x;
        m_cursorY = y;
        writeDebugStatus("getCursorPos");
    }

    void softSetCursorPos(SInt32 targetX, SInt32 targetY, const char* reason)
    {
        const SoftCursorPositioner::Result result =
            SoftCursorPositioner::moveTo(
                *m_cursorPositionProvider, *m_cursorMotionSink, targetX, targetY);

        LOG((CLOG_DEBUG2
            "uhid: soft set cursor reason=%s current=%d,%d target=%d,%d delta=%d,%d bounds=%d,%d %dx%d",
            reason,
            result.m_currentX, result.m_currentY,
            result.m_targetX, result.m_targetY,
            result.m_deltaX, result.m_deltaY,
            m_activeX, m_activeY, m_activeW, m_activeH));

        m_cursorX = result.m_targetX;
        m_cursorY = result.m_targetY;
        writeDebugStatus(reason);
    }

    void writeDebugStatus(const char* eventName) const
    {
        const std::string path = uhidDebugStatusPath();
        if (path.empty()) {
            return;
        }

        FILE* file = fopen(path.c_str(), "w");
        if (file == NULL) {
            return;
        }

        fprintf(file,
            "OK x=%d y=%d hasBounds=%d bounds=%d,%d %dx%d event=%s\n",
            m_cursorX,
            m_cursorY,
            m_hasActiveBounds ? 1 : 0,
            m_activeX,
            m_activeY,
            m_activeW,
            m_activeH,
            eventName != NULL ? eventName : "unknown");
        fclose(file);
    }

    void updateActiveBoundsForPoint(SInt32 x, SInt32 y)
    {
        if (m_screens.empty()) {
            refreshScreens();
        }

        for (std::vector<ClientScreenInfo>::const_iterator it = m_screens.begin();
             it != m_screens.end(); ++it) {
            if (contains(x, y, it->m_x, it->m_y, it->m_w, it->m_h)) {
                setActiveBounds(it->m_x, it->m_y, it->m_w, it->m_h);
                return;
            }
        }

        if (!m_screens.empty()) {
            std::vector<ClientScreenInfo>::const_iterator best = m_screens.begin();
            SInt32 bestDistance = edgeDistanceToScreen(x, y, *best);
            for (std::vector<ClientScreenInfo>::const_iterator it = m_screens.begin() + 1;
                 it != m_screens.end(); ++it) {
                const SInt32 distance = edgeDistanceToScreen(x, y, *it);
                if (distance < bestDistance) {
                    best = it;
                    bestDistance = distance;
                }
            }
            LOG((CLOG_WARN
                "uhid: enter point %d,%d did not hit a monitor; using nearest monitor=%s bounds=%d,%d %dx%d distance=%d",
                x, y, best->m_id.c_str(), best->m_x, best->m_y, best->m_w, best->m_h,
                bestDistance));
            setActiveBounds(best->m_x, best->m_y, best->m_w, best->m_h);
            return;
        }

        if (!m_hasActiveBounds) {
            fallbackToShapeBounds();
        }
    }

    void refreshScreens()
    {
        m_screen->getScreens(m_screens);
        LOG((CLOG_INFO "uhid: detected monitor count=%lu",
            static_cast<unsigned long>(m_screens.size())));
        for (std::vector<ClientScreenInfo>::const_iterator it = m_screens.begin();
             it != m_screens.end(); ++it) {
            LOG((CLOG_INFO "uhid: monitor id=%s bounds=%d,%d %dx%d",
                it->m_id.c_str(), it->m_x, it->m_y, it->m_w, it->m_h));
        }
    }

    SInt32 edgeDistanceToScreen(SInt32 x, SInt32 y, const ClientScreenInfo& screen) const
    {
        const SInt32 left = screen.m_x;
        const SInt32 top = screen.m_y;
        const SInt32 right = screen.m_x + screen.m_w - 1;
        const SInt32 bottom = screen.m_y + screen.m_h - 1;
        const SInt32 clampedX = std::max<SInt32>(left, std::min<SInt32>(right, x));
        const SInt32 clampedY = std::max<SInt32>(top, std::min<SInt32>(bottom, y));
        return std::abs(x - clampedX) + std::abs(y - clampedY);
    }

    void clampToActiveBounds(SInt32& x, SInt32& y)
    {
        if (!m_hasActiveBounds) {
            fallbackToShapeBounds();
        }
        if (!m_hasActiveBounds) {
            return;
        }

        static const SInt32 kGuardInset = 16;
        const SInt32 minX = m_activeX + std::min<SInt32>(kGuardInset, std::max<SInt32>(0, (m_activeW - 1) / 2));
        const SInt32 minY = m_activeY + std::min<SInt32>(kGuardInset, std::max<SInt32>(0, (m_activeH - 1) / 2));
        const SInt32 maxX = m_activeX + m_activeW - 1 -
            std::min<SInt32>(kGuardInset, std::max<SInt32>(0, (m_activeW - 1) / 2));
        const SInt32 maxY = m_activeY + m_activeH - 1 -
            std::min<SInt32>(kGuardInset, std::max<SInt32>(0, (m_activeH - 1) / 2));

        x = std::max<SInt32>(minX, std::min<SInt32>(maxX, x));
        y = std::max<SInt32>(minY, std::min<SInt32>(maxY, y));
    }

private:
    barrier::Screen* m_screen;
    bool m_started;
    std::unique_ptr<UhidServer> m_uhidServer;
    std::unique_ptr<ICursorPositionProvider> m_cursorPositionProvider;
    std::unique_ptr<ICursorMotionSink> m_cursorMotionSink;
    std::vector<ClientScreenInfo> m_screens;
    bool m_hasActiveBounds;
    SInt32 m_activeX;
    SInt32 m_activeY;
    SInt32 m_activeW;
    SInt32 m_activeH;
    SInt32 m_cursorX;
    SInt32 m_cursorY;
    bool m_hasTrackedCursorPos;
};

} // namespace

std::unique_ptr<IInputBackend> createInputBackend(barrier::Screen* screen, const ClientArgs& args)
{
    if (args.m_uhidEnabled) {
        std::unique_ptr<UhidInputBackend> backend(new UhidInputBackend(screen, args.m_uhidName));
        if (backend->started()) {
            LOG((CLOG_NOTE "uhid: using Linux UHID input backend"));
            return std::unique_ptr<IInputBackend>(backend.release());
        }

        LOG((CLOG_WARN "uhid: failed to start Linux UHID input backend, using screen backend"));
        writeUhidDebugStatusLine("ERR backend=uhid event=start-failed fallback=screen");
    }
    else {
        writeUhidDebugStatusLine("ERR backend=screen event=uhid-disabled");
    }

    return std::unique_ptr<IInputBackend>(new ScreenInputBackend(screen));
}
