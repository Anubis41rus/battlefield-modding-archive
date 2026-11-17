#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""verify_ebx_index.py — проверка бинарного индекса, который читает legacyext.dll.

Проверяет ровно то, что делает C++-сторона (VextEbxIndex.cpp):
  * магию и версию заголовка;
  * согласованность смещений с размером файла;
  * что индекс отсортирован по path в нижнем регистре и по GUID (иначе
    бинарный поиск в dll отдаст не тот инстанс);
  * наличие путей, которые ищет мод fun-bots, с ожидаемым классом.

Использование:
    python verify_ebx_index.py [путь_к_индексу]
"""

import struct
import sys

MAGIC = b"LEBXIX02"
HEADER_SIZE = 64
ENTRY_SIZE = 40

# Пути, которые мод запрашивает, и ожидаемый класс инстанса.
# Источник — WEAPONS-RESEARCH.md, раздел «Приложение».
EXPECTED = {
    "Weapons/M67/U_M67": "SoldierWeaponUnlockAsset",
    "Weapons/M67/M67_Projectile": "ProjectileBlueprint",
    "Weapons/M416/U_M416": "SoldierWeaponUnlockAsset",
    "Weapons/M416/U_M416_Kobra": "UnlockAsset",
    "Weapons/Knife/U_Knife": "SoldierWeaponUnlockAsset",
    "Characters/Soldiers/MpSoldier": "SoldierBlueprint",
    "Gameplay/Kits/USAssault": "VeniceSoldierCustomizationAsset",
    "Gameplay/Kits/USEngineer": "VeniceSoldierCustomizationAsset",
    "Weapons/M416/M416": "SoldierWeaponBlueprint",
}

# Пути, которых в данных нет — мод их зовёт, но файла такого нет.
KNOWN_ABSENT = [
    "Gameplay/Kits/USEngi",
    "Gameplay/Kits/USEngi_GM",
    "Gameplay/Kits/USEngi_XP4",
]


def main(argv):
    path = argv[1] if len(argv) > 1 else \
        r"C:\Games\Legacy\legacyext\build\ebx_index.bin"

    with open(path, "rb") as fh:
        blob = fh.read()

    ok = True
    if len(blob) < HEADER_SIZE:
        print("[FAIL] файл короче заголовка: %d байт" % len(blob))
        return 1
    if blob[:8] != MAGIC:
        print("[FAIL] магия: %r" % blob[:8])
        return 1

    (version, count, entries_off, strings_off, strings_size,
     path_order_off, guid_order_off, flags, _res) = struct.unpack_from(
        "<IIQQQQQII", blob, 8)
    print("Версия      : %d" % version)
    print("Записей     : %d" % count)
    print("Размер      : %.2f МБ" % (len(blob) / 1048576.0))
    print("Блок строк  : %.2f МБ" % (strings_size / 1048576.0))

    if version != 2:
        print("[FAIL] версия %d, dll ждёт 2" % version)
        ok = False
    if entries_off + count * ENTRY_SIZE > len(blob):
        print("[FAIL] таблица записей выходит за файл")
        return 1
    if strings_off + strings_size > len(blob):
        print("[FAIL] блок строк выходит за файл")
        return 1
    if path_order_off + 4 * count > len(blob):
        print("[FAIL] таблица порядка по пути выходит за файл")
        return 1

    def s(off):
        start = strings_off + off
        end = blob.index(b"\0", start)
        return blob[start:end].decode("ascii", "replace")

    entries = []
    for i in range(count):
        off = entries_off + i * ENTRY_SIZE
        (plo, pa, ty, nm, gu, fi, doff, dlen) = struct.unpack_from(
            "<IIIIIIQQ", blob, off)
        entries.append((s(plo), s(pa), s(ty), s(nm), s(gu), s(fi), doff, dlen))

    path_order = struct.unpack_from("<%dI" % count, blob, path_order_off)
    guid_order = struct.unpack_from("<%dI" % count, blob, guid_order_off)

    # dll ищет бинарным поиском по path_order — он обязан быть отсортирован.
    keys = [entries[i][0] for i in path_order]
    if keys != sorted(keys):
        print("[FAIL] path_order не отсортирован — бинарный поиск в dll "
              "вернёт не тот инстанс")
        ok = False
    else:
        print("path_order   : отсортирован, ок")

    gkeys = [entries[i][4] for i in guid_order]
    if gkeys != sorted(gkeys):
        print("[WARN] guid_order не отсортирован")
    else:
        print("guid_order   : отсортирован, ок")

    by_path = {}
    by_guid = {}
    for e in entries:
        # Именованные инстансы (непустой путь) видны через SearchForDataContainer;
        # обычные (пустой путь, но есть GUID) — цели разыменовки ссылок.
        if e[1]:
            by_path.setdefault(e[1].lower(), e)
        by_guid.setdefault(e[4].lower(), e)

    print("именованных (по пути) : %d" % len(by_path))
    print("по GUID               : %d" % len(by_guid))

    print()
    print("Проверка путей, которые ищет мод fun-bots:")
    for want, want_type in sorted(EXPECTED.items()):
        e = by_path.get(want.lower())
        if e is None:
            print("  [MISS] %-46s (ожидался %s)" % (want, want_type))
            ok = False
        elif e[2] != want_type:
            print("  [TYPE] %-46s %s, ожидался %s" % (want, e[2], want_type))
            ok = False
        else:
            print("  [ ok ] %-46s %s" % (want, e[2]))

    print()
    print("Пути, которых в данных EBX нет (мод их зовёт — вернётся nil):")
    for want in KNOWN_ABSENT:
        print("  %-46s %s" % (want,
                              "нет в индексе (ожидаемо)" if want.lower()
                              not in by_path else "ЕСТЬ (!)"))

    print()
    print("Проверка ссылок (разыменовка) — то, ради чего индекс вырос до v2:")
    # Реальные GUID из Weapons\M416: путь Weapon -> Object -> WeaponFiring ->
    # PrimaryFire -> Shot.ProjectileData. Взяты из данных, не выдуманы.
    CHAIN = {
        "8775DC85-7E41-5821-C9F8-D4A144884C5E": "SoldierWeaponBlueprint",
        "E289DA4C-F443-4251-AC60-E8C23515CF1A": "WeaponFiringData",
        "CA28953E-9A4A-4579-B1B7-1FFBDBFBE7B9": "FiringFunctionData",
        "6C055C66-9145-4ECB-8C41-BBE1A6C91287": "BulletEntityData",
        "D0941902-5A77-43FF-AB4F-222CC70361B9": "AIWeaponData",
        "C569F719-AB04-7BB0-2C08-B906DBC9FD3A": "SoldierWeaponBlueprint",
        "9F789F05-CE7B-DADC-87D7-16E847DBDD09": "SoldierWeaponUnlockAsset",
    }
    for guid, want_type in sorted(CHAIN.items()):
        e = by_guid.get(guid.lower())
        if e is None:
            print("  [MISS] %s  (ожидался %s)" % (guid, want_type))
            ok = False
        elif e[2] != want_type:
            print("  [TYPE] %s -> %s, ожидался %s" % (guid, e[2], want_type))
            ok = False
        else:
            print("  [ ok ] %s -> %s" % (guid, e[2]))

    print()
    print("ИТОГ:", "OK" if ok else "ЕСТЬ ОШИБКИ")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))