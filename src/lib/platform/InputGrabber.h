#pragma once

#include <string>

class InputGrabber {
public:
    InputGrabber();

    void setExclusive(bool exclusive);
    bool exclusive() const;

    bool apply(int fd, const std::string& path, bool grab) const;

private:
    bool m_exclusive;
};
