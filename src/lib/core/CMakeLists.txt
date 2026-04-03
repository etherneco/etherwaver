file(GLOB headers "*.h" "layout/*.h")
file(GLOB sources "*.cpp" "layout/*.cpp")

if (BARRIER_ADD_HEADERS)
    list(APPEND sources ${headers})
endif()

add_library(core STATIC ${sources})
