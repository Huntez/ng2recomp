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

add_library(ng2_shaders STATIC "${NG2_SHADER_CACHE}")
target_include_directories(ng2_shaders PUBLIC src/gpu)
target_link_libraries(ng2 PRIVATE ng2_shaders)
