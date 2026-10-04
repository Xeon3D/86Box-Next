# 86Box-Next: the USB controllers against fake guest memory and fake devices.
# No test framework; each test is a program that returns non-zero on failure.
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
endif()
