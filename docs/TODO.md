# PS1Recomp — TODO Completo

Ultima atualizacao: 18/02/2026

---

## Fase 0: Setup Inicial ✅
- [x] Pesquisa e analise de fontes (9+ repositorios/artigos)
- [x] Clonar repositorios de referencia no workspace
- [x] Criar estrutura do projeto (analyzer, recompiler, runtime)
- [x] Adicionar submodules (ELFIO, toml11, fmt)
- [x] Instalar dependencias de sistema
- [x] Documentar setup em docs/
- [x] Commits iniciais da estrutura

## Fase 0.5: CI/CD e Testes ✅
- [x] Adicionar GoogleTest como submodule (`lib/googletest`)
- [x] Criar `.github/workflows/ci.yml` (build + test em push/PR)
- [x] Atualizar CMakeLists top-level (GoogleTest + CTest)
- [x] Atualizar `tests/CMakeLists.txt` (targets por componente)
- [x] Criar testes iniciais para o codigo existente (22 testes)
- [x] Commitar e verificar CI rodando no GitHub

## Fase 1: Analise dos Repositorios de Referencia ✅
- [x] **PS2Recomp** — Arquitetura Analyzer+Recomp+Runtime → `docs/analise_ps2recomp.md`
- [x] **N64Recomp** — Data-driven ops, generator pattern → `docs/analise_n64recomp.md`
- [x] **ps1-bare-metal** — Registros HW completos (MMIO, GTE, GPU) → `docs/analise_repos_referencia.md`
- [x] **ghidra_psx_ldr** — 100+ GTE macros PsyQ, 17 GDT files → `docs/analise_repos_referencia.md`
- [x] **psx-modding-toolchain** — Symbol parsing, validacao → `docs/analise_repos_referencia.md`
- [x] **psxsdk** — ECOFF parser (Go) → `docs/analise_repos_referencia.md`

## Fase 2: ps1xAnalyzer (codigo + teste obrigatorio) ✅ (78 tests pass)
- [x] ELF Parser → `elf_parser.cpp` + `test_elf_parser.cpp` ✅ (38 tests pass)
  - [x] Parsear headers ELF (PS-X EXE)
  - [x] Extrair secoes (.text, .data, .bss, .rodata)
  - [x] Extrair tabela de simbolos (se disponivel)
  - [x] Validar entry point e limites de memoria PS1
- [x] Function Finder → `function_finder.cpp` + `test_function_finder.cpp` ✅ (52 tests pass)
  - [x] Detectar funcoes por heuristica (patterns JAL/JR $ra)
  - [x] Detectar funcoes por simbolos ELF
  - [x] Analisar control flow (branch targets)
  - [x] Detectar jump tables
- [x] PsyQ Signatures → `psyq_signatures.cpp` + `test_psyq_signatures.cpp` ✅ (70 tests pass)
  - [x] Carregar patterns de funcoes PsyQ conhecidas
  - [x] Matching por byte patterns
  - [x] Gerar stubs para funcoes PsyQ identificadas
- [x] Config Generator → `config_generator.cpp` + `test_config_generator.cpp` ✅ (78 tests pass)
  - [x] Gerar TOML com funcoes encontradas
  - [x] Listar stubs, skips, patches
  - [x] Incluir metadata (enderecos, tamanhos, jump tables)

## Fase 2.5: Agentes de IA
- [ ] Workflow Agent — automacao de build/test/analyze
  - [ ] Criar `.agent/workflows/build.md` (cmake + build + test)
  - [ ] Criar `.agent/workflows/analyze.md` (rodar ps1xAnalyzer em ELF)
  - [ ] Criar `.agent/workflows/recompile.md` (pipeline completo)
- [ ] Binary Analysis Agent — classificacao de funcoes
  - [ ] Prompt engineering para classificar funcoes desconhecidas
  - [ ] Detectar patterns comuns (init, loop, interrupt handler)
  - [ ] Sugerir nomes/tipos para funcoes anonimas
- [ ] Test Generation Agent
  - [ ] Gerar testes automaticos a partir de funcoes recompiladas
  - [ ] Comparar output com emulador de referencia

## Fase 3: ps1xRecomp (codigo + teste obrigatorio) ← ATUAL
- [x] MIPS I Decoder → `mips_decoder.cpp` + `test_mips_decoder.cpp` ✅ (88 tests pass)
  - [x] Decoder R3000 completo (MIPS I + COP0 + COP2/GTE)
  - [x] Struct Instruction com campos decodificados
  - [x] InstrId enum (~90 valores), InstrCategory, register names
  - [x] 22 GTE commands (RTPS, NCLIP, MVMVA, NCDS, RTPT, etc.)
  - [x] Branch/jump target calculation helpers
- [x] C++ Emitter → `instruction_emitter.cpp` + `test_instruction_emitter.cpp` ✅ (114 tests pass)
  - [x] Traduzir instrucoes MIPS I para C++ via macros runtime
  - [x] Tratar branch delay slots (emitFunction)
  - [x] Gerar labels e gotos para branches
  - [x] Gerar function calls (JAL→resolver, JR $ra→return)
  - [x] Otimizacao: skip writes to $zero
  - [x] COP0/GTE register moves
- [x] GTE Emitter → `gte_emitter.cpp` + `test_gte_emitter.cpp` ✅ (131 tests pass)
  - [x] Traduzir MFC2/MTC2/CFC2/CTC2/LWC2/SWC2 (register moves)
  - [x] Traduzir 22 COP2 commands (RTPS, NCLIP, MVMVA, etc.)
  - [x] MVMVA com decodificacao de flags (matrix/vector/translation)
  - [x] GTE register name tables (32 data + 32 control)
  - [x] GteCommandInfo com cycle counts
- [x] Overlay Handler → `overlay_handler.cpp` + `test_overlay_handler.cpp` ✅ (144 tests pass)
  - [x] OverlaySection struct (name, romOffset, ramBase, size, functions)
  - [x] Parsing de [[overlays]] do config.toml via toml11
  - [x] Address range lookup e conflito detection
  - [x] Qualified naming (overlay_name__addr) para evitar conflitos
  - [x] Dispatch table emission (LOOKUP_FUNC pattern)
- [ ] Agente de Decompilacao (LLM-assisted)
  - [ ] Prompt templates para traduzir MIPS assembly → C legivel
  - [ ] Pipeline: recomp output → LLM cleanup → validacao
  - [ ] Renomear variaveis/funcoes automaticamente via contexto
  - [ ] Detectar structs e tipos de dados a partir de patterns de acesso
  - [ ] Benchmark: comparar output manual vs LLM-assisted

## Fase 4: ps1xRuntime (codigo + teste obrigatorio)
- [x] CPUContext → `cpu_context.h` + `test_cpu_context.cpp` ✅ (173 tests pass)
  - [x] 32 GPRs + HI/LO + PC + COP0[16] + COP2 data/control[32+32]
  - [x] Register/COP0Reg enums, reset(), enforceR0()
- [x] Memory → `memory.h` + `test_memory.cpp` ✅
  - [x] 2MB RAM + 1KB Scratchpad + 512KB BIOS ROM
  - [x] Address masking KUSEG/KSEG0/KSEG1, little-endian R/W
- [x] Runtime Macros → `ps1_runtime_macros.h` + `test_runtime_macros.cpp` ✅
  - [x] MEM_READ/WRITE (8/16/32), DO_LWL/LWR/SWL/SWR
  - [x] GTE register access functions (read/write data/control)
- [x] GTE → `gte.h/cpp` + `test_gte.cpp` ✅ (22 commands, 13 tests, 190 total)
- [x] GPU → `gpu.h` stub ✅
- [x] SPU → `spu.h` stub ✅
- [x] CD-ROM → `cdrom.h` stub ✅
- [x] DMA → `dma.h` stub ✅
- [x] Input → `input.h` stub ✅

## Fase 5: Validacao
- [ ] Testar com homebrews PS1
- [ ] Comparar com DuckStation frame-a-frame
- [ ] Medir performance

## Fase 6: TCC
- [ ] Monografia
- [ ] Metricas
- [ ] Entrega
