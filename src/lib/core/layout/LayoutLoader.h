/*
 * Etherwaver layout loader
 */

#pragma once

#include "barrier/protocol_types.h"
#include "core/layout/ScreenManager.h"

#include <map>
#include <string>
#include <vector>

class Config;

namespace etherwaver {
namespace layout {

struct HostGeometry {
    HostGeometry();
    HostGeometry(int x, int y, int width, int height);

    int m_x;
    int m_y;
    int m_width;
    int m_height;
};

class LayoutLoader {
public:
    static ScreenManager loadLayout(const std::string& layoutPath,
                                    const Config& config,
                                    const std::map<std::string, HostGeometry>& hostGeometries,
                                    const std::map<std::string, std::vector<ClientScreenInfo> >& hostScreens,
                                    const std::string& primaryHostId);

private:
    static ScreenManager loadJsonLayout(const std::string& layoutPath);
    static ScreenManager convertConfigToObjectLayout(const Config& config,
                                                     const std::map<std::string, HostGeometry>& hostGeometries,
                                                     const std::map<std::string, std::vector<ClientScreenInfo> >& hostScreens,
                                                     const std::string& primaryHostId);
};

} // namespace layout
} // namespace etherwaver
