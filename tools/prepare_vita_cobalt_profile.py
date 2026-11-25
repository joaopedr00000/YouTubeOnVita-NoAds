#!/usr/bin/env python3
"""Validate a source-built runtime for the existing Vita loader contract.
Requires pyelftools. Does not execute the binary.
"""
import hashlib
import pathlib
import re
import struct
import sys
from collections import Counter
from elftools.elf.elffile import ELFFile

CAP = 34 * 1024 * 1024
root = pathlib.Path(__file__).resolve().parents[1]
runtime, header = map(pathlib.Path, sys.argv[1:3])
header.unlink(missing_ok=True)
data = runtime.read_bytes()

def need(condition, message):
    if not condition:
        raise SystemExit("Vita candidate blocked: " + message)

with runtime.open("rb") as stream:
    elf = ELFFile(stream)
    need(elf.elfclass == 32 and elf.little_endian and elf["e_machine"] == "EM_ARM"
         and elf["e_type"] == "ET_DYN", "expected ARM32 little-endian shared object")
    need(elf["e_flags"] & 0x400, "ELF does not declare ARM hard-float ABI")
    need(b"ytv-adblock-preload.js" in data, "compiled preload marker missing")
    loads = [p for p in elf.iter_segments() if p["p_type"] == "PT_LOAD"]
    need(0 < len(loads) <= 32 and elf.num_segments() <= 32, "program header count")
    for p in loads:
        need(p["p_filesz"] <= p["p_memsz"] and p["p_vaddr"] + p["p_memsz"] <= CAP,
             "runtime exceeds existing 34 MiB memory layout")
        need(p["p_offset"] + p["p_filesz"] <= len(data), "truncated load segment")
        need((p["p_flags"] & 3) != 3, "writable executable segment")
    executable = [p for p in loads if p["p_flags"] & 1]
    need(len(executable) == 1, "loader supports exactly one RX segment")
    x = executable[0]
    xs = x["p_vaddr"] & ~4095
    xe = (x["p_vaddr"] + x["p_memsz"] + 4095) & ~4095
    need(xe <= CAP, "RX page alignment exceeds allocation")
    for p in loads:
        if p["p_flags"] & 2:
            need(p["p_vaddr"] >= xe or p["p_vaddr"] + p["p_memsz"] <= xs,
                 "writable segment overlaps RX pages")
    dyn = elf.get_section_by_name(".dynamic")
    syms = elf.get_section_by_name(".dynsym")
    need(dyn is not None and syms is not None, "dynamic section/symbol table absent")
    tags = {t.entry.d_tag: t.entry.d_val for t in dyn.iter_tags()}
    need("DT_NEEDED" not in tags, "external ELF shared libraries unsupported")
    need("DT_RELA" not in tags and "DT_RELR" not in tags and "DT_INIT" not in tags,
         "unsupported RELA/RELR/DT_INIT")
    need(tags.get("DT_INIT_ARRAYSZ", 0) > 0 and tags["DT_INIT_ARRAYSZ"] % 4 == 0,
         "missing constructor table")
    events = [s["st_value"] for s in syms.iter_symbols()
              if s.name == "SbEventHandle" and s["st_shndx"] != "SHN_UNDEF"]
    need(len(events) == 1 and xs <= (events[0] & ~1) < xe, "missing valid SbEventHandle")
    known = set(re.findall(r'\{"([^"]+)",\s*reinterpret_cast',
                          (root / "experiments/cobalt/vita/import_lookup.cpp").read_text()))
    counts = Counter()
    unresolved = set()
    for name, addr_tag, size_tag in ((".rel.dyn", "DT_REL", "DT_RELSZ"),
                                     (".rel.plt", "DT_JMPREL", "DT_PLTRELSZ")):
        sec = elf.get_section_by_name(name)
        need(sec is not None and sec["sh_addr"] == tags.get(addr_tag)
             and sec["sh_size"] == tags.get(size_tag), "dynamic relocation table mismatch: " + name)
        for rel in sec.iter_relocations():
            typ, index, addr = rel["r_info_type"], rel["r_info_sym"], rel["r_offset"]
            need(typ in (2, 21, 22, 23), "unsupported ARM relocation " + str(typ))
            need(addr % 4 == 0 and any(p["p_flags"] & 2 and
                 p["p_vaddr"] <= addr and addr + 4 <= p["p_vaddr"] + p["p_memsz"]
                 for p in loads), "relocation target outside writable segment")
            counts[typ] += 1
            if typ == 23:
                need(index == 0, "RELATIVE relocation has a symbol")
                continue
            need(index < syms.num_symbols(), "relocation symbol index out of range")
            sym = syms.get_symbol(index)
            if sym["st_shndx"] == "SHN_UNDEF":
                need(typ in (21, 22), "undefined ABS32 import not supported")
                counts["ext" + str(typ)] += 1
                if sym.name not in known:
                    unresolved.add(sym.name)
    need(not unresolved, "missing Vita imports: " + ", ".join(sorted(unresolved)))
    code = data[x["p_offset"]:x["p_offset"] + x["p_filesz"]]
    prefix = struct.pack("<III", 0xe0801001, 0xe3a02000, 0xe52d7004)
    matches = [i for i in range(0, len(code)-19, 4)
               if code[i:i+12] == prefix and code[i+16:i+20] == struct.pack("<I", 0xef000000)]
    need(len(matches) == 1, "expected one verified V8 ARM cacheflush prologue; found " + str(len(matches)))
    missing = counts["ext21"] + counts["ext22"]
    profile = {
        "EXPECT_BYTES": len(data), "EXPECT_SYMCOUNT": syms.num_symbols(),
        "EXPECT_LOADS": len(loads), "EXPECT_RELATIVE": counts[23],
        "EXPECT_ABS32": counts[2], "EXPECT_GLOB": counts[21], "EXPECT_JUMP": counts[22],
        "EXPECT_MISSING": missing, "EXPECT_EXT_GLOB": counts["ext21"],
        "EXPECT_EXT_JUMP": counts["ext22"],
        "EXPECT_RESOLVED": sum(counts[k] for k in (2,21,22,23)) - missing,
        "EXPECT_XSTART": xs, "EXPECT_XLEN": xe-xs, "EXPECT_CTORS": tags["DT_INIT_ARRAYSZ"]//4,
        "EXPECT_EVENT": events[0], "EXPECT_FLUSH": x["p_vaddr"]+matches[0],
    }
sha = hashlib.sha256(data).hexdigest()
header.parent.mkdir(parents=True, exist_ok=True)
header.write_text("/* Generated from exact experimental ELF; do not edit. */\n" +
                  f'#define CANDIDATE_SHA256 "{sha}"\n' +
                  "".join(f"#define {k} {v}u\n" for k,v in profile.items()))
print("Candidate profile generated:", sha, profile)
