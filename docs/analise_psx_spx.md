# Análise do Repositório: PSX-SPX (No$PSX Specs)

**Repositório Original:** [psx-spx/psx-spx.github.io](https://github.com/psx-spx/psx-spx.github.io)

## Visão Geral
Este repositório é a versão em Markdown da lendária documentação "No$PSX Specifications" criada por Martin Korth. É amplamente considerada a **Bíblia da Arquitetura do PlayStation 1**.

## Documentos Absolutamente Críticos para o `ps1-recomp`
Nossa emulação de Alto Nível (HLE) no `runtime` depende inteiramente de replicar perfeitamente o comportamento do hardware como documentado aqui. Arquivos essenciais absorvidos:

1. **`kernelbios.md` e Especificações ABI**
   - Detalha todas as funções da BIOS (A0, B0, C0). 
   - **Deep Dive (`setjmp/longjmp`):** A documentação afirma explicitly que `setjmp` salva um buffer de *30h bytes* (48 bytes) contendo registradores ABI (S0-S7, GP, SP, FP, RA). Ele retorna 0 na chamada inicial, e o caller do `longjmp` é instruído a "tomar cuidado para que param seja não-zero, para que o programador diferencie a primeira chamada de um rollback". Isso bate 100% com a lógica que codificamos recentemente para consertar o Silent Hill, validando nossa emulação de exceção.

2. **`cdromdrive.md` e `cdrominternalinfoonpsxcdromcontroller.md`**
   - Contém a máquina de estados exata do controlador de CD, os tempos de resposta para as interrupções (`INT1`, `INT2`, `INT3`) e como os comandos da porta `1F80180x` interagem.
   - Ideal para ajeitar nossos falsos *timeouts* de inicialização (`CdInit`).

3. **`graphicsprocessingunitgpu.md` e `geometrytransformationenginegte.md`**
   - Referência central de quando chegarmos na implementação/otimização da renderização. Os formatos de comando da GPU e matemática de GTE (coprocessador) são difíceis de inferir sem esta documentação.

4. **`interrupts.md` e `timers.md`**
   - Controladores lógicos de DMA e os timers de Root Counter do hardware do PS1, essenciais para que jogos rodem na velocidade correta e não entrem em loops de espera infinitos.

## Conclusão de Absorção
Este repositório é a fonte da verdade para o projeto. Ele ficará fisicamente na pasta `reference_repos/psx-spx.github.io/docs` e servirá de material de consulta primário sempre que formos codificar a parte do `runtime` (emulação) do PS1 Recomp.
