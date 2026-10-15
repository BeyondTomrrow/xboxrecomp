cmake_minimum_required(VERSION 3.20)
project(apu_mmio_decode_posix C)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
  message(FATAL_ERROR "This regression decodes real Linux x86-64 SIGSEGV contexts")
endif()
# Each instruction runs on plain memory and on a PROT_NONE page whose faults go
# through apu_hook_handle_mmio; registers, flags and memory must match the CPU.
add_executable(apu_mmio_decode_check test_main.c ../../src/apu/apu_mmio_hook.c)
target_include_directories(apu_mmio_decode_check PRIVATE ../../src ../../src/apu ../../src/platform ../../include)
target_compile_definitions(apu_mmio_decode_check PRIVATE _GNU_SOURCE)
target_compile_options(apu_mmio_decode_check PRIVATE -O1 -w)
enable_testing()
add_test(NAME apu_mmio_decode COMMAND apu_mmio_decode_check)
set_tests_properties(apu_mmio_decode PROPERTIES TIMEOUT 30)
