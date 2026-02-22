#pragma once

#include "barrier/key_types.h"
#include "barrier/mouse_types.h"
#include "base/String.h"

#include <array>

class UhidServer {
public:
    UhidServer();
    ~UhidServer();

    bool start(const String& deviceName);
    void stop();
    bool running() const;
    void clearInputState();

    bool keyDown(KeyID id, KeyModifierMask mask);
    bool keyRepeat(KeyID id, KeyModifierMask mask, SInt32 count);
    bool keyUp(KeyID id, KeyModifierMask mask);

    bool mouseDown(ButtonID id);
    bool mouseUp(ButtonID id);
    bool mouseMoveAbsolute(SInt32 x, SInt32 y);
    bool mouseRelativeMove(SInt32 dx, SInt32 dy);
    bool mouseWheel(SInt32 xDelta, SInt32 yDelta);

private:
    bool sendKeyboardReport();
    bool sendMouseReport(SInt8 dx, SInt8 dy, SInt8 wheel, SInt8 pan);
    bool updateMouseButtons(ButtonID id, bool pressed);
    bool sendRelativeMotion(SInt32 dx, SInt32 dy);
    bool sendWheelMotion(SInt32 xDelta, SInt32 yDelta);

private:
    bool m_running;
    int m_uhidFd;
    bool m_hasLastAbsolute;
    SInt32 m_lastAbsX;
    SInt32 m_lastAbsY;
    UInt8 m_mouseButtons;
    UInt8 m_keyboardModifiers;
    std::array<UInt8, 6> m_keyboardKeys;
};
