# VM / V8 tentative crash mitigation: per-page kernel protection

Status: **development candidate only; no Vita hardware verification of this exact change.**

The three user-provided PS Vita dumps all stop in the same Cobalt instruction
at offset **+0x95a194**, an ARM \`ldr r2, [r0, #-1]\` from tagged-looking
pointers, with **DFSR=7 (read-side translation fault)**. The read address is
inside a live **0x7f000 (508 KiB)** \`YTStarboardMap\` memblock in each dump.
The Vita core snapshot includes the bytes at the newest faulting address,
which argues against a permanently missing *whole* block but does **not**
establish the page-table state at the crash instant.

## Concrete issue found in the previous port

The previous \`SbMemoryProtect\` retained a **single bool per memblock**
(\`m.exec_used\`). Once any subrange in the memblock became JIT-executable,
**all later non-reserved protection requests anywhere in the block called
\`kuKernelMemProtect\`**, even for neighboring ordinary data pages. This
is unnecessarily broad on the Vita and is a plausible contributor to TLB /
second-level page-table churn, but it is **not proven to explain the fault**.

## Changes

- Track kernel-touched pages using a small **bitset per memblock**, allocated
  only on the first executable protect request.
- Mark pages only after successful \`kuKernelMemProtect\` calls.
- On subsequent transitions to Read/ReadWrite, invoke the kernel **only for
  previously kernel-touched contiguous page spans**, never other data pages in
  the same allocation.
- Keep per-page history after RX→R→RW transitions; do not accidentally skip a
  later RW kernel permission restoration.
- Preserve the existing no-W+X rule, logical partial unmap semantics, and
  intentionally unsupported reservation/decommit semantics (flags=0).
- Free both per-map bitmaps when the entire Vita memblock is finally freed.
- Emit bounded, metadata-only tracing \`YTV_VM_TRACE_508K_MAP\`,
  \`YTV_VM_TRACE_UNMAP\`, and \`YTV_VM_TRACE_PROTECT\` in Starboard logs.
  Trace entries contain mapped address/size, memblock ID and permission flags
  but never cookies, URL contents, or raw core memory.

## Tests and hardware gate

\`node tools/test_vm_data_mappings.cjs\` checks source policy.
\`g++ -std=c++17 -Wall -Wextra -Werror -pthread -I tools/tests/vm_stubs
 tools/tests/test_vm_page_protection.cpp -o /tmp/ytv_vmtest && /tmp/ytv_vmtest\`
executes actual port code against stub Vita kernel calls and checks exact
page spans, failed calls, partial unmap and W+X rejection.

The host mock cannot reproduce or prove Vita kernel/TLB behavior. Before
calling this a fixed crash, perform comparable PS Vita runs (30+ minutes
each where possible) using the same exact source-Cobalt ELF and record
whether \`DFSR=7\` recurs. Do not publish raw Vita dumps to public issues.
The code alone does not justify changing the v0.0.3 release status.
