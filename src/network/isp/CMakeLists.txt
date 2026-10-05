#
# 86Box-Next: the virtual ISP -- a PPP server and a libslirp NAT per call --
# behind the modems' "Internet" line (char_modem.c), and isp-server, the same
# behind a TCP port.  The `isp` library depends on nothing of the emulator's,
# so isp-server and the tests link it on its own.
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
    PUBLIC  ${PROJECT_SOURCE_DIR}/src/include ${CMAKE_CURRENT_SOURCE_DIR}
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
        # Whatever links this links libslirp and glib statically -- the
        # emulator, isp-server and the tests alike.  And a static glib cleans
        # up after each thread from a TLS callback, which is only called if
        # the image has a TLS directory: MinGW only links one in when
        # something refers to it.
        target_link_options(isp INTERFACE "-static" "-Wl,-u,_tls_used")
    endif()
else()
    find_package(Threads REQUIRED)
    target_link_libraries(isp PUBLIC Threads::Threads)
endif()

target_link_libraries(86Box isp)

add_executable(isp-server isp_server.c)
target_link_libraries(isp-server PRIVATE isp)
if(MINGW)
    # A console program, whatever the emulator's own link flags say.
    target_link_options(isp-server PRIVATE "-mconsole")
endif()
if(STATIC_BUILD AND UNIX)
    target_link_options(isp-server PRIVATE "-static")
endif()
