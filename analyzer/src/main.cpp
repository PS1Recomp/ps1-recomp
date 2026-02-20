// ps1xAnalyzer — PS1 ELF Analyzer
// Extracts functions, symbols, and relocations from PS1 ELF binaries
// Generates config.toml for the recompiler

#include <cstdio>
#include <fmt/format.h>
#include <ps1recomp/config_generator.h>
#include <ps1recomp/elf_parser.h>
#include <ps1recomp/function_finder.h>
#include <ps1recomp/psyq_signatures.h>

int main(int argc, char *argv[]) {
  if (argc < 2) {
    fmt::print("Usage: ps1xAnalyzer <ps1_elf_file> [output_config.toml]\n");
    return 1;
  }

  const char *elf_path = argv[1];
  const char *output_path = (argc >= 3) ? argv[2] : nullptr;

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
  for (const auto &section : parser.getSections()) {
    const char *type_str;
    switch (section.type) {
    case ps1recomp::SectionType::Text:
      type_str = "TEXT";
      break;
    case ps1recomp::SectionType::Data:
      type_str = "DATA";
      break;
    case ps1recomp::SectionType::Bss:
      type_str = "BSS ";
      break;
    default:
      type_str = "    ";
      break;
    }
    fmt::print("  {:16s}  addr=0x{:08X}  size={:8d}  [{}]\n", section.name,
               section.vaddr, section.size, type_str);
  }

  // 1. Find functions using heuristics and symbols
  ps1recomp::FunctionFinder finder;
  finder.findFunctions(parser);

  // 2. Classify functions against PsyQ database
  ps1recomp::PsyQMatcher matcher;
  matcher.matchFunctions(parser, finder);

  fmt::print("\nFunction stats:\n");
  fmt::print("  Total functions: {}\n", finder.getFunctionCount());
  fmt::print("  PsyQ matches:    {}\n", matcher.getMatchCount());

  // 3. Generate config if requested
  if (output_path) {
    ps1recomp::ConfigGenerator generator;
    if (generator.generate(parser, finder, matcher, elf_path, output_path)) {
      fmt::print("\nSuccess! Generated config at: {}\n", output_path);
    } else {
      fmt::print(stderr, "\nFailed to generate config: {}\n",
                 generator.getError());
      return 1;
    }
  } else {
    fmt::print("\nNo output_config.toml specified. Skipping generation.\n");
  }

  return 0;
}
