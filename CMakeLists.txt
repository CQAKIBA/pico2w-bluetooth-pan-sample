cmake_minimum_required(VERSION 3.13)
if (NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
endif()
if (NOT PICO_SDK_PATH)
    message(FATAL_ERROR "PICO_SDK_PATH is required (pico-sdk with its submodules)")
endif()
include("${PICO_SDK_PATH}/external/pico_sdk_import.cmake")
project(pico2w_bt_pan_tether C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()
add_executable(pico2w_bt_pan_tether main.c)
target_include_directories(pico2w_bt_pan_tether PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(pico2w_bt_pan_tether PRIVATE CYW43_LWIP=0)
target_link_libraries(pico2w_bt_pan_tether PRIVATE
    pico_stdlib
    pico_cyw43_arch_threadsafe_background
    pico_btstack_cyw43
    pico_btstack_classic
    pico_btstack_bnep_lwip
    pico_lwip_nosys
)
pico_enable_stdio_usb(pico2w_bt_pan_tether 1)
pico_enable_stdio_uart(pico2w_bt_pan_tether 0)
pico_add_extra_outputs(pico2w_bt_pan_tether)
