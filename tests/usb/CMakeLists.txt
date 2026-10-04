# 86Box-Next: the UHCI controller against fake guest memory and a fake device.
if(BUILD_TESTING)
    add_executable(usb_uhci_tests
        usb_uhci_test.c
        ../../src/usb/usb_uhci.c
    )
    target_include_directories(usb_uhci_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_BINARY_DIR}/src/include
    )
    add_test(NAME Usb.Uhci COMMAND usb_uhci_tests)
endif()
