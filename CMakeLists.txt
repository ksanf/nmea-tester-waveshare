# NMEA Tester project configuration
cmake_minimum_required(VERSION 3.5)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/VERSION" PROJECT_VER LIMIT_COUNT 1)
if(NOT PROJECT_VER MATCHES "^[0-9]+\\.[0-9]$")
    message(FATAL_ERROR "VERSION must use tenths: 1.1, 1.2, ... 1.9, 2.0")
endif()
project(nmea-tester)
