# Native Linux x86_64 build profile.
#
# Usage:
#   cmake -S . -B build-linux-x86_64 \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-x86_64.cmake
#
# Override any CACHE path below with an additional -D option when the
# dependency is installed elsewhere.

# Do not set CMAKE_SYSTEM_NAME here: this is a native profile, and forcing it
# would make CMake report CMAKE_CROSSCOMPILING even on an x86_64 Linux host.
string(TOLOWER "${CMAKE_HOST_SYSTEM_NAME}" _xqc_host_system_name)
string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" _xqc_host_processor)
if(NOT _xqc_host_system_name STREQUAL "linux"
    OR NOT _xqc_host_processor MATCHES "^(x86_64|amd64)$")
    message(FATAL_ERROR
        "linux-x86_64.cmake requires a native Linux x86_64 host; "
        "detected ${CMAKE_HOST_SYSTEM_NAME}/${CMAKE_HOST_SYSTEM_PROCESSOR}")
endif()
unset(_xqc_host_system_name)
unset(_xqc_host_processor)

set(SSL_TYPE "babassl" CACHE STRING "TLS implementation")
set(SSL_PATH "/usr/local/babassl" CACHE PATH
    "native Linux x86_64 BabaSSL/Tongsuo root")
