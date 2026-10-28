# Writes a SPIR-V binary as a C++ array.
# cmake -DINPUT=<file.spv> -DOUTPUT=<file.cpp> -DNAME=<symbol> -P embed_spirv.cmake
file(READ "${INPUT}" data HEX)
string(LENGTH "${data}" length)
math(EXPR size "${length} / 2")
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${data}")
file(WRITE "${OUTPUT}"
    "#include <cstddef>\n#include <cstdint>\n"
    "alignas(4) extern const uint8_t ${NAME}[] = {${bytes}};\n"
    "extern const size_t ${NAME}_size = ${size};\n")
