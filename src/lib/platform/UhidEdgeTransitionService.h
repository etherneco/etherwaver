/*
 * Etherwaver -- UHID-relative edge transition service
 *
 * This service tracks relative mouse motion reported into UHID, maintains a
 * compositor-independent virtual cursor, and emits pre-edge transition intent
 * events before a Wayland compositor clamps the real cursor at the screen edge.
 */

#pragma once

#include "platform/UhidServer.h"

#include "base/Stopwatch.h"

class IUhidEdgeTransitionHandler {
public:
    enum Direction {
        kLeft,
        kRight,
        kTop,
        kBottom
    };

    virtual ~IUhidEdgeTransitionHandler() {}
    virtual void onTransition(Direction direction) = 0;
};

class UhidEdgeTransitionService : public UhidServer::MouseMotionListener {
public:
    struct ScreenGeometry {
        SInt32 m_x;
        SInt32 m_y;
        SInt32 m_width;
        SInt32 m_height;

        ScreenGeometry()
            : m_x(0)
            , m_y(0)
            , m_width(0)
            , m_height(0)
        {
        }

        ScreenGeometry(SInt32 x, SInt32 y, SInt32 width, SInt32 height)
            : m_x(x)
            , m_y(y)
            , m_width(width)
            , m_height(height)
        {
        }
    };

    struct Config {
        SInt32 m_leftThreshold;
        SInt32 m_rightThreshold;
        SInt32 m_topThreshold;
        SInt32 m_bottomThreshold;
        SInt32 m_virtualMin;
        SInt32 m_virtualMax;
        SInt32 m_warpOffset;
        UInt32 m_cooldownMs;
        int m_requiredConsecutiveEvents;
        bool m_enableTopBottom;
        bool m_enableVirtualWarp;
        bool m_debugLogging;

        Config();
    };

public:
    explicit UhidEdgeTransitionService(const Config& config = Config());

    void setTransitionHandler(IUhidEdgeTransitionHandler* handler);
    void setScreenGeometry(SInt32 x, SInt32 y, SInt32 width, SInt32 height);
    void setScreenGeometry(const ScreenGeometry& geometry);

    void resetVirtualCursor();
    void setVirtualCursor(SInt32 x, SInt32 y);

    void clearSystemCursorSample();
    void updateSystemCursorSample(SInt32 x, SInt32 y);

    SInt32 virtualX() const;
    SInt32 virtualY() const;
    const ScreenGeometry& screenGeometry() const;

    void onRelativeMouseMotion(SInt32 dx, SInt32 dy) override;

private:
    struct DirectionState {
        DirectionState()
            : m_consecutive(0)
        {
        }

        int m_consecutive;
    };

private:
    bool hasUsableGeometry() const;
    bool cooldownActive();
    void applyVirtualMotion(SInt32 dx, SInt32 dy);
    void updateDirectionState(DirectionState& state, bool matches);
    bool systemCursorLooksStuck(SInt32 dx, SInt32 dy) const;
    void maybeTrigger(IUhidEdgeTransitionHandler::Direction direction);
    void applyPostTransitionWarp(IUhidEdgeTransitionHandler::Direction direction);
    void logMotion(SInt32 dx, SInt32 dy, bool stuck) const;
    const char* directionName(IUhidEdgeTransitionHandler::Direction direction) const;
    SInt32 clampVirtual(SInt32 value) const;

private:
    Config m_config;
    ScreenGeometry m_geometry;
    SInt32 m_virtualX;
    SInt32 m_virtualY;
    bool m_virtualInitialized;
    IUhidEdgeTransitionHandler* m_handler;
    Stopwatch m_cooldownTimer;
    bool m_cooldownStarted;

    DirectionState m_leftState;
    DirectionState m_rightState;
    DirectionState m_topState;
    DirectionState m_bottomState;

    bool m_hasSystemCursorSample;
    bool m_hasPreviousSystemCursorSample;
    SInt32 m_systemCursorX;
    SInt32 m_systemCursorY;
    SInt32 m_previousSystemCursorX;
    SInt32 m_previousSystemCursorY;
};
