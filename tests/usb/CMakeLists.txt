# 86Box-Next: the USB controllers against fake guest memory and fake devices,
# and the host passthrough against a fake libusb.  No test framework; each
# test is a program that returns non-zero on failure.
if(BUILD_TESTING)
    foreach(t uhci ehci)
        add_executable(usb_${t}_tests
            usb_${t}_test.c
            usb_test_fakes.c
            ../../src/usb/usb_uhci.c
            ../../src/usb/usb_bus.c
            $<$<STREQUAL:${t},ehci>:../../src/usb/usb_ehci.c>
        )
        target_include_directories(usb_${t}_tests PRIVATE
            ${PROJECT_SOURCE_DIR}/src/include
            ${PROJECT_BINARY_DIR}/src/include
        )
        add_test(NAME Usb.${t} COMMAND usb_${t}_tests)
    endforeach()

    find_package(PkgConfig QUIET)
    if(PKG_CONFIG_FOUND)
        pkg_check_modules(LIBUSB_TEST QUIET libusb-1.0)
    endif()
    if(LIBUSB_TEST_FOUND)
        # libusb's header only: every function the backend calls is faked.
        add_executable(usb_host_tests
            usb_host_test.c
            ../../src/usb/usb_host.c
            ../../src/usb/usb_speed.c
        )
        target_compile_definitions(usb_host_tests PRIVATE USE_LIBUSB)
        target_include_directories(usb_host_tests PRIVATE
            ${PROJECT_SOURCE_DIR}/src/include
            ${PROJECT_BINARY_DIR}/src/include
            ${LIBUSB_TEST_INCLUDE_DIRS}
        )
        if(WIN32)
            target_link_libraries(usb_host_tests PRIVATE setupapi)
        endif()
        add_test(NAME Usb.host COMMAND usb_host_tests)
    endif()
endif()
