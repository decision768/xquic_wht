set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# CMake reloads this toolchain file in the temporary projects created by
# try_compile().  Explicitly propagate the two custom inputs so compiler ABI
# detection uses the same SDK and compiler paths as the parent project.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
    SIM8810_SDK_ROOT
    SIM8810_TOOLCHAIN_DIR)
list(REMOVE_DUPLICATES CMAKE_TRY_COMPILE_PLATFORM_VARIABLES)

if(NOT SIM8810_SDK_ROOT AND DEFINED ENV{SIM8810_SDK_ROOT})
    set(SIM8810_SDK_ROOT "$ENV{SIM8810_SDK_ROOT}")
endif()
if(NOT SIM8810_TOOLCHAIN_DIR AND DEFINED ENV{SIM8810_TOOLCHAIN_DIR})
    set(SIM8810_TOOLCHAIN_DIR "$ENV{SIM8810_TOOLCHAIN_DIR}")
endif()

if(NOT SIM8810_SDK_ROOT)
    message(FATAL_ERROR
        "Set SIM8810_SDK_ROOT to the SDK directory containing sysroots_pvm")
endif()
if(NOT SIM8810_TOOLCHAIN_DIR)
    message(FATAL_ERROR
        "Set SIM8810_TOOLCHAIN_DIR to the directory containing "
        "aarch64-oe-linux-gcc and aarch64-oe-linux-g++")
endif()

set(SIM8810_SDK_ROOT "${SIM8810_SDK_ROOT}" CACHE PATH
    "SIM8810 SDK directory containing sysroots_pvm")
set(SIM8810_TOOLCHAIN_DIR "${SIM8810_TOOLCHAIN_DIR}" CACHE PATH
    "Directory containing the SIM8810 AArch64 compiler")

set(CMAKE_SYSROOT "${SIM8810_SDK_ROOT}/sysroots_pvm")
set(CMAKE_C_COMPILER
    "${SIM8810_TOOLCHAIN_DIR}/aarch64-oe-linux-gcc")
set(CMAKE_CXX_COMPILER
    "${SIM8810_TOOLCHAIN_DIR}/aarch64-oe-linux-g++")

if(NOT EXISTS "${CMAKE_SYSROOT}")
    message(FATAL_ERROR "SIM8810 sysroot not found: ${CMAKE_SYSROOT}")
endif()
if(NOT EXISTS "${CMAKE_C_COMPILER}")
    message(FATAL_ERROR "SIM8810 C compiler not found: ${CMAKE_C_COMPILER}")
endif()

set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
