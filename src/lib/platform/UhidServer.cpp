#include "platform/UhidServer.h"

#include "base/Log.h"

#if defined(__linux__)

#include <linux/uhid.h>

#include <algorithm>
#include <cstring>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

static const char* kUhidPath = "/dev/uhid";
static const size_t kKeyboardSlots = 6;
static const int kStartTimeoutMs = 3000;

static const uint8_t kHidReportDesc[] = {
    0x05, 0x01,
    0x09, 0x02,
    0xA1, 0x01,
    0x85, 0x01,
    0x09, 0x01,
    0xA1, 0x00,
    0x05, 0x09,
    0x19, 0x01,
    0x29, 0x05,
    0x15, 0x00,
    0x25, 0x01,
    0x95, 0x05,
    0x75, 0x01,
    0x81, 0x02,
    0x95, 0x01,
    0x75, 0x03,
    0x81, 0x03,
    0x05, 0x01,
    0x09, 0x30,
    0x09, 0x31,
    0x09, 0x38,
    0x05, 0x0C,
    0x0A, 0x38, 0x02,
    0x15, 0x81,
    0x25, 0x7F,
    0x75, 0x08,
    0x95, 0x04,
    0x81, 0x06,
    0xC0,
    0xC0,

    0x05, 0x01,
    0x09, 0x06,
    0xA1, 0x01,
    0x85, 0x02,
    0x05, 0x07,
    0x19, 0xE0,
    0x29, 0xE7,
    0x15, 0x00,
    0x25, 0x01,
    0x75, 0x01,
    0x95, 0x08,
    0x81, 0x02,
    0x95, 0x01,
    0x75, 0x08,
    0x81, 0x03,
    0x95, 0x06,
    0x75, 0x08,
    0x15, 0x00,
    0x25, 0x65,
    0x05, 0x07,
    0x19, 0x00,
    0x29, 0x65,
    0x81, 0x00,
    0xC0
};

static int uhid_write(int fd, const struct uhid_event* ev)
{
    ssize_t ret = write(fd, ev, sizeof(*ev));
    return (ret < 0) ? -1 : 0;
}

static int uhid_create(int fd, const String& deviceName)
{
    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_CREATE2;

    struct uhid_create2_req* c = &ev.u.create2;
    const char* name = deviceName.empty() ? "BarrierVirtual HID" : deviceName.c_str();
    snprintf(reinterpret_cast<char*>(c->name), sizeof(c->name), "%s", name);

    c->bus = BUS_USB;
    c->vendor = 0x1234;
    c->product = 0x5678;
    c->version = 1;
    c->country = 0;
    memcpy(c->rd_data, kHidReportDesc, sizeof(kHidReportDesc));
    c->rd_size = sizeof(kHidReportDesc);

    return uhid_write(fd, &ev);
}

static void uhid_destroy(int fd)
{
    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_DESTROY;
    uhid_write(fd, &ev);
}

static bool wait_for_start(int uhidFd)
{
    int remainingMs = kStartTimeoutMs;
    while (remainingMs > 0) {
        struct timeval tv;
        tv.tv_sec = remainingMs / 1000;
        tv.tv_usec = (remainingMs % 1000) * 1000;

        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(uhidFd, &rfds);

        int ret = select(uhidFd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (ret == 0) {
            return false;
        }

        struct uhid_event ev;
        ssize_t n = read(uhidFd, &ev, sizeof(ev));
        if (n > 0 && ev.type == UHID_START) {
            return true;
        }

        remainingMs -= 50;
    }

    return false;
}

struct KeyMapResult {
    uint8_t usage;
    uint8_t requiredModifiers;
    uint8_t modifierBit;
    bool isModifier;
};

static uint8_t modifier_from_mask(KeyModifierMask mask)
{
    uint8_t mods = 0;
    if ((mask & KeyModifierControl) != 0) {
        mods |= 0x01;
    }
    if ((mask & KeyModifierShift) != 0) {
        mods |= 0x02;
    }
    if ((mask & KeyModifierAlt) != 0) {
        mods |= 0x04;
    }
    if ((mask & KeyModifierMeta) != 0 || (mask & KeyModifierSuper) != 0) {
        mods |= 0x08;
    }
    if ((mask & KeyModifierAltGr) != 0) {
        mods |= 0x40;
    }
    return mods;
}

static KeyMapResult map_ascii(KeyID id)
{
    KeyMapResult out = {0, 0, 0, false};
    if (id > 0x7f) {
        return out;
    }

    const char c = static_cast<char>(id);

    if (c >= 'a' && c <= 'z') {
        out.usage = static_cast<uint8_t>(0x04 + (c - 'a'));
        return out;
    }

    if (c >= 'A' && c <= 'Z') {
        out.usage = static_cast<uint8_t>(0x04 + (c - 'A'));
        out.requiredModifiers = 0x02;
        return out;
    }

    if (c >= '1' && c <= '9') {
        out.usage = static_cast<uint8_t>(0x1e + (c - '1'));
        return out;
    }

    if (c == '0') {
        out.usage = 0x27;
        return out;
    }

    switch (c) {
    case '!': out.usage = 0x1e; out.requiredModifiers = 0x02; break;
    case '@': out.usage = 0x1f; out.requiredModifiers = 0x02; break;
    case '#': out.usage = 0x20; out.requiredModifiers = 0x02; break;
    case '$': out.usage = 0x21; out.requiredModifiers = 0x02; break;
    case '%': out.usage = 0x22; out.requiredModifiers = 0x02; break;
    case '^': out.usage = 0x23; out.requiredModifiers = 0x02; break;
    case '&': out.usage = 0x24; out.requiredModifiers = 0x02; break;
    case '*': out.usage = 0x25; out.requiredModifiers = 0x02; break;
    case '(': out.usage = 0x26; out.requiredModifiers = 0x02; break;
    case ')': out.usage = 0x27; out.requiredModifiers = 0x02; break;
    case '-': out.usage = 0x2d; break;
    case '_': out.usage = 0x2d; out.requiredModifiers = 0x02; break;
    case '=': out.usage = 0x2e; break;
    case '+': out.usage = 0x2e; out.requiredModifiers = 0x02; break;
    case '[': out.usage = 0x2f; break;
    case '{': out.usage = 0x2f; out.requiredModifiers = 0x02; break;
    case ']': out.usage = 0x30; break;
    case '}': out.usage = 0x30; out.requiredModifiers = 0x02; break;
    case '\\': out.usage = 0x31; break;
    case '|': out.usage = 0x31; out.requiredModifiers = 0x02; break;
    case ';': out.usage = 0x33; break;
    case ':': out.usage = 0x33; out.requiredModifiers = 0x02; break;
    case '\'': out.usage = 0x34; break;
    case '"': out.usage = 0x34; out.requiredModifiers = 0x02; break;
    case '`': out.usage = 0x35; break;
    case '~': out.usage = 0x35; out.requiredModifiers = 0x02; break;
    case ',': out.usage = 0x36; break;
    case '<': out.usage = 0x36; out.requiredModifiers = 0x02; break;
    case '.': out.usage = 0x37; break;
    case '>': out.usage = 0x37; out.requiredModifiers = 0x02; break;
    case '/': out.usage = 0x38; break;
    case '?': out.usage = 0x38; out.requiredModifiers = 0x02; break;
    case ' ': out.usage = 0x2c; break;
    default: break;
    }

    return out;
}

static KeyMapResult map_key(KeyID id)
{
    KeyMapResult out = map_ascii(id);
    if (out.usage != 0 || out.isModifier) {
        return out;
    }

    switch (id) {
    case kKeyReturn:
    case kKeyKP_Enter:
        out.usage = 0x28;
        break;
    case kKeyEscape:
        out.usage = 0x29;
        break;
    case kKeyBackSpace:
        out.usage = 0x2a;
        break;
    case kKeyTab:
    case kKeyLeftTab:
        out.usage = 0x2b;
        break;
    case kKeyDelete:
        out.usage = 0x4c;
        break;
    case kKeyInsert:
        out.usage = 0x49;
        break;
    case kKeyHome:
        out.usage = 0x4a;
        break;
    case kKeyEnd:
        out.usage = 0x4d;
        break;
    case kKeyPageUp:
        out.usage = 0x4b;
        break;
    case kKeyPageDown:
        out.usage = 0x4e;
        break;
    case kKeyRight:
        out.usage = 0x4f;
        break;
    case kKeyLeft:
        out.usage = 0x50;
        break;
    case kKeyDown:
        out.usage = 0x51;
        break;
    case kKeyUp:
        out.usage = 0x52;
        break;
    case kKeyNumLock:
        out.usage = 0x53;
        break;
    case kKeyKP_Divide:
        out.usage = 0x54;
        break;
    case kKeyKP_Multiply:
        out.usage = 0x55;
        break;
    case kKeyKP_Subtract:
        out.usage = 0x56;
        break;
    case kKeyKP_Add:
        out.usage = 0x57;
        break;
    case kKeyKP_Decimal:
    case kKeyKP_Delete:
        out.usage = 0x63;
        break;
    case kKeyKP_0:
    case kKeyKP_Insert:
        out.usage = 0x62;
        break;
    case kKeyKP_1:
    case kKeyKP_End:
        out.usage = 0x59;
        break;
    case kKeyKP_2:
    case kKeyKP_Down:
        out.usage = 0x5a;
        break;
    case kKeyKP_3:
    case kKeyKP_PageDown:
        out.usage = 0x5b;
        break;
    case kKeyKP_4:
    case kKeyKP_Left:
        out.usage = 0x5c;
        break;
    case kKeyKP_5:
    case kKeyKP_Begin:
        out.usage = 0x5d;
        break;
    case kKeyKP_6:
    case kKeyKP_Right:
        out.usage = 0x5e;
        break;
    case kKeyKP_7:
    case kKeyKP_Home:
        out.usage = 0x5f;
        break;
    case kKeyKP_8:
    case kKeyKP_Up:
        out.usage = 0x60;
        break;
    case kKeyKP_9:
    case kKeyKP_PageUp:
        out.usage = 0x61;
        break;
    case kKeyCapsLock:
        out.usage = 0x39;
        break;
    case kKeyPrint:
        out.usage = 0x46;
        break;
    case kKeyScrollLock:
        out.usage = 0x47;
        break;
    case kKeyPause:
        out.usage = 0x48;
        break;
    case kKeyMenu:
        out.usage = 0x65;
        break;
    case kKeyShift_L:
        out.isModifier = true;
        out.modifierBit = 0x02;
        break;
    case kKeyShift_R:
        out.isModifier = true;
        out.modifierBit = 0x20;
        break;
    case kKeyControl_L:
        out.isModifier = true;
        out.modifierBit = 0x01;
        break;
    case kKeyControl_R:
        out.isModifier = true;
        out.modifierBit = 0x10;
        break;
    case kKeyAlt_L:
        out.isModifier = true;
        out.modifierBit = 0x04;
        break;
    case kKeyAlt_R:
    case kKeyAltGr:
        out.isModifier = true;
        out.modifierBit = 0x40;
        break;
    case kKeyMeta_L:
    case kKeySuper_L:
        out.isModifier = true;
        out.modifierBit = 0x08;
        break;
    case kKeyMeta_R:
    case kKeySuper_R:
        out.isModifier = true;
        out.modifierBit = 0x80;
        break;
    default:
        break;
    }

    if (out.usage == 0 && id >= kKeyF1 && id <= kKeyF12) {
        out.usage = static_cast<uint8_t>(0x3a + (id - kKeyF1));
    }
    else if (out.usage == 0 && id >= kKeyF13 && id <= kKeyF24) {
        out.usage = static_cast<uint8_t>(0x68 + (id - kKeyF13));
    }

    return out;
}

} // namespace

UhidServer::UhidServer()
    : m_running(false)
    , m_uhidFd(-1)
    , m_hasLastAbsolute(false)
    , m_lastAbsX(0)
    , m_lastAbsY(0)
    , m_mouseButtons(0)
    , m_keyboardModifiers(0)
{
    m_keyboardKeys.fill(0);
}

UhidServer::~UhidServer()
{
    stop();
}

bool UhidServer::start(const String& deviceName)
{
    if (m_running) {
        return true;
    }

    int fd = open(kUhidPath, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        LOG((CLOG_WARN "uhid: open %s failed (%s)", kUhidPath, strerror(errno)));
        return false;
    }

    if (uhid_create(fd, deviceName) < 0) {
        LOG((CLOG_WARN "uhid: create failed (%s)", strerror(errno)));
        close(fd);
        return false;
    }

    if (!wait_for_start(fd)) {
        LOG((CLOG_WARN "uhid: timed out waiting for UHID_START"));
        uhid_destroy(fd);
        close(fd);
        return false;
    }

    m_uhidFd = fd;
    m_running = true;
    clearInputState();
    LOG((CLOG_NOTE "uhid: connected to Barrier input stream"));
    return true;
}

void UhidServer::stop()
{
    if (!m_running) {
        return;
    }

    clearInputState();
    uhid_destroy(m_uhidFd);
    close(m_uhidFd);
    m_uhidFd = -1;
    m_running = false;
}

bool UhidServer::running() const
{
    return m_running;
}

void UhidServer::clearInputState()
{
    m_hasLastAbsolute = false;
    m_lastAbsX = 0;
    m_lastAbsY = 0;
    m_mouseButtons = 0;
    m_keyboardModifiers = 0;
    m_keyboardKeys.fill(0);

    if (m_running) {
        sendKeyboardReport();
        sendMouseReport(0, 0, 0, 0);
    }
}

bool UhidServer::sendKeyboardReport()
{
    if (!m_running) {
        return false;
    }

    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_INPUT2;

    uint8_t report[9];
    memset(report, 0, sizeof(report));
    report[0] = 0x02;
    report[1] = m_keyboardModifiers;
    for (size_t i = 0; i < kKeyboardSlots; ++i) {
        report[3 + i] = m_keyboardKeys[i];
    }

    memcpy(ev.u.input2.data, report, sizeof(report));
    ev.u.input2.size = sizeof(report);
    return (uhid_write(m_uhidFd, &ev) == 0);
}

bool UhidServer::sendMouseReport(SInt8 dx, SInt8 dy, SInt8 wheel, SInt8 pan)
{
    if (!m_running) {
        return false;
    }

    struct uhid_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = UHID_INPUT2;

    uint8_t report[6];
    report[0] = 0x01;
    report[1] = m_mouseButtons;
    report[2] = static_cast<uint8_t>(dx);
    report[3] = static_cast<uint8_t>(dy);
    report[4] = static_cast<uint8_t>(wheel);
    report[5] = static_cast<uint8_t>(pan);

    memcpy(ev.u.input2.data, report, sizeof(report));
    ev.u.input2.size = sizeof(report);
    return (uhid_write(m_uhidFd, &ev) == 0);
}

bool UhidServer::updateMouseButtons(ButtonID id, bool pressed)
{
    uint8_t bit = 0;
    if (id == kButtonLeft) {
        bit = 0x01;
    }
    else if (id == kButtonRight) {
        bit = 0x02;
    }
    else if (id == kButtonMiddle) {
        bit = 0x04;
    }
    else if (id == kButtonExtra0) {
        bit = 0x08;
    }
    else if (id == kButtonExtra1) {
        bit = 0x10;
    }

    if (bit == 0) {
        return true;
    }

    if (pressed) {
        m_mouseButtons |= bit;
    }
    else {
        m_mouseButtons &= static_cast<uint8_t>(~bit);
    }

    return sendMouseReport(0, 0, 0, 0);
}

bool UhidServer::sendRelativeMotion(SInt32 dx, SInt32 dy)
{
    if (!m_running) {
        return false;
    }

    while (dx != 0 || dy != 0) {
        const SInt32 stepX = std::max<SInt32>(-127, std::min<SInt32>(127, dx));
        const SInt32 stepY = std::max<SInt32>(-127, std::min<SInt32>(127, dy));

        if (!sendMouseReport(static_cast<SInt8>(stepX), static_cast<SInt8>(stepY), 0, 0)) {
            return false;
        }

        dx -= stepX;
        dy -= stepY;
    }

    return true;
}

bool UhidServer::sendWheelMotion(SInt32 xDelta, SInt32 yDelta)
{
    if (!m_running) {
        return false;
    }

    SInt32 wheelSteps = 0;
    SInt32 panSteps = 0;

    if (yDelta != 0) {
        wheelSteps = yDelta / 120;
        if (wheelSteps == 0) {
            wheelSteps = (yDelta > 0) ? 1 : -1;
        }
    }

    if (xDelta != 0) {
        panSteps = xDelta / 120;
        if (panSteps == 0) {
            panSteps = (xDelta > 0) ? 1 : -1;
        }
    }

    while (wheelSteps != 0 || panSteps != 0) {
        const SInt32 stepWheel = std::max<SInt32>(-127, std::min<SInt32>(127, wheelSteps));
        const SInt32 stepPan = std::max<SInt32>(-127, std::min<SInt32>(127, panSteps));

        if (!sendMouseReport(0, 0, static_cast<SInt8>(stepWheel), static_cast<SInt8>(stepPan))) {
            return false;
        }

        wheelSteps -= stepWheel;
        panSteps -= stepPan;
    }

    return true;
}

bool UhidServer::keyDown(KeyID id, KeyModifierMask mask)
{
    if (!m_running) {
        return false;
    }

    const KeyMapResult key = map_key(id);
    m_keyboardModifiers = modifier_from_mask(mask);

    if (key.isModifier) {
        m_keyboardModifiers |= key.modifierBit;
        return sendKeyboardReport();
    }

    if (key.usage == 0) {
        return true;
    }

    m_keyboardModifiers |= key.requiredModifiers;

    for (size_t i = 0; i < kKeyboardSlots; ++i) {
        if (m_keyboardKeys[i] == key.usage) {
            return sendKeyboardReport();
        }
    }

    for (size_t i = 0; i < kKeyboardSlots; ++i) {
        if (m_keyboardKeys[i] == 0) {
            m_keyboardKeys[i] = key.usage;
            return sendKeyboardReport();
        }
    }

    m_keyboardKeys[kKeyboardSlots - 1] = key.usage;
    return sendKeyboardReport();
}

bool UhidServer::keyRepeat(KeyID id, KeyModifierMask mask, SInt32 count)
{
    if (!m_running || count <= 0) {
        return false;
    }

    const KeyMapResult key = map_key(id);
    if (key.isModifier || key.usage == 0) {
        return true;
    }

    for (SInt32 i = 0; i < count; ++i) {
        keyDown(id, mask);
        keyUp(id, mask);
    }

    return true;
}

bool UhidServer::keyUp(KeyID id, KeyModifierMask mask)
{
    if (!m_running) {
        return false;
    }

    const KeyMapResult key = map_key(id);
    m_keyboardModifiers = modifier_from_mask(mask);

    if (key.isModifier) {
        m_keyboardModifiers &= static_cast<uint8_t>(~key.modifierBit);
        return sendKeyboardReport();
    }

    if (key.usage == 0) {
        return true;
    }

    for (size_t i = 0; i < kKeyboardSlots; ++i) {
        if (m_keyboardKeys[i] == key.usage) {
            m_keyboardKeys[i] = 0;
        }
    }

    return sendKeyboardReport();
}

bool UhidServer::mouseDown(ButtonID id)
{
    if (!m_running) {
        return false;
    }

    return updateMouseButtons(id, true);
}

bool UhidServer::mouseUp(ButtonID id)
{
    if (!m_running) {
        return false;
    }

    return updateMouseButtons(id, false);
}

bool UhidServer::mouseMoveAbsolute(SInt32 x, SInt32 y)
{
    if (!m_running) {
        return false;
    }

    if (!m_hasLastAbsolute) {
        m_lastAbsX = x;
        m_lastAbsY = y;
        m_hasLastAbsolute = true;
        return true;
    }

    const SInt32 dx = x - m_lastAbsX;
    const SInt32 dy = y - m_lastAbsY;
    m_lastAbsX = x;
    m_lastAbsY = y;
    return sendRelativeMotion(dx, dy);
}

bool UhidServer::mouseRelativeMove(SInt32 dx, SInt32 dy)
{
    if (!m_running) {
        return false;
    }

    return sendRelativeMotion(dx, dy);
}

bool UhidServer::mouseWheel(SInt32 xDelta, SInt32 yDelta)
{
    return sendWheelMotion(xDelta, yDelta);
}

#else

UhidServer::UhidServer()
    : m_running(false)
    , m_uhidFd(-1)
    , m_hasLastAbsolute(false)
    , m_lastAbsX(0)
    , m_lastAbsY(0)
    , m_mouseButtons(0)
    , m_keyboardModifiers(0)
{
    m_keyboardKeys.fill(0);
}

UhidServer::~UhidServer()
{
}

bool UhidServer::start(const String&)
{
    return false;
}

void UhidServer::stop()
{
}

bool UhidServer::running() const
{
    return false;
}

void UhidServer::clearInputState()
{
}

bool UhidServer::keyDown(KeyID, KeyModifierMask)
{
    return false;
}

bool UhidServer::keyRepeat(KeyID, KeyModifierMask, SInt32)
{
    return false;
}

bool UhidServer::keyUp(KeyID, KeyModifierMask)
{
    return false;
}

bool UhidServer::mouseDown(ButtonID)
{
    return false;
}

bool UhidServer::mouseUp(ButtonID)
{
    return false;
}

bool UhidServer::mouseMoveAbsolute(SInt32, SInt32)
{
    return false;
}

bool UhidServer::mouseRelativeMove(SInt32, SInt32)
{
    return false;
}

bool UhidServer::mouseWheel(SInt32, SInt32)
{
    return false;
}

bool UhidServer::sendKeyboardReport()
{
    return false;
}

bool UhidServer::sendMouseReport(SInt8, SInt8, SInt8, SInt8)
{
    return false;
}

bool UhidServer::updateMouseButtons(ButtonID, bool)
{
    return false;
}

bool UhidServer::sendRelativeMotion(SInt32, SInt32)
{
    return false;
}

bool UhidServer::sendWheelMotion(SInt32, SInt32)
{
    return false;
}

#endif
