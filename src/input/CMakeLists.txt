# Xbox Input → XInput Compatibility Layer

# Xbox Input  Unified SDL3 Gamepad Layer
add_library(xbox_input STATIC
        xinput_device.c
)

target_include_directories(xbox_input PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(SDL3 REQUIRED)
target_link_libraries(xbox_input PUBLIC platform SDL3::SDL3)

if(MSVC)
    target_compile_options(xbox_input PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_input PRIVATE -Wall -Wextra -O2)
endif()