# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

# This project targets the Seeed Studio XIAO ESP32-S3 (8 MB flash/8 MB PSRAM).
# Keep the target here so a fresh checkout cannot silently build for another chip.
set(IDF_TARGET "esp32s3")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(esp-irc)
