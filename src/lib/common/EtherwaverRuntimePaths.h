/*
 * Etherwaver -- shared runtime path helpers
 *
 * The cursor position server (waverd --cursor-position-server) and the client
 * that queries it must agree on these paths. Keeping them in one place stops
 * the two sides from drifting apart.
 */

#pragma once

#include <string>

#if defined(__linux__)
#include <stdio.h>
#include <unistd.h>
#endif

namespace etherwaver {

inline std::string cursorPositionSocketPath()
{
#if defined(__linux__)
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "/tmp/etherwaver-cursor-%ld.sock",
             static_cast<long>(getuid()));
    return buffer;
#else
    return "";
#endif
}

} // namespace etherwaver
