# Writes OUT, a C file holding IN's bytes as index_html_start[] (NUL-ended)
# and index_html_size (the NUL included), as ESP-IDF's EMBED_TXTFILES gives.
file(READ ${IN} hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
string(REGEX REPLACE "((0x..,){32})" "\\1\n" bytes "${bytes}")
file(WRITE ${OUT} "// Generated from main/web/index.html by linux/embed.cmake.\n"
  "const char index_html_start[] = {\n${bytes}0x00};\n"
  "const unsigned long index_html_size = sizeof(index_html_start);\n")
