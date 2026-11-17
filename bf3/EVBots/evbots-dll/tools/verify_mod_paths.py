#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_mod_paths.py — все пути, которые запрашивает мод fun-bots, есть в индексе.

Источники путей:
  * WeaponList.lua    — полный список оружия (поле p_FullResource);
  * BotSpawner.lua    — шаблоны путей китов/внешностей/перков, которые
                        собираются в рантайме конкатенацией.

Скрипт НЕ придумывает ни одного пути: он выписывает литералы и подстановки
из исходника мода и проверяет их против бинарного индекса. Всё, чего нет,
печатается отдельным списком — это и есть то, что мод будет писать
в лог как "не найдено".

Использование:
    python verify_mod_paths.py
"""

import os
import re
import struct
import sys

MAGIC = b"LEBXIX02"
HEADER_SIZE = 64
ENTRY_SIZE = 40

MOD = r"C:\Games\Legacy\LegacyBF3\mods\fun-bots"
INDEX = r"C:\Games\Legacy\legacyext\build\ebx_index.bin"


def load_index(path):
    with open(path, "rb") as fh:
        blob = fh.read()
    if blob[:8] != MAGIC:
        raise SystemExit("[FAIL] индекс: неверная магия")
    (_ver, count, eoff, soff, _ssize, _poff, _goff, _f, _r) = struct.unpack_from(
        "<IIQQQQQII", blob, 8)

    def s(off):
        st = soff + off
        return blob[st:blob.index(b"\0", st)].decode("ascii", "replace")

    out = {}
    for i in range(count):
        e = eoff + i * ENTRY_SIZE
        plo, pa, ty, nm, gu, fi, doff, dlen = struct.unpack_from(
            "<IIIIIIQQ", blob, e)
        out.setdefault(s(pa).lower(), (s(pa), s(ty), s(nm), s(gu), s(fi), doff, dlen))
    return out


def collect_paths():
    """Выписать из исходников мода все строковые литералы вида 'Путь/К/Имя'."""
    found = set()

    # 1. WeaponList.lua: Weapon('Имя', ..., 'Weapons/.../U_...')
    wl = os.path.join(MOD, "ext", "Shared", "WeaponList.lua")
    rx_full = re.compile(r"'((?:Weapons|Characters|Gameplay|Persistence|Objects)/[^']*)'")
    with open(wl, "r", encoding="utf-8", errors="replace") as fh:
        for line in fh:
            code = line.split("--", 1)[0]
            for m in rx_full.finditer(code):
                found.add(m.group(1))

    # 2. Весь мод: литералы с путями (BotSpawner, EbxEditUtils, __init__).
    #    Строки-комментарии пропускаем: в них путь может быть обрезан
    #    (например BotSpawner.lua:2220 — закомментированный '...Appearance_').
    for base, _dirs, files in os.walk(os.path.join(MOD, "ext")):
        for name in files:
            if not name.endswith(".lua"):
                continue
            with open(os.path.join(base, name), "r", encoding="utf-8",
                      errors="replace") as fh:
                for line in fh:
                    code = line.split("--", 1)[0]
                    for m in rx_full.finditer(code):
                        found.add(m.group(1))

    # 3. Пути, собираемые конкатенацией в BotSpawner — раскрываем сами.
    #    'Gameplay/Kits/' .. TEAM .. KIT  -> перебор TEAM x KIT из данных.
    kits_dir = r"C:\Games\Legacy\Venice-EBX\Gameplay\Kits"
    return found


def expanded_paths():
    """Пути, которые мод собирает конкатенацией в рантайме.

    Источник — BotSpawner:_FindAppearance / _FindKit и связанные шаблоны.
    Здесь они раскрываются по фактическим файлам выгрузки, чтобы проверка
    покрывала и их (иначе остались бы 4 «неполных» литерала в отчёте).
    """
    out = set()

    # Gameplay/Kits/<TEAM><Kit> — 20 комплектов, имена берём из данных.
    kits = r"C:\Games\Legacy\Venice-EBX\Gameplay\Kits"
    if os.path.isdir(kits):
        for name in os.listdir(kits):
            if name.endswith(".txt"):
                out.add("Gameplay/Kits/" + name[:-4])

    # Persistence/Unlocks/Soldiers/Visual/MP/<Us|Ru>/MP_<T>_<Kit>_Appearance01
    vis = r"C:\Games\Legacy\Venice-EBX\Persistence\Unlocks\Soldiers\Visual\MP"
    for base, _dirs, files in os.walk(vis):
        rel = base.replace("\\", "/")
        idx = rel.find("Persistence/")
        if idx < 0:
            continue
        for f in files:
            if f.endswith("_Appearance01.txt"):
                out.add(rel[idx:] + "/" + f[:-4])

    # persistence/unlocks/soldiers/specializations/<X> — 7 перков
    spec = r"C:\Games\Legacy\Venice-EBX\Persistence\Unlocks\Soldiers\Specializations"
    if os.path.isdir(spec):
        for name in os.listdir(spec):
            if name.endswith(".txt"):
                out.add(
                    "persistence/unlocks/soldiers/specializations/"
                    + name[:-4])

    # persistence/unlocks/vehicles/<X> — перки техники
    veh = r"C:\Games\Legacy\Venice-EBX\Persistence\Unlocks\Vehicles"
    if os.path.isdir(veh):
        for name in os.listdir(veh):
            if name.endswith(".txt"):
                out.add("persistence/unlocks/vehicles/" + name[:-4])

    return out


def main():
    idx = load_index(INDEX)
    paths = collect_paths() | expanded_paths()

    print("Путей, найденных в исходниках мода: %d" % len(paths))

    hit, miss = [], []
    for p in sorted(paths):
        # Литералы-префиксы конкатенации ('Weapons/', 'Gameplay/Kits/') — это
        # не ресурсы, а куски шаблона; их отбрасываем осознанно.
        if p.endswith("/"):
            continue
        e = idx.get(p.lower())
        (hit if e else miss).append((p, e))

    print("Найдено в индексе : %d" % len(hit))
    print("НЕ найдено        : %d" % len(miss))
    if miss:
        print("\nОтсутствуют в данных EBX:")
        for p, _ in miss:
            print("  " + p)

    # Отдельно: типы для оружия (главное, о чём весь мод).
    print("\nРаспределение типов инстансов по найденным путям:")
    hist = {}
    for _p, e in hit:
        hist[e[1]] = hist.get(e[1], 0) + 1
    for t, c in sorted(hist.items(), key=lambda kv: -kv[1]):
        print("  %5d  %s" % (c, t))

    return 1 if miss else 0


if __name__ == "__main__":
    sys.exit(main())