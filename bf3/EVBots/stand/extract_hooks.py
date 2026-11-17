# -*- coding: utf-8 -*-
"""Разбор строк botlayer.dll по категориям: env-флаги, cvars, Class::method,
Ghidra-аннотации (адреса/смещения), обфускация.

Пример:
  python extract_hooks.py C:\\Games\\BF3LegacyLan\\activation\\botlayer.dll
"""
import sys, re

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

import triage_pe as T


def report(path):
    strs = sorted(set(T.strings(path) + T.wide_strings(path)))

    cats = {
        "ENV_FLAGS (BF3ACT_*)": [],
        "CVARS (cvar-like)": [],
        "ENGIN_FUNCS (Class::method)": [],
        "GHIDRA_ADDRS (0x2xxxxxx / offsets)": [],
        "STRUCT_FIELDS (m_* / Rust)": [],
        "RUST_BUILD": [],
        "DLLMACH": [],
    }

    re_env = re.compile(r"BF3ACT_[A-Z0-9_]+")
    re_cvar = re.compile(r"^[a-z][a-zA-Z0-9_]*\.[a-zA-Z0-9_.]+$")
    re_func = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(::[A-Za-z_~][A-Za-z0-9_]*)+$")
    re_addr = re.compile(r"0x[0-9a-fA-F]{7,8}")
    re_off = re.compile(r"\[(?:GA|mgr|outer|[A-Za-z_]+)\+0x[0-9a-fA-F]+\]")
    re_field = re.compile(r"^(m_[a-zA-Z_]+|[a-z]+Update[a-zA-Z]*|[a-z]*Enabled|[a-z]*Needed)$")

    for s in strs:
        if re_env.search(s):
            cats["ENV_FLAGS (BF3ACT_*)"].append(s)
        elif "build/release" in s or "cargo" in s.lower() or "rustc" in s.lower():
            cats["RUST_BUILD"].append(s)
        elif re_func.match(s) and "::" in s:
            cats["ENGIN_FUNCS (Class::method)"].append(s)
        elif re_cvar.match(s):
            cats["CVARS (cvar-like)"].append(s)
        elif re_addr.search(s) or re_off.search(s):
            cats["GHIDRA_ADDRS (0x2xxxxxx / offsets)"].append(s)
        elif re_field.match(s):
            cats["STRUCT_FIELDS (m_* / Rust)"].append(s)

    for name in ("RUST_BUILD", "ENV_FLAGS (BF3ACT_*)", "CVARS (cvar-like)",
                 "ENGIN_FUNCS (Class::method)", "STRUCT_FIELDS (m_* / Rust)",
                 "GHIDRA_ADDRS (0x2xxxxxx / offsets)"):
        vals = sorted(set(cats[name]))
        print("=" * 72)
        print("%s  [%d]" % (name, len(vals)))
        print("=" * 72)
        for v in vals:
            print("  " + v)
        print()

    return cats


if __name__ == "__main__":
    report(sys.argv[1] if len(sys.argv) > 1 else
           r"C:\Games\BF3LegacyLan\activation\botlayer.dll")