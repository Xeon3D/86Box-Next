#
# 86Box    A hypervisor and IBM PC system emulator that specializes in
#          running old operating systems and software designed for IBM
#          PC systems and compatibles from 1981 through fairly recent
#          system designs based on the PCI bus.
#
#          This file is part of the 86Box distribution.
#
#          CMake build script for 88emu, the Roland Sound Canvas / MT-32 / CM board
#          emulator of gearmulator (https://github.com/dsp56300/gearmulator, GPLv3),
#          behind the "Roland Sound Canvas" MIDI out device. gearmulator/ is the subset
#          of its source tree that 88lib needs; see gearmulator/VENDORED.md.
#

set(GM ${CMAKE_CURRENT_SOURCE_DIR}/gearmulator/source)

# asmjit: the custom chips' (XP, LSP, Boss reverb) DSP JITs emit x86-64 / AArch64 with it.
file(GLOB_RECURSE EMU88_ASMJIT_SOURCES CONFIGURE_DEPENDS ${GM}/cpu/dsp56300/source/asmjit/src/asmjit/*.cpp)

set(EMU88_SOURCES)
foreach(dir
        ronaldo/88emu/88lib
        ronaldo/custom_chips/gp
        ronaldo/custom_chips/xp
        ronaldo/custom_chips/la32
        ronaldo/custom_chips/lp
        ronaldo/custom_chips/rcc
        ronaldo/custom_chips/mt32reverb
        cpu/h8500
        cpu/mcs96
        cpu/sh2
        cpu/dsp56300/source/dsp56kBase
        framework/baseLib
        framework/synthLib
        framework/hardwareLib)
    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS ${GM}/${dir}/*.cpp)
    list(APPEND EMU88_SOURCES ${_sources})
endforeach()
# gearmulator builds its framework libraries as C++17 (std::filesystem's u8string() changed in C++20)
# and the CPU cores, the custom chips and 88lib as C++20.
set(EMU88_CXX17_SOURCES)
foreach(dir framework/baseLib framework/synthLib framework/hardwareLib cpu/dsp56300/source/dsp56kBase)
    file(GLOB_RECURSE _sources CONFIGURE_DEPENDS ${GM}/${dir}/*.cpp)
    list(APPEND EMU88_CXX17_SOURCES ${_sources})
endforeach()
set_source_files_properties(${EMU88_CXX17_SOURCES} ${EMU88_ASMJIT_SOURCES} PROPERTIES COMPILE_OPTIONS -std=gnu++17)

list(APPEND EMU88_SOURCES
    ${GM}/3rdparty/libresample/src/filterkit.c
    ${GM}/3rdparty/libresample/src/resample.c
    ${GM}/3rdparty/libresample/src/resamplesubs.c)

add_library(emu88 STATIC
    ${EMU88_SOURCES}
    ${EMU88_ASMJIT_SOURCES}
    emu88_host.cpp
    emu88_lcd.cpp
    lcd_font.cpp
)

target_include_directories(emu88
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE
        ${GM}
        ${GM}/cpu
        ${GM}/ronaldo
        ${GM}/ronaldo/88emu
        ${GM}/ronaldo/custom_chips
        ${GM}/framework
        ${GM}/cpu/dsp56300/source
        ${GM}/3rdparty
        ${GM}/cpu/dsp56300/source/asmjit/src
)

set_target_properties(emu88 PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_definitions(emu88 PRIVATE ASMJIT_STATIC ASMJIT_NO_FOREIGN EMU88_VERSION_STRING="gearmulator-567ee1ff")

# gearmulator's own release flags (-Ofast on Linux, /fp:fast on Windows), the warnings of
# foreign code silenced; its MIDI file tempo maths needs the NaN/Inf checks back.
target_compile_options(emu88 PRIVATE -ffast-math -w)
set_source_files_properties(${GM}/framework/synthLib/midi/rcpTempo.cpp PROPERTIES COMPILE_OPTIONS "-std=gnu++17;-fno-finite-math-only")

if(WIN32)
    target_link_libraries(emu88 PRIVATE ole32 shell32 uuid)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(emu88 PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
    if(NOT APPLE)
        target_link_libraries(emu88 PRIVATE rt)
    endif()
endif()
