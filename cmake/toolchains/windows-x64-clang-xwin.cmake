# Cross-compile Ninja Gaiden II for Windows x86-64 from Linux: clang (its GNU-style
# driver, the MSVC ABI, as the ReXGlue SDK builds on Windows) and lld,
# with Microsoft's C runtime and Windows SDK from xwin
# (`xwin --accept-license splat --output ~/.xwin`; the license is the user's
# to accept). Used by packaging/windows/build.sh.
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_VERSION 10.0)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(XWIN_ROOT "$ENV{HOME}/.xwin" CACHE PATH "xwin's splatted MSVC CRT and Windows SDK")
if(NOT EXISTS "${XWIN_ROOT}/crt/include" OR NOT EXISTS "${XWIN_ROOT}/sdk/include/um")
    message(FATAL_ERROR "No MSVC CRT and Windows SDK in ${XWIN_ROOT} (xwin --accept-license splat --output ${XWIN_ROOT})")
endif()

set(_triple x86_64-pc-windows-msvc)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET ${_triple})
set(CMAKE_CXX_COMPILER_TARGET ${_triple})
set(CMAKE_RC_COMPILER llvm-rc)
set(CMAKE_MT llvm-mt)
set(CMAKE_AR llvm-ar)  # CMake archives with ar's commands for a GNU-style driver
set(CMAKE_LINKER lld-link)
# The release DLL runtime (msvcrt, vcruntime140, msvcp140) in every
# configuration, as on Windows: xwin's CRT has no debug libraries, and the SDK's
# libraries use the same runtime.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
set(CMAKE_TRY_COMPILE_CONFIGURATION Release)

# Headers the code spells in another case than the SDK files (Windows
# doesn't care, Linux does): aliases in a directory of their own.
set(_case_dir "${CMAKE_CURRENT_LIST_DIR}/../../out/windows-deps/include-case")
file(MAKE_DIRECTORY "${_case_dir}")
foreach(_alias "ObjBase.h=um/objbase.h" "SDKDDKVer.h=shared/sdkddkver.h")
    string(REPLACE "=" ";" _pair "${_alias}")
    list(GET _pair 0 _name)
    list(GET _pair 1 _target)
    if(NOT EXISTS "${_case_dir}/${_name}")
        file(CREATE_LINK "${XWIN_ROOT}/sdk/include/${_target}" "${_case_dir}/${_name}" SYMBOLIC)
    endif()
endforeach()

set(_xwin_includes
    "${_case_dir}"
    "${XWIN_ROOT}/crt/include"
    "${XWIN_ROOT}/sdk/include/ucrt"
    "${XWIN_ROOT}/sdk/include/um"
    "${XWIN_ROOT}/sdk/include/shared"
    "${XWIN_ROOT}/sdk/include/winrt")
# The SDK's headers as Visual Studio's INCLUDE puts them: after clang's own
# (-idirafter), whose intrinsics headers must win (with the SDK's first, SSE
# intrinsics became calls to functions that do not exist). Extra flags for the
# build (-march, PGO, ...) come in NG2_CROSS_FLAGS: a CMAKE_C_FLAGS given
# on the command line would replace these.
set(NG2_CROSS_FLAGS "-march=x86-64-v2" CACHE STRING "Extra C/C++ flags for the Windows cross build")
# xwin's STL (VS 17.10 and later) makes std::mutex's constructor constexpr,
# which crashes in mutex::lock with an older msvcp140.dll: off, so any
# 2015-2022 runtime works.
string(APPEND NG2_CROSS_FLAGS " -D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR")
set(_flags "${NG2_CROSS_FLAGS}")
foreach(_dir IN LISTS _xwin_includes)
    string(APPEND _flags " -idirafter \"${_dir}\"")
endforeach()
set(CMAKE_C_FLAGS_INIT "${_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_flags}")
set(_xwin_libs "${XWIN_ROOT}/crt/lib/x86_64" "${XWIN_ROOT}/sdk/lib/um/x86_64" "${XWIN_ROOT}/sdk/lib/ucrt/x86_64")
set(CMAKE_C_STANDARD_LINK_DIRECTORIES ${_xwin_libs})
set(CMAKE_CXX_STANDARD_LINK_DIRECTORIES ${_xwin_libs})
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
# The resource compiler's preprocessor (clang) finds windows.h for the version
# resource here.
set(_rc "")
foreach(_dir IN LISTS _xwin_includes)
    string(APPEND _rc " -I \"${_dir}\"")
endforeach()
set(CMAKE_RC_FLAGS_INIT "${_rc}")

# Nothing of this Linux system's: headers, libraries and CMake packages come
# from the Windows side only.
# The Windows build of the ReXGlue SDK (its install prefix) is searched too.
set(NG2_FIND_ROOTS "${CMAKE_CURRENT_LIST_DIR}/../../thirdparty/rexglue-sdk/out/install/windows-x64"
    CACHE STRING "Extra Windows install prefixes to search")
set(CMAKE_FIND_ROOT_PATH "${XWIN_ROOT}" ${NG2_FIND_ROOTS})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(PKG_CONFIG_EXECUTABLE "" CACHE FILEPATH "" FORCE)
