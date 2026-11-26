# EXPERIMENT ONLY — serial V8 garbage collection (2026-11-25)

**No release. Do not merge into main before physical PS Vita testing.**

## Evidence prompting this A/B test

Across four physical Vita crashes, the exact **same Cobalt +0x95a194** instruction attempts `ldr r2, [r0,#-1]`; **DFSR=7** indicates a read-side translation fault. The faulting V8 tagged-looking object address is inside a live 508 KiB `YTStarboardMap` in every core dump.

The earlier changes to the initial read-only policy and later per-page kernel protection **did not fix** the reproduction during video switching. The Cobalt instruction sequence contains acquire/release-type memory barriers and tagged-object checks, which makes concurrent access worth investigating, **without proving** a GC race or excluding a stale pointer.

The exact runtime (`libcobalt.so`, SHA-256 `1aa4c117417f943e5f6595edcdf5ab462ab4f6e6f1074829bd3111538291a7c4`) includes the V8 flag `single_threaded_gc`, and the pinned upstream Cobalt source (`762d9bf...`) sets V8 flags in `cobalt/script/v8c/isolate_fellowship.cc`.

## Controlled source change

The experimental workflow compiles pinned source with **one additional V8 command-line flag: `--single-threaded-gc`**, which disables background garbage-collector tasks. It retains the exact existing YouTube TV NoAds preload patch, Vita native application source, fork IDs `YTVNX0001/0002`, and standard Cobalt JIT behavior.

**Compared with the previous failing experimental build**, only the V8 background GC mode and consequent source-built Cobalt bytes/layout should differ. The ELF loader is regenerated and verified against the exact new runtime SHA-256. Existing default `jit=1` continues; the experiment does **not** itself disable JIT.

## Why this may help — and what it does not prove

V8 may be concurrently visiting or reclaiming objects while video navigation causes allocations and DOM teardown. Serial GC could lower the chance of a race or allocation lifetime issue. It is a mitigation experiment, not a demonstrated repair, and can reduce performance or increase GC pause times. It will not fix bad virtual mappings or invalid object pointers caused elsewhere.

## How to test it

- Download **only** the artifacts of `Experimental V8 single-threaded GC Cobalt + Vita VPKs (NO release)`, under the branch `experiments/v8-serial-gc`.
- Make a backup of `ux0:data/youtubevita-noads/`; install either Full or Lite test VPK using VitaShell. Its title ID is the same as the fork's existing installation, not the upstream app's.
- Remove the earlier `jit=0` override if present, or set `jit=1`, then fully restart the app. Otherwise two variables (JIT and GC concurrency) would change together.
- In Settings -> Tweaks, leave AdBlock/SponsorBlock in the **same positions** as during the previous crash. Repeat video-switching (not just watching one live) and several 30–60 minute sessions if the app remains responsive.
- If another crash occurs, save `cobalt_boot.log`, `starboard15.log`, their `_prev.log` counterparts, and the new `.psp2dmp` **privately**.
- To return to the existing public version, reinstall the official fork v0.0.3 Full/Lite VPK. Keep the original `ux0:data/youtubevita-noads/` backup.

## Acceptance criteria

The source and VPK metadata must pass CI, including the patch marker and compiled Cobalt SHA-256. Hardware success requires repeated switch/reproduction runs, **not** compilation alone. In particular, check that video/audio remains responsive, that GC pauses do not cause intolerable frame stalls, and whether a future fault remains at `+0x95a194`.

Do not call the issue fixed, update a public release, or submit a proven-fix PR to Wolff's Room without that hardware evidence.
