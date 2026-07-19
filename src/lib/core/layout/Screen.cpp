#include "core/layout/Screen.h"

namespace etherwaver {
namespace layout {

Screen::Screen() :
    m_x(0),
    m_y(0),
    m_width(0),
    m_height(0)
{
}

Screen::Screen(const std::string& id,
               const std::string& hostId,
               const std::string& name,
               int x,
               int y,
               int width,
               int height,
               const std::string& kind,
               const std::string& leftLink,
               const std::string& rightLink,
               const std::string& topLink,
               const std::string& bottomLink,
               const std::string& bridgeAddress,
               int bridgePort) :
    m_id(id),
    m_hostId(hostId),
    m_name(name),
    m_x(x),
    m_y(y),
    m_width(width),
    m_height(height),
    m_kind(kind),
    m_leftLink(leftLink),
    m_rightLink(rightLink),
    m_topLink(topLink),
    m_bottomLink(bottomLink),
    m_bridgeAddress(bridgeAddress),
    m_bridgePort(bridgePort)
{
}

bool
Screen::contains(int globalX, int globalY) const
{
    return (globalX >= m_x &&
            globalY >= m_y &&
            globalX < m_x + m_width &&
            globalY < m_y + m_height);
}

} // namespace layout
} // namespace etherwaver
