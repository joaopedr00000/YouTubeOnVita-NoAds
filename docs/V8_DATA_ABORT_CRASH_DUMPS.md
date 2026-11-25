# Correção candidata de V8 Data Abort em reprodução contínua

## Evidência extraída de dois dumps incompletos (.psp2dmp.tmp)

Os arquivos recebidos têm somente 9.216 B e 9.728 B, respectivamente, com
conteúdo gzip interrompido. A descompressão parcial ainda fornece o início
do ELF ARM e as notas `THREAD_INFO` / `THREAD_REG_INFO`; não contém
`MODULE_INFO`, `STACK_INFO` ou as páginas de código/heap, necessárias
para simbolização e backtrace completo.

| Registro | YTVCB0001 (dump 1795593222) | YTVNX0001 (dump 1795618705) |
| --- | --- | --- |
| Thread | `pthread`, 0x4001029f | `pthread`, 0x400102bb |
| Stop reason | `0x30004` Data Abort | `0x30004` Data Abort |
| PC | `0x89b59014` | `0x89b5a194` |
| LR | `0x89b5eb0c` | `0x89b5fc8c` |
| r0 | `0x8c644601` | `0x8c644601` |
| DFAR | `0x8c644600` | `0x8c644600` |
| DFSR | `0x00000007` | `0x00000007` |

Os endereços PC e LR se deslocam **0x1180** entre os dois builds. O
mesmo DFAR e o mesmo r0 são fortes indícios de um mesmo acesso de objeto
V8 inválido/temporariamente inacessível. `DFSR=7` corresponde a uma falha
de tradução de segundo nível, e não comprova, por si só, falta de heap ou
página sem permissão de escrita.

A hipótese mais próxima na implementação era a **inconsistência da
política de proteção**: `SbMemoryMap(Read)` aplicava proteção real de
somente leitura usando `kuKernelMemProtect` enquanto
`SbMemoryProtect(ReadWrite)` em mapeamentos de dados apenas
retornava sucesso e não revertia o estado real das páginas. O código já
documentava falhas `DFSR=7` associadas a mudanças de proteção de páginas
compartilhadas com o heap V8 em versões anteriores.

## Mitigação proposta nesta PR

- Não aplicar `kuKernelMemProtect` na criação de mapeamentos **não
  executáveis**. Todos os dados começam RW, conforme a própria
  implementação de `SbMemoryProtect` para páginas de dados.
- Manter validação que rejeita alocação executável inicial e W+X.
- Continuar fazendo proteção real de páginas de **código JIT executável**,
  inclusive com `SbMemoryProtect` e o flag `exec_used`.
- **Não modificar** Cobalt experimental (libcobalt.so), preload de AdBlock,
  redirecionamento DNS, SponsorBlock, decodificador H.264, bitrate, FPS
  nem orçamentos de buffer.

É uma **correção preventiva e plausível**, não uma causa confirmada.
Sem páginas de código/heap ou ELF exato do Cobalt experimental, não é
possível saber qual função desreferenciou o objeto.

## Validação

1. Executar `node tools/test_vm_data_mappings.cjs` e o build no VitaSDK.
2. **Não instalar** artefatos do CI baseline como substituto do Cobalt
   experimental com AdBlock do usuário. É preciso empacotar a correção com
   o **mesmo `libcobalt.so` experimental usado no Vita**.
3. Testar um vídeo de 720p/60 FPS por 20–30 min, com AdBlock ligado.
   Comparar com o mesmo vídeo/qualidade na versão atual.
4. Se voltar a cair, fornecer o dump **sem extensão `.tmp`** após o Vita
   concluir de gravar (copiar com VitaShell após esperar alguns segundos),
   mais os logs `*_prev.log`. Não descartar arquivos antigos.
5. Confirmar `PC`, `DFAR`, `DFSR` no próximo dump: se a mesma falha
   ocorrer, revisar `SbMemoryProtect` nas transições JIT e buscar a função
   exata com `libcobalt.so` e seus símbolos antes de alterar mais lógica.

Nota: os dumps podem conter material sensível de memória; nunca publicá-los
automaticamente em issue ou comentário público.
