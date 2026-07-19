/*
 * Etherwaver layout screen model
 */

#pragma once

#include <string>

namespace etherwaver {
namespace layout {

class Screen {
public:
    Screen();
    Screen(const std::string& id,
           const std::string& hostId,
           const std::string& name,
           int x,
           int y,
           int width,
           int height,
           const std::string& kind = std::string(),
           const std::string& leftLink = std::string(),
           const std::string& rightLink = std::string(),
           const std::string& topLink = std::string(),
           const std::string& bottomLink = std::string(),
           const std::string& bridgeAddress = std::string(),
           int bridgePort = 0);

    bool contains(int globalX, int globalY) const;

public:
    std::string m_id;
    std::string m_hostId;
    std::string m_name;
    int m_x;
    int m_y;
    int m_width;
    int m_height;
    std::string m_kind;
    std::string m_leftLink;
    std::string m_rightLink;
    std::string m_topLink;
    std::string m_bottomLink;
    std::string m_bridgeAddress;
    int m_bridgePort;
};

} // namespace layout
} // namespace etherwaver
