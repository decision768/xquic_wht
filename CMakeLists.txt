# Copyright (c) 2022, Alibaba Group Holding Limited

cmake_minimum_required (VERSION 3.6)
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

# set(TOOLCHAIN_DIR /opt/crosstools/toolchain/gcc/usr/bin/aarch64-oe-linux)
# set(CMAKE_C_COMPILER ${TOOLCHAIN_DIR}/aarch64-oe-linux-gcc)
# set(CMAKE_CXX_COMPILER ${TOOLCHAIN_DIR}/aarch64-oe-linux-g++)

# === SIM8810 SDK ===
set(CMAKE_SYSROOT "/opt/crosstools/sysroot")

# toolchain（跟 sdk.mk 一致）
set(TOOLCHAIN_DIR "/opt/crosstools/toolchain/gcc/usr/bin/aarch64-oe-linux")
set(CMAKE_C_COMPILER  ${TOOLCHAIN_DIR}/aarch64-oe-linux-gcc)
set(CMAKE_CXX_COMPILER ${TOOLCHAIN_DIR}/aarch64-oe-linux-g++)

# 让 cmake 查找 include/lib 时走 sysroot
set(CMAKE_FIND_ROOT_PATH ${CMAKE_SYSROOT})
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
# ====================
project (xquic)
set (xquic_VERSION_MAJOR 0)
set (xquic_VERSION_MINOR 1)
set (CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")


option (XQC_ENABLE_BBR2 "enable bbr2" OFF)
option (XQC_ENABLE_COPA "enable copa" OFF)
option (XQC_ENABLE_RENO "enable reno" OFF)
option (XQC_ENABLE_UNLIMITED "enable unlimited cc" OFF)
option (XQC_ENABLE_MP_INTEROP "enable MPQUIC interop" OFF)
option (XQC_NO_PID_PACKET_PROCESS "do not expose path_id in xqc_engine_packet_process" OFF)
option (XQC_PROTECT_POOL_MEM "enable write protection for pool memory (for debug)" OFF)
option (XQC_COMPAT_DUPLICATE "qpack compat dup" OFF)
option (XQC_ENABLE_FEC "enable fec" OFF)
option (XQC_ENABLE_XOR "enable fec scheme xor" OFF)
option (XQC_ENABLE_RSC "enable fec scheme reed-solomon code" OFF)
option (XQC_ENABLE_PKM "enable fec scheme packet mask" OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()


# SSL lib type, xquic support babassl and boringssl interfaces
# SSL lib type, xquic support babassl and boringssl interfaces
if(NOT SSL_TYPE)
    set(SSL_TYPE "babassl")
    message(NOTICE "-- SSL_TYPE not set, use default babassl")
endif()

if(${SSL_TYPE} MATCHES "babassl")
    set(SSL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/Tongsuo-8.3.0")
    set(SSL_INC_PATH "${SSL_PATH}/include")
    set(SSL_LIB_PATH
        "${SSL_PATH}/libssl.a"
        "${SSL_PATH}/libcrypto.a"
    )

elseif(${SSL_TYPE} MATCHES "boringssl")
    add_definitions(-DNOCRYPT=1)
    set(SSL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boringssl")
    set(SSL_INC_PATH "${SSL_PATH}/include")
    set(SSL_LIB_PATH
        "${SSL_PATH}/ssl/libssl.a"
        "${SSL_PATH}/crypto/libcrypto.a"
    )
endif()

if(NOT EXISTS "${SSL_INC_PATH}")
    message(FATAL_ERROR "ssl include path not found: ${SSL_INC_PATH}")
endif()

foreach(LIB IN LISTS SSL_LIB_PATH)
    if(NOT EXISTS "${LIB}")
        message(FATAL_ERROR "ssl lib not found: ${LIB}")
    endif()
endforeach()

message(STATUS "SSL_TYPE     = ${SSL_TYPE}")
message(STATUS "SSL_PATH     = ${SSL_PATH}")
message(STATUS "SSL_INC_PATH = ${SSL_INC_PATH}")
message(STATUS "SSL_LIB_PATH = ${SSL_LIB_PATH}")


# C_FLAGS
if(ANDROID_ABI OR PLATFORM MATCHES "mac")
    if(PLATFORM STREQUAL "mac32")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -m32")
    endif()
endif()

if(NOT (CMAKE_C_COMPILER_ID MATCHES "MSVC" OR CMAKE_CXX_COMPILER_ID MATCHES "MSVC"))
    set(CMAKE_C_FLAGS_OPTION "-Werror -Wno-unused -Wno-pointer-sign -Wno-format-security ")
endif()

# C_FLAGS
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -O2 -std=gnu11 -Wall ${CMAKE_C_FLAGS_OPTION} -DNDEBUG_PRINT -DNPRINT_MALLOC ")
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS} -g -O0 -std=gnu11 -Wall ${CMAKE_C_FLAGS_OPTION} -DNDEBUG_PRINT -DNPRINT_MALLOC -DXQC_DEBUG ")

if(ASAN)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -w -fsanitize=address -fsanitize-address-use-after-scope -fno-omit-frame-pointer")
endif()


if(CMAKE_SYSTEM_NAME MATCHES "Windows")
    add_definitions(-DXQC_SYS_WINDOWS=1)
    set(CMAKE_C_FLAGS_OPTION " ")
endif()

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -isystem ${SSL_INC_PATH}")

# configure file
configure_file (
    "${CMAKE_CURRENT_SOURCE_DIR}/xqc_configure.h.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/xquic/xqc_configure.h"
)

set(LIBEVENT_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/third_party/libevent_aarch64/libevent-2.1.12-stable)

set(LIBEVENT_INCLUDE_DIR ${LIBEVENT_ROOT}/include)
set(LIBEVENT_LIBRARY ${LIBEVENT_ROOT}/.libs/libevent.a)
set(LIBEVENT_PTHREADS_LIBRARY ${LIBEVENT_ROOT}/.libs/libevent_pthreads.a)





# print tls traffic secret in keylog
if(XQC_PRINT_SECRET)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DXQC_PRINT_SECRET")
endif()

# compat with the stateless reset before version v1.6.0
if(XQC_COMPAT_GENERATE_SR_PKT)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DXQC_COMPAT_GENERATE_SR_PKT")
endif()

# print only error log
if(XQC_ONLY_ERROR_LOG)
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -DXQC_ONLY_ERROR_LOG ")
endif()

# enable event log
if(XQC_ENABLE_EVENT_LOG)
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -DXQC_ENABLE_EVENT_LOG ")
endif()

# sendmmsg
if(XQC_SUPPORT_SENDMMSG_BUILD)
    add_definitions(-DXQC_SUPPORT_SENDMMSG)
endif()

# coverity
if(GCOV STREQUAL "on")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-arcs -ftest-coverage")
endif()



# testing
if(XQC_ENABLE_TESTING)
    add_subdirectory(mini)
endif(XQC_ENABLE_TESTING)


# http3/qpack source
set(
    HTTP3_SOURCES
    "src/http3/xqc_h3_conn.c"
    "src/http3/xqc_h3_stream.c"
    "src/http3/xqc_h3_request.c"
    "src/http3/frame/xqc_h3_frame.c"
    "src/http3/xqc_h3_header.c"
    "src/http3/xqc_h3_ctx.c"
    "src/http3/xqc_h3_defs.c"
    "src/http3/qpack/xqc_qpack.c"
    "src/http3/qpack/xqc_prefixed_int.c"
    "src/http3/qpack/xqc_prefixed_str.c"
    "src/http3/qpack/xqc_decoder.c"
    "src/http3/qpack/xqc_encoder.c"
    "src/http3/qpack/xqc_rep.c"
    "src/http3/qpack/xqc_ins.c"
    "src/http3/qpack/stable/xqc_stable.c"
    "src/http3/qpack/dtable/xqc_dtable.c"
    "src/http3/xqc_h3_ext_dgram.c"
    "src/http3/xqc_h3_ext_bytestream.c"
)

# Transport source
set(
    TRANSPORT_SOURCES
    "src/transport/xqc_engine.c"
    "src/transport/xqc_conn.c"
    "src/transport/xqc_client.c"
    "src/transport/xqc_cid.c"
    "src/transport/xqc_packet_parser.c"
    "src/transport/xqc_frame_parser.c"
    "src/transport/xqc_stream.c"
    "src/transport/xqc_datagram.c"
    "src/transport/xqc_packet_out.c"
    "src/transport/xqc_packet_in.c"
    "src/transport/xqc_send_ctl.c"
    "src/transport/xqc_send_queue.c"
    "src/transport/xqc_packet.c"
    "src/transport/xqc_frame.c"
    "src/transport/xqc_recv_record.c"
    "src/transport/xqc_pacing.c"
    "src/transport/xqc_utils.c"
    "src/transport/xqc_multipath.c"
    "src/transport/xqc_defs.c"
    "src/transport/xqc_transport_params.c"
    "src/transport/xqc_quic_lb.c"
    "src/transport/xqc_timer.c"
    "src/transport/xqc_reinjection.c"
    "src/transport/xqc_recv_timestamps_info.c"
    "src/transport/reinjection_control/xqc_reinj_default.c"
    "src/transport/reinjection_control/xqc_reinj_deadline.c"
    "src/transport/reinjection_control/xqc_reinj_dgram.c"
    "src/transport/scheduler/xqc_scheduler_minrtt.c"
    "src/transport/scheduler/xqc_scheduler_common.c"
    "src/transport/scheduler/xqc_scheduler_backup.c"
    "src/transport/scheduler/xqc_scheduler_backup_fec.c"
    "src/transport/scheduler/xqc_scheduler_rap.c"
    "src/transport/scheduler/xqc_scheduler_balanced.c"
    "src/transport/scheduler/xqc_scheduler_act.c"
    "src/transport/scheduler/xqc_scheduler_bandwidth.c"
)

if(XQC_ENABLE_MP_INTEROP)
    set(
        TRANSPORT_SOURCES
        ${TRANSPORT_SOURCES}
        "src/transport/scheduler/xqc_scheduler_interop.c"
    )
endif()


# fec framework
set(
    FEC_FRAMEWORK_SOURCE
    "src/transport/xqc_fec.c"
    "src/transport/xqc_fec_scheme.c"
    "src/transport/fec_schemes/xqc_galois_calculation.c"
)

if(XQC_ENABLE_XOR)
    set(
        FEC_FRAMEWORK_SOURCE
        ${FEC_FRAMEWORK_SOURCE}
        "src/transport/fec_schemes/xqc_xor.c"
    )
endif()

if(XQC_ENABLE_RSC)
    set(
        FEC_FRAMEWORK_SOURCE
        ${FEC_FRAMEWORK_SOURCE}
        "src/transport/fec_schemes/xqc_reed_solomon.c"
    )
endif()

if(XQC_ENABLE_PKM)
    set(
        FEC_FRAMEWORK_SOURCE
        ${FEC_FRAMEWORK_SOURCE}
        "src/transport/fec_schemes/xqc_packet_mask.c"
    )
endif()

if(XQC_ENABLE_FEC)
    set(
        TRANSPORT_SOURCES
        ${TRANSPORT_SOURCES}
        ${FEC_FRAMEWORK_SOURCE}
    )
endif()

# TLS source
set (
    TLS_SOURCE
    "src/tls/xqc_hkdf.c"
    "src/tls/xqc_null_crypto.c"
    "src/tls/xqc_crypto.c"
    "src/tls/xqc_tls_ctx.c"
    "src/tls/xqc_tls.c"
)

if(${SSL_TYPE} MATCHES "boringssl")
    set(
        TLS_SOURCE
        ${TLS_SOURCE}
        "src/tls/boringssl/xqc_hkdf_impl.c"
        "src/tls/boringssl/xqc_crypto_impl.c"
        "src/tls/boringssl/xqc_ssl_if_impl.c"
    )
elseif(${SSL_TYPE} MATCHES "babassl")
    set(
        TLS_SOURCE
        ${TLS_SOURCE}
        "src/tls/babassl/xqc_hkdf_impl.c"
        "src/tls/babassl/xqc_crypto_impl.c"
        "src/tls/babassl/xqc_ssl_if_impl.c"
    )
endif()

# common source
set(
    COMMON_SOURCES
    "src/common/xqc_random.c"
    "src/common/xqc_str.c"
    "src/common/xqc_log.c"
    "src/common/xqc_log_event_callback.c"
    "src/common/xqc_time.c"
    "src/common/utils/huffman/xqc_huffman_code.c"
    "src/common/utils/huffman/xqc_huffman.c"
    "src/common/utils/vint/xqc_discrete_int_parser.c"
    "src/common/utils/vint/xqc_variable_len_int.c"
    "src/common/utils/ringarray/xqc_ring_array.c"
    "src/common/utils/ringmem/xqc_ring_mem.c"
    "src/common/utils/2d_hash/xqc_2d_hash_table.c"
    "src/common/utils/var_buf/xqc_var_buf.c"

)

# congestion control
set(
    CONGESTION_CONTROL_SOURCES
    "src/congestion_control/xqc_cubic.c"
    "src/congestion_control/xqc_bbr.c"
    "src/congestion_control/xqc_window_filter.c"
    "src/congestion_control/xqc_sample.c"
)

if(XQC_ENABLE_RENO)
    set(
        CONGESTION_CONTROL_SOURCES
        ${CONGESTION_CONTROL_SOURCES}
        "src/congestion_control/xqc_new_reno.c"
    )
endif()

if(XQC_ENABLE_BBR2)
    set(
        CONGESTION_CONTROL_SOURCES
        ${CONGESTION_CONTROL_SOURCES}
        "src/congestion_control/xqc_bbr2.c"
    )
endif()

if(XQC_ENABLE_COPA)
    set(
        CONGESTION_CONTROL_SOURCES
        ${CONGESTION_CONTROL_SOURCES}
        "src/congestion_control/xqc_copa.c"
    )
endif()

if(XQC_ENABLE_UNLIMITED)
    set(
        CONGESTION_CONTROL_SOURCES
        ${CONGESTION_CONTROL_SOURCES}
        "src/congestion_control/xqc_unlimited_cc.c"
    )
endif()


# xquic source
set (
    XQC_SOURCE
    ${XQC_SOURCE}
    ${HTTP3_SOURCES}
    ${TRANSPORT_SOURCES}
    ${TLS_SOURCE}
    ${COMMON_SOURCES}
    ${CONGESTION_CONTROL_SOURCES}
)



# target
add_library(
    xquic-static
    STATIC
    ${XQC_SOURCE}
)

add_library(
    xquic
    SHARED
    ${XQC_SOURCE}
)
target_include_directories(xquic-static BEFORE PRIVATE
    ${SSL_INC_PATH}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBEVENT_INCLUDE_DIR}
)

target_include_directories(xquic BEFORE PRIVATE
    ${SSL_INC_PATH}
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${LIBEVENT_INCLUDE_DIR}
)

target_compile_options(xquic-static PRIVATE -UOPENSSL_API_COMPAT)
target_compile_options(xquic PRIVATE -UOPENSSL_API_COMPAT)

if(PLATFORM MATCHES "mac")
    target_link_libraries(
        xquic-static
        "-ldl -Wl,-all_load"
        ${SSL_LIB_PATH}
        "-Wl"
        -lpthread
        -lstdc++
        m
    )
else()
    target_link_libraries(
        xquic-static
        "-ldl -Wl,--whole-archive -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/scripts/xquic.lds"
        ${SSL_LIB_PATH}
        "-Wl,--no-whole-archive"
        -lpthread
        m
    )
endif()


# Strip binary for release builds
if(CMAKE_BUILD_TYPE STREQUAL MinSizeRel)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_STRIP} libxquic.so)
endif()

