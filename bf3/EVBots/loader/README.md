# EVBots Loader — `evbots-loader.exe`

Свой загрузчик EVBots вместо `activation-loader.exe` старого комплекта (BF3LegacyLan/BF3NativeBots, исходников нет).
Запускает `bf3.exe` / `bf3_server_Final.exe` подвешенным, внедряет DLL по одной (`LoadLibraryW` удалённым потоком)
и отпускает главный поток. Весь код x86. Создан 23.10.2026; поведение старого снято его дизассемблером
(`ext\MODLOG.md`, «Свой загрузчик EVBots»).

## Отличия от старого

| | старый `activation-loader` | `evbots-loader` |
|---|---|---|
| DLL на процесс | ровно одна; `--dll` **вытесняет** `activation.dll` | несколько, `--dll` повторяется, по порядку |
| клиент | `activation.dll` | `activation.dll` первой, затем каждая `--dll` (`--no-activation` — без неё) |
| сервер по умолчанию | `botlayer.dll` (нативные боты старого проекта) | `EVBots.dll` |
| исходники | нет | `src\EVBotsLoader.cpp` |

Остальное — как у старого:
- окружение: клиенту `EALaunchCode=1`, `ContentId=71067`; всем `BF3ACT_MODE`, `BF3ACT_LOG`, `BF3ACT_DRM` (`0` / `1` для
  `--drm-bypass` / `strict` для `--drm-strict`), `BF3ACT_DLF_SRC` (`--license-artifact`). Те же имена из родительского
  окружения вычищаются, остальное (`BF3ACT_DNSREDIR`, `EVBOTS_*`) проходит как есть;
- `logs\activation-ГГГГММДД-ччммсс.log` рядом с загрузчиком, если нет `--log`;
- рабочий каталог игры = `--game`; аргументы игры — после `--`, кавычки не трогаются;
- отказ работать с правами администратора (`bf3.exe` под админ-токеном не стартует);
- сторож клиента (`--guard`, отдельный процесс, `logs\guard-<pid>.log`): раз в 50 мс снимает `<игра>\Core\activation.exe`,
  выходит, когда `bf3.exe` и `EACoreServer.exe` ушли + 4 с, или через 20 мин; `--no-activation-guard` — без него;
- если внедрение не удалось — процесс игры завершается, а не работает без нашего слоя.

Коды выхода: 0 — ок, 2 — аргументы/права, 3 — нет exe или DLL, 4 — `CreateProcess`, 5 — внедрение/`ResumeThread`.

## Сборка и проверка

```
build.cmd                                           → build\evbots-loader.exe, build\test\marker1.dll, marker2.dll
powershell -ExecutionPolicy Bypass -File test\test.ps1   → TEST <имя> PASS|FAIL, TESTS-DONE fail=0
```

Тест гоняет загрузчик на `C:\Windows\SysWOW64\cmd.exe` с DLL-метками (`test\marker.cpp`), игру не трогает:
порядок DLL, окружение клиента и сервера, кавычки после `--`, коды ошибок, сторож.

## Где применяется

- **Стенд** — с 23.10.2026 14:03: `stand\activation\evbots-loader.exe`, `$Loader` в `stand\run\common.ps1`. Прогон: сервер
  (`--dll EVBots.dll`) и вход клиента (activation.dll) работают как со старым. Старый `activation-loader.exe` лежит рядом как запасной.
- **Лаунчеры** (код, 23.10): EV Launcher и окно стенда запускают `evbots-loader.exe` (`EVBots.Core\Paths.cs`: `LoaderName`,
  `FindLoader` — в старой установке без него берётся `activation-loader.exe`). Сборка без предупреждений, тесты ядра 102/0.
  Вживую окном не проверено.
- **Комплект и установщик** (`dist\…\EVBots\activation\`, `EVBotsSetup\kit.zip`) — ещё без `evbots-loader.exe`: добавить при
  следующей сборке комплекта (её отдают другу только по слову пользователя).
