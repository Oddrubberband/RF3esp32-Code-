# Top-level Espressif IoT Development Framework (ESP-IDF) project entry point.
#
# The real application sources live in the src/ component; this file mostly
# tells ESP-IDF where to find that component and what to name the final
# firmware project.
cmake_minimum_required(VERSION 3.16.0)
# Treat src/ as an extra component directory so its own CMake build list
# (CMakeLists) can register the firmware sources and dependencies.
set(EXTRA_COMPONENT_DIRS src)
# This project doesn't use managed components, and disabling the component
# manager avoids a broken prepare_components step in the current toolchain.
set(ENV{IDF_COMPONENT_MANAGER} 0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nrf24_app)
