# 86Box-Next: the virtual ISP (src/network/isp/) -- framing, the PPP
# automaton, and whole sessions through libslirp to the host's loopback.
# No test framework; non-zero on failure.  Offline.
if(BUILD_TESTING)
    add_executable(isp_tests
        isp_test.c
        ppp_client.c
    )
    target_link_libraries(isp_tests PRIVATE isp)
    add_test(NAME Isp.core COMMAND isp_tests)

    # Not run by ctest: two scripted guests against a running isp-server,
    #   isp_probe [--host H] [--port P] [--internet NAME]
    add_executable(isp_probe
        isp_probe.c
        ppp_client.c
    )
    target_link_libraries(isp_probe PRIVATE isp)
endif()
