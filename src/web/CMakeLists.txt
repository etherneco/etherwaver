# Etherwaver web panel (waver-web): Python 3 backend + static frontend.
# No third-party Python or JavaScript dependencies are needed at build or run time.

if (CMAKE_VERSION VERSION_LESS 3.12)
    find_package (PythonInterp 3 REQUIRED)
    set (WAVER_WEB_PYTHON ${WAVER_WEB_PYTHON})
else ()
    find_package (Python3 COMPONENTS Interpreter REQUIRED)
    set (WAVER_WEB_PYTHON ${Python3_EXECUTABLE})
endif ()

set (WAVER_WEB_SRC ${CMAKE_CURRENT_SOURCE_DIR})
set (WAVER_WEB_STATIC ${CMAKE_CURRENT_BINARY_DIR}/static)
set (WAVER_WEB_DATADIR share/etherwaver/web)

file (GLOB WAVER_WEB_FRONTEND_SOURCES ${WAVER_WEB_SRC}/frontend/src/*.* ${WAVER_WEB_SRC}/frontend/src/js/*.js
      ${WAVER_WEB_SRC}/frontend/src/js/views/*.js)
add_custom_command (
    OUTPUT ${WAVER_WEB_STATIC}/index.html
    COMMAND ${WAVER_WEB_PYTHON} -m waver_web.build --src ${WAVER_WEB_SRC}/frontend/src --out ${WAVER_WEB_STATIC}
    WORKING_DIRECTORY ${WAVER_WEB_SRC}
    DEPENDS ${WAVER_WEB_FRONTEND_SOURCES} ${WAVER_WEB_SRC}/waver_web/build.py
    COMMENT "Building the web panel frontend")
add_custom_target (waver-web-frontend ALL DEPENDS ${WAVER_WEB_STATIC}/index.html)

configure_file (${WAVER_WEB_SRC}/dist/waver-web.in ${CMAKE_CURRENT_BINARY_DIR}/waver-web @ONLY)
configure_file (${WAVER_WEB_SRC}/dist/waver-web.service.in ${CMAKE_CURRENT_BINARY_DIR}/waver-web.service @ONLY)

install (DIRECTORY ${WAVER_WEB_SRC}/waver_web DESTINATION ${WAVER_WEB_DATADIR} COMPONENT web
         FILES_MATCHING PATTERN "*.py" PATTERN "languages.json"
         PATTERN "__pycache__" EXCLUDE PATTERN "static" EXCLUDE)
install (DIRECTORY ${WAVER_WEB_STATIC} DESTINATION ${WAVER_WEB_DATADIR}/waver_web COMPONENT web)
install (PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/waver-web DESTINATION bin COMPONENT web)
install (FILES ${WAVER_WEB_SRC}/dist/web.ini.example ${WAVER_WEB_SRC}/dist/nginx.conf.example
         DESTINATION share/doc/etherwaver COMPONENT web)
if (${CMAKE_SYSTEM_NAME} MATCHES "Linux")
    install (FILES ${CMAKE_CURRENT_BINARY_DIR}/waver-web.service DESTINATION lib/systemd/user COMPONENT web)
endif ()

if (BARRIER_BUILD_TESTS)
    add_test (NAME waver-web-unit
              COMMAND ${WAVER_WEB_PYTHON} -m unittest discover -s tests -p "test_*.py"
              WORKING_DIRECTORY ${WAVER_WEB_SRC})
    set_tests_properties (waver-web-unit PROPERTIES ENVIRONMENT "WAVER_BIN_DIR=${CMAKE_BINARY_DIR}/bin")
endif ()
