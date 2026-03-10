# Análise do Repositório: PSX-SPX (No$PSX Specs)

**Repositório Original:** [psx-spx/psx-spx.github.io](https://github.com/psx-spx/psx-spx.github.io)

## Visão Geral
Este repositório é a versão em Markdown da lendária documentação "No$PSX Specifications" criada por Martin Korth. É amplamente considerada a **Bíblia da Arquitetura do PlayStation 1**.

## Documentos Absolutamente Críticos para o `ps1-recomp`
Nossa emulação de Alto Nível (HLE) no `runtime` depende inteiramente de replicar perfeitamente o comportamento do hardware como documentado aqui. Arquivos essenciais absorvidos:

1. **`kernelbios.md`**
   - Detalha todas as funções da BIOS (A0, B0, C0), incluindo as que acabamos de lutar com (como `setjmp`, `longjmp`, matrizes de interrupção e chamadas de rotina do CDROM).
   - Deve ser consultado sempre que formos emular uma nova syscall na nossa classe `Bios`.

2. **`cdromdrive.md` e `cdrominternalinfoonpsxcdromcontroller.md`**
   - Contém a máquina de estados exata do controlador de CD, os tempos de resposta para as interrupções (`INT1`, `INT2`, `INT3`) e como os comandos da porta `1F80180x` interagem.
   - Ideal para ajeitar nossos falsos *timeouts* de inicialização (`CdInit`).

3. **`graphicsprocessingunitgpu.md` e `geometrytransformationenginegte.md`**
   - Referência central de quando chegarmos na implementação/otimização da renderização. Os formatos de comando da GPU e matemática de GTE (coprocessador) são difíceis de inferir sem esta documentação.

4. **`interrupts.md` e `timers.md`**
   - Controladores lógicos de DMA e os timers de Root Counter do hardware do PS1, essenciais para que jogos rodem na velocidade correta e não entrem em loops de espera infinitos.

## Conclusão de Absorção
Este repositório é a fonte da verdade para o projeto. Ele ficará fisicamente na pasta `reference_repos/psx-spx.github.io/docs` e servirá de material de consulta primário sempre que formos codificar a parte do `runtime` (emulação) do PS1 Recomp.
