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

#pragma once

#include "barrier/key_types.h"
#include "barrier/mouse_types.h"

class IInputBackend {
public:
    virtual ~IInputBackend() {}

    virtual void enter(SInt32 xAbs, SInt32 yAbs) = 0;
    virtual void leave() = 0;

    virtual void keyDown(KeyID id, KeyModifierMask mask, KeyButton button) = 0;
    virtual void keyRepeat(KeyID id, KeyModifierMask mask, SInt32 count, KeyButton button) = 0;
    virtual void keyUp(KeyID id, KeyModifierMask mask, KeyButton button) = 0;

    virtual void mouseDown(ButtonID id) = 0;
    virtual void mouseUp(ButtonID id) = 0;
    virtual void mouseMove(SInt32 xAbs, SInt32 yAbs) = 0;
    virtual void mouseRelativeMove(SInt32 dx, SInt32 dy) = 0;
    virtual void mouseWheel(SInt32 xDelta, SInt32 yDelta) = 0;
};

