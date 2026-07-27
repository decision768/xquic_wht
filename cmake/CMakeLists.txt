# Copyright (c) 2022, Alibaba Group Holding Limited

cmake_minimum_required (VERSION 3.6)

# Native Linux x86_64 is the normal host build. When the historical SIM8810
# SDK is present, this entry point can also select its AArch64 cross compiler.
# An explicitly supplied CMAKE_TOOLCHAIN_FILE always remains authoritative.
set(XQC_SIM8810_SDK_ROOT
    "/home/admin123/SIM8810_SDK_OCPU_LE3.0_1.1.0_251207"
    CACHE PATH "SIM8810 SDK root")
set(XQC_SIM8810_TOOLCHAIN_DIR
    "/opt/crosstools/toolchain/gcc/usr/bin/aarch64-oe-linux"
    CACHE PATH
    "directory containing aarch64-oe-linux-gcc and aarch64-oe-linux-g++")
set(_xqc_sim8810_default OFF)
if((NOT DEFINED CMAKE_TOOLCHAIN_FILE OR CMAKE_TOOLCHAIN_FILE STREQUAL "")
    AND EXISTS "${XQC_SIM8810_SDK_ROOT}/sysroots_pvm"
    AND EXISTS "${XQC_SIM8810_TOOLCHAIN_DIR}/aarch64-oe-linux-gcc"
    AND EXISTS "${XQC_SIM8810_TOOLCHAIN_DIR}/aarch64-oe-linux-g++")
    set(_xqc_sim8810_default ON)
endif()
option(XQC_USE_SIM8810_TOOLCHAIN
    "build with the SIM8810 AArch64 SDK toolchain"
    ${_xqc_sim8810_default})
unset(_xqc_sim8810_default)

if(XQC_USE_SIM8810_TOOLCHAIN)
    if(DEFINED CMAKE_TOOLCHAIN_FILE
        AND NOT CMAKE_TOOLCHAIN_FILE STREQUAL "")
        message(FATAL_ERROR
            "choose either XQC_USE_SIM8810_TOOLCHAIN or "
            "CMAKE_TOOLCHAIN_FILE, not both")
    endif()
    if(NOT EXISTS "${XQC_SIM8810_SDK_ROOT}/sysroots_pvm"
        OR NOT EXISTS
            "${XQC_SIM8810_TOOLCHAIN_DIR}/aarch64-oe-linux-gcc"
        OR NOT EXISTS
            "${XQC_SIM8810_TOOLCHAIN_DIR}/aarch64-oe-linux-g++")
        message(FATAL_ERROR
            "XQC_USE_SIM8810_TOOLCHAIN is ON, but the configured SDK or "
            "compiler does not exist")
    endif()
    set(CMAKE_SYSTEM_NAME Linux)
    set(CMAKE_SYSTEM_PROCESSOR aarch64)
    set(CMAKE_SYSROOT "${XQC_SIM8810_SDK_ROOT}/sysroots_pvm")
    set(CMAKE_C_COMPILER
        "${XQC_SIM8810_TOOLCHAIN_DIR}/aarch64-oe-linux-gcc")
    set(CMAKE_CXX_COMPILER
        "${XQC_SIM8810_TOOLCHAIN_DIR}/aarch64-oe-linux-g++")
    set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
    set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
    set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
    set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
endif()

project (xquic)

set (xquic_VERSION_MAJOR 0)
set (xquic_VERSION_MINOR 1)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _xqc_system_processor)
    if(_xqc_system_processor MATCHES "^(aarch64|arm64)$")
        set(XQC_LINUX_ARCH "aarch64")
    elseif(_xqc_system_processor MATCHES "^(x86_64|amd64)$")
        set(XQC_LINUX_ARCH "x86_64")
    else()
        set(XQC_LINUX_ARCH "${_xqc_system_processor}")
    endif()
    unset(_xqc_system_processor)
    message(STATUS "XQUIC Linux target architecture: ${XQC_LINUX_ARCH}")
endif()

set(CMAKE_C_STANDARD 11 )
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(XQC_BINARY_TYPE SHARED)

# SSL lib type, xquic support babassl and boringssl interfaces
if(NOT SSL_TYPE)
    set(SSL_TYPE "babassl")
endif()
if(NOT SSL_TYPE STREQUAL "babassl"
    AND NOT SSL_TYPE STREQUAL "boringssl")
    message(FATAL_ERROR
        "unsupported SSL_TYPE=${SSL_TYPE}; use babassl or boringssl")
endif()

# PATH of ssl
if(NOT SSL_PATH)
    if(XQC_LINUX_ARCH STREQUAL "aarch64")
        set(SSL_PATH
            "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/Tongsuo-8.3.0")
    else()
        set(SSL_PATH "/usr/local/babassl")
    endif()
endif()

# ssl include path
if(NOT SSL_INC_PATH)
    set(SSL_INC_PATH
        ${SSL_PATH}/include
    )
endif()

# ssl lib denpendency
if(NOT SSL_LIB_PATH)
    if(SSL_TYPE STREQUAL "babassl"
        AND XQC_LINUX_ARCH STREQUAL "aarch64")
        set(SSL_LIB_PATH
            ${SSL_PATH}/libssl.a
            ${SSL_PATH}/libcrypto.a
        )
    else()
        set(SSL_LIB_PATH
            ${SSL_PATH}/lib/libssl.a
            ${SSL_PATH}/lib/libcrypto.a
        )
    endif()
endif()

set(COMMON_FLAGS            "-Wall -Wno-deprecated  -Wno-unused -Wno-unused-variable -Wno-pointer-sign -Wno-format-security ")

set(COMMON_RELEASE_FLAGS    "-DNDEBUG")
set(COMMON_MIN_FLAGS        " ${COMMON_RELEASE_FLAGS} -Os -ffunction-sections -fdata-sections -fvisibility=hidden")
set(COMMON_DEBUG_FLAGS      " -O0")
if(GCOV)
    set(COMMON_DEBUG_FLAGS "${COMMON_DEBUG_FksLAGS} -fprofile-arcs -ftest-coverage")
endif()

if (CMAKE_BUILD_TYPE STREQUAL Release)
    set(DYNAMIC_COMPILE_FLAGS ${COMMON_RELEASE_FLAGS})
elseif(CMAKE_BUILD_TYPE STREQUAL Debug)
    set(DYNAMIC_COMPILE_FLAGS ${COMMON_DEBUG_FLAGS})
elseif(CMAKE_BUILD_TYPE STREQUAL Minsizerel)
    set(DYNAMIC_COMPILE_FLAGS ${COMMON_MIN_FLAGS})
    set(DYMAMIC_LINK_OPTION "-Wl,--gc-sections")
endif()


add_custom_target(xquic_global)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${COMMON_FLAGS} ${DYNAMIC_COMPILE_FLAGS}")

if (XQC_DISABLE_LOG)
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -DXQC_DISABLE_LOG")
endif()

if(XQC_COMPAT_GENERATE_SR_PKT)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -DXQC_COMPAT_GENERATE_SR_PKT")
endif()

if (XQC_ONLY_ERROR_LOG)
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -DXQC_ONLY_ERROR_LOG")
endif()

if (XQC_ENABLE_EVENT_LOG)
    set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -DXQC_ENABLE_EVENT_LOG ")
endif()

# sendmmsg
if(XQC_SUPPORT_SENDMMSG_BUILD)
    add_definitions(-DXQC_SUPPORT_SENDMMSG)
endif()

if(ANDROID)
    set(DYMAMIC_LINK_OPTION 
        ${DYMAMIC_LINK_OPTION}
        -lm
        -llog)

    if(NOT ANDROID_TOOLCHAIN_ROOT)
        message(FATAL_ERROR  "You Must Define DANDROID_TOOLCHAIN_ROOT")
    endif()

endif()


include(CMakeOptions.txt)

if (XQC_BUILD_OPENSSL)
    if(${SSL_TYPE} MATCHES "boringssl")
        if (XQC_OPENSSL_SMALL)
            set(OPENSSL_SMALL "1")
        endif()

        add_subdirectory(${SSL_PATH})
        include_directories(${SSL_PATH}/include)

        if (BORINGSSL_PREFIX)
            add_definitions(-DBORINGSSL_PREFIX=${BORINGSSL_PREFIX})
            add_dependencies(xquic_global boringssl_prefix_symbols)
            include_directories(${SSL_PATH}/symbol_prefix_include)
        endif()

    else()
        set(EXTRA_CONFIGURE_OPENSSL_PARAMS "${EXTRA_CONFIGURE_OPENSSL_PARAMS} ${DYNAMIC_COMPILE_FLAGS} ")
        add_subdirectory(external/openssl-cmake)
    endif()

    set(DYMAMIC_LINK_OPTION 
            ssl 
            crypto 
            ${DYMAMIC_LINK_OPTION}
        )

elseif(XQC_EXISTS_OPENSSL)
    if(not XQC_EXISTS_OPENSSL_INCLUDE_DIR or not XQC_EXISTS_OPENSSL_BINARY)
        message(FATAL_ERROR  "You Must DEFINE XQC_EXISTS_OPENSSL_INCLUDE_DIR and XQC_EXISTS_OPENSSL_BINARY")
    endif()

    include_directories(XQC_EXISTS_OPENSSL_INCLUDE_DIR)
    set(DYMAMIC_LINK_OPTION "${XQC_EXISTS_OPENSSL_BINARY} ${DYMAMIC_LINK_OPTION}")

else()
    message(FATAL_ERROR  "There Must Exists one tls impl")
endif()


configure_file (
    "${CMAKE_CURRENT_SOURCE_DIR}/xqc_configure.h.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/include/xquic/xqc_configure.h"
    @ONLY
)

include_directories(
    include/
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}/include
)


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
    "src/transport/scheduler/xqc_scheduler_fastprobe.c"
    "src/transport/scheduler/xqc_scheduler_adaptive.c"
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

# add tunnel
if(XQC_ENABLE_TH3 OR XQC_ENABLE_TUNNEL)
    set(
        XQC_SOURCE
    )
    if(XQC_ENABLE_TNL_DG)
        set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -DXQC_ENABLE_TNL_DG ")
    endif()
    add_subdirectory(tunnel)
endif()

if (XQC_NO_SHARED)
    set(XQC_BINARY_TYPE STATIC)
endif()

set(
    NETINS_UDP_SOURCES
    "src/netins_udp/netins_udp_api_version.c"
    "src/netins_udp/netins_udp_backend.c"
    "src/netins_udp/netins_udp_observer.c"
    "src/netins_udp/netins_udp_transport_metrics.c"
    "src/netins_udp/netins_udp_client.c"
    "src/netins_udp/netins_udp_server.c"
    "src/netins_udp/netins_udp_v2_notifier.c"
    "src/netins_udp/netins_udp_v2_client.c"
    "src/netins_udp/netins_udp_v2_h3.c"
    "src/netins_udp/netins_udp_v2_transport.c"
)

add_library(
    xquic
    ${XQC_BINARY_TYPE}
    ${NETINS_UDP_SOURCES}
    ${HTTP3_SOURCES}
    ${TRANSPORT_SOURCES}
    ${TLS_SOURCE}
    ${COMMON_SOURCES}
    ${CONGESTION_CONTROL_SOURCES}
    ${XQC_SOURCE}
)

add_dependencies(xquic xquic_global)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT XQC_NO_SHARED)
    set_property(TARGET xquic APPEND_STRING PROPERTY LINK_FLAGS
        " -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/scripts/xquic.lds")
    set_property(TARGET xquic APPEND PROPERTY LINK_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/xquic.lds")
endif()

if (NOT XQC_NO_SHARED)
    target_link_libraries(
        xquic 
        ${DYMAMIC_LINK_OPTION}
        -pthread
    )
endif()

option(NETINS_UDP_BUILD_TESTS "build netins UDP API tests" OFF)
if(NETINS_UDP_BUILD_TESTS)
    enable_testing()

    add_executable(netins_udp_unit_test
        src/netins_udp/netins_udp_api_version.c
        src/netins_udp/netins_udp_observer.c
        tests/netins_udp/test_netins_udp.c
    )
    target_include_directories(netins_udp_unit_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/netins_udp
    )
    target_link_libraries(netins_udp_unit_test pthread)
    add_test(NAME netins_udp_unit_test COMMAND netins_udp_unit_test)

    add_executable(netins_udp_v2_client_unit_test
        src/netins_udp/netins_udp_v2_notifier.c
        src/netins_udp/netins_udp_v2_client.c
        tests/netins_udp/test_netins_udp_v2_client.c
    )
    target_include_directories(netins_udp_v2_client_unit_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/netins_udp
    )
    target_link_libraries(netins_udp_v2_client_unit_test pthread)
    add_test(NAME netins_udp_v2_client_unit_test
        COMMAND netins_udp_v2_client_unit_test)

    add_executable(netins_udp_v2_h3_unit_test
        src/netins_udp/netins_udp_v2_notifier.c
        src/netins_udp/netins_udp_v2_h3.c
        tests/netins_udp/test_netins_udp_v2_h3.c
    )
    target_include_directories(netins_udp_v2_h3_unit_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src/netins_udp
    )
    target_link_libraries(netins_udp_v2_h3_unit_test pthread)
    add_test(NAME netins_udp_v2_h3_unit_test
        COMMAND netins_udp_v2_h3_unit_test)

    add_executable(netins_udp_v2_notifier_unit_test
        src/netins_udp/netins_udp_v2_notifier.c
        tests/netins_udp/test_netins_udp_v2_notifier.c
    )
    target_include_directories(netins_udp_v2_notifier_unit_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src/netins_udp
    )
    target_link_libraries(netins_udp_v2_notifier_unit_test pthread)
    add_test(NAME netins_udp_v2_notifier_unit_test
        COMMAND netins_udp_v2_notifier_unit_test)

    find_package(PythonInterp 3 QUIET)
    if(PYTHONINTERP_FOUND)
        add_test(NAME netins_udp_api_guard
            COMMAND ${PYTHON_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/netins_udp/verify_netins_udp_api.py)

        find_program(NETINS_UDP_READELF_EXECUTABLE readelf)
        if(NETINS_UDP_READELF_EXECUTABLE
            AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
            AND NOT XQC_NO_SHARED)
            add_test(NAME netins_udp_abi_symbols
                COMMAND ${PYTHON_EXECUTABLE}
                    ${CMAKE_CURRENT_SOURCE_DIR}/tests/netins_udp/verify_netins_udp_api.py
                    --library $<TARGET_FILE:xquic>
                    --readelf ${NETINS_UDP_READELF_EXECUTABLE})
        endif()
    endif()

    # Manual real-transport acceptance test. It requires a certificate/key:
    # netins_udp_v2_loopback_test server.crt server.key 18443
    add_executable(netins_udp_v2_loopback_test
        tests/netins_udp/test_netins_udp_v2_loopback.c
    )
    target_include_directories(netins_udp_v2_loopback_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(netins_udp_v2_loopback_test PRIVATE
        xquic
        ${SSL_LIB_PATH}
        pthread
        dl
        m
    )

    add_library(netins_udp_example OBJECT
        api/examples/netins_udp_snapshot_example.c
        api/examples/netins_udp_connect_example.c
        api/examples/netins_udp_client_lifecycle_example.c
        api/examples/netins_udp_server_lifecycle_example.c
        api/examples/netins_udp_h3_request_example.c
    )
    target_include_directories(netins_udp_example PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
endif()

option(NETINS_UDP_BUILD_CONNECT_EXAMPLE
    "build the standalone netins UDP connection example" OFF)
if(NETINS_UDP_BUILD_CONNECT_EXAMPLE)
    add_executable(netins_udp_connect_cli
        api/examples/netins_udp_connect_example.c
        api/examples/netins_udp_connect_main.c
    )
    target_include_directories(netins_udp_connect_cli PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    )
    target_link_libraries(netins_udp_connect_cli PRIVATE
        xquic
        ${SSL_LIB_PATH}
        pthread
        dl
        m
    )
endif()

if (XQC_ENABLE_TESTING)
    set(CMAKE_MODULE_PATH       "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
    find_package(CUnit REQUIRED)
    include_directories(${CUNIT_INCLUDE_DIR})
    enable_testing()
    add_subdirectory(tests)
    add_subdirectory(tests/unittest)
endif()

if (XQC_BUILD_SAMPLE)
    add_subdirectory(third_party/libevent)
    include_directories(third_party/libevent/include  ${CMAKE_CURRENT_BINARY_DIR}/third_party/libevent/include )
    add_executable(test_client tests/test_client.c)
    add_executable(test_server tests/test_server.c)
    target_link_libraries(test_client xquic event)
    target_link_libraries(test_server xquic event)
endif()

include(GNUInstallDirs)
install(TARGETS xquic
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES include/xquic/netins_udp.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/xquic
)
install(FILES
    api/README.md
    api/NETINS_UDP_API_使用说明.md
    api/examples/netins_udp_snapshot_example.c
    api/examples/netins_udp_connect_example.c
    api/examples/netins_udp_connect_main.c
    api/examples/netins_udp_client_lifecycle_example.c
    api/examples/netins_udp_server_lifecycle_example.c
    api/examples/netins_udp_h3_request_example.c
    DESTINATION ${CMAKE_INSTALL_DOCDIR}/netins_udp
)

