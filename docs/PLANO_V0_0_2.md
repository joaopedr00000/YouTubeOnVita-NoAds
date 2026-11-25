# Plano para YouTubeOnVita v0.0.2

Estado: **planejamento, sem implementar, compilar ou publicar versão nova.**
Versão de referência: v0.0.1 já publicada, com build Full/Lite validada no GitHub Actions e avaliação inicial de estabilidade pelo usuário.

## P0 — Corrigir o nome da aplicação

- [ ] Trocar o nome exibido no LiveArea/param.sfo de **YouTube Candidate** por **YouTube** e de **YouTube Candidate Lite** por **YouTube Lite**.
- [ ] Em `CMakeLists.txt`, atualizar **somente** os nomes de exibição para o modo com `YTV_CUSTOM_COBALT_DIR`, que hoje os define explicitamente (linhas próximas de 395–401).
- [ ] **Preservar os Title IDs `YTVNX0001` e `YTVNX0002`** para atualização in-place e preservação das bolhas já instaladas. Não substituí-los por `YTVCB0001/0002`, que correspondem a outra modalidade de pacote.
- [ ] Padronizar nome do VPK de distribuição e dos outputs internos (incluindo `.github/workflows/build-vita.yml`), mantendo as verificações para empacotar o Cobalt personalizado correto.
- [ ] Corrigir a versão interna passada ao VitaSDK: workflow atual usa `YTV_COBALT_APP_VERSION=00.65`, herdada da versão original, apesar de o fork já ter release v0.0.1. Aplicar numeração coerente ao futuro v0.0.2, verificando o formato SFO aceito, sem tocar no release existente.
- [ ] Futuramente testar a instalação por cima da v0.0.1, preservando `ux0:data/youtubevita/` e evitando bolha duplicada.

## P0 — Investigar distorções visuais intermitentes

**Evidência disponível:** relato de imagem ocasionalmente distorcida. Ainda não há imagem, gravação ou log sincronizado que indique se a corrupção aparece nos quadros do vídeo ou nos elementos da interface. As hipóteses abaixo não são causas confirmadas.

### Suspeitas de maior prioridade, identificadas no código

- [ ] **Reutilização prematura de buffers de vídeo:** `experiments/cobalt/vita/port_player.cpp` usa quadros YUV mapeados diretamente para a GPU, pool com até seis slots e `kGpuFramesInFlight=4`. Auditar o ciclo `Frame.used` / `retired_at` / `displayed`, a contagem `g_swaps` e o momento em que cada slot pode ser sobrescrito. Verificar dessincronização entre comandos de renderização ainda na fila e decodificação de novos quadros.
- [ ] **Plano YUV e descritores de textura:** `upload_current` vincula três planos por `sceGxmTextureInitLinear` a cada novo frame. Checar retorno de inicialização, validade dos três planos, restauração de textura vinculada e uso de alocações que ainda possam estar sendo lidas pela GPU. Instrumentar erros com limites de log.
- [ ] **Largura/pitch, cropping e mudanças de resolução:** conferir `align32(stream_w)`, `align16(stream_h)`, `SceAvcdecPicture.framePitch`, `frameWidth/Height`, layout dos planos I420 e regiões de conteúdo. Testar tamanhos não padronizados (ex. 1190×720), 480p/720p e modos Original/Zoom/Stretch, com mudança automática de qualidade.
- [ ] **Buffers geométricos da UI:** `experiments/cobalt/vita/port_graphics.cpp` documenta defeitos anteriores de **triângulos esticados sobre miniaturas/textos** quando os buffers GL_DYNAMIC_DRAW/GL_STREAM_DRAW eram mantidos em scratch memory reutilizada. O código atual chama `vglSetupScratchMemory(GL_FALSE, GL_FALSE)`. Verificar se a distorção remanescente é na UI e se existem caminhos ou alocações transientes não cobertos por essa medida.
- [ ] **Uploads de pixels para textura:** examinar `repack_upload`, `GL_UNPACK_ALIGNMENT` e `GL_UNPACK_ROW_LENGTH` se os artefatos afetarem especificamente thumbnails, imagens ou texto. Medir também as reservas de RAM/VRAM do VitaGL durante a falha.

### Diagnóstico planejado

- [ ] Diferenciar visualmente artefatos em vídeo (blocos, faixas, cores/quadros trocados) de artefatos na UI (triângulos, miniaturas deformadas e textos).
- [ ] Comparar 480p e 720p, Bilinear e Nearest, Original/Zoom/Stretch, inclusive troca de vídeos/qualidade e reprodução prolongada.
- [ ] Preservar e analisar os logs contemporâneos `ux0:data/youtubevita/starboard15.log` e `cobalt_boot.log`, e `*_prev.log` se aplicável. Correlacionar `YTV_LIFECYCLE_PLAYER_STATS`, `YTV_LIFECYCLE_PLAYER_SPS`, `YTV_LIFECYCLE_VIDEO_ASPECT_FRAME`, `YTV_LIFECYCLE_PLAYER_MEM`, `YTV_FPS` e uso de memória de VitaGL.
- [ ] Somente depois de identificar o caminho de falha, implementar correção mínima e testes de alternância/resolução com validação no PS Vita.

## Critérios para v0.0.2

- Nome visível correto em Full e Lite, com mesmos IDs e perfil preservado.
- Distorção reproduzida, causa demonstrada pelos testes e correção confirmada no hardware; se permanecer hipótese, documentar como tal.
- Sem regressões no Cobalt experimental, navegação, qualidade de vídeo, funções atuais ou estabilidade prolongada.
- Não publicar como versão pronta antes de testar.

*Este arquivo registra apenas o backlog técnico; não modifica código de execução, não gera VPKs e não publica release.*
