# 86Box-Next: the COM port modems' AT engine, identification and line, against
# stubbed sockets, clock and speaker.  No test framework; non-zero on failure.
if(BUILD_TESTING)
    add_executable(modem_tests
        modem_test.c
        ../../src/char/char_modem.c
    )
    target_include_directories(modem_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_BINARY_DIR}/src/include
    )
    if(WIN32)
        target_link_libraries(modem_tests PRIVATE ws2_32)
    endif()
    add_test(NAME Modem.char_modem COMMAND modem_tests)
endif()
