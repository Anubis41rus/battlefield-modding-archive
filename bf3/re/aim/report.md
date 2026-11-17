# Огонь ботов: почему урона 0 (14.10.2026, агент, только чтение; сохранено основной сессией)

Итог: у 13 из 15 ботов пустой магазин в текущем оружии при запасе 66–400. Перезарядка не завершается — огонь
нажимается, пуль нет. Гипотеза «сервер стреляет только по сообщению клиента» по коду НЕ подтверждается: сервер сам
переводит EIAFire в выстрел. Данные: capstone по bf3_server_Final.exe (VA) + снимок памяти сервера (PROCESS_VM_READ).
Ghidra-проект re\ghidra-proj-server выдал «Unable to lock project» (было и 12.10). Скрипты агента — scratchpad сессии:
fire7.py, dis.py, callers.py, peekfire.py, peekwf.py, peekdyn.py.

## ФАКТЫ
1. ServerSoldierWeaponsComponent::updateWeaponFiring = 0x00E09750 (thiscall, [ebp+8] = EntryInputState).
   0x00E0993C–0x00E09970: action = [[[WF+0x94]+8]+0x110] (0x0089F7A0), level = getLevel(action) (0x008BEB00),
   порог = float [...+0x11C] (0x0089F810), огонь = level >= порог. Фронт нажатия/downTime не нужны.
   Снимок ввода → WeaponFiring 0x008B7970 (вызов 0x00E09D5B). Обёртка 0x00E0BAC0 (vtable-слот 0x025C309C):
   текущее оружие 0x00DFF6E0 → 0x00E09750.
2. Огонь гасится, если состояние персонажа ([солдат+0x160] vf+0x44) == 6 или нажато действие 0x2C (EIAGiveOrder):
   0x00E09C53–0x00E09C79. Нужно [комп+0x70] (солдат) != 0 и [солдат+0x144] (игрок) != 0.
3. Выстрел: колбэки WeaponFiring (0x0132A43B…0x0133CEA5) → ServerWeapon_primaryFireShotSpawnedCallback 0x00DE5ED0
   → ServerWeapon::fireShot 0x00DE5580 (вызов 0x00DE5F21).
4. ClientWeaponPlayerPrimaryWeaponFire (хеш 0xDFCAD330) на сервере — только диспетчер событий 0x01346879
   (SoldierFire/OutOfAmmo/SoldierReloading → менеджер 0x2B1FBA4), выстрела не делает.
5. Пути: солдат = [игрок+0x3C4]; компонент оружия = [солдат+0x53C]; слот = [[комп+0x68]+4];
   оружие = [[комп+0x30]+слот*4]; WF = [оружие+0x128]; МАГАЗИН = int [WF+0xF4] (assert
   «projectilesLoaded <= primaryAmmoMagCapacity» 0x008AF133; «есть патроны» [+0xF4]>0 в 0x008AF0F2);
   запас, вероятно, [WF+0xF8]; ёмкость — функция 0x0089F1E0.
6. Снимок 15 ботов: действие огня +0x110 = 7, порог 0.20–0.90, +0x114 = 15 (EIAReload), +0x1E8 = 1, isAI = 1.
   Магазин 0 у 13 из 15 (запас 66–400); у двоих 74/400 (F2DDCD20), 4/40 (F2DDD670).
   У пустых [WF+0xD0] = 0x80000000, у заряженных 0. Бот F2DD9E90 6 с держал огонь 1.0 при пустом магазине.
7. ServerSettings [0x02877500]: IsSoldierDetailedCollisionEnabled +0x98 = 1, IsSoldierAnimationEnabled +0x9A = 1,
   IsAiEnabled +0x9B = 1; GameAnimationSettings [0x2B2AFD4]+0x44 = 1;
   cvar ant.DetailedCollision.forceServerPoseUpdate [[0x2B47088]] = 0.
8. botlayer.dll трогает ровно эти места: forceServerPoseUpdate, poseUpdateEnabled [[GA+0xC4]+0x112],
   hasValidTransforms [+0x3C], m_poseNeeded [+0x3E], ForcePoseUpdate [+0x39], [wc+0x110],
   ServerProjectileDisabled [[+0x14]+0x98], BulletDamageModifier.

## ГИПОТЕЗЫ
- H1 (главная): магазин пуст, перезарядка не завершается ([WF+0xD0] = 0x80000000 — вероятно, флаг перезарядки).
  Перезарядку двигает анимация; без клиента конец, видимо, не приходит.
- H2: даже попавшие пули без урона — у хитбоксов нет позы на сервере (forceServerPoseUpdate = 0; botlayer включает).
- H3: оружие, записанное мостом в слоты, пришло с пустым магазином.

## Тесты (чтение памяти)
1. H1: [WF+0xF4], [WF+0xF8], [WF+0xD0] каждые 0.5 с с посадки: магазин → 0, запас не тратится, D0 → 0x80000000.
2. H3: тот же снимок в первую секунду после посадки: магазин сразу 0 → виноват мост.
3. H2: у бота с полным магазином сравнить убыль [WF+0xF4] с ростом onDamage.

## Не перепроверять
- Прицел: +0x1F4/+0x1F8 при +0x1E8 бит0 (геттеры 0x008BECB0/0x008BECD0/0x008BED10; копия 0x00DFC320).
- Действие огня = 7, уровень 1.0 доходит, порог < 1.0. Ввод из игрок+0x3C0 (prePhysicsUpdate 0x00E34280).
- Фронт нажатия/downTime для основного огня не нужны.
