"""Сравнение двух архивов «Проверка Legacy» (legacy-check.ps1): эталон (наш ПК) и проверяемый (друг).

python -I compare-check.py <наш.zip> <друга.zip> [--all]

Сравнивает manifest.csv по областям GAME1, SERVER1, STAND1, LAUNCHER1 (относительные пути, размер, MD5).
Печатает: чего нет у друга, что отличается, что лишнее у друга (лишнее — только с --all или в важных папках).
Только чтение.
"""
import csv
import io
import sys
import zipfile

# Папки, где лишние файлы важны (мод, настройки, языковые файлы, стенд)
IMPORTANT = ("instance\\admin", "data\\win32\\loc", "update\\patch\\data\\win32\\loc", "activation", "backend")
# Наш рабочий стенд шире комплекта друга: в STAND сравниваем только то, что ставит лаунчер
STAND_PAYLOAD = ("activation\\", "backend\\", "progress\\", "license\\")


def load(path):
    with zipfile.ZipFile(path) as z:
        text = z.read("manifest.csv").decode("utf-8-sig")
        report = z.read("report.txt").decode("utf-8-sig") if "report.txt" in z.namelist() else ""
    rows = {}
    roots = {}
    for r in csv.DictReader(io.StringIO(text), delimiter=";"):
        area = r["area"]
        roots.setdefault(area, r["root"])
        rows.setdefault(area, {})[r["rel"].lower()] = (r["rel"], int(r["size"]), r["md5"])
    return rows, roots, report


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    show_all = "--all" in sys.argv
    ours, our_roots, _ = load(sys.argv[1])
    theirs, their_roots, _ = load(sys.argv[2])
    for area in sorted(set(ours) | set(theirs)):
        a, b = ours.get(area, {}), theirs.get(area, {})
        print(f"\n===== {area}: наш {our_roots.get(area, '—')}  |  друг {their_roots.get(area, '—')}")
        if not a or not b:
            print("   область есть только с одной стороны")
            continue
        keys = set(a) | set(b)
        if area.startswith("STAND"):
            keys = {k for k in keys if k.startswith(STAND_PAYLOAD)}
        missing, diff, extra = [], [], []
        for k in sorted(keys):
            if k in a and k not in b:
                missing.append(a[k])
            elif k in b and k not in a:
                if show_all or k.startswith(IMPORTANT) or "\\mods\\" in k:
                    extra.append(b[k])
            else:
                (_, sa, ha), (_, sb, hb) = a[k], b[k]
                if sa != sb or (ha and hb and ha != hb):
                    diff.append((a[k][0], sa, sb, ha[:8], hb[:8]))
        print(f"   нет у друга: {len(missing)}, отличаются: {len(diff)}, лишние у друга (важные): {len(extra)}")
        for rel, size, _ in missing[:80]:
            print(f"   НЕТ    {rel} ({size})")
        for rel, sa, sb, ha, hb in diff[:80]:
            print(f"   ДРУГОЙ {rel}  наш {sa} {ha}  друг {sb} {hb}")
        for rel, size, _ in extra[:80]:
            print(f"   ЛИШНИЙ {rel} ({size})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
