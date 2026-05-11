cmake_minimum_required(VERSION 3.25)
project(CanUartTool VERSION 1.0.0 LANGUAGES C)

add_compile_definitions(UNICODE _UNICODE)

set(SOURCES
    src/main.c
    src/can_hal.c
    src/can_hal_pcan.c
    src/can_hal_ixxat.c
    src/can_dispatcher.c
    src/can_manager.c
    src/uart_manager.c
    src/tab_can_upgrade.c
    src/can_command.c
    src/tab_can_command.c
    src/tab_lora_data.c
    src/tab_lora_cfg.c
)

set(RESOURCES
    resources/resource.rc
)

add_executable(can-uart-tool
    ${SOURCES}
    ${RESOURCES}
)

if(MINGW)
    target_compile_options(can-uart-tool PRIVATE -Os -ffunction-sections -fdata-sections)
    target_link_options(can-uart-tool PRIVATE
        -mwindows -static-libgcc -s -Wl,--gc-sections -Wl,--strip-all
    )
elseif(MSVC)
    target_compile_options(can-uart-tool PRIVATE /utf-8)
    set_target_properties(can-uart-tool PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:WINDOWS /MANIFEST:NO"
    )
endif()

if(MINGW)
    target_link_libraries(can-uart-tool PRIVATE
        -lcomctl32 -lcomdlg32 -lgdi32 -lsetupapi -lws2_32
    )
elseif(MSVC)
    target_link_libraries(can-uart-tool PRIVATE
        comctl32 comdlg32 gdi32 setupapi ws2_32
    )
endif()

target_include_directories(can-uart-tool PRIVATE include)

# Use static lora_sdk (no __declspec dllimport)
target_compile_definitions(can-uart-tool PRIVATE LORA_SDK_STATIC)

# --- LoRa SDK (static library for internal use, DLL for customers) ---
add_subdirectory(loralib)

target_link_libraries(can-uart-tool PRIVATE lora_sdk_static)
