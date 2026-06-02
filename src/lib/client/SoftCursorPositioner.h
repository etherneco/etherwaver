/*
 * barrier -- mouse and keyboard sharing utility
 * Copyright (C) Barrier contributors
 *
 * This package is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * found in the file LICENSE that should have accompanied this file.
 */

#pragma once

#include "common/basic_types.h"

class ICursorPositionProvider {
public:
    virtual ~ICursorPositionProvider() {}
    virtual void getCursorPos(SInt32& x, SInt32& y) = 0;
};

class ICursorMotionSink {
public:
    virtual ~ICursorMotionSink() {}
    virtual void primeAbsolutePosition(SInt32 x, SInt32 y) = 0;
    virtual void mouseMoveAbsolute(SInt32 x, SInt32 y) = 0;
};

class SoftCursorPositioner {
public:
    struct Result {
        Result();

        SInt32 m_currentX;
        SInt32 m_currentY;
        SInt32 m_targetX;
        SInt32 m_targetY;
        SInt32 m_deltaX;
        SInt32 m_deltaY;
    };

    static Result moveTo(ICursorPositionProvider& positionProvider,
                         ICursorMotionSink& motionSink,
                         SInt32 targetX,
                         SInt32 targetY);
};
