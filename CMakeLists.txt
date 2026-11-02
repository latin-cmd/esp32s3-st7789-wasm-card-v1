# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

# Stop runaway plugins after a bounded number of interpreter instructions.
set(WAMR_BUILD_INSTRUCTION_METERING 1 CACHE BOOL "Enable WAMR instruction metering")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32s3-st7789-wasm-card-v1)
