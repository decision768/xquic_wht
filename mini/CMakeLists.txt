### hq mini client/server ###
    set(ZLOG_INCLUDE_DIR "/home/admin123/zlog-1.2.18/src")
    set(ZLOG_STATIC_LIB  "/home/admin123/zlog-1.2.18/build_aarch64/lib/libzlog.a")
    set(
        HQ_SOURCES
        "../demo/xqc_hq_ctx.c"
        "../demo/xqc_hq_conn.c"
        "../demo/xqc_hq_request.c"
    )

    set(
        MINI_CLIENT_SOURCES
        ${HQ_SOURCES}
        "common.c"
        "mini_client.c"
        "mini_client_cb.c"
        "mini_client_main.c"
    )

    set(
        MINI_SERVER_SOURCES
        ${HQ_SOURCES}
        "common.c"
        "mini_server.c"
        "mini_server_cb.c"
        "mini_server_main.c"
    )


    if(${CMAKE_SYSTEM_NAME} MATCHES "Windows")
        set(MINI_SERVER_SOURCES
            ${MINI_SERVER_SOURCES}
            ${GETOPT_SOURCES}
        )

        set(MINI_CLIENT_SOURCES
            ${MINI_CLIENT_SOURCES}
            ${GETOPT_SOURCES}
        )
    endif()

    include_directories(
        "${CMAKE_SOURCE_DIR}/"
        "${CMAKE_SOURCE_DIR}/include"
        ${CMAKE_SOURCE_DIR}
        ${CUNIT_INCLUDE_DIRS}
        ${LIBEVENT_INCLUDE_DIR}
    )

    add_executable(mini_server ${MINI_SERVER_SOURCES})
    add_executable(mini_client ${MINI_CLIENT_SOURCES})
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(mini_client PRIVATE _GNU_SOURCE)
    endif()


    target_include_directories(mini_server PRIVATE ${ZLOG_INCLUDE_DIR})
    target_include_directories(mini_client PRIVATE ${ZLOG_INCLUDE_DIR})

    target_link_libraries(mini_server ${APP_DEPEND_LIBS} xquic-static ${LIBEVENT_LIBRARY} ${LIBEVENT_PTHREADS_LIBRARY} ${ZLOG_STATIC_LIB})
    target_link_libraries(mini_client ${APP_DEPEND_LIBS} xquic-static ${LIBEVENT_LIBRARY} ${LIBEVENT_PTHREADS_LIBRARY} ${ZLOG_STATIC_LIB})
