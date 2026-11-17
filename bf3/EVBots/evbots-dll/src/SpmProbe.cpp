// SpmProbe.cpp — диагностика ServerPlayerManager для поиска createPlayer.
//
// Что установлено реверсом (Ghidra, bf3_server_Final.exe):
//   0x00E5EF00 = ServerPlayerManager::update, this = менеджер.
//   Он обходит ДВА массива УКАЗАТЕЛЕЙ на игроков:
//       [this+0x80] .. [this+0x84]   (счётчик = (end-begin)>>2)
//       [this+0x90] .. [this+0x94]
//   и для каждого вызывает ServerPlayer::update = 0x00E38190.
//   У ServerPlayer: [p+0x10] = имя, [p+0x3c4] = «есть солдат».
//
// Раньше ошибочно считали, что элемент массива = 0x2d0 байт (размер объекта).
// На деле это МАССИВ УКАЗАТЕЛЕЙ: шаг 4 байта (`>> 2`). Значит createPlayer
// обязан выделить ServerPlayer и положить указатель в [this+0x80..0x84].
//
// Здесь мы вешаем inline-hook на update и печатаем живые данные: сам менеджер,
// указатели обоих массивов и, если есть игрок, его имя/признак солдата.
// Так мы проверяем модель на живой игре и получаем адреса, которые Lua-мод
// сможет использовать для спавна.

#include "SpmProbe.h"

#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <intrin.h> // _ReturnAddress() — адрес вызывающего в DetourAddRef

#include "EngineHooks.h"
#include "InlineHook.h"
#include "EVBotsLog.h"

// Опережающие объявления extern-функций этого же файла (нужны в detour
// update, который определён раньше их реализации).
namespace evbots {

// Поставщики и повтор спавнов — определены в других TU, вне анонимного
// namespace (см. VextPlayerManager.cpp).
void* EVBotsEnginePlayerManager();
unsigned long EVBotsEngineSpawnPoint();
unsigned long EVBotsEngineSpawnPointBest();
void EVBotsBridgeRetryPending();
bool EVBotsBridgeDescribePtr(unsigned long v, char* out, unsigned cap);  // сборка 174: чей указатель (для VEH)
void* EVBotsEngineOnCreateMsg();
bool EVBotsIsBridgeBot(unsigned long p);  // сборка 201, VextPlayerManager.cpp  // сообщение о создании игрока  // повтор очереди моста
namespace {

// ВАЖНО: все следующие объявления — внутри анонимного namespace.
// Если объявить их в namespace evbots, а определить здесь, получится
// internal/external mismatch и LNK2019 (проверено).
unsigned long FindUsableSpawnPoint(unsigned long bot);
// ★ Точка спавна НОМЕР n из списка (каждому боту — своя).
// Нужно потому, что все боты на одной точке слипаются в геометрию и
// гибнут в ServerCombatAreaTriggerEntity через ~10 с после спавна
// (доказано 13.10 по колстэку в логе движка).
unsigned long SpawnPointNth(unsigned long n);
unsigned long SpawnPointNthByTeam(unsigned int team, unsigned long n);
void RememberSpawnPoint(unsigned long self, unsigned int team);
void DumpPointWords(const char* tag, unsigned long obj, int dwords);
// ★ Сборка 34: forward-декларации (используются в DetourSpawnSoldier до
// своих определений ниже по файлу).
DWORD WINAPI ScanSpawnThread(LPVOID unused);
void TrySoldierDiff();
unsigned long __fastcall DetourFindFree(void* self, void* edx,
                                        unsigned long p1, unsigned long p2,
                                        unsigned long p3);
void* __fastcall DetourSpawnEntryGet(void* self, void* edx, unsigned long p1,
                                     unsigned long index);
void InstallCrashLogger(void* moduleBase);  // VEH: стек падения в лог

// Смещения RVA (ImageBase 0x400000 вычтен из адресов Ghidra, который
// выдаёт уже готовые VA).
//
//   Ghidra VA 0x00E5EF00 -> RVA 0x00A5EF00 = ServerPlayerManager::update
//   Ghidra VA 0x00E38190 -> RVA 0x00A38190 = ServerPlayer::update
//
// ВАЖНО: если взять VA как RVA, адрес уедет на +0x400000 в участок
// C3 CC CC... (заглушки) — хук встанет, но функция никогда не вызовется,
// а в трамплин попадёт `C3` вместо пролога.
constexpr unsigned long kSpmUpdateRva = 0x00A5EF00UL;   // ServerPlayerManager::update
constexpr unsigned long kPlayerUpdateRva = 0x00A38190UL; // ServerPlayer::update

// Наблюдение за реальными вызовами спавна (подтверждённые адреса).
constexpr unsigned long kCreateWrapRva = 0x00A6F720UL; // FUN_00e6f720: create+add (__thiscall, 4 арг.)
constexpr unsigned long kAddToArrayRva = 0x00A6CA00UL; // FUN_00e6ca00: addToArray (__thiscall, 2 арг.)
constexpr unsigned long kSpawnSoldierRva = 0x009DBBD0UL; // FUN_00ddbbd0: спавн солдата на точке
// PointSystem::registerPlayer (PlayerPointsInfo), VA 0x0131F010 — 13.10, декомпиляция
// обработчика сообщений PointSystem 0x1320230 (ветка ServerPlayerCreateMessage).
constexpr unsigned long kPointsRegRva = 0x00F1F010UL;
// ★ vtable CharacterSpawnEntity (проверен статикой 13.10: 16/16 в .text,
// среди них 0x00E3E1D0=lastPoint). Скан кучи БЕЗ хуков.
constexpr unsigned long kSpawnEntityVtable = 0x025BBB70UL;

// ── Место падения, найдено по minidump 11.10.2026 ─────────────────────────────
// ACCESS_VIOLATION 0xC0000005, запись по 0x0DE4D54, EIP = 0x005C9D6C.
// Декомпиляция: AddRef — LOCK(); *(int*)(param_3+4)++; UNLOCK();
//(param_3) приходит из поля +0x19C точки спавна. Значение 0x0DE4D50
// повторяется ПОБАЙТОВО во всех прогонах — значит это не порча памяти,
// а фиксированное неверное значение. Хутим его, чтобы увидеть вызывающего
// и не ронять сервер.
constexpr unsigned long kAddRefRva = 0x001C9D60UL; // FUN_005c9d60: AddRef

// ── Разбор точек спавна (Ghidra, 11.10.2026, часть 3 отчёта) ────────────────
// FUN_00ddbbd0 — это CharacterSpawnEntity::spawn, this = ТОЧКА спавна.
// Три ранних отказа; движок умеет создать игрока сам, если вызвать его на
// AI/соло-точке с player == 0. Чтобы это проверить, нужно найти такие точки.
//   FUN_00e3e1d0() -> return this[0xCAC] + (-4), если this[0xCA8] != this[0xCAC]
//                     (последний элемент вектора точек спавна)
//   поле у точки: +0x14 -> CharacterSpawnReferenceObjectData,
//   у него +0x180 = тип (0 чел, 1 AI, 2 соло), +0x184 = команда,
//   +0x194 флаг, +0x195 флаг, +0x1A4 лимит
//   флаги точки: +0x49F, +0x318 (проверка «можно ли спавнить»)
// RVAs вычтены из VA вычитанием ImageBase 0x400000.
constexpr unsigned long kSpawnPointMgrRva = 0x00A3E1D0UL; // FUN_00e3e1d0
constexpr unsigned long kSpawnEntryRva = 0x00A37950UL;    // FUN_00e37950: точка по индексу
constexpr unsigned long kFindFreeEntryRva = 0x009CFC20UL; // FUN_00dcfc20: свободный вход
// Тип указателя для thiscall-вызовов (фиктивный EDX — см. инцидент №2).
using PtrFn1 = void* (__fastcall*)(void*, void*);
// Ещё две функции спавна — перехватываем, чтобы узнать их this/аргументы
// на живом спавне, а не угадывать (как с FUN_00e3e1d0 — промахнулся).
constexpr unsigned long kSpawnEntryGetRva = 0x00A37950UL; // FUN_00e37950
constexpr int kOffSpVecBegin = 0xCA8;    // начало вектора точек
constexpr int kOffSpVecEnd = 0xCAC;      // конец вектора точек
constexpr int kOffSpRefData = 0x14;      // указатель на данные точки спавна
constexpr int kOffSpType = 0x180;        // 0 = человек, 1 = AI, 2 = соло
constexpr int kOffSpTeam = 0x184;        // команда
// Подобъект CharacterSpawnEntity (его vtable 025BBB70) лежит на +0x80 внутри
// полного объекта точки спавна (13.10, диагностика POINT: F691EC80+0x80 = F691ED00).
constexpr unsigned long kCharSpawnSubobjOffset = 0x80;
constexpr int kOffSpFlag49F = 0x49F;     // флаг доступности точки
constexpr int kOffSpFlag318 = 0x318;     // второй флаг доступности
constexpr int kOffPlayerAiFlag = 0x318;  // флаг «это AI» у игрока
constexpr int kOffPlayerTeam = 0x31C;    // команда игрока
constexpr int kMaxSpawnPoints = 512;     // предохранитель на дамп
// смещения полей менеджера (из декомпиляции update)
constexpr int kOffArray1Begin = 0x80;
constexpr int kOffArray1End = 0x84;
constexpr int kOffArray2Begin = 0x90;
constexpr int kOffArray2End = 0x94;
// смещения полей ServerPlayer (из декомпиляции ServerPlayer::update)
constexpr int kOffPlayerName = 0x10;
constexpr int kOffPlayerSoldier = 0x3c4;

InlineHook g_hookSpmUpdate;

void* g_spmTrampoline = nullptr;
void* g_moduleBase = nullptr;

// ── Проверка флагов окружения ───────────────────────────────────────────────
// ГРАБЛИ (из-за этого серия тестов была ложными):
//   ::GetEnvironmentVariableA(name, nullptr, 0) возвращает НЕ значение, а
//   ДЛИНУ строки. Если переменная задана как "0", длина = 1 и проверка
//   `> 0` проходит — хуки ставятся, хотя флаг выключен.
//   == 0 означает лишь "переменная НЕ задана".
// Здесь читаем само значение: включено только при числе > 0.
bool EnvFlag(const char* name) {
    char buf[32] = {0};
    const DWORD n = EnvGetCached(name, buf, static_cast<DWORD>(sizeof(buf) - 1));  // сборка 168: зовётся на тиках update
    // Сборка 136: флаги, без которых стенд не работает, включены по умолчанию (как всё «рабочее» в VU — без
    // переключателей). Явное =0 по-прежнему выключает. Остальные флаги — опыты, по умолчанию выключены.
    if (n == 0) {
        static const char* const kDefaultOn[] = {"EVBOTS_SQUAD_HOOK", "EVBOTS_SPM_CUSTOM", "EVBOTS_PTS_REG",
                                                  "EVBOTS_TRACE_SETTEAM"};
        for (const char* d : kDefaultOn) {
            if (std::strcmp(d, name) == 0) return true;
        }
        return false;
    }
    if (n >= sizeof(buf)) return false;
    return std::strtol(buf, nullptr, 10) > 0;
}

// ── автоспавн: состояние, заполняемое перехватчиками ──────────────────────
volatile LONG g_haveCreate = 0;   // пойман реальный вызов createPlayer
volatile LONG g_createA1 = 0;     // его аргументы (эталон для бота)
volatile LONG g_createA2 = 0;
volatile LONG g_createA3 = 0;
unsigned char* g_wrapA2Copy = nullptr;  // копия буфера соединения
volatile LONG g_haveWrapCopy = 0;
volatile LONG g_spawnSelf = 0;   // this у spawnSoldier (НЕ менеджер)
// Эталон полей НАСТОЯЩЕГО игрока (указатели). Заполняется в update,
// когда найден игрок с непустым солдатом. Отдаётся ботам.
volatile LONG g_ref318 = 0;
volatile LONG g_ref31c = 0;
volatile LONG g_ref480 = 0;
volatile LONG g_refFields = 0;
// ★ ЭТАЛОН №2: поля игрока В МОМЕНТ СПАВНА настоящего игрока.
//
// Почему второй: эталон №1 снимается в update у игрока, У КОТОРОГО УЖЕ
// ЕСТЬ СОЛДАТ, и там значения — УКАЗАТЕЛИ на объекты солдата
// (+0x318=F2C0BF40, +0x31C=F2C0BF80, +0x480=F2C0D280). Копировать их боту
// бессмысленно — это состояние ПОСЛЕ спавна (отсюда исключения 12.10).
//
// В момент спавна движок передаёт ДРУГИЕ значения (проверено 12.10.2026):
//     +0x318 = 00000000     <- НЕ 1, как мы ошибочно писали боту
//     +0x31C = команда
//     +0x480 = УКАЗАТЕЛЬ    <- НЕ 0, у настоящего игрока он заполнен
// Вот эти значения и нужно проставить боту ДО вызова spawnSoldier.
volatile LONG g_sref318 = 0;
volatile LONG g_sref31c = 0;
volatile LONG g_sref480 = 0;
volatile LONG g_srefFields = 0;
// Значение +0x1924 у только что созданного движком игрока (reqPlayer-флаг).
volatile LONG g_ref1924 = 0;
// Помечаем, что сейчас мы САМИ вызываем spawnSoldier для бота: перехватчик
// не должен принять это за спавн настоящего игрока и не должен перезаписать
// эталон значениями бота.
volatile LONG g_ourSpawn = 0;
// Сообщение о создании игрока, которое движок шлёт живому игроку
// (FUN_00e48490). Отправляем его ботам, иначе они не считаются
// полноценными игроками и их нет в табе.
volatile LONG g_onCreateMsg = 0;
volatile LONG g_spawnA1 = 0;
volatile LONG g_spawnAttempted = 0;
volatile LONG g_spawnDepth = 0;     // вложенность вызовов spawnSoldier
volatile LONG g_pendingBot = 0;     // «пора спавнить бота», но не из хука
// ★ Эталон addToTeam: снимается перехватчиком у НАСТОЯЩЕГО игрока,
// повторяется мостом для ботов (регистрация в менеджере отрядов).
volatile LONG g_squadMgr = 0;
volatile LONG g_addTeamP2 = 0;
volatile LONG g_addTeamP3 = 0;
volatile LONG g_addTeamP4 = 0;
volatile LONG g_addTeamSeen = 0;
unsigned long g_botPlayer = 0;      // созданный игрок-бот
volatile LONG g_spawnTriesLeft = 0; // сколько попыток посадить солдата
volatile LONG g_spawnTryTick = 0;   // счётчик тиков между попытками
volatile LONG g_pointsDumped = 0;   // дамп точек спавна сделан один раз
const long kSpawnTries = 5;         // 5 попыток (меньше — меньше риска)
const long kSpawnTickGap = 60;      // интервал между попытками (тиками)
void* g_managerPtr = nullptr;    // ServerPlayerManager (из update)
volatile LONG g_nextSlot = 0;    // слот для следующего бота

// Глубина «проб» памяти на текущем потоке. Исключение внутри пробы —
// ожидаемое (его ловит свой __except), VEH его не логирует: иначе пробы
// съедают лимит записей, и настоящее падение в лог не попадает
// (так было в запуске 06:41 15.10 — падение 06:45 без строк VEH).
// RAII-счётчик рядом с __try нельзя (C2712) — инкремент вручную.
thread_local int t_probeDepth = 0;

bool Readable(const void* p) {
    if (p == nullptr) return false;
    bool ok = false;
    ++t_probeDepth;
    __try {
        volatile unsigned char c = *(const unsigned char*)p;
        (void)c;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    --t_probeDepth;
    return ok;
}

// Проверка ЗАПИСИ. Нужна отдельно от Readable: страница может быть
// читаемой, но не пишемой (PAGE_READONLY/PAGE_EXECUTE) — тогда
// Readable() даёт true, а оригинал AddRef на `*(int*)(arg3+4)++` падает
// с ACCESS_VIOLATION (запись по 0x0DE4D54). Именно это и было в дампе.
bool Writable(const void* p) {
    if (p == nullptr) return false;
    bool ok = false;
    ++t_probeDepth;
    __try {
        volatile unsigned char* q = (volatile unsigned char*)p;
        const unsigned char v = *q; // читаем
        *q = v;                     // пишем ТО ЖЕ САМОЕ — побочных эффектов нет
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    --t_probeDepth;
    return ok;
}

template <typename T>
bool Read(const void* base, int off, T* out) {
    if (base == nullptr) return false;
    // ★ Проверять надо ЦЕЛЕВОЙ адрес, а не базу. Раньше проверялась только
    // база, а читалось base+off -> выход за пределы страницы -> краш
    // сервера (инцидент 12.10.2026, дамп структуры reqPlayer).
    const void* target = (const unsigned char*)base + off;
    if (!Readable(target)) return false;
    *out = *(const T*)target;
    return true;
}

// Запись в память процесса. Страницу сначала делаем доступной для записи
// (нужно для строк, которые движок держит в read-only секции), потом
// возвращаем права как были.
template <typename T>
bool Write(const void* base, int off, T value) {
    if (base == nullptr) return false;
    unsigned char* addr = (unsigned char*)base + off;
    // сборка 237: запись в память движка — ядром (ev-core), оно снимает защиту страницы и ловит исключение
    ++t_probeDepth;
    const bool ok = PatchBytes(addr, nullptr, &value, sizeof(T), "зонд SpmProbe");
    --t_probeDepth;
    return ok;
}

// Считает число записей в массиве указателей без логирования.
unsigned DumpArrayQuiet(void* mgr, int beginOff, int endOff) {
    unsigned char** begin = nullptr;
    unsigned char** end = nullptr;
    if (!Read(mgr, beginOff, &begin) || !Read(mgr, endOff, &end)) return 0;
    if (begin == nullptr || end == nullptr || end < begin) return 0;
    // ВАЖНО: end - begin уже даёт ЧИСЛО ЭЛЕМЕНТОВ (разность указателей),
    // т.к. элемент = указатель. Делить ещё на 4 нельзя — так массив из
    // одного игрока (4 байта) даёт 4>>2 = 0. Исходник движка считает
    // ((int*)end - (int*)begin) >> 2, то есть байты/4 = элементы.
    return (unsigned)(end - begin);
}

// Опережающее объявление: дамп образца игрока (определён ниже).
void DumpPlayerSample(const void* p);

// Смещение Team в SpawnReferenceObjectData (по дампу POINT REF, 13.10: 2/1/0
// у трёх различных ref). Читает [[obj+0x14]+0x128]; 0xFFFFFFFF — не прочиталось.
constexpr int kRefOffTeam = 0x128;
unsigned int PointRefTeam(unsigned long obj) {
    unsigned long ref = 0;
    if (!Read((const void*)(ULONGLONG)obj, 0x14, &ref) || ref == 0) return 0xFFFFFFFFu;
    unsigned int t = 0xFFFFFFFFu;
    if (!Read((const void*)(ULONGLONG)ref, kRefOffTeam, &t)) return 0xFFFFFFFFu;
    return t;
}

const char* SafeCStr(const void* base, int off) {
    if (!Readable(base)) return "<unreadable>";
    const char* s = *(const char* const*)((const unsigned char*)base + off);
    if (!Readable(s)) return "<unreadable>";
    return s;
}

unsigned DumpArray(const char* tag, void* mgr, int beginOff, int endOff) {
    unsigned char** begin = nullptr;
    unsigned char** end = nullptr;
    if (!Read(mgr, beginOff, &begin) || !Read(mgr, endOff, &end)) {
        LOG_INFO("SpmProbe: %s: поля не читаются (this=%p)", tag, mgr);
        return 0;
    }

    if (begin == nullptr || end == nullptr || end < begin) {
        LOG_INFO("SpmProbe: %s: пусто (begin=%p end=%p)", tag, (void*)begin, (void*)end);
        return 0;
    }

    // end - begin = число элементов (см. комментарий в DumpArrayQuiet)
    const unsigned count = (unsigned)(end - begin);
    LOG_INFO("SpmProbe: %s: %u записей, begin=%p end=%p", tag, count,
             (void*)begin, (void*)end);

    for (unsigned i = 0; i < count && i < 32; i++) {
        void* p = (void*)begin[i];
        if (!Readable(p)) {
            LOG_INFO("SpmProbe:   [%u] %p <unreadable>", i, p);
            continue;
        }
        const char* name = SafeCStr(p, kOffPlayerName);
        int soldier = 0;
        Read(p, kOffPlayerSoldier, &soldier);
        LOG_INFO("SpmProbe:   [%u] player=%p name=\"%s\" soldier=%d", i, p,
                 name, soldier);
        // ★ Глубокий образец полей — только ОДИН раз за игру и только для
        // первого игрока. Раньше он печатался для каждого игрока при каждом
        // дампе — это основной источник мусора в логе.
        static volatile LONG g_sampleDone = 0;
        if (i == 0 && InterlockedCompareExchange(&g_sampleDone, 0, 0) == 0) {
            InterlockedExchange(&g_sampleDone, 1);
            DumpPlayerSample(p);
        }
    }
    return count;
}

// Первый увиденный живой игрок: снимаем дамп его полей и окрестностей.
// Это образец для спавна: по нему видно, что движок кладёт в новый Player
// и какие поля заполняет сразу после создания.
void DumpPlayerSample(const void* p) {
    // Раньше был флаг «сделано один раз» — этого мало: нужно сравнение
    // «живой игрок против бота», а значит дамп для КАЖДОГО объекта.
    // Ограничиваем 8 объектами, чтобы не залить лог.
    static const void* dumped[8] = {};
    static int dumpedCount = 0;
    if (!Readable(p)) return;
    for (int i = 0; i < dumpedCount; ++i) {
        if (dumped[i] == p) return;
    }
    if (dumpedCount >= 8) return;
    dumped[dumpedCount++] = p;

    LOG_INFO("SpmProbe: SAMPLE player=%p name=\"%s\"", p, SafeCStr(p, kOffPlayerName));

    // дамп начала объекта: тип/имя/vtable
    for (int off = 0; off < 0x40; off += 4) {
        unsigned v = 0;
        Read(p, off, &v);
        LOG_INFO("SpmProbe:   [p+0x%02X] = 0x%08X", off, v);
    }

    // поля вокруг «есть солдат»
    for (int off = 0x3B0; off <= 0x430; off += 4) {
        unsigned v = 0;
        Read(p, off, &v);
        if (v != 0) {
            LOG_INFO("SpmProbe:   [p+0x%03X] = 0x%08X", off, v);
        }
    }

    // ОБЛАСТЬ КОМПЛЕКТА. FUN_00e59340 (шаблон спавна) читает у игрока
    // ровно три поля: +0x318 (флаг AI), +0x31C (команда) и +0x480.
    // Первые два проверены, третье — нет, и старый дамп его НЕ ПОКАЗЫВАЛ:
    // он снимал 0x00..0x40 и 0x3B0..0x430, а 0x480 вне обоих диапазонов.
    // Ветка, где у игрока есть свой комплект, идёт как раз по нему.
    // Только чтение — никаких вызовов движка (иначе падение, инцидент №6).
    LOG_INFO("SpmProbe: --- комплект игрока (0x440..0x4C0) ---");
    for (int off = 0x440; off <= 0x4C0; off += 4) {
        unsigned v = 0;
        Read(p, off, &v);
        if (v != 0) {
            LOG_INFO("SpmProbe:   [p+0x%03X] = 0x%08X", off, v);
        }
    }
    unsigned kit = 0;
    Read(p, 0x480, &kit);
    LOG_INFO("SpmProbe:   >>> p+0x480 (свой комплект) = 0x%08X %s", kit,
             (kit == 0) ? "<== НУЛЬ, комплекта нет" : "");
    LOG_INFO("SpmProbe: SAMPLE done (следующих не будет, чтобы не spam-ить)");
}

int __fastcall DetourSpmUpdate(void* self, void* /*edx*/, void* args) {
    // this в update — тот же ServerPlayerManager, что и у createPlayer
    g_managerPtr = self;

    // ★ Честный замер полей НАСТОЯЩЕГО игрока. Раньше меряли только ботов:
    // перехватчик spawnSoldier срабатывает на ботах из очереди, а живой
    // игрок спавнится раньше и в дамп не попадает. Здесь идём от обратного:
    // в массиве игроков ищем того, у кого солдат (+0x3C4) НЕ пустой —
    // это гарантированно настоящий, заспавненный игрок.
    {
        static LONG tick = 0;
        if ((++tick % 600) == 0) {  // раз в ~20 с при 30 Гц
            unsigned begin = 0, end = 0;
            if (!Read(self, kOffArray1Begin, &begin) ||
                !Read(self, kOffArray1End, &end)) {
                begin = end = 0;
            }
            if (begin != 0 && end > begin && (end - begin) < 64) {
                const unsigned n = (end - begin) / 4;
                for (unsigned i = 0; i < n; ++i) {
                    void** slot = nullptr;
                    if (!Read(self, (int)(kOffArray1Begin + i * 4), &slot)) continue;
                    if (slot == nullptr) continue;
                    unsigned long pl = (unsigned long)(ULONGLONG)slot;
                    unsigned long sol = 0;
                    if (!Read((void*)pl, kOffPlayerSoldier, &sol) || sol == 0) {
                        continue;  // нет солдата — это бот или ещё не спавнен
                    }
                    unsigned v318 = 0, v31c = 0, v480 = 0;
                    Read((void*)pl, 0x318, &v318);
                    Read((void*)pl, 0x31C, &v31c);
                    Read((void*)pl, 0x480, &v480);
                    // ★ Признак «создано через фабрику». Ассерт движка в FUN_005d1c00
    // проверяет бит 1 в поле in_ECX[2] (это байт по +0x08 от начала
    // объекта) и ругается, если он не выставлен. Движок сам советует
    // setIsCreatedFlag(). Снимаем байт у настоящего игрока и у ботов.
    unsigned char flags = 0;
    Read((void*)pl, 0x08, &flags);
    LOG_INFO("TRACE НАСТОЯЩИЙ игрок ptr=%08X: +0x318=%08X "
                             "+0x31C=%08X +0x480=%08X солдат=%08X "
                             "ФЛАГИ+0x08=%02X (бит1=%d)", pl,
                             v318, v31c, v480, sol, flags,
                             (flags & 2) != 0);
                    // ★ ЭТАЛОН. У настоящего игрока эти три поля —
                    // УКАЗАТЕЛИ на объекты, а не простые значения.
                    // Старый вывод «+0x318 флаг AI, +0x31C команда» был
                    // НЕВЕРНЫМ. Запоминаем значения, чтобы мост проставил
                    // их ботам: без заполненного +0x480 (комплект) шаблон
                    // спавна отказывает.
                    if (InterlockedCompareExchange(&g_refFields, 0, 0) == 0) {
                        InterlockedExchange(&g_ref318, (LONG)v318);
                        InterlockedExchange(&g_ref31c, (LONG)v31c);
                        InterlockedExchange(&g_ref480, (LONG)v480);
                        InterlockedExchange(&g_refFields, 1);
                        LOG_INFO("TRACE ЭТАЛОН полей игрока сохранён: "
                                 "318=%08X 31C=%08X 480=%08X", v318, v31c,
                                 v480);
                    }
                    break;
                }
            }
        }
    }

    // Тик моста в потоке движка (EVBotsBridgeRetryPending): очереди спавна/убийств/удалений, поздние боты, лучи, позы,
    // попадания. ★ Сборка 172: каждый кадр update. Было «не чаще раза в 2 с» (осталось от первой очереди создания 11.10),
    // хотя всё, что потом переехало в этот тик, рассчитано на кадр: прогон 171 — спавн бота ждал до 2 с, CreatePlayer
    // мода стоял в RunLateCreateInHook до 1,97 с (тик Lua 1969 мс, 12,7 с из 20 — мод замирал), боты появлялись по одному
    // в 2 с (то же в 155, списано на dt). EVBOTS_BRIDGE_TICK_MS=N — не чаще раза в N мс (2000 — как до 172).
    static const LONG s_tickMs = [] {
        char v[12] = {};
        const DWORD n = EnvGetCached("EVBOTS_BRIDGE_TICK_MS", v, (DWORD)sizeof(v) - 1);
        return (n > 0 && n < sizeof(v)) ? (LONG)std::strtol(v, nullptr, 10) : 0L;
    }();
    static volatile LONG g_lastRetry = 0;
    const LONG now = GetTickCount();
    if (s_tickMs <= 0 || now - InterlockedCompareExchange(&g_lastRetry, 0, 0) >= s_tickMs) {
        InterlockedExchange(&g_lastRetry, now);
        ::evbots::EVBotsBridgeRetryPending();
    }

    // (Сборка 169: автоспавн тестового бота EVBOTS_SPM_SPAWN, повторные попытки посадки и дамп точек
    // EVBOTS_SPM_POINTS удалены — опыты 11–12.10, по умолчанию выкл.; ботов сажает мост.)

    // логируем не каждый кадр — только когда состав изменился или раз в ~5 с
    static unsigned lastCount = 0xFFFFFFFF;
    const unsigned c1 =
        DumpArrayQuiet(self, kOffArray1Begin, kOffArray1End);
    const unsigned c2 =
        DumpArrayQuiet(self, kOffArray2Begin, kOffArray2End);
    const unsigned total = c1 + c2;

    // ★ 12.10.2026: дамп только когда СОСТАВ ИГРОКОВ ИЗМЕНИЛСЯ. Прежний
    // вариант писал список игроков каждые 150 тиков (~5 с) — при 16 игроках
    // это ~19 строк каждые 5 с, и на фоне боевой симуляции лог сам по себе
    // подтормаживал игру. Теперь тишина, пока нечего не показывать.
    //
    // Сборка 93: счёт ботов перенесён в сводку моста («Bridge: МОСТ — под наблюдением…»):
    // эта сводка показывала 0 при 15 ботах (читала другой список, условие размера отбрасывало).
    if (total != lastCount) {
        LOG_INFO("SpmProbe: update this=%p arr1=%u arr2=%u total=%u", self,
                 c1, c2, total);
        DumpArray("arr1[0x80..0x84]", self, kOffArray1Begin, kOffArray1End);
        DumpArray("arr2[0x90..0x94]", self, kOffArray2Begin, kOffArray2End);
        lastCount = total;
    }

    if (g_spmTrampoline != nullptr) {
        // ★ ВОТ БЫЛА ПРИЧИНА 0xC0000409. Движок зовёт update как
        // FUN_00e5ef00(param_1) — это ОДИН стековый аргумент плюс this.
        // Детектор же был объявлен (self, args) — всего два параметра.
        // При проксировании в трамплин реальная функция читала ТРЕТИЙ
        // аргумент из нашего стека, где мусор => порча стека =>
        // __fastfail => 0xC0000409 STATUS_STACK_BUFFER_OVERRUN.
        // Поэтому здесь три параметра: self, edx и стековый аргумент.
        using Tramp = int(__fastcall*)(void*, void*, void*);
        Tramp tramp = (Tramp)(ULONGLONG)g_spmTrampoline;
        return tramp(self, nullptr, args);
    }
    return 0;
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════
// Поставщики данных движка для моста Lua -> движок.
// ОБЯЗАТЕЛЬНО вне анонимного namespace: иначе внутреннее связывание,
// и VextPlayerManager.cpp их не увидит (LNK2019).
// ═══════════════════════════════════════════════════════════════════════

// Файл уже внутри namespace evbots, поэтому дополнительная обёртка дала бы
// evbots::evbots:: и мост не нашёл бы символ.
void* EVBotsEnginePlayerManager() { return g_managerPtr; }
// team = 0xFFFFFFFF = любую (первую попавшуюся, как раньше)
unsigned long EVBotsEngineSpawnPointNth(unsigned long n, unsigned int team) {
    if (team != 0xFFFFFFFF) {
        unsigned long p = SpawnPointNthByTeam(team, n);
        if (p != 0) return p;
        // нет точек этой команды — берём любую (spawnPointBest)
    }
    return (unsigned long)g_spawnSelf;
}

unsigned long EVBotsEngineSpawnPoint() {
    return (unsigned long)g_spawnSelf;
}

// ★ Точка спавна для бота. Сначала берём настоящую (this у spawnSoldier —
// точку, на которую движок посадил живого игрока). Если её нет (живых
// игроков ещё не было), берём любую пригодную из списка, собранного
// конструкторами точек спавна при загрузке уровня. Без этого мост отказывал
// всем ботам: spawnPoint == 0 -> "движок отказал, остаюсь на Lua".
void EVBotsEngineRefFields(unsigned long* f318, unsigned long* f31c,
                           unsigned long* f480) {
    if (InterlockedCompareExchange(&g_refFields, 0, 0) == 0) return;
    *f318 = (unsigned long)InterlockedCompareExchange(&g_ref318, 0, 0);
    *f31c = (unsigned long)InterlockedCompareExchange(&g_ref31c, 0, 0);
    *f480 = (unsigned long)InterlockedCompareExchange(&g_ref480, 0, 0);
}

// ★ Регистрация в менеджере отрядов. Возвращает 1, если адрес addToTeam
// снят перехватчиком, и заполняет p2/p3/p4 его настоящими аргументами
// (для живого игрока). Мост повторит их для бота.
bool EVBotsEngineSquadAddTeamArgs(unsigned long* p2, unsigned long* p3,
                                  unsigned long* p4) {
    if (InterlockedCompareExchange(&g_addTeamSeen, 0, 0) == 0) return false;
    *p2 = (unsigned long)InterlockedCompareExchange(&g_addTeamP2, 0, 0);
    *p3 = (unsigned long)InterlockedCompareExchange(&g_addTeamP3, 0, 0);
    *p4 = (unsigned long)InterlockedCompareExchange(&g_addTeamP4, 0, 0);
    return true;
}

// ★ Менеджер отрядов (this у addToTeam). Снят перехватчиком.
unsigned long EVBotsEngineSquadManager() {
    return (unsigned long)InterlockedCompareExchange(&g_squadMgr, 0, 0);
}

// ★ Указатель на НАСТОЯЩЕГО игрока: его кладёт перехватчик spawnSoldier,
// когда посадку делает ДВИЖОК, а не мы (наши вызовы помечены g_ourSpawn).
// Нужен для сравнения: дифференциальный дамп «настоящий игрок vs бот».
// ★ Сборка 111: настоящий игрок-человек — из списка менеджера (0x80..0x84): первый, чьё имя не BOT_ и не Host.
// Раньше брали g_spawnA1 (a1 последнего spawnSoldier, в том числе бота): в прогоне 110 это был не игрок.
unsigned long FindRealHumanPlayer() {
    if (g_managerPtr == nullptr) return 0;
    unsigned char** begin = nullptr;
    unsigned char** end = nullptr;
    if (!Read(g_managerPtr, kOffArray1Begin, &begin) || !Read(g_managerPtr, kOffArray1End, &end)) return 0;
    if (begin == nullptr || end == nullptr || end < begin) return 0;
    const unsigned count = (unsigned)(end - begin);
    for (unsigned i = 0; i < count && i < 64; i++) {
        void* p = (void*)begin[i];
        if (!Readable(p)) continue;
        const char* name = SafeCStr(p, kOffPlayerName);
        if (name == nullptr || name[0] == '\0' || ::strcmp(name, "<unreadable>") == 0) continue;
        if (::strncmp(name, "BOT_", 4) == 0 || ::strcmp(name, "Host") == 0) continue;
        return (unsigned long)(ULONGLONG)p;
    }
    return 0;
}

unsigned long EVBotsEngineRealPlayer() {
    return FindRealHumanPlayer();
}

// ★ Сборка 201: человек по имени — для нескольких людей на сервере (друг 19.10: «боты бьют только хоста»; мост всем людям
// отдавал позицию первого человека из списка). Тот же список игроков движка, что у FindRealHumanPlayer.
unsigned long EVBotsEngineHumanByName(const char* want) {
    if (g_managerPtr == nullptr || want == nullptr || want[0] == '\0') return 0;
    unsigned char** begin = nullptr;
    unsigned char** end = nullptr;
    if (!Read(g_managerPtr, kOffArray1Begin, &begin) || !Read(g_managerPtr, kOffArray1End, &end)) return 0;
    if (begin == nullptr || end == nullptr || end < begin) return 0;
    const unsigned count = (unsigned)(end - begin);
    for (unsigned i = 0; i < count && i < 64; i++) {
        void* p = (void*)begin[i];
        if (!Readable(p)) continue;
        const char* name = SafeCStr(p, kOffPlayerName);
        if (name == nullptr || name[0] == '\0' || ::strncmp(name, "BOT_", 4) == 0) continue;
        if (::strcmp(name, want) == 0) return (unsigned long)(ULONGLONG)p;
    }
    return 0;
}

// ★ Сборка 202: все люди сервера — как в VU (PlayerManager:GetPlayers() содержит каждого человека). Тот же список игроков
// движка и тот же отбор, что у FindRealHumanPlayer (имя не BOT_ и не Host). Друг, 19.10 вечером: двое людей — боты стоят в упор
// и не стреляют во второго, на его выстрелы не отвечают: мост знал только первого человека (RealPlayer).
// ★ Сборка 214: −1 — список игроков движка не прочитан (нет менеджера, указатели массива не читаются или begin > end). Нужно,
// чтобы «список не прочитан» не принять за «все люди ушли» (Player:Left моду теперь уходит сразу).
int EVBotsEngineHumansChecked(unsigned long* out, int cap) {
    if (g_managerPtr == nullptr || out == nullptr || cap <= 0) return -1;
    unsigned char** begin = nullptr;
    unsigned char** end = nullptr;
    if (!Read(g_managerPtr, kOffArray1Begin, &begin) || !Read(g_managerPtr, kOffArray1End, &end)) return -1;
    if (begin == nullptr || end == nullptr || end < begin) return -1;
    const unsigned count = (unsigned)(end - begin);
    int n = 0;
    bool sure = count <= 64;
    for (unsigned i = 0; i < count && i < 64 && n < cap; i++) {
        void* p = (void*)begin[i];
        if (!Readable(p)) { sure = false; continue; }
        const char* name = SafeCStr(p, kOffPlayerName);
        if (name == nullptr || name[0] == '\0' || ::strcmp(name, "<unreadable>") == 0) { sure = false; continue; }
        if (::strncmp(name, "BOT_", 4) == 0 || ::strcmp(name, "Host") == 0) continue;
        out[n++] = (unsigned long)(ULONGLONG)p;
    }
    // Запись без читаемого имени могла быть человеком — тогда список «не уверен» (как не прочитан).
    return sure ? n : -1 - n;
}
int EVBotsEngineHumans(unsigned long* out, int cap) {
    const int r = EVBotsEngineHumansChecked(out, cap);
    return r >= 0 ? r : -1 - r;
}

bool EVBotsEngineIsHuman(unsigned long ptr) {
    if (ptr == 0) return false;
    unsigned long hs[64];
    const int n = EVBotsEngineHumans(hs, 64);
    for (int i = 0; i < n; ++i) {
        if (hs[i] == ptr) return true;
    }
    return false;
}

// ★ Сборка 139: имя настоящего игрока-человека. В список мода попадает сам человек, а не фейковый Host.
bool EVBotsEngineRealPlayerName(char* out, size_t cap) {
    if (out == nullptr || cap == 0) return false;
    out[0] = '\0';
    const unsigned long p = FindRealHumanPlayer();
    if (p == 0) return false;
    const char* name = SafeCStr((void*)(ULONGLONG)p, kOffPlayerName);
    if (name == nullptr || name[0] == '\0') return false;
    ::strncpy(out, name, cap - 1);
    out[cap - 1] = '\0';
    return true;
}

// ★ НАСТОЯЩИЕ аргументы, которыми движок создаёт игрока по сети (12.10.2026).
// Перехватчик createPlayer снял их при реальном входе клиента:
//     createPlayer(this=F2CC0100 a1=00000001 a2=FFFFFFFF a3=FA000000)
//     ^ this = менеджер игроков
//         a1 = слот/идентификатор игрока (у первого = 1)
//         a2 = FFFFFFFF (sentinel)
//         a3 = FA000000  ← МЫ ПЕРЕДАВАЛИ 0x40603000, ЭТО БЫЛО НЕВЕРНО
// Третий аргумент движок заполняет сам, а мы подставляли чужое значение.
void EVBotsEngineCreateRefArgs(long* a1, long* a2, long* a3) {
    if (InterlockedCompareExchange(&g_haveCreate, 0, 0) == 0) return;
    *a1 = (long)InterlockedCompareExchange(&g_createA1, 0, 0);
    *a2 = (long)InterlockedCompareExchange(&g_createA2, 0, 0);
    *a3 = (long)InterlockedCompareExchange(&g_createA3, 0, 0);
}

// ★ Эталон полей игрока В МОМЕНТ СПАВНА (см. комментарий у g_sref318).
// Мост проставляет боту эти значения ДО вызова spawnSoldier.
void EVBotsEngineSpawnRefFields(unsigned long* f318, unsigned long* f31c,
                                unsigned long* f480) {
    if (InterlockedCompareExchange(&g_srefFields, 0, 0) == 0) return;
    *f318 = (unsigned long)InterlockedCompareExchange(&g_sref318, 0, 0);
    *f31c = (unsigned long)InterlockedCompareExchange(&g_sref31c, 0, 0);
    *f480 = (unsigned long)InterlockedCompareExchange(&g_sref480, 0, 0);
}

// ★ Сборка 167: смена уровня — эталон прошлого уровня указывает на освобождённые объекты. Сбрасываем оба эталона:
// заново снимутся с человека (до спавна — EVBotsEngineAdoptHumanRef, на посадке — перехват spawnSoldier).
void EVBotsEngineResetSpawnRef() {
    InterlockedExchange(&g_srefFields, 0);
    InterlockedExchange(&g_sref480, 0);
    InterlockedExchange(&g_refFields, 0);
    InterlockedExchange(&g_ref480, 0);
    LOG_INFO("Bridge: смена уровня — эталоны полей игрока сброшены");
}

// Пара «начать/закончить НАШ вызов spawnSoldier»: нужна, чтобы перехватчик
// отличал спавн бота от спавна настоящего игрока.
void EVBotsEngineOurSpawnBegin() { InterlockedIncrement(&g_ourSpawn); }
void EVBotsEngineOurSpawnEnd() { InterlockedDecrement(&g_ourSpawn); }

// Сообщение, которым движок помечает НАСТОЯЩЕГО игрока. Отдаём его ботам.
void* EVBotsEngineOnCreateMsg() {
    return (void*)(ULONGLONG)InterlockedCompareExchange(&g_onCreateMsg, 0, 0);
}

unsigned long EVBotsEngineSpawnPointBest() {
    const unsigned long real = (unsigned long)g_spawnSelf;
    if (real != 0) return real;
    return FindUsableSpawnPoint(0);
}
// Повтор отложенных спавнов ботов (очередь в VextPlayerManager.cpp).
// Объявление здесь; определение — в VextPlayerManager.cpp.
namespace {

// (Сборка 168: опыт SPM_GUARD — ловушка guard page на массив игроков для поиска createPlayer — удалён: три попытки
// молча не работали (MODLOG), флаг нигде не задавался, а проверка шла на каждом тике update.)

// ═══════════════════════════════════════════════════════════════════════
// Наблюдение за реальными вызовами создания игрока и спавна солдата.
// Сигнатуры взяты из дизассемблирования (НЕ догадки):
//   FUN_00e6f720(this, a1, a2, a3, a4) -> player*   (RET 0x10)
//   FUN_00e644b0(this, a1, a2, a3)      -> player*   (RET 0xC)
//   FUN_00e6ca00(this, player*, name)               (void)
//   FUN_00e48490(this, msg*)                        onCreatePlayerMessage
//   FUN_00ddbbd0(this, a1, a2, a3, a4)              спавн солдата на точке
//   FUN_00dd9000(this, player*)                     завершение спавна
// Логируем значения — они и есть ответ, что нужно передавать для бота.
// ═══════════════════════════════════════════════════════════════════════

constexpr unsigned long kCreatePlayerRva = 0x00A644B0UL; // FUN_00e644b0 (3 арг.)
constexpr unsigned long kAddToArrayRva2 = 0x00A6CA00UL;  // FUN_00e6ca00 (2 арг.)
constexpr unsigned long kOnCreatePlayerRva = 0x00A48490UL; // FUN_00e48490 (1 арг.)
constexpr unsigned long kSoldierSetupRva = 0x009D9000UL; // FUN_00dd9000 (1 арг.)
InlineHook g_hookFindFree;
InlineHook g_hookSpawnEntryGet;
// Trampoline'ы и снятые значения — определения ниже, глобалы нужны здесь,
// в InstallSpawnTraces.
void* g_trampSpawnEntryGet = nullptr;
void* g_trampFindFree = nullptr;
unsigned long g_realFreeMgr = 0;
unsigned long g_realFreeTeam = 0;
unsigned long g_realFreePlayer = 0;
unsigned long g_realFreeIndex = 0;
unsigned long g_realEntryMgr = 0;
unsigned long g_realEntryIndex = 0;
unsigned long g_realEntryEntity = 0;

InlineHook g_hookCreateWrap;
InlineHook g_hookCreatePlayer;
InlineHook g_hookAddToArray;
InlineHook g_hookOnCreatePlayer;
InlineHook g_hookSpawnSoldier;
InlineHook g_hookSoldierSetup;
InlineHook g_hookAddToTeam;
InlineHook g_hookAddToSquad;
void* g_trampAddToSquad = nullptr;
volatile LONG g_humanSquad = -1;  // отряд человека по перехвату addToSquad (-1 = неизвестно)

void* g_trampCreateWrap = nullptr;
void* g_trampCreatePlayer = nullptr;
void* g_trampAddToArray = nullptr;
void* g_trampOnCreatePlayer = nullptr;
void* g_trampSpawnSoldier = nullptr;
PVOID g_veh = nullptr;
void* g_trampSoldierSetup = nullptr;
void* g_trampAddToTeam = nullptr;
// ★ Состояние проб сборки 34 (всё read-only, SEH):
volatile LONG g_lastBotSoldier = 0;  // солдат бота (снят после нашего спавна)
volatile LONG g_soldierDiffDone = 0; // дифф солдат сделан один раз
volatile LONG g_scanDone = 0;        // скан точек запущен один раз
volatile LONG g_scanFinished = 0;    // скан точек закончен (точки собраны)
volatile LONG g_humanTeam = 0;       // команда живого игрока в момент его спавна (1/2)
volatile LONG g_scanFound = 0;       // всего совпадений vtable в куче
volatile LONG g_scanAccepted = 0;    // принято (команда 0..3 + entry валиден)

// Печатает строку, если указатель похож на текст (для имён игроков).
const char* SafeStr(void* p) {
    if (p == nullptr || !Readable(p)) return "<null>";
    const char* s = (const char*)p;
    if (!Readable(s)) return "<unreadable>";
    return s;
}

// ВАЖНО: движок использует __thiscall (this -> ECX, ВСЕ аргументы в стек).
// MSVC не даёт __thiscall на свободных функциях, поэтому используем
// __fastcall с ФИКТИВНЫМ параметром: первый реальный аргумент __fastcall
// уходит в EDX, значит лишний «пустой» параметр сдвигает всё остальное в
// стек, где их и ждёт движок. Без этого стек едет и RET 0xN рвёт кадр —
// сервер падает (наблюдали при выходе игрока).
// Захваченные при РЕАЛЬНОМ входе игрока значения — их будем повторять
// для бота, вместо того чтобы угадывать аргументы.
// (сами переменные объявлены выше, рядом с прочими глобалами файла)

int __fastcall DetourCreateWrap(void* self, void* /*edx*/, void* a1, void* a2,
                                void* a3, void* a4) {
    LOG_INFO("TRACE createWrap(this=%p a1=%u a2=%p a3=%d a4=%d)",
             self, (unsigned)(ULONGLONG)a1, a2, (int)(long)a3, (int)(long)a4);

    // a2 — временный буфер соединения. Он УЖЕ освобождается сразу после
    // входа игрока: через 2 минуты по тому же адресу лежали ДРУГИЕ данные
    // (проверено 14:37:57, падение 0xC0000005). Поэтому копируем его
    // СЕЙЧАС, пока он живой, в свой буфер — его можно изучать и переиспользовать.
    if (Readable(a2)) {
        static unsigned char* keep = new unsigned char[0x200];
        for (int i = 0; i < 0x200; ++i) keep[i] = 0;
        for (int i = 0; i < 0x100; ++i) {
            char c;
            if (!Read(a2, i, &c)) break;
            keep[i] = (unsigned char)c;
        }
        InterlockedExchange((volatile LONG*)&g_wrapA2Copy, (LONG)0);
        g_wrapA2Copy = keep;
        InterlockedExchange(&g_haveWrapCopy, 1);
        LOG_INFO("TRACE createWrap: a2 СКОПИРОВАН в %p (0x100 байт)", keep);

        // Глубокий дамп: по каждому 4-байтовому значению, похожему на
        // указатель, пробуем прочитать строку — так найдём имя игрока.
        for (int off = 0; off < 0x60; off += 4) {
            unsigned v = 0;
            Read(keep, off, &v);
            if (v < 0x00100000u || v > 0x7FFF0000u) continue;
            char text[33];
            bool ok = true;
            text[0] = '\0';
            for (int i = 0; i < 32; ++i) {
                char c;
                if (!Read((void*)(ULONGLONG)v, i, &c)) { ok = false; break; }
                text[i] = c;
                if (c == '\0') break;
            }
            text[32] = '\0';
            if (!ok || text[0] < 0x20 || text[0] > 0x7E) continue;
            LOG_INFO("TRACE   a2[+0x%02X]=%08X -> строка \"%s\"", off, v,
                     text);
        }
    }

    int result = 0;
    if (g_trampCreateWrap != nullptr) {
        using Tramp = int(__fastcall*)(void*, void*, void*, void*, void*,
                                       void*);
        Tramp tramp = (Tramp)(ULONGLONG)g_trampCreateWrap;
        result = tramp(self, nullptr, a1, a2, a3, a4);
    }
    LOG_INFO("TRACE createWrap -> player=%p", (void*)(ULONGLONG)result);
    return result;
}

int __fastcall DetourCreatePlayer(void* self, void* /*edx*/, void* a1,
                                  void* a2, void* a3) {
    // ★ Печатаем в HEX: прежний %u терял -1 (печатал 4294967295) и было
    // невозможно отличить sentinel от счётчика.
    LOG_INFO("TRACE createPlayer(this=%p a1=%08X a2=%08X a3=%08X)", self,
             (unsigned)(ULONGLONG)a1, (unsigned)(ULONGLONG)a2,
             (unsigned)(ULONGLONG)a3);

    // запоминаем «эталонные» аргументы, чтобы повторить их для бота
    InterlockedExchange(&g_createA1, (LONG)(ULONGLONG)a1);
    InterlockedExchange(&g_createA2, (LONG)(ULONGLONG)a2);
    InterlockedExchange(&g_createA3, (LONG)(ULONGLONG)a3);
    InterlockedExchange(&g_haveCreate, 1);

    int result = 0;
    if (g_trampCreatePlayer != nullptr) {
        using Tramp = int(__fastcall*)(void*, void*, void*, void*, void*);
        Tramp tramp = (Tramp)(ULONGLONG)g_trampCreatePlayer;
        result = tramp(self, nullptr, a1, a2, a3);
    }
    LOG_INFO("TRACE createPlayer -> player=%p", (void*)(ULONGLONG)result);
    // ★ Состояние ТОЛЬКО ЧТО созданного движком игрока. Это ровно то, чего
    // не хватает нашим ботам: движок сам наполняет объект, мы — нет.
    if (result != 0) {
        unsigned char* pp = (unsigned char*)(ULONGLONG)result;
        unsigned int v318 = 0, v31c = 0, v480 = 0, v3c4 = 0, v1924 = 0;
        unsigned char b08 = 0;
        __try {
            v318 = *(const unsigned int*)(pp + 0x318);
            v31c = *(const unsigned int*)(pp + 0x31C);
            v480 = *(const unsigned int*)(pp + 0x480);
            v3c4 = *(const unsigned int*)(pp + 0x3C4);
            v1924 = *(const unsigned int*)(pp + 0x1924);
            b08 = pp[0x08];
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
        LOG_INFO("TRACE НОВЫЙ игрок %p: +0x318=%08X +0x31C=%08X +0x480=%08X "
                 "+0x3C4=%08X +0x1924=%08X байт+0x08=%02X",
                 (void*)(ULONGLONG)result, v318, v31c, v480, v3c4, v1924, b08);
        InterlockedExchange(&g_ref1924, (LONG)v1924);
    }
    return result;
}

void __fastcall DetourAddToArray(void* self, void* /*edx*/, void* player,
                                 void* name) {
    LOG_INFO("TRACE addToArray(this=%p player=%p name=\"%s\")", self, player,
             SafeStr(name));
    if (g_trampAddToArray != nullptr) {
        using Tramp = void(__fastcall*)(void*, void*, void*, void*);
        Tramp tramp = (Tramp)(ULONGLONG)g_trampAddToArray;
        tramp(self, nullptr, player, name);
    }
}

// ★ ВНИМАНИЕ, тот же класс ошибки, что с update: FUN_00e48490 это
// ``void __thiscall(int param_1)`` — ОДИН стековый аргумент. Объявляя
// детектор как ``(self, msg)``, мы брали msg из EDX, а нужно из стека.
// Из-за этого дамп показывал мусор (строки "bf::ServerPreference..."),
// а сервер падал. Поэтому здесь 3 параметра: self, edx и стековый msg.
int __fastcall DetourOnCreatePlayer(void* self, void* /*edx*/, void* msg) {
    // ★ Запоминаем сообщение живого игрока. Движок шлёт его ОДИН раз —
    // только живому игроку, ботам нет. Пока мы его не отправили ботам,
    // они не попадают в таб и не считаются полноценными игроками.
    if (msg != nullptr) {
        InterlockedExchange(&g_onCreateMsg, (LONG)(ULONGLONG)msg);
        // ★ ДАМП структуры создания игрока. FUN_00e48490 — это reqPlayer,
        // param_1 = данные создания (имя, команда, сеть). Чужие данные
        // повторять нельзя, надо понять структуру. Только чтение.
        LOG_INFO("TRACE onCreatePlayer: ДАМП msg=%p (0x100 байт)", msg);
        __try {
        for (int off = 0; off < 0x100; off += 16) {
            unsigned int v[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; ++k) {
                unsigned int t = 0;
                if (!Read(msg, off + k * 4, &t)) break;
                v[k] = t;
            }
            LOG_INFO("TRACE   msg[+0x%03X] = %08X %08X %08X %08X", off,
                     v[0], v[1], v[2], v[3]);
        }
        // ВНИМАНИЕ: перебор указателей внутри дампа УБРАН. Он читал по
        // произвольным значениям, найденным в структуре, и ронял сервер.
        // Только байты самой структуры — их читать безопасно.
        // ★ Но поле +0x38 разобрано как указатель на ИМЯ — читаем строку
        // только по нему (проверено: безопасно, это и есть имя игрока).
        {
            unsigned int pName = 0;
            if (Read(msg, 0x38, &pName) && pName != 0 &&
                pName > 0x00100000u && pName < 0x7FFF0000u) {
                char nm[33];
                ::memset(nm, 0, sizeof(nm));
                bool good = true;
                for (int i = 0; i < 32; ++i) {
                    char c = 0;
                    if (!Read((void*)(ULONGLONG)pName, i, &c)) {
                        good = false;
                        break;
                    }
                    if (c == '\0') break;
                    if (c < 0x20 || c > 0x7E) { good = false; break; }
                    nm[i] = c;
                }
                if (good) {
                    LOG_INFO("TRACE msg+0x38 -> ИМЯ \"%s\" (указатель %08X)",
                             nm, pName);
                } else {
                    LOG_INFO("TRACE msg+0x38 -> указатель %08X (не строка)",
                             pName);
                }
            }
        }
        LOG_INFO("TRACE дамп msg завершён");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            LOG_WARN("TRACE дамп msg прерван исключением (безопасно)");
        }
    }
    LOG_INFO("TRACE onCreatePlayerMessage(this=%p msg=%p)", self, msg);
    if (g_trampOnCreatePlayer != nullptr) {
        // ★ Три аргумента: self, edx и стековый msg. С двумя стек сдвигается
        // и оригинал читает msg не оттуда -> падение внутри FUN_00e48490.
        using Tramp = int(__fastcall*)(void*, void*, void*);
        Tramp tramp = (Tramp)(ULONGLONG)g_trampOnCreatePlayer;
        return tramp(self, nullptr, msg);
    }
    return 0;
}

// ★ addToTeam — ServerPlayerSquadManager. Найдена поиском по строке лога
// «addToTeam(»: PUSH на 0x0118F000 внутри функции с 0x0118EF80
// (trash\2026-10-12\find_squad.py, re\squad_decomp.txt).
//
// Зачем: в логе «Player count for Team2: 7» при 16 игроках на сервере —
// ботов в менеджере отрядов НЕТ. Отсюда нет таба, нет очков, нет респавна.
constexpr unsigned long kAddToTeamRva = 0x00D8EF80UL;
// addToSquad (VA 0x0118D8D0), cdecl(игрок, отряд, fA, fB), RET без операнда (проверено дизассемблером 13.10).
constexpr unsigned long kAddToSquadRva = 0x00D8D8D0UL;

// ★ addToTeam — ServerPlayerSquadManager. Найдена поиском по строке лога
// «addToTeam(»: PUSH на 0x0118F000 внутри функции с 0x0118EF80
// (trash\2026-10-12\find_squad.py, re\squad_decomp.txt).
//
// Зачем: в логе «Player count for Team2: 7» при 16 игроках на сервере —
// ботов в менеджере отрядов НЕТ. Отсюда нет таба, нет очков, нет респавна.
// ★★★ РАЗБОР СИГНАТУРЫ (13.10.2026, три краша подряд)
//
// Что показал лог перехватчика:
//
//     addToTeam(ECX=04E1F100 игрок=F2DDD020 команда=1 отряд=1 [стек+3]=04E1F100)
//
// Значение в ECX МЕНЯЕТСЯ между прогонами (04E5F100 -> 04E2F100 ->
// 04E1F100) — значит это НЕ `this`, а мусор: вызывающий код не грузил ECX.
// Вывод: функция СТАТИЧЕСКАЯ, все четыре параметра лежат в стеке:
//
//     f(игрок, команда, отряд, менеджер)
//
// Отсюда две ошибки, которые я делал:
//   * объявить 3 аргумента -> трамплин делает ret 0x0C вместо ret 0x10;
//   * объявить 4 без двух «пустышек» -> первый настоящий аргумент уезжает
//     в EDX и все остальные сдвигаются.
//
// Правильно: два фиктивных параметра (под ECX и EDX) + четыре настоящих
// стековых. Тогда стек совпадает, и трампoline возвращает 0x10.
// ★ addToSquad (13.10): read-only. Запоминает номер отряда, в который вошёл ЧЕЛОВЕК (joinSquad при смене
// команды и при создании отряда). Мост ставит ботов в этот отряд. Оригинал вызывается дальше. Флаг EVBOTS_SQUAD_HOOK.
unsigned char __cdecl DetourAddToSquad(void* player, unsigned int squad, unsigned int fa, unsigned int fb) {
    if (player != nullptr && squad <= 32 &&
        (unsigned long)(ULONGLONG)player == EVBotsEngineRealPlayer()) {
        InterlockedExchange(&g_humanSquad, (LONG)squad);
        LOG_INFO("TRACE addToSquad(человек, отряд %u, %u, %u) — запомнен", squad, fa, fb);
    }
    if (g_trampAddToSquad == nullptr) return 0;
    using Tramp = unsigned char(__cdecl*)(void*, unsigned int, unsigned int, unsigned int);
    return ((Tramp)g_trampAddToSquad)(player, squad, fa, fb);
}

int __fastcall DetourAddToTeam(void* ecx, void* edx, void* a1,
                              void* a2, void* a3, void* a4) {
    if (InterlockedCompareExchange(&g_addTeamSeen, 0, 0) == 0) {
        InterlockedExchange(&g_addTeamP2, (LONG)(ULONGLONG)a1);
        InterlockedExchange(&g_addTeamP3, (LONG)(ULONGLONG)a2);
        InterlockedExchange(&g_addTeamP4, (LONG)(ULONGLONG)a3);
        // менеджер отрядов — последний аргумент
        InterlockedExchange(&g_squadMgr, (LONG)(ULONGLONG)a4);
        InterlockedExchange(&g_addTeamSeen, 1);
        LOG_INFO("TRACE addToTeam(игрок=%08X команда=%08X отряд=%08X "
                 "менеджер=%08X)", a1, a2, a3, a4);
    }
    if (g_trampAddToTeam != nullptr) {
        using Tramp = int(__fastcall*)(void*, void*, void*, void*, void*,
                                       void*);
        Tramp tramp = (Tramp)(ULONGLONG)g_trampAddToTeam;
        // ECX/EDX передаём КАК ЕСТЬ. Подставлять nullptr было ошибкой:
        // оригинал может их читать (13.10.2026 — ACCESS_VIOLATION).
        return tramp(ecx, edx, a1, a2, a3, a4);
    }
    return 0;
}

int __fastcall DetourSpawnSoldier(void* self, void* /*edx*/, void* a1,
                                  void* a2, void* a3, void* a4) {
    // Счётчик вложенности. Когда мы сами вызываем spawnSoldier для бота,
    // перехватчик сработает ещё раз — важно не принять это за новый
    // «реальный» вызов и не вызвать спавн снова изнутри себя.
    const bool nested = InterlockedIncrement(&g_spawnDepth) > 1;
    // Наш собственный вызов для бота — эталон не перезаписываем.
    // Сборка 201: и бот моста, которого движок посадил сам (событие Spawn точки, как VU).
    const bool ours =
        InterlockedCompareExchange(&g_ourSpawn, 0, 0) != 0 || nested ||
        ::evbots::EVBotsIsBridgeBot((unsigned long)(ULONG_PTR)a1);

    if (!ours) {
        // Один раз: поля точки живого игрока — сравнить с точками скана (DumpPointWords).
        static volatile LONG g_dumpedLive = 0;
        if (self != nullptr && InterlockedCompareExchange(&g_dumpedLive, 1, 0) == 0) {
            DumpPointWords("LIVE", (unsigned long)(ULONGLONG)self, 0x40);
        }
        LOG_INFO("TRACE spawnSoldier(this=%p a1=%p a2=%p a3=%p a4=%p)", self,
                 a1, a2, a3, a4);
        // this у spawnSoldier — отдельный объект, запоминаем его
        InterlockedExchange(&g_spawnSelf, (LONG)(ULONGLONG)self);
        InterlockedExchange(&g_spawnA1, (LONG)(ULONGLONG)a1);
        // ★ Скан точек спавна в куче (сборка 34): уровень загружен, сущности
        // есть. Один раз за прогон, в фоне.
        if (InterlockedCompareExchange(&g_scanDone, 1, 0) == 0) {
            HANDLE h =
                ::CreateThread(nullptr, 0, ScanSpawnThread, nullptr, 0, nullptr);
            if (h != nullptr) ::CloseHandle(h);
        }
        // ★ Читаем команду точки спавна (CharacterSpawnEntity+0xD8;
        // FUN_00dd8f90 сравнивает [lastPoint+0xD8] с [player+0x31C]).
        // 0 = общая точка, тоже запоминаем (37-я: раньше `if (spTeam != 0)`
        // отбрасывал все общие точки, а у всех 8 реальных team=0).
        // Это позволяет собирать точки ПО КОМАНДЕ и выдавать ботам
        // правильную точку (не сливать обе команды в одну).
        unsigned int spTeam = 0;
        __try {
            spTeam = *(const unsigned int*)((const unsigned char*)(ULONGLONG)self + 0xD8);
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
        {
            // Команда точки — из ref (+0x128), а не из +0xD8 (там всегда 0).
            const unsigned int rt = PointRefTeam((unsigned long)(ULONGLONG)self);
            const unsigned int pt = (rt <= 3) ? rt : spTeam;
            if (pt <= 3) RememberSpawnPoint((unsigned long)self, pt);
            // Команда живого игрока в момент спавна (его HQ = эта точка).
            unsigned int humanT = 0;
            Read(a1, 0x31C, &humanT);
            if (humanT == 1 || humanT == 2) {
                InterlockedExchange(&g_humanTeam, (LONG)humanT);
            }
            LOG_INFO("TRACE живая точка %p refTeam=%u команда игрока=%u", self,
                     rt, humanT);
        }
        // ТЕ ЖЕ поля у ЖИВОГО игрока — для сравнения с ботом.
        {
            const unsigned char* pp = (const unsigned char*)(ULONGLONG)a1;
            unsigned int v480 = 0, v318 = 0, v31c = 0, v3c4 = 0;
            __try {
                v480 = *(const unsigned int*)(pp + 0x480);
                v318 = *(const unsigned int*)(pp + 0x318);
                v31c = *(const unsigned int*)(pp + 0x31C);
                v3c4 = *(const unsigned int*)(pp + 0x3C4);
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
            LOG_INFO("TRACE живой игрок: +0x318=%08X +0x31C=%08X +0x480=%08X "
                     "+0x3C4(солдат)=%08X", v318, v31c, v480, v3c4);
            // ★ ЭТАЛОН В МОМЕНТ СПАВНА — вот эти значения и есть те, которых
            // не хватает боту. Именно здесь движок сажает игрока.
            // ★ Сборка 167: обновляем на КАЖДОЙ посадке человека (раньше — только первой). Прогон 166: после
            // перезагрузки уровня боту ставился +0x480 прошлого уровня (F580BDC8, освобождён) → исключение в
            // spawnSoldier, addref по мёртвому объекту, падение сервера на следующей смене раунда.
            if (v480 != 0 && (InterlockedCompareExchange(&g_srefFields, 0, 0) == 0 ||
                              (unsigned int)InterlockedCompareExchange(&g_sref480, 0, 0) != v480)) {
                InterlockedExchange(&g_sref318, (LONG)v318);
                InterlockedExchange(&g_sref31c, (LONG)v31c);
                InterlockedExchange(&g_sref480, (LONG)v480);
                InterlockedExchange(&g_srefFields, 1);
                LOG_INFO("TRACE ЭТАЛОН СПАВНА сохранён: +0x318=%08X +0x31C=%08X "
                         "+0x480=%08X", v318, v31c, v480);
            }
        }
    } else {
        LOG_INFO("TRACE spawnSoldier(вложенный, бот) a1=%p", a1);
    }

    int result = 0;
    if (g_trampSpawnSoldier != nullptr) {
        using Tramp = int(__fastcall*)(void*, void*, void*, void*, void*,
                                       void*);
        Tramp tramp = (Tramp)(ULONGLONG)g_trampSpawnSoldier;
        result = tramp(self, nullptr, a1, a2, a3, a4);
    }

    if (!nested && result != 0) {
        // Реальный спавн состоялся — теперь можно попробовать бота.
        InterlockedExchange(&g_pendingBot, 1);
    }

    if (ours && result != 0) {
        // ★ Наш спавн бота состоялся: запоминаем его солдата для диффа
        // (сборка 34) и пробуем снять дифф, если человек уже известен.
        unsigned long bs = 0;
        if (Read(a1, 0x3C4, &bs) && bs != 0) {
            InterlockedExchange(&g_lastBotSoldier, (LONG)bs);
        }
        TrySoldierDiff();
    }

    InterlockedDecrement(&g_spawnDepth);
    return result;
}

int __fastcall DetourSoldierSetup(void* self, void* player) {
    LOG_INFO("TRACE soldierSetup(this=%p player=%p)", self, player);
    if (g_trampSoldierSetup != nullptr) {
        using Tramp = int(__fastcall*)(void*, void*);
        Tramp tramp = (Tramp)(ULONGLONG)g_trampSoldierSetup;
        return tramp(self, player);
    }
    return 0;
}

// ★ Read-only хук applyCustomization (сборка 34): снимает настоящие аргументы
// кастомизации у ЖИВОГО игрока (this=ServerPlayer, a1=customization,
// a2=addToPool). Оригинал ВСЕГДА вызывается дальше (цепочка не рвётся).
// ★ Сборка 106, read-only: ServerSoldierEntity::prePhysicsUpdateEntry 0x00E0A7C0 (thiscall, 2 стековых, RET 8:
// a1 — номер входа, a2 — ввод). Цель: подтвердить, что движок применяет к солдату бота ввод с «вперёд» != 0
// (опыт BotOwnInput). Пишем вызовы с ненулевым EIAThrottle (+0x10 ввода) и общий счётчик. Флаг EVBOTS_SPM_PREPHYS.
constexpr unsigned long kPrePhysEntryRva = 0x00A0A7C0UL;  // VA 0x00E0A7C0
InlineHook g_hookPrePhysEntry;
void* g_trampPrePhysEntry = nullptr;
volatile LONG g_prePhysCalls = 0;
volatile LONG g_prePhysLogged = 0;

// Сборка 107: сводка по каждому солдату — вызовы, вызовы с «вперёд», смещение позиции (трансформ +0x1C → +0x30),
// vtable компонента +0x538 (ему уходит ввод) и признак ИИ игрока (солдат +0x144 → игрок +0x318). Цель: понять,
// считает ли сервер движение сам (у человека «вперёд» в 106 не встретился ни разу — возможно, позиция от клиента).
struct PrePhysStat { void* soldier; LONG calls; LONG thCalls; float px, pz; bool havePrev; double pathTh, pathAll;
                     unsigned long vt538, player, ai; };
PrePhysStat g_prePhysStat[24];
LONG g_prePhysStatCount = 0;
volatile LONG g_prePhysSummaries = 0;

static bool SoldierXZ(const unsigned char* s, float* x, float* z) {
    __try {
        const unsigned long trp = *(const unsigned long*)(s + 0x1C);
        if (trp == 0) return false;
        const unsigned char* tr = (const unsigned char*)(ULONGLONG)trp;
        if (((*(const unsigned long*)(s + 8) >> 18) & 1u) != 0) tr = tr + 0x10 + (unsigned long)tr[6] * 0x50;
        *x = *(const float*)(tr + 0x30);
        *z = *(const float*)(tr + 0x38);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall DetourPrePhysEntry(void* self, void* /*edx*/, void* entry, void* input) {
    const LONG c = InterlockedIncrement(&g_prePhysCalls);
    float th = 0.0f;
    __try {
        if (input != nullptr) th = *(const float*)((const unsigned char*)input + 0x10);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        th = 0.0f;
    }
    const bool moving = (th > 0.01f || th < -0.01f);
    if (moving && InterlockedIncrement(&g_prePhysLogged) <= 12) {
        LOG_INFO("TRACE prePhysEntry: солдат=%p вход=%p ввод=%p вперёд=%.2f (вызовов всего %ld)", self, entry, input,
                 (double)th, c);
    }
    PrePhysStat* st = nullptr;
    for (LONG i = 0; i < g_prePhysStatCount; ++i) {
        if (g_prePhysStat[i].soldier == self) { st = &g_prePhysStat[i]; break; }
    }
    float x = 0.0f, z = 0.0f;
    const bool havePos = self != nullptr && SoldierXZ((const unsigned char*)self, &x, &z);
    if (st == nullptr && g_prePhysStatCount < 24 && self != nullptr) {
        st = &g_prePhysStat[g_prePhysStatCount++];
        *st = {};
        st->soldier = self;
        __try {
            const unsigned char* so = (const unsigned char*)self;
            const unsigned long comp = *(const unsigned long*)(so + 0x538);
            st->vt538 = comp != 0 ? *(const unsigned long*)(ULONGLONG)comp : 0;
            st->player = *(const unsigned long*)(so + 0x144);
            st->ai = st->player != 0 ? *(const unsigned char*)(ULONGLONG)(st->player + 0x318) : 0xFF;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (st != nullptr) {
        ++st->calls;
        if (moving) ++st->thCalls;
        if (havePos) {
            if (st->havePrev) {
                const double dx = (double)x - st->px, dz = (double)z - st->pz;
                const double d = sqrt(dx * dx + dz * dz);
                if (d < 5.0) {  // больше — респаун/телепорт, не ходьба
                    st->pathAll += d;
                    if (moving) st->pathTh += d;
                }
            }
            st->px = x;
            st->pz = z;
            st->havePrev = true;
        }
    }
    if (c % 30000 == 0 && InterlockedIncrement(&g_prePhysSummaries) <= 8) {
        LOG_INFO("TRACE prePhysEntry: вызовов всего %ld, солдат %ld", c, g_prePhysStatCount);
        for (LONG i = 0; i < g_prePhysStatCount; ++i) {
            const PrePhysStat& t = g_prePhysStat[i];
            LOG_INFO("TRACE prePhys солдат=%p игрок=%08lX ИИ=%lu вызовов=%ld с газом=%ld путь с газом=%.1f м всего=%.1f м "
                     "vt538=%08lX", t.soldier, t.player, t.ai, t.calls, t.thCalls, t.pathTh, t.pathAll, t.vt538);
        }
    }
    if (g_trampPrePhysEntry != nullptr) {
        using Tramp = void(__fastcall*)(void*, void*, void*, void*);
        ((Tramp)(ULONGLONG)g_trampPrePhysEntry)(self, nullptr, entry, input);
    }
}

// ★ PointSystem: регистрация игрока в системе очков (thiscall, 1 стековый аргумент).
// Без записи PlayerPointsInfo смерть бота даёт «No player info found for victim» и
// клиент показывает человеку «Самоубийство». Перехватчик только запоминает this.
InlineHook g_hookPointsReg;
void* g_trampPointsReg = nullptr;
volatile LONG g_pointSystem = 0;

void __fastcall DetourPointsReg(void* self, void* /*edx*/, void* player) {
    if (InterlockedCompareExchange(&g_pointSystem, (LONG)(ULONGLONG)self, 0) == 0) {
        LOG_INFO("TRACE PointSystem::registerPlayer this=%p player=%p (запомнен)", self, player);
    }
    if (g_trampPointsReg != nullptr) {
        using Tramp = void(__fastcall*)(void*, void*, void*);
        ((Tramp)(ULONGLONG)g_trampPointsReg)(self, nullptr, player);
    }
}

// ★ Сборка 171, read-only: setTeam игрока (0x00E207A0, thiscall, 1 стековый, RET 4; пролог 53 56 8B F1 8B 9E 1C 03 00 00 —
// без относительных адресов). Кто и когда даёт команду человеку: onNewRound только меняет уже назначенные команды
// (re\newround_decomp.txt), первичное назначение — один из 11 вызовов setTeam. Задача «~22 с до ботов». Лог — первые 60.
// EVBOTS_TRACE_SETTEAM=0 — не ставить.
constexpr unsigned long kSetTeamRva = 0x00A207A0UL;
InlineHook g_hookSetTeam;
void* g_trampSetTeam = nullptr;

void __fastcall DetourSetTeam(void* self, void* /*edx*/, int team) {
    static volatile LONG s_logged = 0;
    if (InterlockedIncrement(&s_logged) <= 60) {
        void* from = _ReturnAddress();
        int was = -1;
        unsigned ai = 0xFF, online = 0;
        __try {
            was = *(const int*)((const unsigned char*)self + 0x31C);
            ai = *(const unsigned char*)((const unsigned char*)self + 0x318);
            online = *(const unsigned*)((const unsigned char*)self + 0x20);
        } __except (EXCEPTION_EXECUTE_HANDLER) { }
        LOG_INFO("TRACE setTeam игрок=%p (ИИ=%u onlineId=%08X): команда %d -> %d, вызов из %p", self, ai, online, was, team, from);
    }
    if (g_trampSetTeam != nullptr) {
        using Tramp = void(__fastcall*)(void*, void*, int);
        ((Tramp)(ULONGLONG)g_trampSetTeam)(self, nullptr, team);
    }
}

// ★ Скан кучи на точки спавна БЕЗ хуков (сборка 34, идея внешнего ревью п.3):
// ищем DWORD == vtable CharacterSpawnEntity по committed RW-страницам.
// Карта полей ПО ДЕКОМПИЛЯЦИИ (re\spawn_gates.txt стр.65-67, re\spawn_check.txt):
//   team     = [obj+0xD8] (FUN_00dd8f90 сравнивает [lastPoint+0xD8] с
//              [player+0x31C]; lastPoint от FUN_00e3e1d0 — та же
//              CharacterSpawnEntity, то же смещение. 0 = общая, легитимно).
//   ref      = [obj+0x14] (CharacterSpawnReferenceObjectData*),
//   type     = [ref+0x180] (0 чел, 1 AI, 2 соло — ветки FUN_00ddbbd0),
//   entryIdx = [ref+0x184] (ИНДЕКС входа для FUN_00dcfc20, НЕ указатель,
//              0 легитимен).
// ★ 37-я: 36-я резала все 8 реальных точек проверкой entry=[obj+0x19C]!=0
// (у реальных точек +0x19C = 0).
// ★ 38-я (по прогону 37): ref=[obj+0x14] у реальных точек — мусор
// (00000205, 00000000), type не читается (SEH оставил 0xFFFFFFFF).
// Значит +0x14 — НЕ ref-указатель в рантайме, либо ref лежит глубже
// (цепочка указателей). Проверяем тип ТОЛЬКО если ref читаем как
// указатель (ref >= 0x10000 и страница ref читаема); иначе судим
// только по team 0..3. entryIdx не фильтруем вообще.
// Всё под SEH, в фоновом потоке (без хита по тику), один раз за прогон.
DWORD WINAPI ScanSpawnThread(LPVOID /*unused*/) {
    SYSTEM_INFO si;
    ::GetSystemInfo(&si);
    uintptr_t p = (uintptr_t)si.lpMinimumApplicationAddress;
    const uintptr_t hi = (uintptr_t)si.lpMaximumApplicationAddress;
    LONG found = 0, accepted = 0;
    int logged = 0;
    while (p < hi) {
        MEMORY_BASIC_INFORMATION mbi;
        if (::VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) == 0) break;
        const uintptr_t next =
            (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        const DWORD prot = mbi.Protect;
        // ★ 143: только MEM_PRIVATE (куча движка). Образы exe/DLL содержат
        // константу vtable как операнд кода: ложные «точки» 00DDE2A8 (код exe,
        // +0x14 = CC CC CC CC) и адрес в нашей DLL в КАЖДОМ запуске. Чтение
        // [0xCCCCCCCC+0x180] давало VEH #1 сразу после входа человека.
        const bool readable =
            (mbi.State == MEM_COMMIT) && (mbi.Type == MEM_PRIVATE) &&
            !(prot & (PAGE_NOACCESS | PAGE_GUARD)) &&
            (prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
                     PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE |
                     PAGE_EXECUTE_WRITECOPY));
        if (readable) {
            __try {
                const unsigned long* base =
                    (const unsigned long*)mbi.BaseAddress;
                const size_t n = mbi.RegionSize / 4;
                for (size_t i = 0; i < n; ++i) {
                    if (base[i] != kSpawnEntityVtable) continue;
                    ++found;
                    // ★ 13.10 (прогон 42, диагностика POINT): найденный адрес — это
                    // подобъект CharacterSpawnEntity (его vtable), а полный объект
                    // (он же this у spawnSoldier) лежит на kCharSpawnSubobjOffset ниже.
                    // Живая точка F691EC80 имеет 025BBB70 на +0x80 = F691ED00 (в списке
                    // скана). Поэтому team/ref/type читаем от полного объекта, иначе
                    // поля сдвинуты и отсюда битые ref и team=0 у всех точек.
                    const unsigned char* obj =
                        (const unsigned char*)&base[i] - kCharSpawnSubobjOffset;
                    unsigned int team = 0xFFFFFFFF;
                    unsigned long ref = 0;
                    unsigned int type = 0xFFFFFFFF;
                    unsigned int entryIdx = 0xFFFFFFFF;
                    // Через Read (проба): сбой чтения — ожидаемый, не для VEH.
                    if (!Read(obj, 0xD8, &team) || !Read(obj, 0x14, &ref)) continue;
                    if (ref != 0 && Read((const void*)ref, 0x180, &type)) {
                        Read((const void*)ref, 0x184, &entryIdx);
                    }
                    // ★ 35-я: лог ПРИЧИН отказа — 34-я дала hits=10/accepted=0,
                    // и без значений team/entry непонятно, что фильтр режет.
                    // ★ 37-я: принимаем точки с team 0..3 (0 = общая,
                    // легитимно) и type 0..2 (0 чел, 1 AI, 2 соло).
                    // entryIdx=[ref+0x184] — индекс, 0 легитимен, не фильтруем.
                    // ★ 38-я: type проверяем ТОЛЬКО если ref реально прочитался
                    // (иначе SEH оставил 0xFFFFFFFF и режет реальные точки).
                    // ref считается валидным, если похож на указатель и чтение
                    // по нему не упало (type/entryIdx изменились с 0xFFFFFFFF).
                    const bool typeRead = (type != 0xFFFFFFFF);
                    if (logged < 24) {
                        LOG_INFO("SCAN hit: obj=%p team=%u ref=%p type=%u entryIdx=%u%s",
                                 (const void*)obj, team, (const void*)ref, type,
                                 entryIdx,
                                 (team > 3 || (typeRead && type > 2)) ? " (reject)" : "");
                        ++logged;
                    }
                    if (team > 3) continue;
                    if (typeRead && type > 2) continue;
                    {
                        const unsigned int rt = PointRefTeam((unsigned long)obj);
                        RememberSpawnPoint((unsigned long)obj, rt <= 3 ? rt : team);
                        LOG_INFO("SCAN point %p refTeam=%u", (const void*)obj, rt);
                    }
                    DumpPointWords("SCAN", (unsigned long)obj, 0x40);
                    // Данные ссылки (CharacterSpawnReferenceObjectData): у них
                    // static Team (HQ: Team1/Team2 + LockedTeam; флаги: динамика).
                    // Дамп один раз на каждый различный ref — ищем поле команды.
                    {
                        static unsigned long seenRefs[16];
                        static int nSeen = 0;
                        bool seen = false;
                        for (int s = 0; s < nSeen; ++s) {
                            if (seenRefs[s] == ref) seen = true;
                        }
                        if (!seen && ref != 0 && nSeen < 16) {
                            seenRefs[nSeen++] = ref;
                            DumpPointWords("REF", ref, 0x70);
                        }
                    }
                    ++accepted;
                    if (accepted >= 64) break;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
        if (next <= p) break;
        p = next;
        if (accepted >= 64) break;
    }
    InterlockedExchange(&g_scanFound, found);
    InterlockedExchange(&g_scanAccepted, accepted);
    LOG_INFO("SCAN spawn done: hits=%ld accepted=%ld (vtable=%08X)", found,
             accepted, kSpawnEntityVtable);
    // Очередь ботов ждёт именно этого флага (см. EVBotsEngineSpawnScanDone).
    // Пустой скан (уровень ещё не создал сущности) — не «закончен»: разрешаем
    // повтор (скан теперь запускается из очереди ботов, не только по живому спавну).
    if (accepted > 0) {
        InterlockedExchange(&g_scanFinished, 1);
    } else {
        InterlockedExchange(&g_scanDone, 0);
    }
    return 0;
}

// ★ Дифф солдат человек/бот (сборка 34, read-only): первые 0x400 байт,
// в лог только различающиеся DWORD (не более 40). Один раз за прогон.
void TrySoldierDiff() {
    if (InterlockedCompareExchange(&g_soldierDiffDone, 0, 0) != 0) return;
    const unsigned long bot =
        (unsigned long)InterlockedCompareExchange(&g_lastBotSoldier, 0, 0);
    const unsigned long humanPlayer =
        (unsigned long)InterlockedCompareExchange(&g_spawnA1, 0, 0);
    if (bot == 0 || humanPlayer == 0) return;
    unsigned long human = 0;
    if (!Read((const void*)humanPlayer, 0x3C4, &human) || human == 0 ||
        !Readable((const void*)human)) {
        return;
    }
    if (!Readable((const void*)bot)) return;
    int shown = 0, total = 0;
    for (int off = 0; off < 0x400; off += 4) {
        unsigned long a = 0, b = 0;
        if (!Read((const void*)human, off, &a)) break;
        if (!Read((const void*)bot, off, &b)) break;
        if (a != b) {
            ++total;
            if (shown < 40) {
                LOG_INFO("SOLDIFF +0x%03X human=%08X bot=%08X", off, a, b);
                ++shown;
            }
        }
    }
    LOG_INFO("SOLDIFF done: differing dwords=%d (human=%p bot=%p)", total,
             (const void*)human, (const void*)bot);
    InterlockedExchange(&g_soldierDiffDone, 1);
}

bool InstallSpawnTraces(void* moduleBase) {
    const ULONGLONG base = (ULONGLONG)moduleBase;

    struct H {
        InlineHook* hook;
        void** tramp;
        unsigned long rva;
        void* detour;
        const char* name;
        int args;  // число аргументов (0 = this-only, -1 = void)
        bool isVoid;
        // Имя переменной окружения для ТОЧЕЧНОГО включения хука.
        // nullptr = хук ставится только при общем EVBOTS_SPM_PROBE=1.
        // Задано    = хук ставится ТОЛЬКО если эта переменная > 0,
        //            независимо от SPM_PROBE.
        // Нужно, чтобы получить указатель менеджера игроков ОДНИМ хуком,
        // не ставя остальные: полный набор перехватчиков роняет сервер
        // при создании серверных сущностей уровня (проверено 11.10.2026).
        const char* flag;
    };

    const H hooks[] = {
        // ★ 12.10.2026: каждый из этих хуков ставится ТОЛЬКО по своему флагу.
        // Полный набор (PROBE=1) роняет сервер при загрузке уровня:
        //   Entity.cpp(38) Assert realm -> ACCESS_VIOLATION, ServerLoad.
        // То есть один из них ломает создание СЕРВЕРНЫХ сущностей. Включаем
        // по одному и смотрим, какой именно безопасен.
        {&g_hookCreateWrap, &g_trampCreateWrap, kCreateWrapRva,
         (void*)(ULONGLONG)&DetourCreateWrap, "createWrap", 4, false,
         "EVBOTS_SPM_WRAP"},
        {&g_hookCreatePlayer, &g_trampCreatePlayer, kCreatePlayerRva,
         (void*)(ULONGLONG)&DetourCreatePlayer, "createPlayer", 3, false,
         "EVBOTS_SPM_CREATE"},
        {&g_hookAddToArray, &g_trampAddToArray, kAddToArrayRva2,
         (void*)(ULONGLONG)&DetourAddToArray, "addToArray", 2, true,
         "EVBOTS_SPM_ADD"},
        // Диагностика: движок шлёт это сообщение, когда считает игрока полноценным.
        // Если для живого игрока вызывается, а для ботов нет — вот и причина
        // пустого таба. Включается точечно: EVBOTS_SPM_ONCREATE=1.
        {&g_hookOnCreatePlayer, &g_trampOnCreatePlayer, kOnCreatePlayerRva,
         (void*)(ULONGLONG)&DetourOnCreatePlayer, "onCreatePlayerMessage", 3,
         false, "EVBOTS_SPM_ONCREATE"},
        // ★ Единственный безопасный способ узнать точку спавна БЕЗ живого игрока:
        // этот хук кладёт в g_spawnSelf точку, на которую движок посадил
        // игрока. Списки точек из конструкторов недоступны (они ломают
        // загрузку уровня). Включается точечно: EVBOTS_SPM_SOLDIER=1.
        {&g_hookSpawnSoldier, &g_trampSpawnSoldier, kSpawnSoldierRva,
         (void*)(ULONGLONG)&DetourSpawnSoldier, "spawnSoldier", 4, false,
         "EVBOTS_SPM_SOLDIER"},
        {&g_hookSoldierSetup, &g_trampSoldierSetup, kSoldierSetupRva,
         (void*)(ULONGLONG)&DetourSoldierSetup, "soldierSetup", 1, false,
         "EVBOTS_SPM_SETUP"},
        // ★ addToTeam: снимает настоящие аргументы регистрации в команде.
        // Без неё ботов нет в ServerPlayerSquadManager (нет таба и очков).
        // Включается точечно: EVBOTS_SPM_SQUAD=1.
        {&g_hookAddToTeam, &g_trampAddToTeam, kAddToTeamRva,
         (void*)(ULONGLONG)&DetourAddToTeam, "addToTeam", 4, false,
         "EVBOTS_SPM_SQUAD"},
        // ★ addToSquad: отряд человека (для ботов). Read-only, флаг EVBOTS_SQUAD_HOOK.
        {&g_hookAddToSquad, &g_trampAddToSquad, kAddToSquadRva,
         (void*)(ULONGLONG)&DetourAddToSquad, "addToSquad", 4, false,
         "EVBOTS_SQUAD_HOOK"},
        // (Сборка 169: read-only хук applyCustomization — только лог первых 40 вызовов — удалён; флаг EVBOTS_SPM_CUSTOM
        // по-прежнему включает SpmProbe, см. ModdingInterface.)
        // ★ PointSystem: регистрация PlayerPointsInfo (0x131F010, thiscall, 1 стековый,
        // RET 4). Вызывается движком на ServerPlayerCreateMessage для человека.
        // Read-only: запоминаем this (PointSystem), чтобы мост зарегистрировал ботов.
        {&g_hookPointsReg, &g_trampPointsReg, kPointsRegRva,
         (void*)(ULONGLONG)&DetourPointsReg, "pointsRegister", 1, true,
         "EVBOTS_PTS_REG"},
        // ★ Сборка 171, read-only: кто назначает команду человеку (см. DetourSetTeam). По умолчанию ВКЛ.
        {&g_hookSetTeam, &g_trampSetTeam, kSetTeamRva,
         (void*)(ULONGLONG)&DetourSetTeam, "setTeam", 1, true,
         "EVBOTS_TRACE_SETTEAM"},
        // ★ Сборка 106, read-only: применение ввода к солдату (prePhysicsUpdateEntry). Флаг EVBOTS_SPM_PREPHYS=1.
        {&g_hookPrePhysEntry, &g_trampPrePhysEntry, kPrePhysEntryRva,
         (void*)(ULONGLONG)&DetourPrePhysEntry, "prePhysEntry", 2, false,
         "EVBOTS_SPM_PREPHYS"},
        // Вспомогательные: снимаем их this/аргументы на живом спавне,
        // потому что угадать не вышло (FUN_00e3e1d0 вернула мусор).
        // (Сборка 169: пассивный TRACE-хук spawnMgrGet удалён — g_managerPtr ставит хук update; флаг EVBOTS_SPM_MGR
        // по-прежнему включает сам SpmProbe, см. ModdingInterface.)
        {&g_hookFindFree, &g_trampFindFree, kFindFreeEntryRva,
         (void*)(ULONGLONG)&DetourFindFree, "findFreeEntry", 3, false, nullptr},
        {&g_hookSpawnEntryGet, &g_trampSpawnEntryGet, kSpawnEntryGetRva,
         (void*)(ULONGLONG)&DetourSpawnEntryGet, "spawnEntryGet", 2, false,
         nullptr},
    };

    const bool probeOn = EnvFlag("EVBOTS_SPM_PROBE");

    for (const H& h : hooks) {
        // (Запрещённые 11–12.10 хуки spawnMgrGet, spawnCtor1/2, addRef005c9d60 — роняли сервер — удалены из таблицы
        // в сборках 168–169.)
        // Точечное включение: хук со своим флагом ставится только если этот
        // флаг > 0 (независимо от SPM_PROBE). Остальные — только при общем
        // SPM_PROBE=1. Так можно снять ОДИН хук для менеджера игроков,
        // не ставя роняющие сервер перехватчики спавна.
        const bool want = (h.flag != nullptr) ? EnvFlag(h.flag) : probeOn;
        if (!want) continue;
        void* target = (void*)(base + h.rva);
        const HookResult r = h.hook->Install(target, h.detour, h.name);
        if (r == HookResult::Ok) {
            *h.tramp = h.hook->Trampoline();
            LOG_INFO("TRACE hook %s @ %p tramp=%p", h.name, target, *h.tramp);
        } else {
            *h.tramp = nullptr;
            LOG_INFO("TRACE hook %s FAILED @ %p - %s", h.name, target,
                     ToString(r));
        }
    }
    return true;
}

// ═══════════════════════════════════════════════════════════════════════
// ПЕРЕХВАТЫ вспомогательных функций спавна.
// Логика: не угадывать `this`, а снять реальные значения на живом спавне.
// FUN_00e3e1d0 была вызвана с this = точка спавна — и вернула мусор
// (begin=0x3F800000). Значит this у неё другой; узнаем из перехвата.
// ═══════════════════════════════════════════════════════════════════════

// Что сняли на живом спавне (глобалы объявлены выше, рядом с хуками)

unsigned long __fastcall DetourFindFree(void* self, void* /*edx*/,
                                        unsigned long p1, unsigned long p2,
                                        unsigned long p3) {
    unsigned long result = 0;
    if (g_trampFindFree) {
        using Tramp = unsigned long(__fastcall*)(void*, void*, unsigned long,
                                                unsigned long, unsigned long);
        result = ((Tramp)(ULONGLONG)g_trampFindFree)(self, nullptr, p1, p2, p3);
    }
    if (InterlockedCompareExchange(&g_realFreeMgr, 0, 0) == 0) {
        InterlockedExchange(&g_realFreeMgr, (LONG)p1);
        InterlockedExchange(&g_realFreeTeam, (LONG)p2);
        InterlockedExchange(&g_realFreePlayer, (LONG)p3);
        InterlockedExchange(&g_realFreeIndex, (LONG)result);
        LOG_INFO("TRACE findFreeEntry(this=%p mgr=%p team=%u player=%p -> %u)",
                 self, (void*)p1, p2, (void*)p3, result);
    }
    return result;
}

void* __fastcall DetourSpawnEntryGet(void* self, void* /*edx*/,
                                     unsigned long p1, unsigned long index) {
    void* result = 0;
    if (g_trampSpawnEntryGet) {
        // ВАЖНО: у FUN_00e37950 ДВА стековых аргумента (param_1, param_2).
        // Раньше объявили с одним — движок получал мусор вместо param_1 и
        // уходил в assert ("getEntry invoked with invalid index") =
        // STATUS_BREAKPOINT 0x80000003 (падало 15:21).
        using Tramp = void* (__fastcall*)(void*, void*, unsigned long,
                                          unsigned long);
        result = ((Tramp)(ULONGLONG)g_trampSpawnEntryGet)(self, nullptr, p1,
                                                          index);
    }
    if (InterlockedCompareExchange(&g_realEntryMgr, 0, 0) == 0) {
        InterlockedExchange(&g_realEntryMgr, (LONG)(ULONGLONG)self);
        InterlockedExchange(&g_realEntryIndex, (LONG)index);
        InterlockedExchange(&g_realEntryEntity, (LONG)(ULONGLONG)result);
        LOG_INFO("TRACE spawnEntryGet(this=%p p1=%p index=%u -> %p)", self,
                 (void*)p1, index, result);
    }
    return result;
}

// ═══════════════════════════════════════════════════════════════════════
// СПАВН ЧЕРЕЗ ФЛАГИ AI.
//
// Статический разбор FUN_00ddbbd0 (spawn_check.txt) дал ТОЧНЫЕ условия
// трёх отказов. Вместо поиска AI-точки в уровне (их вектор у точки спавна
// содержит 1 элемент, это не список точек уровня) выполняем эти условия:
//
//   1) player[0x318] — флаг «AI». Если 0, а точка не AI-типа -> отказ.
//   2) точка[0x49F] или точка[0x318] — если оба 0 -> «is not allowed to
//      spawn yet», отказ. Флаг поднимаем на время вызова и возвращаем.
//   3) player[0x31C] — команда; копируем у живого игрока.
//
// Всё пишем с логом «до/после» и откатом флагов точки.
// ═══════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════
// ДИАГНОСТИКА ОТКАЗА. Не гадаем, какой из проверок отказал: вызываем
// каждую отдельно и смотрим, какая вернула 0. Все — __thiscall,
// this = точка спавна. Только чтение состояния, ничего не меняем.
// ═══════════════════════════════════════════════════════════════════════

constexpr unsigned long kCanSpawnRva = 0x00A144C0UL;    // FUN_00e144c0
constexpr unsigned long kEntryListRva = 0x009D7140UL;    // FUN_00dd7140
constexpr unsigned long kLastPointRva = 0x00A3E1D0UL;    // FUN_00e3e1d0
constexpr unsigned long kReadyRva = 0x009D8F90UL;        // FUN_00dd8f90
constexpr unsigned long kFallbackRva = 0x00A44EA0UL;     // FUN_00e44ea0
constexpr unsigned long kTemplateRva = 0x00A59340UL;     // FUN_00e59340

constexpr int kMaxSpawnList = 1024;
unsigned long g_spPoints[kMaxSpawnList];
volatile LONG g_spPointCount = 0;
// Команда каждой точки: [obj+0xD8] (сравнение с [player+0x31C] в FUN_00dd8f90,
// 0 = общая; см. re\spawn_gates.txt). Тип/индекс входа — в ref=[obj+0x14]:
// [ref+0x180] тип, [ref+0x184] индекс входа.
unsigned int g_spTeams[kMaxSpawnList];

// Запомнить точку спавна с командой (с дедуплицикацией).
// Только чтение: первые 0x30 DWORD объекта точки спавна (диагностика 13.10).
// Нужна, чтобы сравнить живую точку (LIVE) с точками скана (SCAN): один ли это
// класс (vtable в +0), где битый ref (+0x14) и совпадает ли структура.
void DumpPointWords(const char* tag, unsigned long obj, int dwords) {
    if (obj == 0 || !Readable((const void*)(ULONGLONG)obj)) {
        LOG_INFO("POINT %s obj=%08X: не читается", tag, obj);
        return;
    }
    LOG_INFO("POINT %s obj=%08X", tag, obj);
    for (int row = 0; row < dwords; row += 8) {
        unsigned long w[8] = {0};
        for (int k = 0; k < 8; ++k) {
            Read((const void*)(ULONGLONG)obj, row * 4 + k * 4, &w[k]);
        }
        LOG_INFO("POINT %s +%03X: %08X %08X %08X %08X %08X %08X %08X %08X",
                 tag, row * 4, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
}

void RememberSpawnPoint(unsigned long self, unsigned int team) {
    if (self == 0) return;
    const LONG n = InterlockedCompareExchange(&g_spPointCount, 0, 0);
    for (LONG i = 0; i < n; ++i) {
        if (g_spPoints[i] == self) {
            g_spTeams[i] = team;  // обновляем команду (из ref точнее, чем +0xD8)
            return;               // уже есть
        }
    }
    if (n >= kMaxSpawnList) return;
    const LONG slot = InterlockedIncrement(&g_spPointCount) - 1;
    if (slot < 0 || slot >= kMaxSpawnList) return;
    g_spPoints[slot] = self;
    g_spTeams[slot] = team;
}

// round-robin счётчик для каждой команды
volatile LONG g_spRoundRobin[16] = {0};

// Вернуть n-ю точку спавна для ДАННОЙ команды (round-robin внутри команды).
// Если точек команды < n — возвращает 0 (ложится на spawnPointBest).
unsigned long SpawnPointNthByTeam(unsigned int team, unsigned long n) {
    const LONG cnt = InterlockedCompareExchange(&g_spPointCount, 0, 0);
    if (cnt <= 0) return 0;
    unsigned long found = 0;
    unsigned long seen = 0;
    for (LONG i = 0; i < cnt && i < kMaxSpawnList; ++i) {
        void* c = (void*)(ULONGLONG)g_spPoints[i];
        if (c == nullptr || !Readable(c)) continue;
        if (g_spTeams[i] != team) continue;
        found = (unsigned long)(ULONGLONG)c;
        if (seen++ >= n) break;
    }
    return found;
}

// Точка пригодна для spawnSoldier, только если цепочка [obj+0x14] -> [ref+0x180]
// читается. У точек скана ref битый (STATE 13.10: 00000205, 000000DA, 0),
// чтение падает в SEH (прогон 41: 0xC0000005 по 0x385) — такие точки не отдаём.
bool SpawnPointUsable(unsigned long obj) {
    unsigned long ref = 0;
    if (!Read((const void*)(ULONGLONG)obj, 0x14, &ref)) return false;
    if (ref == 0 || !Readable((const void*)(ULONGLONG)ref)) return false;
    unsigned long type = 0;
    return Read((const void*)(ULONGLONG)ref, 0x180, &type);
}

// ★ Точка спавна для команды бота (13.10, после прогона 38). Раньше мост брал
// EVBotsEngineSpawnPointBest() = g_spawnSelf (точка живого игрока), и все боты
// садились на него. Мод (BotSpawner.lua: _ConquestSpawn/_FindClosestSpawnPoint)
// принимает точку, если её команда == команде бота ИЛИ == 0 (общая). Берём такие
// точки по кругу внутри команды; точку живого игрока обходим, пока есть другие.
// 0 = подходящих точек нет (вызывающий берёт старый путь).
unsigned long SpawnPointForTeamImpl(unsigned int team) {
    const LONG cnt = InterlockedCompareExchange(&g_spPointCount, 0, 0);
    if (cnt <= 0) return 0;
    // ★ Правило HQ (13.10, дамп POINT REF): у ссылки точки team=[ref+0x128]:
    // 1/2 — командные спавны флагов, 0 — нейтральные (HQ). Флаги в начале раунда
    // ничьи, поэтому бот команды T должен спавниться на HQ своей команды.
    // HQ живого игрока = g_spawnSelf (он нейтральный, команда игрока = g_humanTeam),
    // HQ другой команды = другая нейтральная точка. Командные точки флагов пока
    // не используем: владелец флага не известен.
    const unsigned long real = (unsigned long)InterlockedCompareExchange(&g_spawnSelf, 0, 0);
    const unsigned int humanTeam = (unsigned int)InterlockedCompareExchange(&g_humanTeam, 0, 0);
    bool realNeutral = false;
    for (LONG i = 0; i < cnt && i < kMaxSpawnList; ++i) {
        if (g_spPoints[i] == real && g_spTeams[i] == 0) realNeutral = true;
    }
    // Живого спавна ещё не было (боты создаются сразу после подключения): HQ по
    // порядку адресов нейтральных точек — команда 1 = меньший, команда 2 = следующий
    // (наблюдалось в прогоне 46: игрок команды 1 -> F691EC80, команды 2 -> F691FA00).
    // Если живой спавн потом опровергнет порядок — логируем расхождение.
    if (real == 0 && (team == 1 || team == 2)) {
        unsigned long first = 0, second = 0;
        for (LONG i = 0; i < cnt && i < kMaxSpawnList; ++i) {
            const unsigned long c = g_spPoints[i];
            if (c == 0 || g_spTeams[i] != 0) continue;
            if (!Readable((const void*)(ULONGLONG)c) || !SpawnPointUsable(c)) continue;
            if (first == 0 || c < first) {
                second = first;
                first = c;
            } else if (second == 0 || c < second) {
                second = c;
            }
        }
        return team == 1 ? first : second;
    }
    // Своя команда совпала с командой живого игрока -> его HQ (как в BF3: все
    // свои садятся на одну базу).
    if (real != 0 && realNeutral && humanTeam != 0 && team == humanTeam &&
        SpawnPointUsable(real)) {
        return real;
    }
    // Иначе — нейтральные точки, кроме HQ живого игрока (HQ другой команды).
    unsigned long eligible = 0;
    for (LONG i = 0; i < cnt && i < kMaxSpawnList; ++i) {
        const unsigned long c = g_spPoints[i];
        if (c == 0 || c == real || g_spTeams[i] != 0) continue;
        if (!Readable((const void*)(ULONGLONG)c) || !SpawnPointUsable(c)) continue;
        ++eligible;
    }
    if (eligible == 0) return 0;
    const unsigned long want =
        (unsigned long)(InterlockedIncrement(&g_spRoundRobin[team & 15]) - 1) % eligible;
    unsigned long seen = 0;
    for (LONG i = 0; i < cnt && i < kMaxSpawnList; ++i) {
        const unsigned long c = g_spPoints[i];
        if (c == 0 || c == real || g_spTeams[i] != 0) continue;
        if (!Readable((const void*)(ULONGLONG)c) || !SpawnPointUsable(c)) continue;
        if (seen++ == want) return c;
    }
    return 0;
}



// Перебрать ВСЕ найденные точки и выбрать ту, на которой движок согласится
// спавнить.
//
// ВАЖНО (инцидент №6): вызывать движковые функции на всех найденных точках
// НЕЛЬЗЯ — часть объектов не до конца инициализирована, и перебор роняет
// сервер с 0xC0000005. Поэтому здесь ТОЛЬКО безопасные чтения полей через
// Read(), а реальный вызов делаем один раз, на лучшем кандидате, под SEH.
unsigned long g_chosenSpawn = 0;   // точка, выбранная FindUsableSpawnPoint

// ★ Каждому боту — своя точка спавна. Возвращает n-ю пригодную точку
// из уже собранного списка. Если точек меньше n — берём последнюю
// пригодную (лучше переполнение одной точки, чем возврат 0).
unsigned long SpawnPointNth(unsigned long n) {
    const LONG cnt = InterlockedCompareExchange(&g_spPointCount, 0, 0);
    if (cnt <= 0) return 0;
    unsigned long found = 0;
    LONG seen = 0;
    for (LONG i = 0; i < cnt && i < kMaxSpawnList; ++i) {
        void* c = (void*)(ULONGLONG)g_spPoints[i];
        if (c == nullptr || !Readable(c)) continue;
        unsigned long entry = 0;
        Read(c, 0x19C, &entry);
        if (entry == 0 || !Readable((void*)entry)) continue;
        found = (unsigned long)(ULONGLONG)c;
        if (seen++ >= (LONG)n) break;
    }
    return found;
}

unsigned long FindUsableSpawnPoint(unsigned long bot) {
    static int s_pickLogged = 0;  // сборка 168: сколько выборов уже расписано в лог
    const LONG n = InterlockedCompareExchange(&g_spPointCount, 0, 0);
    LOG_INFO("SpawnPick: найдено точек спавна = %ld", (long)n);
    (void)bot;
    if (n <= 0) return 0;

    unsigned long best = 0;
    unsigned long bestScore = 0;
    for (LONG i = 0; i < n && i < kMaxSpawnList; ++i) {
        void* c = (void*)(ULONGLONG)g_spPoints[i];
        if (c == nullptr || !Readable(c)) continue;

        unsigned long p10 = 0, p14 = 0;
        char f49f = 0, f318 = 0;
        Read(c, 0x10, &p10);
        Read(c, 0x14, &p14);
        Read(c, kOffSpFlag49F, &f49f);
        Read(c, kOffSpFlag318, &f318);

        // ПОЛЕ +0x19C — «объект входа». Именно его значение уходит в
        // счётчик ссылок FUN_005c9d60 как param_3, и оно же стало
        // причиной ACCESS_VIOLATION (дикий указатель 0x0DE4D50).
        // Точку без валидного +0x19C использовать для спавна НЕЛЬЗЯ.
        unsigned long entry = 0;
        Read(c, 0x19C, &entry);
        const bool entryOk = (entry != 0) && Readable((void*)entry);

        // ТИП ТОЧКИ: *(int*)(*(uint*)(точка+0x14) + 0x180).
        // 0 = человеческая, 1 = AI, 2 = соло. Штатный путь создания игрока
        // (player = NULL) работает ТОЛЬКО на типе != 0.
        unsigned long type = 0xFFFFFFFF;
        if (p14 != 0 && Readable((void*)p14)) {
            Read((void*)p14, 0x180, &type);
        }
        const char* kind = (type == 0) ? "ЧЕЛОВЕЧЕСКАЯ"
                          : (type == 1) ? "AI"
                          : (type == 2) ? "СОЛО" : "?";

        // Только чтения — никаких вызовов движка.
        unsigned long score = 0;
        if (p10 != 0 && Readable((void*)p10)) score += 4;  // объект входов
        if (p14 != 0 && Readable((void*)p14)) score += 2;  // ref-данные точки
        if (f49f != 0 || f318 != 0) score += 1;            // флаг доступности
        if (type == 1 || type == 2) score += 8;            // AI/соло — главное!
        if (!entryOk) score = 0;  // БЕЗ валидного +0x19C спавн = краш

        if (score > bestScore) {
            bestScore = score;
            best = (unsigned long)(ULONGLONG)c;
        }
        // Сборка 168: построчный разбор точек — только первые 3 выбора (было на каждом спавне, ~1700 строк за прогон).
        if (s_pickLogged < 3) {
            LOG_INFO("SpawnPick: [%3ld] %p тип=%lu (%s) +0x10=%08X +0x14=%08X "
                     "флаги=%d/%d +0x19C=%08X%s score=%lu",
                     (long)i, c, type, kind, p10, p14, (int)f49f, (int)f318,
                     entry, entryOk ? "" : " <-- НЕЛЬЗЯ", score);
        }
    }
    if (s_pickLogged < 3) ++s_pickLogged;
    LOG_INFO("SpawnPick: ЛУЧШАЯ = %p (score=%lu); всего точек %ld", (void*)best,
             bestScore, (long)n);
    if (best != 0) {
        InterlockedExchange(&g_chosenSpawn, (LONG)best);
    }
    return best;
}

// ═══════════════════════════════════════════════════════════════════════
// Поставщики для моста Lua -> движок (VextPlayerManager.cpp).
// Мост не должен знать про пробник: он просто спрашивает у движка
// «где менеджер игроков» и «какая точка спавна» — данные, которые
// хуки и так видят на живом сервере.
// ═══════════════════════════════════════════════════════════════════════

// ═══════════════════════════════════════════════════════════════════════
// ДАМП ТОЧЕК СПАВНА. Ничего не создаёт и не меняет — только читает.
// Нужен, чтобы одним прогоном узнать: сколько точек в уровне, какие из
// них AI/соло (тип != 0) и какие флаги у них стоят.
// ═══════════════════════════════════════════════════════════════════════

bool InstallSpmProbeImpl(void* moduleBase) {
    g_moduleBase = moduleBase;

    // Хук ServerPlayerManager::update (0x00E5EF00) СНОВА ДОСТУПЕН.
    // Раньше он ронял сервер 0xC0000409 из-за ОШИБКИ СИГНАТУРЫ: детектор был
    // объявлен (self, args) — два параметра, а движок зовёт
    // FUN_00e5ef00(param_1): this + edx + стековый аргумент. Реальная функция
    // читала третий параметр из нашего мусора => порча стека => __fastfail.
    // Сигнатура исправлена (три параметра), поэтому хук включается снова.
    // Кроме того, только он даёт мосту g_managerPtr — указатель менеджера.
    if (!EnvFlag("EVBOTS_SPM_TICK")) {
        LOG_WARN("хук update (0x00E5EF00) не установлен: нужен "
                 "EVBOTS_SPM_TICK=1 (без него у моста нет g_managerPtr)");
        return false;
    }

    void* target =
        (void*)(ULONGLONG)((ULONGLONG)moduleBase + kSpmUpdateRva);
    void* detour = (void*)(ULONGLONG)&DetourSpmUpdate;

    const HookResult r = g_hookSpmUpdate.Install(target, detour, "SpmProbe:g_hookSpmUpdate");
    if (r != HookResult::Ok) {
        LOG_INFO("SpmProbe: hook update NOT installed at %p - %s", target,
                 ToString(r));
        return false;
    }

    g_spmTrampoline = g_hookSpmUpdate.Trampoline();
    LOG_INFO("SpmProbe: hook installed update=%p trampoline=%p", target,
             g_spmTrampoline);

    // Перехватчики реальных вызовов спавна — чтобы узнать аргументы
    InstallSpawnTraces(moduleBase);
    InstallCrashLogger(moduleBase);
    return true;
}

// ★ 143: «исключение поймает наш же __try» — без правки 142 мест в мосте.
// Кадр MSVC EH4 (x86): регистрация в fs:[0] = [Next][Handler][ScopeTable][TryLevel],
// Handler = __except_handler4 нашей DLL, TryLevel = -2 — вне __try. Все __except
// в DLL — EXCEPTION_EXECUTE_HANDLER (сверено 15.10), значит активный __try = поймает.
// Адрес нашего __except_handler4 берём из собственного кадра (без объявления CRT).
void* g_ownSehHandler = nullptr;
volatile LONG g_vehCaught = 0;  // пойманных своими __try (не логируются построчно)

void CaptureOwnSehHandler() {
    __try {
        const NT_TIB* tib = (const NT_TIB*)::NtCurrentTeb();
        const unsigned long* rec = (const unsigned long*)tib->ExceptionList;
        g_ownSehHandler = (void*)(ULONG_PTR)rec[1];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_ownSehHandler = nullptr;
    }
}

// Ближайший кадр SEH самого упавшего кода — первый с адресом >= ESP падения
// (кадры глубже ставит уже диспетчер исключений). Читаем только внутри стека потока.
bool CaughtByOwnTry(const CONTEXT* ctx, ULONG_PTR stLo, ULONG_PTR stHi) {
    if (g_ownSehHandler == nullptr || ctx == nullptr) return false;
    const NT_TIB* tib = (const NT_TIB*)::NtCurrentTeb();
    ULONG_PTR rec = (ULONG_PTR)tib->ExceptionList;
    for (int guard = 0; guard < 64; ++guard) {
        if (rec == (ULONG_PTR)-1 || rec < stLo || rec + 16 > stHi) return false;
        const unsigned long* r = (const unsigned long*)rec;
        if (rec >= (ULONG_PTR)ctx->Esp) {
            return (void*)(ULONG_PTR)r[1] == g_ownSehHandler && (long)r[3] != -2;
        }
        rec = (ULONG_PTR)r[0];
    }
    return false;
}

// ── VEH: стек падения ────────────────────────────────────────────────────────
// Зачем: minidump от BF3 пишется 64-битным писателем и кладёт DataSize=0,
//    из-за чего стек из дампа НЕ ИЗВЛЕКАЕТСЯ вообще. Свой обработчик
//    логирует EIP + цепочку адресов возврата до падения — это надёжнее дампа.
// Возвращаем EXCEPTION_CONTINUE_SEARCH: поведение процесса не меняем,
// дамп и дальше пишется.
LONG WINAPI CrashLoggerVeh(PEXCEPTION_POINTERS info) {
    if (info == nullptr || info->ExceptionRecord == nullptr) return EXCEPTION_CONTINUE_SEARCH;
    const unsigned long code = info->ExceptionRecord->ExceptionCode;
    // ГРАБЛИ: не логируем ВСЁ. 0x40010006 (DBG_PRINTEXCEPTION_C из
    // OutputDebugString/KERNELBASE) — безобидное исключение, оно прилетает
    // при старте и съедало лимит в 3 сообщения, из-за чего настоящее
    // падение не попадало в лог. Логируем только фатальные (>= 0xC0000000).
    if (code < 0xC0000000UL) return EXCEPTION_CONTINUE_SEARCH;
    // ★ 143: исключение внутри пробы (Readable/Writable/Write/Read) ловит её
    // собственный __except — это не падение, лимит на него не тратим.
    if (t_probeDepth > 0) return EXCEPTION_CONTINUE_SEARCH;
    // ★ 143: повторный вход на том же потоке (исключение внутри самого
    // обработчика) не логируем — раньше обход стека выходил за верх стека,
    // ловил AV и вкладывал VEH сам в себя: записи #2–#12 в каждом запуске.
    static thread_local int t_inVeh = 0;
    if (t_inVeh > 0) return EXCEPTION_CONTINUE_SEARCH;
    // ★ 143: исключение внутри любого __try нашей DLL (пробы моста, 142 места) —
    // его поймает свой __except. Прогон 11:12: 12 записей от ReadObjName
    // (поиск внешности по словам кита, имя по [obj+8] = 0xCCCCCCCC).
    {
        const NT_TIB* tib = (const NT_TIB*)::NtCurrentTeb();
        if (CaughtByOwnTry(info->ContextRecord, (ULONG_PTR)tib->StackLimit,
                           (ULONG_PTR)tib->StackBase)) {
            InterlockedIncrement(&g_vehCaught);
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    static LONG s_reported = 0;
    const LONG n = InterlockedIncrement(&s_reported);
    if (n <= 12) {
        ++t_inVeh;
        void* eip = info->ExceptionRecord->ExceptionAddress;
        // Модуль+смещение: EIP бывает и в нашей DLL (rva от exe там бессмыслен).
        char modPath[MAX_PATH] = "?";
        HMODULE mod = nullptr;
        unsigned long modOff = 0;
        if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 (LPCSTR)eip, &mod) && mod != nullptr) {
            ::GetModuleFileNameA(mod, modPath, MAX_PATH);
            modOff = (unsigned long)((ULONGLONG)eip - (ULONGLONG)mod);
        }
        const char* modName = ::strrchr(modPath, '\\');
        modName = (modName != nullptr) ? modName + 1 : modPath;
        LOG_WARN("VEH #%ld исключение=0x%08lX EIP=%p rva=%08lX (%s+0x%lX)",
                 n, code, eip,
                 (unsigned long)((ULONGLONG)eip - (ULONGLONG)g_moduleBase),
                 modName, modOff);
        if (code == 0xC0000005 && info->ExceptionRecord->NumberParameters >= 2) {
            LOG_WARN("VEH доступ: %s по 0x%08lX",
                     info->ExceptionRecord->ExceptionInformation[0] ? "ЗАПИСЬ" : "ЧТЕНИЕ",
                     info->ExceptionRecord->ExceptionInformation[1]);
        }
        // Цепочка адресов возврата: идём по стеку и логируем всё, что
        // попадает в образ exe — это и есть стек вызовов.
        // ГРАБЛИ: (а) диапазон образа брать из PE-заголовка — у exe
        // SizeOfImage около 0x2D00000, жёсткая константа 0xA00000
        // отсекала всё; (б) ★ 143: обход только в границах стека потока
        // (NT_TIB StackLimit..StackBase) — читать без проб, ничего не падает.
        // Прежний обход 4096 слов через Readable() выходил за StackBase.
        // ★ Сборка 174: регистры и чьи они (игрок/солдат бота, живой или труп) — прогон 173: падение на трупе с Glock18,
        // по стеку не понять, чей солдат. Сверка только с массивами моста (память движка не читаем).
        if (info->ContextRecord != nullptr) {
            const CONTEXT* c = info->ContextRecord;
            LOG_WARN("VEH регистры: EAX=%08lX EBX=%08lX ECX=%08lX EDX=%08lX ESI=%08lX EDI=%08lX EBP=%08lX ESP=%08lX",
                     c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp, c->Esp);
            const struct { const char* n; unsigned long v; } regs[] = {
                {"EAX", c->Eax}, {"EBX", c->Ebx}, {"ECX", c->Ecx}, {"EDX", c->Edx},
                {"ESI", c->Esi}, {"EDI", c->Edi}, {"EBP", c->Ebp}};
            char who[160];
            for (const auto& r : regs) {
                if (::evbots::EVBotsBridgeDescribePtr(r.v, who, sizeof(who))) LOG_WARN("VEH %s = %s", r.n, who);
            }
            const NT_TIB* tibW = (const NT_TIB*)::NtCurrentTeb();
            const ULONG_PTR espW = (ULONG_PTR)c->Esp;
            if (espW >= (ULONG_PTR)tibW->StackLimit && espW < (ULONG_PTR)tibW->StackBase) {
                const void* const* spW = (const void* const*)espW;
                const int wordsW = (int)(((ULONG_PTR)tibW->StackBase - espW) / sizeof(void*));
                int hits = 0;
                for (int i = 0; i < 1024 && i < wordsW && hits < 16; ++i) {
                    if (::evbots::EVBotsBridgeDescribePtr((unsigned long)(ULONG_PTR)spW[i], who, sizeof(who))) {
                        LOG_WARN("VEH стек[%d] = %08lX — %s", i, (unsigned long)(ULONG_PTR)spW[i], who);
                        ++hits;
                    }
                }
            }
        }
        if (info->ContextRecord != nullptr) {
            const unsigned long base = (unsigned long)(ULONGLONG)g_moduleBase;
            unsigned long imgSize = 0x00400000UL;
            const auto* dos = (const IMAGE_DOS_HEADER*)g_moduleBase;
            if (dos != nullptr && dos->e_magic == IMAGE_DOS_SIGNATURE) {
                const auto* nt = (const IMAGE_NT_HEADERS32*)
                    ((const char*)g_moduleBase + dos->e_lfanew);
                if (nt->Signature == IMAGE_NT_SIGNATURE) {
                    imgSize = nt->OptionalHeader.SizeOfImage;
                }
            }
            const NT_TIB* tib = (const NT_TIB*)::NtCurrentTeb();
            const ULONG_PTR stLo = (ULONG_PTR)tib->StackLimit;
            const ULONG_PTR stHi = (ULONG_PTR)tib->StackBase;
            const ULONG_PTR esp = (ULONG_PTR)info->ContextRecord->Esp;
            if (esp < stLo || esp >= stHi) {
                LOG_WARN("VEH: ESP=%08lX вне стека потока [%08lX..%08lX) — стек не обхожу",
                         (unsigned long)esp, (unsigned long)stLo, (unsigned long)stHi);
            } else {
                void** sp = (void**)esp;
                const int words = (int)((stHi - esp) / sizeof(void*));
                int found = 0;
                for (int i = 0; i < 4096 && i < words && found < 32; ++i) {
                    const unsigned long v = (unsigned long)(ULONGLONG)sp[i];
                    if (v >= base && v < base + imgSize) {
                        LOG_WARN("VEH стек[%d] = 0x%08lX (rva 0x%08lX)", i, v, v - base);
                        ++found;
                    }
                }
                if (found == 0) LOG_WARN("VEH: в стеке не нашлось адресов образа");
            }
        }
        --t_inVeh;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallCrashLogger(void* /*moduleBase*/) {
    if (g_veh == nullptr) {
        CaptureOwnSehHandler();
        g_veh = AddVectoredExceptionHandler(1, &CrashLoggerVeh);
        if (g_veh != nullptr) {
            LOG_INFO("VEH установлен: %p (стек падения будет в логе; свой SEH-обработчик %p)",
                     g_veh, g_ownSehHandler);
        } else {
            LOG_WARN("VEH НЕ установлен");
        }
    }
}

}  // namespace

// Тонкая экспортируемая обёртка: ModdingInterface.cpp вызывает
// InstallSpmProbe, поэтому он обязан быть вне анонимного namespace.
bool InstallSpmProbe(void* moduleBase) {
    return InstallSpmProbeImpl(moduleBase);
}

// Точка спавна по команде бота — для моста (VextPlayerManager.cpp).
unsigned long EVBotsEngineSpawnPointForTeam(unsigned int team) {
    return SpawnPointForTeamImpl(team);
}

// ★ Сборка 195: запасная точка для бота команды team. EVBotsEngineSpawnPointBest() — точка живого игрока (HQ его
// команды) для ЛЮБОЙ команды: прогон 194 (01:49:07) — BOT_Lew (т2) сел на HQ США F70F3D60 сразу после спавна человека
// (т1), CombatArea:PlayerDeserting и смерть на чужой базе. Чужую точку не отдаём: 0 — спавн ждёт свою (очередь повторит).
unsigned long EVBotsEngineSpawnPointBestForTeam(unsigned int team) {
    const unsigned long real = (unsigned long)g_spawnSelf;
    if (real != 0) {
        const unsigned int humanTeam = (unsigned int)InterlockedCompareExchange(&g_humanTeam, 0, 0);
        return (humanTeam != 0 && team == humanTeam) ? real : 0;
    }
    return FindUsableSpawnPoint(0);
}

// Скан точек закончен: до этого боты не создаются (иначе садятся на точку игрока).
bool EVBotsEngineSpawnScanDone() {
    return InterlockedCompareExchange(&g_scanFinished, 0, 0) != 0;
}

// Указатель PointSystem (снят перехватчиком регистрации у человека), 0 — ещё нет.
unsigned long EVBotsEnginePointSystem() {
    return (unsigned long)InterlockedCompareExchange(&g_pointSystem, 0, 0);
}

// Команда живого игрока в момент его последнего спавна (1/2, 0 — неизвестна).
unsigned int EVBotsEngineHumanTeam() {
    return (unsigned int)InterlockedCompareExchange(&g_humanTeam, 0, 0);
}

// Запуск скана точек из очереди ботов (без живого спавна), не чаще раза в 3 с.
// Нужен, чтобы боты создавались сразу после подключения игрока, как в VU.
void EVBotsEngineStartSpawnScan() {
    static ULONGLONG s_last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_last < 3000) return;
    if (InterlockedCompareExchange(&g_scanDone, 1, 0) == 0) {
        s_last = now;
        HANDLE h = ::CreateThread(nullptr, 0, ScanSpawnThread, nullptr, 0, nullptr);
        if (h != nullptr) ::CloseHandle(h);
    }
}

// ★ Сборка 173: смена уровня (Level:Destroy → Level:Loaded, NativeBridge.LevelReload). Точки спавна, собранные сканом
// на старом уровне, и точка живого игрока (g_spawnSelf) указывают на освобождённые объекты: прогон 172, раунд 2 (12:14) —
// все 15 спавнов ботов «исключение … адрес=00000180» ([ref+0x180] у пустой ссылки), ни один бот не получил тело.
// Сбрасываем список и запускаем скан нового уровня.
void EVBotsEngineResetSpawnPoints() {
    const LONG had = InterlockedExchange(&g_spPointCount, 0);
    InterlockedExchange(&g_spawnSelf, 0);
    InterlockedExchange(&g_scanFinished, 0);
    InterlockedExchange(&g_scanDone, 0);
    LOG_INFO("Bridge: смена уровня — точки спавна старого уровня сброшены (%ld), скан нового уровня", had);
    EVBotsEngineStartSpawnScan();
}

// Кит (customization) из a1 вызова spawnSoldier у живого игрока (13.10: a1 — НЕ
// игрок, а объект комплекта; см. MODLOG «applyCustomization»).
unsigned long EVBotsEngineSpawnKit() {
    return (unsigned long)InterlockedCompareExchange(&g_spawnA1, 0, 0);
}

// ★ Сборка 123: эталон человека ДО его спавна (боты до дислокации, как в VU). Живого спавна ещё не было —
// берём команду, +0x318 и +0x480 прямо у игрока-человека из списка менеджера; кит (a1 spawnSoldier) = сам игрок
// (в логе a1 spawnSoldier == указатель человека). g_spawnSelf НЕ трогаем: точки команд — по скану.
// Сборка 124: fallback480 — блюпринт MpSoldier из памяти (у человека до дислокации +0x480 пуст).
bool EVBotsEngineAdoptHumanRef(unsigned long human, unsigned long fallback480) {
    if (human == 0) return false;
    unsigned int t = 0, v318 = 0, v480 = 0;
    const void* hp = (const void*)(ULONGLONG)human;
    if (!Read(hp, 0x31C, &t) || !Read(hp, 0x318, &v318) || !Read(hp, 0x480, &v480)) return false;
    if (v480 == 0) v480 = (unsigned int)fallback480;
    if (v480 == 0) return false;
    if (t != 1 && t != 2) t = 0;  // ★ 167: команда человека ещё не назначена — эталону она не нужна (боту пишется своя)
    InterlockedCompareExchange(&g_spawnA1, (LONG)human, 0);
    if (t != 0) InterlockedCompareExchange(&g_humanTeam, (LONG)t, 0);
    if (InterlockedCompareExchange(&g_srefFields, 0, 0) == 0) {
        InterlockedExchange(&g_sref318, (LONG)v318);
        InterlockedExchange(&g_sref31c, (LONG)t);
        InterlockedExchange(&g_sref480, (LONG)v480);
        InterlockedExchange(&g_srefFields, 1);
        LOG_INFO("TRACE ЭТАЛОН ДО СПАВНА (человек %08lX): +0x318=%08X +0x31C=%08X +0x480=%08X", human, v318, t, v480);
    }
    return true;
}

// Внешняя обёртка (вне анонимного namespace, иначе линкер не видит символ из VextPlayerManager.cpp).
int EVBotsEngineHumanSquad() { return (int)InterlockedCompareExchange(&g_humanSquad, 0, 0); }

// Read-only signature check for Player.teamId/squadId. Preserve our trace detours;
// accept a patched entry only when this exact InlineHook owns it and its stolen
// original bytes and the remainder still match the verified PE instructions.
bool EVBotsVerifyTeamPropertyEntry(unsigned long address,const unsigned char* expected,unsigned size) {
    const InlineHook* hook=nullptr;
    if(address==0x0118EF80)hook=&g_hookAddToTeam;
    else if(address==0x0118D8D0)hook=&g_hookAddToSquad;
    else return false;
    if(!expected || !size || size>32)return false;
    __try {
        const auto* live=reinterpret_cast<const unsigned char*>(address);
        if(!hook->IsInstalled())return std::memcmp(live,expected,size)==0;
        const size_t stolen=hook->StolenBytes();
        constexpr size_t patched=8; // ev-core EvHook.cpp kPatchSize; stolen may be larger.
        if(hook->Target()!=live || !hook->Trampoline() || stolen<patched || stolen>size || live[0]!=0xE9)return false;
        const auto destination=address+5+*reinterpret_cast<const long*>(live+1);
        if(destination!=reinterpret_cast<unsigned long>(hook->Detour()))return false;
        for(size_t i=5;i<patched;++i)if(live[i]!=0x90)return false;
        const auto* trampoline=static_cast<const unsigned char*>(hook->Trampoline());
        if(trampoline[stolen]!=0xE9 || reinterpret_cast<unsigned long>(trampoline)+stolen+5+
            *reinterpret_cast<const long*>(trampoline+stolen+1)!=address+stolen)return false;
        return std::memcmp(trampoline,expected,stolen)==0 &&
            std::memcmp(live+patched,expected+patched,size-patched)==0;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}

}  // namespace evbots
