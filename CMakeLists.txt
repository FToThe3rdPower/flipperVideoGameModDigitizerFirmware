cmake_minimum_required(VERSION 3.13)

# Pull in Pico SDK (must be before project())
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(vgm_scope C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(vgm_scope
    main.c
)

target_link_libraries(vgm_scope
    pico_stdlib
    hardware_adc
    hardware_dma
    hardware_uart
)

# UART output over USB (for debugging with Flipper acting as monitor)
pico_enable_stdio_usb(vgm_scope 0)
pico_enable_stdio_uart(vgm_scope 0)

# Generate .uf2 for drag-and-drop flashing
pico_add_extra_outputs(vgm_scope)
