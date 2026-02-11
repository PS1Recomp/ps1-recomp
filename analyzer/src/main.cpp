// ps1xAnalyzer — PS1 ELF Analyzer
// Extracts functions, symbols, and relocations from PS1 ELF binaries
// Generates config.toml for the recompiler

#include <cstdio>
#include <ps1recomp/elf_parser.h>
#include <fmt/format.h>

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fmt::print("Usage: ps1xAnalyzer <ps1_elf_file> [output_config.toml]\n");
        return 1;
    }

    const char* elf_path = argv[1];
    const char* output_path = (argc >= 3) ? argv[2] : nullptr;

    ps1recomp::ElfParser parser;
    if (!parser.load(elf_path)) {
        fmt::print(stderr, "Error: {}\n", parser.getError());
        return 1;
    }

    fmt::print("PS1 ELF loaded successfully: {}\n", elf_path);
    fmt::print("  Entry point: 0x{:08X}\n", parser.getEntryPoint());
    fmt::print("  Sections:    {}\n", parser.getSections().size());
    fmt::print("  Symbols:     {}\n", parser.getSymbols().size());
    fmt::print("  Code size:   {} bytes\n", parser.getTotalCodeSize());
    fmt::print("  Data size:   {} bytes\n", parser.getTotalDataSize());

    fmt::print("\nSections:\n");
    for (const auto& section : parser.getSections()) {
        const char* type_str;
        switch (section.type) {
            case ps1recomp::SectionType::Text: type_str = "TEXT"; break;
            case ps1recomp::SectionType::Data: type_str = "DATA"; break;
            case ps1recomp::SectionType::Bss:  type_str = "BSS "; break;
            default:                           type_str = "    "; break;
        }
        fmt::print("  {:16s}  addr=0x{:08X}  size={:8d}  [{}]\n",
                   section.name, section.vaddr, section.size, type_str);
    }

    // Show function symbols if any
    auto funcs = parser.getFunctionSymbols();
    if (!funcs.empty()) {
        fmt::print("\nFunction symbols ({}):\n", funcs.size());
        for (const auto* sym : funcs) {
            const char* psyq_tag = ps1recomp::ElfParser::isLikelyPsyQ(sym->address) ? " [PsyQ?]" : "";
            fmt::print("  0x{:08X}  {:6d}  {}{}\n",
                       sym->address, sym->size, sym->name, psyq_tag);
        }
    }

    if (output_path) {
        fmt::print("\nTODO: Generate config.toml at {}\n", output_path);
    }

    return 0;
}
