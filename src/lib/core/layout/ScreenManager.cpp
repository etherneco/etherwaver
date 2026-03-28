#include "core/layout/ScreenManager.h"

#include <limits>

namespace etherwaver {
namespace layout {

void
ScreenManager::setScreens(const std::vector<Screen>& screens)
{
    m_screens = screens;
    m_indexById.clear();
    m_indicesByHost.clear();

    for (size_t i = 0; i < m_screens.size(); ++i) {
        m_indexById[m_screens[i].m_id] = i;
        m_indicesByHost[m_screens[i].m_hostId].push_back(i);
    }
}

bool
ScreenManager::empty() const
{
    return m_screens.empty();
}

const std::vector<Screen>&
ScreenManager::getScreens() const
{
    return m_screens;
}

const Screen*
ScreenManager::findScreenAt(int globalX, int globalY) const
{
    for (std::vector<Screen>::const_iterator it = m_screens.begin();
         it != m_screens.end(); ++it) {
        if (it->contains(globalX, globalY)) {
            return &(*it);
        }
    }
    return NULL;
}

const Screen*
ScreenManager::getScreen(const std::string& screenId) const
{
    std::map<std::string, size_t>::const_iterator it = m_indexById.find(screenId);
    if (it == m_indexById.end()) {
        return NULL;
    }
    return &m_screens[it->second];
}

const Screen*
ScreenManager::getFirstScreenForHost(const std::string& hostId) const
{
    std::map<std::string, std::vector<size_t> >::const_iterator it = m_indicesByHost.find(hostId);
    if (it == m_indicesByHost.end() || it->second.empty()) {
        return NULL;
    }
    return &m_screens[it->second.front()];
}

const Screen*
ScreenManager::getNextScreen(const std::string& screenId) const
{
    std::map<std::string, size_t>::const_iterator it = m_indexById.find(screenId);
    if (it == m_indexById.end() || m_screens.empty()) {
        return NULL;
    }

    size_t next = (it->second + 1) % m_screens.size();
    return &m_screens[next];
}

std::string
ScreenManager::getHostForScreen(const std::string& screenId) const
{
    const Screen* screen = getScreen(screenId);
    return (screen != NULL) ? screen->m_hostId : std::string();
}

bool
ScreenManager::rangesOverlap(int start1, int end1, int start2, int end2)
{
    return (start1 < end2 && start2 < end1);
}

bool
ScreenManager::hasAdjacentScreen(const std::string& screenId, EDirection direction) const
{
    return (findScreenInDirection(screenId, direction) != NULL);
}

const Screen*
ScreenManager::findScreenInDirection(const std::string& screenId, EDirection direction) const
{
    const Screen* source = getScreen(screenId);
    if (source == NULL) {
        return NULL;
    }

    const Screen* best = NULL;
    int bestDistance = std::numeric_limits<int>::max();

    for (std::vector<Screen>::const_iterator it = m_screens.begin();
         it != m_screens.end(); ++it) {
        if (it->m_id == source->m_id) {
            continue;
        }

        bool adjacent = false;
        int distance = std::numeric_limits<int>::max();
        switch (direction) {
        case kLeft:
            adjacent =
                (it->m_x + it->m_width == source->m_x) &&
                rangesOverlap(it->m_y, it->m_y + it->m_height,
                              source->m_y, source->m_y + source->m_height);
            if (adjacent) {
                distance = source->m_x - it->m_x;
            }
            break;

        case kRight:
            adjacent =
                (source->m_x + source->m_width == it->m_x) &&
                rangesOverlap(it->m_y, it->m_y + it->m_height,
                              source->m_y, source->m_y + source->m_height);
            if (adjacent) {
                distance = it->m_x - source->m_x;
            }
            break;

        case kTop:
            adjacent =
                (it->m_y + it->m_height == source->m_y) &&
                rangesOverlap(it->m_x, it->m_x + it->m_width,
                              source->m_x, source->m_x + source->m_width);
            if (adjacent) {
                distance = source->m_y - it->m_y;
            }
            break;

        case kBottom:
            adjacent =
                (source->m_y + source->m_height == it->m_y) &&
                rangesOverlap(it->m_x, it->m_x + it->m_width,
                              source->m_x, source->m_x + source->m_width);
            if (adjacent) {
                distance = it->m_y - source->m_y;
            }
            break;

        default:
            break;
        }

        if (adjacent && distance < bestDistance) {
            bestDistance = distance;
            best = &(*it);
        }
    }

    return best;
}

} // namespace layout
} // namespace etherwaver
