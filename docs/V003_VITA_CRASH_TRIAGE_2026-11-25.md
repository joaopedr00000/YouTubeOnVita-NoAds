# Physical PS Vita crash triage — subsequent v0.0.3 test (2026-11-25)

**Status: recurrent crash confirmed from the user-supplied Vita dumps. Root cause unresolved. No executable changes made by this report.**

**Privacy:** This report contains only redacted crash metadata. Do not upload the original `.psp2dmp` file, Vita session files, or memory image to a public issue; those dumps can contain credentials and personal information.

## Files analyzed

Newest boot: `cobalt_boot-4.log` / `starboard15-4.log`.

Pre-crash boot: `cobalt_boot_prev-3.log` / `starboard15_prev-3.log`.

Newest physical-device dump: `psp2core-1795654240-0x000017257f-eboot.bin.psp2dmp`.

For reproducibility, compared the earlier user-supplied local dumps `psp2core-1795632543-0x0000f228c3-eboot.bin.psp2dmp` and `psp2core-1795623670-0x000008306b-eboot.bin.psp2dmp`.

The `.psp2dmp` files are gzip-compressed ELF32 ARM core files. THREAD_INFO, THREAD_REG_INFO, MEM_BLK_INFO and ELF PT_LOAD segments were decoded directly. No full raw memory contents have been added to this repository.

## Three crashes — same Cobalt code address after relocation

| Dump | Stop reason | PC | Cobalt text base | PC offset | DFSR | DFAR | R0 | Active YTStarboardMap |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1795623670 | `0x30004` Data Abort | `0x89a5a194` | `0x89100000` | `+0x95a194` | `0x7` | `0x8c544600` | `0x8c544601` | `0x8c540000` size `0x7f000` |
| 1795632543 | `0x30004` Data Abort | `0x89b5a194` | `0x89200000` | `+0x95a194` | `0x7` | `0x8e861d20` | `0x8e861d21` | `0x8e840000` size `0x7f000` |
| **1795654240 (newest)** | `0x30004` Data Abort | `0x89b5a194` | `0x89200000` | `+0x95a194` | `0x7` | `0x8e98f160` | `0x8e98f161` | `0x8e980000` size `0x7f000` |

The relevant Cobalt instruction in the memory image at PC is the ARM word `0xe5102001`: **`ldr r2, [r0, #-1]`**. In all three crashes, **DFAR == R0 - 1**: a read of the untagged address associated with an odd, tagged-looking pointer. The instruction is the **same offset within the exact same Cobalt code layout**, even with base address randomization.

The newest failing thread is named `pthread`, with UID `0x400102b1`. Newest LR is `0x89b5fc8c`; SP is `0x8da004e8`. The observed `DFSR=7` denotes a **second-level page translation fault on a read**, not a demonstrated read-only permission fault.

The fault address is *inside a memblock still listed in MEM_BLK_INFO* for **all three dumps**. In the newest dump, the page containing the fault also appears in the ELF core's PT_LOAD memory snapshots. This observation makes an obviously freed **entire** memblock or global out-of-memory explanation weaker. However, neither the memblock listing nor the core snapshot proves the faulting thread had a valid second-level page-table entry at the instant it executed.

## Relevant runtime observations

- `cobalt_boot_prev-3.log` continued to `COBALT_HEARTBEAT t=95s`: roughly **75 MiB user memory free** and **15 MiB CDRAM free**. No `VM_MAP_FAIL` or fatal decoder error was visible in these logs.
- `starboard15_prev-3.log` recorded a successful 634x360 video at approximately 30 FPS. During the last logged interval (`media_ms=44892`), 150 frames were decoded and 150 shown with **zero dropped**, while free user memory during playback was about **72 MiB**.
- `cobalt_boot-4.log` is the shorter subsequent boot, ending at `COBALT_HEARTBEAT t=24s`, and `starboard15-4.log` only shows startup/storage activity, not a new decoder initialization.
- Storage is correctly separated under `ux0:data/youtubevita-noads/`.
- Latest logs do **not** contain a reliable APP_VER assertion, but the Cobalt profile checksum matches the experimental Cobalt runtime carried across the fork's releases.

## What this does and does not prove

**Established:** Repeated physical Vita crashes in the same Cobalt instruction when attempting to read an address inside a live `YTStarboardMap` allocation, with a translation-fault status. **The prior data-memory protection policy change has not reliably prevented the crashes.**

**Hypotheses requiring tests:** stale/invalid page-table translation or TLB state, interference from another protection change (including JIT/page-neighbor effects), use of a stale/invalid tagged V8 object pointer, or allocator/mapping lifecycle mismatch.

**Not established:** exactly which API unmapped/protected the page, whether V8 itself produced an invalid pointer, whether third-party plugins changed page tables, or any single patch that fixes the problem. Avoid guessing, increasing random memory allocations, or silently widening executable permissions.

## Narrow, actionable follow-up

1. Obtain **the exact Cobalt ELF with symbols** corresponding to SHA-256 `1aa4c117417f943e5f6595edcdf5ab462ab4f6e6f1074829bd3111538291a7c4`, then symbolicate PC `+0x95a194`, LR `+0x95fc8c` and a small stack window. No raw dump upload to public GitHub.
2. Trace only `SbMemoryMap`, `SbMemoryUnmap`, `SbMemoryProtect` and `kuKernelMemProtect` transitions, with monotonic sequence numbers, memblock UIDs, base/size, page spans, operation, result and separate logs to flash. Include the **full affected allocation range** when page permissions change.
3. Record page occupancy/state at time of partial `SbMemoryUnmap`, including duplicate partial unmaps and any attempt to protect previously released pages.
4. Reproduce using matched hardware, plugin configuration and video/sign-in state. Compare the same exact Cobalt runtime with protection changes isolated, without claiming that a source-only test proves a PS Vita fix.
5. Evaluate V8 object-pointer lifetime and possible stale references only *after* identifying the function at `+0x95a194`; the low-bit pattern suggests a tagged heap pointer but is not itself a root-cause diagnosis.

Do not label the earlier protection change a **confirmed crash fix** in any upstream PR or release notes. Avoid shipping a further public release as a solved crash until the same hardware reproduction succeeds over repeated tests.
