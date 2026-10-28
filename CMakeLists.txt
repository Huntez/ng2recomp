# ng2 - ReXGlue Recompiled Project
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(ng2 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(NG2_SOURCES
    src/main.cpp
    src/gpu/video.cpp
    src/gpu/d3d_hooks.cpp
    src/movie/movie_hooks.cpp
    src/movie/host_movie.cpp
    src/debug_waits.cpp
)

if(WIN32)
    add_executable(ng2 WIN32 ${NG2_SOURCES})
else()
    add_executable(ng2 ${NG2_SOURCES})
endif()

# xenos (GPU emulation) is the reference renderer while the native one is built.
rexglue_setup_target(ng2 GPU_PLUGINS xenos)

# The SDK sets an $ORIGIN rpath on Linux but only stages its shared runtime
# libraries on Windows and macOS.
if(UNIX AND NOT APPLE)
    foreach(_lib rex::runtime rex::TracyClient)
        if(TARGET ${_lib})
            add_custom_command(TARGET ng2 POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:${_lib}> $<TARGET_FILE_DIR:ng2>
                VERBATIM)
        endif()
    endforeach()
endif()

# Shaders, translated ahead of time like Unleashed Recompiled does: the
# shader containers sit in the XEX image, ng2_dump_image extracts that image
# from the user's default.xex and XenosRecomp (patched for NG2, see
# docs/shaders.md) turns every container into SPIR-V, embedded as a zstd
# compressed cache.
set(NG2_GAME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/game_files" CACHE PATH
    "Extracted game files (default.xex is read at build time for its shaders)")
set(NG2_XENOS_RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/thirdparty/XenosRecomp")

add_executable(ng2_dump_image tools/dump_image/main.cpp)
target_link_libraries(ng2_dump_image PRIVATE rex::runtime)

include(ExternalProject)
ExternalProject_Add(xenos_recomp
    SOURCE_DIR "${NG2_XENOS_RECOMP_DIR}"
    BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/xenos_recomp"
    CMAKE_ARGS
        -G Ninja
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -DXENOS_RECOMP_DXIL=OFF
    BUILD_BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/xenos_recomp/XenosRecomp/XenosRecomp"
    INSTALL_COMMAND ""
    BUILD_ALWAYS ON)

set(NG2_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
set(NG2_SHADER_CACHE "${NG2_SHADER_DIR}/ng2_shader_cache.cpp")
add_custom_command(
    OUTPUT "${NG2_SHADER_CACHE}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${NG2_SHADER_DIR}/input"
    COMMAND ng2_dump_image "${NG2_GAME_DIR}" default.xex "${NG2_SHADER_DIR}/input/default.img"
    COMMAND "${CMAKE_CURRENT_BINARY_DIR}/xenos_recomp/XenosRecomp/XenosRecomp"
        "${NG2_SHADER_DIR}/input" "${NG2_SHADER_CACHE}"
        "${NG2_XENOS_RECOMP_DIR}/XenosRecomp/shader_common.h"
    DEPENDS ng2_dump_image xenos_recomp "${NG2_GAME_DIR}/default.xex"
        "${CMAKE_CURRENT_BINARY_DIR}/xenos_recomp/XenosRecomp/XenosRecomp"
        "${NG2_XENOS_RECOMP_DIR}/XenosRecomp/shader_common.h"
    COMMENT "Translating NG2 shaders with XenosRecomp"
    VERBATIM)

add_library(ng2_shaders STATIC "${NG2_SHADER_CACHE}"
    "${NG2_XENOS_RECOMP_DIR}/thirdparty/smol-v/source/smolv.cpp")
target_include_directories(ng2_shaders PUBLIC src/gpu
    "${NG2_XENOS_RECOMP_DIR}/thirdparty/smol-v/source")
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)
target_link_libraries(ng2_shaders PUBLIC PkgConfig::ZSTD)
target_link_libraries(ng2 PRIVATE ng2_shaders)

# Movies decoded on the host (src/movie): the game's WMV3/VC-1 and WMA files.
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavformat libavcodec libavutil libswresample libswscale)
target_link_libraries(ng2 PRIVATE PkgConfig::FFMPEG)

# Native renderer: plume (Unleashed Recompiled's RHI), Vulkan only, with the
# Wayland surface support from patches/plume.
add_subdirectory(thirdparty/plume EXCLUDE_FROM_ALL)
if(UNIX AND NOT APPLE)
    find_package(X11 REQUIRED)
    target_link_libraries(plume PUBLIC X11::X11)
endif()
target_link_libraries(ng2 PRIVATE plume)

# Internal shaders (present, copies), HLSL compiled to SPIR-V with the DXC
# that ships with XenosRecomp and embedded in the executable.
set(NG2_DXC_DIR "${NG2_XENOS_RECOMP_DIR}/thirdparty/dxc-bin")
set(NG2_INTERNAL_SHADERS
    "fullscreen_vs:vs_6_0"
    "present_ps:ps_6_0"
    "rect_list_gs:gs_6_0"
    "copy_ps:ps_6_0"
    "depth_copy_ps:ps_6_0"
    "depth_copy_ms_ps:ps_6_0"
    "debug_ps:ps_6_0"
)
set(NG2_INTERNAL_SHADER_SOURCES "")
foreach(_entry ${NG2_INTERNAL_SHADERS})
    string(REPLACE ":" ";" _parts "${_entry}")
    list(GET _parts 0 _name)
    list(GET _parts 1 _profile)
    set(_hlsl "${CMAKE_CURRENT_SOURCE_DIR}/src/gpu/shaders/${_name}.hlsl")
    set(_spv "${NG2_SHADER_DIR}/internal/${_name}.spv")
    set(_cpp "${NG2_SHADER_DIR}/internal/${_name}.cpp")
    add_custom_command(
        OUTPUT "${_cpp}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${NG2_SHADER_DIR}/internal"
        COMMAND ${CMAKE_COMMAND} -E env "LD_LIBRARY_PATH=${NG2_DXC_DIR}/lib/x64"
            "${NG2_DXC_DIR}/bin/x64/dxc-linux" -T ${_profile} -E main -spirv
            -fvk-use-dx-layout -HV 2021 -Qstrip_debug -Fo "${_spv}" "${_hlsl}"
        COMMAND ${CMAKE_COMMAND} -DINPUT=${_spv} -DOUTPUT=${_cpp} -DNAME=g_${_name}
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake"
        DEPENDS "${_hlsl}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_spirv.cmake"
        COMMENT "Compiling ${_name}.hlsl"
        VERBATIM)
    list(APPEND NG2_INTERNAL_SHADER_SOURCES "${_cpp}")
endforeach()
target_sources(ng2_shaders PRIVATE ${NG2_INTERNAL_SHADER_SOURCES})
