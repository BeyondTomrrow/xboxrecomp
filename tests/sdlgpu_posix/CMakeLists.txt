cmake_minimum_required(VERSION 3.20)
project(sdlgpu_posix C)
# The SDL3 GPU renderer against a real Vulkan device (Mesa lavapipe is
# enough): GPU clears and downloads must match guest RAM byte for byte.
find_package(SDL3 REQUIRED CONFIG)
add_executable(sdlgpu_check test_main.c)
target_include_directories(sdlgpu_check PRIVATE ../../src)
target_link_libraries(sdlgpu_check PRIVATE SDL3::SDL3)
target_compile_options(sdlgpu_check PRIVATE -Wall -Wextra -Wno-unused-function)
enable_testing()
add_test(NAME sdlgpu_clear_download COMMAND sdlgpu_check)
set_tests_properties(sdlgpu_clear_download PROPERTIES TIMEOUT 60
                     ENVIRONMENT "SDL_VIDEO_DRIVER=offscreen")
