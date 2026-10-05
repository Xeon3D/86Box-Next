# 86Box-Next: the COM port modems' AT engine, identification and line, against
# stubbed sockets, clock and speaker -- and calls through the modem to
# isp-server's core (src/network/isp), behind the stubbed sockets.  No test
# framework; non-zero on failure.
if(BUILD_TESTING)
    add_executable(modem_tests
        modem_test.c
        ../isp/ppp_client.c
        ../../src/char/char_modem.c
    )
    target_include_directories(modem_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_BINARY_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/tests/isp
    )
    target_link_libraries(modem_tests PRIVATE isp)
    if(WIN32)
        target_link_libraries(modem_tests PRIVATE ws2_32)
    endif()
    add_test(NAME Modem.char_modem COMMAND modem_tests)
endif()
