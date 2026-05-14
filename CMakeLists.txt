cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/src")
project(esp_vp)

if(IDF_VERSION_MAJOR LESS 6)
    message(FATAL_ERROR "ESP-IDF 6.0 or newer is required. Export an ESP-IDF 6.x environment before building.")
endif()
