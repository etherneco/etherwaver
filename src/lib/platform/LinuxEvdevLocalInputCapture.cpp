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

#include "platform/LinuxEvdevLocalInputCapture.h"

#include "platform/InputGrabber.h"
#include "platform/XWindowsUtil.h"
#include "base/Log.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <set>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <linux/input.h>

#if HAVE_XKBCOMMON
#include <xkbcommon/xkbcommon.h>
#endif

using namespace barrier::remote;
typedef barrier::remote::Event RemoteEvent;

namespace {

const size_t kBitsPerLong = sizeof(unsigned long) * 8;
#define EW_NLONGS(x) (((x) + kBitsPerLong - 1) / kBitsPerLong)

bool
testBit(const unsigned long* bits, unsigned int bit)
{
    return (bits[bit / kBitsPerLong] >> (bit % kBitsPerLong)) & 1UL;
}

double
clockSeconds(clockid_t id)
{
    struct timespec ts;
    clock_gettime(id, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

std::string
lowerAscii(const std::string& s)
{
    std::string r(s);
    for (size_t i = 0; i < r.size(); ++i) {
        r[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(r[i])));
    }
    return r;
}

std::string
unquote(std::string v)
{
    while (!v.empty() && (v[v.size() - 1] == '\r' || v[v.size() - 1] == ' ')) {
        v.erase(v.size() - 1);
    }
    if (v.size() >= 2 && (v[0] == '"' || v[0] == '\'') && v[v.size() - 1] == v[0]) {
        v = v.substr(1, v.size() - 2);
    }
    // only the first layout of a list
    const size_t comma = v.find(',');
    if (comma != std::string::npos) {
        v = v.substr(0, comma);
    }
    return v;
}

} // namespace

//
// EvdevKeyTranslator
//

EvdevKeyTranslator::EvdevKeyTranslator() :
    m_context(NULL),
    m_keymap(NULL),
    m_state(NULL)
{
}

EvdevKeyTranslator::~EvdevKeyTranslator()
{
#if HAVE_XKBCOMMON
    if (m_state != NULL)   xkb_state_unref(m_state);
    if (m_keymap != NULL)  xkb_keymap_unref(m_keymap);
    if (m_context != NULL) xkb_context_unref(m_context);
#endif
}

bool
EvdevKeyTranslator::isSupported()
{
#if HAVE_XKBCOMMON
    return true;
#else
    return false;
#endif
}

void
EvdevKeyTranslator::systemLayout(std::string& layout, std::string& variant)
{
    const char* envLayout = std::getenv("XKB_DEFAULT_LAYOUT");
    const char* envVariant = std::getenv("XKB_DEFAULT_VARIANT");
    if (envLayout != NULL && envLayout[0] != '\0') {
        layout  = unquote(envLayout);
        variant = envVariant != NULL ? unquote(envVariant) : std::string();
        return;
    }
    std::ifstream in("/etc/default/keyboard");
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, 10, "XKBLAYOUT=") == 0) {
            layout = unquote(line.substr(10));
        }
        else if (line.compare(0, 11, "XKBVARIANT=") == 0) {
            variant = unquote(line.substr(11));
        }
    }
    if (layout.empty()) {
        layout = "us";
    }
}

bool
EvdevKeyTranslator::init(const std::string& layoutArg, const std::string& variantArg,
                         std::string& error)
{
#if HAVE_XKBCOMMON
    std::string layout = layoutArg, variant = variantArg;
    if (layout.empty()) {
        systemLayout(layout, variant);
    }
    m_layout = variant.empty() ? layout : layout + "(" + variant + ")";

    m_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (m_context == NULL) {
        error = "xkbcommon: cannot create context";
        return false;
    }
    struct xkb_rule_names names;
    std::memset(&names, 0, sizeof(names));
    names.rules   = "evdev";
    names.model   = "pc105";
    names.layout  = layout.c_str();
    names.variant = variant.empty() ? NULL : variant.c_str();
    m_keymap = xkb_keymap_new_from_names(m_context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (m_keymap == NULL) {
        error = "xkbcommon: unknown keyboard layout \"" + m_layout + "\"";
        return false;
    }
    m_state = xkb_state_new(m_keymap);
    if (m_state == NULL) {
        error = "xkbcommon: cannot create keyboard state";
        return false;
    }
    return true;
#else
    (void)layoutArg;
    (void)variantArg;
    error = "built without xkbcommon";
    return false;
#endif
}

EvdevKeyTranslator::Key
EvdevKeyTranslator::translate(unsigned int evdevCode, int value)
{
    Key key;
    key.m_button = static_cast<KeyButton>(evdevCode + 8);
#if HAVE_XKBCOMMON
    if (m_state == NULL) {
        return key;
    }
    const xkb_keycode_t kc = evdevCode + 8;

    // modifiers as they are *before* this key changes them (barrier
    // primary screens report the same)
    struct { const char* m_name; KeyModifierMask m_mask; } mods[] = {
        { XKB_MOD_NAME_SHIFT, KeyModifierShift },
        { XKB_MOD_NAME_CTRL,  KeyModifierControl },
        { XKB_MOD_NAME_ALT,   KeyModifierAlt },
        { XKB_MOD_NAME_LOGO,  KeyModifierSuper },
        { "Mod5",             KeyModifierAltGr },
        { XKB_MOD_NAME_CAPS,  KeyModifierCapsLock },
        { XKB_MOD_NAME_NUM,   KeyModifierNumLock }
    };
    for (size_t i = 0; i < sizeof(mods) / sizeof(mods[0]); ++i) {
        if (xkb_state_mod_name_is_active(m_state, mods[i].m_name, XKB_STATE_MODS_EFFECTIVE) > 0) {
            key.m_mask |= mods[i].m_mask;
        }
    }

    const xkb_keysym_t sym = xkb_state_key_get_one_sym(m_state, kc);
    key.m_id = XWindowsUtil::mapKeySymToKeyID(sym);

    const xkb_layout_index_t layoutIndex = xkb_state_key_get_layout(m_state, kc);
    const xkb_keysym_t* baseSyms = NULL;
    if (layoutIndex != XKB_LAYOUT_INVALID &&
        xkb_keymap_key_get_syms_by_level(m_keymap, kc, layoutIndex, 0, &baseSyms) > 0) {
        key.m_baseId = XWindowsUtil::mapKeySymToKeyID(baseSyms[0]);
    }
    else {
        key.m_baseId = key.m_id;
    }

    if (value == 1) {
        xkb_state_update_key(m_state, kc, XKB_KEY_DOWN);
    }
    else if (value == 0) {
        xkb_state_update_key(m_state, kc, XKB_KEY_UP);
    }
#else
    (void)value;
#endif
    return key;
}

//
// LinuxEvdevLocalInputCapture
//

LinuxEvdevLocalInputCapture::LinuxEvdevLocalInputCapture(const std::string& layout,
                                                         const std::string& variant,
                                                         const std::string& ignoredPrefix) :
    m_layout(layout),
    m_variant(variant),
    m_ignoredPrefix(ignoredPrefix.empty() ? std::string("Etherwaver") : ignoredPrefix),
    m_running(false),
    m_stopRequested(false),
    m_startDone(false),
    m_sink(NULL)
{
    m_wakePipe[0] = m_wakePipe[1] = -1;
}

LinuxEvdevLocalInputCapture::~LinuxEvdevLocalInputCapture()
{
    stop();
}

bool
LinuxEvdevLocalInputCapture::shouldIgnoreDevice(const std::string& name, const std::string& prefix)
{
    // our own virtual UHID devices (--uhid) and other synthetic devices
    const std::string lower = lowerAscii(name);
    if (!prefix.empty() && name.compare(0, prefix.size(), prefix) == 0) {
        return true;
    }
    return lower.find("etherwaver") != std::string::npos ||
           lower.find("barrier") != std::string::npos ||
           lower.find("xtest") != std::string::npos;
}

bool
LinuxEvdevLocalInputCapture::isAvailable(std::string& whyNot) const
{
    if (!EvdevKeyTranslator::isSupported()) {
        whyNot = "this build has no xkbcommon (evdev backend disabled)";
        return false;
    }
    DIR* dir = opendir("/dev/input");
    if (dir == NULL) {
        whyNot = "/dev/input does not exist";
        return false;
    }
    bool any = false, denied = false;
    while (struct dirent* entry = readdir(dir)) {
        if (std::strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }
        const std::string path = std::string("/dev/input/") + entry->d_name;
        const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd >= 0) {
            any = true;
            ::close(fd);
            break;
        }
        if (errno == EACCES || errno == EPERM) {
            denied = true;
        }
    }
    closedir(dir);
    if (!any) {
        whyNot = denied ? "no permission to read /dev/input/event* "
                          "(add the user to the \"input\" group and log in again)"
                        : "no input devices found in /dev/input";
        return false;
    }
    whyNot.clear();
    return true;
}

bool
LinuxEvdevLocalInputCapture::openDevices(std::vector<Device>& devices, std::string& error) const
{
    DIR* dir = opendir("/dev/input");
    if (dir == NULL) {
        error = "/dev/input does not exist";
        return false;
    }
    std::vector<std::string> paths;
    while (struct dirent* entry = readdir(dir)) {
        if (std::strncmp(entry->d_name, "event", 5) == 0) {
            paths.push_back(std::string("/dev/input/") + entry->d_name);
        }
    }
    closedir(dir);
    std::sort(paths.begin(), paths.end());

    for (size_t i = 0; i < paths.size(); ++i) {
        const int fd = ::open(paths[i].c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        char nameBuf[256] = { 0 };
        ioctl(fd, EVIOCGNAME(sizeof(nameBuf) - 1), nameBuf);
        Device dev;
        dev.m_fd   = fd;
        dev.m_path = paths[i];
        dev.m_name = nameBuf;

        unsigned long evBits[EW_NLONGS(EV_MAX + 1)] = { 0 };
        unsigned long keyBits[EW_NLONGS(KEY_MAX + 1)] = { 0 };
        unsigned long relBits[EW_NLONGS(REL_MAX + 1)] = { 0 };
        ioctl(fd, EVIOCGBIT(0, sizeof(evBits)), evBits);
        ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keyBits)), keyBits);
        ioctl(fd, EVIOCGBIT(EV_REL, sizeof(relBits)), relBits);

        dev.m_keyboard = testBit(evBits, EV_KEY) && testBit(keyBits, KEY_A) &&
                         testBit(keyBits, KEY_Z) && testBit(keyBits, KEY_ENTER);
        dev.m_pointer  = testBit(evBits, EV_REL) && testBit(relBits, REL_X) &&
                         testBit(relBits, REL_Y) && testBit(keyBits, BTN_LEFT);
#ifdef REL_WHEEL_HI_RES
        dev.m_hiResWheel  = testBit(relBits, REL_WHEEL_HI_RES);
        dev.m_hiResHWheel = testBit(relBits, REL_HWHEEL_HI_RES);
#else
        dev.m_hiResWheel = dev.m_hiResHWheel = false;
#endif
        if ((!dev.m_keyboard && !dev.m_pointer) || shouldIgnoreDevice(dev.m_name, m_ignoredPrefix)) {
            ::close(fd);
            continue;
        }
        devices.push_back(dev);
    }
    if (devices.empty()) {
        error = "no keyboard or mouse found in /dev/input (check permissions: \"input\" group)";
        return false;
    }
    return true;
}

void
LinuxEvdevLocalInputCapture::closeDevices(std::vector<Device>& devices) const
{
    InputGrabber grabber;
    for (size_t i = 0; i < devices.size(); ++i) {
        grabber.apply(devices[i].m_fd, devices[i].m_path, false);
        ::close(devices[i].m_fd);
    }
    devices.clear();
}

void
LinuxEvdevLocalInputCapture::reportStart(const std::string& error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_startError = error;
    m_startDone  = true;
    m_startCv.notify_all();
}

bool
LinuxEvdevLocalInputCapture::start(Mode mode, const Config& config, Sink* sink, std::string& error)
{
    stop();
    if (mode != kModeCapture) {
        error = "the evdev backend supports full capture only";
        return false;
    }
    if (::pipe(m_wakePipe) != 0) {
        error = std::string("pipe: ") + std::strerror(errno);
        return false;
    }
    fcntl(m_wakePipe[0], F_SETFL, O_NONBLOCK);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_config     = config;
        m_startDone  = false;
        m_startError.clear();
    }
    m_sink = sink;
    m_stopRequested = false;
    m_running = true;
    m_thread = std::thread(&LinuxEvdevLocalInputCapture::run, this);

    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_startCv.wait_for(lock, std::chrono::seconds(6), [this]() { return m_startDone; })) {
        lock.unlock();
        error = "timed out while grabbing the input devices";
        stop();
        return false;
    }
    if (!m_startError.empty()) {
        error = m_startError;
        lock.unlock();
        stop();
        return false;
    }
    return true;
}

void
LinuxEvdevLocalInputCapture::stop()
{
    m_stopRequested = true;
    if (m_wakePipe[1] >= 0) {
        const char c = 1;
        ssize_t r = ::write(m_wakePipe[1], &c, 1);
        (void)r;
    }
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

void
LinuxEvdevLocalInputCapture::updateTarget(const Config& config)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config = config;
}

void
LinuxEvdevLocalInputCapture::run()
{
    EvdevKeyTranslator translator;
    std::string error;
    if (!translator.init(m_layout, m_variant, error)) {
        reportStart(error);
        m_running = false;
        return;
    }
    std::vector<Device> devices;
    if (!openDevices(devices, error)) {
        reportStart(error);
        m_running = false;
        return;
    }

    // wait until nothing is held, otherwise the local system would never
    // see the release of a key pressed before the grab (stuck key)
    for (int attempt = 0; attempt < 150 && !m_stopRequested; ++attempt) {
        bool held = false;
        for (size_t i = 0; i < devices.size() && !held; ++i) {
            unsigned long keys[EW_NLONGS(KEY_MAX + 1)] = { 0 };
            if (ioctl(devices[i].m_fd, EVIOCGKEY(sizeof(keys)), keys) >= 0) {
                for (size_t k = 0; k < EW_NLONGS(KEY_MAX + 1); ++k) {
                    if (keys[k] != 0) {
                        held = true;
                        break;
                    }
                }
            }
        }
        if (!held) {
            break;
        }
        usleep(20000);
    }

    InputGrabber grabber;
    grabber.setExclusive(true);
    for (size_t i = 0; i < devices.size(); ++i) {
        if (!grabber.apply(devices[i].m_fd, devices[i].m_path, true)) {
            error = "cannot grab " + devices[i].m_path + " exclusively (" + devices[i].m_name +
                    "): another program holds it";
            closeDevices(devices);
            reportStart(error);
            m_running = false;
            return;
        }
        LOG((CLOG_INFO "remote control: evdev grabbed %s (%s%s%s)", devices[i].m_path.c_str(),
             devices[i].m_name.c_str(), devices[i].m_keyboard ? ", keyboard" : "",
             devices[i].m_pointer ? ", pointer" : ""));
    }
    LOG((CLOG_INFO "remote control: evdev capture started, layout %s", translator.layout().c_str()));
    reportStart(std::string());

    std::set<unsigned int> down;
    unsigned int hotkeyCode = 0;
    double drift = clockSeconds(CLOCK_BOOTTIME) - clockSeconds(CLOCK_MONOTONIC);
    std::map<int, std::pair<int, int> > pendingRel;  // per fd dx,dy until SYN_REPORT

    while (!m_stopRequested) {
        std::vector<struct pollfd> fds(devices.size() + 1);
        for (size_t i = 0; i < devices.size(); ++i) {
            fds[i].fd = devices[i].m_fd;
            fds[i].events = POLLIN;
            fds[i].revents = 0;
        }
        fds[devices.size()].fd = m_wakePipe[0];
        fds[devices.size()].events = POLLIN;
        fds[devices.size()].revents = 0;
        const int r = ::poll(&fds[0], fds.size(), 250);

        const double nowDrift = clockSeconds(CLOCK_BOOTTIME) - clockSeconds(CLOCK_MONOTONIC);
        if (nowDrift - drift > 2.0) {
            m_sink->onCaptureLost("system sleep detected");
            break;
        }
        drift = nowDrift;
        if (r <= 0) {
            continue;
        }

        Hotkey hotkey;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            hotkey = m_config.m_releaseHotkey;
        }

        bool lost = false;
        for (size_t i = 0; i < devices.size() && !lost; ++i) {
            if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
                m_sink->onCaptureLost("input device removed: " + devices[i].m_name);
                lost = true;
                break;
            }
            if ((fds[i].revents & POLLIN) == 0) {
                continue;
            }
            struct input_event events[64];
            const ssize_t n = ::read(devices[i].m_fd, events, sizeof(events));
            if (n < 0) {
                if (errno == ENODEV) {
                    m_sink->onCaptureLost("input device removed: " + devices[i].m_name);
                    lost = true;
                }
                continue;
            }
            const size_t count = static_cast<size_t>(n) / sizeof(struct input_event);
            for (size_t e = 0; e < count; ++e) {
                const struct input_event& ev = events[e];
                if (ev.type == EV_REL) {
                    std::pair<int, int>& rel = pendingRel[devices[i].m_fd];
                    if (ev.code == REL_X) {
                        rel.first += ev.value;
                    }
                    else if (ev.code == REL_Y) {
                        rel.second += ev.value;
                    }
#ifdef REL_WHEEL_HI_RES
                    else if (ev.code == REL_WHEEL_HI_RES) {
                        m_sink->onCaptureEvent(RemoteEvent::wheel(0, ev.value));
                    }
                    else if (ev.code == REL_HWHEEL_HI_RES) {
                        m_sink->onCaptureEvent(RemoteEvent::wheel(ev.value, 0));
                    }
#endif
                    else if (ev.code == REL_WHEEL && !devices[i].m_hiResWheel) {
                        m_sink->onCaptureEvent(RemoteEvent::wheel(0, ev.value * 120));
                    }
                    else if (ev.code == REL_HWHEEL && !devices[i].m_hiResHWheel) {
                        m_sink->onCaptureEvent(RemoteEvent::wheel(ev.value * 120, 0));
                    }
                }
                else if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
                    std::pair<int, int>& rel = pendingRel[devices[i].m_fd];
                    if (rel.first != 0 || rel.second != 0) {
                        m_sink->onCaptureEvent(RemoteEvent::moveRel(rel.first, rel.second));
                        rel = std::make_pair(0, 0);
                    }
                }
                else if (ev.type == EV_KEY) {
                    ButtonID button = kButtonNone;
                    switch (ev.code) {
                    case BTN_LEFT:   button = kButtonLeft; break;
                    case BTN_RIGHT:  button = kButtonRight; break;
                    case BTN_MIDDLE: button = kButtonMiddle; break;
                    case BTN_SIDE:   button = kButtonExtra0; break;
                    case BTN_EXTRA:  button = kButtonExtra1; break;
                    default: break;
                    }
                    if (button != kButtonNone) {
                        if (ev.value == 1) {
                            m_sink->onCaptureEvent(RemoteEvent::buttonDown(button));
                        }
                        else if (ev.value == 0) {
                            m_sink->onCaptureEvent(RemoteEvent::buttonUp(button));
                        }
                        continue;
                    }
                    if (ev.code >= BTN_MISC && ev.code < KEY_OK) {
                        continue; // other buttons (joystick, touch, ...)
                    }
                    const EvdevKeyTranslator::Key key = translator.translate(ev.code, ev.value);
                    if (ev.value == 1) {
                        if (hotkey.matches(key.m_baseId, key.m_mask)) {
                            hotkeyCode = ev.code;
                            m_sink->onCaptureReleaseHotkey();
                            continue;
                        }
                        if (key.m_id == kKeyNone) {
                            continue;
                        }
                        down.insert(ev.code);
                        m_sink->onCaptureEvent(RemoteEvent::keyDown(key.m_id, key.m_mask, key.m_button));
                    }
                    else if (ev.value == 2) {
                        if (ev.code != hotkeyCode && down.count(ev.code) != 0) {
                            m_sink->onCaptureEvent(RemoteEvent::keyRepeat(key.m_id, key.m_mask,
                                                                          key.m_button, 1));
                        }
                    }
                    else if (ev.value == 0) {
                        if (ev.code == hotkeyCode) {
                            hotkeyCode = 0;
                            continue;
                        }
                        if (down.erase(ev.code) != 0) {
                            m_sink->onCaptureEvent(RemoteEvent::keyUp(key.m_id, key.m_mask, key.m_button));
                        }
                    }
                }
            }
        }
        if (lost) {
            break;
        }
    }

    closeDevices(devices);
    LOG((CLOG_INFO "remote control: evdev capture ended, devices released"));
    m_running = false;
}
