# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

# Custom boards (apnea_v1, apnea_v2) are found under boards/
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
# For the nRF52840 DK, pass the overlay with -DDTC_OVERLAY_FILE (see README) or uncomment:
# list(APPEND DTC_OVERLAY_FILE ${CMAKE_CURRENT_SOURCE_DIR}/boards/arm/nrf52840dk_nrf52840/nrf52840dk_nrf52840.overlay)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(Apnea)

# Nordic SDK app start
FILE(GLOB app_sources src/*.c)
target_sources(app PRIVATE
  ${app_sources}
)
# Nordic SDK app end

# Add subdirectories for drivers and lib files 
add_subdirectory(drivers)
add_subdirectory(app)

# Headers shared with the Zephyr Bluetooth samples
zephyr_library_include_directories(${ZEPHYR_BASE}/samples/bluetooth)

