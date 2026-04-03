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
           int height);

    bool contains(int globalX, int globalY) const;

public:
    std::string m_id;
    std::string m_hostId;
    std::string m_name;
    int m_x;
    int m_y;
    int m_width;
    int m_height;
};

} // namespace layout
} // namespace etherwaver
