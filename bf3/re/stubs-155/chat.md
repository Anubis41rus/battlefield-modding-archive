# Трек 3 — чат: входящий Player:Chat и исходящий SendMessage

Агент (haiku) 15–16.10.2026; файл записал оркестратор из сообщения агента (агенту запись .md запрещена). Только чтение, capstone по исходному exe.
Скрипты и дампы агента: `C:\Temp\claude\C--Games\c4657c58-f592-45b8-8608-819ca88fc837\scratchpad\3\` (`tools\disasm_va.py`, `funcs.py`, `xref.py`, `strs.py`; `handleMessage.asm.txt`; функции в `f\`). `scratchpad\3\dis.py` ошибочный (затеняет модуль `dis`) — не использовать.

## Итог
1. Вход: `handleMessage` VA 0x00E4DAF0, thiscall, RET 8. Arg1 = M (ChatMessage: текст UTF-16 [M+0]..[M+4], маска 64 бита [M+0x110..0x117], тип канала [M+0x118]). Arg2 = S (отправитель; 0 для admin.say). Уверенность высокая.
2. Канал по маске: всем — lo=hi=0xFFFFFFFF; команда — TeamId (lo<17, hi=0); отряд — lo = 17 + 33*team + squad. Высокая.
3. Штатное `player.onChat` (0x113D810) из диспетчера ServerPlayerChatMessage (тип 0xDBC6F766): отправитель [B+0x28], текст [B+0x2C], маска [B+0x30]/[B+0x34] — соответствует VU `Player:Chat(player|nil, recipientMask:int, message:string)`.
4. Штатное `player.onChat` не годится для кириллицы: 0x49C460 сужает строку и отбрасывает символы >0x7F. Текст брать из UTF-16 самим.
5. Мост: `DetourChatMessage` (EngineHooks.cpp:221) уже на handleMessage. Рассылка — по образцу Soldier:Damage: очередь под SRWLock (`PushDmg`), `NativeBridge.PollChat`, `Events._DispatchEngine(Events,'Player:Chat', ...)`.
6. Исходящий `ChatManager:SendMessage` (VextChatManager.cpp:34–70) — только лог. Путь движка: ctor M (0x011346B0) → `handleMessage(ChatMgr, M, 0)` (как `admin.say`).
7. `ChatMgr = [[0x02B3EE2C]+0x34]+0x3034` (по коду admin.say). Адресная отправка — `ServerConnection::sendChatMessage` 0x00E3B7D0 (thiscall, RET 4).
8. Текст, начинающийся с «/», не рассылается (E4DC60). Длина по VU ≤127. Отправка через handleMessage снова проходит наш хук — нужен guard от рекурсии.
9. Низкая уверенность: кто строит M для клиентского чата; является ли S игроком моста.
10. Главный открытый вопрос: совпадает ли S (`[conn+0x20]`) с объектом игрока моста — проверяется одним сообщением человека в чат.

## 2. Факты
### 2.1 handleMessage 0x00E4DAF0
- Пролог `81 EC B4 02 00 00` (sub esp,0x2B4), как в EngineHooks.cpp:268–269. Файловое смещение 0xA4CEF0.
- thiscall (ECX = ServerChatManager*), 2 стековых аргумента, `ret 8` (E4DD8A).
- arg1 = M: `mov ebx,[esp+0x2bc]` (E4DAFE). arg2 = S: `mov edi,[esp+0x2cc]` (E4DB63); `test edi,edi / je` (E4DB69–E4DB6C) — S=0 поддержан.
- Лог «ServerChatManager::handleMessage(): » (0x025CAB34) за флагом `[0x2843658]&1` (E4DAF6).
- Текст: begin = [M+0] & ~3, `cmp [M+4], begin` (E4DC56–E4DC5B, равенство — пусто), `cmp word [begin],0x2F` (E4DC60–E4DC64): «/» → E4DD65 без рассылки. Событие 549A20 отправлено раньше (E4DC51).
- Рассылка: `[ChatMgr+8]+0x2FF8` (D69050; копия E4CC40, E4DC6D–E4DC72) — вектор ServerConnection; игрок `[conn+0x1924]` (E4DC94); ключ `[player+0x20]` 64 бита (E4DCA2); карта `ChatMgr+0x30` (E4DCAE, E40930); маска получателя node+0x30/+0x34 (E4DD0A–E4DD0D).
- Фильтр E39FB0 (E39FB0–E3A039): аргументы [M+0x110], [M+0x114], маска получателя; слоты по 16 бит: A=0 пропуск, A=0xFFFF — всем, иначе совпадение с ненулевым слотом получателя.
- Отправка получателю: `E3B7D0(conn, &msg)` (E4DD33); строка «ServerConnection::sendChatMessage(): » (0x025C9210).
- Копия в объект события: E43130 (из E4DB5E), inline-буфер 0x80 wchar (`call 0x494420`), текст `call 0x4A1910`, поля +0x10C..+0x11C (E43156–E43186).

### 2.2 Кто вызывает handleMessage
- Клиент: `fb::ServerConnection::handleChatMessage` (строка 0x025CAC00; функция E4DDC0, ret 4): ChatMgr = [[this+0x1C]+0x3034] (E4DDC9–E4DDCD); `handleMessage(ChatMgr, M, [this+0x20])` (E4DE59–E4DE67) ⇒ S = [conn+0x20].
- `admin.say` (строка 0x264AA24; регистрация около 0x114CF20): проверки team ≤ 0x10 (0x113A24C), squad ≤ 0x20 (0x113A294); ChatMgr по цепочке [0x2B3EE2C]→+0x34→+0x3034 (0x113A2D8–0x113A2EB); `0x11346B0(M, 4, chId, 0, text, 0, 0)` (0x113A342–0x113A35E); `handleMessage(ChatMgr, &M, 0)` (ebx=0 с 0x113A037, вызов 0x113A370).

### 2.3 Структура M (ctor 0x011346B0, E43130)
- ctor: `0x011346B0(this=M, arg1, arg2, arg3, text, arg5, arg6)`, RET 0x18: arg1→+0x118, arg2→+0x110, arg3→+0x114, text→0x4A1910, arg5→+0x10C, arg6(byte)→+0x11C.
- +0x00 begin (UTF-16, младшие 2 бита — флаги), +0x04 end, +0x08 cap, inline +0x0C..+0x10B (128 wchar), +0x10C dword, +0x110 маска lo, +0x114 маска hi, +0x118 тип канала, +0x11C byte. Без vtable.
- Тип канала (VU fb/chatchanneltype.md): SayAll=0, Team=1, Squad=2, SquadLeader=3, Admin=4 (admin.say передаёт 4).

### 2.4 Маска (onChat 0x113D8DC…, 0x1175F50)
- Все: (lo & hi) == −1 (0x113D8E4–0x113D8E9). Нет адресата: lo|hi == 0 (0x113D903–0x113D905).
- Отряд: hi>0 или lo ≥ 0x11; q=(lo−0x11)/0x21, r=(lo−0x11)%0x21 (0x113D95D–0x113D962). Команда: lo < 0x11, hi == 0 (0x113D9C7).
- 0x01175F50 (cdecl, team, squad): chId = team*33 + squad + 0x11, проверка < 0x10000, возврат (chId<<16)|team в EDX:EAX. admin.say кладёт chId в lo без сдвига, hi = 0.

### 2.5 Штатное player.onChat
- Диспетчер (с 0x1152F6F): для типа 0xDBC6F766 (= −607717530, VextEnums.cpp:1093) → 0x112F1D0 → `0x113D810(this=edi, msg)` (0x1152F95–0x1152FAA); единственный прямой вызов.
- Строка 'player.onChat' (0x264943C), вызов 0x113D85B. Отправитель X=[B+0x28] (имя X+0x28, иначе 'Server'); текст [B+0x2C] → 0x49C460; маска [B+0x30]/[B+0x34].
- 0x49C460: `and eax,0x7F; cmp ax,bx; jne skip` (0x49C47B–0x49C481) — символы >0x7F пропускаются.

### 2.6 Мост сейчас
- EngineHooks.cpp:221–230 `DetourChatMessage`: счётчик `chatMessage`, `CallOriginal2(3, self, edx, arg1, arg2)`, лог первых 4 раз (72–77). Цель — таблица 268–269 (значения там VA; комментарий 260–263 про RVA устарел).
- Образец очереди: `DmgEvt g_dmgEvt[128]` + `g_dmgLock` (VextPlayerManager.cpp:7662–7664), `PushDmg` (7681–7694), `l_bridge_poll_damage` (7736–7750), регистрация (7309–7310); Lua 1526–1561 (`PollDamage` → `Hooks._Fire`); `Events:_DispatchEngine` (LuaEvents.cpp:241–246).
- Поля игрока: имя 0x10, OnlineId 0x20, солдат 0x3C4, команда 0x31C. `[pl+0x20]` = ключ карты чата ⇒ `[conn+0x1924]` — игрок моста.
- VU: `ChatManager:SendMessage(message[, toTeam[, toSquad]] | toPlayer | toPlayers)` → bool, ≤127; `Player:Chat(player|nil, recipientMask:int, message:string)`.
- fun-bots: подписка `Player:Chat` (ext/Server/__init__.lua:120), `OnPlayerChat` 451–454 → `m_ChatCommands:Execute`; при `p_Player == nil` — возврат (Commands/Chat.lua:15). Мост `Player:Chat` не шлёт.

### 2.7 Исходящая отправка
- `ServerConnection::sendChatMessage` 0x00E3B7D0 (thiscall, msg*, RET 4): при `[conn+0x2E7C] ≠ 0` вызывает 0x01A376E0 (не разобрано).
- handleMessage — и широковещательная рассылка. ctor M 0x011346B0: (M, channelType, maskLo, maskHi, text, playerId, isSenderDead).
- Личное (0x113A4C8–0x113A5BC): M(4, 0, 0, text, 0, 0) → `E3B7D0(conn, &M)`.

### 2.8 Смена подписки
- 0x00E50DC0 — ServerPlayerChangeChatChannelMessage (841705347): пишет маску [msg+0x30]/[msg+0x34] в map по [игрок+0x20] (E50DD3–E50DE9); map, вероятно, ChatMgr+0x30 (гипотеза).

### 2.9 Прочее
- Событие 549A20: `cmp [E+8], 0x7E267BC0` → 0x547660 (не разобрано). Строка 'max-chat-message-chars' (0x02765438).

## 3. Гипотезы
- H1 (средняя): S = объект игрока моста; для клиента S = [conn+0x20]. Проверка: S в логе = `LegacyEngineRealPlayer()`, [S+0x10] = ник.
- H2 (средняя): [M+0x10C] = playerId, [M+0x11C] = isSenderDead, [M+0x118] = channelType (по VU `UI:CreateChatMessage`).
- H3: клиентский чат идёт через handleMessage — растёт счётчик `chatMessage trampoline (N)`.
- H4: кто строит M для клиента — вызов E4DDC0 через vtable ServerConnection (ссылка в .rdata 0x025CA320), не разобрано.
- H5: 0x113D810 только для клиентского сообщения.
- H6: recipientMask для VU = lo32 (−1 = всем).
- H7: место дренажа исходящих — ServerPlayerManager::update (0x00A5EF00), соглашение не проверено.

## 4. Опровергнуто
- handleMessage получает ServerPlayerChatMessage (B) — нет, M и B разные объекты.
- Текст из штатного player.onChat без потерь — нет для кириллицы.
- Нулевой отправитель не поддержан — поддержан (admin.say передаёт 0).
- Мост уже шлёт Player:Chat — нет.

## 5. Реализация (флаги по умолчанию ВЫКЛ)
A. Входящий (`LEGACYEXT_CHAT_EVENTS=1`): в `DetourChatMessage` до `CallOriginal2` под `__try` прочитать текст (≤128 wchar), маску, тип, playerId, sender=arg2; UTF-16→UTF-8 своей функцией; очередь `g_chatEvt[64]` под SRWLOCK (+ счётчик дропов); Lua `NativeBridge.PollChat()` → `player = byPtr[senderPtr]` или nil → `pcall(Events._DispatchEngine, Events, 'Player:Chat', player, lo32, text)`; guard на время собственной отправки. Хук на 0x113D810 не ставить.
B. Исходящий (`LEGACYEXT_CHAT_SEND=1`, VextChatManager.cpp:34–70): очередь запросов {all|team|squad, teamId, squadId, text}; дренаж только в потоке движка; ChatMgr под `__try`; буфер M 0x130 на стеке; ctor 0x011346B0; `handleMessage(ChatMgr, M, 0)`. Маски: all → type 0, lo=hi=0xFFFFFFFF; team → type 1, lo=teamId; squad → type 2, lo=17+33*team+squad. Личное — E3B7D0 (соединение игрока не найдено). Риски: «/», длина, рекурсия, порядок вызовов не проверен.
Файлы: EngineHooks.cpp (221–230, 268–269), VextPlayerManager.cpp (~7657–7750, ~7309), VextChatManager.cpp (34–70).

## 6. Проверить на стенде
- Флаги выкл — как сейчас.
- `LEGACYEXT_CHAT_EVENTS=1`: «тест» в ALL → `chatMessage trampoline (N)`, lo=0xFFFFFFFF; TEAM → lo=TeamId; SQUAD → lo=17+33T+S; admin.say → S=0, Player:Chat(nil,…); S сверить с `LegacyEngineRealPlayer()`; «привет» в UTF-8 без потерь.
- `LEGACYEXT_CHAT_SEND=1`: `ChatManager:SendMessage('test')` виден клиенту, одно сообщение = один вызов handleMessage.

## 7. Открытые вопросы
1. Кто строит M для клиентского чата. 2. Смысл [B+0x28], M+0x10C, M+0x11C. 3. ServerConnection по игроку (для toPlayer). 4. Соглашение ServerPlayerManager::update. 5. Событие 549A20 / 0x547660. 6. Всё статическое — на живом сервере не проверено.
