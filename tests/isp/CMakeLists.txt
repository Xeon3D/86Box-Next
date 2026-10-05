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

    # isp-server's status page, requests handled directly.
    add_executable(isp_web_tests isp_web_test.c)
    target_link_libraries(isp_web_tests PRIVATE isp_web isp)
    add_test(NAME Isp.web COMMAND isp_web_tests)

    # Not run by ctest: two scripted guests against a running isp-server,
    #   isp_probe [--host H] [--port P] [--internet NAME]
    add_executable(isp_probe
        isp_probe.c
        ppp_client.c
    )
    target_link_libraries(isp_probe PRIVATE isp)
endif()
