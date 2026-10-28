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
