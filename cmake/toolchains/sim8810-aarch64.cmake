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

set(XQC_SIM8810_SDK_ROOT "" CACHE PATH "SIM8810 SDK root")
set(XQC_SIM8810_TOOLCHAIN_DIR "" CACHE PATH
    "directory containing aarch64-oe-linux-gcc and aarch64-oe-linux-g++")

if(NOT IS_DIRECTORY "${XQC_SIM8810_SDK_ROOT}/sysroots_pvm")
    message(FATAL_ERROR
        "SIM8810 sysroot not found. Set XQC_SIM8810_SDK_ROOT to the SDK root.")
endif()
if(NOT EXISTS
        "${XQC_SIM8810_TOOLCHAIN_DIR}/aarch64-oe-linux-gcc"
    OR NOT EXISTS
        "${XQC_SIM8810_TOOLCHAIN_DIR}/aarch64-oe-linux-g++")
    message(FATAL_ERROR
        "SIM8810 compiler not found. Set XQC_SIM8810_TOOLCHAIN_DIR to the "
        "directory containing aarch64-oe-linux-gcc and "
        "aarch64-oe-linux-g++.")
endif()

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
