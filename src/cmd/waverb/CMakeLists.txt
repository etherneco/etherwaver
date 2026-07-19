if (UNIX AND NOT APPLE)
    add_executable(waverb linux_main.cpp)
    target_link_libraries(waverb
        arch base common io ipc mt net platform synlib ${libs} ${OPENSSL_LIBS})
endif()
