#include "core/layout/ScreenManager.h"

#include <limits>

namespace etherwaver {
namespace layout {

namespace {

const Screen*
resolveLinkedScreen(const std::vector<Screen>& screens, const std::string& target)
{
    if (target.empty()) {
        return NULL;
    }

    for (std::vector<Screen>::const_iterator it = screens.begin(); it != screens.end(); ++it) {
        if (it->m_id == target || it->m_name == target || it->m_hostId == target) {
            return &(*it);
        }
    }

    return NULL;
}

const std::string&
getDirectionalLink(const Screen& screen, EDirection direction)
{
    switch (direction) {
    case kLeft:
        return screen.m_leftLink;
    case kRight:
        return screen.m_rightLink;
    case kTop:
        return screen.m_topLink;
    case kBottom:
        return screen.m_bottomLink;
    default:
        break;
    }

    static const std::string kEmpty;
    return kEmpty;
}

} // namespace

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

    const Screen* linked = resolveLinkedScreen(m_screens, getDirectionalLink(*source, direction));
    if (linked != NULL && linked->m_id != source->m_id) {
        return linked;
    }

    const Screen* best = NULL;
    int bestDistance = std::numeric_limits<int>::max();
    int bestOffset = std::numeric_limits<int>::max();

    const int sourceRight = source->m_x + source->m_width;
    const int sourceBottom = source->m_y + source->m_height;
    const int sourceCenterX = source->m_x + source->m_width / 2;
    const int sourceCenterY = source->m_y + source->m_height / 2;

    for (std::vector<Screen>::const_iterator it = m_screens.begin();
         it != m_screens.end(); ++it) {
        if (it->m_id == source->m_id) {
            continue;
        }

        bool candidate = false;
        int distance = std::numeric_limits<int>::max();
        int offset = std::numeric_limits<int>::max();
        const int candidateRight = it->m_x + it->m_width;
        const int candidateBottom = it->m_y + it->m_height;
        const int candidateCenterX = it->m_x + it->m_width / 2;
        const int candidateCenterY = it->m_y + it->m_height / 2;

        switch (direction) {
        case kLeft:
            candidate =
                (candidateRight <= source->m_x) &&
                rangesOverlap(it->m_y, it->m_y + it->m_height,
                              source->m_y, source->m_y + source->m_height);
            if (candidate) {
                distance = source->m_x - candidateRight;
                offset = std::abs(candidateCenterY - sourceCenterY);
            }
            break;

        case kRight:
            candidate =
                (sourceRight <= it->m_x) &&
                rangesOverlap(it->m_y, it->m_y + it->m_height,
                              source->m_y, source->m_y + source->m_height);
            if (candidate) {
                distance = it->m_x - sourceRight;
                offset = std::abs(candidateCenterY - sourceCenterY);
            }
            break;

        case kTop:
            candidate =
                (candidateBottom <= source->m_y) &&
                rangesOverlap(it->m_x, it->m_x + it->m_width,
                              source->m_x, source->m_x + source->m_width);
            if (candidate) {
                distance = source->m_y - candidateBottom;
                offset = std::abs(candidateCenterX - sourceCenterX);
            }
            break;

        case kBottom:
            candidate =
                (sourceBottom <= it->m_y) &&
                rangesOverlap(it->m_x, it->m_x + it->m_width,
                              source->m_x, source->m_x + source->m_width);
            if (candidate) {
                distance = it->m_y - sourceBottom;
                offset = std::abs(candidateCenterX - sourceCenterX);
            }
            break;

        default:
            break;
        }

        if (candidate &&
            (distance < bestDistance ||
             (distance == bestDistance && offset < bestOffset))) {
            bestDistance = distance;
            bestOffset = offset;
            best = &(*it);
        }
    }

    return best;
}

} // namespace layout
} // namespace etherwaver
