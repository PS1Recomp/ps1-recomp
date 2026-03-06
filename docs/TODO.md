# PS1Recomp — TODO Completo

Ultima atualizacao: 03/03/2026

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
- [x] Pipeline end-to-end testada (3 testes e2e)
- [ ] Testar com homebrews PS1
- [ ] Comparar com DuckStation frame-a-frame
- [ ] Medir performance

## Fase 6: Disc Image Loader (BIN/CUE/CHD/ISO/PBP)
Ler jogos comerciais de PS1 a partir de imagens de disco

### Implementacao
- [x] Parser de .CUE (indice de trilhas de audio + dados)
- [x] Parser de .BIN (raw sector data, 2352 bytes/sector)
- [x] Extrair PS-X EXE (SYSTEM.CNF → SLUS_XXX.XX / SCES_XXX.XX)
- [x] Parser de .ISO (ISO9660 filesystem)
- [ ] Parser de .CHD (MAME compressed hunks — libchdr)
- [ ] Parser de .PBP (PSP format — EBOOT.PBP)
- [x] Filesystem virtual: CD-ROM read commands → leitura do arquivo de imagem
- [x] Suporte a multi-disc (swapDisc, getDiscCount, getCurrentDisc)

### Testes
- [x] `test_cue_parser.cpp` — parsear .CUE com 1 trilha, multi-trilha, audio+dados
- [x] `test_bin_reader.cpp` — ler setores RAW, validar EDC/ECC se presente
- [x] `test_iso9660.cpp` — navegar diretorio ISO, encontrar arquivos por path
- [x] `test_exe_extractor.cpp` — extrair PS-X EXE de SYSTEM.CNF
- [x] Teste de integracao: `test_virtual_fs.cpp` carrega e lê arquivos

### Qualidade
- [x] Error handling: retorno coerente com std::optional
- [x] Logging: trilhas detectadas, filesystem montado, EXE encontrado
- [x] Documentar em `docs/arquitetura_disc_loader.md`

## Fase 7: BIOS / Kernel Emulation
Jogos comerciais chamam funcoes da BIOS do PS1

### Implementacao
- [x] Tabela A (0xA0) — ~100 funcoes (printf, malloc, memcpy, file ops, etc.)
- [x] Tabela B (0xB0) — ~60 funcoes (open, close, read, GPU/pad init, etc.)
- [x] Tabela C (0xC0) — ~20 funcoes (exception handler, event handling)
- [x] Interrupt/Exception handler (SYSCALL, BREAK)
- [x] Event system (OpenEvent, EnableEvent, TestEvent)
- [x] Heap management (InitHeap, malloc, free)
- [x] File I/O via CD-ROM (open, read, close)
- [x] Memory card stubs (save/load game)
- [x] Printf/puts via serial (debug output)

### Testes
- [x] `test_bios_table_a.cpp` — testar malloc/free, memcpy, printf, strlen
- [x] `test_bios_table_b.cpp` — testar open/close/read, event system
- [x] `test_bios_table_c.cpp` — testar exception handler registration
- [x] `test_heap.cpp` — alloc/free/realloc, fragmentacao, out-of-memory
- [x] `test_event_system.cpp` — OpenEvent, EnableEvent, TestEvent, DeliverEvent
- [x] Teste de integracao: homebrew que usa printf/malloc funciona

### Qualidade
- [x] Logging: cada chamada BIOS logada com argumentos (DEBUG level)
- [x] Funcoes nao implementadas: log WARNING + return valor seguro (nao crash)
- [x] Documentar em `docs/arquitetura_bios.md`

## Fase 8: GPU Completa (Renderizacao)
GPU real com SDL2/OpenGL backend para rodar jogos visuais

### 8.1: Backend de Renderizacao
- [x] Janela SDL2 com contexto OpenGL
- [x] VRAM 1024x512x16bpp como textura OpenGL
- [x] Double buffering com VSync
- [x] Resolucoes: 256/320/368/512/640 x 240/480 (PAL/NTSC)

### 8.2: GP0 — Comandos de Renderizacao
- [x] Polygons: triangulo/quad, flat/gouraud, textured/untextured
- [x] Lines: flat/gouraud, single/poly-line
- [x] Rectangles: variable/1x1/8x8/16x16, textured/untextured
- [x] VRAM-to-VRAM blit (copy, fill)
- [x] CPU-to-VRAM (texture upload)
- [x] VRAM-to-CPU (framebuffer readback)
- [x] Texture windowing (mask + offset)
- [x] Semi-transparency (4 modos: B/2+F/2, B+F, B-F, B+F/4)
- [x] Dithering (4x4 ordered dither)
- [x] Texture color modes (4-bit, 8-bit, 15-bit direct)
- [x] CLUT (Color Lookup Table) para texturas indexadas

### 8.3: GP1 — Controle de Display
- [x] Display enable/disable
- [x] Display area start (VRAM offset do framebuffer)
- [x] Horizontal/Vertical display range
- [x] Display mode (NTSC/PAL, interlaced, 24-bit color)
- [x] DMA direction
- [x] GPUSTAT register completo

### 8.4: Ordering Table
- [x] DMA linked-list traversal (GPU DMA channel 2)
- [x] Ordering table sorting (Z-buffer via OT)

### Testes
- [x] `test_gpu_commands.cpp` — decodificar GP0/GP1 commands corretamente
- [x] `test_gpu_rasterizer.cpp` — rasterizar triangulo flat, verificar pixels no VRAM
- [x] `test_gpu_textures.cpp` — upload textura 4/8/16bit, verificar CLUT lookup
- [x] `test_gpu_transparency.cpp` — verificar os 4 modos de semi-transparencia
- [x] `test_gpu_vram.cpp` — VRAM-to-VRAM blit, CPU-to-VRAM, VRAM-to-CPU
- [x] `test_gpu_display.cpp` — resolucoes, PAL/NTSC, interlaced
- [x] Teste visual: `test_gpu_screenshot.cpp` (5 testes VRAM verification)

### Qualidade
- [x] Logging: GP0/GP1 commands com parametros (TRACE level)
- [ ] Debug: VRAM viewer via ImGui overlay
- [x] Error handling: comandos GP0 desconhecidos logados, nao crash
- [x] Documentar em `docs/arquitetura_gpu.md`

## Fase 9: SPU Completa (Audio) ✅
Audio com SDL2 Audio backend

### Implementacao
- [x] 24 voice channels com ADPCM decoding
- [x] ADSR envelope (Attack, Decay, Sustain, Release) per channel
- [x] Master e per-voice volume logic
- [x] Pitch modulation e noise generation
- [x] Reverb DSP (registros de configuração)
- [x] Mixagem de CD-DA e XA-ADPCM audio
- [x] 512KB SPU RAM ring buffers e IRQs
- [x] Bind audio mix to SDL2 Audio Callback

### Testes
- [x] `test_spu.cpp` — 15 testes (ADPCM, ADSR, volume, reverb, XA, Sound RAM)

### Qualidade
- [x] Thread safety (std::mutex no generateSamples)
- [ ] ImGui SPU channel debugger
- [x] Log de eventos key-on/off
- [x] Documentar em `docs/arquitetura_spu.md`

## Fase 10: CD-ROM Completa ✅
Dados de CD-ROM precisos, subheaders e maquina de estado

### Implementacao
- [x] CD-ROM Commands: SetLoc, ReadN, ReadS, Pause, Stop, GetStat + 14 outros
- [x] Sector buffer logic (2048 data / 2352 raw)
- [x] Async read interrupts: INT1 (data ready), INT5 (errors)
- [x] XA-ADPCM sector filtering by file/channel numbers
- [x] Parse sub-headers (Mode 2 Form 1 / Form 2) via mode register
- [x] Interpret TOC (Table of Contents) — GetTN/GetTD commands
- [x] CD-DA playback modes e speed control (1x/2x)

### Testes
- [x] `test_cdrom_controller.cpp` — 8 testes (GetStat, SetLoc, Init, ACK, MSF, BCD)

### Qualidade
- [x] State machine estrita (Idle/Reading/Seeking/Playing/Paused/SpinUp)
- [x] Log de command hex codes
- [x] Out-of-bounds seeking mitigations
- [x] Documentar em `docs/arquitetura_cdrom.md`

## Fase 11: DMA Completa ✅
7 canais DMA de transferencia

### Implementacao
- [x] Channel 0/1 (MDEC Video transfers)
- [x] Channel 2 (GPU block/linked-list transfers)
- [x] Channel 3 (CD-ROM to RAM slices)
- [x] Channel 4 (SPU uploads)
- [x] Channel 5 (PIO expansion interface — stub)
- [x] Channel 6 (OTC reverse linked-list initializer)
- [x] Respect DPCR priorities e DICR interrupt flags

### Testes
- [x] `test_dma.cpp` — 6 testes (DPCR, registers, OTC, DICR, block)

### Qualidade
- [x] Prevenir loops infinitos nas listas linkadas (max iterations)
- [x] Logs nos blocos de copia
- [x] Documentar em `docs/arquitetura_dma.md`

## Fase 12: Input & Memory Cards ✅
Perifericos PC e emulacao SIO

### Implementacao
- [x] SDL2 keyboard/gamepad API mapping (Digital Pad e DualShock)
- [x] SIO hardware logic (polling ACK, state machine)
- [ ] Multitap logic
- [x] Memory Cards 128KB (15 blocks x 8KB) interface
- [x] Local file system storage para memory cards (.mcr)
- [x] Read/write sector commands via SIO

### Testes
- [x] `test_input.cpp` — 10 testes (digital, analog, SIO, memory card)

### Qualidade
- [ ] External TOML configuration para bindings
- [x] File I/O para save files
- [x] Documentar em `docs/arquitetura_input.md`

## Fase 13: MDEC Video ✅
Decodificador de video MDEC para cutscenes

### Implementacao
- [x] MDEC command protocol (opcode 1/2/3)
- [x] Run-Length Decoding passes (zigzag + dequantize)
- [x] Inverse Discrete Cosine Transform (iDCT) — precomputed cosine table
- [x] YCbCr (6-block Macroblocks) para RGB15/24 converter
- [x] Connect DMA 0 (In) e DMA 1 (Out)

### Testes
- [x] `test_mdec.cpp` — 7 testes (state, reset, quant table, decode, status, DMA, zigzag)

### Qualidade
- [x] iDCT eficiente (precomputed table, fixed-point)
- [x] Tratar garbage input graciosamente
- [x] Documentar em `docs/arquitetura_mdec.md`

## Fase 14: Timers e Interrupts ✅
Precise system timing e IRQ propagation

### Implementacao
- [x] Root counters: TMR0 (dotclock), TMR1 (hblank), TMR2 (sysclock/8)
- [x] Timer overflow logic e clock source selection
- [x] IRQ logic via I_STAT/I_MASK (AND-acknowledge)
- [x] Integrate VBlank, CDROM, DMA, TMR0-2, Pad/MC, SPU

### Testes
- [x] `test_timers.cpp` — 11 testes (IRQ raise/ack, counter, target, overflow, VBlank)

### Qualidade
- [x] IRQ repeat/toggle modes
- [x] WriteMode resets counter
- [x] Documentar em `docs/arquitetura_timers.md`

## Fase 15: Integração Completa ✅ (parcial)
Ecosystem merge e QoL

### Implementacao
- [x] Main loop tick orchestration (CPU → DMA → Timers → VBlank → IRQ)
- [x] Memory I/O routing para 8 subsistemas
- [x] SDL2 audio callback → SPU::generateSamples()
- [x] SDL2 keyboard/gamepad → InputController
- [x] VBlank/CDROM/DMA/Input/SPU IRQ propagation
- [x] XA-ADPCM: CDROM → SPU via callback
- [x] Config file reader (TOML) — `config.h/cpp`
- [ ] Dev/Debug UI overlays (ImGui)
- [x] Save State logic — `save_state.h/cpp`
- [ ] Fast Forward e Frame Stepping

### Testes
- [x] `test_save_state.cpp` — 5 testes (RAM, COP0, IRQ, invalid file, isValid)
- [x] `test_config.cpp` — 6 testes (defaults, parse, save/reload, comments)
- [ ] `test_integration_homebrew.cpp`

### Qualidade
- [x] Threading safety (SPU mutex)
- [ ] ASAN checks loop
- [ ] Performance profiling markers
- [x] Documentar em `docs/arquitetura_integracao.md`

## Fase 16: Validação com Jogos Comerciais

### Estado Atual

| Componente | Status |
|-----------|--------|
| Analyzer → PS-EXE nativo (`PS-X EXE` magic detection) | ✅ |
| Analyzer → Disc Reader BIN/CUE + ISO9660 | ✅ |
| Analyzer → aceita .bin direto (auto-extract boot EXE) | ✅ |
| Recompiler → ELF/PS-EXE → C++ | ✅ |
| Runtime → GPU (polígonos, linhas, rects) | ✅ (completo — todos GP0/GP1 opcodes) |
| Runtime → DMA, Timers, SPU, MDEC, Input | ✅ (completo — MDEC conectado via DMA) |
| Runtime → BIOS (A0/B0/C0 tables) | ✅ (GPU_cw/cwp, InitPAD/StartPAD funcionais) |
| Runtime → `CALL_INDIRECT` | ✅ (dispatch table via unordered_map, 7-layer lookup) |
| Runtime → CD-ROM lendo disco real | ✅ (montagem de VirtualFs + respostas INT/status completas) |
| Overlay system | ✅ (Scanner + Recompiler multi-segment + Runtime manager) |

### Progresso da Análise de Jogos
- [x] `test_game_validation.cpp` — 10 testes (I/O, frame tick, DMA, SPU, CDROM, input, MDEC, reset, 1000-frame stress)
- [x] `compatibility.md` — lista de 15 jogos comerciais + 2 homebrews
- [x] Testar spinningCube interativamente (corrigido Stack Pointer $sp → 0x801FFFF0)
- [x] Testar Crash Bandicoot: analisado (666 funções), compilou, executou — GPU mostra primitivos mas output é lixo sem sentido
- [x] Diagnóstico: 5 problemas estruturais identificados (CALL_INDIRECT morto, boot EXE é só loader, 0 PsyQ matches, GPU opcodes faltando, sem VSync)

---

### Fase 16A — Fundação (necessário para QUALQUER jogo)

#### A1. `CALL_INDIRECT` com dispatch table
O recompiler gera uma tabela `endereço → ponteiro de função`. O runtime faz lookup nessa tabela. 70+ chamadas no Crash Bandicoot dependem disso.
- [x] Gerar tabela de dispatch no recompiler (mapa addr → func_ptr)
- [x] Implementar lookup no runtime (`CALL_INDIRECT` e `JUMP_INDIRECT`)
- [x] Fallback: log warning (não crash)

#### A2. GPU opcodes completos
Variantes faltantes: 0x22, 0x25, 0x26, 0x2A, 0x2D, 0x2E, 0x65, 0x6C, 0x7D, 0xAC, 0xFC, etc.
- [x] Mapear TODOS os opcodes GP0 do PS1 (tabela de referência da nocash)
- [x] Adicionar variantes semi-transparent/textured/opaque/blended que faltam
- [x] Testar com Crash Bandicoot
- [x] Corrigido entry point `PC=0x00000000` e exceção de `SYSCALL` paralizado no runtime

#### A3. CD-ROM no Runtime lendo disco real e Tabelas Básicas
O runtime recebe o `.bin` e responde a chamadas BIOS de leitura com dados reais. Reutiliza o `DiscReader` (VirtualFs) e lida com o hardware (`cdrom_controller`).
- [x] O runtime montar o path do `.bin`/ISO passado como argumento.
- [x] Hardware do CD-ROM responde com delays INT1 (Read Data Ready), INT2 (Seek Complete), e INT5 (GetID/Error) para emular tempos de drive mecânico e evitar jogo travar na identificação.
- [x] A BIOS fornece tabelas B0 e C0 válidas em memória RAM com "JR RA" hooks para evitar Crash Reference Null Pointer enqueuing exception handlers.
- [x] MDEC (decodificador de vídeo) emite stubs não bloqueantes para todos os opcodes para escapar do infinite busy-loop de inicialização em vídeos FMV.

**Critério de sucesso em progresso da Fase A**: O Runtime já não entra nos loops infinitos bloqueantes na BIOS e CD-ROM (GetID, tabelas enqueued, busy spin do MDEC). O próximo entrave é resolver os loops no I_STAT de interrupções e o CALL_INDIRECT que não realiza os saltos corretamente.

---

### Fase 16B — Overlays (necessário para jogos que carregam código do CD)

#### B1. Overlay Scanner ✅
- [x] Escanear arquivos do disco por código MIPS (heurística: patterns de instruções válidas)
- [x] Identificar endereço de carga de cada overlay (RAM base inference via LUI/ORI)
- [x] Catalogar overlays por jogo (genérico, sem hardcode)
- [x] Exportar overlays para TOML
- [x] CLI: `--scan-overlays <disc.bin> [output.toml]`
- [x] 16 testes unitários (`test_overlay_scanner.cpp`)

#### B2. Recompiler Multi-Segment ✅
- [x] Aceitar boot EXE + N overlays como inputs (via `[[overlays]]` no config TOML)
- [x] Gerar `recompiled_out.cpp` com TODAS as funções (boot + overlays)
- [x] Gerar `OverlayInfo` struct e `recomp_overlay_table` no output
- [x] Prefixar funções de overlay (`overlay_{name}__{addr:08X}`)
- [x] Auto-detectar funções por prólogo MIPS (ADDIU $sp,$sp,-N)
- [x] 76 testes recompiler passando

#### B3. Overlay Manager no Runtime ✅
- [x] Interceptar BIOS FileOpen/FileRead (via `notifyMemWrite()` em `file_io.cpp`)
- [x] Quando jogo carrega arquivo na RAM, consultar tabela de overlays
- [x] Ativar funções pré-recompiladas quando overlay é carregado
- [x] Desativar quando descarregado (conflito de região RAM → auto-deactivate)
- [x] 8 testes unitários (`test_overlay_manager.cpp`)

**Critério de sucesso**: Jogo carrega overlays do CD e o código executa nativamente (sem interpretador).

---

### Fase 16C — Polimento (qualidade visual + áudio)

- [x] VSync / Double Buffer (snapshot display buffer com mutex, renderer lê cópia)
- [x] BIOS mais completo: GPU_cw/GPU_cwp forwardam para GPU real, InitPAD/StartPAD/StopPAD com buffers + VBlank polling
- [x] MDEC FMV: DMA canais 0/1 conectados ao MDEC (writeCommand/dmaOutRead)
- [ ] Screenshots automáticos p/ diffing via DuckStation
- [ ] Fuzzing de input para teste de stress
- [ ] Diagnostico de crash dumps
- [ ] Live benchmarks
- [ ] Bug reports para jogos defeituosos

### Nota sobre Crash Bandicoot
O Crash usa formato proprietário `.NSD`/`.NSF` para níveis. O engine está no boot EXE (~288KB, 666 funções), mas dados e overlays estão nos `.NSF`. Jogos mais simples (RPGs, puzzles, 2D) podem funcionar já na Fase 16A se todo o código estiver no boot EXE.

---

### Fase 16D — PsyQ HLE (substituição da biblioteca PsyQ por implementações nativas)

**Problema fundamental**: Jogos PS1 são linkados estaticamente com a biblioteca PsyQ da Sony.
Ao recompilar o jogo, a PsyQ é recompilada junto. Esse código PsyQ faz:
- Busy-wait polling em registradores de hardware (SPUSTAT, GPUSTAT)
- Instalação de interrupt handlers via BIOS (C0 stubs que não funcionam)
- Manipulação direta de DMA, timers, e I/O ports
Resultado: jogos travam em loops infinitos na inicialização da PsyQ.
**Solução**: Identificar funções PsyQ e substituí-las por implementações HLE nativas,
igual ao PS2Recomp (ps2_stubs.cpp, 121KB) e N64Recomp (libultra substituída).

#### D0. Correções imediatas de hardware (desbloquear jogo AGORA) ✅
O código PsyQ recompilado acessa hardware diretamente. Correções mínimas para que
os busy-waits se resolvam sem precisar de HLE completo:
- [x] SPU: Atualizar `spuStat_` imediatamente ao escrever `spuCtrl_` (SPUCNT)
      → desbloqueia SpuInit que faz busy-wait em SPUSTAT & 0x07FF
- [x] BIOS C0: Implementar `ChangeClearRCnt` (0x0A) — stub HLE retorna 0
      → PsyQ VSync depende de rcnt[3].counter_ptr para busy-wait
- [x] BIOS C0: Implementar `SysEnqIntRP` (0x01) + `SysDeqIntRP` (0x02) — stubs HLE
      → Eventos entregues diretamente pela main thread, handler chain não necessária
- [~] Threading: Memory fence parcial (atomics no event system + `cdIntPending_`)
      → VBlank counter ainda usa read/write plain sem fence explícito

#### D0.5. Correções de CD-ROM e PsyQ INT mapping (Rayman) ✅
Correções descobertas empiricamente durante debug do Rayman:
- [x] CDROM command table corrigida: 0x0D=SetFilter, 0x0E=SetMode, 0x0F=GetParam,
      0x10=GetLocL, 0x11=GetLocP (antes estavam deslocados)
- [x] INT3→syncByte=2 mapping (PsyQ CdCommand espera syncByte==2 para INT3, não 3)
- [x] Cada tipo de INT escreve apenas no byte correto:
      INT1→readyByte=1, INT2→syncByte=2, INT3→syncByte=2, INT4→readyByte=4, INT5→ambos=5
- [x] HLE sector copy executado ANTES de sinalizar ready byte (evita race condition)
- [x] Callback dispatch via `drainPendingCallbacks` (pumps até 32 setores por drain)
- [x] gpuSwapCb: dispatch de func_801300AC(r4=4) a cada VBlank para desbloquear poll loop
- [x] Dispatch table fixes: func_8019F848 (counter loader), func_801328E0 (switch table inline)

#### D1. Infraestrutura PsyQ HLE no Runtime — parcial
Infraestrutura implementada inline no BIOS (sem módulos dedicados ainda):
- [x] `PsyqAddresses` struct com 9 campos configuráveis em `bios.h`
      (vblankCounter, cdSyncByte, cdReadyByte, cdRemaining, cdDestPtr,
       cdWordCount, cdDataCb, cdNotifyCb, gpuSwapCb)
- [x] 9 variáveis de ambiente `PSYQ_*` para configuração per-game em `main_host.cpp`
      (PSYQ_VSYNC_COUNTER, PSYQ_CD_SYNC_BYTE, PSYQ_CD_READY_BYTE, etc.)
- [ ] Criar `runtime/include/runtime/psyq/psyq_hle.h` — registry de funções HLE
- [ ] Criar `runtime/src/psyq/psyq_hle.cpp` — dispatcher e implementações
- [ ] Definir interface: `typedef void (*PsyqHleFunc)(uint8_t* rdram, recomp_context* ctx)`
- [ ] Criar mapa `nome_funcao → implementacao_hle`
- [ ] Subsistemas: `psyq_hle_gpu.cpp`, `psyq_hle_spu.cpp`, `psyq_hle_cd.cpp`, etc.

#### D2. Detecção de funções PsyQ em binários stripped
**Problema**: Rayman e Crash têm `stubs = []` no config porque não têm símbolos.
O matcher atual (Pass 1: nome exato, Pass 2: prefixo) não funciona sem nomes.
- [ ] Criar banco de assinaturas por bytes (estilo FLIRT/IDA) para PsyQ
      → Primeiros 16-32 bytes de cada função PsyQ conhecida
- [ ] Implementar matching por bytes no analyzer (Pass 3)
- [ ] Usar referências: ghidra_psx_ldr GDT files, ps1-bare-metal, PSY-Q 4.x headers
- [ ] Testar: re-analisar Rayman, verificar que funções são detectadas
- [ ] Fallback: permitir configuração manual de stubs no TOML por endereço

#### D3. Implementações HLE por subsistema

**SPU (libspu/libsnd)** — 17 funções:
- [ ] `SpuInit` → `spu.reset()`, configura SPU para estado limpo
- [ ] `SpuSetKey` → `spu.keyOn(voices)` / `spu.keyOff(voices)`
- [ ] `SpuSetCommonAttr` → setar master volume, reverb, CD volume
- [ ] `SpuSetVoiceAttr` → configurar pitch, volume, ADSR por voice
- [ ] `SpuSetTransferMode` → setar modo DMA/manual
- [ ] `SpuSetTransferStartAddr` → setar endereço base de transferência na SPU RAM
- [ ] `SpuWrite` → copiar dados do RAM para SPU RAM
- [ ] `SpuIsTransferCompleted` → retornar 1 (transferência instantânea)
- [ ] `SsInit`/`SsStart`/`SsSetMVol` → inicializar sequenciador de áudio

**GPU (libgpu/libgs)** — 31 funções:
- [x] `GPU_cw` (B0:0x49) → envia GP0 word para GPU
- [x] `GPU_cwp` (B0:0x4A) → envia múltiplas GP0 words do RAM
- [x] `gpuSwapCb` → dispatch de callback de swap de display a cada VBlank
- [ ] `ResetGraph` → `gpu.reset()`, configurar modo gráfico
- [ ] `SetDefDispEnv`/`SetDefDrawEnv` → preencher structs DispEnv/DrawEnv
- [ ] `PutDispEnv`/`PutDrawEnv` → aplicar environment via GP1/GP0
- [ ] `DrawPrim` → submeter primitivo para GPU (converter struct → GP0 commands)
- [ ] `DrawOTag`/`DrawOTagEnv` → percorrer ordering table e submeter
- [ ] `ClearImage`/`LoadImage`/`StoreImage`/`MoveImage` → VRAM transfers
- [ ] `DrawSync` → esperar GPU terminar (retornar imediatamente ou wait frame)
- [ ] `VSync` → esperar VBlank (sincronizar com main thread a 60fps)
- [ ] `LoadTPage`/`LoadClut`/`GetTPage`/`GetClut` → helpers de textura
- [ ] `ClearOTag`/`ClearOTagR` → inicializar ordering table em RAM
- [ ] `FntLoad`/`FntOpen`/`FntFlush` → debug font (stub ou implementar)

**CD-ROM (libcd)** — 6 funções (HLE via hardware emulation, não substituição):
- [x] CD-ROM controller com 20 comandos: GetStat, SetLoc, Play, ReadN, Stop,
      Pause, Init, Mute, Demute, SetFilter, SetMode, GetParam, GetLocL, GetLocP,
      GetTN, GetTD, SeekL, SeekP, Test, GetID, ReadS
- [x] `triggerCdromEvent` — HLE de INT1/2/3/4/5 com sector copy e byte mapping
- [x] `drainPendingCallbacks` — pumping loop (até 32 setores por drain)
- [ ] `CdSearchFile` → buscar arquivo no filesystem ISO9660 (HLE puro)

**Controller (libpad)** — 5 funções:
- [x] `InitPAD` (B0:0x12) → armazena endereços de buffer dos 2 pads
- [x] `StartPAD` (B0:0x13) / `StopPAD` (B0:0x14) → flag `padActive_`
- [x] `updatePadBuffers` → escreve status digital (SIO 4-byte) na RAM a cada VBlank
- [ ] `PadInitDirect` → configurar polling de controle (variante alternativa)
- [ ] `PadStartCom`/`PadStopCom` → iniciar/parar comunicação SIO

**GTE (libgte)** — 23 funções:
- [ ] `InitGeom` → inicializar GTE (já temos GTE no runtime)
- [ ] `SetGeomOffset`/`SetGeomScreen` → escrever registros de controle GTE
- [ ] `SetRotMatrix`/`SetTransMatrix`/`SetLightMatrix` → CTC2 batch
- [ ] `RotTransPers`/`RotTransPers3`/`RotTrans` → executar commands GTE
- [ ] Nota: muitas funções GTE são thin wrappers sobre CTC2/MTC2/COP2,
      podem continuar recompiladas se o GTE do runtime funciona

**VSync/System** — 7 funções:
- [x] VBlank counter incrementado em `triggerVBlankEvent` (game polls diretamente)
- [x] Root counter events (RCnt0/1/2 target+overflow) disparados a cada VBlank
- [ ] `VSync(mode)` HLE explícito → mode=0: esperar próximo VBlank; mode>0: esperar N frames
- [ ] `VSyncCallback` → registrar callback chamado a cada VBlank
- [ ] `DrawSync` → esperar GPU idle
- [ ] `ResetCallback`/`StopCallback`/`RestartCallback` → gerenciar callbacks

#### D4. Modificar Recompiler para gerar chamadas HLE
- [ ] Ler `[[stubs]]` com campo `name` e `subsystem` do TOML
- [ ] Em vez de `{ /* PsyQ Stub */ }`, gerar `{ psyq_hle_<Name>(rdram, ctx); }`
- [ ] Tratar `[[skips]]` → gerar `{ return; }`
- [ ] Tratar `[[passthroughs]]` → manter código recompilado (não mudar)
- [ ] Adicionar `#include "runtime/psyq/psyq_hle.h"` no output

#### D5. Configuração por jogo
- [ ] Permitir override manual de stubs no TOML:
      `[[stubs]] name = "SpuInit" address = "0x801B5194"`
- [ ] Ferramenta de detecção semi-automática: comparar disassembly com base de assinaturas
- [ ] Documentar processo de adicionar suporte a novo jogo

#### D6. Bugs ativos (SEGFAULT / regressões)
- [ ] SEGFAULT na última execução do Rayman (exit 139) — deque assertion failure
      → Possível corrupção de stack no dispatch de gpuSwapCb ou CDROM responseFifo_
- [ ] Dispatch 0x801328E0 ainda aparece como unknown apesar do fix inline
      → Verificar se build recompilou corretamente o recompiled_out.cpp
- [x] PSYQ_CD_STATUS_HW hardcoded para Crash (0x80053936) — agora é campo `cdStatusHw`
      em `PsyqAddresses` + env var `PSYQ_CD_STATUS_HW`
- [x] `PsyqAddresses` defaults eram endereços do Crash — agora todos default=0,
      com warnings se endereços críticos não forem configurados
- [ ] Cleanup de debug logging temporário ([EVTDBG], [VSYNC-DBG], [DRAIN-DBG], etc.)
- [ ] Patches manuais no recompiled_out.cpp (debug prints, drainPendingCallbacks
      injetados, switch table inline) — devem ser resolvidos no recompiler/runtime

**Critério de sucesso**: Rayman passa da inicialização SPU/GPU e mostra gráficos na tela.
**Progresso parcial**: Logo da Ubisoft foi visível em execução anterior. Última execução
com gpuSwapCb habilitado progrediu mais longe (passou poll loop) mas crashou com SEGFAULT.

---

### Fase 16E — Roadmap para 100% funcional

Plano priorizado para tornar o ps1recomp genérico e funcional com qualquer jogo PS1.

#### Camada 1 — Fix imediato (desbloquear Rayman) 🔴

| # | Item | Status | Impacto | Esforço |
|---|------|--------|---------|---------|
| 1 | Fix SEGFAULT (deque assertion / gpuSwapCb stack corruption) | ⬜ | Desbloqueia Rayman | Médio |
| 2 | Descobrir `PSYQ_CD_STATUS_HW` para Rayman | ⬜ | Completa config per-game | Baixo |
| 3 | Fix dispatch 0x801328E0 (verificar se fix foi aplicado corretamente) | ⬜ | Elimina crash path | Baixo |

**Detalhes:**
- [ ] **SEGFAULT**: `std::deque::operator[]` assertion failure — provável acesso a
      CDROM `responseFifo_` vazia OU corrupção de stack no dispatch de gpuSwapCb.
      Investigar: (a) bounds check no responseFifo_, (b) caller-saved register
      save/restore no gpuSwapCb dispatch, (c) re-entrancy do dispatch
- [ ] **cdStatusHw Rayman**: Buscar no binário a função PsyQ que lê um halfword
      como gate antes de acessar HW registers do CDROM. Padrão: `lhu $v0, X($gp)`
      seguido de `beqz $v0, loop`
- [ ] **Dispatch 0x801328E0**: Verificar se o inline if/else no recompiled_out.cpp
      foi compilado (grep no .o), ou se há segunda call-site que não foi patcheada

#### Camada 2 — Recompiler genérico (funcionar com QUALQUER jogo) 🟡

| # | Item | Status | Impacto | Esforço |
|---|------|--------|---------|---------|
| 4 | Jump tables / switch no recompiler | ⬜ | Elimina patches manuais | Alto |
| 5 | Mid-function entry points no recompiler | ⬜ | Elimina patches manuais | Médio |
| 6 | PsyQ byte-signature detection (D2) | ⬜ | Auto-detecta stubs | Alto |
| 7 | Config TOML per-game (substituir env vars) | ⬜ | UX para novos jogos | Médio |
| 8 | Auto-inject `drainPendingCallbacks` em busy-waits | ⬜ | Remove patches manuais | Médio |

**Detalhes:**
- [ ] **Jump tables**: Detectar padrão `lui/addiu/sll/addu/lw/jr $v0` no analyzer.
      Extrair endereços da jump table do .rodata. Emitir `switch(idx) { case 0: goto L_addr; ... }`
      em vez de `JUMP_INDIRECT`. Testar com func_80132898 do Rayman (3 targets)
- [ ] **Mid-function entries**: No analyzer, detectar `JAL addr` onde `addr` está no
      meio de outra função. Criar entry-point secundário que carrega estado parcial
      e faz `goto L_addr` dentro da função host. Testar com func_8019F848→8019F850
- [ ] **PsyQ byte-signatures**: Criar DB de primeiros 16-32 bytes de funções PsyQ
      conhecidas (SpuInit, CdInit, VSync, DrawOTag, etc.). Implementar Pass 3 no
      analyzer: para cada função sem nome, comparar bytes com DB. Marcar matches
      como `[[stubs]]` no config TOML. Referências: ghidra_psx_ldr GDT files
- [ ] **Config TOML**: Criar `configs/rayman.toml` com seção `[psyq_addresses]`
      contendo todos os endereços atualmente passados por env vars. Runtime lê
      TOML se `--config rayman.toml` passado, senão usa env vars como fallback
- [ ] **Auto-inject drain**: No emitter, detectar padrões de polling loop:
      `L: lbu/lhu $v0, addr; beqz/bnez $v0, L` e inserir
      `if (ctx->bios) ctx->bios->drainPendingCallbacks();` antes do branch-back

#### Camada 3 — PsyQ HLE completo (jogos complexos) 🟢

| # | Item | Status | Impacto | Esforço |
|---|------|--------|---------|---------|
| 9 | VSync/DrawSync HLE | ⬜ | Todo jogo precisa | Médio |
| 10 | DrawOTag/ClearOTag HLE | ⬜ | Jogos 3D | Médio |
| 11 | ResetGraph + DispEnv/DrawEnv HLE | ⬜ | Setup gráfico | Médio |
| 12 | SPU HLE (SpuInit, SpuSetKey, etc) | ⬜ | Áudio | Médio |
| 13 | Memory fences no VBlank counter | ⬜ | Race condition | Baixo |

**Detalhes:**
- [ ] **VSync HLE**: Quando detectado como stub, `VSync(0)` faz `while(vblank_counter == old) { yield(); }`.
      `VSync(n)` espera N frames. `DrawSync(0)` retorna imediatamente (GPU é síncrona no recomp)
- [ ] **DrawOTag HLE**: Percorrer linked list na RAM, submeter cada pacote para GPU via
      `gpu->sendLinkedList()`. Essencial para jogos 3D que usam ordering table
- [ ] **ResetGraph HLE**: `gpu->reset()`, configurar resolução, ativar display.
      `SetDefDispEnv`/`SetDefDrawEnv` preenchem structs na RAM do jogo.
      `PutDispEnv`/`PutDrawEnv` enviam GP1/GP0 commands
- [ ] **SPU HLE**: `SpuInit → spu.reset()`, `SpuSetKey → spu.keyOn/Off()`,
      `SpuSetVoiceAttr → configurar voice`, `SpuWrite → copiar RAM→SPU RAM`
- [ ] **Memory fences**: Usar `std::atomic` ou `std::atomic_thread_fence` no
      write do VBlank counter em `triggerVBlankEvent()`. Game thread lê com acquire

#### Camada 4 — Validação multi-jogo 🔵

| # | Item | Status | Impacto | Esforço |
|---|------|--------|---------|---------|
| 14 | Testar 3+ jogos diferentes | ⬜ | Validação | Médio |
| 15 | Documentar processo per-game | ⬜ | Reprodutibilidade | Baixo |
| 16 | Cleanup debug logging | ⬜ | Qualidade | Baixo |

**Detalhes:**
- [ ] **Multi-jogo**: Testar pelo menos: (a) Rayman — 2D platformer, (b) um RPG
      (Final Fantasy VII/VIII/IX ou Chrono Cross), (c) um jogo 3D simples
      (Spyro, Tomb Raider). Cada um exercita paths diferentes do PsyQ
- [ ] **Processo per-game**: Documentar em `docs/adding_game_support.md`:
      1. Rodar analyzer no .bin → gerar config.toml
      2. Identificar PsyQ addresses (via Ghidra ou patterns)
      3. Rodar recompiler → gerar recompiled_out.cpp
      4. Compilar runtime → testar
      5. Iterar em stubs/patches até funcionar
- [ ] **Cleanup**: Remover todos os `fmt::print` temporários de debug:
      [EVTDBG], [VSYNC-DBG], [DRAIN-DBG], [F8D0], [GAME], [POLL-DBG],
      [SPU_INIT], [SPU2], [SPUINIT], [CD-INT1], [CD-HLE-COPY], [CDINIT]

---

## Fase 17: TCC ✅

### Implementacao
- [x] Mermaid / draw.io architecture charts → `docs/tcc_architecture_diagrams.md` (12 diagramas)
- [x] Test reports extract → `docs/tcc_test_report.md` (158 testes documentados)
- [x] Monograph refinement → `docs/tcc_monograph_structure.md` (7 capítulos + apêndices)
- [x] Demo assets e build final → `tools/demo_build.sh`

