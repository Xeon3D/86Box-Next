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

    # The telephone exchange: the real server in-process, modems played by
    # sockets speaking its protocol.
    add_executable(exchange_tests exchange_test.c ppp_client.c)
    target_link_libraries(exchange_tests PRIVATE isp_srv isp_web isp)
    add_test(NAME Isp.exchange COMMAND exchange_tests)

    # The status page's phone: the test is the page (a WebSocket) and a
    # modem on the exchange.
    add_executable(web_phone_tests web_phone_test.c)
    target_include_directories(web_phone_tests PRIVATE ${PROJECT_SOURCE_DIR}/src/include)
    target_link_libraries(web_phone_tests PRIVATE isp_srv isp_web isp)
    add_test(NAME Isp.web_phone COMMAND web_phone_tests)

    # Not run by ctest: two scripted guests against a running isp-server,
    #   isp_probe [--host H] [--port P] [--internet NAME]
    add_executable(isp_probe
        isp_probe.c
        ppp_client.c
    )
    target_link_libraries(isp_probe PRIVATE isp)
endif()
