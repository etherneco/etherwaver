#include "platform/UhidInputDevices.h"

#include "base/Log.h"

#if defined(__linux__)

#include <algorithm>
#include <array>
#include <cstring>

#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <linux/uhid.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

const char* kUhidPath = "/dev/uhid";
const int kStartTimeoutMs = 3000;

const uint8_t kKeyboardReportDesc[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
    0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02,
    0x95, 0x01, 0x75, 0x08, 0x81, 0x03, 0x95, 0x06,
    0x75, 0x08, 0x15, 0x00, 0x25, 0xE7, 0x05, 0x07,
    0x19, 0x00, 0x29, 0xE7, 0x81, 0x00, 0xC0
};

const uint8_t kMouseReportDesc[] = {
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x01,
    0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01,
    0x29, 0x05, 0x15, 0x00, 0x25, 0x01, 0x95, 0x05,
    0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x03,
    0x81, 0x03, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31,
    0x09, 0x38, 0x05, 0x0C, 0x0A, 0x38, 0x02, 0x15,
    0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x04, 0x81,
    0x06, 0xC0, 0xC0
};

int uhid_write(int fd, const struct uhid_event* ev)
{
    return (write(fd, ev, sizeof(*ev)) < 0) ? -1 : 0;
}

bool wait_for_start(int fd)
{
    int remainingMs = kStartTimeoutMs;
    while (remainingMs > 0) {
        struct timeval tv;
        tv.tv_sec = remainingMs / 1000;
        tv.tv_usec = (remainingMs % 1000) * 1000;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);

        const int ready = select(fd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (ready == 0) {
            return false;
        }

        struct uhid_event ev;
        const ssize_t n = read(fd, &ev, sizeof(ev));
        if (n > 0 && ev.type == UHID_START) {
            return true;
        }

        remainingMs -= 50;
    }

    return false;
}

bool create_device(int fd, const std::string& name, const uint8_t* reportDesc, size_t reportSize)
{
    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_CREATE2;
    ev.u.create2.bus = BUS_USB;
    ev.u.create2.vendor = 0x45a7;
    ev.u.create2.product = 0x1001;
    ev.u.create2.version = 1;
    snprintf(reinterpret_cast<char*>(ev.u.create2.name), sizeof(ev.u.create2.name), "%s", name.c_str());
    memcpy(ev.u.create2.rd_data, reportDesc, reportSize);
    ev.u.create2.rd_size = reportSize;
    return (uhid_write(fd, &ev) == 0);
}

void destroy_device(int fd)
{
    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_DESTROY;
    uhid_write(fd, &ev);
}

int open_device(const std::string& deviceName, const uint8_t* reportDesc, size_t reportSize)
{
    const int fd = open(kUhidPath, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        LOG((CLOG_WARN "uhid: open %s failed (%s)", kUhidPath, strerror(errno)));
        return -1;
    }

    if (!create_device(fd, deviceName, reportDesc, reportSize)) {
        LOG((CLOG_WARN "uhid: create failed for %s (%s)", deviceName.c_str(), strerror(errno)));
        close(fd);
        return -1;
    }

    if (!wait_for_start(fd)) {
        LOG((CLOG_WARN "uhid: timed out waiting for UHID_START on %s", deviceName.c_str()));
        destroy_device(fd);
        close(fd);
        return -1;
    }

    LOG((CLOG_NOTE "uhid: started %s", deviceName.c_str()));
    return fd;
}

bool send_input_report(int fd, const void* report, size_t reportSize)
{
    if (fd < 0) {
        return false;
    }

    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_INPUT2;
    memcpy(ev.u.input2.data, report, reportSize);
    ev.u.input2.size = reportSize;
    return (uhid_write(fd, &ev) == 0);
}

struct KeyMapping {
    uint8_t usage;
    uint8_t modifierBit;
};

KeyMapping map_key_code(unsigned short code)
{
    switch (code) {
    case KEY_A: return {0x04, 0x00};
    case KEY_B: return {0x05, 0x00};
    case KEY_C: return {0x06, 0x00};
    case KEY_D: return {0x07, 0x00};
    case KEY_E: return {0x08, 0x00};
    case KEY_F: return {0x09, 0x00};
    case KEY_G: return {0x0A, 0x00};
    case KEY_H: return {0x0B, 0x00};
    case KEY_I: return {0x0C, 0x00};
    case KEY_J: return {0x0D, 0x00};
    case KEY_K: return {0x0E, 0x00};
    case KEY_L: return {0x0F, 0x00};
    case KEY_M: return {0x10, 0x00};
    case KEY_N: return {0x11, 0x00};
    case KEY_O: return {0x12, 0x00};
    case KEY_P: return {0x13, 0x00};
    case KEY_Q: return {0x14, 0x00};
    case KEY_R: return {0x15, 0x00};
    case KEY_S: return {0x16, 0x00};
    case KEY_T: return {0x17, 0x00};
    case KEY_U: return {0x18, 0x00};
    case KEY_V: return {0x19, 0x00};
    case KEY_W: return {0x1A, 0x00};
    case KEY_X: return {0x1B, 0x00};
    case KEY_Y: return {0x1C, 0x00};
    case KEY_Z: return {0x1D, 0x00};
    case KEY_1: return {0x1E, 0x00};
    case KEY_2: return {0x1F, 0x00};
    case KEY_3: return {0x20, 0x00};
    case KEY_4: return {0x21, 0x00};
    case KEY_5: return {0x22, 0x00};
    case KEY_6: return {0x23, 0x00};
    case KEY_7: return {0x24, 0x00};
    case KEY_8: return {0x25, 0x00};
    case KEY_9: return {0x26, 0x00};
    case KEY_0: return {0x27, 0x00};
    case KEY_ENTER: return {0x28, 0x00};
    case KEY_ESC: return {0x29, 0x00};
    case KEY_BACKSPACE: return {0x2A, 0x00};
    case KEY_TAB: return {0x2B, 0x00};
    case KEY_SPACE: return {0x2C, 0x00};
    case KEY_MINUS: return {0x2D, 0x00};
    case KEY_EQUAL: return {0x2E, 0x00};
    case KEY_LEFTBRACE: return {0x2F, 0x00};
    case KEY_RIGHTBRACE: return {0x30, 0x00};
    case KEY_BACKSLASH: return {0x31, 0x00};
    case KEY_SEMICOLON: return {0x33, 0x00};
    case KEY_APOSTROPHE: return {0x34, 0x00};
    case KEY_GRAVE: return {0x35, 0x00};
    case KEY_COMMA: return {0x36, 0x00};
    case KEY_DOT: return {0x37, 0x00};
    case KEY_SLASH: return {0x38, 0x00};
    case KEY_CAPSLOCK: return {0x39, 0x00};
    case KEY_F1: return {0x3A, 0x00};
    case KEY_F2: return {0x3B, 0x00};
    case KEY_F3: return {0x3C, 0x00};
    case KEY_F4: return {0x3D, 0x00};
    case KEY_F5: return {0x3E, 0x00};
    case KEY_F6: return {0x3F, 0x00};
    case KEY_F7: return {0x40, 0x00};
    case KEY_F8: return {0x41, 0x00};
    case KEY_F9: return {0x42, 0x00};
    case KEY_F10: return {0x43, 0x00};
    case KEY_F11: return {0x44, 0x00};
    case KEY_F12: return {0x45, 0x00};
    case KEY_SYSRQ: return {0x46, 0x00};
    case KEY_SCROLLLOCK: return {0x47, 0x00};
    case KEY_PAUSE: return {0x48, 0x00};
    case KEY_INSERT: return {0x49, 0x00};
    case KEY_HOME: return {0x4A, 0x00};
    case KEY_PAGEUP: return {0x4B, 0x00};
    case KEY_DELETE: return {0x4C, 0x00};
    case KEY_END: return {0x4D, 0x00};
    case KEY_PAGEDOWN: return {0x4E, 0x00};
    case KEY_RIGHT: return {0x4F, 0x00};
    case KEY_LEFT: return {0x50, 0x00};
    case KEY_DOWN: return {0x51, 0x00};
    case KEY_UP: return {0x52, 0x00};
    case KEY_NUMLOCK: return {0x53, 0x00};
    case KEY_KPSLASH: return {0x54, 0x00};
    case KEY_KPASTERISK: return {0x55, 0x00};
    case KEY_KPMINUS: return {0x56, 0x00};
    case KEY_KPPLUS: return {0x57, 0x00};
    case KEY_KPENTER: return {0x58, 0x00};
    case KEY_KP1: return {0x59, 0x00};
    case KEY_KP2: return {0x5A, 0x00};
    case KEY_KP3: return {0x5B, 0x00};
    case KEY_KP4: return {0x5C, 0x00};
    case KEY_KP5: return {0x5D, 0x00};
    case KEY_KP6: return {0x5E, 0x00};
    case KEY_KP7: return {0x5F, 0x00};
    case KEY_KP8: return {0x60, 0x00};
    case KEY_KP9: return {0x61, 0x00};
    case KEY_KP0: return {0x62, 0x00};
    case KEY_KPDOT: return {0x63, 0x00};
    case KEY_102ND: return {0x64, 0x00};
    case KEY_COMPOSE: return {0x65, 0x00};
    case KEY_POWER: return {0x66, 0x00};
    case KEY_KPEQUAL: return {0x67, 0x00};
    case KEY_F13: return {0x68, 0x00};
    case KEY_F14: return {0x69, 0x00};
    case KEY_F15: return {0x6A, 0x00};
    case KEY_F16: return {0x6B, 0x00};
    case KEY_F17: return {0x6C, 0x00};
    case KEY_F18: return {0x6D, 0x00};
    case KEY_F19: return {0x6E, 0x00};
    case KEY_F20: return {0x6F, 0x00};
    case KEY_F21: return {0x70, 0x00};
    case KEY_F22: return {0x71, 0x00};
    case KEY_F23: return {0x72, 0x00};
    case KEY_F24: return {0x73, 0x00};
    case KEY_OPEN: return {0x74, 0x00};
    case KEY_HELP: return {0x75, 0x00};
    case KEY_PROPS: return {0x76, 0x00};
    case KEY_FRONT: return {0x77, 0x00};
    case KEY_STOP: return {0x78, 0x00};
    case KEY_AGAIN: return {0x79, 0x00};
    case KEY_UNDO: return {0x7A, 0x00};
    case KEY_CUT: return {0x7B, 0x00};
    case KEY_COPY: return {0x7C, 0x00};
    case KEY_PASTE: return {0x7D, 0x00};
    case KEY_FIND: return {0x7E, 0x00};
    case KEY_MUTE: return {0x7F, 0x00};
    case KEY_VOLUMEUP: return {0x80, 0x00};
    case KEY_VOLUMEDOWN: return {0x81, 0x00};
    case KEY_LEFTCTRL: return {0x00, 0x01};
    case KEY_LEFTSHIFT: return {0x00, 0x02};
    case KEY_LEFTALT: return {0x00, 0x04};
    case KEY_LEFTMETA: return {0x00, 0x08};
    case KEY_RIGHTCTRL: return {0x00, 0x10};
    case KEY_RIGHTSHIFT: return {0x00, 0x20};
    case KEY_RIGHTALT: return {0x00, 0x40};
    case KEY_RIGHTMETA: return {0x00, 0x80};
    default: return {0x00, 0x00};
    }
}

unsigned char button_mask(unsigned short code)
{
    switch (code) {
    case BTN_LEFT: return 0x01;
    case BTN_RIGHT: return 0x02;
    case BTN_MIDDLE: return 0x04;
    case BTN_SIDE: return 0x08;
    case BTN_EXTRA: return 0x10;
    default: return 0x00;
    }
}

} // namespace

UhidKeyboardDevice::UhidKeyboardDevice()
    : m_fd(-1)
    , m_modifiers(0)
    , m_reserved(0)
    , m_keys{0, 0, 0, 0, 0, 0}
{
}

UhidKeyboardDevice::~UhidKeyboardDevice()
{
    stop();
}

bool
UhidKeyboardDevice::start(const std::string& deviceName)
{
    stop();
    m_fd = open_device(deviceName, kKeyboardReportDesc, sizeof(kKeyboardReportDesc));
    return (m_fd >= 0);
}

void
UhidKeyboardDevice::stop()
{
    if (m_fd >= 0) {
        clear();
        destroy_device(m_fd);
        close(m_fd);
        m_fd = -1;
    }
}

bool
UhidKeyboardDevice::running() const
{
    return (m_fd >= 0);
}

void
UhidKeyboardDevice::clear()
{
    m_modifiers = 0;
    memset(m_keys, 0, sizeof(m_keys));
    sendReport();
}

bool
UhidKeyboardDevice::handleKeyEvent(unsigned short code, int value)
{
    const KeyMapping mapping = map_key_code(code);
    if (mapping.usage == 0 && mapping.modifierBit == 0) {
        return false;
    }

    if (mapping.modifierBit != 0) {
        if (value) {
            m_modifiers |= mapping.modifierBit;
        }
        else {
            m_modifiers &= static_cast<unsigned char>(~mapping.modifierBit);
        }
        return sendReport();
    }

    if (value) {
        for (size_t i = 0; i < 6; ++i) {
            if (m_keys[i] == mapping.usage) {
                return sendReport();
            }
        }

        for (size_t i = 0; i < 6; ++i) {
            if (m_keys[i] == 0) {
                m_keys[i] = mapping.usage;
                return sendReport();
            }
        }

        LOG((CLOG_WARN "uhid-keyboard: rollover overflow for key code=%u", code));
        return false;
    }

    for (size_t i = 0; i < 6; ++i) {
        if (m_keys[i] == mapping.usage) {
            m_keys[i] = 0;
        }
    }
    return sendReport();
}

bool
UhidKeyboardDevice::tapKey(unsigned short code)
{
    return pressKey(code) && releaseKey(code);
}

bool
UhidKeyboardDevice::pressKey(unsigned short code)
{
    return handleKeyEvent(code, 1);
}

bool
UhidKeyboardDevice::releaseKey(unsigned short code)
{
    return handleKeyEvent(code, 0);
}

bool
UhidKeyboardDevice::pressChord(std::initializer_list<unsigned short> codes)
{
    for (unsigned short code : codes) {
        if (!pressKey(code)) {
            return false;
        }
    }
    return true;
}

bool
UhidKeyboardDevice::releaseChord(std::initializer_list<unsigned short> codes)
{
    for (auto it = codes.end(); it != codes.begin();) {
        --it;
        if (!releaseKey(*it)) {
            return false;
        }
    }
    return true;
}

bool
UhidKeyboardDevice::sendReport() const
{
    struct KeyboardReport {
        uint8_t reportId;
        uint8_t modifiers;
        uint8_t reserved;
        uint8_t keys[6];
    } report;

    report.reportId = 0x01;
    report.modifiers = m_modifiers;
    report.reserved = m_reserved;
    memcpy(report.keys, m_keys, sizeof(m_keys));
    return send_input_report(m_fd, &report, sizeof(report));
}

UhidMouseDevice::UhidMouseDevice()
    : m_fd(-1)
    , m_buttons(0)
{
}

UhidMouseDevice::~UhidMouseDevice()
{
    stop();
}

bool
UhidMouseDevice::start(const std::string& deviceName)
{
    stop();
    m_fd = open_device(deviceName, kMouseReportDesc, sizeof(kMouseReportDesc));
    return (m_fd >= 0);
}

void
UhidMouseDevice::stop()
{
    if (m_fd >= 0) {
        clear();
        destroy_device(m_fd);
        close(m_fd);
        m_fd = -1;
    }
}

bool
UhidMouseDevice::running() const
{
    return (m_fd >= 0);
}

void
UhidMouseDevice::clear()
{
    m_buttons = 0;
    sendReport(0, 0, 0, 0);
}

bool
UhidMouseDevice::handleButtonEvent(unsigned short code, int value)
{
    const unsigned char mask = button_mask(code);
    if (mask == 0) {
        return false;
    }

    if (value) {
        m_buttons |= mask;
    }
    else {
        m_buttons &= static_cast<unsigned char>(~mask);
    }

    return sendReport(0, 0, 0, 0);
}

bool
UhidMouseDevice::moveRelative(int dx, int dy)
{
    while (dx != 0 || dy != 0) {
        const int chunkDx = std::max(-127, std::min(127, dx));
        const int chunkDy = std::max(-127, std::min(127, dy));
        if (!sendReport(static_cast<signed char>(chunkDx), static_cast<signed char>(chunkDy), 0, 0)) {
            return false;
        }
        dx -= chunkDx;
        dy -= chunkDy;
    }
    return true;
}

bool
UhidMouseDevice::wheel(int xDelta, int yDelta)
{
    while (xDelta != 0 || yDelta != 0) {
        const int chunkX = std::max(-127, std::min(127, xDelta));
        const int chunkY = std::max(-127, std::min(127, yDelta));
        if (!sendReport(0, 0, static_cast<signed char>(chunkY), static_cast<signed char>(chunkX))) {
            return false;
        }
        xDelta -= chunkX;
        yDelta -= chunkY;
    }
    return true;
}

bool
UhidMouseDevice::sendReport(signed char dx, signed char dy, signed char wheel, signed char pan) const
{
    struct MouseReport {
        uint8_t reportId;
        uint8_t buttons;
        int8_t dx;
        int8_t dy;
        int8_t wheel;
        int8_t pan;
    } report;

    report.reportId = 0x01;
    report.buttons = m_buttons;
    report.dx = dx;
    report.dy = dy;
    report.wheel = wheel;
    report.pan = pan;
    return send_input_report(m_fd, &report, sizeof(report));
}

#endif
