cmake_minimum_required(VERSION 3.20)
project(sdlgpu_posix C)
# The SDL3 GPU renderer against a real Vulkan device (Mesa lavapipe is
# enough): clears match guest RAM byte for byte, flat 2D draws match the
# software rasterizer's rules.
find_package(SDL3 REQUIRED CONFIG)
find_program(GLSLANG glslangValidator REQUIRED)
set(_src ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel/sdlgpu/shaders)
set(_out ${CMAKE_CURRENT_BINARY_DIR}/shaders)
foreach(_shader flat2d.vert flat2d.frag)
    string(REPLACE "." "_" _name ${_shader})
    add_custom_command(OUTPUT ${_out}/${_name}.h
        COMMAND ${CMAKE_COMMAND} -E make_directory ${_out}
        COMMAND ${GLSLANG} -V --target-env vulkan1.0 --vn k_sg_${_name} -o ${_out}/${_name}.h ${_src}/${_shader}
        DEPENDS ${_src}/${_shader} VERBATIM)
    list(APPEND _headers ${_out}/${_name}.h)
endforeach()
add_executable(sdlgpu_check test_main.c ${_headers})
target_include_directories(sdlgpu_check PRIVATE ../../src ${_out})
target_compile_definitions(sdlgpu_check PRIVATE SG_HAVE_SHADERS=1)
target_link_libraries(sdlgpu_check PRIVATE SDL3::SDL3 m)
target_compile_options(sdlgpu_check PRIVATE -Wall -Wextra -Wno-unused-function)
enable_testing()
add_test(NAME sdlgpu_renderer COMMAND sdlgpu_check)
set_tests_properties(sdlgpu_renderer PROPERTIES TIMEOUT 120 ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen")
