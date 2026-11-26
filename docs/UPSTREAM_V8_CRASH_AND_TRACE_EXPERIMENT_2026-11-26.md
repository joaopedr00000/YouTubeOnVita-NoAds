# VM 508 KiB crash tracing — experimental fork build (26 November 2026)

**Not a proven fix. No change to `main` or published release v0.0.3.**

## Upstream status reviewed

The public upstream `WolffsRoom/YouTubeOnVita` main is still at commit
`54b89cecce1f4f1806094f062dc3fec524ec9d54`, dated 23 November 2026,
and the latest public release is v0.65. In the 26 November PR comments,
Wolff's Room said its **unpublished local v0.67** resolves an RO→RW
inconsistency without relaxing protections; the **Cobalt V8 crash remains
under investigation**. The fix's code is not published in main, so this
experiment does not pretend to import or reproduce it.

Separately, v0.68 locally includes an optional MediaSession Skip Ad adaptation
(PR #11 closed, locally incorporated), and SponsorBlock with Tweaks (PR #7)
remains draft. These are *not* evidence that the crash is fixed.

## Hardware failure confirmed

Four physical Vita core dumps, including a failed retest of
`experiments/vm-page-granular-kernel-protection`, all fault at the same Cobalt
code offset `+0x95a194` (`ldr r2, [r0, #-1]`, `DFSR=7`), reading within a
still-listed `YTStarboardMap` allocation of 0x7f000 bytes (508 KiB).
This is a read-side **translation fault**, not proof of a read-only permission
violation. Previous RO-first and page-granular protection approaches did not
stop the hardware crash.

## Exactly what changes in this branch

- In `port_virtual_memory.cpp`, keep the **same memory permission behavior**
  as the failing last experimental build.
- Fix diagnostic bookkeeping: the final partial and full unmap traces now
  preserve `released_after=127` for a 508 KiB block rather than logging 0
  after resetting its mapping metadata. This does not change memory release.
- Log `YTV_VM_TRACE_508K_MAP` for each 508 KiB mapping (uid, allocation id,
  base, size), `YTV_VM_TRACE_UNMAP` for those maps, and
  `YTV_VM_TRACE_PROTECT` including RW requests not actually sent to kernel.
- Permit **only `YTV_VM_TRACE_*`** to reach `starboard15.log` without a
  `verbose.txt` file. The source logger caps them at **320 events per
  process**; unrelated verbose logs remain disabled.
- The **Cobalt ELF is exactly the older source-built experimental runtime**,
  SHA-256 `1aa4c117417f943e5f6595edcdf5ab462ab4f6e6f1074829bd3111538291a7c4`.
  No GC change or new AdBlock behavior is present. This is a controlled
  logging-only experiment.

## How to test

1. Back up `ux0:data/youtubevita-noads/` and install one Full/Lite VPK from
   this branch's successful GitHub Actions run.
2. Keep `jit=1` (or remove the JIT override). Do not create `verbose.txt`.
3. Repeat the video changes that previously triggered the crash. Compare
   startup time and playback; synchronous logging has some I/O overhead.
4. If a crash recurs, share the relevant `starboard15.log` and
   `starboard15_prev.log`, `cobalt_boot.log` and its predecessor, and
   submit the `.psp2dmp` **privately**, not as a public attachment.
5. Correlate `DFAR` from the dump with the latest
   `YTV_VM_TRACE_508K_MAP` base/uid, then inspect whether any recent
   `YTV_VM_TRACE_UNMAP` or `YTV_VM_TRACE_PROTECT` touched the same range.

This branch is for **evidence collection**, not a release. Compare it against
the separate `experiments/v8-serial-gc` build. Avoid changing both GC mode
and native page-permission behavior simultaneously.
