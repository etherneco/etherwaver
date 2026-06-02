#include "client/SoftCursorPositioner.h"

#include "test/global/gtest.h"

namespace {

class FakeCursorPositionProvider : public ICursorPositionProvider {
public:
    FakeCursorPositionProvider(SInt32 x, SInt32 y)
        : m_x(x)
        , m_y(y)
        , m_getCursorPosCount(0)
    {
    }

    void getCursorPos(SInt32& x, SInt32& y) override
    {
        ++m_getCursorPosCount;
        x = m_x;
        y = m_y;
    }

    SInt32 m_x;
    SInt32 m_y;
    int m_getCursorPosCount;
};

class FakeCursorMotionSink : public ICursorMotionSink {
public:
    FakeCursorMotionSink()
        : m_primeCount(0)
        , m_moveCount(0)
        , m_primeX(0)
        , m_primeY(0)
        , m_moveX(0)
        , m_moveY(0)
    {
    }

    void primeAbsolutePosition(SInt32 x, SInt32 y) override
    {
        ++m_primeCount;
        m_primeX = x;
        m_primeY = y;
    }

    void mouseMoveAbsolute(SInt32 x, SInt32 y) override
    {
        ++m_moveCount;
        m_moveX = x;
        m_moveY = y;
    }

    int m_primeCount;
    int m_moveCount;
    SInt32 m_primeX;
    SInt32 m_primeY;
    SInt32 m_moveX;
    SInt32 m_moveY;
};

TEST(SoftCursorPositionerTests, moveToPrimesFromCurrentCursorPositionBeforeMovingToTarget)
{
    FakeCursorPositionProvider positionProvider(3000, 400);
    FakeCursorMotionSink motionSink;

    const SoftCursorPositioner::Result result =
        SoftCursorPositioner::moveTo(positionProvider, motionSink, 1920, 400);

    EXPECT_EQ(1, positionProvider.m_getCursorPosCount);
    EXPECT_EQ(1, motionSink.m_primeCount);
    EXPECT_EQ(3000, motionSink.m_primeX);
    EXPECT_EQ(400, motionSink.m_primeY);
    EXPECT_EQ(1, motionSink.m_moveCount);
    EXPECT_EQ(1920, motionSink.m_moveX);
    EXPECT_EQ(400, motionSink.m_moveY);

    EXPECT_EQ(3000, result.m_currentX);
    EXPECT_EQ(400, result.m_currentY);
    EXPECT_EQ(1920, result.m_targetX);
    EXPECT_EQ(400, result.m_targetY);
    EXPECT_EQ(-1080, result.m_deltaX);
    EXPECT_EQ(0, result.m_deltaY);
}

} // namespace
