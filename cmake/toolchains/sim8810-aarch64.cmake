# SIM8810 Linux AArch64 cross-build profile.
#
# Usage:
#   cmake -S . -B build-aarch64 \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/sim8810-aarch64.cmake
#
# XQC_SIM8810_SDK_ROOT and XQC_SIM8810_TOOLCHAIN_DIR are CACHE paths and can
# be overridden on the command line without editing this file.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(XQC_SIM8810_SDK_ROOT
    "/home/admin123/SIM8810_SDK_OCPU_LE3.0_1.1.0_251207"
    CACHE PATH "SIM8810 SDK root")
set(XQC_SIM8810_TOOLCHAIN_DIR
    "/opt/crosstools/toolchain/gcc/usr/bin/aarch64-oe-linux"
    CACHE PATH
    "directory containing aarch64-oe-linux-gcc and aarch64-oe-linux-g++")

set(CMAKE_SYSROOT "${XQC_SIM8810_SDK_ROOT}/sysroots_pvm"
    CACHE PATH "SIM8810 target sysroot")
set(CMAKE_C_COMPILER
    "${XQC_SIM8810_TOOLCHAIN_DIR}/aarch64-oe-linux-gcc"
    CACHE FILEPATH "SIM8810 C compiler")
set(CMAKE_CXX_COMPILER
    "${XQC_SIM8810_TOOLCHAIN_DIR}/aarch64-oe-linux-g++"
    CACHE FILEPATH "SIM8810 C++ compiler")

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}" CACHE STRING
    "roots used to locate SIM8810 target headers and libraries")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(SSL_TYPE "babassl" CACHE STRING "TLS implementation")
set(LIBEVENT_ROOT
    "/home/admin123/libevent_aarch64/libevent-2.1.12-stable"
    CACHE PATH "SIM8810 AArch64 libevent root")
set(ZLOG_INCLUDE_DIR "/home/admin123/zlog-1.2.18/src" CACHE PATH
    "SIM8810 AArch64 zlog include directory")
set(ZLOG_STATIC_LIB
    "/home/admin123/zlog-1.2.18/build_aarch64/lib/libzlog.a"
    CACHE FILEPATH "SIM8810 AArch64 zlog static library")
