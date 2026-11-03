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

#include "barrier/RemoteInput.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace barrier {
namespace remote {

const char*
controlStateName(int state)
{
    switch (state) {
    case kStateIdle:              return "idle";
    case kStateGranted:           return "granted";
    case kStateControlledByOther: return "controlled-by-other";
    case kStateDenied:            return "denied";
    case kStateRevoked:           return "revoked";
    case kStateReleased:          return "released";
    default:                      return "unknown";
    }
}

const char*
controlReasonText(int reason)
{
    switch (reason) {
    case kReasonNone:           return "";
    case kReasonNotPermitted:   return "this computer is allowed to view only (remoteControl is disabled for it on the server)";
    case kReasonBusy:           return "another computer is controlling the host (ask for a takeover; the server may forbid it)";
    case kReasonTakenOver:      return "another computer took over control";
    case kReasonTimeout:        return "the control lease timed out";
    case kReasonUnknownTarget:  return "the requested host is not connected";
    case kReasonTargetIsSelf:   return "a computer cannot control itself";
    case kReasonTargetChanged:  return "the controlled host changed";
    case kReasonServerShutdown: return "the server is shutting down";
    case kReasonQueueOverflow:  return "the input queue overflowed";
    case kReasonProtocol:       return "protocol error";
    default:                    return "unknown reason";
    }
}

bool
Event::isStateChange() const
{
    switch (m_kind) {
    case kEventKeyDown:
    case kEventKeyUp:
    case kEventButtonDown:
    case kEventButtonUp:
        return true;
    default:
        return false;
    }
}

bool
isValidEvent(int kind, SInt32 a, SInt32 b, SInt32 c, SInt32 d)
{
    switch (kind) {
    case kEventKeyDown:
    case kEventKeyUp:
        return a != (SInt32)kKeyNone && c >= 0 && c <= 0xffff;
    case kEventKeyRepeat:
        return a != (SInt32)kKeyNone && c >= 0 && c <= 0xffff && d > 0 && d <= 1000;
    case kEventButtonDown:
    case kEventButtonUp:
        return a > (SInt32)kButtonNone && a <= 32;
    case kEventMoveAbs:
        return a > -1000000 && a < 1000000 && b > -1000000 && b < 1000000;
    case kEventMoveRel:
        return a > -100000 && a < 100000 && b > -100000 && b < 100000;
    case kEventWheel:
        return a > -100000 && a < 100000 && b > -100000 && b < 100000;
    case kEventHeartbeat:
        return true;
    default:
        (void)d;
        return false;
    }
}

bool
isModifierKey(KeyID id)
{
    if (id >= kKeyShift_L && id <= kKeyHyper_R) {
        return true;
    }
    // AltGr / ISO_Level3_Shift (0xfe03 is mapped by barrier to 0xEE03)
    return id == kKeyAltGr || id == 0xEE03 || id == 0xEE11;
}

//
// HeldInputState
//

bool
HeldInputState::pressKey(KeyID id, KeyModifierMask mask, KeyButton button)
{
    if (m_keys.find(button) != m_keys.end()) {
        return false;
    }
    HeldKey key;
    key.m_id     = id;
    key.m_mask   = mask;
    key.m_button = button;
    key.m_order  = ++m_counter;
    m_keys[button] = key;
    return true;
}

bool
HeldInputState::releaseKey(KeyButton button)
{
    return m_keys.erase(button) != 0;
}

bool
HeldInputState::isKeyDown(KeyButton button) const
{
    return m_keys.find(button) != m_keys.end();
}

bool
HeldInputState::pressButton(ButtonID id)
{
    if (m_buttons.find(id) != m_buttons.end()) {
        return false;
    }
    m_buttons[id] = ++m_counter;
    return true;
}

bool
HeldInputState::releaseButton(ButtonID id)
{
    return m_buttons.erase(id) != 0;
}

bool
HeldInputState::isButtonDown(ButtonID id) const
{
    return m_buttons.find(id) != m_buttons.end();
}

std::vector<Event>
HeldInputState::releaseAll()
{
    std::vector<Event> result;

    // mouse buttons first so a drag ends before modifiers change
    std::vector<std::pair<UInt32, ButtonID> > buttons;
    for (std::map<ButtonID, UInt32>::const_iterator i = m_buttons.begin();
         i != m_buttons.end(); ++i) {
        buttons.push_back(std::make_pair(i->second, i->first));
    }
    std::sort(buttons.rbegin(), buttons.rend());
    for (size_t i = 0; i < buttons.size(); ++i) {
        result.push_back(Event::buttonUp(buttons[i].second));
    }

    // then ordinary keys, then modifiers (newest first in each group)
    std::vector<HeldKey> keys;
    for (std::map<KeyButton, HeldKey>::const_iterator i = m_keys.begin();
         i != m_keys.end(); ++i) {
        keys.push_back(i->second);
    }
    std::sort(keys.begin(), keys.end(), [](const HeldKey& a, const HeldKey& b) {
        const bool am = isModifierKey(a.m_id);
        const bool bm = isModifierKey(b.m_id);
        if (am != bm) {
            return !am;
        }
        return a.m_order > b.m_order;
    });
    for (size_t i = 0; i < keys.size(); ++i) {
        result.push_back(Event::keyUp(keys[i].m_id, keys[i].m_mask, keys[i].m_button));
    }

    clear();
    return result;
}

void
HeldInputState::clear()
{
    m_keys.clear();
    m_buttons.clear();
}

//
// InputQueue
//

InputQueue::InputQueue(size_t capacity) :
    m_capacity(std::max<size_t>(capacity, 4))
{
}

InputQueue::PushResult
InputQueue::push(const Event& event)
{
    // merge with the newest queued event when that cannot reorder
    // state changes: only adjacent events of the same motion kind merge.
    if (!m_events.empty()) {
        Event& last = m_events.back();
        if (event.m_kind == kEventMoveRel && last.m_kind == kEventMoveRel) {
            last.m_a += event.m_a;
            last.m_b += event.m_b;
            ++m_coalesced;
            return kCoalesced;
        }
        if (event.m_kind == kEventMoveAbs && last.m_kind == kEventMoveAbs) {
            last.m_a = event.m_a;
            last.m_b = event.m_b;
            ++m_coalesced;
            return kCoalesced;
        }
        if (event.m_kind == kEventWheel && last.m_kind == kEventWheel) {
            last.m_a += event.m_a;
            last.m_b += event.m_b;
            ++m_coalesced;
            return kCoalesced;
        }
        if (event.m_kind == kEventHeartbeat && last.m_kind == kEventHeartbeat) {
            ++m_coalesced;
            return kCoalesced;
        }
    }

    PushResult result = kQueued;
    if (m_events.size() >= m_capacity) {
        // drop the oldest droppable event (motion, heartbeat).  dropping
        // a relative motion loses some distance but never a press or
        // release; an absolute motion is superseded by later ones.
        std::deque<Event>::iterator i = m_events.begin();
        for (; i != m_events.end(); ++i) {
            if (i->isMotion() || i->m_kind == kEventHeartbeat) {
                break;
            }
        }
        if (i == m_events.end()) {
            if (event.isMotion() || event.m_kind == kEventHeartbeat) {
                ++m_dropped;
                return kDroppedMotion;
            }
            return kOverflow;
        }
        m_events.erase(i);
        ++m_dropped;
        result = kDroppedMotion;
    }
    m_events.push_back(event);
    return result;
}

bool
InputQueue::pop(Event& event)
{
    if (m_events.empty()) {
        return false;
    }
    event = m_events.front();
    m_events.pop_front();
    return true;
}

//
// Hotkey
//

KeyModifierMask
hotkeyModifiers(KeyModifierMask mask)
{
    return mask & (KeyModifierShift | KeyModifierControl | KeyModifierAlt |
                   KeyModifierMeta | KeyModifierSuper | KeyModifierAltGr);
}

KeyID
foldKeyCase(KeyID id)
{
    if (id >= 'A' && id <= 'Z') {
        return id - 'A' + 'a';
    }
    // Latin-1 upper case letters (except multiplication sign)
    if (id >= 0xC0 && id <= 0xDE && id != 0xD7) {
        return id + 0x20;
    }
    return id;
}

namespace {

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
trim(const std::string& s)
{
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) {
        return std::string();
    }
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

bool
parseModifier(const std::string& token, KeyModifierMask& mask)
{
    const std::string t = lowerAscii(token);
    if (t == "ctrl" || t == "control") {
        mask = KeyModifierControl;
    }
    else if (t == "alt" || t == "option" || t == "opt") {
        mask = KeyModifierAlt;
    }
    else if (t == "shift") {
        mask = KeyModifierShift;
    }
    else if (t == "super" || t == "win" || t == "windows" ||
             t == "cmd" || t == "command") {
        mask = KeyModifierSuper;
    }
    else if (t == "meta") {
        mask = KeyModifierMeta;
    }
    else if (t == "altgr") {
        mask = KeyModifierAltGr;
    }
    else {
        return false;
    }
    return true;
}

} // namespace

bool
Hotkey::parse(const std::string& text, Hotkey& out, std::string* error)
{
    std::vector<std::string> tokens;
    std::string current;
    for (size_t i = 0; i < text.size(); ++i) {
        // a trailing "+" means the plus key ("Ctrl+Alt++")
        if (text[i] == '+' && !current.empty()) {
            tokens.push_back(trim(current));
            current.clear();
        }
        else {
            current += text[i];
        }
    }
    if (!trim(current).empty()) {
        tokens.push_back(trim(current));
    }
    if (tokens.empty()) {
        if (error) *error = "empty hotkey";
        return false;
    }

    KeyModifierMask mask = 0;
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        KeyModifierMask m;
        if (!parseModifier(tokens[i], m)) {
            if (error) *error = "unknown modifier \"" + tokens[i] + "\"";
            return false;
        }
        mask |= m;
    }

    const std::string& keyName = tokens.back();
    KeyID key = kKeyNone;
    if (keyName.size() == 1) {
        key = foldKeyCase(static_cast<unsigned char>(keyName[0]));
    }
    else {
        const std::string lower = lowerAscii(keyName);
        if (lower == "esc") {
            key = kKeyEscape;
        }
        for (const KeyNameMapEntry* e = kKeyNameMap; key == kKeyNone && e->m_name != NULL; ++e) {
            if (lowerAscii(e->m_name) == lower) {
                key = e->m_id;
            }
        }
    }
    if (key == kKeyNone) {
        if (error) *error = "unknown key \"" + keyName + "\"";
        return false;
    }
    if (mask == 0) {
        if (error) *error = "the hotkey needs at least one modifier";
        return false;
    }
    out = Hotkey(key, mask);
    return true;
}

Hotkey
Hotkey::defaultRelease()
{
    return Hotkey('q', KeyModifierControl | KeyModifierAlt | KeyModifierShift);
}

std::string
Hotkey::toString() const
{
    std::string s;
    if (m_mask & KeyModifierControl) s += "Ctrl+";
    if (m_mask & KeyModifierAlt)     s += "Alt+";
    if (m_mask & KeyModifierAltGr)   s += "AltGr+";
    if (m_mask & KeyModifierShift)   s += "Shift+";
    if (m_mask & KeyModifierSuper)   s += "Super+";
    if (m_mask & KeyModifierMeta)    s += "Meta+";
    if (m_key >= 0x21 && m_key < 0x7f) {
        s += static_cast<char>(std::toupper(static_cast<int>(m_key)));
        return s;
    }
    for (const KeyNameMapEntry* e = kKeyNameMap; e->m_name != NULL; ++e) {
        if (e->m_id == m_key) {
            return s + e->m_name;
        }
    }
    return s + "?";
}

bool
Hotkey::matches(KeyID key, KeyModifierMask mask) const
{
    if (!isValid()) {
        return false;
    }
    return foldKeyCase(key) == foldKeyCase(m_key) && hotkeyModifiers(mask) == m_mask;
}

//
// coordinate mapping
//

Rect
fitRect(double viewW, double viewH, SInt32 remoteW, SInt32 remoteH)
{
    if (viewW <= 0 || viewH <= 0 || remoteW <= 0 || remoteH <= 0) {
        return Rect();
    }
    const double scale = std::min(viewW / remoteW, viewH / remoteH);
    const double w = remoteW * scale;
    const double h = remoteH * scale;
    return Rect((viewW - w) / 2.0, (viewH - h) / 2.0, w, h);
}

bool
mapViewToRemote(double viewW, double viewH, double devicePixelRatio,
                SInt32 remoteX, SInt32 remoteY, SInt32 remoteW, SInt32 remoteH,
                double localX, double localY,
                SInt32& outX, SInt32& outY)
{
    if (devicePixelRatio <= 0) {
        devicePixelRatio = 1.0;
    }
    const Rect image = fitRect(viewW, viewH, remoteW, remoteH);
    if (image.m_w <= 0 || image.m_h <= 0) {
        outX = remoteX;
        outY = remoteY;
        return false;
    }

    // device pixels -> logical pixels
    const double lx = localX / devicePixelRatio;
    const double ly = localY / devicePixelRatio;

    const bool inside = lx >= image.m_x && lx < image.m_x + image.m_w &&
                        ly >= image.m_y && ly < image.m_y + image.m_h;

    double fx = (lx - image.m_x) / image.m_w;
    double fy = (ly - image.m_y) / image.m_h;
    fx = std::max(0.0, std::min(fx, 1.0));
    fy = std::max(0.0, std::min(fy, 1.0));

    SInt32 x = remoteX + static_cast<SInt32>(std::floor(fx * remoteW));
    SInt32 y = remoteY + static_cast<SInt32>(std::floor(fy * remoteH));
    outX = std::min(x, remoteX + remoteW - 1);
    outY = std::min(y, remoteY + remoteH - 1);
    return inside;
}

} // namespace remote
} // namespace barrier
