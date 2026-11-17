# -*- coding: utf-8 -*-
"""brain.ini: читаем как CP1251, вытаскиваем секции и все параметры.

brain.ini — «боевой профиль» ботов (93 КБ). Формат: комментарии-подсказки
и строки параметров. Нужен, чтобы понять, что уже настроено и что крутить.

Пример:
  python brain_ini.py
  python brain_ini.py --keys
"""
import sys, re
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

SRC = Path(r"C:\Games\BF3LegacyLan\activation\brain.ini")
OUT = Path(r"C:\Games\Legacy\LegacyBF3")

RE_KEY = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_.]*)\s*=\s*(.+?)\s*$")
RE_SEC = re.compile(r"^\s*\[(.+)\]\s*$")


def load():
    raw = SRC.read_bytes()
    for enc in ("cp1251", "utf-8"):
        try:
            return raw.decode(enc), enc
        except UnicodeDecodeError:
            continue
    return raw.decode("cp1251", errors="replace"), "cp1251(replace)"


def main():
    text, enc = load()
    lines = text.split("\n")

    params = []      # (line_no, section, key, value)
    section = None
    comments = []    # (line_no, text)
    for i, ln in enumerate(lines, 1):
        s = ln.rstrip("\r")
        st = s.strip()
        if not st:
            continue
        if st.startswith("#"):
            comments.append((i, st.lstrip("# ").strip()))
            continue
        m = RE_SEC.match(s)
        if m:
            section = m.group(1)
            continue
        m = RE_KEY.match(s)
        if m:
            params.append((i, section or "(без секции)", m.group(1), m.group(2)))

    out = []
    out.append("brain.ini — разбор")
    out.append("файл: %s" % SRC)
    out.append("размер: %d б, кодировка: %s, строк: %d" % (SRC.stat().st_size, enc, len(lines)))
    out.append("комментариев: %d, параметров: %d, секций: %d"
               % (len(comments), len(params), len({p[1] for p in params})))
    out.append("")

    out.append("=== СЕКЦИИ ===")
    for name in dict.fromkeys(p[1] for p in params):
        cnt = sum(1 for p in params if p[1] == name)
        out.append("  %-40s %3d параметров" % (name, cnt))
    out.append("")

    out.append("=== ПАРАМЕТРЫ (вкл/выкл и числовые) ===")
    on = [(p[2], p[3]) for p in params if p[3].strip().lower() in ("on", "off", "true", "false", "1", "0")]
    num = [(p[2], p[3]) for p in params if re.match(r"^-?\d", p[3])]
    other = [(p[2], p[3]) for p in params
             if p[3].strip().lower() not in ("on", "off", "true", "false", "1", "0")
             and not re.match(r"^-?\d", p[3])]
    out.append("-- переключатели (%d)" % len(on))
    for k, v in sorted(on):
        out.append("   %-40s %s" % (k, v))
    out.append("")
    out.append("-- числовые (%d)" % len(num))
    for k, v in sorted(num):
        out.append("   %-40s %s" % (k, v))
    out.append("")
    out.append("-- прочие значения (%d)" % len(other))
    for k, v in sorted(other)[:120]:
        out.append("   %-40s %s" % (k, v[:70]))
    out.append("")

    out.append("=== ИМЕНА-ФЛАГИ (префиксы) ===")
    pref = {}
    for _, _, k, _v in params:
        head = k.split("_")[0]
        pref[head] = pref.get(head, 0) + 1
    for k, v in sorted(pref.items(), key=lambda x: -x[1])[:30]:
        out.append("   %-24s %d" % (k, v))

    (OUT / "brain-ini.md").write_text("\n".join(out), encoding="utf-8")
    print("\n".join(out[:40]))
    print("...")
    print("wrote %s" % (OUT / "brain-ini.md"))


if __name__ == "__main__":
    main()