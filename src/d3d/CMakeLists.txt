# Xbox D3D8 Compatibility Layer
#   _WIN32 -> Direct3D 11 + SDL3 GPU (selectable at runtime via RECOMP_RENDERER)
#   POSIX  -> SDL3 GPU (Vulkan / Metal)

find_package(SDL3 REQUIRED)

if(WIN32)
    add_library(xbox_d3d8 STATIC
            d3d8_device.c
            d3d8_gamma.c
            d3d8_resources.c
            d3d8_shaders.c
            d3d8_states.c
            d3d8_combiners.c
            d3d8_vsh.c
            d3d8_sdl_gpu.c
    )
    target_compile_definitions(xbox_d3d8 PRIVATE D3D8_RUNTIME_DISPATCH=1)
    target_link_libraries(xbox_d3d8 PUBLIC platform SDL3::SDL3 d3d11 dxgi dxguid d3dcompiler)

    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../../tests/d3d8_smoke
            ${CMAKE_CURRENT_BINARY_DIR}/d3d8_smoke)
else()
    add_library(xbox_d3d8 STATIC
            d3d8_sdl_gpu.c
    )
    target_link_libraries(xbox_d3d8 PUBLIC platform SDL3::SDL3)
endif()

target_include_directories(xbox_d3d8 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(xbox_d3d8 PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_d3d8 PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()