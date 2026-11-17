# -*- coding: utf-8 -*-
"""Инвентарь VEXT API, который реально вызывают боты fun-bots.

Ищет в *.lua:
  - calls вида  ClassManager:Method(  /  ClassManager.Method(  (список литералов
    из VEXT: PlayerManager, SoldierEntity, RaycastManager, ChatManager,
    NetEvents, Hooks, Events, EntityManager, MathUtils, TeamSquadManager...)
  - строки-имена событий:  EventManager:AddCallback / RegisterCallback / "Class:Event"
  - имена хуков: RegisterHook / AddHook / "Class:Hook"

Результат: какие API нужны ботам -> это и есть целевой список LegacyEXT.
"""
import sys, re, json
from pathlib import Path
from collections import defaultdict

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

ROOT = Path(r"C:\Games\BF-Portable\Battlefield_3_Server\Instance\Admin\Mods\fun-bots\ext")  # единственная рабочая копия (16.10.2026)
OUT = Path(r"C:\Games\Legacy\LegacyBF3")

# литералы, которыми VEXT-скрипты берут менеджеры/сущности
LITERALS = {
    "PlayerManager", "SoldierEntity", "ControllableEntity", "EntityManager",
    "RaycastManager", "ChatManager", "NetEvents", "Net", "Hooks", "Events",
    "EventManager", "HookManager", "TeamSquadManager", "TicketManager",
    "ServerUtils", "SharedUtils", "MathUtils", "ResourceManager",
    "SoldierWeapon", "GameEntity", "PhysicsEntity", "Raycast",
    "Player", "Soldier", "Entity", "Vec3", "Vec2", "Quat", "Transform",
    "ServerPlayer", "DataContainer", "WeaponFiring", "Rcon",
    "Client", "UI", "UIman", "UIManager", "SoldierHealthModule",
    "PhysicsEntityBase", "SpatialEntity", "SoldierWeaponsComponent",
    "NetSocket", "Message", "LineTransform", "AimAssist",
}

RE_CALL = re.compile(r"\b([A-Z][A-Za-z0-9_]{2,})\s*[:.]\s*([A-Za-z_][A-Za-z0-9_]*)\s*\(")
RE_STRING = re.compile(r"[\"']([A-Za-z][A-Za-z0-9_]*)\s*:\s*([A-Za-z_][A-Za-z0-9_]*)[\"']")
RE_EVT = re.compile(
    r"\b(?:AddCallback|AddListener|Register|RegisterCallback|Subscribe|Connect|"
    r"AddHook|RegisterHook|Hook|Destroy|RemoveCallback)\s*\(\s*[\"']([A-Za-z][A-Za-z0-9_]*)[\"']",
    re.I,
)


def main():
    calls = defaultdict(set)      # literal.method -> set(files)
    raw_names = defaultdict(set)  # Class:method from strings -> files
    events = defaultdict(set)
    files = sorted(ROOT.rglob("*.lua"))

    for p in files:
        try:
            txt = p.read_text(encoding="utf-8", errors="replace")
        except Exception:
            continue
        rel = str(p.relative_to(ROOT)).replace("\\", "/")

        for m in RE_CALL.finditer(txt):
            lit, meth = m.group(1), m.group(2)
            if lit in LITERALS:
                calls["%s:%s" % (lit, meth)].add(rel)
        for m in RE_STRING.finditer(txt):
            raw_names["%s:%s" % (m.group(1), m.group(2))].add(rel)
        for m in RE_EVT.finditer(txt):
            events[m.group(1)].add(rel)

    lines = []
    lines.append("VEXT API, используемый ботами fun-bots")
    lines.append("скан %d .lua файлов в %s" % (len(files), ROOT))
    lines.append("")

    lines.append("=== ВЫЗОВЫ МЕНЕДЖЕРОВ: Литерал:Метод (%d) ===" % len(calls))
    for k in sorted(calls, key=lambda x: (-len(calls[x]), x)):
        lines.append("  %-42s %2d файла(ов)  %s" % (k, len(calls[k]), sorted(calls[k])[0]))
    lines.append("")

    lines.append("=== ИМЕНА В СТРОКАХ Class:Method (%d) ===" % len(raw_names))
    for k in sorted(raw_names, key=lambda x: (-len(raw_names[x]), x)):
        lines.append("  %-42s %2d  %s" % (k, len(raw_names[k]), sorted(raw_names[k])[0]))
    lines.append("")

    lines.append("=== ЗАРЕГИСТРИРОВАННЫЕ ХУКИ/ИВЕНТЫ (%d) ===" % len(events))
    for k in sorted(events, key=lambda x: (-len(events[x]), x)):
        lines.append("  %-42s %2d  %s" % (k, len(events[k]), sorted(events[k])[0]))

    text = "\n".join(lines)
    (OUT / "funbots-api-usage.md").write_text(text, encoding="utf-8")

    (OUT / "funbots-api-usage.json").write_text(
        json.dumps(
            {
                "calls": {k: sorted(v) for k, v in calls.items()},
                "raw_names": {k: sorted(v) for k, v in raw_names.items()},
                "events": {k: sorted(v) for k, v in events.items()},
                "lua_files": len(files),
            },
            ensure_ascii=False, indent=1,
        ),
        encoding="utf-8",
    )

    print("lua files : %d" % len(files))
    print("calls     : %d" % len(calls))
    print("raw names : %d" % len(raw_names))
    print("events    : %d" % len(events))
    print("wrote %s" % (OUT / "funbots-api-usage.md"))


if __name__ == "__main__":
    main()