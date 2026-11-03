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

#pragma once

#include "barrier/key_types.h"
#include "barrier/mouse_types.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <string>
#include <vector>

//
// Shared building blocks for "remote control" (protocol 1.8): a client
// captures its *local* keyboard and mouse and forwards the events to the
// server, which injects them through the existing input path.
//
// Nothing in this module logs key identifiers or typed text.
//

namespace barrier {
namespace remote {

//! Kind of a forwarded input event (wire value of kMsgDRemoteInput $2)
enum EventKind {
    kEventNone        = 0,
    kEventKeyDown     = 1,  // a = KeyID, b = mask, c = KeyButton
    kEventKeyUp       = 2,  // a = KeyID, b = mask, c = KeyButton
    kEventKeyRepeat   = 3,  // a = KeyID, b = mask, c = KeyButton, d = count
    kEventButtonDown  = 4,  // a = ButtonID
    kEventButtonUp    = 5,  // a = ButtonID
    kEventMoveAbs     = 6,  // a = x, b = y (target host coordinates)
    kEventMoveRel     = 7,  // a = dx, b = dy
    kEventWheel       = 8,  // a = xDelta, b = yDelta (120 per notch)
    kEventHeartbeat   = 9,  // keeps the control lease alive
    kEventLast        = kEventHeartbeat
};

//! State reported by the server (wire value of kMsgCRemoteControlState $1)
enum ControlState {
    kStateIdle           = 0, // nobody controls the server
    kStateGranted        = 1, // receiving client controls the target
    kStateControlledByOther = 2, // another client controls the target
    kStateDenied         = 3, // request refused, see reason
    kStateRevoked        = 4, // control taken away, see reason
    kStateReleased       = 5  // control released (by request)
};

//! Reason codes (wire value of kMsgCRemoteControlState $2)
enum ControlReason {
    kReasonNone            = 0,
    kReasonNotPermitted    = 1, // screen is configured as view only
    kReasonBusy            = 2, // another client has control, takeover disabled
    kReasonTakenOver       = 3, // another client took over
    kReasonTimeout         = 4, // no heartbeat within the lease timeout
    kReasonUnknownTarget   = 5, // requested host does not exist / not connected
    kReasonTargetIsSelf    = 6, // a client cannot control itself
    kReasonTargetChanged   = 7, // controlled host changed (edge or hotkey switch)
    kReasonServerShutdown  = 8,
    kReasonQueueOverflow   = 9, // client input queue overflowed
    kReasonProtocol        = 10 // malformed or stale message
};

//! Capability flags (wire value of kMsgCRemoteControlState $6)
enum ControlCapability {
    kCapAbsolute        = 0x01, // absolute moves supported
    kCapRelative        = 0x02, // relative moves supported
    kCapHorizontalWheel = 0x04,
    kCapNativeRepeat    = 0x08, // target generates autorepeat itself
    kCapCtrlAltDel      = 0x10  // target can emulate ctrl+alt+del
};

const char*            controlStateName(int state);
const char*            controlReasonText(int reason);

//! A single forwarded input event
struct Event {
    Event() : m_kind(kEventNone), m_a(0), m_b(0), m_c(0), m_d(0) { }
    Event(EventKind kind, SInt32 a = 0, SInt32 b = 0, SInt32 c = 0, SInt32 d = 0) :
        m_kind(kind), m_a(a), m_b(b), m_c(c), m_d(d) { }

    static Event        keyDown(KeyID id, KeyModifierMask mask, KeyButton button)
                        { return Event(kEventKeyDown, (SInt32)id, (SInt32)mask, (SInt32)button); }
    static Event        keyUp(KeyID id, KeyModifierMask mask, KeyButton button)
                        { return Event(kEventKeyUp, (SInt32)id, (SInt32)mask, (SInt32)button); }
    static Event        keyRepeat(KeyID id, KeyModifierMask mask, KeyButton button, SInt32 count)
                        { return Event(kEventKeyRepeat, (SInt32)id, (SInt32)mask, (SInt32)button, count); }
    static Event        buttonDown(ButtonID id) { return Event(kEventButtonDown, (SInt32)id); }
    static Event        buttonUp(ButtonID id)   { return Event(kEventButtonUp, (SInt32)id); }
    static Event        moveAbs(SInt32 x, SInt32 y) { return Event(kEventMoveAbs, x, y); }
    static Event        moveRel(SInt32 dx, SInt32 dy) { return Event(kEventMoveRel, dx, dy); }
    static Event        wheel(SInt32 dx, SInt32 dy) { return Event(kEventWheel, dx, dy); }

    bool                isMotion() const
                        { return m_kind == kEventMoveAbs || m_kind == kEventMoveRel; }
    //! true for events that change key or button state (never dropped)
    bool                isStateChange() const;

    bool                operator==(const Event& o) const
                        { return m_kind == o.m_kind && m_a == o.m_a && m_b == o.m_b &&
                                 m_c == o.m_c && m_d == o.m_d; }

    EventKind           m_kind;
    SInt32              m_a, m_b, m_c, m_d;
};

//! Validates an event received from the network
bool                    isValidEvent(int kind, SInt32 a, SInt32 b, SInt32 c, SInt32 d);

//! Tracks keys and mouse buttons that are currently held down
/*!
Used on both ends: the client uses it to drop unmatched releases and to
release everything when it stops capturing; the server uses it to release
everything that a (possibly crashed) controller left pressed.
*/
class HeldInputState {
public:
    //! Record a key press.  Returns false if the key is already down
    //! (the caller should treat the event as an autorepeat).
    bool                pressKey(KeyID id, KeyModifierMask mask, KeyButton button);

    //! Record a key release.  Returns false if the key was not down
    //! (an unmatched release that must not be forwarded).
    bool                releaseKey(KeyButton button);

    bool                isKeyDown(KeyButton button) const;

    //! Record a mouse button press.  Returns false if already down.
    bool                pressButton(ButtonID id);
    //! Record a mouse button release.  Returns false if it was not down.
    bool                releaseButton(ButtonID id);
    bool                isButtonDown(ButtonID id) const;

    bool                empty() const { return m_keys.empty() && m_buttons.empty(); }
    size_t              keyCount() const { return m_keys.size(); }
    size_t              buttonCount() const { return m_buttons.size(); }
    bool                anyButtonDown() const { return !m_buttons.empty(); }

    //! Returns release events for everything held (buttons first, then
    //! ordinary keys, then modifier keys, newest first) and clears state.
    std::vector<Event>  releaseAll();

    void                clear();

private:
    struct HeldKey {
        KeyID           m_id;
        KeyModifierMask m_mask;
        KeyButton       m_button;
        UInt32          m_order;
    };

    std::map<KeyButton, HeldKey> m_keys;
    std::map<ButtonID, UInt32>   m_buttons;
    UInt32              m_counter = 0;
};

//! true if \p id is a modifier key (shift, control, alt, altgr, super...)
bool                    isModifierKey(KeyID id);

//! Bounded FIFO of outgoing input events
/*!
Consecutive mouse motion (and wheel) events are merged so a slow link
never loses key or button transitions.  When the queue is full the
oldest motion event is dropped; if no motion event can be dropped the
push fails with kOverflow and the caller must end remote control safely.
*/
class InputQueue {
public:
    enum PushResult { kQueued, kCoalesced, kDroppedMotion, kOverflow };

    explicit InputQueue(size_t capacity = 512);

    PushResult          push(const Event& event);
    bool                pop(Event& event);
    bool                empty() const { return m_events.empty(); }
    size_t              size() const { return m_events.size(); }
    size_t              capacity() const { return m_capacity; }
    void                clear() { m_events.clear(); }

    // statistics (no content)
    std::uint64_t              coalescedCount() const { return m_coalesced; }
    std::uint64_t              droppedCount() const { return m_dropped; }

private:
    size_t              m_capacity;
    std::deque<Event>   m_events;
    std::uint64_t              m_coalesced = 0;
    std::uint64_t              m_dropped = 0;
};

//! Local emergency / toggle hotkey
class Hotkey {
public:
    Hotkey() : m_key(kKeyNone), m_mask(0) { }
    Hotkey(KeyID key, KeyModifierMask mask) : m_key(key), m_mask(mask) { }

    //! Parse "Ctrl+Alt+Shift+Q", "Control+Super+Escape", ...
    static bool         parse(const std::string& text, Hotkey& out, std::string* error = NULL);

    //! Default release shortcut.  Ctrl+Alt+Shift+Q was chosen because it
    //! is not reserved on Windows (Ctrl+Shift+Esc, Ctrl+Alt+Del), macOS
    //! (Cmd+Opt+Esc, Cmd+Shift+Q use Command, not Control), GNOME, KDE
    //! (Ctrl+Alt+Esc = xkill) or the Linux VT switcher (Ctrl+Alt+Fn), and
    //! the Q key sits in the same place on UK and Polish Programmer layouts.
    static Hotkey       defaultRelease();

    std::string         toString() const;
    bool                isValid() const { return m_key != kKeyNone; }

    //! true if pressing \p key (the unshifted key symbol) while
    //! modifiers \p mask are active triggers the hotkey.  Lock
    //! modifiers are ignored.
    bool                matches(KeyID key, KeyModifierMask mask) const;

    KeyID               key() const { return m_key; }
    KeyModifierMask     mask() const { return m_mask; }

private:
    KeyID               m_key;
    KeyModifierMask     m_mask;
};

//! Relevant modifier bits for hotkey matching
KeyModifierMask         hotkeyModifiers(KeyModifierMask mask);

//! Fold a key symbol to its lower-case form (ASCII and Latin-1 letters)
KeyID                   foldKeyCase(KeyID id);

//! Rectangle used for coordinate mapping
struct Rect {
    Rect() : m_x(0), m_y(0), m_w(0), m_h(0) { }
    Rect(double x, double y, double w, double h) : m_x(x), m_y(y), m_w(w), m_h(h) { }
    double m_x, m_y, m_w, m_h;
};

//! Area of a \p viewW x \p viewH view (logical pixels) occupied by a
//! remote image of \p remoteW x \p remoteH scaled to fit while keeping
//! the aspect ratio.  The rest of the view are letterbox bars.
Rect                    fitRect(double viewW, double viewH, SInt32 remoteW, SInt32 remoteH);

//! Map a pointer position inside a session view to remote coordinates.
/*!
\p localX/\p localY are in device pixels as reported by the windowing
system, \p devicePixelRatio converts them to the logical pixels the view
size is expressed in (HiDPI).  Positions on the letterbox bars are
clamped to the nearest edge of the image; the function returns false
when the position lies on a bar so callers can decide to ignore it.
*/
bool                    mapViewToRemote(double viewW, double viewH, double devicePixelRatio,
                            SInt32 remoteX, SInt32 remoteY, SInt32 remoteW, SInt32 remoteH,
                            double localX, double localY,
                            SInt32& outX, SInt32& outY);

} // namespace remote
} // namespace barrier
