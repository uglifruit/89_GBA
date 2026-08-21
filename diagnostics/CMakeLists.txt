# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.13)
# reuse the parent applet's pico_sdk_import.cmake
include(${CMAKE_CURRENT_LIST_DIR}/../pico_sdk_import.cmake)

set(CARD_NAME gba_loopback)
project(gba_loopback C CXX ASM)
set(CMAKE_CXX_STANDARD 17)
pico_sdk_init()

# The diagnostic reuses the real transport (gba_spi_init/gba_spi_xfer32) from the parent
# gba_multiboot.cpp, and the parent's gba_spi.pio, so a pass here validates the real path.
add_executable(${CARD_NAME}
    loopback.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../gba_multiboot.cpp)
pico_generate_pio_header(${CARD_NAME} ${CMAKE_CURRENT_LIST_DIR}/../gba_spi.pio)
target_include_directories(${CARD_NAME} PUBLIC ${CMAKE_CURRENT_LIST_DIR}/..)
target_link_libraries(${CARD_NAME}
    pico_unique_id
    pico_stdlib
    hardware_dma
    hardware_i2c
    hardware_pwm
    hardware_adc
    hardware_spi
    hardware_pio
    pico_multicore)
pico_add_extra_outputs(${CARD_NAME})
target_compile_definitions(${CARD_NAME} PRIVATE PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)
pico_enable_stdio_usb(${CARD_NAME} 0)
target_compile_options(${CARD_NAME} PRIVATE -Wall -Wextra)

# Second diagnostic: live GBA handshake probe (shows the GBA's actual reply on the LEDs).
add_executable(gba_probe
    gba_probe.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../gba_multiboot.cpp)
pico_generate_pio_header(gba_probe ${CMAKE_CURRENT_LIST_DIR}/../gba_spi.pio)
target_include_directories(gba_probe PUBLIC ${CMAKE_CURRENT_LIST_DIR}/..)
target_link_libraries(gba_probe
    pico_unique_id pico_stdlib hardware_dma hardware_i2c hardware_pwm
    hardware_adc hardware_spi hardware_pio pico_multicore)
pico_add_extra_outputs(gba_probe)
target_compile_definitions(gba_probe PRIVATE PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)
pico_enable_stdio_usb(gba_probe 0)
target_compile_options(gba_probe PRIVATE -Wall -Wextra)

# Staged, scope-friendly bring-up diagnostic (switch selects stage 1/2/3).
add_executable(bringup
    bringup.cpp
    ${CMAKE_CURRENT_LIST_DIR}/../gba_multiboot.cpp)
pico_generate_pio_header(bringup ${CMAKE_CURRENT_LIST_DIR}/../gba_spi.pio)
target_include_directories(bringup PUBLIC ${CMAKE_CURRENT_LIST_DIR}/..)
target_link_libraries(bringup
    pico_unique_id pico_stdlib hardware_dma hardware_i2c hardware_pwm
    hardware_adc hardware_spi hardware_pio pico_multicore)
pico_add_extra_outputs(bringup)
target_compile_definitions(bringup PRIVATE PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)
pico_enable_stdio_usb(bringup 0)
target_compile_options(bringup PRIVATE -Wall -Wextra)
