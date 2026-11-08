#include <cstdint>
#include <gtest/gtest.h>
#include <ps1recomp/config_generator.h>
#include <ps1recomp/elf_parser.h>
#include <ps1recomp/function_finder.h>
#include <ps1recomp/instruction_emitter.h>
#include <ps1recomp/mips_decoder.h>
#include <ps1recomp/psyq_signatures.h>
#include <string>

using namespace ps1recomp;

TEST(PipelineE2E, EndToEndSimulation) {
  // 1. Analyze
  ElfParser parser;
  if (!parser.load(PS1RECOMP_TEST_ROMS "/example08_spinningCube.elf")) {
    GTEST_SKIP() << "Test ROM not found. Skipping.";
  }

  // Test we can identify PsyQ signatures on this ELF
  FunctionFinder finder;
  finder.findFunctions(parser);
  PsyQMatcher psyq;
  psyq.matchFunctions(parser, finder);
  EXPECT_GT(psyq.getMatchCount(), 0);

  // We already know function finding works. We don't need to rebuild traversing
  // everything. Let's test that emitter handles the basic signature generation.
  InstructionEmitter emitter;
  emitter.setFuncResolver([](uint32_t addr) -> std::string {
    return "func_" + std::to_string(addr);
  });

  // Generate a dummy RecompFunction and emit it
  RecompFunction func;
  func.name = "__e2e_start";
  func.address = 0x80010000;
  func.size = 12;
  // JAL to some function
  uint32_t jal_inst = (3u << 26) | ((0x80010040 & 0x0FFFFFFF) >> 2);
  func.instructions = {jal_inst};

  std::string resulting_code = emitter.emitFunction(func);
  EXPECT_NE(resulting_code.find("func_2147549248"),
            std::string::npos); // Target out of bounds
}

// Running a game is covered by GoldenFrame.CrashRendersContentAtFrame400: the
// runtime binary is built around one game's recompiled code, so it cannot boot
// an arbitrary ELF such as this homebrew one.
