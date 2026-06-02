#include "platform/InputGrabber.h"

#include "base/Log.h"

#if defined(__linux__)

#include <errno.h>
#include <linux/input.h>
#include <string.h>
#include <sys/ioctl.h>

InputGrabber::InputGrabber()
    : m_exclusive(true)
{
}

void
InputGrabber::setExclusive(bool exclusive)
{
    m_exclusive = exclusive;
}

bool
InputGrabber::exclusive() const
{
    return m_exclusive;
}

bool
InputGrabber::apply(int fd, const std::string& path, bool grab) const
{
    if (fd < 0) {
        return false;
    }

    if (::ioctl(fd, EVIOCGRAB, grab ? 1 : 0) == 0) {
        LOG((CLOG_NOTE "input-grabber: %s %s", grab ? "grabbed" : "released", path.c_str()));
        return true;
    }

    LOG((CLOG_WARN
        "input-grabber: failed to %s %s (%s)",
        grab ? "grab" : "release",
        path.c_str(),
        strerror(errno)));
    return false;
}

#endif
