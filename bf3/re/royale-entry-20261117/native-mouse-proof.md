# Мышь Royale — 17.11.2026

Пользователь нажал Ready в DLL320482AD, но журнал WebUI не получил WebUI:Deploy: кнопка не получала мышь. В слое EVBots Frame.mouse по умолчаниюfalse; выбор владельца мыши учитывал только WebUI:EnableMouse. Royale использует нативный UIGraph с ActionKey MouseInput.

Проверка zbf3.exe Capstone:
- Таблица действий22B6698: ключB53C0BD2 (djb2-xor MouseInput), factory008EBAB0.
- Factory создаёт action с vtable020C0DF0, slot0=008F80E0.
- Action разбирает строковые bool-параметры и пишет UI+ A821/A822 в008F813E/44, затем77A7A0 пересчитывает ввод.
- UI getter00FF6120: mov eax,[02385CE0];ret.
- Пассивное чтение клиента7784: UI F60D5030, bytesA820..A823=01 01 01 00. Нативная мышь включена, наш WebUI её игнорировал.

EVBots теперь читает этот флаг только в кадре движка после проверки сигнатур getter/action; exe не меняется. EVBOTS_WEBUI_NATIVE_MOUSE=0 возвращает прежнее поведение. STA-поток WebUI получает уведомление и отдаёт мышь верхнему видимому загруженному фрейму, если нет явного DisableMouse. EnableMouse принудительно включает, ResetMouse возвращает наследование. Клавиатура остаётся отдельным явным запросом. VU ResetMouse10B6D2F0 — отдельный вызов слотаC8; DisableMouse10B6CBA0 вызываетC0(false), это разные операции.

Дополнительно: на Web_Loading было настоящее Connect03:55:47 и Deleted03:56:07, перед Connect основной карты03:56:47. Эти служебные сообщения теперь не превращаются в игровые Player-события, аналогично существующему фильтру Level; Engine:Message сохраняется. Иначе Royale раньше времени запускал HUD finalization и создавал граф мыши в служебной сцене.

DLL4C6F22A48ABB6202AAABA39AD8B7519E8917DF77B80E30B11C8B3D07F54AACF7: собрана, core97 PASS. Прогон17.11 04:12 подтвердил signatures PASS, nativeMouse enabled04:14:15.463 и владельца vu-battleroyale. Пришли SetSkin, Solo04:14:50.476 и Deploy04:14:53.682; nativeMouse disabled04:14:53.973 передал ввод игре. Пользователь подтвердил нажатие Ready. Дальнейший вход блокирует отдельная серверная ошибка MathUtils:RandomGuid()==nil, а не захват мыши. ServerConfig Royale PlayersPerTeam=4 для проверки Team type (соло/команда); MinPlayersToStart=1 сохранён. Бэкап ServerConfig-before-team-choice.lua в backup/2026-11-17/royale-checksum. Fun-bots не менялся.
