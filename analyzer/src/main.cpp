// ps1xAnalyzer — PS1 ELF Analyzer
// Extracts functions, symbols, and relocations from PS1 ELF binaries
// Generates config.toml for the recompiler

#include <cstdio>
#include <elfio/elfio.hpp>
#include <fmt/format.h>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fmt::print("Usage: ps1xAnalyzer <ps1_elf_file> [output_config.toml]\n");
        return 1;
    }

    const char* elf_path = argv[1];

    ELFIO::elfio reader;
    if (!reader.load(elf_path)) {
        fmt::print(stderr, "Error: Could not load ELF file: {}\n", elf_path);
        return 1;
    }

    // Verify it's a MIPS little-endian 32-bit ELF (PS1)
    if (reader.get_machine() != ELFIO::EM_MIPS) {
        fmt::print(stderr, "Error: Not a MIPS ELF (machine type: {})\n", reader.get_machine());
        return 1;
    }

    if (reader.get_class() != ELFIO::ELFCLASS32) {
        fmt::print(stderr, "Error: Not a 32-bit ELF\n");
        return 1;
    }

    fmt::print("PS1 ELF loaded successfully: {}\n", elf_path);
    fmt::print("  Entry point: 0x{:08X}\n", reader.get_entry());
    fmt::print("  Sections:    {}\n", reader.sections.size());
    fmt::print("  Segments:    {}\n", reader.segments.size());

    for (const auto& section : reader.sections) {
        fmt::print("  Section: {:16s}  addr=0x{:08X}  size={}\n",
                   section->get_name(),
                   section->get_address(),
                   section->get_size());
    }

    fmt::print("\nTODO: Function detection, PsyQ signature matching, config.toml generation\n");

    return 0;
}
