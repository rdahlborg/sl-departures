cmake_minimum_required(VERSION 3.16)
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
# Optional developer Wi-Fi credentials, never checked in. Release builds
# leave them out and get Wi-Fi through the web installer instead.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/secrets.defaults")
    list(APPEND SDKCONFIG_DEFAULTS "secrets.defaults")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(sl-departures)
