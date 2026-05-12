# barrier -- mouse and keyboard sharing utility
#
# This package is free software; you can redistribute it and/or
# modify it under the terms of the GNU General Public License
# found in the file LICENSE that should have accompanied this file.

find_package(Qt5 REQUIRED COMPONENTS Core Widgets)

set(CMAKE_AUTOMOC ON)

add_executable(etherwaver-debug-bounds
    debug-bounds.cpp
)

target_link_libraries(etherwaver-debug-bounds Qt5::Core Qt5::Widgets)

if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    install(TARGETS etherwaver-debug-bounds DESTINATION bin)
endif()
