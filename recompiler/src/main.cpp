// ps1xRecomp — PS1 Static Recompiler
// Translates MIPS I instructions to C++ code (1:1 literal translation)

#include <cstdio>
#include <fmt/format.h>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fmt::print("Usage: ps1xRecomp <config.toml> [output_dir]\n");
        return 1;
    }

    fmt::print("ps1xRecomp — PS1 MIPS I → C++ Recompiler\n");
    fmt::print("TODO: MIPS I decoder, C++ emitter, GTE support, overlay handling\n");

    return 0;
}
