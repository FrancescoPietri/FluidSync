# Generated Cmake Pico project file

cmake_minimum_required(VERSION 3.13)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Initialise pico_sdk from installed location
# (note this can come from environment, CMake cache etc)

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

set(PICO_BOARD pico_w CACHE STRING "Board type")

include(pico_sdk_import.cmake)
project(FluidSync C CXX ASM)

set(FREERTOS_KERNEL_PATH ${CMAKE_CURRENT_LIST_DIR}/FreeRTOS-Kernel CACHE PATH "Path to FreeRTOS")
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config INTERFACE ${CMAKE_CURRENT_LIST_DIR})
include(${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2040/FreeRTOS_Kernel_import.cmake)

pico_sdk_init()

add_executable(FluidSync FluidSync.c FluidSyncHW.c FluidSyncUI.c FluidSyncWeb.c)

# web/index.html (the web app graphics) is embedded as a byte array: edit the HTML and rebuild
set(WEB_INDEX ${CMAKE_CURRENT_LIST_DIR}/web/index.html)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${WEB_INDEX})
file(READ ${WEB_INDEX} WEB_HEX HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," WEB_HEX "${WEB_HEX}")
file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/web_index.h
     CONTENT "//generated from web/index.html, do not edit\nstatic const char WEB_INDEX_HTML[] = {@WEB_HEX@};\n" @ONLY)
target_include_directories(FluidSync PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

add_subdirectory(lvgl)
target_include_directories(lvgl PUBLIC ${CMAKE_CURRENT_LIST_DIR})

# WLAN created by the on-board ESP8285 for the order web server. Change with: cmake -DAP_SSID=.. -DAP_PASSWORD=.. (min 8 chars, no quotes or commas)
set(AP_SSID "FluidSync" CACHE STRING "Access point name")
set(AP_PASSWORD "fluidsync" CACHE STRING "Access point password")
target_compile_definitions(FluidSync PRIVATE AP_SSID="${AP_SSID}" AP_PASSWORD="${AP_PASSWORD}")

pico_set_program_name(FluidSync "FluidSync")
pico_set_program_version(FluidSync "0.1")

pico_enable_stdio_uart(FluidSync 0)
pico_enable_stdio_usb(FluidSync 1)

target_include_directories(FluidSync PRIVATE
        ${CMAKE_CURRENT_LIST_DIR}
        ${FREERTOS_KERNEL_PATH}/include
        ${FREERTOS_KERNEL_PATH}/portable/ThirdParty/GCC/RP2040/include
)

target_link_libraries(FluidSync PRIVATE
        pico_stdlib
        pico_rand
        FreeRTOS-Kernel
        FreeRTOS-Kernel-Heap4
        pico_cyw43_arch_none
        hardware_pwm
        lvgl
        hardware_spi
        hardware_flash
        pico_flash
        hardware_timer
)

pico_enable_stdio_usb(FluidSync 1)

pico_add_extra_outputs(FluidSync)