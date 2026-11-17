# Трек 1 — техника: перечисление ServerVehicleEntity и брошенная техника

Агент (haiku) 16.10.2026; файл записал оркестратор из сообщения агента (агенту запись .md запрещена). Только чтение.
Скрипты агента: `C:\Temp\claude\C--Games\c4657c58-f592-45b8-8608-819ca88fc837\scratchpad\1\` (scan_exe.py … scan_exe8.py, disasm_preupdate.py).

## Сводка
1. Брошенную технику уничтожает ДВИЖОК (prePhysicsUpdate → abandonCheck 0x00DF9DA0), не мод и не мост. «Правила пустой техники» в моде нет (опровергнуто, MODLOG 6239).
2. Таймер стоит, вероятнее всего, потому что солдат (бот или игрок) в радиусе KeepAliveRadius (20 или 50 м) сбрасывает его (H1, средняя). Альтернативы: ApplyDamage=0 у спавна (H3), проверка дистанции до точки (H2).
3. Мост отдаёт технику пустым списком; EnterVehicle/ExitVehicle/controlledControllable — заглушки; событий Vehicle:* нет; `ControllableEntity()` в мосте не определена. В логе стенда GetIterator(ServerVehicleEntity) ни разу не вызывался.
4. Перечислять по образцу ListEntities: TypeInfo 0x02C17090 (имя проверено), vtable 0x025BF8E8. Без обёрток мод упадёт.
5. Для «взрыва» мост менять не нужно. Следующий шаг — read-only пробник (5.3).

## 2.1 Мод
- BotActions.lua:376–400 `_EnterVehicle`: GetIterator('ServerVehicleEntity'), Next(), `ControllableEntity(x)`, `x.transform.trans:Distance(...)`; нет техники — −3. 272–330: entryCount (293), GetPlayerInEntry (327), EnterVehicle (328).
- BotManager.lua:413–423 CheckForFlareOrSmoke — итератор, GetPlayerInEntry(0..2), uniqueId (420).
- Vehicles.lua:15–16, 29–30, 51 — controlledControllable:Is("ServerSoldierEntity"), `VehicleEntityData(x.data).controllableType:gsub(".+/.+/","")`; 74, 121 — entryCount; 78, 98, 121 — GetPlayerInEntry.
- Bot.lua 269, 305–375, 401, 434, 461, 581; VehicleMovement.lua 26, 69–83, 116–122, 352; запись `controlledControllable.transform` (173).
- GameDirector.lua 600 (ControllableEntity), 775–809 (OnVehicleUnspawn), 814–831 (OnVehicleExit; IsEmpty только для мобильных точек, 826), 836+ (OnVehicleEnter).
- Мод технику не уничтожает (Destroy только у солдат/ботов; слова abandon нет).
- Минимум от моста: итератор Next(); у сущности .transform.trans, .data, .uniqueId, .entryCount, :GetPlayerInEntry(i), :Is(тип); глобальные касты ControllableEntity(x), VehicleEntityData(x); controlledControllable; EnterVehicle/ExitVehicle.

## 2.2 Мост
- VextEntityManager.cpp:367–385 — GetIterator только KIND_CP и KIND_SPAWN; kTypes (43–46) — 2 типа; комментарий 15.
- VextPlayerManager.cpp:579–580 заглушки; 628 controlledControllable=nil. Хэши ServerVehicle*Message (VextEnums.cpp:1155–1165) не используются.
- VextConfig.cpp:69–203: UseVehicles=true, MaxBotsPerVehicle=3, KeepVehicleSeatForPlayer=true, SpawnInMobileRespawnVehicles=true.
- Адреса пути брошенной техники мостом не хукаются и не патчатся.

## 2.3 Движок
- TypeInfo ServerVehicleEntity 0x02C17090 (имя по TI−0x2EC → 0x025BCF20); контроль ServerCapturePointEntity 0x02C23918. TI+0x14 (rid) задаётся в рантайме. 53 ссылки на TI в .text.
- vtable 0x025BF8E8 (запись по 0x00DF6FA9, функция с 0x00DF6F90; второй imm по 0x00DF7F3D). Поправка к ENGINE-ENTITIES.md:25 — «конструктор 0x00DF6FAB» это адрес операнда.
- Поля Entity: +0x08 флаги, +0x10 шина, +0x14 data, +0x1C трансформ (trans +0x30).
- ControllableType data+0xD4 (геттер 0x00DE9660; формат значения не проверен).
- Места: вектор [+0xFC]..[+0x100] (0x00DEC560); entryCount = (end−begin)/4 — гипотеза. Игрок на месте: [[e+0x4C]+0x3C4] != 0.
- Здоровье: vt+0xDC → 0x00DE9640 → [ecx+0x234] → 0x008A3100 (сумма по массиву — гипотеза).
- Таймер брошенной [+0x250] (float), результат abandonCheck [+0x2F4] (byte), компонент [+0x10C] типа 0x02C17AD0.

## 2.4 Механизм брошенной техники (порядок — по дизассемблеру)
- prePhysicsUpdate (с 0x00DF9F20) → abandonCheck(this, dt) 0x00DF9DA0 (вызов 0x00DFA1B3; второй — 0x00DFA249 с dt=0). Компонент comp=[this+0x10C] типа 0x02C17AD0; данные d=[comp+0x14].
- P1 0x00DEFCD0: d.+0x1ED (ApplyDamageToAbandonedVehicles)==0 → выход; проверки [this+0x230]→[obj+0x84] vt 0xE0, [obj+0xBC]&5 (смысл неизвестен); bl=[comp+0xD18]: 0 → допустимо, иначе только при dist² ≥ RespawnRange² (d.+0x1D8).
- P2 0x00DEC560: живой солдат в месте → таймер = d.+0x1C0, выход.
- P3 0x00DF9CD0: пространственный запрос по [world+0x844] (world=[0x02B3EE30]) в радиусе d.+0x1E4 (KeepAliveRadius), объекты типа 0x02C174D0 (вероятно солдаты), лимит 10 → найден ≥1 → таймер = TUA, выход.
- Отсчёт [this+0x250] −= dt; > 0 → идёт. Распад: v = d.+0x1C4 (TUAID) + таймер, урон w·dt/v через 0x00D9BDF0 → 0x00D908A0; здоровье уходит к нулю за TUAID с.
- Любое присутствие (P2/P3) сбрасывает таймер на TUA (10 с).
- Поля d: ApplyDamage 0x1ED, RespawnRange 0x1D8 (15), TimeUntilAbandoned 0x1C0 (10), TimeUntilAbandonedIsDestroyed 0x1C4 (10), KeepAliveRadius 0x1E4 (20/50), WreckDuration 0x1D4, BotBailOutDelay 0x1D0, BotBailWhenHealthBelow 0x1CC, VehicleIsNearDistance 0x1E8. Проверка таблицы: CapturableType → 0xA8 совпадает с мостом.

## 2.5 EBX (карта MP_001, server.config:27)
- ApplyDamageToAbandonedVehicles=False: Base3_RU.txt:652, 799; Base4_RU.txt:594, 741; CQ_logic_US.txt:503; CQ_logic_RU.txt:617; CQL_logic_US.txt:911; CQL_logic_RU.txt:675.
- True: Base2_US.txt:623; Base3_US.txt:623; Base4_US.txt:747; CQ_logic_US.txt:209, 356; CQ_logic_RU.txt:470, 764; CQL_logic_US.txt:470, 617, 764; CQL_logic_RU.txt:528, 822, 969.
- Base3_RU 652–656: RespawnRange 15, TUA 10, KeepAliveRadius 50. DynamicSpawn_XP_M1Abrams 788–801: KeepAlive 20, VehicleIsNearDistance 15. BTR90.txt:10869 — ControllableType. Какой спавн MP_001 — БТР, не установлено.

## 2.6 История
- Записи, когда техника взрывалась, в MODLOG нет. EnterVehicle и итератор техники — no-op с начала моста; боты в технику не садились ни в одной сборке.

## 3. Гипотезы
- H1 (главная, средняя): солдаты в KeepAliveRadius (P3) сбрасывают таймер — 15 ботов, спавн на базе/точках, радиус 50 м.
- H2 (средняя): [comp+0xD18]≠0 и техника в 15 м от исходной точки.
- H3 (средняя): ApplyDamage=False у спавна этого БТР — тогда не регрессия.
- H4 (низкая): в месте игрок с живым солдатом. H5 (низкая): условия через [this+0x230]/[obj+0xBC].

## 4. Опровергнуто
- «В моде правило пустой техники, мост блокирует».
- «Заглушки GetIterator/EnterVehicle останавливают таймер».
- «Наш патч/перехват трогает технику» (для адресов пути).
- Имя поля «AbandonedTime».

## 5. Реализация (по решению оркестратора)
- 5.1 Перечисление: kTypes += {"ServerVehicleEntity", 0x02C17090, 0x025BF8E8} под `LEGACYEXT_VEHICLES=1`; перебор как ListEntities (57–88); обёртка: transform, data, entryCount, GetPlayerInEntry, Is, controllableType; алиас ControllableEntity(x) обязателен (иначе ошибки BotActions.lua:383, BotManager.lua:418, GameDirector.lua:600); чтение под __try.
- 5.2 Распад: мост не нужен; рычаги — спавн ботов (мод), данные уровня, патч по правилу 18.
- 5.3 Пробник `LEGACYEXT_VEH_PROBE=1`: раз в 2 с для каждой техники — адрес, ControllableType, comp и тип, d.+0x1ED/+0x1C0/+0x1C4/+0x1D8/+0x1E4, [comp+0xD18], таймер [+0x250], [+0x2F4], места и занятые, число солдат в радиусе и расстояния. Строка «VEH probe: …».

## 6. Проверить на стенде
- «VEH probe:» у стоящего БТР: ApplyDamage=1? таймер убывает? солдат в радиусе > 0? [comp+0xD18], расстояние до исходной точки.
