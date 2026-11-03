/*
 * Etherwaver -- remote control of a server from a client's local input.
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 *
 * This package is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "platform/XWindowsLocalInputCapture.h"

#include "platform/XWindowsUtil.h"
#include "base/Log.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <chrono>
#include <set>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

using namespace barrier::remote;
typedef barrier::remote::Event RemoteEvent;

namespace {

double
clockSeconds(clockid_t id)
{
    struct timespec ts;
    clock_gettime(id, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

bool
polishUi()
{
    const char* vars[] = { "LC_ALL", "LC_MESSAGES", "LANG" };
    for (size_t i = 0; i < 3; ++i) {
        const char* v = std::getenv(vars[i]);
        if (v != NULL && v[0] != '\0') {
            return std::strncmp(v, "pl", 2) == 0;
        }
    }
    return false;
}

std::string
asciiFold(const std::string& utf8)
{
    // fallback when no UTF-8 font set is available: drop diacritics
    static const struct { const char* from; char to; } map[] = {
        { "\xc4\x85", 'a' }, { "\xc4\x87", 'c' }, { "\xc4\x99", 'e' }, { "\xc5\x82", 'l' },
        { "\xc5\x84", 'n' }, { "\xc3\xb3", 'o' }, { "\xc5\x9b", 's' }, { "\xc5\xba", 'z' },
        { "\xc5\xbc", 'z' }, { "\xc4\x84", 'A' }, { "\xc4\x86", 'C' }, { "\xc4\x98", 'E' },
        { "\xc5\x81", 'L' }, { "\xc5\x83", 'N' }, { "\xc3\x93", 'O' }, { "\xc5\x9a", 'S' },
        { "\xc5\xb9", 'Z' }, { "\xc5\xbb", 'Z' }, { "\xe2\x80\x9e", '"' }, { "\xe2\x80\x9d", '"' }
    };
    std::string out;
    for (size_t i = 0; i < utf8.size();) {
        bool done = false;
        for (size_t k = 0; k < sizeof(map) / sizeof(map[0]); ++k) {
            const size_t n = std::strlen(map[k].from);
            if (utf8.compare(i, n, map[k].from) == 0) {
                out += map[k].to;
                i += n;
                done = true;
                break;
            }
        }
        if (!done) {
            const unsigned char c = static_cast<unsigned char>(utf8[i]);
            out += (c < 0x80) ? static_cast<char>(c) : '?';
            ++i;
            while (i < utf8.size() && (static_cast<unsigned char>(utf8[i]) & 0xC0) == 0x80) {
                ++i;
            }
        }
    }
    return out;
}

//! X modifier bits -> barrier modifiers (one modifier per bit; AltGr
//! wins over Alt, Alt over Super, Super over Meta)
void
buildModifierMap(Display* d, KeyModifierMask modMap[8])
{
    for (int bit = 0; bit < 8; ++bit) {
        modMap[bit] = 0;
    }
    XModifierKeymap* modmap = XGetModifierMapping(d);
    if (modmap != NULL) {
        for (int bit = 0; bit < 8; ++bit) {
            KeyModifierMask best = 0;
            int bestRank = 0;
            for (int k = 0; k < modmap->max_keypermod; ++k) {
                const KeyCode kc = modmap->modifiermap[bit * modmap->max_keypermod + k];
                if (kc == 0) {
                    continue;
                }
                const KeySym sym = XkbKeycodeToKeysym(d, kc, 0, 0);
                const UInt32 mbit = XWindowsUtil::getModifierBitForKeySym(sym);
                if (mbit == kKeyModifierBitNone) {
                    continue;
                }
                int rank = 1;
                switch (mbit) {
                case kKeyModifierBitAltGr: rank = 6; break;
                case kKeyModifierBitAlt:   rank = 5; break;
                case kKeyModifierBitSuper: rank = 4; break;
                case kKeyModifierBitMeta:  rank = 3; break;
                default:                   rank = 2; break;
                }
                if (rank > bestRank) {
                    bestRank = rank;
                    best = (KeyModifierMask)(1u << mbit);
                }
            }
            modMap[bit] = best;
        }
        XFreeModifiermap(modmap);
    }
    // the core bits are fixed
    modMap[ShiftMapIndex]   = KeyModifierShift;
    modMap[LockMapIndex]    = KeyModifierCapsLock;
    modMap[ControlMapIndex] = KeyModifierControl;
}

} // namespace

//
// per-capture state, owned by the capture thread
//

struct XWindowsLocalInputCapture::Impl {
    Display*        m_display = NULL;
    Window          m_root = None;
    Window          m_window = None;
    Cursor          m_blankCursor = None;
    XFontSet        m_fontSet = NULL;
    GC              m_gc = NULL;
    Atom            m_wmDelete = None;
    int             m_screenW = 0, m_screenH = 0;
    int             m_winW = 960, m_winH = 600;
    int             m_centerX = 0, m_centerY = 0;
    int             m_lastX = 0, m_lastY = 0;
    int             m_savedX = 0, m_savedY = 0;
    bool            m_warpPending = false;
    bool            m_focused = false;
    bool            m_grabbed = false;
    unsigned int    m_hotkeyKeycode = 0;
    std::set<unsigned int> m_down;
    // X modifier bit (0..7) -> barrier modifier mask
    KeyModifierMask m_modMap[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
};

XWindowsLocalInputCapture::XWindowsLocalInputCapture(const char* displayName) :
    m_displayName(displayName != NULL ? displayName : ""),
    m_running(false),
    m_stopRequested(false),
    m_configChanged(false),
    m_sink(NULL),
    m_startDone(false)
{
    m_wakePipe[0] = m_wakePipe[1] = -1;
}

XWindowsLocalInputCapture::~XWindowsLocalInputCapture()
{
    stop();
}

bool
XWindowsLocalInputCapture::isWaylandSession()
{
    const char* waylandDisplay = std::getenv("WAYLAND_DISPLAY");
    if (waylandDisplay != NULL && waylandDisplay[0] != '\0') {
        return true;
    }
    const char* sessionType = std::getenv("XDG_SESSION_TYPE");
    return sessionType != NULL && std::strcmp(sessionType, "wayland") == 0;
}

bool
XWindowsLocalInputCapture::isAvailable(std::string& whyNot) const
{
    Display* d = XOpenDisplay(m_displayName.empty() ? NULL : m_displayName.c_str());
    if (d == NULL) {
        whyNot = "cannot open the X display" +
                 (m_displayName.empty() ? std::string() : " " + m_displayName);
        return false;
    }
    XCloseDisplay(d);
    whyNot.clear();
    return true;
}

bool
XWindowsLocalInputCapture::supportsMode(Mode mode) const
{
    if (mode == kModeCapture) {
        // a global X11 grab does not see native Wayland applications
        return !isWaylandSession();
    }
    return true;
}

void
XWindowsLocalInputCapture::updateTarget(const Config& config)
{
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        m_config = config;
        m_configChanged = true;
    }
    wakeThread();
}

void
XWindowsLocalInputCapture::wakeThread()
{
    if (m_wakePipe[1] >= 0) {
        const char c = 1;
        ssize_t r = ::write(m_wakePipe[1], &c, 1);
        (void)r;
    }
}

void
XWindowsLocalInputCapture::reportStart(const std::string& error)
{
    std::lock_guard<std::mutex> lock(m_configMutex);
    m_startError = error;
    m_startDone  = true;
    m_startCv.notify_all();
}

bool
XWindowsLocalInputCapture::start(Mode mode, const Config& config, Sink* sink, std::string& error)
{
    stop();
    if (!supportsMode(mode)) {
        error = "full capture is not possible with X11 under Wayland "
                "(use window mode or --remote-control-backend evdev)";
        return false;
    }
    if (::pipe(m_wakePipe) != 0) {
        error = std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    fcntl(m_wakePipe[0], F_SETFL, O_NONBLOCK);
    fcntl(m_wakePipe[1], F_SETFL, O_NONBLOCK);
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        m_config = config;
        m_configChanged = false;
        m_startDone = false;
        m_startError.clear();
    }
    m_sink = sink;
    m_stopRequested = false;
    m_running = true;
    m_thread = std::thread(&XWindowsLocalInputCapture::run, this, mode);

    std::unique_lock<std::mutex> lock(m_configMutex);
    if (!m_startCv.wait_for(lock, std::chrono::seconds(5), [this]() { return m_startDone; })) {
        lock.unlock();
        error = "timed out while grabbing the keyboard and mouse";
        stop();
        return false;
    }
    if (!m_startError.empty()) {
        error = m_startError;
        lock.unlock();
        stop();
        return false;
    }
    LOG((CLOG_INFO "remote control: x11 %s started",
         mode == kModeCapture ? "full capture" : "session window"));
    return true;
}

void
XWindowsLocalInputCapture::stop()
{
    m_stopRequested = true;
    wakeThread();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    m_running = false;
    if (m_wakePipe[0] >= 0) {
        ::close(m_wakePipe[0]);
        ::close(m_wakePipe[1]);
        m_wakePipe[0] = m_wakePipe[1] = -1;
    }
}

//
// capture thread
//

void
XWindowsLocalInputCapture::run(Mode mode)
{
    Impl s;
    std::string error;
    if (!setup(mode, s, error)) {
        cleanup(mode, s);
        reportStart(error);
        m_running = false;
        return;
    }
    reportStart(std::string());
    loop(mode, s);
    cleanup(mode, s);
    m_running = false;
}

bool
XWindowsLocalInputCapture::setup(Mode mode, Impl& s, std::string& error)
{
    s.m_display = XOpenDisplay(m_displayName.empty() ? NULL : m_displayName.c_str());
    if (s.m_display == NULL) {
        error = "cannot open the X display";
        return false;
    }
    Display* d = s.m_display;
    Bool supported = False;
    XkbSetDetectableAutoRepeat(d, True, &supported);

    const int screen = DefaultScreen(d);
    s.m_root    = RootWindow(d, screen);
    s.m_screenW = DisplayWidth(d, screen);
    s.m_screenH = DisplayHeight(d, screen);

    buildModifierMap(d, s.m_modMap);

    if (mode == kModeCapture) {
        // invisible cursor while the input belongs to the remote host
        static const char empty[8] = { 0 };
        Pixmap pixmap = XCreateBitmapFromData(d, s.m_root, empty, 8, 8);
        XColor black;
        std::memset(&black, 0, sizeof(black));
        s.m_blankCursor = XCreatePixmapCursor(d, pixmap, pixmap, &black, &black, 0, 0);
        XFreePixmap(d, pixmap);

        Window rootRet, childRet;
        int wx, wy;
        unsigned int mask;
        XQueryPointer(d, s.m_root, &rootRet, &childRet, &s.m_savedX, &s.m_savedY, &wx, &wy, &mask);

        // the user may still hold the key or button that started the
        // capture; retry for a moment
        int kr = GrabNotViewable, pr = GrabNotViewable;
        for (int attempt = 0; attempt < 100 && !m_stopRequested; ++attempt) {
            kr = XGrabKeyboard(d, s.m_root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
            if (kr == GrabSuccess) {
                pr = XGrabPointer(d, s.m_root, False,
                                  ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                                  GrabModeAsync, GrabModeAsync, None, s.m_blankCursor,
                                  CurrentTime);
                if (pr == GrabSuccess) {
                    break;
                }
                XUngrabKeyboard(d, CurrentTime);
            }
            XSync(d, False);
            usleep(20000);
        }
        if (kr != GrabSuccess || pr != GrabSuccess) {
            error = "another application holds the keyboard or mouse grab";
            return false;
        }
        s.m_grabbed = true;
        s.m_centerX = s.m_screenW / 2;
        s.m_centerY = s.m_screenH / 2;
        XWarpPointer(d, None, s.m_root, 0, 0, 0, 0, s.m_centerX, s.m_centerY);
        XSync(d, False);
        // forget motion from before the warp
        XEvent ev;
        while (XCheckMaskEvent(d, PointerMotionMask, &ev)) {
        }
        s.m_lastX = s.m_centerX;
        s.m_lastY = s.m_centerY;
        s.m_focused = true;
    }
    else {
        s.m_winW = std::max(320, std::min(960, s.m_screenW * 2 / 3));
        s.m_winH = std::max(200, std::min(600, s.m_screenH * 2 / 3));
        XSetWindowAttributes attr;
        std::memset(&attr, 0, sizeof(attr));
        attr.background_pixel = BlackPixel(d, screen);
        attr.event_mask = KeyPressMask | KeyReleaseMask | ButtonPressMask |
                          ButtonReleaseMask | PointerMotionMask | FocusChangeMask |
                          ExposureMask | StructureNotifyMask;
        s.m_window = XCreateWindow(d, s.m_root,
                                   (s.m_screenW - s.m_winW) / 2, (s.m_screenH - s.m_winH) / 2,
                                   s.m_winW, s.m_winH, 0, CopyFromParent, InputOutput,
                                   CopyFromParent, CWBackPixel | CWEventMask, &attr);
        std::string title;
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            title = (polishUi() ? "Etherwaver - sterowanie: " : "Etherwaver - controlling: ") +
                    (m_config.m_targetName.empty() ? std::string("server") : m_config.m_targetName);
        }
        Xutf8SetWMProperties(d, s.m_window, title.c_str(), title.c_str(), NULL, 0, NULL, NULL, NULL);
        Atom netWmName = XInternAtom(d, "_NET_WM_NAME", False);
        Atom utf8 = XInternAtom(d, "UTF8_STRING", False);
        XChangeProperty(d, s.m_window, netWmName, utf8, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(title.data()), (int)title.size());
        s.m_wmDelete = XInternAtom(d, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(d, s.m_window, &s.m_wmDelete, 1);
        Cursor cross = XCreateFontCursor(d, 34 /* XC_crosshair */);
        XDefineCursor(d, s.m_window, cross);

        char** missing = NULL;
        int missingCount = 0;
        char* defString = NULL;
        s.m_fontSet = XCreateFontSet(d, "-*-*-medium-r-normal--14-*-*-*-*-*-*-*,*",
                                     &missing, &missingCount, &defString);
        if (missing != NULL) {
            XFreeStringList(missing);
        }
        s.m_gc = XCreateGC(d, s.m_window, 0, NULL);
        XMapRaised(d, s.m_window);
        XSync(d, False);
    }
    XFlush(d);
    return true;
}

void
XWindowsLocalInputCapture::cleanup(Mode mode, Impl& s)
{
    Display* d = s.m_display;
    if (d == NULL) {
        return;
    }
    if (s.m_grabbed) {
        // let the user release the keys of the release hotkey first, so
        // local applications do not receive releases without presses
        for (int i = 0; i < 40; ++i) {
            char keys[32];
            XQueryKeymap(d, keys);
            bool any = false;
            for (int k = 0; k < 32; ++k) {
                any = any || keys[k] != 0;
            }
            if (!any) {
                break;
            }
            usleep(10000);
        }
        XEvent ev;
        while (XCheckMaskEvent(d, KeyPressMask | KeyReleaseMask | ButtonPressMask |
                                  ButtonReleaseMask | PointerMotionMask, &ev)) {
        }
        XUngrabPointer(d, CurrentTime);
        XUngrabKeyboard(d, CurrentTime);
        // give the cursor back where it was
        XWarpPointer(d, None, s.m_root, 0, 0, 0, 0, s.m_savedX, s.m_savedY);
        s.m_grabbed = false;
    }
    if (s.m_blankCursor != None) {
        XFreeCursor(d, s.m_blankCursor);
    }
    if (s.m_gc != NULL) {
        XFreeGC(d, s.m_gc);
    }
    if (s.m_fontSet != NULL) {
        XFreeFontSet(d, s.m_fontSet);
    }
    if (s.m_window != None) {
        XDestroyWindow(d, s.m_window);
    }
    (void)mode;
    XSync(d, False);
    XCloseDisplay(d);
    s.m_display = NULL;
    LOG((CLOG_INFO "remote control: x11 capture ended, local input restored"));
}

void
XWindowsLocalInputCapture::loop(Mode mode, Impl& s)
{
    Display* d = s.m_display;
    const int xfd = ConnectionNumber(d);
    // suspend detection: CLOCK_BOOTTIME keeps running while suspended,
    // CLOCK_MONOTONIC does not
    double drift = clockSeconds(CLOCK_BOOTTIME) - clockSeconds(CLOCK_MONOTONIC);

    while (!m_stopRequested) {
        while (XPending(d) > 0 && !m_stopRequested) {
            XEvent ev;
            XNextEvent(d, &ev);
            handleEvent(mode, s, &ev);
        }
        if (m_stopRequested) {
            break;
        }
        bool changed = false;
        {
            std::lock_guard<std::mutex> lock(m_configMutex);
            changed = m_configChanged;
            m_configChanged = false;
        }
        if (changed && s.m_window != None) {
            redraw(s);
        }
        XFlush(d);

        struct pollfd fds[2];
        fds[0].fd = xfd;         fds[0].events = POLLIN; fds[0].revents = 0;
        fds[1].fd = m_wakePipe[0]; fds[1].events = POLLIN; fds[1].revents = 0;
        const int r = ::poll(fds, 2, 250);
        if (r > 0 && (fds[1].revents & POLLIN)) {
            char buf[64];
            while (::read(m_wakePipe[0], buf, sizeof(buf)) > 0) {
            }
        }
        if (r > 0 && (fds[0].revents & (POLLERR | POLLHUP))) {
            m_sink->onCaptureLost("X display connection closed");
            break;
        }

        const double nowDrift = clockSeconds(CLOCK_BOOTTIME) - clockSeconds(CLOCK_MONOTONIC);
        if (nowDrift - drift > 2.0) {
            m_sink->onCaptureLost("system sleep detected");
            break;
        }
        drift = nowDrift;
    }
}

void
XWindowsLocalInputCapture::handleEvent(Mode mode, Impl& s, XEventPtr event)
{
    XEvent& ev = *event;
    Display* d = s.m_display;
    Config config;
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        config = m_config;
    }

    switch (ev.type) {
    case KeyPress:
    case KeyRelease: {
        if (!s.m_focused) {
            return;
        }
        XKeyEvent& xkey = ev.xkey;
        const unsigned int keycode = xkey.keycode;

        KeyModifierMask mask = 0;
        for (int bit = 0; bit < 8; ++bit) {
            if (xkey.state & (1u << bit)) {
                mask |= s.m_modMap[bit];
            }
        }

        KeySym keysym = NoSymbol;
        char buf[16];
        XLookupString(&xkey, buf, sizeof(buf), &keysym, NULL);
        const KeyID id = XWindowsUtil::mapKeySymToKeyID(keysym);

        if (ev.type == KeyPress) {
            // the release hotkey is matched on the unshifted symbol of the
            // physical key in the current group and never forwarded
            const int group = XkbGroupForCoreState(xkey.state);
            const KeySym base = XkbKeycodeToKeysym(d, (KeyCode)keycode, group, 0);
            const KeyID baseId = XWindowsUtil::mapKeySymToKeyID(base);
            if (config.m_releaseHotkey.matches(baseId, mask)) {
                s.m_hotkeyKeycode = keycode;
                m_sink->onCaptureReleaseHotkey();
                return;
            }
            if (id == kKeyNone) {
                return;
            }
            if (s.m_down.count(keycode) != 0) {
                m_sink->onCaptureEvent(RemoteEvent::keyRepeat(id, mask, (KeyButton)keycode, 1));
            }
            else {
                s.m_down.insert(keycode);
                m_sink->onCaptureEvent(RemoteEvent::keyDown(id, mask, (KeyButton)keycode));
            }
        }
        else {
            if (keycode == s.m_hotkeyKeycode) {
                s.m_hotkeyKeycode = 0;
                return;
            }
            if (s.m_down.erase(keycode) == 0) {
                return;
            }
            m_sink->onCaptureEvent(RemoteEvent::keyUp(id, mask, (KeyButton)keycode));
        }
        return;
    }

    case ButtonPress:
    case ButtonRelease: {
        if (!s.m_focused) {
            return;
        }
        const bool press = (ev.type == ButtonPress);
        const unsigned int b = ev.xbutton.button;
        if (b >= 4 && b <= 7) {
            if (press) {
                const SInt32 dy = (b == 4) ? 120 : (b == 5) ? -120 : 0;
                const SInt32 dx = (b == 7) ? 120 : (b == 6) ? -120 : 0;
                m_sink->onCaptureEvent(RemoteEvent::wheel(dx, dy));
            }
            return;
        }
        ButtonID id = kButtonNone;
        switch (b) {
        case 1: id = kButtonLeft; break;
        case 2: id = kButtonMiddle; break;
        case 3: id = kButtonRight; break;
        case 8: id = kButtonExtra0; break;
        case 9: id = kButtonExtra1; break;
        default: return;
        }
        if (mode == kModeWindow && press) {
            // make sure the remote pointer is where the click happens
            SInt32 rx, ry;
            if (config.m_targetW > 0 &&
                mapViewToRemote(s.m_winW, s.m_winH, 1.0, config.m_targetX, config.m_targetY,
                                config.m_targetW, config.m_targetH,
                                ev.xbutton.x, ev.xbutton.y, rx, ry)) {
                m_sink->onCaptureEvent(RemoteEvent::moveAbs(rx, ry));
            }
        }
        m_sink->onCaptureEvent(press ? RemoteEvent::buttonDown(id) : RemoteEvent::buttonUp(id));
        return;
    }

    case MotionNotify: {
        if (!s.m_focused) {
            return;
        }
        const int x = ev.xmotion.x_root;
        const int y = ev.xmotion.y_root;
        if (mode == kModeCapture) {
            if (s.m_warpPending && x == s.m_centerX && y == s.m_centerY) {
                // the motion caused by our own warp
                s.m_warpPending = false;
                s.m_lastX = x;
                s.m_lastY = y;
                return;
            }
            const int dx = x - s.m_lastX;
            const int dy = y - s.m_lastY;
            s.m_lastX = x;
            s.m_lastY = y;
            if (dx != 0 || dy != 0) {
                m_sink->onCaptureEvent(RemoteEvent::moveRel(dx, dy));
            }
            // keep the hidden pointer away from the screen edges
            if (!s.m_warpPending &&
                (std::abs(x - s.m_centerX) > s.m_screenW / 4 ||
                 std::abs(y - s.m_centerY) > s.m_screenH / 4)) {
                XWarpPointer(d, None, s.m_root, 0, 0, 0, 0, s.m_centerX, s.m_centerY);
                s.m_warpPending = true;
            }
        }
        else if (config.m_targetW > 0) {
            SInt32 rx, ry;
            mapViewToRemote(s.m_winW, s.m_winH, 1.0, config.m_targetX, config.m_targetY,
                            config.m_targetW, config.m_targetH,
                            ev.xmotion.x, ev.xmotion.y, rx, ry);
            m_sink->onCaptureEvent(RemoteEvent::moveAbs(rx, ry));
        }
        return;
    }

    case FocusIn:
        if (mode == kModeWindow && ev.xfocus.window == s.m_window) {
            if (!s.m_focused) {
                s.m_focused = true;
                m_sink->onCaptureFocus(true);
                redraw(s);
            }
        }
        return;

    case FocusOut:
        if (mode == kModeWindow && ev.xfocus.window == s.m_window &&
            ev.xfocus.mode != NotifyGrab && ev.xfocus.mode != NotifyUngrab) {
            if (s.m_focused) {
                s.m_focused = false;
                s.m_down.clear();
                s.m_hotkeyKeycode = 0;
                m_sink->onCaptureFocus(false);
                redraw(s);
            }
        }
        else if (mode == kModeCapture && ev.xfocus.mode == NotifyUngrab) {
            // somebody broke our grab
            m_sink->onCaptureLost("keyboard grab lost");
            m_stopRequested = true;
        }
        return;

    case MapNotify:
        if (mode == kModeWindow && ev.xmap.window == s.m_window) {
            XSetInputFocus(d, s.m_window, RevertToParent, CurrentTime);
            redraw(s);
        }
        return;

    case MappingNotify:
        // keyboard layout changed while capturing
        XRefreshKeyboardMapping(&ev.xmapping);
        buildModifierMap(d, s.m_modMap);
        return;

    case ConfigureNotify:
        if (ev.xconfigure.window == s.m_window) {
            s.m_winW = std::max(1, ev.xconfigure.width);
            s.m_winH = std::max(1, ev.xconfigure.height);
        }
        return;

    case Expose:
        if (ev.xexpose.count == 0) {
            redraw(s);
        }
        return;

    case ClientMessage:
        if (ev.xclient.window == s.m_window &&
            (Atom)ev.xclient.data.l[0] == s.m_wmDelete) {
            m_sink->onCaptureLost("session window closed");
            m_stopRequested = true;
        }
        return;

    default:
        return;
    }
}

void
XWindowsLocalInputCapture::redraw(Impl& s)
{
    if (s.m_window == None || s.m_gc == NULL) {
        return;
    }
    Display* d = s.m_display;
    Config config;
    {
        std::lock_guard<std::mutex> lock(m_configMutex);
        config = m_config;
    }
    const int screen = DefaultScreen(d);
    XSetForeground(d, s.m_gc, BlackPixel(d, screen));
    XFillRectangle(d, s.m_window, s.m_gc, 0, 0, s.m_winW, s.m_winH);

    // the remote screen, letterboxed
    if (config.m_targetW > 0 && config.m_targetH > 0) {
        const Rect r = fitRect(s.m_winW, s.m_winH, config.m_targetW, config.m_targetH);
        XSetForeground(d, s.m_gc, s.m_focused ? 0x203a5a : 0x303030);
        XFillRectangle(d, s.m_window, s.m_gc, (int)r.m_x, (int)r.m_y,
                       (unsigned)r.m_w, (unsigned)r.m_h);
    }

    const bool pl = polishUi();
    const std::string host = config.m_targetName.empty() ? std::string("server") : config.m_targetName;
    const std::string hotkey = config.m_releaseHotkey.toString();
    std::string line1, line2;
    if (s.m_focused) {
        line1 = pl ? "Mysz i klawiatura steruj\xc4\x85 teraz: " + host + "."
                   : "Keyboard and mouse now control: " + host + ".";
        line2 = pl ? "Naci\xc5\x9bnij " + hotkey + ", aby wr\xc3\xb3" "ci\xc4\x87 do tego komputera."
                   : "Press " + hotkey + " to return to this computer.";
    }
    else {
        line1 = pl ? "Sterowanie wstrzymane (okno nie ma fokusu): " + host
                   : "Control paused (window not focused): " + host;
        line2 = pl ? "Kliknij okno, aby wznowi\xc4\x87 sterowanie. " + hotkey + " ko\xc5\x84" "czy sterowanie."
                   : "Click the window to resume. " + hotkey + " ends remote control.";
    }
    XSetForeground(d, s.m_gc, WhitePixel(d, screen));
    if (s.m_fontSet != NULL) {
        Xutf8DrawString(d, s.m_window, s.m_fontSet, s.m_gc, 16, 24, line1.c_str(), (int)line1.size());
        Xutf8DrawString(d, s.m_window, s.m_fontSet, s.m_gc, 16, 44, line2.c_str(), (int)line2.size());
    }
    else {
        const std::string a = asciiFold(line1), b = asciiFold(line2);
        XDrawString(d, s.m_window, s.m_gc, 16, 24, a.c_str(), (int)a.size());
        XDrawString(d, s.m_window, s.m_gc, 16, 44, b.c_str(), (int)b.size());
    }
    XFlush(d);
}
