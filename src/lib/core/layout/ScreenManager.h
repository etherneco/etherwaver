/*
 * Etherwaver object layout manager
 */

#pragma once

#include "barrier/protocol_types.h"
#include "core/layout/Screen.h"

#include <map>
#include <string>
#include <vector>

namespace etherwaver {
namespace layout {

class ScreenManager {
public:
    void setScreens(const std::vector<Screen>& screens);

    bool empty() const;
    const std::vector<Screen>& getScreens() const;
    const Screen* findScreenAt(int globalX, int globalY) const;
    const Screen* getScreen(const std::string& screenId) const;
    const Screen* getFirstScreenForHost(const std::string& hostId) const;
    const Screen* getNextScreen(const std::string& screenId) const;
    std::string getHostForScreen(const std::string& screenId) const;
    bool hasAdjacentScreen(const std::string& screenId, EDirection direction) const;
    const Screen* findScreenInDirection(const std::string& screenId, EDirection direction) const;

private:
    static bool rangesOverlap(int start1, int end1, int start2, int end2);

private:
    std::vector<Screen> m_screens;
    std::map<std::string, size_t> m_indexById;
    std::map<std::string, std::vector<size_t> > m_indicesByHost;
};

} // namespace layout
} // namespace etherwaver
