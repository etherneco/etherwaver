#include "core/layout/Host.h"

namespace etherwaver {
namespace layout {

Host::Host()
{
}

Host::Host(const std::string& id, const std::string& name) :
    m_id(id),
    m_name(name)
{
}

void
Host::addScreen(const Screen& screen)
{
    m_screens.push_back(screen);
}

} // namespace layout
} // namespace etherwaver
