#include "platform/UhidEdgeTransitionService.h"

#include "base/Log.h"

#include <algorithm>

UhidEdgeTransitionService::Config::Config()
    : m_leftThreshold(40)
    , m_rightThreshold(40)
    , m_topThreshold(40)
    , m_bottomThreshold(40)
    , m_virtualMin(-10000)
    , m_virtualMax(10000)
    , m_warpOffset(40)
    , m_cooldownMs(300)
    , m_requiredConsecutiveEvents(4)
    , m_enableTopBottom(true)
    , m_enableVirtualWarp(true)
    , m_debugLogging(false)
{
}

UhidEdgeTransitionService::UhidEdgeTransitionService(const Config& config)
    : m_config(config)
    , m_virtualX(0)
    , m_virtualY(0)
    , m_virtualInitialized(false)
    , m_handler(NULL)
    , m_cooldownTimer(true)
    , m_cooldownStarted(false)
    , m_hasSystemCursorSample(false)
    , m_hasPreviousSystemCursorSample(false)
    , m_systemCursorX(0)
    , m_systemCursorY(0)
    , m_previousSystemCursorX(0)
    , m_previousSystemCursorY(0)
{
}

void
UhidEdgeTransitionService::setTransitionHandler(IUhidEdgeTransitionHandler* handler)
{
    m_handler = handler;
}

void
UhidEdgeTransitionService::setScreenGeometry(SInt32 x, SInt32 y, SInt32 width, SInt32 height)
{
    setScreenGeometry(ScreenGeometry(x, y, width, height));
}

void
UhidEdgeTransitionService::setScreenGeometry(const ScreenGeometry& geometry)
{
    m_geometry = geometry;
    if (!m_virtualInitialized && hasUsableGeometry()) {
        resetVirtualCursor();
    }
}

void
UhidEdgeTransitionService::resetVirtualCursor()
{
    if (!hasUsableGeometry()) {
        m_virtualX = 0;
        m_virtualY = 0;
        m_virtualInitialized = false;
        return;
    }

    m_virtualX = m_geometry.m_width / 2;
    m_virtualY = m_geometry.m_height / 2;
    m_virtualInitialized = true;
}

void
UhidEdgeTransitionService::setVirtualCursor(SInt32 x, SInt32 y)
{
    m_virtualX = clampVirtual(x);
    m_virtualY = clampVirtual(y);
    m_virtualInitialized = true;
}

void
UhidEdgeTransitionService::clearSystemCursorSample()
{
    m_hasSystemCursorSample = false;
    m_hasPreviousSystemCursorSample = false;
}

void
UhidEdgeTransitionService::updateSystemCursorSample(SInt32 x, SInt32 y)
{
    if (m_hasSystemCursorSample) {
        m_previousSystemCursorX = m_systemCursorX;
        m_previousSystemCursorY = m_systemCursorY;
        m_hasPreviousSystemCursorSample = true;
    }

    m_systemCursorX = x;
    m_systemCursorY = y;
    m_hasSystemCursorSample = true;
}

SInt32
UhidEdgeTransitionService::virtualX() const
{
    return m_virtualX;
}

SInt32
UhidEdgeTransitionService::virtualY() const
{
    return m_virtualY;
}

const UhidEdgeTransitionService::ScreenGeometry&
UhidEdgeTransitionService::screenGeometry() const
{
    return m_geometry;
}

void
UhidEdgeTransitionService::onRelativeMouseMotion(SInt32 dx, SInt32 dy)
{
    if (!hasUsableGeometry() || (dx == 0 && dy == 0)) {
        return;
    }

    if (!m_virtualInitialized) {
        resetVirtualCursor();
    }

    applyVirtualMotion(dx, dy);

    const bool stuck = systemCursorLooksStuck(dx, dy);
    if (m_config.m_debugLogging) {
        logMotion(dx, dy, stuck);
    }

    const bool leftIntent =
        (dx < 0 && m_virtualX <= m_config.m_leftThreshold);
    const bool rightIntent =
        (dx > 0 && m_virtualX >= m_geometry.m_width - 1 - m_config.m_rightThreshold);
    const bool topIntent =
        (m_config.m_enableTopBottom && dy < 0 &&
         m_virtualY <= m_config.m_topThreshold);
    const bool bottomIntent =
        (m_config.m_enableTopBottom && dy > 0 &&
         m_virtualY >= m_geometry.m_height - 1 - m_config.m_bottomThreshold);

    updateDirectionState(m_leftState, leftIntent);
    updateDirectionState(m_rightState, rightIntent);
    updateDirectionState(m_topState, topIntent);
    updateDirectionState(m_bottomState, bottomIntent);

    if (m_leftState.m_consecutive >= m_config.m_requiredConsecutiveEvents) {
        maybeTrigger(IUhidEdgeTransitionHandler::kLeft);
        return;
    }
    if (m_rightState.m_consecutive >= m_config.m_requiredConsecutiveEvents) {
        maybeTrigger(IUhidEdgeTransitionHandler::kRight);
        return;
    }
    if (m_topState.m_consecutive >= m_config.m_requiredConsecutiveEvents) {
        maybeTrigger(IUhidEdgeTransitionHandler::kTop);
        return;
    }
    if (m_bottomState.m_consecutive >= m_config.m_requiredConsecutiveEvents) {
        maybeTrigger(IUhidEdgeTransitionHandler::kBottom);
        return;
    }
}

bool
UhidEdgeTransitionService::hasUsableGeometry() const
{
    return (m_geometry.m_width > 0 && m_geometry.m_height > 0);
}

bool
UhidEdgeTransitionService::cooldownActive()
{
    if (!m_cooldownStarted) {
        return false;
    }

    return (m_cooldownTimer.getTime() * 1000.0) < static_cast<double>(m_config.m_cooldownMs);
}

void
UhidEdgeTransitionService::applyVirtualMotion(SInt32 dx, SInt32 dy)
{
    m_virtualX = clampVirtual(m_virtualX + dx);
    m_virtualY = clampVirtual(m_virtualY + dy);
}

void
UhidEdgeTransitionService::updateDirectionState(DirectionState& state, bool matches)
{
    if (matches) {
        ++state.m_consecutive;
    }
    else {
        state.m_consecutive = 0;
    }
}

bool
UhidEdgeTransitionService::systemCursorLooksStuck(SInt32 dx, SInt32 dy) const
{
    if (!m_hasSystemCursorSample || !m_hasPreviousSystemCursorSample) {
        return false;
    }

    if (dx == 0 && dy == 0) {
        return false;
    }

    return (m_systemCursorX == m_previousSystemCursorX &&
            m_systemCursorY == m_previousSystemCursorY);
}

void
UhidEdgeTransitionService::maybeTrigger(IUhidEdgeTransitionHandler::Direction direction)
{
    if (cooldownActive()) {
        return;
    }

    if (m_config.m_debugLogging) {
        LOG((CLOG_INFO
            "uhid-edge trigger direction=%s virtual=%d,%d geometry=%d,%d %dx%d",
            directionName(direction),
            m_virtualX,
            m_virtualY,
            m_geometry.m_x,
            m_geometry.m_y,
            m_geometry.m_width,
            m_geometry.m_height));
    }

    m_cooldownTimer.reset();
    m_cooldownStarted = true;
    m_leftState.m_consecutive = 0;
    m_rightState.m_consecutive = 0;
    m_topState.m_consecutive = 0;
    m_bottomState.m_consecutive = 0;

    applyPostTransitionWarp(direction);

    if (m_handler != NULL) {
        m_handler->onTransition(direction);
    }
}

void
UhidEdgeTransitionService::applyPostTransitionWarp(IUhidEdgeTransitionHandler::Direction direction)
{
    if (!m_config.m_enableVirtualWarp || !hasUsableGeometry()) {
        return;
    }

    const SInt32 rightWarp = std::max<SInt32>(0, m_geometry.m_width - 1 - m_config.m_warpOffset);
    const SInt32 bottomWarp = std::max<SInt32>(0, m_geometry.m_height - 1 - m_config.m_warpOffset);

    switch (direction) {
    case IUhidEdgeTransitionHandler::kLeft:
        m_virtualX = rightWarp;
        break;

    case IUhidEdgeTransitionHandler::kRight:
        m_virtualX = std::min<SInt32>(m_config.m_warpOffset, m_geometry.m_width - 1);
        break;

    case IUhidEdgeTransitionHandler::kTop:
        m_virtualY = bottomWarp;
        break;

    case IUhidEdgeTransitionHandler::kBottom:
        m_virtualY = std::min<SInt32>(m_config.m_warpOffset, m_geometry.m_height - 1);
        break;
    }
}

void
UhidEdgeTransitionService::logMotion(SInt32 dx, SInt32 dy, bool stuck) const
{
    LOG((CLOG_DEBUG1
        "uhid-edge motion dx=%+d dy=%+d virtual=%d,%d left=%d right=%d top=%d bottom=%d stuck=%s",
        dx,
        dy,
        m_virtualX,
        m_virtualY,
        m_leftState.m_consecutive,
        m_rightState.m_consecutive,
        m_topState.m_consecutive,
        m_bottomState.m_consecutive,
        stuck ? "yes" : "no"));
}

const char*
UhidEdgeTransitionService::directionName(IUhidEdgeTransitionHandler::Direction direction) const
{
    switch (direction) {
    case IUhidEdgeTransitionHandler::kLeft:
        return "left";

    case IUhidEdgeTransitionHandler::kRight:
        return "right";

    case IUhidEdgeTransitionHandler::kTop:
        return "top";

    case IUhidEdgeTransitionHandler::kBottom:
        return "bottom";
    }

    return "unknown";
}

SInt32
UhidEdgeTransitionService::clampVirtual(SInt32 value) const
{
    return std::max(m_config.m_virtualMin, std::min(m_config.m_virtualMax, value));
}
