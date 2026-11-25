# cmake -DIN=<file> -DOUT=<header> -DNAME=<symbol> -P embed_text.cmake
# Writes `static const char NAME[] = { ..., 0 };` holding IN's bytes, so a
# shader's source can be compiled again at run time (sdlgpu_fscache.inc.c).
file(READ "${IN}" _hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _hex "${_hex}")
file(WRITE "${OUT}" "/* generated from ${IN} */\nstatic const char ${NAME}[] = {${_hex}0x00};\n")
