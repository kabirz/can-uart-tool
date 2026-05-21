cmake_minimum_required(VERSION 3.15)

set(LORA_SDK_SOURCES
    lora_sdk.c
    lora_sdk_net.c
    lora_sdk_tcp.c
    lora_sdk_udp.c
    lora_sdk_serial.c
    lora_sdk_at.c
    crc16.c
    cJSON.c
)

# --- Static library (for internal use) ---
add_library(lora_sdk_static STATIC ${LORA_SDK_SOURCES})

target_compile_definitions(lora_sdk_static PUBLIC
    LORA_SDK_STATIC
    _CRT_SECURE_NO_WARNINGS
)

target_include_directories(lora_sdk_static PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(lora_sdk_static PRIVATE
    ws2_32
    iphlpapi
)

# Set output name to "lora_sdk" (produces liblora_sdk.a on MinGW)
set_target_properties(lora_sdk_static PROPERTIES
    OUTPUT_NAME "lora_sdk"
)

if(MSVC)
    target_compile_options(lora_sdk_static PRIVATE /W3 /utf-8)
endif()

# --- Shared library / DLL (for customer distribution) ---
add_library(lora_sdk_shared SHARED ${LORA_SDK_SOURCES})

target_compile_definitions(lora_sdk_shared PRIVATE
    LORA_SDK_EXPORTS
    _CRT_SECURE_NO_WARNINGS
)

target_include_directories(lora_sdk_shared PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(lora_sdk_shared PRIVATE
    ws2_32
    iphlpapi
)

set_target_properties(lora_sdk_shared PROPERTIES
    OUTPUT_NAME "lora_gateway_sdk"
)

if(MINGW)
    # Enable static linking of gcc runtime for clean DLL
    target_link_options(lora_sdk_shared PRIVATE
        -static-libgcc -s
    )
elseif(MSVC)
    target_compile_options(lora_sdk_shared PRIVATE /W3 /utf-8)
endif()

# --- Example program (links against shared DLL) ---
add_executable(lora_sdk_example lora_sdk_example.c)
target_link_libraries(lora_sdk_example PRIVATE lora_sdk_shared)
target_compile_definitions(lora_sdk_example PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MINGW)
    target_link_options(lora_sdk_example PRIVATE -static-libgcc -s)
elseif(MSVC)
    target_compile_options(lora_sdk_example PRIVATE /utf-8)
endif()
