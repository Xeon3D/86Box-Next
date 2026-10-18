# 86Box-Next: the SLiRP network driver (src/network/net_slirp.c) behind a stub
# network card, driven by the virtual ISP tests' scripted client.  Offline
# unless given --internet NAME.  No test framework; non-zero on failure.
if(BUILD_TESTING)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(NET_SLIRP_TEST REQUIRED slirp)

    add_executable(net_slirp_tests
        net_slirp_test.c
        ../../isp-server/tests/ppp_client.c
        ../../src/network/net_slirp.c
        ../../src/network/net_event.c
    )
    target_include_directories(net_slirp_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_BINARY_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/src/cpu
        ${PROJECT_SOURCE_DIR}/isp-server/tests
        ${NET_SLIRP_TEST_INCLUDE_DIRS}
    )
    if(STATIC_BUILD)
        target_compile_definitions(net_slirp_tests PRIVATE LIBSLIRP_STATIC)
    endif()
    # libslirp, glib and the ISP's host layer come with it.
    target_link_libraries(net_slirp_tests PRIVATE isp)
    add_test(NAME Network.net_slirp COMMAND net_slirp_tests)

    # Upstream's RTL8139 and network queue tests.  86Box-Next: they count on
    # --gc-sections to drop what they do not reach (network.c, the cards),
    # which PE linking does not do, so they do not link on Windows.
    if(NOT WIN32)
    add_executable(rtl8139_timer_tests rtl8139_timer_test.c ../../src/timer.c)
    target_include_directories(rtl8139_timer_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/src/cpu
        ${PROJECT_BINARY_DIR}/src/include
    )
    target_compile_options(rtl8139_timer_tests PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-ffunction-sections;-fdata-sections>
    )
    target_link_options(rtl8139_timer_tests PRIVATE
        $<$<PLATFORM_ID:Darwin>:-Wl,-dead_strip>
        $<$<AND:$<NOT:$<PLATFORM_ID:Darwin>>,$<C_COMPILER_ID:GNU,Clang>>:-Wl,--gc-sections>
    )
    add_test(NAME rtl8139_timer COMMAND rtl8139_timer_tests)

    add_executable(rtl8139_rx_tests rtl8139_rx_test.c)
    target_include_directories(rtl8139_rx_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/src/cpu
        ${PROJECT_BINARY_DIR}/src/include
    )
    target_compile_options(rtl8139_rx_tests PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-ffunction-sections;-fdata-sections>
    )
    target_link_options(rtl8139_rx_tests PRIVATE
        $<$<PLATFORM_ID:Darwin>:-Wl,-dead_strip>
        $<$<AND:$<NOT:$<PLATFORM_ID:Darwin>>,$<C_COMPILER_ID:GNU,Clang>>:-Wl,--gc-sections>
    )
    add_test(NAME rtl8139_rx COMMAND rtl8139_rx_tests)

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(NETWORK_TEST_SLIRP REQUIRED IMPORTED_TARGET slirp)
    add_executable(network_queue_tests network_queue_test.c)
    target_include_directories(network_queue_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/src/cpu
        ${PROJECT_BINARY_DIR}/src/include
    )
    target_compile_options(network_queue_tests PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-ffunction-sections;-fdata-sections>
    )
    target_link_options(network_queue_tests PRIVATE
        $<$<PLATFORM_ID:Darwin>:-Wl,-dead_strip>
        $<$<AND:$<NOT:$<PLATFORM_ID:Darwin>>,$<C_COMPILER_ID:GNU,Clang>>:-Wl,--gc-sections>
    )
    target_link_libraries(network_queue_tests PRIVATE PkgConfig::NETWORK_TEST_SLIRP)
    add_test(NAME network_queue COMMAND network_queue_tests)
    endif()
endif()
