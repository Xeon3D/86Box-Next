#
# 86Box-Next: isp-server, the virtual ISP -- a PPP server and a libslirp NAT
# per call, with a status and control page in the browser -- and the
# telephone exchange.  A program of its own, kept out of src/: the emulator's
# modems reach it over their TCP line and are not linked with any of this.
# The `isp` library (the core) and `isp_web` (the page) depend on nothing of
# the emulator's, so the tests link them on their own; the exchange borrows
# the modems' voice frames (src/char/modem_voice.c).
#
# On Windows it is a window, not a console: a log that minimizes to the
# notification area (isp_ui_win.c).  Elsewhere it logs to the terminal
# (isp_ui_term.c).
#
# libslirp by plain link flags rather than the PkgConfig::SLIRP imported
# target, which is only visible below src/network, not to tests/.
#
find_package(PkgConfig REQUIRED)
pkg_check_modules(ISP_SLIRP REQUIRED slirp)

add_library(isp STATIC
    isp.c
    isp_plat.c
    isp_nat_slirp.c
    ppp_framing.c
    ppp_session.c
)
target_include_directories(isp
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${ISP_SLIRP_INCLUDE_DIRS}
)
target_compile_options(isp PRIVATE ${ISP_SLIRP_CFLAGS_OTHER})

if(STATIC_BUILD)
    target_compile_definitions(isp PRIVATE LIBSLIRP_STATIC)
    target_link_directories(isp PUBLIC ${ISP_SLIRP_STATIC_LIBRARY_DIRS})
    target_link_libraries(isp PUBLIC ${ISP_SLIRP_STATIC_LIBRARIES})
else()
    target_link_directories(isp PUBLIC ${ISP_SLIRP_LIBRARY_DIRS})
    target_link_libraries(isp PUBLIC ${ISP_SLIRP_LIBRARIES})
endif()

if(WIN32)
    target_link_libraries(isp PUBLIC ws2_32 iphlpapi)
    if(NOT MSVC)
        # glib and intl need it after them; pkg-config lists it before.
        target_link_libraries(isp PUBLIC iconv)
    endif()
    if(MINGW AND STATIC_BUILD)
        # Whatever links this links libslirp and glib statically -- isp-server
        # and the tests alike.  And a static glib cleans up after each thread
        # from a TLS callback, which is only called if the image has a TLS
        # directory: MinGW only links one in when something refers to it.
        target_link_options(isp INTERFACE "-static" "-Wl,-u,_tls_used")
    endif()
else()
    find_package(Threads REQUIRED)
    target_link_libraries(isp PUBLIC Threads::Threads)
endif()

# The page, isp_web_page.html, goes into the program as a C string.
add_custom_command(
    OUTPUT  ${CMAKE_CURRENT_BINARY_DIR}/isp_web_page.h
    COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/isp_web_page.html
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/isp_web_page.h -P ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/isp_web_page.html ${CMAKE_CURRENT_SOURCE_DIR}/embed.cmake
    VERBATIM
)
add_library(isp_web OBJECT
    isp_web.c
    ${CMAKE_CURRENT_BINARY_DIR}/isp_web_page.h
)
target_include_directories(isp_web PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(isp_web PUBLIC isp)

# The server: listeners, ISP calls, the telephone exchange, the .ini -- and
# the status page's phone, which speaks the modems' voice frames
# (src/char/modem_voice.c).
add_library(isp_srv OBJECT isp_srv.c isp_phone.c ${PROJECT_SOURCE_DIR}/src/char/modem_voice.c)
target_include_directories(isp_srv PRIVATE ${PROJECT_SOURCE_DIR}/src/include)
target_link_libraries(isp_srv PUBLIC isp_web isp)

if(WIN32)
    enable_language(RC)
    add_executable(isp-server WIN32 isp_server.c isp_ui_win.c isp-server.rc)
    target_link_libraries(isp-server PRIVATE shell32)
    if(MSVC)
        # main(), not WinMain(), in a window program.
        target_link_options(isp-server PRIVATE "/ENTRY:mainCRTStartup")
    endif()
else()
    add_executable(isp-server isp_server.c isp_ui_term.c)
endif()
target_link_libraries(isp-server PRIVATE isp_srv isp_web isp)
if(STATIC_BUILD AND UNIX)
    target_link_options(isp-server PRIVATE "-static")
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
