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
        if (it->m_id == target || it->m_name == target) {
            return &(*it);
        }
    }

    return NULL;
}

bool
rangesOverlapLocal(int start1, int end1, int start2, int end2)
{
    return (start1 < end2 && start2 < end1);
}

bool
scoreDirectionalCandidate(const Screen& source,
                          const Screen& candidate,
                          EDirection direction,
                          int& distance,
                          int& offset)
{
    const int sourceRight = source.m_x + source.m_width;
    const int sourceBottom = source.m_y + source.m_height;
    const int sourceCenterX = source.m_x + source.m_width / 2;
    const int sourceCenterY = source.m_y + source.m_height / 2;
    const int candidateRight = candidate.m_x + candidate.m_width;
    const int candidateBottom = candidate.m_y + candidate.m_height;
    const int candidateCenterX = candidate.m_x + candidate.m_width / 2;
    const int candidateCenterY = candidate.m_y + candidate.m_height / 2;

    switch (direction) {
    case kLeft:
        if ((candidateRight <= source.m_x) &&
            rangesOverlapLocal(candidate.m_y, candidate.m_y + candidate.m_height,
                               source.m_y, source.m_y + source.m_height)) {
            distance = source.m_x - candidateRight;
            offset = std::abs(candidateCenterY - sourceCenterY);
            return true;
        }
        break;

    case kRight:
        if ((sourceRight <= candidate.m_x) &&
            rangesOverlapLocal(candidate.m_y, candidate.m_y + candidate.m_height,
                               source.m_y, source.m_y + source.m_height)) {
            distance = candidate.m_x - sourceRight;
            offset = std::abs(candidateCenterY - sourceCenterY);
            return true;
        }
        break;

    case kTop:
        if ((candidateBottom <= source.m_y) &&
            rangesOverlapLocal(candidate.m_x, candidate.m_x + candidate.m_width,
                               source.m_x, source.m_x + source.m_width)) {
            distance = source.m_y - candidateBottom;
            offset = std::abs(candidateCenterX - sourceCenterX);
            return true;
        }
        break;

    case kBottom:
        if ((sourceBottom <= candidate.m_y) &&
            rangesOverlapLocal(candidate.m_x, candidate.m_x + candidate.m_width,
                               source.m_x, source.m_x + source.m_width)) {
            distance = candidate.m_y - sourceBottom;
            offset = std::abs(candidateCenterX - sourceCenterX);
            return true;
        }
        break;

    default:
        break;
    }

    return false;
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

    const std::string& directionalLink = getDirectionalLink(*source, direction);
    const Screen* linked = resolveLinkedScreen(m_screens, directionalLink);
    if (linked != NULL && linked->m_id != source->m_id) {
        return linked;
    }

    const bool constrainToLinkedHost =
        !directionalLink.empty() && m_indicesByHost.find(directionalLink) != m_indicesByHost.end();

    const Screen* best = NULL;
    int bestDistance = std::numeric_limits<int>::max();
    int bestOffset = std::numeric_limits<int>::max();

    for (std::vector<Screen>::const_iterator it = m_screens.begin();
         it != m_screens.end(); ++it) {
        if (it->m_id == source->m_id) {
            continue;
        }

        if (constrainToLinkedHost && it->m_hostId != directionalLink) {
            continue;
        }

        int distance = std::numeric_limits<int>::max();
        int offset = std::numeric_limits<int>::max();
        if (scoreDirectionalCandidate(*source, *it, direction, distance, offset) &&
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
