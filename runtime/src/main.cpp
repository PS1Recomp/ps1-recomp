// ps1xRuntime — PS1 Hardware Simulation Runtime
// Executes recompiled C++ code with GPU, GTE, SPU, CD-ROM simulation

#include <cstdio>
#include <fmt/format.h>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fmt::print("Usage: ps1xRuntime <recompiled_game>\n");
        return 1;
    }

    fmt::print("ps1xRuntime — PS1 Hardware Simulation\n");
    fmt::print("TODO: CPU context, GTE, GPU (OpenGL), SPU (SDL), CD-ROM, Memory, Input\n");

    return 0;
}
