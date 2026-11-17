# Как botlayer давал урон от пуль ботов (разбор агента 14.10.2026, сохранено основной сессией)

Главное: в рабочем запуске (host.bat: BF3ACT_BOTLAYER_HITHOOK=1, BOTARBITRATION=1) botlayer по урону пишет в движок
ОДНО: байт [[0x02B2AD70]+0x1E] (SyncedGameSettings.AllowClientSideDamageArbitration) = 0 только на время вызова
handleCollision 0x00E72040 и только для пуль своих ботов, затем возвращает прежнее значение (1). Позу, GA,
ForcePoseUpdate, cvar forceServerPoseUpdate, BulletDamageModifier в этом режиме только ЧИТАЕТ; в +0x112 не пишет вообще.

Попутно: наш «детур onDamage» на 0x01183000 — это ClientDamageListener::onMessage (хеш сообщения [msg+8] == 0x46E163B4,
флаг арбитража читается в 0x01183061). У ботов нет клиента — счётчик 0 ожидаем, урон он не меряет.
Мерить: здоровье float [солдат+0x34] (100 = полное) или детуром handleCollision.

## botlayer (дизасм DLL + сверка с exe) — ФАКТ
- Установка 0x10026B80; пролог 0x00E72040 = 55 8B EC 83 E4 F0 (украдено 6 байт). thiscall, this = пуля, 4 аргумента,
  ret 0x10 (0x00E720CD, 0x00E72360). Единственный вызывающий — 0x00E77CDD (рядом ServerProjectileEntityUpdater::update
  0x00E78080).
- Детур 0x10026DC0: подмена адреса возврата на 0x10026FA0, трамплин.
- До вызова (0x10027B50): стрелок = [[пуля+0x84]] − 8 (движок 0x00DD1DE0) → ServerPlayer; сверка со своими ботами;
  b = [0x02B2AD70] (0 — ничего), v = byte [b+0x1E], v ∈ {0,1} → запомнить, записать 0.
- После (0x10027A60): byte [b+0x1E] = v. Писатели 0x10027C97 (ноль), 0x10027AC9 (возврат).
- Лог BF3LegacyLan\logs\botlayer.log 11.10 02:44–02:46: детур 157, стрелок наш 157, выключили/вернули 157;
  40 строк «ЗДОРОВЬЕ бота … ИЗМЕНИЛОСЬ В ОБРАБОТЧИКЕ» (100000→77189); вне окна флаг 1 во всех 150+ чтениях.
- Поза в том же запуске: forceServerPoseUpdate 0, ForcePoseUpdate [AntSettings+0x39] 0 — для урона поза не нужна.
- Пути (чтение): anim = [солдат+0x148], GA = anim+0x20, m_animatable = [GA+0xC4] = [anim+0xE4]; в m_animatable:
  hasValidTransforms [+0x3C], [+0x3D], m_poseNeeded [+0x3E], poseUpdateEnabled [+0x112]; GA: +0x150, +0x155, +0x158 LOD,
  +0x160, +0x164, +0x228, +0x234; хитбоксы — от [солдат+0x544].
- Глобали (чтение): GameAnimationSettings [0x2B2AFD4]+0x44 = 1; AntSettings [0x2B18144] +0x32 = 1, +0x36 = 1, +0x39 = 0;
  SyncedGameSettings [0x2B2AD70] +0x1D FriendlyFire 0, +0x14 BulletDamageModifier 1.0; ServerSettings [0x2877500]
  +0x98 = 1, +0x9A = 1, +0x9B = 1, +0xA2 = 0.
- Глобальная запись +0x1E (0x1000845C, ключ ARBITRATION) есть, но автор признал её негодной.

## Рецепт (реализован в сборке 131)
Детур 0x00E72040 (сверка 12 байт пролога), стрелок [[пуля+0x84]] − 8, если наш бот — окно +0x1E = 0 только на вызов.
Вне окна 0 читают 0x01183061 (урон человека) и 0x0132E560 (NetworkSettingsMessage).
ГИПОТЕЗА: кандидат без детура — предикат 0x00FDF340 (из 0x00FE46DF) смотрит +0x1E, [игрок+0x318], бит [игрок+0x41C]&8.

Скрипты агента: scratchpad сессии — refs.txt, bd.py, sd.py, sref.py, calls.py, peekdmg.py.
