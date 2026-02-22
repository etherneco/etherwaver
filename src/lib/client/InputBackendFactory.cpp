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
#include "barrier/ClientArgs.h"
#include "barrier/Screen.h"
#include "base/Log.h"
#include "platform/UhidServer.h"

#include <cassert>

namespace {

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

class UhidInputBackend : public IInputBackend {
public:
    explicit UhidInputBackend(const String& deviceName)
        : m_started(false)
        , m_uhidServer(new UhidServer())
    {
        m_started = m_uhidServer->start(deviceName);
    }

    bool started() const
    {
        return m_started;
    }

    void enter(SInt32 xAbs, SInt32 yAbs) override
    {
        m_uhidServer->clearInputState();
        m_uhidServer->mouseMoveAbsolute(xAbs, yAbs);
    }

    void leave() override
    {
        m_uhidServer->clearInputState();
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
        m_uhidServer->mouseMoveAbsolute(xAbs, yAbs);
    }

    void mouseRelativeMove(SInt32 dx, SInt32 dy) override
    {
        m_uhidServer->mouseRelativeMove(dx, dy);
    }

    void mouseWheel(SInt32 xDelta, SInt32 yDelta) override
    {
        m_uhidServer->mouseWheel(xDelta, yDelta);
    }

private:
    bool m_started;
    std::unique_ptr<UhidServer> m_uhidServer;
};

} // namespace

std::unique_ptr<IInputBackend> createInputBackend(barrier::Screen* screen, const ClientArgs& args)
{
    if (!args.m_uhidEnabled) {
        return std::unique_ptr<IInputBackend>(new ScreenInputBackend(screen));
    }

    std::unique_ptr<UhidInputBackend> uhidBackend(new UhidInputBackend(args.m_uhidName));
    if (uhidBackend->started()) {
        LOG((CLOG_NOTE "uhid: using backend"));
        return std::move(uhidBackend);
    }

    LOG((CLOG_WARN "uhid: failed to start, falling back to screen backend"));
    return std::unique_ptr<IInputBackend>(new ScreenInputBackend(screen));
}
