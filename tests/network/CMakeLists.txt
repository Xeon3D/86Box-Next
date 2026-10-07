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
endif()
