cmake_minimum_required(VERSION 3.20)

# Xbox Kernel Replacement Layer
# Replaces 147 Xbox kernel imports with Win32 equivalents.
# Memory layout, file I/O, threading, synchronization, crypto, HAL.

set(KERNEL_SOURCES
    kernel_path.c
    kernel_pool.c
    kernel_rtl.c
    kernel_memory.c
    kernel_file.c
    kernel_overlay.c
    kernel_thread.c
    xbox_ggl.c
    kernel_sync.c
    kernel_hal.c
    kernel_xbox.c
    kernel_ob.c
    kernel_io.c
    kernel_crypto.c
    kernel_thunks.c
    kernel_bridge.c
    xbox_memory_layout.c
    icall_feedback.c
    recomp_trace.c
    nv2a_pb_scan.c
    nv2a_pb_exec.c

)

add_library(xbox_kernel STATIC ${KERNEL_SOURCES})
target_link_libraries(xbox_kernel PUBLIC SDL3::SDL3)

# See src/platform/CMakeLists.txt: resolve src/ relative to this file, not via
# CMAKE_SOURCE_DIR, which is the consuming project's root under add_subdirectory().
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

target_include_directories(xbox_kernel PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${XBOXRECOMP_SRC_DIR}
)

if(WIN32)
    # platform contributes the src/ include path and guarded guest-memory copies.
    target_link_libraries(xbox_kernel PUBLIC platform kernel32 bcrypt)
else()
    # Linux: platform layer (POSIX host primitives) + OpenSSL for crypto.
    find_package(OpenSSL REQUIRED)
    target_link_libraries(xbox_kernel PUBLIC platform OpenSSL::Crypto)

    # SDL3 GPU renderer shaders (sdlgpu/shaders): GLSL compiled to SPIR-V at
    # build time and embedded as C arrays. Without glslangValidator the
    # renderer reports itself unavailable and drawing stays in software.
    find_program(XBOXRECOMP_GLSLANG glslangValidator)
    if(XBOXRECOMP_GLSLANG)
        set(_sg_src ${CMAKE_CURRENT_SOURCE_DIR}/sdlgpu/shaders)
        set(_sg_out ${CMAKE_CURRENT_BINARY_DIR}/sdlgpu_shaders)
        set(_sg_headers)
        foreach(_sg_shader flat2d.vert flat2d.frag xf.vert xf.frag)
            string(REPLACE "." "_" _sg_name ${_sg_shader})
            add_custom_command(
                OUTPUT ${_sg_out}/${_sg_name}.h
                COMMAND ${CMAKE_COMMAND} -E make_directory ${_sg_out}
                COMMAND ${XBOXRECOMP_GLSLANG} -V --target-env vulkan1.0
                        --vn k_sg_${_sg_name} -o ${_sg_out}/${_sg_name}.h ${_sg_src}/${_sg_shader}
                DEPENDS ${_sg_src}/${_sg_shader}
                COMMENT "SPIR-V: ${_sg_shader}"
                VERBATIM)
            list(APPEND _sg_headers ${_sg_out}/${_sg_name}.h)
        endforeach()
        # xf.frag's source as well: specialised variants are compiled from
        # it at run time (sdlgpu/sdlgpu_fscache.inc.c).
        add_custom_command(
            OUTPUT ${_sg_out}/xf_frag_src.h
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_sg_out}
            COMMAND ${CMAKE_COMMAND} -DIN=${_sg_src}/xf.frag -DOUT=${_sg_out}/xf_frag_src.h
                    -DNAME=k_sg_xf_frag_src -P ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/embed_text.cmake
            DEPENDS ${_sg_src}/xf.frag ${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/embed_text.cmake
            COMMENT "embed: xf.frag source"
            VERBATIM)
        list(APPEND _sg_headers ${_sg_out}/xf_frag_src.h)
        add_custom_target(xbox_sdlgpu_shaders DEPENDS ${_sg_headers})
        add_dependencies(xbox_kernel xbox_sdlgpu_shaders)
        target_include_directories(xbox_kernel PRIVATE ${_sg_out})
        target_compile_definitions(xbox_kernel PRIVATE SG_HAVE_SHADERS=1)
    else()
        message(STATUS "glslangValidator not found: the SDL3 GPU renderer is unavailable (install glslang)")
    endif()
        # Runtime shader compilation for vertex programs translated on the fly
    # (sdlgpu/sdlgpu_compile.inc.c): glslang's C interface as a library.
    find_package(glslang CONFIG QUIET)
    if(glslang_FOUND)
        target_link_libraries(xbox_kernel PRIVATE
                glslang::glslang glslang::glslang-default-resource-limits)
        target_compile_definitions(xbox_kernel PRIVATE SG_HAVE_GLSLANG=1)
    else()
        message(STATUS "glslang library not found: vertex programs stay on the CPU")
    endif()

    # SPIRV-Cross: the Metal backend (macOS) takes MSL, not SPIR-V. The
    # renderer cross-compiles its SPIR-V at run time (sdlgpu/sdlgpu_msl.inc.c).
    # Each component ships its own config, none pulls in the others, and
    # spirv_cross_c refuses to load until the targets it names exist: find
    # them first, in dependency order.
    foreach(_spvc core glsl hlsl msl cpp reflect c)
        find_package(spirv_cross_${_spvc} CONFIG QUIET)
    endforeach()
    if(spirv_cross_c_FOUND)
        target_link_libraries(xbox_kernel PRIVATE spirv-cross-c)
        if(APPLE)
            target_link_libraries(xbox_kernel PRIVATE c++)   # static C++ library from a C target
        else()
            target_link_libraries(xbox_kernel PRIVATE stdc++)
        endif()
        target_compile_definitions(xbox_kernel PRIVATE SG_HAVE_SPIRV_CROSS=1)
        message(STATUS "SPIRV-Cross found: SDL3 GPU renderer can use Metal (MSL)")
    elseif(APPLE)
        message(STATUS "SPIRV-Cross not found: the SDL3 GPU renderer is unavailable on Metal (brew install spirv-cross)")
    endif()
endif()

if(MSVC)
    # /Zi: private symbols, so profiles and crash reports name static
    # functions instead of the nearest public one.
    target_compile_options(xbox_kernel PRIVATE /W4 /WX- /O2 /GS- /Gy /Zi /FS)
    target_compile_definitions(xbox_kernel PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(xbox_kernel PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

target_compile_definitions(xbox_kernel PRIVATE $<$<CONFIG:Debug>:_DEBUG>)

# PUBLIC: icall_feedback.c's g_icall_seen array and the dump/init functions
# are compiled under this guard. Anything linking xbox_kernel (the generated
# recompiled code included) reads/writes g_icall_seen through the
# RECOMP_ICALL_OBSERVE macro in recomp_icall_feedback.h, so the define has to
# be visible on both sides of that link, not just inside this library.
# Matches config/game.cmake, which defines the same macro for the NFL2K5
# target itself.
target_compile_definitions(xbox_kernel PUBLIC RECOMP_ICALL_FEEDBACK)
