cmake_minimum_required(VERSION 3.13)
include(pico_sdk_import.cmake)

# Portal Zero - Skylanders portal emulator
# Waveshare RP2040-Zero (2 MB flash)
set(PICO_BOARD waveshare_rp2040_zero)

project(portalzero C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if (PICO_SDK_VERSION_STRING VERSION_LESS "1.3.0")
    message(FATAL_ERROR "Raspberry Pi Pico SDK version 1.3.0 (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
endif()

pico_sdk_init()

add_executable(PortalZero
        ${CMAKE_CURRENT_LIST_DIR}/main.c
        ${CMAKE_CURRENT_LIST_DIR}/portal.c
        ${CMAKE_CURRENT_LIST_DIR}/figures.c
        ${CMAKE_CURRENT_LIST_DIR}/nfc.c
        ${CMAKE_CURRENT_LIST_DIR}/menu.c
        ${CMAKE_CURRENT_LIST_DIR}/oled.c
        ${CMAKE_CURRENT_LIST_DIR}/usb_descriptors.c
        ${CMAKE_CURRENT_LIST_DIR}/hw_config.c
        )

# SD card library (FatFs over SPI)
add_subdirectory(no-OS-FatFS-SD-SPI-RPi-Pico/FatFs_SPI build)

target_link_libraries(PortalZero
        PUBLIC
        pico_stdlib
        pico_multicore
        tinyusb_device
        tinyusb_board
        hardware_i2c
        hardware_gpio
        hardware_spi
        hardware_watchdog
        FatFs_SPI
)

# No serial output: GP0/GP1 would otherwise be taken by the UART,
# but GP0 is used for the UP button. USB is used by the portal itself.
pico_enable_stdio_uart(PortalZero 0)
pico_enable_stdio_usb(PortalZero 0)

# Fix for newer toolchains: the SD driver uses the BSD type u_int8_t
target_compile_definitions(PortalZero PRIVATE u_int8_t=uint8_t)

# take tusb_config.h straight from the project folder (instead of copying it into the SDK)
target_include_directories(PortalZero PRIVATE ${CMAKE_CURRENT_LIST_DIR})

# generate .uf2 etc.
pico_add_extra_outputs(PortalZero)

# SD card test program (build/SDTEST.uf2) - for troubleshooting only
add_executable(SDTEST
        ${CMAKE_CURRENT_LIST_DIR}/tools/sdtest.c
        ${CMAKE_CURRENT_LIST_DIR}/oled.c
        ${CMAKE_CURRENT_LIST_DIR}/hw_config.c
        )
target_link_libraries(SDTEST PUBLIC pico_stdlib hardware_i2c hardware_gpio hardware_spi FatFs_SPI)
pico_enable_stdio_uart(SDTEST 0)
pico_enable_stdio_usb(SDTEST 0)
target_compile_definitions(SDTEST PRIVATE u_int8_t=uint8_t)
target_include_directories(SDTEST PRIVATE ${CMAKE_CURRENT_LIST_DIR})
pico_add_extra_outputs(SDTEST)
