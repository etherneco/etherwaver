/*
 * Etherwaver layout host model
 */

#pragma once

#include "core/layout/Screen.h"

#include <string>
#include <vector>

namespace etherwaver {
namespace layout {

class Host {
public:
    Host();
    Host(const std::string& id, const std::string& name);

    void addScreen(const Screen& screen);

public:
    std::string m_id;
    std::string m_name;
    std::vector<Screen> m_screens;
};

} // namespace layout
} // namespace etherwaver
