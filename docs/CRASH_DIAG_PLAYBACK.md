# Diagnóstico de fechamento após alguns minutos de vídeo

**Problema observado:** o PS Vita encerra inesperadamente a sessão após alguns minutos. Não há
dump `psp2core-*.psp2dmp` do aparelho anexado ao repositório: portanto, ainda não há
confirmação da causa. A compilação no CI não prova estabilidade de reprodução contínua.

## Alterações deste candidato

- O worker do SponsorBlock realiza consultas HTTPS e decide se precisa saltar
  trecho patrocinado, mas **nunca** chama o Cobalt diretamente, nem segura
  `sb_mutex` durante um comando de MediaSession.
- `Skipad` e `Seekto` são enviados à fila do Starboard para execução na
  **thread principal**. Eventos antigos são cancelados ao trocar vídeo ou
  encerrar a sessão e revalidados contra o ID/geração e as permissões atuais.
- O AdBlock DNS e a lista de endereços bloqueados não mudaram.
- A cada 30 segundos de reprodução o log recebe
  `YTV_LIFECYCLE_PLAYER_MEM playback_30s`, além das métricas
  `YTV_LIFECYCLE_PLAYER_STATS` já existentes, para investigar esgotamento de
  memória ou excesso de alvos de vídeo.
- Avisos de impossibilidade de agendar evento são limitados para evitar
  crescimento descontrolado de log.

## Como verificar no aparelho sem desativar o AdBlock

1. Mantenha **AdBlock: On**, coloque **SponsorBlock: Off**, reinicie o app e
   reproduza **o mesmo vídeo** por pelo menos o tempo em que normalmente ocorre
   o fechamento. Não altere outras opções nesse teste.
2. Mantenha **AdBlock: On**, volte ao **SponsorBlock: Sponsors**, reproduza o
   mesmo vídeo e observe se o problema volta.
3. Repita em 480p e 720p para distinguir memória/decoder de SponsorBlock.
4. Caso ocorra o fechamento, copie com o VitaShell os arquivos
   `ux0:data/youtubevita/cobalt_boot.log`,
   `ux0:data/youtubevita/starboard15.log` e respectivos `*_prev.log`,
   além do `psp2core-*.psp2dmp` mais recente, quando disponível.
5. Compare os últimos `YTV_LIFECYCLE_PLAYER_STATS` e
   `YTV_LIFECYCLE_PLAYER_MEM` com os registros
   `YTV_SPONSORBLOCK` e `YTV_ADBLOCK` anteriores à queda.

**Privacidade:** revise logs antes de compartilhar: podem conter identificadores
de vídeos, caminhos, dados de rede ou sessão. Não envie dados de login,
tokens ou outros arquivos do perfil.

Este patch precisa passar no build Full/Lite e num teste real prolongado antes
de ser lançado como uma versão final; não modifica a versão instalada do usuário.

## Evidências de reprodução coletadas em 25/11/2026

Análise de `cobalt_boot.log` e `starboard15.log` fornecidos pelo testador,
**sem publicar o conteúdo dos logs nem dados de sessão**:

- **Perfil ativo: Cobalt experimental**, confirmado por `YTV_EXPERIMENTAL_COBALT_PROFILE`.
  Os VPKs de CI no fluxo `Build Vita Full and Lite` usam o **Cobalt oficial
  não modificado**, portanto **não equivalem** ao binário experimental em uso
  e podem perder a filtragem antecipada de anúncios. Não os instalar como
  substitutos do aplicativo que já está funcionando com AdBlock experimental.
- Reprodução H.264 de **1280×720**, com aproximadamente **300 quadros
  decodificados por intervalo de 5 segundos** (60 FPS no fluxo).
- Cobalt emite heartbeat até **170 segundos** e um evento de pump aos
  **180 segundos**. Não há uma linha final de exceção/erro fatal nem
  encerramento normal; o fim abrupto, sozinho, não revela a instrução que falhou.
- `YTV_LIFECYCLE_ALLOC_RESCUE` aparece **6 vezes**, em solicitações de
  **~0,95–1,20 MB** (decimais), todas com `ok=1`. São sinais de
  pressão/fragmentação do heap, **não** provas de falta definitiva de RAM.
- O consumo `newlib_used` oscila (pico observado ~82 MiB, último
  heartbeat ~62 MiB), sem crescimento monotônico até a saída;
  `user_free` no último heartbeat ~79 MiB e
  `cdram_free` ~15 MiB. Não há `ALLOC_FAIL` explícito.
- Os registros finais do player têm `targets=1 dead=0`, sem evidência de
  vazamento crescente de alvos de texturas; a fila de vídeo pode estar
  normalmente preenchida (`free=0`, `vin=11–12`).
- Nenhum registro de ação `YTV_SPONSORBLOCK` ou `YTV_ADBLOCK` aparece
  nos dois logs fornecidos. Assim, o problema de thread corrigido nesta PR
  **permanece uma hipótese**, não uma causa confirmada.

### Próxima evidência necessária

O `psp2core-*.psp2dmp` do mesmo fechamento, mais os arquivos
`cobalt_boot_prev.log` e `starboard15_prev.log`, se existirem,
permitem distinguir um abort, acesso inválido, stack overflow ou
encerramento externo. Preserve registros como estão, sem apagar
`ux0:data/youtubevita/`. Em nenhum caso publicar tokens/credenciais.

**Critério de publicação:** o patch deve ser empacotado no **mesmo Cobalt
experimental** usado pelo aparelho e validado com teste prolongado. O
build baseline só prova compilação da camada Starboard; não prova
a preservação do adblock experimental nem a correção do crash.
