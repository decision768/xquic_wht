if(XQC_ENABLE_MINI_API)
    include(GNUInstallDirs)

    if(TARGET xquic AND NOT XQC_NO_SHARED)
        set_target_properties(xquic PROPERTIES
            VERSION "1.0.0"
            SOVERSION "1")
    endif()

    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/include/xquic/netins_udp.h"
        DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/xquic"
        COMPONENT mini_api)

    foreach(library_target xquic-static xquic)
        if(TARGET ${library_target})
            install(TARGETS ${library_target}
                ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
                LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
                RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
                COMPONENT mini_api)
        endif()
    endforeach()

    foreach(demo_target
        netins_udp_mini_api_client
        netins_udp_mini_api_server
        netins_udp_stream_client
        netins_udp_stream_server
        mini_stream_protocol_test
        mini_stream_reorder_test
        netins_udp_stream_api_contract_test)
        if(TARGET ${demo_target})
            install(TARGETS ${demo_target}
                RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
                COMPONENT mini_api)
        endif()
    endforeach()

    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/README.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/NETINS_UDP_API_使用说明.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/NETINS_UDP_编译指南.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/MINI_API_测试使用说明.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/NETINS_UDP_STREAM_API_阶段0与1设计.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/NETINS_UDP_STREAM_API_阶段2与3实现.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/NETINS_UDP_STREAM_API_阶段4至7交付与验收.md"
        "${CMAKE_CURRENT_SOURCE_DIR}/VERSION"
        DESTINATION "."
        COMPONENT mini_api)

    install(PROGRAMS
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/stream_api_acceptance.py"
        DESTINATION "tests"
        COMPONENT mini_api)
    install(PROGRAMS
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/mini_api/check_abi.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/mini_api/check_stream_isolation.py"
        DESTINATION "scripts/mini_api"
        COMPONENT mini_api)
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/xquic.lds"
        DESTINATION "scripts"
        COMPONENT mini_api)
endif()
