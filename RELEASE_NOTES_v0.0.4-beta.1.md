# YouTubeOnVita-NoAds v0.0.4-beta.1 — V8 Serial GC (public beta) 🇧🇷

**BETA / PRERELEASE — crash mitigation is NOT yet confirmed.**

Independent fork of **[YouTubeOnVita by Wolff's Room](https://github.com/WolffsRoom/YouTubeOnVita)**. The YouTube TV experience, native player, downloads and most other app features originate from Wolff's Room's excellent original project; SponsorBlock and the experimental Cobalt AdBlock layers are modifications in this fork.

## English — Changes and test status

- **New experimental V8 stability measure:** the pinned, source-built Cobalt 25 runtime now supplies `--single-threaded-gc`, disabling V8 garbage collection tasks in the background. This aims to reduce concurrency pressure when switching YouTube videos; **it does not establish that a GC race was the root cause**.
- The existing **JIT remains configurable**, and the custom YouTube TV ad-filter preload is retained. SponsorBlock remains opt-in, under **Settings → Tweaks**.
- **Actual PS Vita testing (maintainer report): approximately 15 minutes watching videos and switching between them, with NO crash observed.** Previously, switching between videos had repeatedly produced a `DFSR=7` crash at the same Cobalt instruction. **Fifteen minutes is only an encouraging preliminary test; we cannot yet confirm the crash is fixed.** Longer tests, different videos and other Vita hardware setups are needed.
- Full and Lite maintain distinct fork title IDs **`YTVNX0001`** / **`YTVNX0002`**, with isolated data folder `ux0:data/youtubevita-noads/`. This beta is packaged as **internal version `00.04`** (v0.0.4 beta).
- The VPKs in this beta were rebuilt to update the internal version while retaining **exactly the same verified Serial GC Cobalt runtime** (SHA-256 `7fac748639488da0873280dd2a6d549612798034a46a6ec20e440fefb24a2ab4`) as the earlier, tested experimental build. **These exact repackaged v0.0.4-beta.1 VPKs have passed automated tests, but have not yet been independently retested on Vita hardware.**
- **Known limitation:** potential V8 crashes, playback/UI glitches and extra pauses due to serialized garbage collection. This release does **not** claim a permanent fix.

## Português (Brasil) — Alterações e testes

- **Nova tentativa experimental de estabilização do V8:** o Cobalt 25 recompilado utiliza `--single-threaded-gc` para desativar tarefas de coleta de lixo do V8 em segundo plano. A hipótese é reduzir problemas de concorrência ao trocar vídeos, **sem afirmar que essa é a causa comprovada**.
- O **JIT continua configurável**. Foram mantidos os recursos de filtragem de anúncios do fork; SponsorBlock permanece opcional em **Configurações → Modificações (Tweaks)**.
- **Teste real no PS Vita, relatado pelo mantenedor: aproximadamente 15 minutos assistindo e alternando vídeos, sem nenhum crash observado.** Como as compilações anteriores travaram ao alternar vídeos, o resultado é promissor. **Porém, ainda não é possível confirmar a correção definitiva.** Precisamos de testes mais longos e em diferentes situações.
- Versão interna **`00.04`**, mantendo os IDs próprios **`YTVNX0001`** (Full) e **`YTVNX0002`** (Lite) e os dados em `ux0:data/youtubevita-noads/`. Os VPKs finais foram recompilados apenas para atualizar a identificação de versão, preservando o mesmo binário Cobalt Serial GC do teste. **A recompilação passou nos testes automatizados; os pacotes finais ainda precisam ser retestados no console.**
- **Versão beta pública**, com possíveis travamentos ou pausas de desempenho ainda não solucionados.

## Español — Cambios y pruebas

- **Mitigación experimental del V8:** el Cobalt 25 compilado desde el código fuente usa `--single-threaded-gc` para desactivar las tareas de recolección de basura en segundo plano. Esta modificación podría reducir fallos al cambiar de vídeo, **pero no demuestra la causa del problema**.
- Se mantienen el JIT configurable, el filtro de anuncios y SponsorBlock opcional en **Settings → Tweaks**.
- **Prueba del mantenedor en una PS Vita real:** aproximadamente 15 minutos viendo y cambiando vídeos, **sin cierres inesperados observados**. El resultado es positivo, pero **todavía no es posible confirmar que el fallo esté corregido**. Se necesitan pruebas más largas y en otros dispositivos.
- Version interna **`00.04`**, paquetes Full y Lite independientes con los mismos identificadores del fork. Los paquetes beta se recompilaron conservando exactamente el Cobalt Serial GC probado, aunque los VPKs finales aún no han sido probados físicamente.
- **Beta pública:** pueden persistir fallos, problemas visuales o pausas adicionales.

## Install / Instalação / Instalación

Install the **Full** or **Lite** VPK with VitaShell over the same edition of this fork; both can coexist. Back up `ux0:data/youtubevita-noads/` before upgrading. Requires **kubridge v0.3.1+** under `*KERNEL` and `ur0:data/libshacccg.suprx` (ShaRKF00D), as documented by the original project. Use `jit=1` or remove any previous `jit=0` diagnostic override if you want to test Serial GC independently; the JIT preference is not changed automatically.

To report a crash, keep `cobalt_boot.log`, `starboard15.log`, their `_prev.log` versions and the `.psp2dmp` privately; crash dumps may contain account/session memory and should not be publicly attached.

## Credits / Créditos / Créditos

**Original PS Vita application and principal developer: [Wolff's Room — YouTubeOnVita](https://github.com/WolffsRoom/YouTubeOnVita).** Special thanks for the immense work on the native port and existing features. This independent fork is maintained in Brazil 🇧🇷 and is unaffiliated with Google/YouTube, Sony or the original author. Thanks also to Cobalt, VitaSDK, vitaGL and the Vita homebrew community.
