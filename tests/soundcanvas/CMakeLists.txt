# 86Box-Next: the Roland Sound Canvas MIDI device's 88emu glue (src/sound/emu88):
# the board catalogue, ROM status, and -- with BOX86NEXT_SC_ROMS pointing at a
# folder of dumps -- booting an SC-55mkII, its sound, display and power switch.
# No test framework; non-zero on failure.
if(BUILD_TESTING AND SOUNDCANVAS)
    add_executable(soundcanvas_tests soundcanvas_test.c)
    target_link_libraries(soundcanvas_tests PRIVATE emu88)
    add_test(NAME SoundCanvas.emu88 COMMAND soundcanvas_tests)
    set_tests_properties(SoundCanvas.emu88 PROPERTIES TIMEOUT 300)
endif()
