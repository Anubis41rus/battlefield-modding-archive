# LegacyBF3 — рабочий комплект Legacy (обновлено 16.10.2026, сборка 170)

Комплект нового стека: Lua-моды через `legacyext.dll` (наш аналог VeniceEXT), бэкенд Blaze, лаунчер. Прежняя версия
этого файла (статусы 08–10.10) — `C:\Games\trash\2026-10-16\docs-archive\LegacyBF3-README-before-170.md`.

## Структура

    run\                   скрипты стенда: start.cmd/start.ps1, status, tests, stop, join, cycle (см. run\README.md)
    Legacy.exe             лаунчер (рядом server.config → комплект найден); исходники — Legacy\launcher\
    server.config          пути к игре и BF-Portable, порты, bots.dll=legacyext.dll
    activation\            activation-loader.exe + activation.dll; legacyext.dll — развёрнутая сборка рантайма
                           (деплой — run\start.ps1 из Legacy\legacyext\build\, с бэкапом в C:\Games\backup\<дата>\)
    backend\               Legacy.Backend.exe (собранный из Legacy\backend\)
    license\               71067.dlf — лицензионный артефакт
    profiles\, progress\   аккаунты/солдаты, каталог статов
    logs\                  legacyext.log (лог рантайма; при старте >64 МБ → legacyext-<дата-время>.log),
                           run-state.json (смещение лога и md5 DLL текущего прогона), dedicated.log (лаунчер)
    ebx_index.bin          офлайн-индекс EBX (LEGACYEXT_EBX_INDEX)

Игра (`C:\Games\Battlefield 3`) и выделенный сервер (`C:\Games\BF-Portable`) в комплект не входят. Мод fun-bots — одна
рабочая копия: `BF-Portable\Battlefield_3_Server\Instance\Admin\Mods\fun-bots\` (только читать; дубль из `mods\` убран
16.10, удалён 19.10; оригинал `Config.lua` — `backup\2026-10-17\fun-bots-check\`).

## Справочные материалы в этой папке

- Цель и устройство: `GOAL.md` (читать первым), `VU-HOWITWORKS.md`, `DESIGN-LOCAL.md`, `VU-ARCH.md`, `VU-DESIGN.md`.
- API: `VU-API-SERVER.md`, `VEXT-API-MAP.md`, `funbots-api-usage.md` (что вызывает fun-bots; `funbots_api.py`),
  `vext-inventory.md` (`vext_inventory.py`).
- Реверс botlayer (только справочник): `BOTLAYER-REV.md`, `BOTLAYER-HOOKS.md`, `HOOK-TABLE.md`, `PHASE1-MAP.md`,
  `DECOMPILE-2026-10-10.md`, `RECON.md`; скрипты `triage_pe.py`, `extract_hooks.py`, `brain_ini.py`, `tools.py`.

Текущее состояние и как работать — `Legacy\ext\HANDOFF.md`. Правила — `C:\Games\AGENTS.md`.
