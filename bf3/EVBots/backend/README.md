# Legacy.Backend — свой бэкенд вместо Zeus

## Компактная упаковка бэкенда 262 (09.11.2026)

По прямой просьбе пользователя возвращена схема 258: код backend собирается AnyCPU, прежний x64 apphost использует уже установленный .NET 10 x64, как и лаунчер. Игровые DLL отдельно остаются x86. Включённый ранее runtime x86 увеличивал установщик с 10,1 до 42,5 МиБ и удалён из актуальной выдачи. Все исправления общего прогресса сохранены.

Сборка локальным SDK 10.0.401: `dotnet publish src/EVBots.Backend/EVBots.Backend.csproj --no-restore -c Release --self-contained false -p:PlatformTarget=AnyCPU -p:UseAppHost=false -p:IntermediateOutputPath=C:/Games/trash/2026-11-09/setup262-compact/obj-anycpu/ -o C:/Games/trash/2026-11-09/setup262-compact/backend-anycpu`. Нужен чистый IntermediateOutputPath: первая попытка в прежнем obj повторно выдала старые артефакты x86. В выходной каталог скопирован неизменённый `EV Backend.exe` из 258 (SHA-256 `06bd8ef2e9fe113a397b62021d7123cb6b9aebce36d5a16265b192219cc26dbe`); он запускает тот же файл `EV Backend.dll`. У новой DLL CLR flags=1 (AnyCPU), SHA-256 `ba62cbd9811f50310122e5681ab10ee906a862aeb62756634e56f24eb733b383`.

Проверка из установленной копии: progress-selftest 11/11, TDF PASS; DOTNET_HOST_TRACE показывает `C:/Program Files/dotnet`, а ошибочный DOTNET_ROOT_X86 не влияет. Каталога backend/runtime в выдаче нет. Тихая установка проверена начисто и поверх 258/большой 262: данные сохраняются, устаревший runtime большой версии уходит в бэкап обновления. Актуальная выдача — `dist/EVBots-262/`; большая упаковка сохранена в `trash/2026-11-09/release262-large-runtime/`, прежние kit/установщик — в `backup/2026-11-09/setup262/`. Игровые процессы и serve не запускались.

**Что это.** Локальный Blaze-эмулятор для Battlefield 3: аккаунты, солдаты,
прогресс/ранги, жизненный цикл игры, серверный браузер и раздача модов.
В Venice Unleashed ту же роль играет облачный **Zeus**; здесь он заменён этим
сервисом. Интернет не требуется.

> **Имена полей протокола — в `BLASE-FIELDS.md`.** Это источник истины.
> Правило проекта: не выдумывать имена полей; если поле не найдено —
> искать дальше, а не придумывать правдоподобное.

**Что уже работает** (проверено запуском, `tdf-selftest` и живыми запросами):

- TDF-кодек (все 11 типов, round-trip байт в байт) и кадры Blaze;
- TLS-слушатели: редиректор и ядро Blaze, свой CA + сертификаты;
- QoS-пробы игроков (UDP echo);
- HTTP: список серверов, профили, статистика, каталог прогресса, раздача модов;
- чтение **существующих** данных комплекта: `profiles\<personaref>\`,
  `progress\bf3-stats-catalog.json` — формат совместим, ничего конвертировать
  не нужно;
- создание солдат и клонирование прогресса (отсюда у ботов появляется ранг).

---

## Запуск

```
Legacy.Backend.exe serve [опции]
Legacy.Backend.exe gen-cert <каталог>
Legacy.Backend.exe tdf-selftest [--catalog <файл>]
Legacy.Backend.exe --help
```

Опции `serve` (порты по умолчанию совпадают со схемой `BF3LegacyLan`):

| Опция | По умолчанию | Назначение |
|---|---|---|
| `--redirector-port N` | 42127 | редиректор Blaze (адрес, который вводит клиент) |
| `--blaze-port N` | 10041 | ядро Blaze |
| `--http-port N` | 8080 | HTTP: серверы, статистика, раздача модов |
| `--qos-port N` | 17554 | QoS-пробы игроков |
| `--ev-port N` | 7828 | EV-канал (функции Zeus для EVBots.dll и лаунчера; 0 — выкл), `EV-PROTOCOL.md` |
| `--game-server-address A:B` | 127.0.0.1:25300 | адрес игрового сервера |
| `--profiles-dir PATH` | ./profiles | каталог профилей |
| `--progress-dir PATH` | ./progress | каталог прогресса |
| `--stats-catalog FILE` | <progress>/bf3-stats-catalog.json | лестница рангов |
| `--instance PATH` | из `server.config` | инстанс сервера с `Admin\` |
| `--mods-dir PATH` | <instance>/Admin/Mods | папка модов |
| `--advertise-ip IP` | авто | IP для серверного браузера (нашли `192.168.0.10`) |
| `--server-name "Имя"` | Legacy LAN Server | имя сервера, если нет `vars.serverName` |
| `--server-id GUID` | из каталога данных | свой ID сервера (вместо `server.key`) |
| `--debug` | выкл | подробный лог |

Проверка на существующих данных комплекта (порты другие, чтобы не мешать):

```
Legacy.Backend.exe serve ^
  --redirector-port 42128 --blaze-port 10042 --http-port 8081 --qos-port 17555 ^
  --profiles-dir  C:\Games\BF3LegacyLan\profiles ^
  --progress-dir  C:\Games\BF3LegacyLan\progress ^
  --stats-catalog C:\Games\BF3LegacyLan\progress\bf3-stats-catalog.json
```

---

## HTTP-маршруты

| Метод | Путь | Что делает |
|---|---|---|
| GET | `/` | обзор: порты, ID сервера, счётчики, состояние статистики |
| GET | `/servers` | список серверов (аналог `getServerBrowser.php` из BF-Portable) |
| GET | `/servers/{id}` | один сервер |
| GET | `/profiles` | солдаты: personaref, имя, убийства, счёт |
| GET | `/profiles/{personaref}` | полная статистика солдата |
| POST | `/profiles` | создать солдата: `{"name":"Имя","cloneFrom":234471775}` |
| POST | `/profiles/{personaref}/clone` | скопировать прогресс: `{"from":N}` |
| DELETE | `/profiles/{personaref}` | удалить солдата |
| GET | `/stats` | каталог прогресса: группа, число стат, размер payload |
| GET | `/admin/startup` | параметры из `Admin\Startup.txt` |
| POST | `/admin/startup` | задать параметр: `{"key":"vars.maxPlayers","value":32}` |
| GET | `/admin/maplist` | ротация карт из `Admin\MapList.txt` |
| POST | `/admin/maplist` | задать ротацию: `{"entries":[{"map":"MP_001","gamemode":"ConquestSmall0","rounds":1}]}` |
| GET | `/admin/modlist` | что загрузится, что загружено, что доступно |
| POST | `/admin/modlist` | команда `modList.*`: `{"command":"modList.Add","argument":"fun-bots"}` |
| POST | `/rcon` | строка команды: `{"command":"modList.Available"}` |
| GET | `/mods` | список модов + содержимое `ModList.txt` |
| GET | `/mods/{мод}` | описание мода (mod.json, список файлов) |
| GET | `/mods/{мод}/{файл}` | **скачать файл мода** (раздача клиентам) |

Примеры:

```
curl http://127.0.0.1:8080/servers
curl http://127.0.0.1:8080/profiles
curl -X POST http://127.0.0.1:8080/profiles -H "Content-Type: application/json" -d "{\"name\":\"Бот\",\"cloneFrom\":234471775}"
curl -X POST http://127.0.0.1:8080/admin/modlist -H "Content-Type: application/json" -d "{\"command\":\"modList.Add\",\"argument\":\"fun-bots\"}"
```

---

## Инстанс сервера в структуре VU

По умолчанию берётся из `server.config` существующего комплекта
(`game.dedicatedInstance`), иначе — `%APPDATA%\Legacy\server`. Переопределяется
опцией `--instance`.

```
<instance>\Admin\Startup.txt        vars.serverName "…", vars.gamePassword "…"
<instance>\Admin\MapList.txt        MapID GamemodeID Rounds
<instance>\Admin\ModList.txt        список модов, по имени папки
<instance>\Admin\Mods\<мод>\       mod.json, ext\{client,server,shared}\__init__.lua, ui.vuic
```

`Startup.txt`, `MapList.txt`, `ModList.txt` создаются при первом запуске, если их
нет. Имя сервера в браузере берётся из `vars.serverName` — как у настоящего BF3.

Команды `modList.*` из `hosting\commands.md`:

| Команда | Что делает |
|---|---|
| `modList.List` | что загрузится при следующем старте |
| `modList.ListRunning` | что загружено сейчас |
| `modList.Available` | моды в `Admin\Mods` |
| `modList.Add <мод>` | добавить в `ModList.txt` (не грузит сразу) |
| `modList.Remove <мод>` | убрать из `ModList.txt` |
| `modList.Clear` | очистить `ModList.txt` |
| `modList.Debug` | переключить режим отладки скриптов |
| `modList.ReloadExtensions` | перечитывание на живом сервере — пока отвечает, что нужен перезапуск |

Раздача модов: `GET /mods/<мод>/<файл>`. Отдаются только нужные клиенту файлы
(`.lua`, `.json`, `.vuic`, картинки, шрифты) — служебные каталоги `.git`,
`node_modules` и скрытые файлы не отдаются. Выход за пределы каталога мода
закрыт (проверка на `..`, ответ 403).

Проверено на `fun-bots`: 198 файлов к раздаче, из них **114 `.lua`** — это все
скрипты мода; `mod.json` и `ui.vuic` на месте.

---

## Как появляется ранг у игрока и у бота

```
клиент → getStatGroup("player_mpdefault2")
           = ОПРЕДЕЛЕНИЯ (лестница рангов)  ← progress\bf3-stats-catalog.json
клиент → getStatsByGroupAsync(EID=<personaref>)
           = ЗНАЧЕНИЯ игрока                 ← profiles\<personaref>\progress.json
```

Без записи прогресса клиент не знает ранг и в таблице его не рисует. Поэтому
при входе (`silentLogin` / `loginPersona` / `login`) сервис проверяет, есть ли
прогресс, и при отсутствии копирует его с первого игрока, у которого он есть.
Для бота это делается явно:

```
POST /profiles  {"name":"БотИзФазы5","cloneFrom":234471775}
```

---

## Что выяснено про форматы

### `getStatGroup` отдаёт НЕ TDF

Поле `getStatGroupPayloadBase64` из `bf3-stats-catalog.json` (135 690 байт) —
это **бинарный блок DICE `statgroups/bf3/bf3_player_mpdefault2.bin`**, а не
TDF: строки лежат как `<id:3 байта> 01 <длина> <ASCII> 00`, без меток-символов.

Совпадает с ошибкой в строках рабочего бэкенда: `getStatGroup payload is not TDF`.

**Вывод:** блок отдаётся клиенту байт в байт (`SendRaw`), разбирать его не нужно.

### TDF: важные детали формата

- **Метки — только буквы.** Символ кодируется как `(c & 0x1F) | 0x40`, поэтому
  цифры и знаки в метке не обратимы: `B63` прочитается как `BWBD`. В протоколе
  метки всегда заглавные буквы (`EVAL`, `VALU`, `GID`, `NAME`).
- **Varint — не protobuf.** Если первый байт `< 0x40`, значение напрямую
  (0..63). Иначе 6 младших бит и продолжение по 7 бит. 64-битному значению
  нужно до 10 байт: референсный декодер ограничен 8 итерациями и ломается.
- **Строки — байтовые, без Unicode.** UTF-8 кириллицы в игре станет
  «кракозябрами», поэтому `Tdf/AsciiSafe.cs` транслитерирует кириллицу в
  латиницу. В HTTP-морде имя остаётся полным.
- **Float — IEEE 754, 4 байта, little-endian** (соглашение DICE). Это
  единственный пункт, который нужно подтвердить живым клиентом.
- **HNET-особый случай:** список из двух структур пишется как
  `02 <struct> <struct> 00` вместо обычного списка.

### Кадр Blaze: расширенный размер

Заголовок — ровно **12 байт**. Если payload больше 64 КБ, бит 4 байта 9
устанавливается и **сразу после заголовка** идут ещё **2 байта** старшей
половины размера, и только потом payload:

```
[0..1]  размер payload, младшие 16 бит
[2..3]  component      [4..5] command     [6..7] error code
[8]     старшие 4 бита — тип сообщения (0 запрос, 1 ответ, 2 уведомление, 3 ошибка)
[9]     бит 4 — «размер расширенный»; младшие 4 бита — старшие биты message ID
[10..11] message ID (20 бит суммарно)
[12..13] старшая половина размера   ← только если бит 4 установлен
[14..]  payload (TDF)
```

Читать эти 2 байта из заголовка нельзя — индекса 12 в 12-байтовом массиве нет.
Именно на этом падал первый прогон сквозной проверки: `getStatGroup` отдаёт
135 690 байт, то есть всегда расширенный кадр.

### Порядок ответов

`getStatsByGroupAsync` сначала шлёт **уведомление** `Stats 0x32` с картой
статистики, и только потом пустой ответ на сам запрос. Клиент обязан уметь
пропускать уведомления — так же, как в настоящем Blaze.

---

## Известные ограничения

- **HTTP в LAN требует прав.** Windows не даёт `HttpListener` занять не-localback
  адрес без URL-ACL. При запуске выводится точная команда:
  `netsh http add urlacl url=http://+:8080/ user=<пользователь>` (один раз).
  Гостям HTTP не нужен — они заходят через Blaze-редиректор.
- **Полный проход клиента ещё не проверен.** Сквозная проверка закрывает
  транспорт, TLS, разбор кадров, диспетчер и обработчики, но не подтверждает,
  что набор и имена полей в ответах совпадают с ожиданиями клиента BF3.
  Очереди уведомлений (`PlayerJoining` → `ClaimingReservation` → `UserAdded` →
  `JoiningPlayerInitiateConnections`) и структуру `IPAD` в `getServerInstance`
  нужно сверить с живым клиентом.
- **На неизвестную команду сервис отвечает ошибкой 1003**, а не молчит:
  клиент ждёт ответа на каждый запрос и иначе зависает на этом кадре.
- **WebUI и Rime не делаем** первыми (см. `..\LegacyBF3\DESIGN-LOCAL.md`).
- **SQLite не используется** — JSON-файлы как в существующем комплекте.
  SQLite понадобится только если захотим историю матчей.
- **`modList.ReloadExtensions`** отвечает, что нужен перезапуск: горячая
  перезагрузка модов не реализована.

---

## Структура исходников

```
src\Legacy.Backend\
  Program.cs               точка входа, разбор команд и опций
  ServeCommand.cs          параметры запуска, старт/остановка слушателей, логгер
  CertificateTool.cs       свой CA и сертификаты для TLS
  Tdf\
    Tdf.cs                 модель полей TDF (11 типов)
    TdfCodec.cs            сериализация и десериализация
    AsciiSafe.cs           байтовые строки, транслитерация кириллицы
  Blaze\
    BlazeComponents.cs     номера компонентов и команд протокола
    BlazeFrame.cs          заголовок кадра, сборка и разбор
    BlazeListener.cs       TLS-приём, чтение кадров
    BlazeSession.cs        одно соединение, отправка ответов
    BlazeServices.cs       диспетчер и обработчики компонентов
    GameRegistry.cs        игры, игроки, реестр (аналог Zeus)
    QosListener.cs         UDP-пробы игроков
  Data\
    ProfileStore.cs        аккаунты, солдаты, прогресс
    StatsCatalog.cs        каталог статов и payload getStatGroup
    InstanceCatalog.cs     Startup.txt, MapList.txt, ModList.txt, modList.*
  Http\
    WebHost.cs             HTTP-маршруты
    ModServer.cs           список и раздача модов
  Tests\
    TdfRoundTrip.cs        самопроверка кодека, кадра, границ varint
    ProtocolRoundTrip.cs   сквозная проверка протокола через настоящий TLS
```

Источники протокола: `..\New-Blaze-Emulator` (TDF-кодек, номера команд),
`..\..\re\backend-utf16.txt` (наблюдаемое поведение рабочего бэкенда).

---

## Самопроверка

Две команды, обе без игры:

```
Legacy.Backend.exe tdf-selftest --catalog C:\Games\BF3LegacyLan\progress\bf3-stats-catalog.json
Legacy.Backend.exe protocol-selftest --profiles-dir C:\Games\BF3LegacyLan\profiles --progress-dir C:\Games\BF3LegacyLan\progress --catalog C:\Games\BF3LegacyLan\progress\bf3-stats-catalog.json --instance <папка>
```

### `tdf-selftest` — кодек и кадры

Четыре группы проверок:

- **A)** синтетический TDF: все типы → кодирование → декодирование → обратно,
  сравнение байт в байт, проверка границ varint;
- **B)** эталонный блок `getStatGroup`: pass-through без изменений и вывод,
  что это не TDF;
- **C)** кадр Blaze: сборка → разбор заголовка, все поля совпали;
- **D)** границы varint + транслитерация кириллицы.

Ожидаемый вывод: `ИТОГ: все проверки пройдены.`

### `protocol-selftest` — сквозная проверка через TLS

Поднимает слушатели на свободных портах и говорит с ними настоящим
TLS-клиентом: пишет кадры в сокет, читает ответы, разбирает TDF. Две сессии
(редиректор и ядро Blaze), в каждой 12 команд:

`fetchClientConfig`, `preAuth`, `postAuth`, `getTelemetryServer`,
`loginPersona`, `getStatGroup` (135 690 байт, расширенный размер кадра),
`getStatsByGroupAsync`, `getServerInstance`, `userSettingsSave`,
`userSettingsLoadAll`, `listUserEntitlements2`, неизвестная команда, `ping`.

Ожидаемый вывод: `ИТОГ: протокол отвечает на все проверенные команды.`

**Чего эта проверка не подтверждает:** что набор и имена полей в ответах
совпадают с тем, что ждёт клиент BF3. Для этого нужен живой клиент.
