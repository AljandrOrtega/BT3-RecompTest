# Cross-compile toolchain for the Windows release flow (tools/release-windows/).
#
# clang-cl -> x86_64-pc-windows-msvc against the CRT/SDK materialised by xwin,
# linked with lld-link (the splat provides no link.exe). Native builds on
# Linux/macOS keep their default host toolchain; this file is only given to CMake
# with -DCMAKE_TOOLCHAIN_FILE.
#
# Assumes the xwin splat at $XWIN_ROOT (default /opt/xwin, the image layout):
#   ${XWIN_ROOT}/crt/include                      CRT headers
#   ${XWIN_ROOT}/crt/lib/x86_64                   CRT import libs (ucrt/vcruntime)
#   ${XWIN_ROOT}/sdk/include/{shared,ucrt,um}     Windows SDK headers
#   ${XWIN_ROOT}/sdk/lib/{ucrt,um}/x86_64         Windows SDK import libs
# The exact same dirs are fed to clang-cl via -imsvc (headers) and to lld-link
# through the LIB environment variable (libs) exported by entrypoint.sh.

if(NOT DEFINED XWIN_ROOT)
    set(XWIN_ROOT "$ENV{XWIN_ROOT}")
endif()
if(NOT XWIN_ROOT)
    message(FATAL_ERROR "XWIN_ROOT must point at the xwin splat output (export XWIN_ROOT=... or pass -DXWIN_ROOT=...)")
endif()

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_SYSTEM_VERSION 10.0)

# clang-cl on a POSIX host still needs the explicit MSVC triple.
set(CMAKE_C_COMPILER clang-cl)
set(CMAKE_CXX_COMPILER clang-cl)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Headers: CRT + SDK. -imsvc keeps these out of CMake's -I bookkeeping and is
# the exact mechanism clang-cl's MSVC mode uses for system includes.
set(_xwin_arch x86_64)
set(_xwin_includes
    "${XWIN_ROOT}/sdk/include/shared"
    "${XWIN_ROOT}/sdk/include/ucrt"
    "${XWIN_ROOT}/sdk/include/um"
    "${XWIN_ROOT}/crt/include"
)
set(_xwin_imsvc "")
foreach(_d IN LISTS _xwin_includes)
    string(APPEND _xwin_imsvc " -imsvc ${_d}")
endforeach()
set(CMAKE_C_FLAGS_INIT "--target=x86_64-pc-windows-msvc${_xwin_imsvc}")
set(CMAKE_CXX_FLAGS_INIT "--target=x86_64-pc-windows-msvc${_xwin_imsvc}")

# Link with lld-link instead of link.exe (xwin ships no MSVC linker).
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")

# Where find_library/find_path look for import libs/headers such as opengl32,
# winmm or shlwapi (required by raylib/GLFW/Qt beyond the LIB env the driver
# feeds to lld-link).
set(CMAKE_LIBRARY_PATH
    "${XWIN_ROOT}/sdk/lib/um/${_xwin_arch}"
    "${XWIN_ROOT}/sdk/lib/ucrt/${_xwin_arch}"
    "${XWIN_ROOT}/crt/lib/${_xwin_arch}"
)
set(CMAKE_INCLUDE_PATH ${_xwin_includes})

# Do not let find_program reach into the splat; everything else stays bounded to
# it so a native Linux library can never be picked for the Windows tree.
set(CMAKE_FIND_ROOT_PATH "${XWIN_ROOT}")
# The Qt MSVC kit (aqtinstall) is a second provider of Windows-built artefact
# packages; feed its root in with -DPS2X_CMAKE_EXTRA_ROOTS=... so find_package
# (Qt6) with the ONLY modes below can still see it.
if(DEFINED PS2X_CMAKE_EXTRA_ROOTS)
    list(APPEND CMAKE_FIND_ROOT_PATH ${PS2X_CMAKE_EXTRA_ROOTS})
endif()
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# launcher/CMakeLists and ps2xRuntime guard on MSVC for flags; clang-cl is
# reported as MSVC by CMake, which is exactly what we want here.