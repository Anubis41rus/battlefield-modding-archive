# Трек 5. Управление игроком: заглушки Player и RCON (сборка 155)

Дата: 15.10.2026. Режим: только чтение. Читались исходники моста, мод fun-bots (BF-Portable), VU-доки и бинарник `bf3_server_Final.exe` (capstone). Сборка, стенд, файлы моста не трогались.
Временные файлы: `C:\Temp\claude\C--Games\c4657c58-f592-45b8-8608-819ca88fc837\scratchpad\5\` (`disasm.py`, `disasm-out*.txt`).
Адреса даны как VA. Файловое смещение = VA − 0x400C00 (проверено: секция .text, VA 0x401000 ↔ raw 0x400).

---

## 1. Вопросы

1. Какие заглушки мод fun-bots реально вызывает (места, частота).
2. Что они должны делать по VU-докам и что ломается у мода, пока они заглушки.
3. Адреса движковых функций для кика, выбора оружия, фейда, ввода. Достаточно ли удаления бота (DeleteEnginePlayer) вместо Kick.

---

## 2. Факты (с источником)

### 2.1 Вызовы из мода

Проверено Grep по `BF-Portable\...\fun-bots\ext`. Копия `Legacy\LegacyBF3\mods\fun-bots` дает те же 22 вхождения.

| Заглушка (VextPlayerManager.cpp) | Вызовов в моде | Где в моде | Вывод |
|---|---|---|---|
| Player:Kick (:399) | 1 | BotSpawner.lua:154, только человек с именем как у бота | Боты через Kick не удаляются |
| Player:Ban, BanTemporarily, BanByName, BanByNameTemporarily, BanByIp, BanByIpTemporarily (:400–405) | 0 | — | Не вызываются |
| Player:Spawn (:406) | 0 | Мод зовет Player:SpawnSoldierAt (BotManager.lua:913). Это НЕ заглушка (VextPlayerManager.cpp:500) | Не вызывается |
| Player:SelectWeapon (:407) | 0 | EIASelectWeapon* в моде — вход _SetInput, не метод | Не вызывается |
| Player:AttachSoldier (:576) | 0 | — | Не вызывается |
| Player:EnableInput (:577) | 0 | — | Не вызывается |
| Player:Fade (:578) | 0 | — | Не вызывается |
| Player:EnterVehicle (:579) | 6 | BotActions.lua:328; Bot.lua:345, 360, 375; BotManager.lua:531 (человек из UI), 536 | Вызывается, но до моста не доходит (пункт 2.3, V) |
| Player:ExitVehicle (:580) | 2 | VehicleActions.lua:11; BotManager.lua:530 | То же |
| ResourceManager:GetSettings (VextResourceManager.cpp:234, всегда nil) | 1 | __init__.lua:599, только в обработчике BulletEntity:Collision | Сейчас мертвый код (пункт 2.3, H) |
| RCON:TriggerEvent (VextRcon.cpp:130–135) | 0 | — | Не вызывается |
| RCON:SendCommand (VextRcon.cpp:117–124), не в списке трека, но фейк | 3 | BotManager.lua:638 (vars.maxPlayers); __init__.lua:779 (vars.playerRespawnTime); __init__.lua:796 (modlist.ListRunning) | Вызывается, возвращает выдуманные данные (пункт 2.3, R) |

Удаление ботов в моде:
- Bot.lua:467–477 (Bot:Destroy): soldier:Destroy() + PlayerManager:DeletePlayer.
- BotManager.lua:1008–1019 (DestroyAllOldBotPlayers), строка 1016.
- Все команды «kick/destroy»: Chat.lua:498–535 (`!kick`, `!kickteam`, `!kickall`, `!kickp_Player`), RCON.lua:90–112 (funbots.kickAll, funbots.kickBot). Они идут через DestroyAll / DestroyBot / DestroyPlayerBots, затем Bot:Destroy и DeletePlayer. Ни одна не вызывает Player:Kick.
- Человека мод не кикает, кроме правила «имя как у бота» (BotSpawner.lua:147–168).

### 2.2 VU-доки (эталон; описаний почти нет, только сигнатуры)

- `server\type\player.md`:
  - Kick() / Kick(reason: string), :219–231.
  - Ban(reason?), :233–245. BanTemporarily(seconds), :247–. BanByName и BanByIp с Temporarily-вариантами, :268–331.
  - Spawn(spawnPoint: ServerCharacterSpawnEntity, checkSpawnArea: bool): bool, :338–353.
  - SelectWeapon(slot: WeaponSlot, weaponUnlockAsset: DataContainer, unlockAssets: DataContainer{}), :355–365.
  - SpawnSoldierAt(soldier, transform, pose), :407–417.
  - AttachSoldier(soldier), :430–438. EnableInput(action: EntryInputActionEnum, enable: bool), :440–449. Fade(fadeTime: float, fadeToBlack: bool), :451–460.
  - EnterVehicle(vehicle: Entity, entryId: int), :462–471. ExitVehicle(force: bool, exitOnEntry: bool), :473–482.
- Отдельного файла ServerPlayer в `server\type` нет. Методы описаны в player.md.
- `shared\library\resourcemanager.md:65`: GetSettings(settingsType: string): DataContainer | nil.
- `server\library\rcon.md`: SendCommand(command[, args]) → string{} (:11–12, :24, :40). TriggerEvent(event[, args]) → void (:17–18, :114–131), без описания.

### 2.3 Что делает мост сейчас (VextPlayerManager.cpp, если не указано иное)

- **K. Kick.** Заглушка :399, пишет лог «Player:Kick заглушка» и ничего не делает. Для ботов удаление уже настоящее: PlayerManager:DeletePlayer (:820–850). Если бот не nobodyRemoved и есть nativePtr, вызывается NativeBridge.DeleteEnginePlayer (:844–847).
- **DeleteEnginePlayer** (:5995–6013) ставит бота в очередь, только если:
  - (а) LEGACYEXT_BOT_DELETE не равен 0 (:5986–5993, по умолчанию включено);
  - (б) бот есть в списке наблюдения g_watchBot (проверка :6000–6002).
  Lua результат игнорирует (:846) и обнуляет nativePtr (:847). Если (б) не выполнено, бот молча остается в движке.
- **Список наблюдения ограничен 64** (g_watchBot[64], :2674; добавления :3189–3193, :3351–3354, :4891–4894). Позднему боту при полном списке остается только LOG_ERR «список наблюдения полон» (:3356). В legacyext.log таких строк 0.
- **Тиковое удаление** (:7889–7968): чистка всех списков, затем isAI (+0x318) := 0 (:7962) и движковый deletePlayer (:7963) с dmgr = NativeManager(). Очередь обрабатывается в тике, до апдейта менеджера. Это соответствует ассерту движка (пункт 2.4).
- **nobodyRemoved** (бот без тела): из движка НЕ удаляется, только забывается (:838–843; ForgetEngineBot :6027–6038; обработка :7954–7957). Причина в комментарии: падение 06:45, сборка 141.
- **Kill уже настоящий:** KillSoldierNow (:3731–3751) вызывает ServerSoldierEntity::kill 0x00E01660.
- **SpawnSoldierAt настоящий** (:500–550). Список заглушек в шапке (:14–15) устарел: там SpawnSoldierAt назван заглушкой.
- **Поля техники у игрока:** controlledControllable и attachedControllable = nil, controlledEntryId = 0 (:626–628). Мост их из движка не заполняет.
- **V. Техника моду не отдается.** VextEntityManager.cpp:15: «Технику (ServerVehicleEntity) и прочие типы пока не отдаём: мод полезет в поля, которых ещё нет». Списки техники в GameDirector (m_SpawnableVehicles и др.) поэтому пусты (гипотеза H1). В legacyext.log строк «EnterVehicle» и «ExitVehicle» 0 (файл 62 МБ, все сохраненные прогоны).
- **H. Хуки.** Мост вызывает только `Hooks._Fire('Soldier:Damage', …)` (:1529–1561). Хук 'BulletEntity:Collision' мод регистрирует (__init__.lua:145), но мост его не вызывает: диспетчера в src нет.
- **R. RCON.** SendCommand возвращает `{command, "100"}` (VextRcon.cpp:117–124, :123). Шапка файла (:18–19) говорит «пустую таблицу», то есть документация расходится с кодом. TriggerEvent — заглушка (:130–135). RegisterCommand хранит команды (:66–101), но ни один путь моста их не вызывает: диспетчера нет. Команды funbots.* извне, по коду, недоступны (гипотеза H12).
- **Лог.** legacyext.log (62 МБ) содержит 1033 строки «Player:SelectUnlockAssets заглушка». В текущем коде такой заглушки нет (VextPlayerManager.cpp:421 — настоящая функция). Значит, лог включает прогоны старее 155. Для проверки 155 смотреть хвост лога.

### 2.4 Движок (bf3_server_Final.exe, capstone)

| Функция (VA) | Подтверждение | Арность | Что делает (по коду и строкам) |
|---|---|---|---|
| admin.kickPlayer, обработчик 0x01139D50 | capstone: ret 8 (0x01139DDA, 0x01139F9B). Регистрация строкой «admin.kickPlayer»: kill_decomp.txt:279–280 (по тому же шаблону, что admin.killPlayer, :355–356) | 2 стек. + ECX (this = RCON-объект) | Аргументы — вектор смещений: [1] = имя игрока, [2] = причина (необязательно), размер 8 или 12 байт. Игрок ищется через 0x011311F0. Строки: «InvalidArguments», «InvalidPlayerName», «CanNotKickAdminPlayer», «OK». Дальше 0xD6E1C0 и 0xE48890 |
| ServerConnection::kickPlayer 0x00E48890 | capstone: ret 8 (0x00E488B6, 0x00E489FB). Строка «Kicking player %s with reason: %s reasonText: %s» (0x00E48962) | ECX = ServerConnection. Стек: int reason (в обработчике 0x16), затем std::string* | Если [this+0x2EC8] != 0, выходит без действия (0x00E4889F–0x00E488B6). Лог при флаге [0x2843658]&4. Дальше вызывает 0xD69230 (ret 0x10), назначение не разобрано |
| Поиск соединения по игроку 0x00D6E1C0 | capstone: ret 4 (0x00D6E1D0, 0x00D6E201, 0x00D6E20A) | ECX = менеджер [[0x2B3EE2C]+0x34]. Стек: ServerPlayer* | Перебирает вектор [this+0x2FF8..0x2FFC] и сравнивает игрока соединения. Геттер 0x00E3B0E0: `mov eax,[ecx+0x1924]; ret` (игрок соединения, поле +0x1924) |
| Поиск игрока по имени 0x011311F0 | capstone: ret без imm, вызов с одним push, сразу `add esp,4` в caller (0x01139E2D) — cdecl | 1 (указатель на строку) | Возвращает ServerPlayer* (killplayer_decomp.txt, FUN_011311f0) |
| deletePlayer (ServerPlayerManager) 0x00E67460 | capstone: ret 4 (0x00E6773D). Строка «Trying to delete ServerPlayer that is currently updating, Game will crash» (0x00E674FC) | ECX = менеджер; 1 стек. (ServerPlayer*) | Сравнивает индекс игрока с [this+0xA4] (текущий апдейт). Отсюда очередь до апдейта |
| ServerSoldierEntity::kill 0x00E01660 | capstone: ret 4 (0x00E016B2) | ECX = солдат; 1 стек. (1) | Уже используется мостом (SoldierEntity:Kill) |
| enterVehicle 0x00E37950 | capstone: ret 8 (0x00E37CF1, 0x00E37D00). Первый аргумент [entry+4] → ebp, его поля +0xA, +0xFC, +0x100 (вектор входов). Второй [entry+8] → entryId; проверка «ServerEntryComponent::getEntry invoked with invalid index (%i)» (0x00E379CD) | ECX = ServerPlayer. Стек: ServerEntryComponent*, int entryId | Вход занят игроком в +0x4C. Если занят ИИ-игроком (+0x318 != 0), снимает его через 0xE365F0(1,1). Дальше 0xE35450 |
| leaveControllable 0x00E36090 | capstone: ret 0xC (0x00E36238) | ECX = ServerPlayer. Стек: [ebp+8] int, [ebp+0xC] bool, [ebp+0x10] bool | Ассерт: [this+0x3B0] != 0 (0x00E360E1, пустой текст). Если [this+0x3C8] == 0, вызывает exitControllable (0x00E35AC0) |
| exitControllable 0x00E35AC0 | capstone: ret без imm (0x00E35F7E), 0 аргументов. Строка «ServerPlayer_exitControllable» (0x00E35AF0) | ECX = ServerPlayer | Согласуется с STUB-RESEARCH §3.3 |
| enterControllable 0x00E35450 | capstone: ret 8 (0x00E35AB7). Строка «ServerPlayer_enterControllable» (0x00E3548C) | ECX = ServerPlayer. 2 стек. ([ebp+8], [ebp+0xC]) | Шаг посадки, вызывается из enterVehicle |

**Не найдено** (берем из STUB-RESEARCH.md §4, пп. 1, 3, 4, 5; не перепроверял): SelectWeapon, AttachSoldier, EnableInput, Fade. Адресов нет. Мод их не вызывает.

---

## 3. Ответы

### Q1. Что вызывает мод
- Реально вызывается: Player:Kick (1 раз, человек с именем как у бота); Player:EnterVehicle (6 мест); Player:ExitVehicle (2 места); ResourceManager:GetSettings (1, мертвый код); RCON:SendCommand (3, фейк).
- Не вызывается вообще: Ban x6, Spawn, SelectWeapon, AttachSoldier, EnableInput, Fade, RCON:TriggerEvent.

### Q2. Что должно быть и что ломается
- **Kick (человек).** По VU: отключить игрока с причиной. Ломается правило «имя как у бота» (BotSpawner.lua:153–166): оно считает кик выполненным и убирает имя из очередей. Человек остается в игре с именем бота. Случается редко, в логе 0 раз.
- **Kick (бот).** Модом не вызывается. Удаление уже работает через DeletePlayer (пункт 2.3).
- **EnterVehicle / ExitVehicle.** По VU: посадить и высадить. Сейчас это no-op. Мод после ExitVehicle сбрасывает свое состояние (VehicleActions.lua:11–13: m_ActiveVehicle = nil, состояние Moving), то есть считает, что бот вышел. До этого кода мод сейчас не доходит: техника не отдается (VextEntityManager.cpp:15), в логе 0 вызовов. Значит, сейчас это не причина багов. Станет причиной, когда технику отдадут.
- **GetSettings.** По VU: DataContainer настроек «SyncedGameSettings». Сейчас nil, и мод делает return (__init__.lua:601). Ломания нет, потому что хук BulletEntity:Collision мост не вызывает. Если хук включат, ранний return до `p_HookCtx:Call()` (__init__.lua:601–607) может отменить штатную обработку пули бота. Это гипотеза H13 (по VU-семантике HookContext:Call).
- **RCON:SendCommand (вызывается модом).**
  - vars.maxPlayers возвращает "100" вместо 64 из `Startup.txt:6`. Получается Globals.MaxPlayers = 100 (BotManager.lua:638–646). Лимит ботов считается от 100 (BotManager.lua:856–865; BotSpawner.lua:428, 838–845). Это главный риск трека.
  - vars.playerRespawnTime возвращает "100", множитель 1.0 (__init__.lua:779–786). Реальное значение неизвестно (в Startup.txt его нет).
  - modlist.ListRunning возвращает пусто, поэтому флаги совместимости IsInputRestrictionDisabled и RemoveKitVisuals всегда false (__init__.lua:796–806).
- **Ban, Spawn, SelectWeapon, AttachSoldier, EnableInput, Fade, TriggerEvent.** Мод их не вызывает, поэтому не ломают его.

### Q3. Движок и Kick бота
- **Для бота достаточно текущего пути:** DeletePlayer, затем DeleteEnginePlayer, затем тик, затем deletePlayer (0x00E67460). Оговорки:
  - (а) бот вне списка наблюдения (64) молча остается в движке;
  - (б) nobodyRemoved остается в движке по дизайну (падение сборки 141);
  - (в) удаление откладывается до тика.
- Отдельного сетевого вызова для Kick бота не нужно, если у бота нет ServerConnection (гипотеза H2).
- **Для человека:** путь admin.kickPlayer подтвержден. Нативный вызов: 0xD6E1C0 (соединение), затем 0xE48890 (кик). Строковый ABI движка не разобран. Это риск.

---

## 4. Гипотезы (как проверить)

- **H1.** Техника не доходит до мода (VextEntityManager.cpp:15), поэтому GameDirector.m_SpawnableVehicles пуст и ветки EnterVehicle / ExitVehicle не выполняются. Проверка на стенде: строк «Player:EnterVehicle заглушка» в логе нет (сейчас 0).
- **H2.** У бота нет ServerConnection: 0xD6E1C0 вернет 0, и кик-путь для ботов не нужен, достаточно DeletePlayer. Проверка: при вызове кик-пути для бота получить лог «нет соединения» (в коде пока не написан).
- **H3.** Призраки в движке: (а) при более чем 64 отслеживаемых ботах; (б) для nobodyRemoved. Проверка: строки «список наблюдения полон» (ожидаем 0), «бот без тела … забыт мостом» (число), парные «поставлен в очередь на удаление в движке» и «удалён в движке (deletePlayer)». Сверить со счетом игроков в табе.
- **H4.** Фейковый maxPlayers 100 при реальных 64: при 65 и более Lua-игроках мост и движок могут разойтись. Сейчас латентно (строк «список наблюдения полон» нет). Проверка: строка мода «there are 100 slots on this server» (BotManager.lua:643). Куда пишет m_Logger, не проверено.
- **H5.** deletePlayer (0x00E67460) также удаляет тело солдата. Не проверено: разобраны первые 60 инструкций, ret 4 на 0x00E6773D. Проверка: дальше по капстону либо на стенде, остается ли тело бота после DeletePlayer.
- **H6.** Вход человека в технику через UI (UIServer.lua:77, BotManager.lua:1226–1240) читает p_Player.controlledControllable.entryCount. Поле nil (VextPlayerManager.cpp:628). Ошибка возможна только если FindOutVehicleType вернет технику, что по H1 не происходит.
- **H7.** Связь «брошенная техника стоит и не взрывается» с заглушками EnterVehicle / ExitVehicle. Явного кода взрыва пустой техники в моде нет (поиск по Explo, Abandon, Kill пуст). Связь не доказана; вне трека, проверить в треке техники.
- **H8.** 0xE48890 отправляет причину клиенту и рвет соединение (через 0xD69230 → 0xE47D90). Не разобрано.
- **H9.** Флаг [[0x285BAD8]+игрок+0xFD] означает «админ» (проверка «CanNotKickAdminPlayer»). Не подтверждено.
- **H10.** [ebp+8] у leaveControllable — entryId (передается в 0xE304E0 при ненулевом [this+0x3C4] или [+0x3C8]). Назначение 0xE304E0 не разобрано.
- **H11.** Значение 0x16 (22) в kickPlayer — код причины «KickedByAdmin». Не сверено с enum.
- **H12.** RegisterCommand не диспетчеризуется: funbots.* команды извне недоступны. Проверка на стенде: отправить funbots.kickAll через RCON и смотреть лог.
- **H13.** Ранний return GetSettings до `p_HookCtx:Call()` отменяет обработку пули. Актуально только если хук BulletEntity:Collision начнут вызывать.

---

## 5. Опровергнуто и уточнено

- **R1.** STUB-RESEARCH.md §3.2 (строка 85): enterVehicle = `int(this, entryId)`, один аргумент. **Опровергнуто:** RET 8, два стековых аргумента (ServerEntryComponent*, entryId). Сверка: capstone 0x00E37950, чтение [esp+4] и [esp+8]. Также неверно «готово к вызову» (§5, строка 176), пока неизвестен ServerEntryComponent машины.
- **R2.** «Мод вызывает Player:Kick для ботов». **Опровергнуто:** 0 вызовов для ботов, 1 для человека (BotSpawner.lua:154).
- **R3.** «Мод вызывает Player:Spawn». **Опровергнуто:** 0 вызовов. Мод использует SpawnSoldierAt, он настоящий.
- **R4.** «Ban, SelectWeapon, AttachSoldier, EnableInput, Fade, TriggerEvent вызываются модом». **Опровергнуто:** 0 вхождений в моде и 0 строк в логе.
- **R5.** «RCON:SendCommand возвращает пустую таблицу» (шапка VextRcon.cpp:18–19). **Опровергнуто кодом:** возвращает `{command, "100"}` (VextRcon.cpp:123).
- **R6.** «GetSettings → nil безвредно». **Верно только потому,** что хук BulletEntity:Collision не вызывается (см. H13).
- **R7.** «Хук BulletEntity:Collision работает». **Опровергнуто:** мост вызывает только Soldier:Damage (VextPlayerManager.cpp:1561).
- **Подтверждено:** exitControllable 0 аргументов (согласуется с STUB-RESEARCH §3.3); leaveControllable 3 аргумента (RET 0xC); enterControllable 2 аргумента (RET 8); kickPlayer 2 аргумента (RET 8); deletePlayer 1 аргумент (RET 4).

---

## 6. Предлагаемая реализация (ничего не внедрено)

Приоритет по риску и пользе:

- **P1. vars.maxPlayers (RCON:SendCommand).** Реальный лимит. Вернуть значение из конфига или из env `LEGACYEXT_MAXPLAYERS=N`. Если env не задан, поведение текущее. Файл: VextRcon.cpp:117–124, ветка для 'vars.maxPlayers'. Риск: мод начнет ограничивать 64 слотами. Это ожидаемо. Проверить строку «there are 64 slots» в логе мода.
- **P2. Player:Kick для бота (onlineId == 0).** Lua-обертка как у Bot:Destroy: `if self.soldier then self.soldier:Destroy() end; PlayerManager:DeletePlayer(self)`. Файл: VextPlayerManager.cpp:399. Нативного кода нет, env не нужен. Риск: призраки, как у DeletePlayer (H3).
- **P3. Player:Kick для человека.** Env `LEGACYEXT_KICK=1`, по умолчанию ВЫКЛ. Нативно: `NativeBridge.KickEnginePlayer(ptr, text)` кладет в очередь, как g_deleteQueue (:5984–6013). В тике рядом с :7889:
  - conn = fastcall(0x00D6E1C0)(this = [[0x2B3EE2C]+0x34], arg = player);
  - если conn != 0 и [conn+0x2EC8] == 0, нужна строка движка (ABI!), затем fastcall(0x00E48890)(conn, 0x16, &str).
  - Риски: (1) ABI строки движка: ctor и dtor не разобраны (в обработчике 0x01139D50 это 0x402090, 0x4025C2, 0x532A60, 0x4C1E80, 0x4C2010); (2) 0xD69230 рвет соединение, поэтому только из тика; (3) неверная арность приводит к падению 0xC0000005 (см. STUB-RESEARCH §5).
  - Нужен указатель ServerPlayer человека. Проверить, есть ли у человека nativePtr.
  - Приоритет низкий: мод кикает человека в одном случае.
- **P4. SelectWeapon, AttachSoldier, EnableInput, Fade.** Не делать: мод не вызывает. Кандидат для SelectWeapon, если понадобится: мост уже ставит оружие по слотам (SetBotWeapons, VextPlayerManager.cpp:262–270). Как замену не проверял.
- **P5. EnterVehicle / ExitVehicle.** Не делать до отдачи техники моду (пункт V). Когда понадобится:
  - EnterVehicle → 0x00E37950. ECX = ServerPlayer. Стек: сначала push entryId, затем push entryComponent (первый аргумент = entryComponent). Нужен указатель ServerEntryComponent машины; смещение от Entity машины не найдено.
  - ExitVehicle → 0x00E36090. ECX = ServerPlayer. Стек: int entryId, bool force, bool exitOnEntry. entryId берется из текущего входа игрока (поле не разобрано).
  - Файлы: VextPlayerManager.cpp:579–580. Env: `LEGACYEXT_VEHICLE=1`, по умолчанию ВЫКЛ. Риск: неверная арность дает падение. Проверять только на стенде.
- **P6. GetSettings.** Оставить nil. Если хук BulletEntity:Collision начнут вызывать, сначала решить: вернуть DataContainer «SyncedGameSettings» или не вызывать хук. Файл: VextResourceManager.cpp:234.
- **P7. Ban x6, Spawn, TriggerEvent.** Оставить заглушки: мод не вызывает.
- **Побочно:** шапки VextRcon.cpp:18–19 и VextPlayerManager.cpp:14–17 устарели. RegisterCommand не диспетчеризуется (H12).

---

## 7. Что проверить на стенде (после сборки оркестратором; сам стенд не трогал)

- В legacyext.log (хвост прогона): строки «Player:Kick заглушка», «Player:EnterVehicle заглушка», «Player:ExitVehicle заглушка». Ожидаем 0.
- «RCON:SendCommand("vars.maxPlayers")» (VextRcon.cpp:121): ожидаем одну строку при старте. Строка мода «there are 100 slots on this server» подтверждает фейк (H4).
- «Bridge: бот … поставлен в очередь на удаление в движке» и «удалён в движке (deletePlayer)»: парно на каждого удаленного бота. Расхождение = призрак (H3).
- «список наблюдения полон»: ожидаем 0. «бот без тела … забыт мостом»: число призраков (по дизайну).
- Счет игроков: PlayerManager:GetPlayerCount() (Lua) и таб движка должны совпадать, кроме nobodyRemoved.
- Если тестируется P3 (кик человека): строка «Kicking player» (лог движка при включенном флаге) и строка моста о кике. Если соединения нет, строка «нет соединения».
