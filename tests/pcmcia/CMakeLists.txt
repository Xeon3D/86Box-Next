# 86Box-Next: the PC Card controller against fake cards, I/O space, memory map,
# PIC and timer; and the emulated cards' CIS.  No test framework; each test
# returns non-zero on failure.
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

    # The cards' CIS: parsed by Linux cistpl.c's rules, Windows 9x IDs.
    add_executable(pcmcia_cis_tests
        cis_test.c
        ../../src/pcmcia/pccard_cis.c
    )
    target_include_directories(pcmcia_cis_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_BINARY_DIR}/src/include
    )
    add_test(NAME Pcmcia.cis COMMAND pcmcia_cis_tests)

    # The flash card under TrueFFS's Intel MTD.
    add_executable(pcmcia_flash_tests
        flash_test.c
        ../../src/pcmcia/flash_card.c
        ../../src/pcmcia/pccard_cis.c
    )
    target_include_directories(pcmcia_flash_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_BINARY_DIR}/src/include
    )
    add_test(NAME Pcmcia.flash COMMAND pcmcia_flash_tests)

    # Multi-function cards: the MFC CIS walked by cistpl.c's rules, the
    # functions' configuration registers, I/O routing and the shared IREQ.
    add_executable(pcmcia_mfc_tests
        mfc_test.c
        ../../src/pcmcia/pccard_mfc.c
        ../../src/pcmcia/pccard_cis.c
    )
    target_include_directories(pcmcia_mfc_tests PRIVATE
        ${PROJECT_SOURCE_DIR}/src/include
        ${PROJECT_BINARY_DIR}/src/include
    )
    add_test(NAME Pcmcia.mfc COMMAND pcmcia_mfc_tests)
endif()
