# -*- coding: utf-8 -*-
"""Инвентарь VeniceEXT API из локальной документации VU-Docs.

Источник истины: Legacy\\VU-Docs\\content\\vext\\ref\\<side>\\<kind>\\*.md
Схема именования: <Class>_<snake_case_method>.md  ->  Class:method

Вывод: CSV/JSON-инвентарь + сводка по сторонам/видам/классам.

Примеры:
  python vext_inventory.py
  python vext_inventory.py --json
  python vext_inventory.py --side server --kind hook
"""
import sys, os, re, json, io
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

REF = Path(r"C:\Games\Legacy\VU-Docs\content\vext\ref")
OUT = Path(r"C:\Games\Legacy\LegacyBF3")
KINDS = ("event", "hook", "library", "type")
SIDES = ("client", "server", "shared")

# документы-заглушки / индексы
SKIP = {"_index.md"}


def api_name(fname: str):
    """foo_bar_baz.md -> Foo:barBaz  (Class:method) или Foo:typeName для type."""
    stem = fname[:-3] if fname.endswith(".md") else fname
    parts = stem.split("_")
    if len(parts) == 1:
        return stem[:1].upper() + stem[1:]
    cls = parts[0][:1].upper() + parts[0][1:]
    rest = "_".join(parts[1:])
    return "%s:%s" % (cls, rest)


def parse_file(path: Path, side: str, kind: str):
    """Достаёт из md: имя, сигнатуру, описания полей/аргументов/возврата."""
    try:
        text = path.read_text(encoding="utf-8", errors="replace")
    except Exception:
        text = ""

    rec = {
        "side": side,
        "kind": kind,
        "doc": str(path.relative_to(REF)).replace("\\", "/"),
        "name": api_name(path.name),
        "bytes": path.stat().st_size,
        "desc": "",
        "fields": [],
        "args": [],
        "returns": "",
    }

    # заголовок первого уровня = краткое описание
    for line in text.split("\n"):
        if line.startswith("# "):
            rec["desc"] = line[2:].strip()
            break

    # поля таблиц / аргументы
    for line in text.split("\n"):
        s = line.strip()
        m = re.match(r"^\|\s*\*{0,2}(\w+)\*{0,2}\s*\|\s*\*{0,2}`?([^|`]*)`?\*{0,2}\s*\|", s)
        if m and m.group(1).lower() not in ("name", "---", "parameter", "field", "argument", "type"):
            nm, ty = m.group(1), m.group(2).strip()
            if nm and nm != "---":
                rec["fields"].append({"name": nm, "type": ty})
    return rec


def collect():
    recs = []
    for side in SIDES:
        for kind in KINDS:
            d = REF / side / kind
            if not d.is_dir():
                continue
            for p in sorted(d.glob("*.md")):
                if p.name in SKIP:
                    continue
                recs.append(parse_file(p, side, kind))
    return recs


def summarize(recs):
    out = []
    out.append("VENICEEXT API — инвентарь из VU-Docs")
    out.append("источник: %s" % REF)
    out.append("")

    # 1. сводка
    out.append("=== Сводка ===")
    out.append("%-8s %6s %6s %8s %8s %8s" % ("side", "event", "hook", "library", "type", "всего"))
    tot = {k: 0 for k in KINDS}
    for side in SIDES:
        row = [side]
        n_all = 0
        for k in KINDS:
            n = sum(1 for r in recs if r["side"] == side and r["kind"] == k)
            row.append(n)
            tot[k] += n
            n_all += n
        row.append(n_all)
        out.append("%-8s %6d %6d %8d %8d %8d" % tuple(row))
    out.append("%-8s %6d %6d %8d %8d %8d" % ("ВСЕГО", tot["event"], tot["hook"],
                                              tot["library"], tot["type"], len(recs)))
    out.append("")

    # 2. hooks — то, что реально нужно для бота
    for kind in ("hook", "event"):
        out.append("=== %s (%s) ===" % (kind.upper(), "перехватчики" if kind == "hook" else "события"))
        for side in SIDES:
            names = [r["name"] for r in recs if r["side"] == side and r["kind"] == kind]
            if not names:
                continue
            out.append("-- %s (%d)" % (side, len(names)))
            for n in names:
                out.append("   %s" % n)
        out.append("")

    # 3. libraries с описанием — основная рабочая поверхность
    out.append("=== LIBRARY (с описанием, >=1 КБ) ===")
    for side in SIDES:
        for r in recs:
            if r["side"] == side and r["kind"] == "library" and r["bytes"] >= 1024:
                out.append("%-8s %-34s %6d Б  %s" % (side, r["name"], r["bytes"],
                                                     r["desc"][:60]))
    out.append("")

    # 4. types по размеру — крупные = богатые объекты
    out.append("=== TYPE (топ-30 по размеру) ===")
    big = sorted((r for r in recs if r["kind"] == "type"),
                 key=lambda r: -r["bytes"])[:30]
    for r in big:
        out.append("%-8s %-34s %6d Б  %s" % (r["side"], r["name"], r["bytes"],
                                             r["desc"][:50]))
    return "\n".join(out)


if __name__ == "__main__":
    recs = collect()
    if "--json" in sys.argv:
        p = OUT / "vext-inventory.json"
        p.write_text(json.dumps(recs, ensure_ascii=False, indent=1), encoding="utf-8")
        print("wrote %s (%d records)" % (p, len(recs)))
    else:
        text = summarize(recs)
        (OUT / "vext-inventory.md").write_text(text, encoding="utf-8")
        print(text[:2000])
        print("...")
        print("wrote %s" % (OUT / "vext-inventory.md"))