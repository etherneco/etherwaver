/*
 * Etherwaver -- remote control unit tests: shared building blocks.
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#include "barrier/RemoteInput.h"

#include "test/global/gtest.h"

using namespace barrier::remote;
typedef barrier::remote::Event RE;

TEST(RemoteInputTests, heldStateTracksPressAndRelease)
{
    HeldInputState held;
    EXPECT_TRUE(held.pressKey('a', 0, 38));
    EXPECT_FALSE(held.pressKey('a', 0, 38)) << "second press is an autorepeat";
    EXPECT_TRUE(held.isKeyDown(38));
    EXPECT_TRUE(held.releaseKey(38));
    EXPECT_FALSE(held.releaseKey(38)) << "unmatched release";
    EXPECT_TRUE(held.pressButton(kButtonLeft));
    EXPECT_FALSE(held.pressButton(kButtonLeft));
    EXPECT_TRUE(held.anyButtonDown());
    EXPECT_TRUE(held.releaseButton(kButtonLeft));
    EXPECT_TRUE(held.empty());
}

TEST(RemoteInputTests, releaseAllReleasesButtonsThenKeysThenModifiers)
{
    HeldInputState held;
    held.pressKey(kKeyControl_L, 0, 37);
    held.pressKey(kKeyAlt_L, KeyModifierControl, 64);
    held.pressKey(kKeyShift_L, KeyModifierControl | KeyModifierAlt, 50);
    held.pressKey('c', KeyModifierControl | KeyModifierAlt | KeyModifierShift, 54);
    held.pressButton(kButtonLeft);
    held.pressButton(kButtonRight);

    std::vector<RE> r = held.releaseAll();
    ASSERT_EQ(6u, r.size());
    EXPECT_EQ(RE::buttonUp(kButtonRight), r[0]);
    EXPECT_EQ(RE::buttonUp(kButtonLeft), r[1]);
    EXPECT_EQ(kEventKeyUp, r[2].m_kind);
    EXPECT_EQ(54, r[2].m_c) << "ordinary key before modifiers";
    EXPECT_EQ(50, r[3].m_c) << "then modifiers, newest first";
    EXPECT_EQ(64, r[4].m_c);
    EXPECT_EQ(37, r[5].m_c);
    EXPECT_TRUE(held.empty());
    EXPECT_TRUE(held.releaseAll().empty());
}

TEST(RemoteInputTests, queueCoalescesMotionButKeepsStateChangesInOrder)
{
    InputQueue q(16);
    EXPECT_EQ(InputQueue::kQueued, q.push(RE::moveRel(1, 2)));
    EXPECT_EQ(InputQueue::kCoalesced, q.push(RE::moveRel(3, 4)));
    EXPECT_EQ(InputQueue::kQueued, q.push(RE::buttonDown(kButtonLeft)));
    EXPECT_EQ(InputQueue::kQueued, q.push(RE::moveRel(5, 5)));
    EXPECT_EQ(InputQueue::kCoalesced, q.push(RE::moveRel(1, 1)));
    EXPECT_EQ(InputQueue::kQueued, q.push(RE::buttonUp(kButtonLeft)));
    EXPECT_EQ(InputQueue::kQueued, q.push(RE::wheel(0, 120)));
    EXPECT_EQ(InputQueue::kCoalesced, q.push(RE::wheel(0, 120)));

    RE e;
    ASSERT_TRUE(q.pop(e)); EXPECT_EQ(RE::moveRel(4, 6), e);
    ASSERT_TRUE(q.pop(e)); EXPECT_EQ(RE::buttonDown(kButtonLeft), e);
    ASSERT_TRUE(q.pop(e)); EXPECT_EQ(RE::moveRel(6, 6), e) << "drag motion between press and release";
    ASSERT_TRUE(q.pop(e)); EXPECT_EQ(RE::buttonUp(kButtonLeft), e);
    ASSERT_TRUE(q.pop(e)); EXPECT_EQ(RE::wheel(0, 240), e);
    EXPECT_FALSE(q.pop(e));
}

TEST(RemoteInputTests, queueDropsMotionNotPressesWhenFull)
{
    InputQueue q(4);
    q.push(RE::moveAbs(1, 1));
    q.push(RE::keyDown('a', 0, 38));
    q.push(RE::moveRel(1, 1));
    q.push(RE::keyUp('a', 0, 38));
    EXPECT_EQ(4u, q.size());
    // full: the oldest motion goes, the key event stays
    EXPECT_EQ(InputQueue::kDroppedMotion, q.push(RE::keyDown('b', 0, 56)));
    EXPECT_EQ(4u, q.size());
    EXPECT_EQ(InputQueue::kDroppedMotion, q.push(RE::keyUp('b', 0, 56)));
    // only state changes left: motion is dropped, state change overflows
    EXPECT_EQ(InputQueue::kDroppedMotion, q.push(RE::moveAbs(5, 5)));
    EXPECT_EQ(InputQueue::kOverflow, q.push(RE::keyDown('c', 0, 54)));

    RE e;
    int downs = 0, ups = 0;
    while (q.pop(e)) {
        downs += e.m_kind == kEventKeyDown;
        ups   += e.m_kind == kEventKeyUp;
    }
    EXPECT_EQ(2, downs);
    EXPECT_EQ(2, ups);
}

TEST(RemoteInputTests, hotkeyParseAndMatch)
{
    Hotkey h;
    ASSERT_TRUE(Hotkey::parse("Ctrl+Alt+Shift+Q", h));
    EXPECT_EQ("Ctrl+Alt+Shift+Q", h.toString());
    EXPECT_TRUE(h.matches('q', KeyModifierControl | KeyModifierAlt | KeyModifierShift));
    EXPECT_TRUE(h.matches('Q', KeyModifierControl | KeyModifierAlt | KeyModifierShift));
    EXPECT_TRUE(h.matches('q', KeyModifierControl | KeyModifierAlt | KeyModifierShift |
                               KeyModifierNumLock | KeyModifierCapsLock))
        << "lock keys are ignored";
    EXPECT_FALSE(h.matches('q', KeyModifierControl | KeyModifierAlt));
    EXPECT_FALSE(h.matches('q', KeyModifierControl | KeyModifierAlt | KeyModifierShift |
                                KeyModifierSuper));
    EXPECT_FALSE(h.matches('w', KeyModifierControl | KeyModifierAlt | KeyModifierShift));

    ASSERT_TRUE(Hotkey::parse("control + super + Escape", h));
    EXPECT_TRUE(h.matches(kKeyEscape, KeyModifierControl | KeyModifierSuper));
    ASSERT_TRUE(Hotkey::parse("Cmd+Opt+F12", h));
    EXPECT_TRUE(h.matches(kKeyF12, KeyModifierSuper | KeyModifierAlt));

    std::string error;
    EXPECT_FALSE(Hotkey::parse("Q", h, &error)) << "a modifier is required";
    EXPECT_FALSE(Hotkey::parse("Ctrl+Bogus", h, &error));
    EXPECT_FALSE(Hotkey::parse("Hyperx+Q", h, &error));
    EXPECT_FALSE(Hotkey::parse("", h, &error));

    EXPECT_EQ("Ctrl+Alt+Shift+Q", Hotkey::defaultRelease().toString());
}

TEST(RemoteInputTests, viewMappingHandlesLetterboxAndHiDpi)
{
    // 1920x1080 remote in a 1000x1000 view: image 1000x562.5 at y=218.75
    SInt32 x, y;
    EXPECT_TRUE(mapViewToRemote(1000, 1000, 1.0, 0, 0, 1920, 1080, 0, 218.75, x, y));
    EXPECT_EQ(0, x);
    EXPECT_EQ(0, y);
    EXPECT_TRUE(mapViewToRemote(1000, 1000, 1.0, 0, 0, 1920, 1080, 500, 500, x, y));
    EXPECT_EQ(960, x);
    EXPECT_EQ(540, y);
    EXPECT_TRUE(mapViewToRemote(1000, 1000, 1.0, 0, 0, 1920, 1080, 999.9, 781.2, x, y));
    EXPECT_EQ(1919, x);
    EXPECT_EQ(1079, y);

    // on the bars: clamped to the image edge, reported as outside
    EXPECT_FALSE(mapViewToRemote(1000, 1000, 1.0, 0, 0, 1920, 1080, 500, 10, x, y));
    EXPECT_EQ(960, x);
    EXPECT_EQ(0, y);
    EXPECT_FALSE(mapViewToRemote(1000, 1000, 1.0, 0, 0, 1920, 1080, 500, 990, x, y));
    EXPECT_EQ(1079, y);

    // HiDPI: 2x device pixels in a 500x500 logical view, pillarbox
    EXPECT_TRUE(mapViewToRemote(500, 500, 2.0, 0, 0, 1000, 2000, 500, 500, x, y));
    EXPECT_EQ(500, x);
    EXPECT_EQ(1000, y);
    EXPECT_FALSE(mapViewToRemote(500, 500, 2.0, 0, 0, 1000, 2000, 50, 500, x, y))
        << "left pillar bar";
    EXPECT_EQ(0, x);

    // remote origin offset (multi-monitor host)
    EXPECT_TRUE(mapViewToRemote(1920, 1080, 1.0, -1920, 0, 1920, 1080, 0, 0, x, y));
    EXPECT_EQ(-1920, x);
}

TEST(RemoteInputTests, validatesWireEvents)
{
    EXPECT_TRUE(isValidEvent(kEventKeyDown, 'a', 0, 38, 0));
    EXPECT_FALSE(isValidEvent(kEventKeyDown, kKeyNone, 0, 38, 0));
    EXPECT_FALSE(isValidEvent(kEventKeyRepeat, 'a', 0, 38, 0)) << "repeat count must be positive";
    EXPECT_TRUE(isValidEvent(kEventButtonDown, kButtonLeft, 0, 0, 0));
    EXPECT_FALSE(isValidEvent(kEventButtonDown, 0, 0, 0, 0));
    EXPECT_FALSE(isValidEvent(kEventButtonDown, 99, 0, 0, 0));
    EXPECT_FALSE(isValidEvent(0, 0, 0, 0, 0));
    EXPECT_FALSE(isValidEvent(42, 0, 0, 0, 0));
    EXPECT_TRUE(isValidEvent(kEventHeartbeat, 0, 0, 0, 0));
}
