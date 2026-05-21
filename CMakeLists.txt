cmake_minimum_required(VERSION 3.25)
project(ModHandlerPcTool VERSION 0.4.0 LANGUAGES C)

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

add_executable(modhandler-pc-tool
    ${SOURCES}
    ${RESOURCES}
)

if(MINGW)
    target_compile_options(modhandler-pc-tool PRIVATE -Os -ffunction-sections -fdata-sections)
    target_link_options(modhandler-pc-tool PRIVATE
        -mwindows -static-libgcc -s -Wl,--gc-sections -Wl,--strip-all
    )
elseif(MSVC)
    target_compile_options(modhandler-pc-tool PRIVATE /utf-8)
    set_target_properties(modhandler-pc-tool PROPERTIES
        LINK_FLAGS "/SUBSYSTEM:WINDOWS /MANIFEST:NO"
    )
endif()

if(MINGW)
    target_link_libraries(modhandler-pc-tool PRIVATE
        -lcomctl32 -lcomdlg32 -lgdi32 -lsetupapi -lws2_32
    )
elseif(MSVC)
    target_link_libraries(modhandler-pc-tool PRIVATE
        comctl32 comdlg32 gdi32 setupapi ws2_32
    )
endif()

target_include_directories(modhandler-pc-tool PRIVATE include)

# --- LoRa SDK (static library for internal use, DLL for customers) ---
add_subdirectory(loralib)

target_link_libraries(modhandler-pc-tool PRIVATE lora_sdk_static)
