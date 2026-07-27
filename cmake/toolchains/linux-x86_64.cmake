# Native Linux x86_64 build profile.
#
# Usage:
#   cmake -S . -B build-linux-x86_64 \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/linux-x86_64.cmake
#
# Override any CACHE path below with an additional -D option when the
# dependency is installed elsewhere.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(XQC_USE_SIM8810_TOOLCHAIN OFF CACHE BOOL
    "do not use the SIM8810 cross compiler for native x86_64")
set(SSL_TYPE "babassl" CACHE STRING "TLS implementation")
set(SSL_PATH "/usr/local/babassl" CACHE PATH
    "native Linux x86_64 BabaSSL/Tongsuo root")
set(LIBEVENT_ROOT "/data/quic/libevent-2.1.12-stable" CACHE PATH
    "native Linux x86_64 libevent root")
set(ZLOG_INCLUDE_DIR "/data/quic/zlog-1.2.18/src" CACHE PATH
    "native Linux x86_64 zlog include directory")
set(ZLOG_STATIC_LIB
    "/data/quic/zlog-1.2.18/build_x86_64/lib/libzlog.a"
    CACHE FILEPATH "native Linux x86_64 zlog static library")
