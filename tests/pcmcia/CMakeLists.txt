# 86Box-Next: the PC Card controller against a fake card, I/O space, memory map
# and PIC.  No test framework; the test returns non-zero on failure.
if(BUILD_TESTING)
    add_executable(pcmcia_pcic_tests
        pcic_test.c
        ../../src/pcmcia/pcic_pd6722.c
    )
    target_include_directories(pcmcia_pcic_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_BINARY_DIR}/src/include
    )
    add_test(NAME Pcmcia.pcic COMMAND pcmcia_pcic_tests)
endif()
