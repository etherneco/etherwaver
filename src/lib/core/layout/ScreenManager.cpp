#include "core/layout/ScreenManager.h"

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
ScreenManager::getScreenByIdOrName(const std::string& screenIdOrName) const
{
    const Screen* screen = getScreen(screenIdOrName);
    if (screen != NULL) {
        return screen;
    }

    for (std::vector<Screen>::const_iterator it = m_screens.begin();
         it != m_screens.end(); ++it) {
        if (it->m_name == screenIdOrName) {
            return &(*it);
        }
    }

    return NULL;
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

    return NULL;
}

} // namespace layout
} // namespace etherwaver
