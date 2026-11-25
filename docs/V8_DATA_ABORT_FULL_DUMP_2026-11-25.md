# Reprodução recorrente: V8 Data Abort e build combinada

## Evidência de hardware (25 novembro de 2026)

O dump integral `psp2core-1795623670-...-eboot.bin.psp2dmp` contém a exceção de uma thread do processo `YTVNX0001`. Registradores: `PC=0x89a5a194`, `LR=0x89a5fc8c`, `r0=0x8c544601`, `DFAR=0x8c544600`, `DFSR=7`. Uma execução anterior reportou PC/LR/R0/DFAR deslocados por exatamente +`0x100000`.

## Desmontagem do runtime experimental exato

O `libcobalt.so` de SHA-256 `1aa4c117417f943e5f6595edcdf5ab462ab4f6e6f1074829bd3111538291a7c4` foi extraído **do artefato original verificado**, e não de uma compilação genérica. A página de código no dump confere byte a byte com a sequência do ELF próxima de **VAddr `0x95a194`**. Nesse ponto, a instrução ARM é:

```asm
ldr r2, [r0, #-1]
ldrh r2, [r2, #7]
cmp r2, #161
```

O erro é uma tentativa de consultar o campo de tipo/mapa do objeto V8 em `r0-1`. O `DFAR` confirma a exata operação que falhou. **Não é possível provar somente pelo dump se houve use-after-free, corrupção, falha de tradução transiente ou bug de integração.**

**Cuidado com a base:** `PC - 0x89200000 = 0x85a194` aponta para **outra instrução** no ELF da build verificada. A sequência de bytes do dump corresponde a `0x95a194`, portanto este crash requer uma base efetiva `0x89100000` para a seção executável deste runtime, que difere da base `0x89200000` registrada nos logs de texto fornecidos. Esses logs foram enviados duas vezes com hashes idênticos e podem não representar a mesma execução do dump. Não inferir que foram contemporâneos.

## Correções reunidas para VPK de teste

- Preserva a mitigação V8 em `port_virtual_memory.cpp`: não chamar `kuKernelMemProtect` para mapeamentos iniciais de dados potencialmente compartilhados com o heap, mantendo W^X e proteção real para JIT.
- Integra a correção de `port_media_session.cpp` (da PR #2): o worker SponsorBlock não chama callback Cobalt diretamente; agenda ação via `SbEventSchedule` para a thread do event loop, com invalidação de geração.
- Integra diagnóstico leve de uso de memória durante playback em `port_player.cpp`.
- Testes estáticos para VM + callbacks em todos os workflows aplicáveis. **Esses testes e a compilação não demonstram ausência de crash no console.**
- Workflows Full/Lite experimentais reutilizam o **mesmo Cobalt NoAds verificado** e conferem seu hash antes de empacotar; não converter a build baseline em suposta versão com bloqueio de anúncios.

## Validação antes de lançamento

Testar a variante Full ou Lite com o mesmo vídeo e qualidade por tempo superior ao de falha observado, incluindo reprodução prolongada. Conferir `YTV_EXPERIMENTAL_COBALT_PROFILE`, logs de memória e versão do `eboot.bin` instalado. Se ocorrer novo Data Abort, capturar dump **completo** e logs correspondentes. Não publicar dumps brutos: podem conter credenciais e material privado na RAM.

**Status**: build experimental para investigação. Ainda não é correção comprovada nem release final.
