#include "client/SoftCursorPositioner.h"

SoftCursorPositioner::Result::Result()
    : m_currentX(0)
    , m_currentY(0)
    , m_targetX(0)
    , m_targetY(0)
    , m_deltaX(0)
    , m_deltaY(0)
{
}

SoftCursorPositioner::Result
SoftCursorPositioner::moveTo(ICursorPositionProvider& positionProvider,
                             ICursorMotionSink& motionSink,
                             SInt32 targetX,
                             SInt32 targetY)
{
    Result result;
    positionProvider.getCursorPos(result.m_currentX, result.m_currentY);
    result.m_targetX = targetX;
    result.m_targetY = targetY;
    result.m_deltaX = targetX - result.m_currentX;
    result.m_deltaY = targetY - result.m_currentY;

    motionSink.primeAbsolutePosition(result.m_currentX, result.m_currentY);
    motionSink.mouseMoveAbsolute(targetX, targetY);

    return result;
}
