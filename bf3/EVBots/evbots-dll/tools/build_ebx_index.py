#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""build_ebx_index.py — индекс EBX-ресурсов: «путь -> инстанс» и «GUID -> инстанс».

Зачем
-----
Lua-мод fun-bots вызывает ResourceManager:SearchForDataContainer(<путь>) и
ожидает типизированный userdata-объект DataContainer, который затем кастует
_G[obj.typeInfo.name](obj) и обходит по ссылкам вида «Weapon.Object.
WeaponFiring.PrimaryFire» (WeaponClass.lua:51-65). Чтобы такая ссылка
отдала другой DataContainer, нужен индекс не только по именованным инстансам,
но и ПО ВСЕМ инстансам каждого раздела — по GUID.

Читать 1.4 ГБ текста в рантайме нельзя, поэтому здесь один раз строится
компактный бинарный индекс, который legacyext.dll грузит лениво.

ЧТО ИМЕННО ПИШЕТСЯ (проверено на Weapons\\M416\\M416.txt, Weapons\\M67\\U_M67.txt)
--------------------------------------------------------------------------------
    Partition 8CEB0AB8-434C-11E0-895E-C63F7BF240ED
    SoldierWeaponUnlockAsset 3A6B6A16-... #primary instance   <- ИМЕНОВАННЫЙ
        $::UnlockAssetBase
            ...
            Name Weapons/M416/U_M416
        Weapon Weapons/M416/M416/8775DC85-7E41-5821-C9F8-D4A144884C5E  <- ссылка
    ...
    WeaponFiringData E289DA4C-F443-4251-AC60-E8C23515CF1A      <- обычный
        PrimaryFire CA28953E-9A4A-4579-B1B7-1FFBDBFBE7B9                    <- ссылка
        Rumble::RumbleFiringData                            <- вложенная структура

• именованный инстанс (помечен #primary instance) даёт запись с непустым
  «путь» — по нему и работает SearchForDataContainer;
• ОСТАЛЬНЫЕ инстансы дают записи с пустым путём, но с GUID — по ним
  разыменовываются ссылки на соседние инстансы того же раздела.

ФОРМАТ ФАЙЛА (legacyebx index v2, little-endian)
------------------------------------------------
Заголовок 64 байта:
    0   char[8]  "LEBXIX02"   магия
    8   u32      version      = 2
    12  u32      entryCount   ВСЕГО записей (именованные + обычные)
    16  u64      entriesOff   смещение таблицы записей
    24  u64      stringsOff   смещение блока строк
    32  u64      stringsSize  размер блока строк
    40  u64      pathOrderOff u32[entryCount] — индексы записей, отсортировано
                        по strPathLo (для поиска по пути; у обычных инстансов
                        путь пустой, они сортируются в начало)
    48  u64      guidOrderOff u32[entryCount] — индексы записей, отсортировано
                        по strGuid (для разыменовки ссылок)
    56  u32      flags        бит 0: есть записи
    60  u32      reserved

Запись 40 байт:
    0   u32  strPathLo   путь в нижнем регистре ("" у обычных инстансов)
    4   u32  strPath     путь в исходном регистре ("" у обычных)
    8   u32  strType     класс инстанса ("SoldierWeaponUnlockAsset")
    12  u32  strName     значение поля Name инстанса
    16  u32  strGuid     GUID инстанса (36 символов, верхний регистр)
    20  u32  strFile     путь .txt относительно корня EBX
    24  u64  dataOff     смещение блока инстанса внутри .txt
    32  u64  dataLen     длина блока в байтах (до следующего заголовка
                        инстанса, следующего "Partition" или конца файла)

Блок строк: NUL-терминированные ASCII-строки, дедуплицированные.

Использование:
    python build_ebx_index.py                       # весь Venice-EBX
    python build_ebx_index.py --roots Weapons Gameplay Characters
    python build_ebx_index.py --root D:\\... --out D:\\...\\ebx_index.bin
"""

import argparse
import os
import re
import struct
import sys
import time

MAGIC = b"LEBXIX02"
VERSION = 2
HEADER_SIZE = 64
ENTRY_SIZE = 40


def build(root, out_path, roots_filter):
    # Заголовок инстанса: "<Тип> <GUID>" в начале строки.
    rx_instance = re.compile(
        rb"(?m)^([A-Za-z_]\w+) ([0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}"
        rb"-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12})(?![\w-])")
    rx_name = re.compile(rb"(?m)^\t+Name[ \t]*(\S*)")

    t0 = time.time()
    strings = bytearray()
    str_index = {}

    def put(s):
        """Положить ASCII-строку в блок строк (дедупликация), вернуть смещение."""
        off = str_index.get(s)
        if off is not None:
            return off
        off = len(strings)
        str_index[s] = off
        strings.extend(s)
        strings.append(0)
        return off

    entries = []
    scanned = 0
    skipped_dirs = 0

    for top in sorted(os.listdir(root)):
        full_top = os.path.join(root, top)
        if not os.path.isdir(full_top):
            continue
        if roots_filter and top not in roots_filter:
            skipped_dirs += 1
            continue

        for dirpath, _dirnames, filenames in os.walk(full_top):
            for name in filenames:
                if not name.endswith(".txt"):
                    continue
                path = os.path.join(dirpath, name)
                try:
                    with open(path, "rb") as fh:
                        data = fh.read()
                except OSError:
                    continue
                scanned += 1

                rel = os.path.relpath(path, root).replace("\\", "/")
                path_no_ext = rel[:-4]  # "Weapons/M416/U_M416"

                # Границы блоков: все заголовки инстансов + все "Partition ".
                heads = [(m.start(), m.group(1), m.group(2).upper())
                         for m in rx_instance.finditer(data)]
                parts = [data.find(b"\nPartition ", 0)]
                while parts[-1] != -1:
                    nxt = data.find(b"\nPartition ", parts[-1] + 1)
                    if nxt == -1:
                        break
                    parts.append(nxt)

                if not heads:
                    continue  # файла без инстансов в выгрузке нет

                def block_end(start, limit):
                    """Конец блока инстанса: следующий инстанс, следующий
                    раздел или конец файла — что раньше."""
                    end = limit
                    if parts[-1] != -1:
                        p = data.find(b"\nPartition ", start)
                        if p != -1 and p + 1 < end:
                            end = p + 1
                    return end

                # К какому Partition относится каждый заголовок — нужно, чтобы
                # блок инстанса не перелез через границу раздела.
                def next_head(start):
                    for h in heads:
                        if h[0] > start:
                            return h[0]
                    return len(data)

                for idx, (start, type_name, guid) in enumerate(heads):
                    limit = (heads[idx + 1][0] if idx + 1 < len(heads)
                             else len(data))
                    end = block_end(start, limit)

                    eol = data.find(b"\n", start)
                    tail = data[start:end] if eol == -1 else \
                        data[start:eol if eol < end else end]
                    is_primary = b"#primary instance" in tail

                    s_path_lo = put(path_no_ext.lower().encode()) if is_primary \
                        else put(b"")
                    s_path = put(path_no_ext.encode()) if is_primary else put(b"")
                    s_type = put(type_name)
                    s_name = put(b"")
                    if is_primary:
                        nm = rx_name.search(data, start, end)
                        if nm:
                            s_name = put(nm.group(1))
                    s_guid = put(guid)
                    s_file = put(rel.encode())

                    entries.append((s_path_lo, s_path, s_type, s_name, s_guid,
                                    s_file, start, end - start))

                if scanned % 20000 == 0:
                    sys.stderr.write(
                        "  ... %d файлов, %d инстансов, %.0f с\n"
                        % (scanned, len(entries), time.time() - t0))
                    sys.stderr.flush()

    entries.sort(key=lambda e: e[0])
    n = len(entries)

    entries_off = HEADER_SIZE
    strings_off = entries_off + n * ENTRY_SIZE
    path_order_off = strings_off + len(strings)
    guid_order_off = path_order_off + 4 * n

    def cstr(off):
        return strings[off:strings.index(b"\0", off)]

    path_order = list(range(n))
    guid_order = list(range(n))
    path_order.sort(key=lambda i: cstr(entries[i][0]))
    guid_order.sort(key=lambda i: cstr(entries[i][4]))

    header = struct.pack(
        "<8sIIQQQQQII", MAGIC, VERSION, n, entries_off, strings_off,
        len(strings), path_order_off, guid_order_off, 1 if n else 0, 0)
    assert len(header) == HEADER_SIZE, len(header)

    body = bytearray()
    for e in entries:
        body.extend(struct.pack("<IIIIIIQQ", *e))

    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "wb") as fh:
        fh.write(header)
        fh.write(body)
        fh.write(strings)
        fh.write(struct.pack("<%dI" % n, *path_order))
        fh.write(struct.pack("<%dI" % n, *guid_order))

    named = sum(1 for e in entries if e[0] or e[1])
    dt = time.time() - t0
    size = os.path.getsize(out_path)
    print("Файлов прочитано  : %d (пропущено каталогов: %d)"
          % (scanned, skipped_dirs))
    print("Записей в индексе : %d" % n)
    print("  именованных     : %d" % named)
    print("  по GUID          : %d" % n)
    print("Размер индекса    : %.2f МБ (%d байт)" % (size / 1048576.0, size))
    print("Время             : %.1f с" % dt)
    return 0


def main():
    ap = argparse.ArgumentParser(description="Сборка индекса EBX для legacyext")
    ap.add_argument("--root", default=r"C:\Games\Legacy\Venice-EBX",
                    help="корень выгрузки Venice-EBX")
    ap.add_argument("--out",
                    default=r"C:\Games\Legacy\legacyext\build\ebx_index.bin",
                    help="куда писать индекс")
    ap.add_argument("--roots", nargs="*", default=None,
                    help="ограничить список каталогов верхнего уровня")
    args = ap.parse_args()

    if not os.path.isdir(args.root):
        print("[ОШИБКА] нет каталога %s" % args.root)
        return 1
    return build(args.root, args.out, set(args.roots) if args.roots else None)


if __name__ == "__main__":
    sys.exit(main())