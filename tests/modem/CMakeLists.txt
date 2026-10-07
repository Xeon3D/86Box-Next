# 86Box-Next: the COM port modems' AT engine, identification and line, against
# stubbed sockets, clock and speaker -- and calls through the modem to
# isp-server's core (isp-server/), behind the stubbed sockets.  No test
# framework; non-zero on failure.
if(BUILD_TESTING)
    add_executable(modem_tests
        modem_test.c
        ../../isp-server/tests/ppp_client.c
        ../../src/char/char_modem.c
        ../../src/char/modem_voice.c
    )
    target_include_directories(modem_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_BINARY_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/isp-server/tests
    )
    target_link_libraries(modem_tests PRIVATE isp)
    if(WIN32)
        target_link_libraries(modem_tests PRIVATE ws2_32)
    endif()
    add_test(NAME Modem.char_modem COMMAND modem_tests)

    # Two modems calling each other over isp-server's telephone network: the
    # real modem, real sockets, the real exchange in-process.
    if(WIN32)
        set(PHONE_NETSOCKET ../../src/qt/win_netsocket.c)
    else()
        set(PHONE_NETSOCKET ../../src/unix/unix_netsocket.c)
    endif()
    add_executable(phone_tests
        phone_test.c
        ../../isp-server/tests/ppp_client.c
        ../../src/char/char_modem.c
        ${PHONE_NETSOCKET}
    )
    target_include_directories(phone_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_BINARY_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/isp-server/tests
    )
    # (modem_voice.c comes with isp_srv, whose page phone speaks it too)
    target_link_libraries(phone_tests PRIVATE isp_srv isp_web isp)
    if(WIN32)
        target_link_libraries(phone_tests PRIVATE ws2_32)
    endif()
    add_test(NAME Modem.phone COMMAND phone_tests)

    # Voice calls: the codecs, then two real modems in voice mode through the
    # real exchange, and the handset.
    add_executable(voice_tests
        voice_test.c
        ../../isp-server/tests/ppp_client.c
        ../../src/char/char_modem.c
        ${PHONE_NETSOCKET}
    )
    target_include_directories(voice_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_BINARY_DIR}/src/include
        ${PROJECT_SOURCE_DIR}/isp-server/tests
        ${PROJECT_SOURCE_DIR}/isp-server
    )
    target_link_libraries(voice_tests PRIVATE isp_srv isp_web isp)
    if(WIN32)
        target_link_libraries(voice_tests PRIVATE ws2_32 winmm)
    endif()
    add_test(NAME Modem.voice COMMAND voice_tests)
endif()
