# 86Box-Next: isp_web_page.html as a C string, for isp-server.
#   cmake -DIN=isp_web_page.html -DOUT=isp_web_page.h -P embed.cmake
file(READ "${IN}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
file(WRITE "${OUT}" "/* Generated from isp_web_page.html by embed.cmake: do not edit. */\nstatic const char isp_web_page[] = { ${bytes}0x00 };\n")
