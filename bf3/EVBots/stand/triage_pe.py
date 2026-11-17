# -*- coding: utf-8 -*-
"""PE-триаж botlayer.dll: заголовки, секции, импорты, экспорты, строки.

Зачем: узнать, как устроен бот-слой ДО установки Ghidra — какие API движка
он дёргает, какие хуки-флаги читает, есть ли свои точки входа.

Пример:
  python triage_pe.py C:\\Games\\BF3LegacyLan\\activation\\botlayer.dll
  python triage_pe.py <dll> --strings
"""
import sys, io, os, struct

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def eprint(s=""):
    sys.stdout.write(s + "\n")


def section_line(secs):
    eprint("%-10s %-10s %-10s %-10s %s" % ("name", "vaddr", "vsize", "rawsize", "chars"))
    for s in secs:
        ch = []
        chars = s.Characteristics
        if chars & 0x20000000:
            ch.append("X")
        if chars & 0x40000000:
            ch.append("R")
        if chars & 0x80000000:
            ch.append("W")
        if chars & 0x00000020:
            ch.append("code")
        if chars & 0x00000040:
            ch.append("idata")
        if chars & 0x00000080:
            ch.append("udata")
        eprint("%-10s 0x%08x 0x%08x %-10d %s" % (
            s.Name.decode("ascii", "replace").rstrip("\x00"),
            s.VirtualAddress, s.Misc_VirtualSize, s.SizeOfRawData,
            ",".join(ch) or hex(chars)))


def triage(path):
    import pefile

    pe = pefile.PE(path, fast_load=False)
    eprint("=" * 70)
    eprint("FILE  %s" % path)
    eprint("SIZE  %d bytes" % os.path.getsize(path))
    machine = {0x14C: "x86 (32-bit)", 0x8664: "x64", 0x1C0: "ARM"}.get(
        pe.FILE_HEADER.Machine, hex(pe.FILE_HEADER.Machine))
    eprint("MACH  %s" % machine)
    eprint("SECT  %d" % pe.FILE_HEADER.NumberOfSections)
    ts = pe.FILE_HEADER.TimeDateStamp
    eprint("BUILD %d (%s)" % (ts, __import__("time").strftime(
        "%Y-%m-%d %H:%M", __import__("time").gmtime(ts))))
    magic = {0x10B: "PE32", 0x20B: "PE32+"}.get(pe.OPTIONAL_HEADER.Magic, "?")
    eprint("MAGIC %s  entry=0x%08x  imagebase=0x%08x  subsystem=%d" % (
        magic, pe.OPTIONAL_HEADER.AddressOfEntryPoint,
        pe.OPTIONAL_HEADER.ImageBase, pe.OPTIONAL_HEADER.Subsystem))
    dllch = pe.OPTIONAL_HEADER.DllCharacteristics
    eprint("DLLCH 0x%04x  %s" % (dllch, ", ".join(
        n for b, n in ((0x0040, "DYNAMIC_BASE(ASLR)"), (0x0100, "NX_COMPAT"),
                       (0x0400, "NO_SEH"), (0x8000, "TERMINAL_SERVER_AWARE"))
        if dllch & b)))

    eprint()
    eprint("--- sections ---")
    section_line(pe.sections)

    eprint()
    eprint("--- imports ---")
    if hasattr(pe, "DIRECTORY_ENTRY_IMPORT"):
        for entry in pe.DIRECTORY_ENTRY_IMPORT:
            dll = entry.dll.decode("ascii", "replace")
            names = []
            for imp in entry.imports:
                names.append(imp.name.decode("ascii", "replace") if imp.name else
                             "ord%d" % imp.ordinal)
            eprint("%s (%d): %s" % (dll, len(names), ", ".join(names[:24]) +
                                    (" ..." if len(names) > 24 else "")))
    else:
        eprint("(none)")

    eprint()
    eprint("--- exports ---")
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        eprint("dll name: %s" % (pe.DIRECTORY_ENTRY_EXPORT.name or b"?").decode("ascii", "replace"))
        for s in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            nm = s.name.decode("ascii", "replace") if s.name else "ord%d" % s.ordinal
            eprint("  0x%08x  %s" % (pe.OPTIONAL_HEADER.ImageBase + s.address, nm))
    else:
        eprint("(none)")

    eprint()
    eprint("--- debug/PDB ---")
    if hasattr(pe, "DIRECTORY_ENTRY_DEBUG"):
        for d in pe.DIRECTORY_ENTRY_DEBUG:
            if d.struct.Type == 2 and d.entry:
                pdb = pe.get_string_at_rva(d.entry.PointerToRawData)
                if isinstance(pdb, bytes):
                    pdb = pdb.split(b"\x00")[0].decode("ascii", "replace")
                eprint("PDB: %s" % pdb)
    else:
        eprint("(none)")
    return pe


def strings(path, minlen=5):
    data = open(path, "rb").read()
    out = []
    cur = bytearray()
    for b in data:
        if 32 <= b < 127:
            cur.append(b)
        else:
            if len(cur) >= minlen:
                out.append(cur.decode("ascii"))
            cur = bytearray()
    if len(cur) >= minlen:
        out.append(cur.decode("ascii"))
    return out


def wide_strings(path, minlen=4):
    data = open(path, "rb").read()
    out = []
    cur = bytearray()
    i = 0
    while i < len(data) - 1:
        b, b2 = data[i], data[i + 1]
        if b2 == 0 and 32 <= b < 127:
            cur.append(b)
            i += 2
            continue
        if len(cur) >= minlen:
            out.append(cur.decode("ascii"))
        cur = bytearray()
        i += 1
    if len(cur) >= minlen:
        out.append(cur.decode("ascii"))
    return out


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = set(a for a in sys.argv[1:] if a.startswith("--"))
    if not args:
        eprint(__doc__)
        sys.exit(1)
    pe = triage(args[0])
    if "--strings" in flags:
        eprint()
        eprint("--- ascii strings (>=5) ---")
        for s in sorted(set(strings(args[0]))):
            eprint("  " + s)
        eprint()
        eprint("--- wide strings (>=4) ---")
        for s in sorted(set(wide_strings(args[0]))):
            eprint("  " + s)