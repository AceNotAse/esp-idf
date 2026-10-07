# Home Assistant rotary knob for the Elecrow CrowPanel 1.28" ESP32-S3 Rotary Display.
# This is a normal ESP-IDF project: `idf.py build flash monitor` from this folder.
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(ha_rotary_knob)
