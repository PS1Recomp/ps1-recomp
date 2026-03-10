# Análise do Repositório: DuckStation

**Repositório Original:** [stenzek/duckstation](https://github.com/stenzek/duckstation)

## Visão Geral
DuckStation é indiscutivelmente o emulador de PlayStation 1 focado em jogabilidade e performance mais avançado da atualidade. Escrito em C++, ele suporta Recompilação JIT (Just-In-Time), renderização por Hardware (OpenGL/Vulkan/D3D), correções de precisão (PGXP) e possui uma vasta compatibilidade.

## O que o `ps1-recomp` pode absorver do DuckStation?
Nosso projeto, sendo um recompilador *Estático* AOT (Ahead-of-Time), não compartilha do núcleo JIT do DuckStation, mas ambos os projetos precisam de um poderoso ambiente de *Runtime* (HLE). O DuckStation é o "gabarito perfeito" para:

1. **Emulação do Componente Core:**
   - O diretório `src/core/` contém o código do sistema bruto do PS1. As implementações de barramento (Bus), de interrupções da BIOS HLE e controle de periféricos (como Memory Cards e Controladores) são o padrão-ouro.
   
2. **Ciclos de Clock e Timings (Exemplo real: CDROM):**
   - Ao mergulhar em `src/core/cdrom.cpp`, notamos o uso exaustivo do sistema de `TimingEvent` atrelado aos `GlobalTickCounter`. Para resolver nosso problema com `CdInit`, DuckStation define `INIT_TICKS = 4000000` ciclos base. Em emuladores imaturos, respostas de interrupções de hardware são despachadas imediatamente; O DuckStation prova que **espalhar os `ACK` e as respostas `INT_x` via scheduler de TickCycles** é a única forma de evitar que os jogos travem em loops de polling fechados. Em nosso projeto, a classe implementando o atraso no `cdrom_controller.cpp` é a direção correta.

3. **Arquitetura GPU e PGXP:**
   - Quando for o momento de escrever os interpretadores de lista de comandos DMA da GPU no nosso projeto, o renderizador de hardware do DuckStation é a melhor referência do mercado.

## Estrutura do Workspace
O repositório do DuckStation agora habita localmente em `reference_repos/duckstation`. Caso nos deparemos com "Comportamento Indocumentado" de algum jogo específico, será extremamente útil `grep`'ar (pesquisar) os arquivos `.cpp` do DuckStation para descobrir se há algum workaround conhecido.
