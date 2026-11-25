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
