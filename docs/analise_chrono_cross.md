# Análise do Repositório: Chrono Cross Decompilation

**Repositório Original:** [jdperos/chrono-cross-decomp](https://github.com/jdperos/chrono-cross-decomp)

## Visão Geral
Este é um projeto de descompilação (matching decompilation) do jogo *Chrono Cross* (SLPS 023.64). O objetivo de um "matching decomp" é reescrever o código do jogo em C de forma que, quando compilado, gere um binário bit-a-bit idêntico ao original do PS1.

## Ferramentas e Infraestrutura Relevantes para o `ps1-recomp`
A infraestrutura deste projeto é uma mina de ouro de referências sobre como binários originais de PS1 eram construídos:

1. **Compilador Alvo (Target Compiler):**
   - Eles usam `gcc 2.8.1-psx` em conjunto com o assembler `maspsx` (um assembler compatível com PSY-Q).
   - *Por que isso importa para nós?* Nosso *Static Analyzer* (`ps1xAnalyzer`) lida diretamente com os padrões de assembly gerados por essa exata versão de compilador. Analisar o output desse gcc pode nos ajudar a melhorar o reconhecimento de funções e blocos lógicos.

2. **Splat (Binary Splitting) e Context Generation:**
   - Eles dividem o `.exe` massivo em arquivos `.s` (assembly) menores usando `splat`. O `splat` automatiza a separação de seções `.text`, `.data`, `.rodata` e `.bss`.
   - Ao mergulhar em `tools/cc_decompile.py`, observei o script extraindo o "Contexto C" (`cc_m2ctx`) que mapeia todos os `structs` e instâncias do MIPS antes de enviar para ferramentas como `m2c`. Isso prova que a descompilação de PS1 precisa impreterivelmente de *Type Information* injetada via headers para que os desassembladores tenham sucesso. No nosso Analisador (`ps1xAnalyzer`), precisaríamos de um parser de C headers forte se quisermos reconstruir chamadas de função complexas oriundas de registradores (como `a0`, `a1` recebendo ponteiros de *structs*).

3. **PSY-Q SDK e BSS RAM Mapping:**
   - O projeto separa as funções do jogo das funções da biblioteca oficial da Sony (PsyQ). 
   - Estrutura de diretórios como `include/psyq/` pode servir de referência perfeita para a construção dos nossos próprios "stubs" HLE da BIOS no nosso ambiente de *Runtime*.
   - **Deep Dive (slps_023.64.yaml e symbol_addrs):** Ao vasculhar a configuração do linker (`yaml`), descobri o exato layout de memória da PsyQ. Módulos como `libcd/bios`, `libc2/setjmp`, e `libgpu` são estaticamente linkados em endereços fixos na RAM (ex: `0x80010000` em diante). Variáveis de status interno como `CdSync` estão mapeadas via BSS diretamente no arquivo de símbolos (ex: `0x800231E4`). Sabendo que jogos de PS1 compilavam a PsyQ inteira para dentro do executável, *nosso Recompilador* (`ps1xAnalyzer`) **precisa** possuir uma database dessas assinaturas de bibliotecas em assembly, ou varrerá código de BIOS emulada como se fosse código do jogo, causando conflito de estado no PC.

## Conclusão de Absorção
O projeto não nos fornece código direto para nosso emulador/recompilador, mas fornece o "molde" perfeito. Sempre que tivermos dúvida de como uma macro de C ou um construto da PsyQ se traduz (ou como a stack é montada nesses binários), o código fonte do Chrono Cross servirá como **referência viva** da engenharia reversa do PS1.
