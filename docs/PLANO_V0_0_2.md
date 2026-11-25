# Plano para YouTubeOnVita v0.0.2

Estado: **planejamento, sem implementar, compilar ou publicar versão nova.**
Versão de referência: v0.0.1 já publicada, com build Full/Lite validada no GitHub Actions e avaliação inicial de estabilidade pelo usuário.

## P0 — Corrigir o nome da aplicação

- [x] Preparado em branch de desenvolvimento: trocar o nome exibido no LiveArea/param.sfo de **YouTube Candidate** por **YouTube** e de **YouTube Candidate Lite** por **YouTube Lite**. **Ainda não compilado nem testado.**
- [x] Preparado em `CMakeLists.txt` para o modo com `YTV_CUSTOM_COBALT_DIR`; manter a verificação após a futura compilação.
- [ ] **Preservar os Title IDs `YTVNX0001` e `YTVNX0002`** para atualização in-place e preservação das bolhas já instaladas. Não substituí-los por `YTVCB0001/0002`, que correspondem a outra modalidade de pacote.
- [x] Preparado: nomes internos `YouTube-Full.vpk` e `YouTube-Lite.vpk` no modo personalizado; adaptados `CMakeLists.txt` e `.github/workflows/build-vita.yml`, preservando validação de hash do Cobalt. **A testar antes do release.**
- [x] Preparado para **v0.0.2**: `YTV_COBALT_APP_VERSION=00.02` no workflow e como padrão do CMake, correspondente ao formato interno `NN.NN` do Vita SFO. Ainda falta validar o arquivo `param.sfo` na futura build. A v0.0.1 publicada permanece intacta.
- [ ] Futuramente testar a instalação por cima da v0.0.1, **sem modificar** `ux0:data/youtubevita/`, criando `ux0:data/youtubevita-noads/` separadamente, sem migrar tokens automaticamente e evitando bolha duplicada.

## Identidade do fork para a v0.0.2

- [x] Adicionar assinatura pública **Proudly made in 🇧🇷**, usando a bandeira, sem escrever o nome do país na assinatura.
- [x] README passa a identificar este fork e preservar os créditos do port original.
- [ ] Quando houver build, conferir a apresentação correta da assinatura nas páginas públicas e considerar uso de arte específica se for exibida dentro do Vita (o suporte a emoji da fonte não está confirmado).

## P0 — Isolamento dos dados do fork (solicitado pelo autor original)

- [x] Preparado na branch: diretório próprio `ux0:data/youtubevita-noads/` em Starboard, Cobalt, configurações, armazenamento de sessão, logs, cache, temporários, downloads e diagnósticos.
- [x] Separar também as capturas em `ux0:picture/YouTubeVita-NoAds/`.
- [x] Alterar o domínio de derivação da chave de criptografia das sessões para o fork, sem copiar automaticamente tokens do diretório antigo.
- [x] Adicionar verificação estática `tools/test_fork_data_isolation.cjs` para a versão em desenvolvimento.
- [ ] Confirmar em hardware que os dois aplicativos não compartilham dados. A primeira execução da v0.0.2 exigirá novo login e terá configurações padrão; os arquivos anteriores permanecerão intocados.
- [ ] Antes da próxima release, revisar documentação dos caminhos de logs/testes e confirmar que Cobalt obtém os novos diretórios da plataforma.

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
- [ ] Preservar e analisar os logs contemporâneos `ux0:data/youtubevita-noads/starboard15.log` e `cobalt_boot.log`, e `*_prev.log` se aplicável. Correlacionar `YTV_LIFECYCLE_PLAYER_STATS`, `YTV_LIFECYCLE_PLAYER_SPS`, `YTV_LIFECYCLE_VIDEO_ASPECT_FRAME`, `YTV_LIFECYCLE_PLAYER_MEM`, `YTV_FPS` e uso de memória de VitaGL.
- [ ] Somente depois de identificar o caminho de falha, implementar correção mínima e testes de alternância/resolução com validação no PS Vita.

## Critérios para v0.0.2

- Nome visível correto em Full e Lite, com mesmos IDs e perfil preservado.
- Distorção reproduzida, causa demonstrada pelos testes e correção confirmada no hardware; se permanecer hipótese, documentar como tal.
- Sem regressões no Cobalt experimental, navegação, qualidade de vídeo, funções atuais ou estabilidade prolongada.
- Não publicar como versão pronta antes de testar.

*Este arquivo registra apenas o backlog técnico; não modifica código de execução, não gera VPKs e não publica release.*
