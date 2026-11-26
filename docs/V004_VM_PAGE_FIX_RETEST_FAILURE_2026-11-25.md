# Physical PS Vita retest: page-granular VM patch still crashes (2026-11-25)

**Status: FAIL on hardware.** This is a follow-up to `docs/VITA_VM_JIT_PAGE_PROTECTION_TEST_PLAN.md`. The newest physical-device test used the experimental Full build from GitHub Actions run **45735960651**, with the exact same custom Cobalt SHA-256 `1aa4c117417f943e5f6595edcdf5ab462ab4f6e6f1074829bd3111538291a7c4` as the earlier release, while navigating between several videos. The application crashed. **The page-granular protection patch must not be called a fix.**

Uploaded local evidence (not public, do not upload memory dumps into a public issue):
- `cobalt_boot-5.log`, `cobalt_boot_prev-4.log`
- `starboard15-5.log`, `starboard15_prev-4.log`
- `psp2core-1795657088-0x00005135e1-eboot.bin.psp2dmp`

## Four independently observed real-device crashes

All four core files are gzipped ELF32 ARM dumps. Registers below were read from `THREAD_REG_INFO`; memory blocks from `MEM_BLK_INFO`.

| Dump epoch | PC | Cobalt text offset | R0 | DFSR | DFAR | Live memblock covering DFAR |
| --- | --- | --- | --- | --- | --- | --- |
| 1795623670 | `0x89a5a194` | `+0x95a194` | `0x8c544601` | `0x7` | `0x8c544600` | `0x8c540000`–`0x8c5bf000` |
| 1795632543 | `0x89b5a194` | `+0x95a194` | `0x8e861d21` | `0x7` | `0x8e861d20` | `0x8e840000`–`0x8e8bf000` |
| 1795654240 | `0x89b5a194` | `+0x95a194` | `0x8e98f161` | `0x7` | `0x8e98f160` | `0x8e980000`–`0x8e9ff000` |
| **1795657088** | **`0x89b5a194`** | **`+0x95a194`** | **`0x908c78c1`** | **`0x7`** | **`0x908c78c0`** | **`0x908c0000`–`0x9093f000`** |

In every crash, the ARM instruction is `ldr r2, [r0, #-1]`: `DFAR == R0 - 1`. Each address is inside a `YTStarboardMap` memblock of `0x7f000` bytes (508 KiB) still registered in the dump. These observations establish consistent crash behavior but **do not establish the page-table state at the instant of the read**, nor whether the underlying issue is a stale tagged pointer, allocator lifecycle, kernel mapping, JIT or something else.

## Previous run and post-crash restart

The pre-crash boot log was still producing heartbeats at `t=95s`, with roughly **72 MiB user memory free** and **15 MiB CDRAM free**. The native video decoder was playing a 634×360 stream at roughly 30 FPS; its final logged 5-second interval showed 150 decoded and 150 shown, with 0 dropped frames. There was no logged decoder failure corresponding directly to the final crash.

`cobalt_boot-5.log` and `starboard15-5.log` record the subsequent startup rather than the previous playback failure. The experimental Cobalt SHA-256 is unchanged. The app data directory remains `ux0:data/youtubevita-noads/`.

## Why the new YTV_VM_TRACE events are absent

`experiments/cobalt/vita/platform15.cpp` filters ordinary `YTV_*` messages by default to avoid costly open/write/close operations on the Vita memory card. **`YTV_VM_TRACE_*` is hidden unless a file named `ux0:data/youtubevita-noads/verbose.txt` exists at startup**. The missing trace lines therefore do **not** prove that the operations never occurred. A temporary verbose run is possible, but it adds storage I/O overhead and should not be compared as a performance-identical A/B trial.

## Next controlled diagnosis without rebuilding

`experiments/cobalt/vita/port_system.cpp` already supports the `jit` preference. Edit or create:

`ux0:data/youtubevita-noads/cobalt_config.txt`

Add the single line `jit=0` (preserve other user overrides). **Close and relaunch** the application, then confirm `YTV_LIFECYCLE_CONFIG ... jit=0 ...` appears in `starboard15.log`. Repeat the same video-switch sequence. This tests whether preventing V8 JIT code generation changes the failure probability using the **same ELF and native VM code**. Expect slower interface performance.

If the crash persists with JIT disabled, it weakens the specific hypothesis that active JIT page transitions cause it. If the crash disappears over repeated comparable sessions, that is a useful lead but **not yet definitive proof**. For the next trace-focused run, preserve logs and optionally enable `verbose.txt`, understanding the I/O overhead. When the experiment ends, remove `jit=0` (or use `jit=1`) and remove `verbose.txt` to restore normal settings.

Do **not** describe the page-granular patch as a confirmed repair in release notes, PR descriptions or GitHub Issues. Do not upload raw `.psp2dmp` memory images to public GitHub.
