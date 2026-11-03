/*
 * Etherwaver -- remote control unit tests: Linux evdev key translation
 * (UK and Polish Programmer layouts) and the local control API parser.
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "client/RemoteControlApi.h"

#include "test/global/gtest.h"

#if defined(__linux__)
#include "platform/LinuxEvdevLocalInputCapture.h"
#include <linux/input.h>
#endif

#include <fstream>
#include <iostream>

TEST(RemoteControlApiTests, parsesRequestLineQueryAndHeaders)
{
    std::string method, path;
    std::map<std::string, std::string> query, headers;
    ASSERT_TRUE(RemoteControlApi::parseRequest(
        "POST /remote-control/start?mode=window&target=rack%20one&takeover=1 HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\nX-Etherwaver-Token: abc\r\n\r\n",
        method, path, query, headers));
    EXPECT_EQ("POST", method);
    EXPECT_EQ("/remote-control/start", path);
    EXPECT_EQ("window", query["mode"]);
    EXPECT_EQ("rack one", query["target"]);
    EXPECT_EQ("1", query["takeover"]);
    EXPECT_EQ("abc", headers["x-etherwaver-token"]);

    EXPECT_FALSE(RemoteControlApi::parseRequest("garbage", method, path, query, headers));
    EXPECT_FALSE(RemoteControlApi::parseRequest("GET nope HTTP/1.1\r\n\r\n", method, path, query, headers));
}

#if defined(__linux__)

TEST(EvdevCaptureTests, ignoresOwnVirtualDevices)
{
    EXPECT_TRUE(LinuxEvdevLocalInputCapture::shouldIgnoreDevice("Etherwaver Keyboard", "Etherwaver"));
    EXPECT_TRUE(LinuxEvdevLocalInputCapture::shouldIgnoreDevice("MyHost Mouse", "MyHost"));
    EXPECT_TRUE(LinuxEvdevLocalInputCapture::shouldIgnoreDevice("Virtual core XTEST keyboard", "Etherwaver"));
    EXPECT_FALSE(LinuxEvdevLocalInputCapture::shouldIgnoreDevice("Logitech USB Receiver", "Etherwaver"));
}

#if HAVE_XKBCOMMON

namespace {

// xkeyboard-config data is needed to compile layouts; without it the
// translator cannot work and the tests are skipped (with a message)
bool
haveLayoutData(const char* layout)
{
    std::ifstream f((std::string("/usr/share/X11/xkb/symbols/") + layout).c_str());
    if (!f.good()) {
        std::cout << "[  SKIPPED ] xkeyboard-config layout \"" << layout << "\" not installed\n";
        return false;
    }
    return true;
}

struct Typed {
    KeyID m_id;
    KeyModifierMask m_mask;
};

// press and release one key, return what the press produced
Typed
tap(EvdevKeyTranslator& t, unsigned int code)
{
    EvdevKeyTranslator::Key k = t.translate(code, 1);
    t.translate(code, 0);
    Typed r = { k.m_id, k.m_mask };
    return r;
}

} // namespace

TEST(EvdevKeyTranslatorTests, ukLayoutSymbols)
{
    if (!haveLayoutData("gb")) {
        return;
    }
    EvdevKeyTranslator t;
    std::string error;
    ASSERT_TRUE(t.init("gb", "", error)) << error;

    EXPECT_EQ((KeyID)'2', tap(t, KEY_2).m_id);
    t.translate(KEY_LEFTSHIFT, 1);
    Typed shift2 = tap(t, KEY_2);
    Typed shift3 = tap(t, KEY_3);
    Typed shiftQuote = tap(t, KEY_APOSTROPHE);
    t.translate(KEY_LEFTSHIFT, 0);
    EXPECT_EQ((KeyID)'"', shift2.m_id) << "UK: Shift+2 is a double quote";
    EXPECT_EQ((KeyModifierMask)KeyModifierShift, shift2.m_mask);
    EXPECT_EQ((KeyID)0xA3, shift3.m_id) << "UK: Shift+3 is the pound sign";
    EXPECT_EQ((KeyID)'@', shiftQuote.m_id) << "UK: Shift+' is @";
    EXPECT_EQ((KeyID)'#', tap(t, KEY_BACKSLASH).m_id) << "UK: key next to Enter";
    EXPECT_EQ((KeyID)'\\', tap(t, KEY_102ND).m_id) << "UK: extra key next to left Shift";
}

TEST(EvdevKeyTranslatorTests, polishProgrammerAltGrLetters)
{
    if (!haveLayoutData("pl")) {
        return;
    }
    EvdevKeyTranslator t;
    std::string error;
    ASSERT_TRUE(t.init("pl", "", error)) << error;

    EXPECT_EQ((KeyID)'a', tap(t, KEY_A).m_id);
    EXPECT_EQ((KeyID)'@', [&]() { t.translate(KEY_LEFTSHIFT, 1); Typed r = tap(t, KEY_2);
                                   t.translate(KEY_LEFTSHIFT, 0); return r.m_id; }())
        << "Polish Programmer keeps the US punctuation";

    // AltGr (right Alt) is ISO_Level3_Shift
    EvdevKeyTranslator::Key altgr = t.translate(KEY_RIGHTALT, 1);
    Typed a = tap(t, KEY_A);
    Typed z = tap(t, KEY_Z);
    Typed x = tap(t, KEY_X);
    t.translate(KEY_RIGHTALT, 0);
    EXPECT_NE(kKeyNone, altgr.m_id);
    EXPECT_EQ((KeyID)0x0105, a.m_id) << "AltGr+a = a with ogonek";
    EXPECT_EQ((KeyModifierMask)KeyModifierAltGr, a.m_mask & KeyModifierAltGr);
    EXPECT_EQ((KeyID)0x017C, z.m_id) << "AltGr+z = z with dot";
    EXPECT_EQ((KeyID)0x017A, x.m_id) << "AltGr+x = z with acute";

    // AltGr+Shift+s = S with acute
    t.translate(KEY_RIGHTALT, 1);
    t.translate(KEY_LEFTSHIFT, 1);
    Typed S = tap(t, KEY_S);
    t.translate(KEY_LEFTSHIFT, 0);
    t.translate(KEY_RIGHTALT, 0);
    EXPECT_EQ((KeyID)0x015A, S.m_id);
}

TEST(EvdevKeyTranslatorTests, modifiersFunctionKeysAndKeypad)
{
    if (!haveLayoutData("gb")) {
        return;
    }
    EvdevKeyTranslator t;
    std::string error;
    ASSERT_TRUE(t.init("gb", "", error)) << error;

    EXPECT_EQ(kKeyControl_L, t.translate(KEY_LEFTCTRL, 1).m_id);
    EvdevKeyTranslator::Key alt = t.translate(KEY_LEFTALT, 1);
    EXPECT_EQ(kKeyAlt_L, alt.m_id);
    EXPECT_EQ((KeyModifierMask)KeyModifierControl, alt.m_mask) << "mask before the key";
    EvdevKeyTranslator::Key del = t.translate(KEY_DELETE, 1);
    EXPECT_EQ(kKeyDelete, del.m_id);
    EXPECT_EQ((KeyModifierMask)(KeyModifierControl | KeyModifierAlt), del.m_mask);
    t.translate(KEY_DELETE, 0);
    t.translate(KEY_LEFTALT, 0);
    t.translate(KEY_LEFTCTRL, 0);

    EXPECT_EQ(kKeySuper_L, tap(t, KEY_LEFTMETA).m_id);
    EXPECT_EQ(kKeyF5, tap(t, KEY_F5).m_id);
    EXPECT_EQ(kKeyF12, tap(t, KEY_F12).m_id);
    EXPECT_EQ(kKeyLeft, tap(t, KEY_LEFT).m_id);
    EXPECT_EQ(kKeyHome, tap(t, KEY_HOME).m_id);
    EXPECT_EQ(kKeyEnd, tap(t, KEY_END).m_id);
    EXPECT_EQ(kKeyPageUp, tap(t, KEY_PAGEUP).m_id);
    EXPECT_EQ(kKeyPageDown, tap(t, KEY_PAGEDOWN).m_id);
    EXPECT_EQ(kKeyInsert, tap(t, KEY_INSERT).m_id);
    EXPECT_EQ(kKeyEscape, tap(t, KEY_ESC).m_id);
    EXPECT_EQ(kKeyTab, tap(t, KEY_TAB).m_id);
    EXPECT_EQ(kKeyReturn, tap(t, KEY_ENTER).m_id);
    EXPECT_EQ(kKeyKP_Enter, tap(t, KEY_KPENTER).m_id);
    EXPECT_EQ(kKeyKP_Add, tap(t, KEY_KPPLUS).m_id);

    // keypad digits depend on Num Lock
    EXPECT_EQ(kKeyKP_Home, tap(t, KEY_KP7).m_id);
    tap(t, KEY_NUMLOCK);
    Typed seven = tap(t, KEY_KP7);
    EXPECT_EQ(kKeyKP_7, seven.m_id);
    EXPECT_NE(0u, seven.m_mask & KeyModifierNumLock);

    // the physical key is reported as evdev code + 8 (X keycode)
    EXPECT_EQ((KeyButton)(KEY_A + 8), t.translate(KEY_A, 1).m_button);
}

TEST(EvdevKeyTranslatorTests, hotkeyMatchesOnBaseSymbol)
{
    if (!haveLayoutData("pl")) {
        return;
    }
    EvdevKeyTranslator t;
    std::string error;
    ASSERT_TRUE(t.init("pl", "", error)) << error;
    t.translate(KEY_LEFTCTRL, 1);
    t.translate(KEY_LEFTALT, 1);
    t.translate(KEY_LEFTSHIFT, 1);
    EvdevKeyTranslator::Key q = t.translate(KEY_Q, 1);
    EXPECT_TRUE(barrier::remote::Hotkey::defaultRelease().matches(q.m_baseId, q.m_mask));
}

#endif // HAVE_XKBCOMMON
#endif // __linux__
