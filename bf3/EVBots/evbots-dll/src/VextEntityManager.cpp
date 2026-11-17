// VextEntityManager.cpp — VeniceEXT EntityManager.
//
// fun-bots берёт через EntityManager:GetIterator флаги Conquest (ServerCapturePointEntity: цели GameDirector,
// спавн на захваченных точках) и точки спавна (ServerCharacterSpawnEntity). До сборки 125 итератор был пустой
// заглушкой — боты не видели флагов и бегали без цели.
//
// Сборка 125 (разбор агента, C:\Games\re\capture\): перечисление — штатный итератор движка (тот же, что у тикетов):
// begin 0x005CF340 (thiscall: мир, TypeInfo*, буфер, 1), next 0x005CF2F0; мир = [0x02B3EE30].
// Поля ServerCapturePointEntity (ФАКТ по ghost-записи 0x01325D50 и update): +0xB0 команда, +0xC8 flagLocation (0..100),
// +0xD4 isControlled, +0xD5 isCaptureEnabled, +0xE6 isAttacked, +0x10 шина, +0x14 data (CapturableType +0xA8: 1 = HQ).
// Трансформ сущности: [+0x1C], при бите 18 в [+0x08] — [+0x1C] + u8[[+0x1C]+6]*0x50 + 0x10; позиция = +0x30.
// Точка спавна: команда = [[+0x14]+0x128]. Имени у точки захвата в движке нет — имя синтетическое (у HQ на «HQ»):
// мод сопоставляет флаги с целями mapfile по позиции, имя ему нужно только уникальное.
// События CapturePoint:Captured / CapturePoint:Lost / Player:EnteredCapturePoint движок моду не шлёт — опрос раз в 0,5 с.
// Сборка 178: техника (ServerVehicleEntity) отдаётся моду — итератор, transform, entryCount, GetPlayerInEntry, data
// (живой VehicleEntityData), velocity по смещению, события Vehicle:SpawnDone/Unspawn/Enter/Exit опросом. Прочие типы — пусто.
// В тест-хосте модуля движка нет — итератор пуст, как раньше.

#include "VextEntityManager.h"

#include <windows.h>
#include <intrin.h>  // сборка 264: __readfsdword — static PE TLS перед вызовом арены

#include <string>

#include "EVBotsLog.h"
#include "EngineResources.h"
#include "InlineHook.h"
#include "SpawnPolicy.h"
#include "RushPolicy.h"
#include "VextPlayerManager.h"  // сборка 264: EngineLevelReady/EVBotsLevelLoadedSeq для имени CapturePoint

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace evbots {
bool EVBotsQueueVehicleTeleport(unsigned long veh, const float* m12);  // сборка 185, определение — VextPlayerManager.cpp

void EVBotsPushLiveInstance(lua_State* L, unsigned long obj);  // сборка 178, определение — VextResourceManager.cpp

namespace {

constexpr unsigned long kImageBase = 0x00400000UL;
constexpr unsigned long kEntityWorldVa = 0x02B3EE30UL;  // [VA] -> мир сущностей

struct TypeRec {
    const char* name;
    unsigned long typeInfoVa;
    unsigned long vtableVa;  // 0 — не проверять
};
const TypeRec kTypes[] = {
    {"ServerCapturePointEntity", 0x02C23918UL, 0x02696660UL},
    {"ServerCharacterSpawnEntity", 0x02C16B84UL, 0x025BBBC0UL},
    {"ServerVehicleEntity", 0x02C17090UL, 0x025BF8E8UL},  // сборка 159 — пробник; сборка 178 — и моду
    // ★ Сборка 186: для Entity:RegisterEventCallback мода — зона Rush (ServerSyncedBoolEntity «SetTrue») и запрет ввода
    // в начале раунда (ServerInputRestrictionEntity «Activate»/«Deactivate»). TypeInfo — слот 0 vtable
    // (`mov eax, TI; ret`, 0x00D8C390 / 0x00DCA670), способ сверен на флаге и технике (TI совпали с известными).
    {"ServerSyncedBoolEntity", 0x02C160B8UL, 0},  // без сверки vtable: прогон 18:45 (Rush) — 0 штук при vtable 0x025B50D8
    {"ServerInputRestrictionEntity", 0x02C16904UL, 0x025BB8A8UL},
    // Сборка 186: аптечки и ящики патронов (диагностика «раскладывают ли боты»). TI — из статической регистрации типа
    // (0x0227CA80: push данные 0x02C236DC «ServerSupplySphereEntity»; mov ecx, 0x02C23A08), vtable — `mov eax, TI; ret`.
    {"ServerSupplySphereEntity", 0x02C23A08UL, 0x02696B68UL},
    // Сборка 186: Rush — счётчик жизней (этапы, LifeCounter:BaseDestroyed) и цели (МКОМ). TI/vtable — typeinfo.py
    // (статическая регистрация типа), vtable счётчика совпала с разбором субагента (0x0268B8D8, onEvent 0x012DC140).
    {"ServerLifeCounterEntity", 0x02C23020UL, 0},
    {"ServerObjectiveEntity", 0x02C1697CUL, 0},
    // ★ Сборка 201: точка спавна техники — дочерняя сущность шины точки спавна (fun-bots: BotSpawner.lua:1001/1030/1256,
    // `bus.entities` → `Is("ServerVehicleSpawnEntity")`). TypeInfo сверен по exe: регистрация 0x02272100 (push 0x02C178CC —
    // данные с именем «ServerVehicleSpawnEntity» @0x025C836C; mov ecx, 0x02C17AD0); тот же TI у слота getType 0x00D4A8A0.
    // Список — тем же алгоритмом, что у остальных типов (peekbus.py читал его на живом сервере); vtable не сверяем.
    {"ServerVehicleSpawnEntity", 0x02C17AD0UL, 0},
};

unsigned char* EngineBase() {
    return reinterpret_cast<unsigned char*>(::GetModuleHandleA("bf3_server_Final.exe"));
}

// ★ Сборка 186: Entity:RegisterEventCallback, как VU. Движок отдаёт событие сущности виртуальным методом onEvent
// (слот vtable +0x10; thiscall(this, EntityEvent*), ret 4; [ev+4] — eventId = хеш движка djb2-xor имени события,
// например SetTrue 0xA4253151, Activate 0xDEAA09DC — подтверждено cmp в 0x00D9C440 и 0x00DE2AB0). Слот vtable класса
// подменяется один раз (сверка: в слоте адрес внутри .text exe); события только «наблюдаемых» сущностей идут в кольцо,
// Lua разбирает его в EntityManager._Poll и зовёт обработчики мода с {eventId}.
struct EvHook {
    volatile LONG slot;  // адрес слота vtable
    volatile LONG orig;  // прежний onEvent
};
EvHook g_evHooks[32] = {};
volatile LONG g_evHookN = 0;
volatile LONG g_evWatched[128] = {};
volatile LONG g_evWatchedN = 0;
struct EvItem {
    unsigned long ent;
    unsigned long id;
    bool baseDestroyed;
    bool finalBase;
};
EvItem g_evRing[256] = {};
volatile LONG g_evWrite = 0;
LONG g_evRead = 0;
SRWLOCK g_evRingLock = SRWLOCK_INIT;
volatile LONG g_evEpoch = 0;

unsigned long TypeOfNoCall(unsigned long obj);

using OnEventFn = void(__fastcall*)(void*, void*, void*);
void __fastcall EvDetour(void* self, void* edx, void* ev) {
    unsigned long orig = 0;
    EvItem item = {};
    int baseBefore = -1;
    bool lifeCounter = false;
    const LONG epoch = InterlockedCompareExchange(&g_evEpoch, 0, 0);
    __try {
        const unsigned long vt = *(const unsigned long*)self;
        const LONG n = g_evHookN;
        for (LONG i = 0; i < n && i < 32; ++i) {
            if ((unsigned long)g_evHooks[i].slot == vt + 0x10) { orig = (unsigned long)g_evHooks[i].orig; break; }
        }
        if (ev != nullptr) {
            const LONG wn = g_evWatchedN;
            for (LONG i = 0; i < wn && i < 128; ++i) {
                if ((unsigned long)g_evWatched[i] == (unsigned long)(ULONG_PTR)self) {
                    item.ent = (unsigned long)(ULONG_PTR)self;
                    item.id = *(const unsigned long*)((const unsigned char*)ev + 4);
                    if (item.id == kRushDestructBase &&
                        TypeOfNoCall(item.ent) == (unsigned long)(ULONG_PTR)(EngineBase() + (0x02C23020UL - kImageBase))) {
                        lifeCounter = true;
                        baseBefore = *(const int*)((const unsigned char*)self + 0xBC);
                    }
                    break;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (orig != 0) ((OnEventFn)(ULONG_PTR)orig)(self, edx, ev);
    if (item.ent == 0) return;
    // Публикуем после оригинала: у отключённого/исчерпанного счётчика вход DestructBase не создаёт BaseDestroyed.
    // Сверено по exe f39f7623…: 012DC212–012DC26D; IncreaseReinforce — отдельная ветка 012D9380.
    if (lifeCounter) __try {
        const int remaining = *(const int*)((const unsigned char*)self + 0xBC);
        item.baseDestroyed = RushBaseDestroyedAccepted(item.id, true, baseBefore, remaining);
        const unsigned char* data = *(const unsigned char* const*)((const unsigned char*)self + 0x14);
        item.finalBase = data != nullptr && RushFinalBase(item.baseDestroyed, remaining, data[0x98] != 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { item.baseDestroyed = item.finalBase = false; }
    AcquireSRWLockExclusive(&g_evRingLock);
    if (epoch == g_evEpoch) {
        const LONG w = InterlockedIncrement(&g_evWrite) - 1;
        g_evRing[w & 255] = item;
    }
    ReleaseSRWLockExclusive(&g_evRingLock);
}

/// EVBotsEmEvWatch(ptr) -> true | false, причина
int l_em_ev_watch(lua_State* L) {
    const unsigned long ent = (unsigned long)luaL_checkinteger(L, 1);
    unsigned char* base = EngineBase();
    if (base == nullptr || ent == 0) { lua_pushboolean(L, 0); lua_pushstring(L, "движка нет"); return 2; }
    const unsigned long lo = (unsigned long)(ULONG_PTR)base, hi = lo + 0x02A00000UL;  // образ exe (.text/.rdata)
    unsigned long vt = 0, cur = 0;
    __try {
        vt = *(const unsigned long*)(ULONG_PTR)ent;
        if (vt >= lo && vt < hi) cur = *(const unsigned long*)(ULONG_PTR)(vt + 0x10);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        vt = 0;
    }
    if (vt < lo || vt >= hi || cur == 0) { lua_pushboolean(L, 0); lua_pushstring(L, "не сущность exe"); return 2; }
    const unsigned long det = (unsigned long)(ULONG_PTR)&EvDetour;
    if (cur != det) {
        if (cur < lo || cur >= lo + 0x01C00000UL) { lua_pushboolean(L, 0); lua_pushstring(L, "слот +0x10 не в .text"); return 2; }
        const LONG n = g_evHookN;
        if (n >= 32) { lua_pushboolean(L, 0); lua_pushstring(L, "перехватов слишком много"); return 2; }
        g_evHooks[n].slot = (LONG)(vt + 0x10);
        g_evHooks[n].orig = (LONG)cur;
        InterlockedExchange(&g_evHookN, n + 1);
        // сборка 237: замена слота — ядром (ev-core), только если в слоте всё ещё прочитанный обработчик
        if (!PatchPointer((void*)(ULONG_PTR)(vt + 0x10), (void*)(ULONG_PTR)det, (void*)(ULONG_PTR)cur, nullptr,
                          "onEvent сущности (RegisterEventCallback)")) {
            lua_pushboolean(L, 0); lua_pushstring(L, "ядро не заменило слот"); return 2;
        }
        LOG_INFO("EVBots: EntityManager: ★ onEvent перехвачен: vtable %08lX слот +0x10 %08lX -> %08lX (RegisterEventCallback, как VU)",
                 vt, cur, det);
    }
    const LONG wn = g_evWatchedN;
    for (LONG i = 0; i < wn; ++i) if ((unsigned long)g_evWatched[i] == ent) { lua_pushboolean(L, 1); return 1; }
    if (wn >= 128) { lua_pushboolean(L, 0); lua_pushstring(L, "наблюдаемых слишком много"); return 2; }
    g_evWatched[wn] = (LONG)ent;
    InterlockedExchange(&g_evWatchedN, wn + 1);
    lua_pushboolean(L, 1);
    return 1;
}

// ★ Сборка 186: хук VU «EntityFactory:Create» (мод: FunBotServer:OnEntityFactoryCreate — ракеты с наведением → боты в
// технике отстреливают ловушки/дым, BotManager:CheckForFlareOrSmoke). Движок: ServerEntityFactory::createEntity
// 0x00DA36E0 (cdecl(params), ret; данные сущности — [params+0xB0], результат — созданная сущность).
// Перехват отдаёт в кольцо только сущности с данными MissileEntityData (тип — vtable[0]() данных, кэш по TypeInfo);
// Lua вызывает обработчики мода после создания: ctx:Call() возвращает уже созданную ракету (оригинал уже выполнен).
using CreateEntityFn = void*(__cdecl*)(void*);
void* g_trampCreate = nullptr;
struct CreatedItem {
    unsigned long ent;
    unsigned long data;
};
CreatedItem g_createdRing[64] = {};
volatile LONG g_createdWrite = 0;
LONG g_createdRead = 0;
unsigned long g_missileTi = 0;            // TypeInfo MissileEntityData (найден по имени)
unsigned long g_notMissile[64] = {};      // TypeInfo других типов (кэш «не ракета»)
volatile LONG g_notMissileN = 0;

// ★ Сборка 201: тип данных БЕЗ вызова виртуального метода. eng::TypeOf зовёт vtable[0]() — для чужого объекта это может быть
// деструктор (прогон 16:52: вскоре после перехода на [params+0x14] — assert'ы SubLevel/EntityWorld и исполнение мусора). У классов
// с рефлексией getType — ровно `mov eax, TypeInfo; ret` (B8 imm32 C3, как 0x00D4A8A0): берём imm32 из байтов кода, ничего не зовём.
unsigned long TypeOfNoCall(unsigned long obj) {
    unsigned char* base = EngineBase();
    if (base == nullptr || obj == 0) return 0;
    const unsigned long lo = (unsigned long)(ULONG_PTR)base;
    __try {
        const unsigned long vt = *(const unsigned long*)(ULONG_PTR)obj;
        if (vt < lo || vt >= lo + 0x02900000UL) return 0;  // vtable — в образе exe
        const unsigned long fn = *(const unsigned long*)(ULONG_PTR)vt;
        if (fn < lo + 0x1000UL || fn >= lo + 0x02028000UL) return 0;  // код — в .text
        const unsigned char* c = (const unsigned char*)(ULONG_PTR)fn;
        if (c[0] != 0xB8 || c[5] != 0xC3) return 0;
        return *(const unsigned long*)(c + 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool IsMissileData(unsigned long data) {
    const unsigned long t = TypeOfNoCall(data);
    if (t == 0) return false;
    if (t == g_missileTi) return true;
    const LONG n = g_notMissileN;
    for (LONG i = 0; i < n && i < 64; ++i) if (g_notMissile[i] == t) return false;
    eng::TypeDesc d;
    if (eng::Describe(t, &d) && d.name != nullptr && ::strcmp(d.name, "MissileEntityData") == 0) {
        g_missileTi = t;
        return true;
    }
    if (n < 64) { g_notMissile[n] = t; InterlockedExchange(&g_notMissileN, n + 1); }
    return false;
}

// ★ Сборка 201, диагностика: сколько раз движок зовёт createEntity и для каких типов данных. В прогоне XP5_002 (стреляли по самолётам)
// не пришла ни одна ракета — нужно знать, идёт ли создание ракеты через эту функцию вообще.
struct CreateStat { unsigned long ti; volatile LONG n; };
CreateStat g_createStat[64] = {};
volatile LONG g_createStatN = 0;
volatile LONG g_createTotal = 0;
volatile LONG g_missileCtors = 0;  // сборка 211: ракеты через конструктор ServerMissileEntity

void CountCreate(unsigned long data) {
    InterlockedIncrement(&g_createTotal);
    const unsigned long t = TypeOfNoCall(data);
    if (t == 0) return;
    const LONG cnt = g_createStatN;
    for (LONG i = 0; i < cnt && i < 64; ++i) {
        if (g_createStat[i].ti == t) { InterlockedIncrement(&g_createStat[i].n); return; }
    }
    if (cnt < 64) {
        g_createStat[cnt].ti = t;
        g_createStat[cnt].n = 1;
        InterlockedExchange(&g_createStatN, cnt + 1);
    }
}

/// EVBotsEmCreateStats() -> total, { {name, n}, ... } (по убыванию)
int l_em_create_stats(lua_State* L) {
    lua_pushinteger(L, (lua_Integer)g_createTotal);
    lua_newtable(L);
    lua_pushinteger(L, (lua_Integer)g_missileCtors);
    lua_setfield(L, -2, "missiles");
    const LONG cnt = g_createStatN;
    int k = 0;
    for (LONG i = 0; i < cnt && i < 64; ++i) {
        eng::TypeDesc d;
        const char* nm = (eng::Describe(g_createStat[i].ti, &d) && d.name != nullptr) ? d.name : "?";
        lua_createtable(L, 2, 0);
        lua_pushstring(L, nm);
        lua_rawseti(L, -2, 1);
        lua_pushinteger(L, (lua_Integer)g_createStat[i].n);
        lua_rawseti(L, -2, 2);
        lua_rawseti(L, -2, ++k);
    }
    return 2;
}

void* __cdecl CreateEntityDetour(void* params) {
    void* ent = ((CreateEntityFn)g_trampCreate)(params);
    __try {
        if (ent != nullptr && params != nullptr) {
            // ★ Сборка 201: данные сущности — [params+0xB0] («params.data»: assert 0x02C54820 у cmp [params+0xB0], 0 в createEntity).
            // [params+0x10] — создатель, [params+0x14] — subLevel («!params.subLevel->isDestroyed()», internalCreateEntity). С 186
            // смотрели +0x10 — тип не определялся (прогоны 16:38/16:55: 1027 и 2672 вызова), ни одной ракеты моду, ловушек не было.
            const unsigned long data = *(const unsigned long*)((const unsigned char*)params + 0xB0);
            if (data != 0) CountCreate(data);
            if (data != 0 && IsMissileData(data)) {
                const LONG w = InterlockedIncrement(&g_createdWrite) - 1;
                g_createdRing[w & 63].ent = (unsigned long)(ULONG_PTR)ent;
                g_createdRing[w & 63].data = data;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return ent;
}

// ★ Сборка 211: ракеты идут МИМО createEntity. Факт: «ДИАГ createEntity» за все прогоны (в т.ч. XP3_Shield 22.10 с боевыми
// вертолётами) — пули, взрывы, наборы, MissileEntityData ни разу; ловушки боты не отстреливали никогда (жалоба друга 22.10).
// Разбор exe (capstone): конструктор ServerMissileEntity 0x00E72F90 (строка лога «No physical representation of missile found»,
// logevent 0x02C5886C) — thiscall(this, info = params+0x10, data = [params+0xB0]), ret 8, возвращает this. Его зовут три создателя:
// 0x00DB7100 (таблица создателей 0x025B0828), 0x012C7870 (VeniceServerMissileEntity, vtable 0x02688BB8) и 0x012C8290 — ни один не
// через createEntity 0x00DA36E0. Перехват конструктора видит каждую ракету; данные — её MissileEntityData, сущность — this.
using MissileCtorFn = void*(__fastcall*)(void*, void*, void*, void*);
void* g_trampMissile = nullptr;
void* __fastcall MissileCtorDetour(void* self, void* edx, void* info, void* data) {
    void* r = ((MissileCtorFn)g_trampMissile)(self, edx, info, data);
    InterlockedIncrement(&g_missileCtors);
    if (self != nullptr && data != nullptr) {
        const LONG w = InterlockedIncrement(&g_createdWrite) - 1;
        g_createdRing[w & 63].ent = (unsigned long)(ULONG_PTR)self;
        g_createdRing[w & 63].data = (unsigned long)(ULONG_PTR)data;
    }
    return r;
}

/// EVBotsEmCreatedPoll() -> { {entity, data}, ... } — созданные ракеты с прошлого опроса
int l_em_created_poll(lua_State* L) {
    lua_newtable(L);
    const LONG w = InterlockedCompareExchange(&g_createdWrite, 0, 0);
    if (w - g_createdRead > 64) g_createdRead = w - 64;
    int k = 0;
    while (g_createdRead < w) {
        const CreatedItem it = g_createdRing[g_createdRead & 63];
        ++g_createdRead;
        lua_createtable(L, 2, 0);
        lua_pushinteger(L, (lua_Integer)it.ent);
        lua_rawseti(L, -2, 1);
        lua_pushinteger(L, (lua_Integer)it.data);
        lua_rawseti(L, -2, 2);
        lua_rawseti(L, -2, ++k);
    }
    return 1;
}

bool EntityTransform(unsigned long ptr, float out[12]);  // определение ниже
/// EVBotsEmTransform(ptr) -> lx,ly,lz, ux,uy,uz, fx,fy,fz, x,y,z | nil — живой трансформ сущности
int l_em_transform(lua_State* L) {
    float m[12] = {};
    if (!EntityTransform((unsigned long)luaL_checkinteger(L, 1), m)) return 0;
    for (int i = 0; i < 12; ++i) lua_pushnumber(L, (lua_Number)m[i]);
    return 12;
}

/// EVBotsEmEvUnwatchAll() — смена уровня: сущности старого уровня больше не наблюдаем
int l_em_ev_unwatch_all(lua_State* L) {
    (void)L;
    InterlockedExchange(&g_evWatchedN, 0);
    AcquireSRWLockExclusive(&g_evRingLock);
    InterlockedIncrement(&g_evEpoch);
    g_evRead = InterlockedCompareExchange(&g_evWrite, 0, 0);
    ReleaseSRWLockExclusive(&g_evRingLock);
    return 0;
}

/// EVBotsEmEvPoll() -> { {ptr, id}, ... }
int l_em_ev_poll(lua_State* L) {
    EvItem pending[256];
    int count = 0;
    AcquireSRWLockExclusive(&g_evRingLock);
    const LONG w = InterlockedCompareExchange(&g_evWrite, 0, 0);
    if (w - g_evRead > 256) g_evRead = w - 256;
    while (g_evRead < w) pending[count++] = g_evRing[g_evRead++ & 255];
    ReleaseSRWLockExclusive(&g_evRingLock);
    lua_newtable(L);
    int k = 0;
    for (int i = 0; i < count; ++i) {
        const EvItem it = pending[i];
        lua_createtable(L, 4, 0);
        lua_pushinteger(L, (lua_Integer)it.ent);
        lua_rawseti(L, -2, 1);
        lua_pushinteger(L, (lua_Integer)(int)it.id);  // знаковый int32, как MathUtils:FNVHash
        lua_rawseti(L, -2, 2);
        lua_pushboolean(L, it.baseDestroyed ? 1 : 0);
        lua_rawseti(L, -2, 3);
        lua_pushboolean(L, it.finalBase ? 1 : 0);
        lua_rawseti(L, -2, 4);
        lua_rawseti(L, -2, ++k);
    }
    return 1;
}

/// EVBotsEmDataGuid(entityPtr) -> "XXXXXXXX-…" | nil — GUID данных сущности ([e+0x14] — DataContainer)
int l_em_data_guid(lua_State* L) {
    const unsigned long ent = (unsigned long)luaL_checkinteger(L, 1);
    unsigned long dc = 0;
    __try {
        dc = *(const unsigned long*)(ULONG_PTR)(ent + 0x14);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        dc = 0;
    }
    eng::Guid16 g;
    if (dc == 0 || !eng::InstanceGuid(dc, &g)) { lua_pushnil(L); return 1; }
    char text[40] = {};
    eng::FormatGuid(g, text);
    lua_pushstring(L, text);
    return 1;
}

// Указатели сущностей типа (до cap). 0 — движка нет или мира нет.
// Сборка 125 (ред. 2): прогон 125 — штатный итератор (begin 0x005CF340 / next 0x005CF2F0), вызванный из потока Lua,
// вернул 0 флагов, хотя они есть (чтение памяти снаружи: 5 точек с vtable 0x02696660). Поэтому — чистое чтение той же
// таблицы, по алгоритму итератора (разбор агента, re\capture): idx = u16[TI+0x14] - u16[мир+0x834];
// сегмент = [[мир+0x820] + idx*8]; массив [seg+0, seg+4), следующий — [seg+0x18].
// +0x1C — только активная часть: отключённые точки Royale тоже нужны GetIterator.
int ListEntities(const TypeRec& t, unsigned long* out, int cap) {
    unsigned char* base = EngineBase();
    if (base == nullptr) return 0;
    int n = 0;
    __try {
        const unsigned char* world = *reinterpret_cast<unsigned char* const*>(base + (kEntityWorldVa - kImageBase));
        if (world == nullptr) return 0;
        const unsigned long tb = *(const unsigned long*)(world + 0x820);
        const unsigned long te = *(const unsigned long*)(world + 0x824);
        const unsigned int baseId = *(const unsigned short*)(world + 0x834);
        const unsigned int rid = *(const unsigned short*)(base + (t.typeInfoVa - kImageBase) + 0x14);
        if (tb == 0 || te <= tb || rid < baseId) return 0;
        const unsigned long idx = rid - baseId;
        if (idx >= (te - tb) / 8) return 0;
        const unsigned long vt = t.vtableVa ? (unsigned long)(ULONG_PTR)(base + (t.vtableVa - kImageBase)) : 0;
        const unsigned char* seg = *(const unsigned char* const*)(ULONG_PTR)(tb + idx * 8);
        for (int segGuard = 0; seg != nullptr && segGuard < 64 && n < cap; ++segGuard) {
            const unsigned long begin = *(const unsigned long*)seg;
            const unsigned long end = *(const unsigned long*)(seg + 4);
            if ((begin & 3) != 0 || end < begin || ((end - begin) & 3) != 0 ||
                (end - begin) / 4 > 65536 || (end != begin && begin == 0)) return n;
            const unsigned long cnt = (end - begin) / 4;
            const unsigned long* arr = (const unsigned long*)(ULONG_PTR)begin;
            for (unsigned long i = 0; arr != nullptr && i < cnt && i < 4096 && n < cap; ++i) {
                const unsigned long e = arr[i];
                if (e == 0) continue;
                if (*(const unsigned long*)(ULONG_PTR)(e + 8) & 0x80000UL) continue;
                if (vt != 0 && *(const unsigned long*)(ULONG_PTR)e != vt) continue;
                out[n++] = e;
            }
            seg = *(const unsigned char* const*)(seg + 0x18);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return n;
    }
    return n;
}

// LinearTransform сущности: left, up, forward, trans (по 3 float). false — нет трансформа.
bool EntityTransform(unsigned long ptr, float out[12]) {
    __try {
        const unsigned char* e = (const unsigned char*)(ULONG_PTR)ptr;
        const unsigned long fl = *(const unsigned long*)(e + 0x08);
        const unsigned char* t = *(const unsigned char* const*)(e + 0x1C);
        if (t == nullptr) return false;
        if (fl & (1UL << 18)) t = t + (unsigned long)(*(t + 6)) * 0x50 + 0x10;
        for (int r = 0; r < 4; ++r) {
            const float* row = (const float*)(t + r * 0x10);
            out[r * 3 + 0] = row[0];
            out[r * 3 + 1] = row[1];
            out[r * 3 + 2] = row[2];
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void PushTransform(lua_State* L, unsigned long ptr) {
    float m[12] = {};
    if (!EntityTransform(ptr, m)) return;
    static const char* const kKeys[12] = {"lx", "ly", "lz", "ux", "uy", "uz", "fx", "fy", "fz", "x", "y", "z"};
    for (int i = 0; i < 12; ++i) {
        lua_pushnumber(L, (lua_Number)m[i]);
        lua_setfield(L, -2, kKeys[i]);
    }
}

/// emList(typeName) -> { ptr, ... } (пусто — тип не поддержан или движка нет)
int l_em_list(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    lua_newtable(L);
    for (const TypeRec& t : kTypes) {
        if (::strcmp(t.name, name) != 0) continue;
        unsigned long ptrs[512];
        const int n = ListEntities(t, ptrs, 512);
        for (int i = 0; i < n; ++i) {
            lua_pushinteger(L, (lua_Integer)ptrs[i]);
            lua_rawseti(L, -2, i + 1);
        }
        break;
    }
    return 1;
}

// ═══════════════════════════════════════════════════════════════════════════
// ★ Сборка 264 (Б25): настоящее имя CapturePoint, как VU FUN_10E06C60.
// VU обходит link-свойство CP.Spawners (0xECEA613A), фильтрует SpawnReferenceObjectData, ищет среди результатов
// ServerCharacterSpawnEntity и берёт его runtime LocationNameSid (0x00E44F00 → 0x02351500, хеш 0x58D1C621).
// Разбор: re/capture-point-name-2026-11-09.md; сверка ABI/TLS/cleanup: re/capture-name-integration-review-2026-11-09.md.
// Все VA сверены по bf3_server_Final.exe (SHA-256 f39f7623…); в рантайме — сверка прологов, иначе ни одного вызова.
// Скрэтч-арена меняет static PE TLS (fs:[0x2C] → [0x02B6AA74] → block+0x118), поэтому вызов только в потоке кадра
// сервера, при готовом уровне и читаемой TLS-цепочке. Cleanup — в нативном порядке и при исключении.
// ═══════════════════════════════════════════════════════════════════════════
constexpr unsigned long kCpArenaCtorVa = 0x00440B90UL;     // thiscall(arena) -> arena; арена 0x18
constexpr unsigned long kCpListCtorVa = 0x00E49200UL;      // thiscall(list, &arenaPtr) -> list; список 0xA48
constexpr unsigned long kCpQueryLinksVa = 0x005E1260UL;    // cdecl(arena, &key8, typeInfo, &busCtx, bus, list)
constexpr unsigned long kCpNodeEntityVa = 0x005CD2D0UL;    // thiscall(node+8, flag) -> Entity*
constexpr unsigned long kCpSpawnNameVa = 0x00E44F00UL;     // thiscall(CharacterSpawnEntity) -> const char** (fb::String)
constexpr unsigned long kCpListClearVa = 0x00E44030UL;     // thiscall(list)
constexpr unsigned long kCpArenaDtorVa = 0x0043F510UL;     // thiscall(arena)
constexpr unsigned long kSpawnReferenceTypeInfoVa = 0x02C02D9CUL;        // SpawnReferenceObjectData
constexpr unsigned long kServerCharSpawnTypeInfoVa = 0x02C16B84UL;       // ServerCharacterSpawnEntity
constexpr unsigned long kSpawnersFieldId = 0xECEA613AUL;                 // CP.Spawners (LinkConnection)
constexpr unsigned long kStaticTlsIndexVa = 0x02B6AA74UL;                // index static PE TLS
constexpr unsigned long kCpVtableVa = 0x02696660UL;                      // ServerCapturePointEntity
constexpr int kCpNameMax = 96;

enum CpNameState { kCpNameDisabled = -1, kCpNamePending = 0, kCpNameOk = 1 };

struct CpNameEntry {
    unsigned long ptr;
    char name[kCpNameMax];
};
CpNameEntry g_cpNameCache[64] = {};
LONG g_cpNameCacheN = 0;
long g_cpNameCacheSeq = -1;
ULONGLONG g_cpNameNextTry = 0;
LONG g_cpNameErrLogged = 0;

struct CpKey8 {
    unsigned long data;
    unsigned long fieldId;
};

using CpArenaCtorFn = void*(__fastcall*)(void*, void*);
using CpListCtorFn = void*(__fastcall*)(void*, void*, void*);
using CpQueryLinksFn = void(__cdecl*)(void*, void*, void*, void*, void*, void*);
using CpNodeEntityFn = void*(__fastcall*)(void*, void*, unsigned long);
using CpSpawnNameFn = const char* const*(__fastcall*)(void*, void*);
using CpListClearFn = void(__fastcall*)(void*, void*);
using CpArenaDtorFn = void(__fastcall*)(void*, void*);
using CpBusCtxFn = void(__fastcall*)(void*, void*, void*);

// Прологи — capstone по bf3_server_Final.exe f39f7623… (см. re/mode-*). Совпали все — можно звать; нет — выключаем.
struct CpCodeSig {
    unsigned long va;
    const unsigned char* bytes;
    size_t n;
};
const unsigned char kSigArenaCtor[] = {0x56, 0x8B, 0xF1, 0x57, 0xC7, 0x46, 0x04, 0x01, 0x00, 0x00, 0x00,
                                       0xC7, 0x46, 0x08, 0x00, 0x00, 0x00, 0x00, 0xC7, 0x06, 0xBC, 0xB9, 0x43, 0x02};
const unsigned char kSigListCtor[] = {0x83, 0xEC, 0x18, 0x8B, 0x44, 0x24, 0x1C, 0x53, 0x55, 0x56, 0x57, 0x33, 0xDB,
                                      0x53, 0x6A, 0x04, 0x8B, 0xF1, 0x8B, 0x08, 0x6A, 0x28, 0x68, 0x00, 0x0A, 0x00, 0x00};
const unsigned char kSigQueryLinks[] = {0x83, 0xEC, 0x18, 0x8B, 0x4C, 0x24, 0x20, 0x8B, 0x01, 0x8B, 0x49, 0x04, 0x53,
                                        0x8B, 0x5C, 0x24, 0x2C, 0x3B, 0x43, 0x08, 0x89, 0x44, 0x24, 0x10, 0x8B, 0x03};
const unsigned char kSigNodeEntity[] = {0x83, 0x79, 0x0C, 0x00, 0x75, 0x05, 0x33, 0xC0, 0xC2, 0x04, 0x00, 0x8B,
                                        0x41, 0x1C, 0x8B, 0x49, 0x0C, 0x8B, 0x11, 0x56, 0x8B, 0x74, 0x24, 0x08};
const unsigned char kSigSpawnName[] = {0x83, 0xEC, 0x14, 0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x10, 0x8B, 0x01, 0x8B,
                                       0x40, 0x30, 0x8D, 0x54, 0x24, 0x0C, 0x52, 0xFF, 0xD0};
const unsigned char kSigListClear[] = {0x53, 0x56, 0x8B, 0xF1, 0x8B, 0x1E, 0x3B, 0xDE, 0x74, 0x38, 0x57, 0xEB, 0x03,
                                       0x8D, 0x49, 0x00};
const unsigned char kSigArenaDtor[] = {0x56, 0x8B, 0xF1, 0xC7, 0x06, 0xBC, 0xB9, 0x43, 0x02, 0x8B, 0x46, 0x14,
                                       0x8B, 0x4E, 0x0C, 0x50, 0x51, 0xE8, 0xCA, 0xEE, 0xFF, 0xFF};
const CpCodeSig kCpCodeSigs[] = {
    {kCpArenaCtorVa, kSigArenaCtor, sizeof(kSigArenaCtor)},
    {kCpListCtorVa, kSigListCtor, sizeof(kSigListCtor)},
    {kCpQueryLinksVa, kSigQueryLinks, sizeof(kSigQueryLinks)},
    {kCpNodeEntityVa, kSigNodeEntity, sizeof(kSigNodeEntity)},
    {kCpSpawnNameVa, kSigSpawnName, sizeof(kSigSpawnName)},
    {kCpListClearVa, kSigListClear, sizeof(kSigListClear)},
    {kCpArenaDtorVa, kSigArenaDtor, sizeof(kSigArenaDtor)},
};

bool CpNameCodeOk(unsigned char* base) {
    for (const CpCodeSig& s : kCpCodeSigs) {
        const unsigned char* p = base + (s.va - kImageBase);
        __try {
            if (::memcmp(p, s.bytes, s.n) != 0) return false;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    return true;
}

bool CpNameTlsReady(unsigned char* base) {
    unsigned char* vec = reinterpret_cast<unsigned char*>(__readfsdword(0x2C));
    if (vec == nullptr) return false;
    unsigned long idx = 0;
    __try {
        idx = *(const unsigned long*)(base + (kStaticTlsIndexVa - kImageBase));
        if (idx > 4096) return false;
        unsigned char* block = *(unsigned char* const*)(vec + idx * 4);
        if (block == nullptr) return false;
        (void)*(volatile const unsigned long*)(block + 0x118);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return true;
}

void CpNameCacheSyncSeq() {
    const long seq = EVBotsLevelLoadedSeq();
    if (seq != g_cpNameCacheSeq) {
        g_cpNameCacheSeq = seq;
        g_cpNameCacheN = 0;
    }
}

const char* CpNameCacheFind(unsigned long cp) {
    const LONG n = g_cpNameCacheN;
    for (LONG i = 0; i < n; ++i) {
        if (g_cpNameCache[i].ptr == cp) return g_cpNameCache[i].name;
    }
    return nullptr;
}

void CpNameCacheAdd(unsigned long cp, const char* name) {
    if (g_cpNameCacheN >= 64) return;
    CpNameEntry& e = g_cpNameCache[g_cpNameCacheN++];
    e.ptr = cp;
    int i = 0;
    for (; i < kCpNameMax - 1 && name[i] != '\0'; ++i) e.name[i] = name[i];
    e.name[i] = '\0';
}

void CpNameClearSafe(CpListClearFn fn, void* list) {
    __try { fn(list, nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) { }
}

/// 1 — имя получено (может быть пустым), 0 — ещё не готово, -1 — выключено/адреса не совпали.
int CpNativeName(unsigned long cp, char* out, size_t cap) {
    if (out == nullptr || cap == 0) return kCpNameDisabled;
    out[0] = '\0';
    char env[4] = {};
    if (::GetEnvironmentVariableA("EVBOTS_CP_NAME", env, (DWORD)sizeof(env)) == 1 && env[0] == '0') {
        return kCpNameDisabled;
    }
    unsigned char* base = EngineBase();
    if (base == nullptr) return kCpNamePending;
    if (!CpNameCodeOk(base)) return kCpNameDisabled;
    // Скрэтч-арена трогает static TLS потока: только поток кадра сервера и только при готовом уровне.
    if (!EVBotsServerFrameTickActive() || !EVBotsOnServerFrameThread() || !EngineLevelReady()) {
        return kCpNamePending;
    }
    CpNameCacheSyncSeq();
    if (const char* hit = CpNameCacheFind(cp)) {
        int i = 0;
        for (; i < static_cast<int>(cap) - 1 && hit[i] != '\0'; ++i) out[i] = hit[i];
        out[i] = '\0';
        return kCpNameOk;
    }
    if (!CpNameTlsReady(base)) return kCpNamePending;
    const ULONGLONG now = ::GetTickCount64();
    if (now < g_cpNameNextTry) return kCpNamePending;

    unsigned long bus = 0, data = 0;
    __try {
        if (*(const unsigned long*)(ULONG_PTR)cp !=
            (unsigned long)(ULONG_PTR)(base + (kCpVtableVa - kImageBase))) {
            return kCpNamePending;
        }
        bus = *(const unsigned long*)(ULONG_PTR)(cp + 0x10);
        data = *(const unsigned long*)(ULONG_PTR)(cp + 0x14);
        if (bus == 0 || data == 0) return kCpNamePending;
        const unsigned long vt = *(const unsigned long*)(ULONG_PTR)bus;
        if (vt < (unsigned long)(ULONG_PTR)base ||
            vt >= (unsigned long)(ULONG_PTR)(base + 0x02900000UL)) {
            return kCpNamePending;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kCpNamePending;
    }

    const ULONG_PTR b = (ULONG_PTR)base;
    CpArenaCtorFn arenaCtor = reinterpret_cast<CpArenaCtorFn>(b + (kCpArenaCtorVa - kImageBase));
    CpListCtorFn listCtor = reinterpret_cast<CpListCtorFn>(b + (kCpListCtorVa - kImageBase));
    CpQueryLinksFn queryLinks = reinterpret_cast<CpQueryLinksFn>(b + (kCpQueryLinksVa - kImageBase));
    CpNodeEntityFn nodeEntity = reinterpret_cast<CpNodeEntityFn>(b + (kCpNodeEntityVa - kImageBase));
    CpSpawnNameFn spawnName = reinterpret_cast<CpSpawnNameFn>(b + (kCpSpawnNameVa - kImageBase));
    CpListClearFn listClear = reinterpret_cast<CpListClearFn>(b + (kCpListClearVa - kImageBase));
    CpArenaDtorFn arenaDtor = reinterpret_cast<CpArenaDtorFn>(b + (kCpArenaDtorVa - kImageBase));

    alignas(16) unsigned char arena[0x18] = {};
    alignas(16) unsigned char list[0xA48] = {};
    alignas(8) unsigned char busCtx[16] = {};
    CpKey8 key = {data, kSpawnersFieldId};
    const unsigned long typeInfo = (unsigned long)(ULONG_PTR)(base + (kSpawnReferenceTypeInfoVa - kImageBase));

    bool arenaOk = false, listOk = false, got = false;
    char tmp[kCpNameMax] = {};
    __try {
        arenaCtor(arena, nullptr);
        arenaOk = true;
        void* arenaPtr = arena;
        listCtor(list, nullptr, &arenaPtr);
        listOk = true;
        const unsigned long busVt = *(const unsigned long*)(ULONG_PTR)bus;
        CpBusCtxFn busCtxFn = reinterpret_cast<CpBusCtxFn>(*(const unsigned long*)(ULONG_PTR)(busVt + 0x30));
        busCtxFn((void*)(ULONG_PTR)bus, nullptr, busCtx);
        queryLinks(arena, &key, (void*)(ULONG_PTR)typeInfo, busCtx, (void*)(ULONG_PTR)bus, list);
        const unsigned long listAddr = (unsigned long)(ULONG_PTR)list;
        unsigned long node = *(const unsigned long*)(ULONG_PTR)list;
        for (int i = 0; i < 4096 && node != 0 && node != listAddr; ++i) {
            const unsigned long next = *(const unsigned long*)(ULONG_PTR)node;
            void* ent = nodeEntity((void*)(ULONG_PTR)(node + 8), nullptr, 1UL);
            if (ent != nullptr &&
                TypeOfNoCall((unsigned long)(ULONG_PTR)ent) ==
                    (unsigned long)(ULONG_PTR)(base + (kServerCharSpawnTypeInfoVa - kImageBase))) {
                // VU: пустой getter результата — продолжаем обход; ненулевой адрес fb::String (даже с пустой
                // строкой) — конец поиска. Успех пустой строки не равен «имени нет».
                const char* const* pStr = spawnName(ent, nullptr);
                if (pStr != nullptr) {
                    const char* s = *pStr;
                    if (s != nullptr) {
                        int k = 0;
                        for (; k < kCpNameMax - 1 && s[k] != '\0'; ++k) tmp[k] = s[k];
                        tmp[k] = '\0';
                        got = true;
                        break;
                    }
                }
            }
            node = next;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_cpNameErrLogged < 4) {
            ++g_cpNameErrLogged;
            LOG_ERR("EVBots: имя CapturePoint %08lX — вызов движка сорвался (cleanup выполняется)", cp);
        }
        got = false;
    }

    if (listOk) CpNameClearSafe(listClear, list);
    if (listOk) {
        *(unsigned long*)(ULONG_PTR)list = (unsigned long)(ULONG_PTR)list;
        *(unsigned long*)(ULONG_PTR)(list + 4) = (unsigned long)(ULONG_PTR)list;
    }
    if (arenaOk) {
        __try { arenaDtor(arena, nullptr); } __except (EXCEPTION_EXECUTE_HANDLER) { }
    }
    if (listOk) CpNameClearSafe(listClear, list);

    if (!got) {
        g_cpNameNextTry = now + 1000;  // pending: не дёргаем 100-КБ арену каждый опрос
        return kCpNamePending;
    }
    CpNameCacheAdd(cp, tmp);
    int i = 0;
    for (; i < static_cast<int>(cap) - 1 && tmp[i] != '\0'; ++i) out[i] = tmp[i];
    out[i] = '\0';
    return kCpNameOk;
}

/// emCp(ptr, wantName) -> { team, flagLocation, ..., bus, name, nameNative, x.. } | nil
int l_em_cp(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    const bool wantName = lua_isnoneornil(L, 2) || lua_toboolean(L, 2) != 0;
    int team = 0, capType = 0;
    float flag = 0.0f;
    unsigned char ctrl = 0, capEn = 0, att = 0;
    unsigned long bus = 0;
    __try {
        const unsigned char* e = (const unsigned char*)(ULONG_PTR)ptr;
        team = *(const int*)(e + 0xB0);
        flag = *(const float*)(e + 0xC8);
        ctrl = *(e + 0xD4);
        capEn = *(e + 0xD5);
        att = *(e + 0xE6);
        bus = *(const unsigned long*)(e + 0x10);
        const unsigned char* data = *(const unsigned char* const*)(e + 0x14);
        if (data != nullptr) capType = *(const int*)(data + 0xA8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    lua_pushinteger(L, team);
    lua_setfield(L, -2, "team");
    lua_pushnumber(L, (lua_Number)flag);
    lua_setfield(L, -2, "flagLocation");
    lua_pushboolean(L, ctrl != 0);
    lua_setfield(L, -2, "isControlled");
    lua_pushboolean(L, capEn != 0);
    lua_setfield(L, -2, "isCaptureEnabled");
    lua_pushboolean(L, att != 0);
    lua_setfield(L, -2, "isAttacked");
    lua_pushinteger(L, capType);
    lua_setfield(L, -2, "capturableType");
    lua_pushinteger(L, (lua_Integer)bus);
    lua_setfield(L, -2, "bus");
    char name[kCpNameMax] = {};
    const int nameState = wantName ? CpNativeName(ptr, name, sizeof(name)) : kCpNameDisabled;
    lua_pushstring(L, name);
    lua_setfield(L, -2, "name");
    lua_pushboolean(L, nameState == kCpNameOk ? 1 : 0);
    lua_setfield(L, -2, "nameNative");
    PushTransform(L, ptr);
    return 1;
}

/// emSpawn(ptr) -> { team, bus, x.. } | nil
int l_em_spawn(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    int team = 0;
    unsigned long bus = 0, data = 0;
    __try {
        const unsigned char* e = (const unsigned char*)(ULONG_PTR)ptr;
        bus = *(const unsigned long*)(e + 0x10);
        data = *(const unsigned long*)(e + 0x14);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lua_pushnil(L);
        return 1;
    }
    // Команда [данные+0x128] — только у данных точки спавна персонажа (CharacterSpawnReferenceObjectData). У
    // ServerVehicleSpawnEntity (сборка 201) данные другого типа — команду отдаёт рефлексия (data.team), здесь не читаем её.
    __try {
        if (data != 0) team = *(const int*)(ULONG_PTR)(data + 0x128);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        team = 0;
    }
    lua_newtable(L);
    lua_pushinteger(L, team);
    lua_setfield(L, -2, "team");
    lua_pushinteger(L, (lua_Integer)bus);
    lua_setfield(L, -2, "bus");
    lua_pushinteger(L, (lua_Integer)data);
    lua_setfield(L, -2, "data");
    PushTransform(L, ptr);
    return 1;
}

// ★ Сборка 178 (техника): ServerVehicleEntity. Поля — из разбора re\vehicle\NOTES.md и пробника 159: места — массив
// указателей [+0xFC..+0x100) (по 4 байта), игрок в месте — [место+0x4C]; данные (VehicleEntityData) — [+0x14].
/// Указатель — живая ServerVehicleEntity (vtable совпадает).
bool IsVehicleEntity(unsigned long ptr) {
    unsigned char* base = EngineBase();
    if (base == nullptr || ptr == 0) return false;
    __try {
        return *(const unsigned long*)(ULONG_PTR)ptr == (unsigned long)(ULONG_PTR)(base + (0x025BF8E8UL - kImageBase));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/// ★ Сборка 211: emVspawnCtl(ptr) -> { машина, ... } — выпущенные спавнером техники машины (VU SpawnEntity.spawnedControllables).
/// Вектор [спавнер+0xCA8 .. +0xCAC) указателей; движок берёт из него текущую машину (0x00E3E1D0: последний элемент), её же
/// проверяет canSpawnInVehicle 0x00DD8F90. Только живые ServerVehicleEntity (vtable), не больше 16.
int l_em_vspawn_ctl(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    unsigned long out[16] = {};
    int n = 0;
    __try {
        const unsigned char* e = (const unsigned char*)(ULONG_PTR)ptr;
        const unsigned long b = *(const unsigned long*)(e + 0xCA8);
        const unsigned long en = *(const unsigned long*)(e + 0xCAC);
        if (b != 0 && en >= b && en - b <= 4UL * 64UL) {
            for (unsigned long a = b; a < en && n < 16; a += 4) {
                const unsigned long v = *(const unsigned long*)(ULONG_PTR)a;
                if (v != 0 && IsVehicleEntity(v)) out[n++] = v;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lua_pushnil(L);
        return 1;
    }
    lua_createtable(L, n, 0);
    for (int i = 0; i < n; ++i) {
        lua_pushinteger(L, (lua_Integer)out[i]);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

/// emIsVeh(ptr) -> bool
int l_em_is_veh(lua_State* L) {
    lua_pushboolean(L, IsVehicleEntity((unsigned long)luaL_checkinteger(L, 1)) ? 1 : 0);
    return 1;
}

/// emVeh(ptr) -> { seats = { игрок_места0, игрок_места1, ... } (0 — свободно), data = ptr данных, x.. трансформ } | nil
int l_em_veh(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    if (!IsVehicleEntity(ptr)) {
        lua_pushnil(L);
        return 1;
    }
    unsigned long seats[16] = {};
    int nSeats = 0;
    unsigned long data = 0;
    __try {
        const unsigned char* e = (const unsigned char*)(ULONG_PTR)ptr;
        data = *(const unsigned long*)(e + 0x14);
        const unsigned long b = *(const unsigned long*)(e + 0xFC);
        const unsigned long en = *(const unsigned long*)(e + 0x100);
        if (b != 0 && en >= b && en - b <= 4UL * 16UL) {
            nSeats = (int)((en - b) / 4UL);
            for (int k = 0; k < nSeats; ++k) {
                const unsigned long s = *(const unsigned long*)(ULONG_PTR)(b + 4UL * (unsigned long)k);
                seats[k] = (s != 0) ? *(const unsigned long*)(ULONG_PTR)(s + 0x4C) : 0;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    lua_newtable(L);
    for (int k = 0; k < nSeats; ++k) {
        lua_pushinteger(L, (lua_Integer)seats[k]);
        lua_rawseti(L, -2, k + 1);
    }
    lua_setfield(L, -2, "seats");
    lua_pushinteger(L, nSeats);
    lua_setfield(L, -2, "seatCount");
    lua_pushinteger(L, (lua_Integer)data);
    lua_setfield(L, -2, "data");
    PushTransform(L, ptr);
    return 1;
}

// ★ Сборка 182: части машины (башни, пушки) — как VU PhysicsEntityBase.partCount / GetPartTransform. Разбор (16.10, veniceext
// 0x10D1F840/0x10D1FAB0 + живая память 181): у VU physicsEntityBase = vtbl[+0xD0] сущности, partCount = vtbl[+0x9C],
// GetPartTransform = vtbl[+0xAC]; в bf3_server_Final.exe те же слоты сдвинуты на +8: vtbl ServerVehicleEntity [+0xD8] =
// 0x00DE94A0 «mov eax,[ecx+0x144]» → physicsEntityBase; у него [+0xA4] = 0x0065ECF0 «mov eax,[ecx+0x84]» (число частей),
// [+0xB4] = 0x0065FF90 → 0x0065E150: часть = [[peb+0x8C] + 4*i]; нет части — трансформ сущности; иначе vtbl части [+0x28] =
// 0x00842770 — копия 64 байт из [часть+8] + (byte[[часть+8]+6]+1)*0x50 + byte[часть+0xC]*0x40 (строки left/up/forward/trans
// по 16 байт, как трансформ сущности). Живой замер LAV-25: 59 частей, части 10/25 (VehicleData мода) — орты, позиция у машины.
// Вызовов движка нет — только чтение под __try; другая функция трансформа у части — отказ (nil → корпус).
constexpr unsigned long kVehPebGetterVa = 0x00DE94A0UL;  // vtbl[+0xD8] ServerVehicleEntity
constexpr unsigned long kPartTransformVa = 0x00842770UL; // vtbl[+0x28] части физики
static unsigned long VehPhysicsBase(unsigned long veh) {
    unsigned char* base = EngineBase();
    if (base == nullptr || !IsVehicleEntity(veh)) return 0;
    __try {
        const unsigned long vt = *(const unsigned long*)(ULONG_PTR)veh;
        const unsigned long getter = *(const unsigned long*)(ULONG_PTR)(vt + 0xD8);
        if (getter != (unsigned long)(ULONG_PTR)(base + (kVehPebGetterVa - kImageBase))) return 0;
        return *(const unsigned long*)(ULONG_PTR)(veh + 0x144);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

/// emPartCount(ptr) -> число частей физики машины (0 — нет)
int l_em_part_count(lua_State* L) {
    const unsigned long peb = VehPhysicsBase((unsigned long)luaL_checkinteger(L, 1));
    unsigned long n = 0;
    if (peb != 0) {
        __try {
            n = *(const unsigned long*)(ULONG_PTR)(peb + 0x84);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            n = 0;
        }
    }
    if (n > 512) n = 0;
    lua_pushinteger(L, (lua_Integer)n);
    return 1;
}

/// emPart(ptr, i) -> { lx..fz, x, y, z } мировой трансформ части i | nil (нет части / не проверенная функция)
int l_em_part(lua_State* L) {
    const unsigned long peb = VehPhysicsBase((unsigned long)luaL_checkinteger(L, 1));
    const lua_Integer idx = luaL_checkinteger(L, 2);
    unsigned char* base = EngineBase();
    float m[12] = {};
    bool ok = false;
    if (peb != 0 && base != nullptr && idx >= 0) {
        __try {
            const unsigned long n = *(const unsigned long*)(ULONG_PTR)(peb + 0x84);
            const unsigned long arr = *(const unsigned long*)(ULONG_PTR)(peb + 0x8C);
            if ((unsigned long)idx < n && n <= 512 && arr != 0) {
                const unsigned long part = *(const unsigned long*)(ULONG_PTR)(arr + 4UL * (unsigned long)idx);
                if (part != 0 && part != peb) {
                    const unsigned long vt = *(const unsigned long*)(ULONG_PTR)part;
                    const unsigned long fn = *(const unsigned long*)(ULONG_PTR)(vt + 0x28);
                    if (fn == (unsigned long)(ULONG_PTR)(base + (kPartTransformVa - kImageBase))) {
                        const unsigned long edx = *(const unsigned long*)(ULONG_PTR)(part + 8);
                        const unsigned long b6 = *(const unsigned char*)(ULONG_PTR)(edx + 6);
                        const unsigned long bc = *(const unsigned char*)(ULONG_PTR)(part + 0xC);
                        const unsigned char* src = (const unsigned char*)(ULONG_PTR)(edx + (b6 + 1UL) * 0x50UL + bc * 0x40UL);
                        for (int r = 0; r < 4; ++r) {
                            const float* row = (const float*)(src + r * 0x10);
                            m[r * 3 + 0] = row[0];
                            m[r * 3 + 1] = row[1];
                            m[r * 3 + 2] = row[2];
                        }
                        ok = true;
                    }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    // Проверка: оси — единичной длины (иначе читаем не то — лучше корпус, чем мусор)
    if (ok) {
        for (int r = 0; r < 3 && ok; ++r) {
            const float l2 = m[r * 3] * m[r * 3] + m[r * 3 + 1] * m[r * 3 + 1] + m[r * 3 + 2] * m[r * 3 + 2];
            if (!(l2 > 0.8f && l2 < 1.2f)) ok = false;
        }
    }
    if (!ok) {
        lua_pushnil(L);
        return 1;
    }
    static const char* const kKeys[12] = {"lx", "ly", "lz", "ux", "uy", "uz", "fx", "fy", "fz", "x", "y", "z"};
    lua_createtable(L, 0, 12);
    for (int i = 0; i < 12; ++i) {
        lua_pushnumber(L, (lua_Number)m[i]);
        lua_setfield(L, -2, kKeys[i]);
    }
    return 1;
}

// ★ Сборка 183: здоровье машины — как VU PhysicsEntity.internalHealth (veniceext 0x10CC5010 = vtbl[+0xD4]; у нас +8:
// vtbl ServerVehicleEntity [+0xDC] = 0x00DE9640 «mov ecx,[ecx+0x234]; jmp 0x008A3100»). 0x008A3100: записи по 16 байт в
// [комп+0x14..+0x18), [запись+0] → float максимум, [запись+4] float текущее; результат = наибольший максимум × наименьшая
// доля текущее/максимум (записи с максимумом 0 пропускаются; нет ни одной — 0). Живой замер 182: Водник 900/900, TOW 150,
// подбитая машина 214/1000. Только чтение под __try; getter не совпал — nil.
constexpr unsigned long kVehHealthGetterVa = 0x00DE9640UL;
static bool VehHealthRead(unsigned long veh, float* out) {
    unsigned char* base = EngineBase();
    bool ok = false;
    float hp = 0.0f;
    if (base != nullptr && IsVehicleEntity(veh)) {
        __try {
            const unsigned long vt = *(const unsigned long*)(ULONG_PTR)veh;
            const unsigned long getter = *(const unsigned long*)(ULONG_PTR)(vt + 0xDC);
            if (getter == (unsigned long)(ULONG_PTR)(base + (kVehHealthGetterVa - kImageBase))) {
                const unsigned long comp = *(const unsigned long*)(ULONG_PTR)(veh + 0x234);
                const unsigned long b = *(const unsigned long*)(ULONG_PTR)(comp + 0x14);
                const unsigned long e = *(const unsigned long*)(ULONG_PTR)(comp + 0x18);
                if (comp != 0 && e >= b && e - b <= 0x10UL * 64UL) {
                    float maxMax = -3.4e38f, minRatio = 3.4e38f;
                    bool any = false;
                    for (unsigned long r = b; r < e; r += 0x10UL) {
                        const unsigned long pm = *(const unsigned long*)(ULONG_PTR)r;
                        if (pm == 0) continue;
                        const float mx = *(const float*)(ULONG_PTR)pm;
                        if (mx == 0.0f) continue;
                        const float cur = *(const float*)(ULONG_PTR)(r + 4);
                        if (mx > maxMax) maxMax = mx;
                        const float ratio = cur / mx;
                        if (ratio < minRatio) minRatio = ratio;
                        any = true;
                    }
                    hp = any ? maxMax * minRatio : 0.0f;
                    ok = true;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    *out = hp;
    return ok;
}

/// emVehVel(ptr) -> vx, vy, vz | nil (сборка 201). Скорость машины из физики движка, как VU (PhysicsEntity.velocity):
/// Vec3 [[[машина+0x144]+0xA0]+0x1B0] (physicsEntityBase → тело → линейная скорость). Факт 19.10 на живом сервере (re\peek/// peekvehvel3.py): 19 совпадений с производной позиции, 6 расхождений — самолёт в вираже (модуль скорости сходится: 88,5 и 88,9).
/// Не конечное или |v| >= 400 — nil (Lua берёт производную позиции). EVBOTS_VEL_ENGINE=0 — всегда nil.
int l_em_veh_vel(lua_State* L) {
    static const bool s_on = []() {
        char b[8] = {0};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_VEL_ENGINE", b, (DWORD)sizeof(b));
        return !(n > 0 && n < sizeof(b) && b[0] == '0');
    }();
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    float v[3] = {0.0f, 0.0f, 0.0f};
    bool ok = false;
    if (s_on && ptr != 0 && IsVehicleEntity(ptr)) {
        __try {
            const unsigned long peb = *(const unsigned long*)(ULONG_PTR)(ptr + 0x144);
            const unsigned long body = peb != 0 ? *(const unsigned long*)(ULONG_PTR)(peb + 0xA0) : 0;
            if (body != 0) {
                const float* f = (const float*)(ULONG_PTR)(body + 0x1B0);
                v[0] = f[0]; v[1] = f[1]; v[2] = f[2];
                const float m2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
                ok = m2 == m2 && m2 < 400.0f * 400.0f;  // NaN даёт false; inf отсекает предел
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
    }
    if (!ok) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, (lua_Number)v[0]);
    lua_pushnumber(L, (lua_Number)v[1]);
    lua_pushnumber(L, (lua_Number)v[2]);
    return 3;
}

/// emVehHealth(ptr) -> здоровье | nil
int l_em_veh_health(lua_State* L) {
    float hp = 0.0f;
    if (!VehHealthRead((unsigned long)luaL_checkinteger(L, 1), &hp)) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushnumber(L, (lua_Number)hp);
    return 1;
}

// ★ Сборка 184: урон технике со стрелявшим — как VU Vehicle:Damage(vehicle, damage, damageGiverInfo). Разбор (exe, 16.10):
// сообщение ServerVehicleDamageMessage (тип 0x936D3098, конструктор 0x02308A00: [+0x28] машина, [+0x2C] ссылка на нанёсшего,
// [+0x30] изменение здоровья, [+0x34] float) отправляет только 0x012BCB80; её зовёт лишь 0x012BE900 — слот +0x14 vtable
// 0x0268733C (интерфейс слушателя урона): thiscall(сущность, a2, a3, float a4), ret 0x10; проверяет, что сущность — техника
// (тип в [0x02C170A4..0x02C170A6]), и передаёт всё в 0x012BCB80: a3 → [+0x2C] (слушатели 0x012DBD52/0x01323C02 берут
// нанёсшего как [[a3]] − 8 через 0x00DD1DE0), [a2+0x40] → [+0x34], a4 − здоровье → [+0x30]. Подмена указателя в слоте
// 0x02687350 (правило 18: слот должен содержать 0x012BE900, иначе не ставим; EVBOTS_VEH_DMG_HOOK=0 — выкл.).
// Перехват только запоминает и зовёт оригинал; Lua забирает записи (emVehDamagePop) и шлёт событие.
constexpr unsigned long kVehDmgSlotVa = 0x02687350UL;
constexpr unsigned long kVehDmgOrigVa = 0x012BE900UL;
struct VehDmgRec { unsigned long veh; unsigned long giver; float a4; float hp; float dmg40; };
static VehDmgRec g_vehDmg[64];
static volatile LONG g_vehDmgHead = 0;  // пишет движок
static LONG g_vehDmgTail = 0;            // читает Lua (под Gate)
static volatile LONG g_vehDmgOrig = 0;
static volatile LONG g_vehDmgInstalled = 0;  // 1 — стоит, -1 — нет
static volatile LONG g_vehDmgHits = 0;

static void __fastcall VehDamageHook(void* self, void* /*edx*/, unsigned long ent, unsigned long a2, unsigned long a3, float a4) {
    InterlockedIncrement(&g_vehDmgHits);
    VehDmgRec r = {};
    r.veh = ent;
    r.a4 = a4;
    __try {
        if (a2 != 0) r.dmg40 = *(const float*)(ULONG_PTR)(a2 + 0x40);
        if (a3 != 0) {
            const unsigned long h = *(const unsigned long*)(ULONG_PTR)a3;
            if (h != 0) {
                const unsigned long o = *(const unsigned long*)(ULONG_PTR)h;
                if (o != 0) r.giver = o - 8;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (!VehHealthRead(ent, &r.hp)) r.hp = -1.0f;
    const LONG slot = (InterlockedIncrement(&g_vehDmgHead) - 1) & 63;
    g_vehDmg[slot] = r;
    using Fn = void(__fastcall*)(void*, void*, unsigned long, unsigned long, unsigned long, float);
    ((Fn)(ULONG_PTR)InterlockedCompareExchange(&g_vehDmgOrig, 0, 0))(self, nullptr, ent, a2, a3, a4);
}

static bool InstallVehDamageHook() {
    const LONG st = InterlockedCompareExchange(&g_vehDmgInstalled, 0, 0);
    if (st != 0) return st > 0;
    char e[4] = {};
    const DWORD en = ::GetEnvironmentVariableA("EVBOTS_VEH_DMG_HOOK", e, (DWORD)sizeof(e));
    unsigned char* base = EngineBase();
    if ((en == 1 && e[0] == '0') || base == nullptr) {
        InterlockedExchange(&g_vehDmgInstalled, -1);
        LOG_INFO("EVBots: урон техники — перехват выключен (EVBOTS_VEH_DMG_HOOK=0 или нет модуля)");
        return false;
    }
    unsigned long* slot = (unsigned long*)(base + (kVehDmgSlotVa - kImageBase));
    const unsigned long expect = (unsigned long)(ULONG_PTR)(base + (kVehDmgOrigVa - kImageBase));
    unsigned long cur = 0;
    __try {
        cur = *slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        cur = 0;
    }
    if (cur != expect) {
        InterlockedExchange(&g_vehDmgInstalled, -1);
        LOG_ERR("EVBots: урон техники: в слоте %08lX, ждали %08lX — перехват НЕ ставлю", cur, expect);
        return false;
    }
    InterlockedExchange(&g_vehDmgOrig, (LONG)cur);
    // сборка 237: замена слота — ядром (ev-core), со сверкой прежнего значения
    if (!PatchPointer((void*)slot, (void*)&VehDamageHook, (void*)(ULONG_PTR)cur, nullptr, "урон техники (vtable)")) {
        InterlockedExchange(&g_vehDmgInstalled, -1);
        LOG_ERR("EVBots: урон техники: ядро не заменило слот");
        return false;
    }
    InterlockedExchange(&g_vehDmgInstalled, 1);
    LOG_INFO("EVBots: ★ урон техники перехвачен (слот %p: %08lX -> %p) — Vehicle:Damage со стрелявшим",
             (void*)slot, cur, (void*)&VehDamageHook);
    return true;
}

/// emVehTeleport(ptr, lx,ly,lz, ux,uy,uz, fx,fy,fz, x,y,z) -> true — запрос переноса машины (выполнит тик моста, сборка 185)
int l_em_veh_teleport(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    float m[12];
    for (int i = 0; i < 12; ++i) m[i] = (float)luaL_checknumber(L, i + 2);
    lua_pushboolean(L, ::evbots::EVBotsQueueVehicleTeleport(ptr, m) ? 1 : 0);
    return 1;
}

/// emVehDamagePop() -> { {veh, giver, a4, hp, dmg40}, ... } новые записи перехвата | nil, если перехвата нет
int l_em_veh_damage_pop(lua_State* L) {
    if (!InstallVehDamageHook()) {
        lua_pushnil(L);
        return 1;
    }
    const LONG head = InterlockedCompareExchange(&g_vehDmgHead, 0, 0);
    if (head - g_vehDmgTail > 64) g_vehDmgTail = head - 64;  // переполнение: старые потеряны
    lua_newtable(L);
    int k = 0;
    while (g_vehDmgTail < head) {
        const VehDmgRec r = g_vehDmg[g_vehDmgTail & 63];
        ++g_vehDmgTail;
        lua_createtable(L, 0, 5);
        lua_pushinteger(L, (lua_Integer)r.veh);
        lua_setfield(L, -2, "veh");
        lua_pushinteger(L, (lua_Integer)r.giver);
        lua_setfield(L, -2, "giver");
        lua_pushnumber(L, (lua_Number)r.a4);
        lua_setfield(L, -2, "a4");
        lua_pushnumber(L, (lua_Number)r.hp);
        lua_setfield(L, -2, "hp");
        lua_pushnumber(L, (lua_Number)r.dmg40);
        lua_setfield(L, -2, "dmg40");
        lua_rawseti(L, -2, ++k);
    }
    return 1;
}

/// emData(ptr) -> живой DataContainer инстанса движка | nil
int l_em_data(lua_State* L) {
    const unsigned long ptr = (unsigned long)luaL_checkinteger(L, 1);
    EVBotsPushLiveInstance(L, ptr);
    return 1;
}

constexpr char kEntityManagerSource[] = R"LUA(
local log, emList, emCp, emSpawn, emIsVeh, emVeh, emData, emPartCount, emPart, emVehHealth, emVehDamagePop, emVehTeleport, emVehVel = ...

local KIND_CP = "ServerCapturePointEntity"
local KIND_SPAWN = "ServerCharacterSpawnEntity"
local KIND_VEH = "ServerVehicleEntity"
local KIND_VSPAWN = "ServerVehicleSpawnEntity"  -- сборка 201

local function zeroGuid()
  local ok, g = pcall(Guid, "00000000-0000-0000-0000-000000000000")
  return ok and g or nil
end

local function makeLT(s)
  if s == nil or s.x == nil then return LinearTransform() end
  return LinearTransform(Vec3._make(s.lx, s.ly, s.lz), Vec3._make(s.ux, s.uy, s.uz),
      Vec3._make(s.fx, s.fy, s.fz), Vec3._make(s.x, s.y, s.z))
end

-- ═══════════════════════════════════════════════════════════════════════════
-- Данные сущности (DataContainer): Is() и поля, которые читает мод
-- ═══════════════════════════════════════════════════════════════════════════
local DataObj = {}
DataObj.__index = DataObj
function DataObj:Is(typeName)
  return typeName == self._type or typeName == "DataContainer"
end

-- ═══════════════════════════════════════════════════════════════════════════
-- Entity — обёртка над сущностью движка
-- ═══════════════════════════════════════════════════════════════════════════
local Entity = {}
Entity.__index = Entity

-- Entity(raw): наша обёртка — как есть; иначе пустая (как раньше в заглушке). Отдельная метатаблица для вызова:
-- setmetatable(Entity, Entity) зациклил бы __index на отсутствующих полях.
setmetatable(Entity, { __call = function(_, raw)
  if type(raw) == "table" and raw._kind ~= nil then return raw end
  local instance = setmetatable({}, Entity)
  instance.data = setmetatable({ instanceGuid = zeroGuid(), _type = "DataContainer" }, DataObj)
  instance._raw = raw
  return instance
end })

function Entity:GetData() return self.data end

function Entity:Is(typeName)
  local k = self._kind
  if k == nil then return false end
  if typeName == k or typeName == "Entity" then return true end
  if k == KIND_CP and typeName == "CapturePointEntity" then return true end
  if k == KIND_SPAWN and (typeName == "CharacterSpawnEntity" or typeName == "SpawnEntity") then return true end
  if k == KIND_VSPAWN and (typeName == "VehicleSpawnEntity" or typeName == "SpawnEntity") then return true end
  if k == KIND_VEH and (typeName == "ControllableEntity" or typeName == "PhysicsEntity" or typeName == "SpatialEntity"
      or typeName == "GameEntity" or typeName == "ServerControllableEntity") then return true end
  return false
end

-- ★ Сборка 185: ServerPlayerEvent и Entity:FireEvent("Spawn") — способ спавна мода в TDM/SDM/GunMaster/AirSuperiority и
-- спавн в самолёт на XP5 (BotSpawner:_TriggerSpawn, BotSpawner.lua:971–1063, 1355). До 185 FireEvent был заглушкой, а
-- ServerPlayerEvent не было вовсе: в этих режимах ботов не сажали (ошибка в обработчике мода). В VU событие «Spawn» на
-- ServerCharacterSpawnEntity сажает игрока на эту точку движком в том же кадре. У нас: CreateSoldier + SpawnSoldierAt с
-- указателем точки — мост сажает спавном этой точки (CharacterSpawnEntity::spawn 0x00DDBBD0) с выбором мода и оружием.
if rawget(_G, "ServerPlayerEvent") == nil then
  _G.ServerPlayerEvent = function(name, player, ...)
    return { eventName = name, player = player, args = { ... }, typeInfo = { name = "ServerPlayerEvent" } }
  end
end
if rawget(_G, "SoldierWeapon") == nil then
  _G.SoldierWeapon = function(w) return w end
end
local fireLogged, spawnEvLogged = false, 0
function Entity:FireEvent(ev, ...)
  if type(ev) == "table" and ev.eventName == "Spawn" and rawget(self, "_kind") == KIND_SPAWN and ev.player ~= nil then
    local p = ev.player
    local ok, err = pcall(function()
      local tr = self.transform
      local lt = LinearTransform(Vec3._make(1, 0, 0), Vec3._make(0, 1, 0), Vec3._make(0, 0, 1),
        Vec3._make(tr.trans.x, tr.trans.y, tr.trans.z))
      local s = p:CreateSoldier(p.selectedKit, lt)
      -- 4-й аргумент: сажать через спавн этой точки (движок сам сажает в привязанную технику — самолёт на XP5)
      p:SpawnSoldierAt(s, lt, CharacterPoseType.CharacterPoseType_Stand, self._ptr)
      -- ★ Сборка 201 (прогон XP5_002 AirSuperiority: 537 ошибок `Vehicles.lua:51`, боты не стреляли): у VU после Spawn на точке
      -- самолёта игрок уже в машине (синхронно), и мод в следующем тике берёт `controlledControllable` как самолёт
      -- (BotSpawner:_GetSpecialSpawnEnity → _EnterVehicleEntity → VehicleEntityData(entity.data)). У нас движок сажает в тике, а до
      -- того controlledControllable был солдатом без `data` → ошибка обрывала Engine:Update. Находим самолёт этой точки (дочерний
      -- ServerVehicleSpawnEntity на той же шине → ближайшая машина) и отдаём его сразу (_vehPending, как в Player:EnterVehicle).
      local vsp = nil
      for _, ch in ipairs(self.bus and self.bus.entities or {}) do
        if ch._kind == KIND_VSPAWN then vsp = ch break end
      end
      -- Сборка 201: мост отдаст солдата моду только после посадки движком (VextPlayerManager.cpp, _spawnVehPoint).
      -- Флаг — по указателю игрока движка: объект игрока из события мода может быть не той же таблицей, что в списке моста.
      if vsp ~= nil then
        local np = p.nativePtr
        if np ~= nil and np ~= 0 then EVBotsSpawnVehPoint = EVBotsSpawnVehPoint or {}; EVBotsSpawnVehPoint[np] = true end
      end
      if vsp ~= nil and EntityManager ~= nil then
        local vt = vsp.transform and vsp.transform.trans or tr.trans
        local best, bestD = nil, 40.0
        local it = EntityManager:GetIterator(KIND_VEH)
        local v = it:Next()
        while v ~= nil do
          local vp = v.transform and v.transform.trans
          if vp ~= nil and v:GetPlayerInEntry(0) == nil then
            local d = math.sqrt((vp.x - vt.x) ^ 2 + (vp.y - vt.y) ^ 2 + (vp.z - vt.z) ^ 2)
            if d < bestD then best, bestD = v, d end
          end
          v = it:Next()
        end
        if best ~= nil then
          -- engineSeats (сборка 235): сажает сам спавн точки, не очередь моста — по очереди не снимать
          rawset(p, '_vehPending', { kind = 1, ent = best, seat = 0, t = os.clock(), engineSeats = true })
          log("INFO ", string.format("FireEvent Spawn: %s — самолёт точки %s (%.1f м) отдан как controlledControllable сразу", tostring(p.name),
            tostring(best.name), bestD))
        else
          -- Нормально для AirSuperiority: самолёт создаёт сам спавн точки (движок), его ещё нет — ждём посадку в мосте.
          log("DBG  ", "FireEvent Spawn: " .. tostring(p.name) .. " — машины точки ещё нет, ждём посадку движком")
          -- ★ Сборка 212 (прогон 23.10 XP5_002 CQL: 52 из 52 спавнов в самолёт кончились Kill и ошибкой BotSpawner.lua:1567).
          -- SpawnSoldierAt отдаёт объект солдата сразу (138), и на следующем тике мод (BotSpawner.lua:179, `soldier ~= nil`)
          -- брал controlledControllable — пешком это сам солдат — «не техника» → _EnterVehicleEntity ≠ 0 → Kill. У VU после
          -- Spawn на такой точке игрок уже в самолёте: движок создаёт его в том же спавне (0x00DDBBD0 → 0x00DD9000 → vtbl+0x6C
          -- спавнера). Пока движок не посадил, солдата моду не показываем (мод ждёт, всё под `soldier ~= nil`); отдаёт мост
          -- (Engine:Update, EVBotsSpawnVehPoint), когда есть обёртка машины.
          if rawget(p, 'pendingSoldier') ~= nil then
            rawset(p, 'soldier', nil)
            log("INFO ", "FireEvent Spawn: " .. tostring(p.name) .. " — солдат моду после посадки движком в машину точки (как VU)")
          end
        end
      end
    end)
    if spawnEvLogged < 20 then
      spawnEvLogged = spawnEvLogged + 1
      log("INFO ", string.format("FireEvent Spawn (как VU): %s на точке %08X (%.0f, %.0f) %s", tostring(p.name), self._ptr or 0,
        (self.transform and self.transform.trans.x) or 0, (self.transform and self.transform.trans.z) or 0,
        ok and "— посадка заказана" or ("ошибка " .. tostring(err))))
    end
    return
  end
  if not fireLogged then
    fireLogged = true
    log("INFO ", "Entity:FireEvent — событие " .. tostring(type(ev) == "table" and ev.eventName or ev) .. " не поддержано (тип "
      .. tostring(self._kind) .. ")")
  end
end
-- ★ Сборка 186: Entity:RegisterEventCallback(callback) / (context, callback), как VU: callback(entity, event), event.eventId —
-- хеш движка имени события (MathUtils:FNVHash). События снимает перехват onEvent (EVBotsEmEvWatch), разбор — в _Poll.
local evCallbacks = {}  -- ptr -> { {ctx, hasCtx, cb}, ... }
local genericEntityRef  -- сборка 186: задаётся ниже (genericEntity)
local evLogged = 0
function Entity:RegisterEventCallback(...)
  local n = select('#', ...)
  local ctx, cb
  if n >= 2 then ctx, cb = ... else cb = (...) end
  local ptr = rawget(self, '_ptr')
  if type(cb) ~= 'function' or ptr == nil or EVBotsEmEvWatch == nil then return nil end
  local ok, res, why = pcall(EVBotsEmEvWatch, ptr)
  if not ok or not res then
    log("WRN  ", string.format("RegisterEventCallback %08X (%s): не поставлен — %s", ptr, tostring(self._kind), tostring(why or res)))
    return nil
  end
  local list = evCallbacks[ptr] or {}
  evCallbacks[ptr] = list
  list[#list + 1] = { ctx = ctx, hasCtx = n >= 2, cb = cb, ent = self }
  log("INFO ", string.format("RegisterEventCallback %08X (%s, данные %s): обработчиков %d", ptr, tostring(self._kind),
    tostring(self.data and self.data.instanceGuid), #list))
  return #list
end

-- ★ Сборка 186: события Rush. Имена по хешу движка (djb2-xor, как MathUtils:FNVHash) — для лога и LifeCounter:BaseDestroyed.
local function engineHash(t)
  local h = 5381
  for i = 1, #t do h = ((h * 33) % 4294967296) ~ string.byte(t, i) end
  if h >= 2147483648 then h = h - 4294967296 end
  return h
end
local EV_NAMES = {}
for _, n in ipairs({ "BaseDestroyed", "OnBaseDestroyed", "FinalBaseDestroyed", "OnFinalBaseDestroyed", "Destroyed", "OnDestroyed",
    "Destroy", "Armed", "OnArmed", "Disarmed", "OnDisarmed", "Arm", "Disarm", "Exploded", "OnExploded", "SetTrue", "SetFalse",
    "Toggle", "Activate", "Deactivate", "Enable", "Disable", "Trigger", "Reset", "Start", "Stop", "LifeLost", "OnLifeLost",
    "LifeCountChanged", "OnLifeCountChanged", "Decrease", "Increase", "Kill", "OnKill", "Spawn", "OnSpawn", "Captured", "Lost",
    "OnCaptured", "OnLost", "Win", "Lose", "RoundOver", "Next", "Advance", "Show", "Hide", "In", "Out", "Enter", "Exit",
    "DestructBase", "IncreaseReinforce" }) do
  EV_NAMES[engineHash(n)] = n
end
local RUSH_WATCH = { ServerLifeCounterEntity = true, ServerObjectiveEntity = true, ServerSyncedBoolEntity = true }
local rushWatched, rushScanT, rushLogged = {}, 5.0, 0
local rushStage = 0
local kindOf = {}
local function rushScan(dt)
  rushScanT = rushScanT + dt
  if rushScanT < 5.0 or EVBotsEmEvWatch == nil then return end
  rushScanT = 0.0
  for kind in pairs(RUSH_WATCH) do
    local ok, list = pcall(emList, kind)
    if ok and type(list) == "table" then
      for _, ptr in ipairs(list) do
        if not rushWatched[ptr] then
          local okw, res, why = pcall(EVBotsEmEvWatch, ptr)
          if okw and res then
            rushWatched[ptr] = true
            kindOf[ptr] = kind
            log("INFO ", string.format("ДИАГ события Rush: наблюдаю %s %08X (данные %s)", kind, ptr,
              tostring(EVBotsEmDataGuid and select(2, pcall(EVBotsEmDataGuid, ptr)))))
          end
        end
      end
    end
  end
end
local function rushEvent(ptr, id, baseDestroyed, finalBase)
  local kind = kindOf[ptr]
  if kind == nil then return end
  local nm = EV_NAMES[id] or string.format("#%08X", id % 4294967296)
  if rushLogged < 200 then
    rushLogged = rushLogged + 1
    log("INFO ", string.format("ДИАГ события Rush: %s %08X — событие %s", kind, ptr, nm))
  end
  -- LifeCounter:BaseDestroyed(lifeCounterEntity, finalBase), как VU: этап Rush пройден (мод ведёт стадии по нему)
  -- Принятие DestructBase и final сняты после оригинального onEvent. IncreaseReinforce не меняет сектор.
  if kind == "ServerLifeCounterEntity" and baseDestroyed and Events ~= nil and Events._DispatchEngine ~= nil then
    rushStage = rushStage + 1
    log("INFO ", string.format("LifeCounter:BaseDestroyed — этап %d пройден (%s), final=%s", rushStage, nm, tostring(finalBase)))
    pcall(Events._DispatchEngine, Events, "LifeCounter:BaseDestroyed", genericEntityRef(kind, ptr), finalBase == true)
  end
end

local function pollEntityEvents()
  if EVBotsEmEvPoll == nil then return end
  local ok, list = pcall(EVBotsEmEvPoll)
  if not ok or type(list) ~= 'table' then return end
  for _, it in ipairs(list) do
    pcall(rushEvent, it[1], it[2], it[3], it[4])
    local cbs = evCallbacks[it[1]]
    if cbs ~= nil then
      local ev = { eventId = it[2], type = it[2] }
      if evLogged < 40 then
        evLogged = evLogged + 1
        log("INFO ", string.format("событие сущности %08X: eventId %d — обработчиков %d", it[1], it[2], #cbs))
      end
      for i = 1, #cbs do
        local c = cbs[i]
        local okc, err
        if c.hasCtx then okc, err = pcall(c.cb, c.ctx, c.ent, ev) else okc, err = pcall(c.cb, c.ent, ev) end
        if not okc then log("ERR  ", "обработчик события сущности: " .. tostring(err)) end
      end
    end
  end
end

_G.Entity = Entity

-- Кэш обёрток по указателю
local cache = {}
local cpCount = 0

local spawnList, spawnListAt = nil, -1
local vspawnList, busCache = nil, {}
local clock = 0.0
local function busOf(e)
  -- bus.entities — все сущности шины (сам спавн, его ServerVehicleSpawnEntity и т.д.), как у VU (сборка 201).
  return setmetatable({}, { __index = function(_, k)
    if k == "entities" then return EVBotsBusEntities(e._bus) end
    return nil
  end })
end
-- Точки спавна техники (сборка 201): обёртка с живыми данными (data.blueprint.name, data.team — рефлексия движка)
local function vspawnEntities()
  if vspawnList ~= nil and clock - spawnListAt < 2.0 then return vspawnList end
  vspawnList = {}
  local ok, list = pcall(emList, KIND_VSPAWN)
  if ok and type(list) == "table" then
    for _, ptr in ipairs(list) do
      local e = cache[ptr]
      if e == nil then
        local okS, s = pcall(emSpawn, ptr)
        if okS and s ~= nil then
          e = setmetatable({ _kind = KIND_VSPAWN, _ptr = ptr, uniqueId = ptr, instanceId = ptr, _bus = s.bus,
            transform = makeLT(s) }, Entity)
          local live = nil
          if s.data ~= nil and s.data ~= 0 then
            local okD, d = pcall(emData, s.data)
            if okD then live = d end
          end
          -- Нет живых данных — заглушка с пустым именем чертежа (мод сравнит с ожидаемым и пропустит точку).
          e.data = live or setmetatable({ instanceGuid = zeroGuid(), _type = "VehicleSpawnReferenceObjectData",
              team = 0, blueprint = { name = "" } }, DataObj)
          e.bus = busOf(e)
          cache[ptr] = e
        end
      end
      if e ~= nil then vspawnList[#vspawnList + 1] = e end
    end
  end
  return vspawnList
end
local function spawnEntities()
  if spawnList ~= nil and clock - spawnListAt < 2.0 then return spawnList end
  spawnList, spawnListAt = {}, clock
  vspawnList, busCache = nil, {}
  local ok, list = pcall(emList, KIND_SPAWN)
  if ok and type(list) == "table" then
    for _, ptr in ipairs(list) do
      local e = cache[ptr]
      if e == nil then
        local okS, s = pcall(emSpawn, ptr)
        if okS and s ~= nil then
          e = setmetatable({ _kind = KIND_SPAWN, _ptr = ptr, uniqueId = ptr, instanceId = ptr, _bus = s.bus,
            transform = makeLT(s) }, Entity)
          e.bus = busOf(e)
          e.data = setmetatable({ instanceGuid = zeroGuid(), _type = "CharacterSpawnReferenceObjectData", team = s.team },
              DataObj)
          cache[ptr] = e
        end
      end
      if e ~= nil then spawnList[#spawnList + 1] = e end
    end
  end
  return spawnList
end

function EVBotsBusEntities(busPtr)
  local out = {}
  if busPtr == nil or busPtr == 0 then return out end
  local hit = busCache[busPtr]
  if hit ~= nil and clock - spawnListAt < 2.0 then return hit end
  for _, s in ipairs(spawnEntities()) do
    if s._bus == busPtr then out[#out + 1] = s end
  end
  for _, v in ipairs(vspawnEntities()) do
    if v._bus == busPtr then out[#out + 1] = v end
  end
  busCache[busPtr] = out
  return out
end
local function busEntities(busPtr) return EVBotsBusEntities(busPtr) end

local function refreshCp(e)
  local ok, s = pcall(emCp, e._ptr, not e._nameReady)
  if not ok or s == nil then return e end
  -- ★ Сборка 264 (Б25): настоящее имя, как VU (CP.Spawners -> CharacterSpawn -> LocationNameSid). Пока native не готов
  -- (нет нужного TLS/потока/уровня), остаётся стабильный синтетический ключ EVBOTS_CP#[_HQ]; pending не кэшируем.
  if s.nameNative then
    e._nameReady = true
    if s.name ~= nil and s.name ~= "" then
      if not e._nameLogged and s.name ~= e.name then
        e._nameLogged = true
        log("INFO ", "CAPTUREPOINT name: " .. tostring(e.name) .. " -> " .. s.name)
      end
      e.name = s.name
    end
  end
  e.team = s.team
  e.flagLocation = s.flagLocation
  e.isControlled = s.isControlled
  e.isCaptureEnabled = s.isCaptureEnabled
  e.isAttacked = s.isAttacked
  e.transform = makeLT(s)
  e._bus = s.bus
  e.data.capturableType = s.capturableType
  return e
end

local function cpEntity(ptr)
  local e = cache[ptr]
  if e ~= nil then return e end
  local ok, s = pcall(emCp, ptr)
  if not ok or s == nil then return nil end
  cpCount = cpCount + 1
  local name = (s.capturableType == 1) and ("EVBOTS_CP" .. cpCount .. "_HQ") or ("EVBOTS_CP" .. cpCount)
  e = setmetatable({ _kind = KIND_CP, _ptr = ptr, uniqueId = ptr, instanceId = ptr, name = name,
      _nameReady = false }, Entity)
  e.data = setmetatable({ instanceGuid = zeroGuid(), _type = "CapturePointEntityData" }, DataObj)
  e.bus = setmetatable({}, { __index = function(_, k)
    if k == "entities" then return busEntities(e._bus) end
    return nil
  end })
  cache[ptr] = e
  return refreshCp(e)
end

-- ═══════════════════════════════════════════════════════════════════════════
-- Техника (сборка 178): ServerVehicleEntity
-- Поля живые: трансформ, места и седоки читаются из движка при каждом обращении. Скорость — по смещению позиции (в
-- серверном классе её смещение ещё не найдено), здоровье — заглушка 100 (смещение не найдено, мод только сравнивает).
-- ═══════════════════════════════════════════════════════════════════════════
local function playerByPtr(ptr)
  if ptr == nil or ptr == 0 or PlayerManager == nil then return nil end
  local list = PlayerManager:GetPlayers()
  for i = 1, #list do
    local p = list[i]
    if rawget(p, "nativePtr") == ptr then return p end
  end
  if NativeBridge ~= nil and NativeBridge.RealPlayer ~= nil then
    local okR, rp = pcall(NativeBridge.RealPlayer)
    if okR and rp == ptr then
      for i = 1, #list do
        local p = list[i]
        local np = rawget(p, "nativePtr")
        if (np == nil or np == 0) and (rawget(p, "onlineId") or 0) ~= 0 then return p end
      end
    end
  end
  return nil
end

local vehCache, vehState = {}, {}
local Vehicle = {}
local VehMt = {}

-- ★ Сборка 179: оси техники — как в движке (VU отдаёт трансформ сущности без перестановки). Нос = forward (замер 178:
-- |cos(движение, forward)| = 0,94–1,00). Мод для наземной техники тоже считает носом forward (VehicleMovement.lua:385,
-- курс atan(forward.z, forward.x) − π/2), а -ToEuler().x = atan(f.x, f.z) с ним совпадает. Перестановка 178 (left' = нос,
-- по строке 375 — это ганшип, у него пушки бортом) сдвигала курс на 90°: руль в упоре, газ 0,5, «застрял» → задний ход.
local function makeVehLT(s)
  return makeLT(s)
end

local function vehRead(e)
  local ok, s = pcall(emVeh, e._ptr)
  if ok then return s end
  return nil
end

local function vehPhysics(e)
  local ph = rawget(e, "_phys")
  if ph ~= nil then return ph end
  -- Сборка 182: partCount и GetPartTransform — из физики машины в движке (emPartCount/emPart, разбор VU — см. l_em_part).
  ph = setmetatable({}, { __index = function(_, k)
    if k == "velocity" then return e.velocity end
    if k == "internalHealth" then return e.internalHealth end
    if k == "position" then return e.transform.trans end
    if k == "partCount" then
      local okC, n = pcall(emPartCount, e._ptr)
      return okC and n or 0
    end
    return nil
  end })
  function ph:GetPart(j) return nil end
  function ph:GetPartTransform(j)
    if type(j) == "number" and j >= 0 then
      local okP, s = pcall(emPart, e._ptr, math.floor(j))
      if okP and s ~= nil then
        local plt = makeLT(s)
        local okQ, q = pcall(plt.ToQuatTransform, plt, false)
        if okQ then
          EVBotsVehPartReads = (EVBotsVehPartReads or 0) + 1
          return q
        end
      end
    end
    EVBotsVehPartMiss = (EVBotsVehPartMiss or 0) + 1
    local lt = e.transform
    local ok, q = pcall(lt.ToQuatTransform, lt, false)
    if ok then return q end
    return nil
  end
  e._phys = ph
  return ph
end

VehMt.__index = function(t, k)
  local m = Vehicle[k]
  if m ~= nil then return m end
  if k == "transform" then
    local s = vehRead(t)
    if s ~= nil and s.x ~= nil then
      t._lastTransform = makeVehLT(s)
    end
    return t._lastTransform or LinearTransform()
  elseif k == "entryCount" then
    local s = vehRead(t)
    return s and s.seatCount or 0
  elseif k == "velocity" then
    -- ★ Сборка 201: скорость из физики движка, как VU; производная позиции ниже — запас (нет данных / отключено).
    if emVehVel ~= nil then
      local okV, vx, vy, vz = pcall(emVehVel, t._ptr)
      if okV and vx ~= nil then return Vec3._make(vx, vy, vz) end
    end
    local s = vehRead(t)
    if s ~= nil and s.x ~= nil then
      local prev = rawget(t, "_vp")
      if prev == nil then
        t._vp = { s.x, s.y, s.z, clock }
        t._vel = Vec3._make(0, 0, 0)
      else
        local dt = clock - prev[4]
        if dt >= 0.1 then
          local dx, dy, dz = s.x - prev[1], s.y - prev[2], s.z - prev[3]
          if dx * dx + dy * dy + dz * dz > 40000.0 then
            t._vel = Vec3._make(0, 0, 0)  -- телепорт/появление: скорость не считаем
          else
            t._vel = Vec3._make(dx / dt, dy / dt, dz / dt)
          end
          prev[1], prev[2], prev[3], prev[4] = s.x, s.y, s.z, clock
        end
      end
    end
    return t._vel or Vec3._make(0, 0, 0)
  elseif k == "internalHealth" then
    -- Сборка 183: здоровье из движка (emVehHealth; мод выходит из подбитой машины по нему, Bot.lua:318). Нет данных — 100.
    local okH, hp = pcall(emVehHealth, t._ptr)
    if okH and hp ~= nil then return hp end
    return 100.0
  elseif k == "physicsEntityBase" then
    return vehPhysics(t)
  elseif k == "partCount" then
    return 0
  elseif k == "typeInfo" then
    return { name = KIND_VEH }
  end
  return Entity[k]
end
-- ★ Сборка 179: запись transform (мод телепортирует застрявшую машину, VehicleMovement.lua:173). Без __newindex поле ложилось
-- в таблицу, __index больше не вызывался — мод навсегда видел старую позицию/курс («застрял» по кругу).
-- ★ Сборка 185: перенос в движке, как VU — SpatialEntity::setTransform машины в тике (emVehTeleport, VextPlayerManager.cpp).
VehMt.__newindex = function(t, k, v)
  if k == "transform" then
    EVBotsVehTeleportStub = (EVBotsVehTeleportStub or 0) + 1
    local ok = false
    if emVehTeleport ~= nil and (type(v) == "table" or type(v) == "userdata") then
      local okc, r = pcall(function()
        local l, u, f, tr = v.left, v.up, v.forward, v.trans
        return emVehTeleport(t._ptr, l.x, l.y, l.z, u.x, u.y, u.z, f.x, f.y, f.z, tr.x, tr.y, tr.z)
      end)
      ok = okc and r == true
    end
    if not ok and rawget(t, "_tpLogged") == nil then
      rawset(t, "_tpLogged", true)
      log("WRN  ", string.format("техника %08X: перенос (vehicle.transform) не поставлен в очередь", t._ptr or 0))
    end
    return
  end
  rawset(t, k, v)
end

function Vehicle:GetPlayerInEntry(i)
  if type(i) ~= "number" or i < 0 then return nil end
  local s = vehRead(self)
  if s == nil then return nil end
  local pp = s.seats[i + 1]
  if pp == nil or pp == 0 then
    -- сборка 186 (прогон 21:06): у VU EnterVehicle синхронный — место занято сразу. У нас посадка ждёт тика моста, и два
    -- бота в начале раунда садились на одно место пилота; второй стоял у вертолёта в состоянии «пилотирую», мод убивал его
    -- через 50 с (GameDirector.lua:217; BOT_Luca, Leon, Niffper1289, Foudroyant). Пока посадка в очереди — место занято.
    if NativeBridge ~= nil and NativeBridge.VehSeatQueued ~= nil then
      local okq, qp = pcall(NativeBridge.VehSeatQueued, self._ptr, i)
      if okq and qp ~= nil and qp ~= 0 then return playerByPtr(qp) end
    end
    -- ★ Сборка 244: посадка, заказанная до привязки игрока движка (Player:EnterVehicle → _enterLater), тоже занимает место (до 6 с)
    local sl = EVBotsSeatLater ~= nil and EVBotsSeatLater[self._ptr] or nil
    local e = sl ~= nil and sl[i] or nil
    if e ~= nil then
      if os.clock() - e.t < 6.0 then return e.p end
      sl[i] = nil
    end
    return nil
  end
  return playerByPtr(pp)
end
function Vehicle:GetEntryCount() return self.entryCount end

local function vehEntity(ptr)
  local e = vehCache[ptr]
  if e ~= nil then return e end
  local okV, isV = pcall(emIsVeh, ptr)
  if not okV or not isV then return nil end
  local s = vehRead({ _ptr = ptr })
  if s == nil then return nil end
  e = setmetatable({ _kind = KIND_VEH, _ptr = ptr, uniqueId = ptr, instanceId = ptr, name = "EVBOTS_VEH" .. ptr }, VehMt)
  local okD, d = pcall(emData, s.data)
  if okD and d ~= nil then
    e.data = d
  else
    e.data = setmetatable({ instanceGuid = zeroGuid(), _type = "VehicleEntityData", controllableType = "" }, DataObj)
  end
  vehCache[ptr] = e
  return e
end

-- Для PlayerManager: указатель сущности движка → объект техники (nil, если это не техника).
local function vehicleByPtr(ptr)
  if ptr == nil or ptr == 0 then return nil end
  return vehEntity(ptr)
end

-- Касты VU (только если их ещё нет)
if rawget(_G, "ControllableEntity") == nil then
  _G.ControllableEntity = function(e) return e end
end
do
  -- ★ Сборка 208: как VU (VU-Docs guides\concepts.md:159): «ТипИмя(x)» с объектом из дерева наследования типа — каст,
  -- с чем угодно другим (в т.ч. nil) — НОВЫЙ объект этого типа. Новый VehicleEntityData — controllableType = "".
  -- Раньше мы возвращали x как есть: данные солдата читались как техника — мусорные имена в fun-bots.log
  -- («[Vehicles] WARNING: KP not found», 20.10), а nil давал ошибку Vehicles.lua:51 (65 раз в прогоне 201).
  -- Ставим всегда (перекрываем общий Dc_Cast, если он уже занял имя). Чужие типы — в журнал, до 12 строк.
  local vedOther = 0
  local function isVed(d)
    if d == nil then return false end
    if type(d) == "table" then
      local t = rawget(d, "_type")
      if t == "VehicleEntityData" then return true end
    end
    local ok, r = pcall(function() return d:Is("VehicleEntityData") end)
    return ok and r == true
  end
  _G.VehicleEntityData = function(d)
    if isVed(d) then return d end
    if vedOther < 12 then
      vedOther = vedOther + 1
      pcall(function()
        local info = debug.getinfo(2, "Sl")
        local tn = "nil"
        if d ~= nil then
          local ok, ti = pcall(function() return d.typeInfo end)
          tn = (ok and type(ti) == "table" and tostring(ti.name)) or type(d)
        end
        log("INFO ", "VehicleEntityData(" .. tn .. ") из " .. tostring(info and (info.short_src .. ":" .. info.currentline))
          .. " — не техника: новый пустой объект, как VU")
      end)
    end
    return setmetatable({ instanceGuid = zeroGuid(), _type = "VehicleEntityData", controllableType = "" }, DataObj)
  end
end
if rawget(_G, "CapturePointEntity") == nil then
  _G.CapturePointEntity = function(e)
    if type(e) == "table" and e._kind == KIND_CP then return refreshCp(e) end
    return e
  end
end
-- CharacterSpawnReferenceObjectData регистрирует VextFbTypes: Guid означает
-- создание native DC. Старый identity-cast возвращал сам Guid вместо точки.
-- ★ Сборка 185: каст SpawnEntity (server\type\spawnentity.md). Мод на XP5_002/XP5_004 CQ ищет точку «спавн в самолёт»
-- (BotSpawner:_CheckAndGetAvailableJetSpawn, BotSpawner.lua:1248). Каста не было — каждый Engine:Update мода падал
-- («attempt to call a nil value (global 'SpawnEntity')»), и ни один бот не садился (прогон XP5_002 18.10 13:58).
-- Поля пока из данных точки (команда), остальное — значения свободной точки. ЧАСТИЧНО: дочерних ServerVehicleSpawnEntity
-- в bus.entities у нас ещё нет, поэтому спавн в самолёт мод не находит и сажает ботов обычным путём.
if rawget(_G, "SpawnEntity") == nil then
  _G.SpawnEntity = function(e)
    if type(e) ~= "table" then return e end
    local team = 0
    pcall(function() team = (e.data and e.data.team) or 0 end)
    local se = { teamId = team, enabled = true, spawnTimer = 0, spawnDelay = 0, spawnCount = 0, isControlled = false,
      canSpawnMore = true, driverControlled = false, driverEntryInputValid = false, spawnRequested = false,
      isAutoSpawnCancelled = false, hasBeenInitialAutoSpawned = false, useFriendZones = false,
      alternatePointIndexOrder = {}, locationNameSid = "" }
    -- ★ Сборка 211: у спавнера техники spawnedControllables — живые машины из движка ([спавнер+0xCA8..+0xCAC), EVBotsEmVspawnCtl).
    -- Друг 22.10 (XP5_002/XP5_004): мод (BotSpawner:_CheckAndGetAvailableJetSpawn) с пустым списком всегда считал самолёт
    -- в воздухе свободным и слал ботов на точку, где самолёт уже летает. У остальных точек — пустой список, как было.
    local isVsp = rawget(e, "_kind") == KIND_VSPAWN and rawget(e, "_ptr") ~= nil and EVBotsEmVspawnCtl ~= nil
    if not isVsp then se.spawnedControllables = {} end
    return setmetatable(se, { __index = function(_, k)
      if k == "spawnedControllables" and isVsp then
        local res = {}
        local okc, list = pcall(EVBotsEmVspawnCtl, rawget(e, "_ptr"))
        if okc and type(list) == "table" then
          for _, vp in ipairs(list) do
            local ve = vehEntity(vp)
            if ve ~= nil then res[#res + 1] = ve end
          end
        end
        return res
      end
      return e[k]
    end })
  end
end
if rawget(_G, "VehicleSpawnReferenceObjectData") == nil then
  _G.VehicleSpawnReferenceObjectData = function(d) return d end
end

-- ═══════════════════════════════════════════════════════════════════════════
-- EntityIterator
-- ═══════════════════════════════════════════════════════════════════════════
local EntityIterator = {}
EntityIterator.__index = EntityIterator

function EntityIterator:Next()
  local items = rawget(self, "_items")
  if items == nil then return nil end
  self._i = (self._i or 0) + 1
  return items[self._i]
end

_G.EntityIterator = EntityIterator

-- ═══════════════════════════════════════════════════════════════════════════
-- EntityManager
-- ═══════════════════════════════════════════════════════════════════════════
local EntityManager = {}
EntityManager.__index = EntityManager

local firstLogged = {}
-- ★ Сборка 186: прочие типы из движка (зона Rush, запрет ввода) — обёртка с настоящими данными (instanceGuid)
local GENERIC_KINDS = { ServerSyncedBoolEntity = true, ServerInputRestrictionEntity = true,
  ServerLifeCounterEntity = true, ServerObjectiveEntity = true, ServerSupplySphereEntity = true }
local function genericEntity(kind, ptr)
  local e = cache[ptr]
  if e ~= nil and e._kind == kind then return e end
  e = setmetatable({ _kind = kind, _ptr = ptr, uniqueId = ptr, instanceId = ptr, name = kind .. ptr }, Entity)
  local g = nil
  if EVBotsEmDataGuid ~= nil then
    local okg, txt = pcall(EVBotsEmDataGuid, ptr)
    if okg and txt ~= nil then
      local okG, gg = pcall(Guid, txt)
      if okG then g = gg end
    end
  end
  e.data = setmetatable({ instanceGuid = g or zeroGuid(), _type = (kind:gsub("^Server", "")) .. "Data" }, DataObj)
  cache[ptr] = e
  return e
end
genericEntityRef = function(kind, ptr) return genericEntity(kind, ptr) end
-- GetIterator(entityName) → EntityIterator (флаги и точки спавна — из движка, остальное пусто)
function EntityManager:GetIterator(entityName)
  local items = {}
  if entityName == KIND_CP then
    local ok, list = pcall(emList, KIND_CP)
    if ok and type(list) == "table" then
      for _, ptr in ipairs(list) do
        local e = cpEntity(ptr)
        if e ~= nil then items[#items + 1] = refreshCp(e) end
      end
    end
  elseif entityName == KIND_SPAWN then
    items = spawnEntities()
  elseif entityName == KIND_VSPAWN then
    spawnEntities()
    items = vspawnEntities()
  elseif entityName == KIND_VEH then
    local ok, list = pcall(emList, KIND_VEH)
    if ok and type(list) == "table" then
      for _, ptr in ipairs(list) do
        local e = vehEntity(ptr)
        if e ~= nil then items[#items + 1] = e end
      end
    end
  elseif GENERIC_KINDS[entityName] then
    local ok, list = pcall(emList, entityName)
    if ok and type(list) == "table" then
      for _, ptr in ipairs(list) do items[#items + 1] = genericEntity(entityName, ptr) end
    end
  end
  if not firstLogged[entityName] then
    firstLogged[entityName] = true
    log("INFO ", "GetIterator(" .. tostring(entityName) .. "): сущностей " .. #items)
  end
  return setmetatable({ _items = items, _i = 0 }, EntityIterator)
end

function EntityManager:GetCount(entityName)
  local it = self:GetIterator(entityName)
  return #(rawget(it, "_items") or {})
end

-- FindEntityByName(name) → nil (заглушка)
function EntityManager:FindEntityByName(name)
  return nil
end

function EntityManager:GetEntityName(entity)
  if type(entity) == "table" and entity.name ~= nil then return entity.name end
  return ""
end

-- ★ Сборка 198: EntityCreationParams и EntityManager:CreateEntitiesFromBlueprint (VU-Docs: shared/type/entitycreationparams.md,
-- shared/library/entitymanager.md, две перегрузки: transform или params → EntityBus | nil). Раньше обоих не было: fun-bots на
-- XP4_Quake ConquestLarge0 (DESTROY_OBSTACLES_ON_START, Server/__init__.lua:339–371, 666–686) падал на nil-вызове ДО
-- GameDirector/BotSpawner:OnLevelLoaded — боты на этой карте не стартовали бы. Создание сущностей движка из блупринта пока не
-- разобрано: возвращаем nil, как VU при неудаче (мод это проверяет: `if createdBus == nil then return end`).
if rawget(_G, 'EntityCreationParams') == nil then
  function EntityCreationParams()
    return { transform = LinearTransform(), variationNameHash = 0, networked = false, parentRepresentative = nil }
  end
end

local createBpLogged = 0
function EntityManager:CreateEntitiesFromBlueprint(blueprint, transformOrParams)
  createBpLogged = createBpLogged + 1
  if createBpLogged <= 10 then
    local nm = '?'
    pcall(function() nm = tostring(blueprint and (blueprint.name or blueprint.typeInfo and blueprint.typeInfo.name) or nil) end)
    log("WRN  ", "EntityManager:CreateEntitiesFromBlueprint(" .. nm .. ") — создание из блупринта не реализовано, nil (как VU при неудаче)")
  end
  return nil
end

EntityManager._VehicleByPtr = vehicleByPtr

-- Техника: события Vehicle:SpawnDone / Unspawn / Enter / Exit опросом (в VU их шлёт движок).
local function dispatchVeh(name, ...)
  if Events ~= nil and Events._DispatchEngine ~= nil then
    local ok, err = pcall(Events._DispatchEngine, Events, name, ...)
    if not ok then log("WRN  ", name .. ": " .. tostring(err)) end
  end
end
local vehDiagT, vehListT = 0.0, 0.0
local vehDmgHook = false   -- сборка 184: перехват урона техники стоит — Vehicle:Damage шлёт он, опрос здоровья не шлёт
local vehDmgLogged = 0
local function pollVehicleDamage()
  local okD, recs = pcall(emVehDamagePop)
  if not okD or type(recs) ~= "table" then return end
  vehDmgHook = true
  for _, r in ipairs(recs) do
    local e = vehEntity(r.veh)
    local giver = (r.giver ~= nil and r.giver ~= 0) and playerByPtr(r.giver) or nil
    local dmg = (r.dmg40 ~= nil and r.dmg40 > 0) and r.dmg40 or math.abs((r.a4 or 0) - (r.hp or 0))
    EVBotsVehDamage = (EVBotsVehDamage or 0) + 1
    if giver ~= nil then EVBotsVehDamageGiver = (EVBotsVehDamageGiver or 0) + 1 end
    if vehDmgLogged < 25 then
      vehDmgLogged = vehDmgLogged + 1
      log("INFO ", string.format("урон техники %08X: стрелял %s (%08X) a4=%.1f здоровье=%.1f [a2+0x40]=%.1f → урон %.1f",
          r.veh or 0, giver and tostring(giver.name) or "?", r.giver or 0, r.a4 or 0, r.hp or 0, r.dmg40 or 0, dmg))
    end
    if e ~= nil then
      dispatchVeh("Vehicle:Damage", e, dmg, giver ~= nil and { giver = giver } or nil)
    end
  end
end
local function pollVehicles()
  pollVehicleDamage()
  local ok, list = pcall(emList, KIND_VEH)
  if not ok or type(list) ~= "table" then return end
  vehDiagT = vehDiagT + 0.5
  vehListT = vehListT + 0.5
  if vehListT >= 20.0 then
    -- Сборка 186: диагностика — после смены раунда боты не садились в технику (0 посадок, 0 строк «техника X: …»)
    vehListT = 0.0
    local known = 0
    for _ in pairs(vehState) do known = known + 1 end
    local okS, sup = pcall(emList, "ServerSupplySphereEntity")
    log("INFO ", string.format("ДИАГ список техники: в движке %d, известно мосту %d; аптечек и ящиков патронов на карте %s", #list, known,
      (okS and type(sup) == "table") and tostring(#sup) or "?"))
  end
  local seen = {}
  for _, ptr in ipairs(list) do
    local e = vehEntity(ptr)
    local s = e ~= nil and vehRead(e) or nil
    if e ~= nil and s ~= nil then
      seen[ptr] = true
      local st = vehState[ptr]
      -- Данные движка могут исчезнуть раньше события Unspawn. Сохраняем имя, пока объект ещё читается.
      local okType, vehicleType = pcall(function() return VehicleEntityData(e.data).controllableType end)
      if okType and type(vehicleType) == "string" and vehicleType ~= "" then
        rawset(e, "_lastControllableType", vehicleType)
      end
      -- Знак руля (диагностика 178): если ввод 4 > 0 увеличивает курс atan(нос.z, нос.x) — руль «согласен» с модом
      -- (у мода ввод = −PID(курс − цель)); считаем раз в 0,5 с, только когда машина едет.
      do
        local drv0 = s.seats[1] ~= nil and s.seats[1] ~= 0 and playerByPtr(s.seats[1]) or nil
        if st ~= nil and drv0 ~= nil and type(drv0.input) == "table" and type(drv0.input._levels) == "table" and s.x ~= nil then
          local hd = math.atan(s.fz, s.fx)
          st.steer = st.steer or { ok = 0, bad = 0, n = 0 }
          if st.lastH ~= nil then
            local dh = hd - st.lastH
            if dh > math.pi then dh = dh - 2 * math.pi elseif dh < -math.pi then dh = dh + 2 * math.pi end
            local moved = math.abs(s.x - st.lastX) + math.abs(s.z - st.lastZ)
            local inp = drv0.input._levels[4] or 0
            if moved > 0.3 and math.abs(dh) > 0.01 and math.abs(inp) > 0.05 then
              if (inp > 0) == (dh > 0) then st.steer.ok = st.steer.ok + 1 else st.steer.bad = st.steer.bad + 1 end
            end
          end
          st.lastH, st.lastX, st.lastZ = hd, s.x, s.z
          -- Сборка 179: ошибка курса как у мода (курс = atan(нос.z, нос.x) − π/2, цель — authoritativeAimingYaw, мод пишет туда
          -- _TargetYaw каждый кадр, VehicleMovement.lua:572). Итог — тест veh-drive.
          -- Сборка 184: цель курса — маршрут мода (_TargetYawMovementVehicle бота, VehicleMovement.lua:363), а не прицел
          -- (authoritativeAimingYaw в атаке смотрит на врага — прогон 183: 18 % при нормальной езде). Только в езде (газ > 0).
          local aim = nil
          local bm = rawget(_G, "g_BotManager")
          if bm ~= nil and type(bm.GetBotById) == "function" then
            local okB, bot = pcall(bm.GetBotById, bm, drv0.id)
            if okB and type(bot) == "table" then aim = tonumber(rawget(bot, "_TargetYawMovementVehicle")) end
          end
          local lvl = drv0.input._levels
          if (lvl[0] or 0) <= 0 then aim = nil end
          if aim ~= nil then
            local err = (hd - math.pi / 2 - aim) % (2 * math.pi)
            if err > math.pi then err = err - 2 * math.pi end
            st.err = err
            EVBotsVehDrive = EVBotsVehDrive or { n = 0, good = 0 }
            EVBotsVehDrive.n = EVBotsVehDrive.n + 1
            if math.abs(err) < 0.3 then EVBotsVehDrive.good = EVBotsVehDrive.good + 1 end
          end
        end
      end
      -- Диагностика езды раз в 5 с (сборка 178): водитель, позиция, скорость, нехулевые уровни ввода водителя (номер действия=уровень).
      -- Сборка 238: раз в 30 с (было 5 с — 8 строк в секунду при 20 машинах, главный источник объёма журнала стенда)
      if vehDiagT >= 30.0 and s.seats[1] ~= nil and s.seats[1] ~= 0 then
        local drv = playerByPtr(s.seats[1])
        local lv = {}
        if drv ~= nil and type(drv.input) == "table" and type(drv.input._levels) == "table" then
          for a, v in pairs(drv.input._levels) do
            if v ~= nil and v ~= 0 then lv[#lv + 1] = tostring(a) .. "=" .. string.format("%.2f", v) end
          end
          table.sort(lv)
        end
        local vel = e.velocity
        local sg = (st and st.steer) or { ok = 0, bad = 0 }
        -- Сборка 183: где солдат водителя (мод считает дистанцию до цели от soldier.worldTransform, BotActions.lua:116)
        if drv ~= nil and NativeBridge ~= nil and NativeBridge.SoldierPos ~= nil and drv.nativePtr ~= nil and drv.nativePtr ~= 0 then
          local okS, sx, _, sz = pcall(NativeBridge.SoldierPos, drv.nativePtr)
          if okS and sx ~= nil then
            lv[#lv + 1] = string.format("солдат (%.0f, %.0f) от машины %.1f м", sx, sz, math.sqrt((sx - (s.x or 0)) ^ 2 + (sz - (s.z or 0)) ^ 2))
          end
        end
        log("INFO ", string.format("ДИАГ езда %08X водитель %s поз (%.0f, %.0f) vel %.1f ввод [%s] руль согл/против %d/%d ошибка курса %s здоровье %s", ptr,
            drv and drv.name or "?", s.x or 0, s.z or 0, vel and vel.magnitude or 0, table.concat(lv, " "), sg.ok, sg.bad,
            (st and st.err) and string.format("%.2f", st.err) or "?", (st and st.hp) and string.format("%.0f", st.hp) or "?"))
        -- Сборка 183: стрелки (места 1+) — ввод наведения/огня, чтобы видеть, жмут ли они и куда
        for si = 2, s.seatCount do
          local gp = s.seats[si] ~= nil and s.seats[si] ~= 0 and playerByPtr(s.seats[si]) or nil
          if gp ~= nil and gp.onlineId == 0 and type(gp.input) == "table" and type(gp.input._levels) == "table" then
            local gl = {}
            for a, v in pairs(gp.input._levels) do
              if v ~= nil and v ~= 0 then gl[#gl + 1] = tostring(a) .. "=" .. string.format("%.2f", v) end
            end
            table.sort(gl)
            log("INFO ", string.format("ДИАГ стрелок %08X место %d %s ввод [%s]", ptr, si - 1, tostring(gp.name), table.concat(gl, " ")))
          end
        end
        if st ~= nil then st.steer = { ok = 0, bad = 0 } end
      end
      -- Сборка 183: Vehicle:Damage как VU (vehicle, damage, damageGiverInfo) — опросом здоровья раз в 0,5 с. Кто стрелял, пока не
      -- известно: giver = nil (мод тогда не отвечает огнём, BotManager.lua:266, но событие и урон — настоящие).
      if st ~= nil then
        local okH, hp = pcall(emVehHealth, ptr)
        if okH and hp ~= nil then
          if st.hp ~= nil and hp < st.hp - 0.01 then
            if not vehDmgHook then
              EVBotsVehDamage = (EVBotsVehDamage or 0) + 1
              dispatchVeh("Vehicle:Damage", e, st.hp - hp, nil)
            end
            if hp <= 0 and st.hp > 0 then
              log("INFO ", string.format("техника %08X: здоровье 0 (уничтожена)", ptr))
            end
          end
          st.hp = hp
        end
      end
      if st == nil then
        st = { seats = {} }
        vehState[ptr] = st
        local vt = ""
        pcall(function() vt = tostring(VehicleEntityData(e.data).controllableType) end)
        log("INFO ", string.format("техника %08X: %s, мест %d", ptr, vt, s.seatCount))
        dispatchVeh("Vehicle:SpawnDone", e)
      end
      for i = 1, s.seatCount do
        local cur, old = s.seats[i] or 0, st.seats[i] or 0
        if cur ~= old then
          st.seats[i] = cur
          if old ~= 0 then
            local p = playerByPtr(old)
            log("INFO ", string.format("техника %08X: место %d покинул %s", ptr, i - 1, p and p.name or "?"))
            if p ~= nil then dispatchVeh("Vehicle:Exit", e, p) end
          end
          if cur ~= 0 then
            local p = playerByPtr(cur)
            log("INFO ", string.format("техника %08X: место %d занял %s", ptr, i - 1, p and p.name or "?"))
            if p ~= nil then dispatchVeh("Vehicle:Enter", e, p) end
          end
        end
      end
    end
  end
  if vehDiagT >= 30.0 then vehDiagT = 0.0 end
  for ptr, _ in pairs(vehState) do
    if not seen[ptr] then
      local e = vehCache[ptr]
      vehState[ptr] = nil
      vehCache[ptr] = nil
      log("INFO ", string.format("техника %08X исчезла", ptr))
      if e ~= nil then
        -- Не читаем освобождённый DataContainer при доставке события моду.
        e.data = setmetatable({ instanceGuid = zeroGuid(), _type = "VehicleEntityData",
          controllableType = rawget(e, "_lastControllableType") or "" }, DataObj)
        dispatchVeh("Vehicle:Unspawn", e, 0, 0)
      end
    end
  end
end

-- ★ Сборка 185: CombatArea:PlayerDeserting / CombatArea:PlayerReturning (entity, player), как VU. События пишет перехват
-- движка (ServerCombatAreaTriggerEntity: 0x00E4FD20 / 0x00E4FE10, NativeBridge.PollCombatArea). Мод разворачивает бота
-- атакующей команды за границей (GameDirector:ToggleDirectionCombatZone); до 185 событий не было вовсе.
local caCache = {}
local function combatAreaEntity(ptr)
  local e = caCache[ptr]
  if e ~= nil then return e end
  e = setmetatable({ _kind = 0, _ptr = ptr, uniqueId = ptr, instanceId = ptr,
    typeInfo = { name = "ServerCombatAreaTriggerEntity" } }, Entity)
  e.data = setmetatable({ instanceGuid = zeroGuid(), _type = "CombatAreaTriggerEntityData" }, DataObj)
  caCache[ptr] = e
  return e
end
local caLogged = 0
local function pollCombatArea()
  if NativeBridge == nil or NativeBridge.PollCombatArea == nil or Events == nil or Events._DispatchEngine == nil then return end
  local ok, list = pcall(NativeBridge.PollCombatArea)
  if not ok or type(list) ~= "table" then return end
  for _, ev in ipairs(list) do
    local pl = playerByPtr(ev.player)
    if pl ~= nil then
      local name = ev.returning and "CombatArea:PlayerReturning" or "CombatArea:PlayerDeserting"
      if caLogged < 60 then
        caLogged = caLogged + 1
        log("INFO ", string.format("%s: %s (команда %s), зона %08X", name, tostring(pl.name), tostring(pl.teamId), ev.entity))
      end
      pcall(Events._DispatchEngine, Events, name, combatAreaEntity(ev.entity), pl)
    end
  end
end

-- Опрос флагов раз в 0,5 с: события, которые в VU шлёт движок (мост зовёт из тика Engine:Update).
local pollT, logT, last = 0.0, 0.0, {}

-- ★ Сборка 186: смена уровня (Level:Destroy) — в VU все сущности уровня уничтожаются, обёртки мода больше не живые.
-- Кэши по адресу сущности сбрасываем: новый уровень выделяет объекты по тем же адресам (особенно та же карта), и без
-- сброса мост считал новые машины «уже известными» — Vehicle:SpawnDone не уходил, боты во 2-м раунде не садились
-- в технику (прогоны 18.10: 34–44 посадки в 1-м раунде, 0 после смены раунда — 08:54, 09:25, 14:40, 15:29).
function EntityManager._LevelReset()
  local nv = 0
  for _ in pairs(vehState) do nv = nv + 1 end
  for k in pairs(vehState) do vehState[k] = nil end
  for k in pairs(vehCache) do vehCache[k] = nil end
  for k in pairs(cache) do cache[k] = nil end
  for k in pairs(caCache) do caCache[k] = nil end
  for k in pairs(last) do last[k] = nil end
  for k in pairs(evCallbacks) do evCallbacks[k] = nil end
  for k in pairs(rushWatched) do rushWatched[k] = nil end
  rushStage, rushScanT, rushLogged, evLogged = 0, 5.0, 0, 0
  pollT, logT, clock = 0, 0, 0
  spawnList, vspawnList, busCache, spawnListAt = nil, nil, {}, -1
  for k in pairs(kindOf) do kindOf[k] = nil end
  if EVBotsEmEvUnwatchAll ~= nil then pcall(EVBotsEmEvUnwatchAll) end
  cpCount = 0
  log("INFO ", string.format("смена уровня: кэши сущностей сброшены (техники было %d)", nv))
end
-- ★ Сборка 186: EntityFactory:Create — созданные ракеты (MissileEntityData) → Hooks._FireCall, как VU.
local missileLogged = 0
local function liveTransform(ptr)
  if EVBotsEmTransform == nil then return LinearTransform() end
  local ok, lx, ly, lz, ux, uy, uz, fx, fy, fz, x, y, z = pcall(EVBotsEmTransform, ptr)
  if not ok or lx == nil then return LinearTransform() end
  return LinearTransform(Vec3._make(lx, ly, lz), Vec3._make(ux, uy, uz), Vec3._make(fx, fy, fz), Vec3._make(x, y, z))
end
local MissileMt = { __index = function(t, k)
  if k == "transform" then return liveTransform(rawget(t, "_ptr")) end
  if k == "uniqueId" or k == "instanceId" then return rawget(t, "_ptr") end
  return Entity[k]
end }
local function pollCreated()
  if EVBotsEmCreatedPoll == nil or Hooks == nil or Hooks._FireCall == nil then return end
  local ok, list = pcall(EVBotsEmCreatedPoll)
  if not ok or type(list) ~= "table" then return end
  for _, it in ipairs(list) do
    local ent = setmetatable({ _kind = "ServerMissileEntity", _ptr = it[1] }, MissileMt)
    local data = nil
    if EVBotsLiveWrap ~= nil then
      local okw, d = pcall(EVBotsLiveWrap, it[2])
      if okw then data = d end
    end
    if data ~= nil then
      if missileLogged < 20 then
        missileLogged = missileLogged + 1
        local okp, pos = pcall(function() return ent.transform.trans end)
        log("INFO ", string.format("EntityFactory:Create: ракета %08X (данные %08X) в (%s) — обработчикам мода", it[1], it[2],
          okp and pos and string.format("%.0f, %.0f, %.0f", pos.x, pos.y, pos.z) or "?"))
      end
      pcall(Hooks._FireCall, "EntityFactory:Create", function() return ent end, data, ent.transform)
    end
  end
end

local createStatAt, createStatLast = 0.0, -1
function EntityManager._Poll(dt)
  dt = dt or 0
  pcall(pollCreated)  -- сборка 186: каждый тик (ракеты летят быстро)
  clock = clock + dt
  -- Сборка 201: раз в 45 с — сколько раз движок создавал сущности через перехваченный createEntity и каких типов данных.
  if EVBotsEmCreateStats ~= nil and clock - createStatAt > 45.0 then
    createStatAt = clock
    local okS, total, list = pcall(EVBotsEmCreateStats)
    if okS and total ~= nil and total ~= createStatLast then
      createStatLast = total
      table.sort(list, function(a, b) return a[2] > b[2] end)
      local parts = {}
      for i = 1, math.min(8, #list) do parts[#parts + 1] = list[i][1] .. "=" .. list[i][2] end
      log("INFO ", "ДИАГ createEntity: вызовов " .. total .. ", типов данных " .. #list .. " [" .. table.concat(parts, " ") .. "]"
        .. "; ракет (конструктор, сборка 211) " .. tostring(list.missiles))
    end
  end
  pollT = pollT + dt
  logT = logT + dt
  if pollT < 0.5 then return end
  pollT = 0.0
  pollCombatArea()  -- сборка 185
  pcall(rushScan, 0.5)  -- сборка 186: события Rush
  pollEntityEvents()  -- сборка 186
  pollVehicles()
  local ok, list = pcall(emList, KIND_CP)
  if not ok or type(list) ~= "table" or #list == 0 then return end
  local summary = {}
  for _, ptr in ipairs(list) do
    local e = cpEntity(ptr)
    if e ~= nil then
      refreshCp(e)
      local p = last[ptr]
      if p ~= nil and Events ~= nil and Events._DispatchEngine ~= nil then
        if e.isControlled and e.team ~= 0 and (not p.isControlled or p.team ~= e.team) then
          log("INFO ", "флаг " .. e.name .. " захвачен командой " .. tostring(e.team))
          pcall(Events._DispatchEngine, Events, "CapturePoint:Captured", e)
        elseif p.isControlled and not e.isControlled then
          log("INFO ", "флаг " .. e.name .. " потерян командой " .. tostring(p.team))
          pcall(Events._DispatchEngine, Events, "CapturePoint:Lost", e)
        end
        if p.isAttacked ~= e.isAttacked then
          -- Сборка 186: Entered — флаг стал «атакуемым», Exited — перестал (мод берёт из события только isAttacked)
          pcall(Events._DispatchEngine, Events, e.isAttacked and "Player:EnteredCapturePoint" or "Player:ExitedCapturePoint", nil, e)
        end
      end
      last[ptr] = { team = e.team, isControlled = e.isControlled, isAttacked = e.isAttacked }
      -- Сборка 147: координаты флага (x, z) — сверка «бот появился на флаге» по строкам «СПАВН проверка».
      local tx, tz = 0, 0
      if e.transform ~= nil and e.transform.trans ~= nil then tx, tz = e.transform.trans.x or 0, e.transform.trans.z or 0 end
      summary[#summary + 1] = string.format("%s т%d %.0f%%%s%s (%.0f, %.0f)", e.name, e.team or -1, e.flagLocation or -1,
          e.isControlled and " контр" or "", e.isAttacked and " атака" or "", tx, tz)
    end
  end
  if logT >= 30.0 then
    logT = 0.0
    log("INFO ", "ФЛАГИ: " .. table.concat(summary, "; "))
  end
end

_G.EntityManager = EntityManager

return EntityManager
)LUA";

bool RegisterVextEntityManagerImpl(lua_State* L) {
    lua_register(L, "EVBotsEmEvWatch", l_em_ev_watch);  // сборка 186: Entity:RegisterEventCallback
    lua_register(L, "EVBotsEmEvPoll", l_em_ev_poll);
    lua_register(L, "EVBotsEmEvUnwatchAll", l_em_ev_unwatch_all);
    lua_register(L, "EVBotsEmDataGuid", l_em_data_guid);
    lua_register(L, "EVBotsEmCreatedPoll", l_em_created_poll);  // сборка 186: EntityFactory:Create
    lua_register(L, "EVBotsEmVspawnCtl", l_em_vspawn_ctl);  // сборка 211: SpawnEntity.spawnedControllables
    lua_register(L, "EVBotsEmCreateStats", l_em_create_stats);  // сборка 201: диагностика createEntity
    lua_register(L, "EVBotsEmTransform", l_em_transform);
    lua_pushcfunction(L, [](lua_State* L_) -> int {
        const char* level = luaL_optstring(L_, 1, "INFO ");
        const char* text = luaL_optstring(L_, 2, "");
        LogPrintf(level, "EVBots: EntityManager: %s", text);
        return 0;
    });
    const int kSourceLen = static_cast<int>(sizeof(kEntityManagerSource) - 1);
    if (luaL_loadbufferx(L, kEntityManagerSource, kSourceLen,
                         "=[entitymanager]", "t") != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: EntityManager не собрался: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 2);
        return false;
    }
    lua_pushvalue(L, -2);  // log
    lua_pushcfunction(L, l_em_list);
    lua_pushcfunction(L, l_em_cp);
    lua_pushcfunction(L, l_em_spawn);
    lua_pushcfunction(L, l_em_is_veh);
    lua_pushcfunction(L, l_em_veh);
    lua_pushcfunction(L, l_em_data);
    lua_pushcfunction(L, l_em_part_count);  // сборка 182
    lua_pushcfunction(L, l_em_part);
    lua_pushcfunction(L, l_em_veh_health);  // сборка 183
    lua_pushcfunction(L, l_em_veh_damage_pop);  // сборка 184
    lua_pushcfunction(L, l_em_veh_teleport);  // сборка 185
    lua_pushcfunction(L, l_em_veh_vel);  // сборка 201
    if (lua_pcall(L, 13, 1, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: EntityManager не выполнился: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 2);
        return false;
    }
    if (!lua_istable(L, -1)) {
        LOG_ERR("EVBots: EntityManager вернул не таблицу");
        lua_pop(L, 2);
        return false;
    }
    lua_setglobal(L, "EntityManager");
    lua_pop(L, 1);  // log (EntityManager._VehicleByPtr — для Player.controlledControllable)
    LOG_INFO("EVBots: EntityManager подключен (флаги и точки спавна из движка, сборка 125)");
    return true;
}

}  // namespace

static bool OwnEntityFactoryHookMatches(const unsigned char* entry,const unsigned char* trampoline,ULONG_PTR detour) {
    // ev-core writes eight bytes and copies two complete instructions (10 bytes).
    // Check the actual detour, untouched entry tail, native trampoline and return jump.
    static const unsigned char original[16] = {
        0x83,0xEC,0x18,0x80,0x3D,0x78,0x35,0xB4,0x02,0x00,0x75,0x06,0x33,0xC0,0x83,0xC4
    };
    if (!entry || !trampoline) return false;
    __try {
        if (entry[0] != 0xE9 || entry[5] != 0x90 || entry[6] != 0x90 || entry[7] != 0x90 ||
            entry[8] != original[8] || entry[9] != original[9] ||
            ::memcmp(entry + 10, original + 10, 6) != 0 ||
            ::memcmp(trampoline, original, 10) != 0 || trampoline[10] != 0xE9) return false;
        const ULONG_PTR target = reinterpret_cast<ULONG_PTR>(entry + 5) + *reinterpret_cast<const LONG*>(entry + 1);
        const ULONG_PTR back = reinterpret_cast<ULONG_PTR>(trampoline + 15) + *reinterpret_cast<const LONG*>(trampoline + 11);
        return target == detour && back == reinterpret_cast<ULONG_PTR>(entry + 10) &&
               IsExecutable(const_cast<unsigned char*>(trampoline));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool EVBotsIsOwnEntityFactoryHook() {
    return EngineBase() && g_trampCreate && OwnEntityFactoryHookMatches(
        reinterpret_cast<const unsigned char*>(0x00DA36E0UL),static_cast<const unsigned char*>(g_trampCreate),
        reinterpret_cast<ULONG_PTR>(&CreateEntityDetour));
}

// ★ Сборка 186: перехват EntityFactory:Create ставится из хука тика (поток движка): патч кода не идёт в чужом потоке.
void EVBotsInstallEntityFactoryHookOnce() {
    static volatile LONG s_done = 0;
    if (InterlockedCompareExchange(&s_done, 1, 0) != 0) return;
    char v[4] = {};
    const DWORD n = ::GetEnvironmentVariableA("EVBOTS_ENTITY_FACTORY", v, (DWORD)sizeof(v));
    if (n == 1 && v[0] == '0') return;
    unsigned char* base = EngineBase();
    if (base == nullptr) return;
    unsigned char* t = (unsigned char*)(ULONG_PTR)0x00DA36E0UL;
    // sub esp, 0x18; cmp byte [0x02B43578], 0 — сверено дизассемблером
    static const unsigned char kPro[10] = {0x83, 0xEC, 0x18, 0x80, 0x3D, 0x78, 0x35, 0xB4, 0x02, 0x00};
    bool same = false;
    __try {
        same = ::memcmp(t, kPro, sizeof(kPro)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        same = false;
    }
    if (!same) {
        LOG_ERR("EVBots: EntityManager: createEntity 0x00DA36E0 — пролог не совпал, EntityFactory:Create НЕ ставлю");
        return;
    }
    static InlineHook s_hook;
    if (s_hook.Install(t, (void*)&CreateEntityDetour, "VextEntityManager:s_hook") != HookResult::Ok) {
        LOG_ERR("EVBots: EntityManager: EntityFactory:Create — установка не удалась");
        return;
    }
    g_trampCreate = s_hook.Trampoline();
    LOG_INFO("EVBots: EntityManager: ★ EntityFactory:Create перехвачен (createEntity 0x00DA36E0) — ракеты для мода, как VU");
    // ★ Сборка 211: ракеты — конструктор ServerMissileEntity (см. MissileCtorDetour). Пролог: push ebp; mov ebp, esp;
    // and esp, -16; mov eax, [ebp+0xC]; sub esp, 0x134 — сверено дизассемблером. EVBOTS_MISSILE_HOOK=0 — выкл.
    const DWORD nm = ::GetEnvironmentVariableA("EVBOTS_MISSILE_HOOK", v, (DWORD)sizeof(v));
    if (nm == 1 && v[0] == '0') return;
    unsigned char* tm = (unsigned char*)(ULONG_PTR)0x00E72F90UL;
    static const unsigned char kMis[15] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x8B, 0x45, 0x0C, 0x81, 0xEC, 0x34, 0x01, 0x00, 0x00};
    bool sameM = false;
    __try {
        sameM = ::memcmp(tm, kMis, sizeof(kMis)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sameM = false;
    }
    if (!sameM) {
        LOG_ERR("EVBots: EntityManager: конструктор ракеты 0x00E72F90 — пролог не совпал, ракеты моду не идут");
        return;
    }
    static InlineHook s_hookM;
    if (s_hookM.Install(tm, (void*)&MissileCtorDetour, "VextEntityManager:s_hookM") != HookResult::Ok) {
        LOG_ERR("EVBots: EntityManager: конструктор ракеты — установка перехвата не удалась");
        return;
    }
    g_trampMissile = s_hookM.Trampoline();
    LOG_INFO("EVBots: EntityManager: ★ ракеты — перехват конструктора ServerMissileEntity 0x00E72F90 (EntityFactory:Create для мода)");
}

// ★ Сборка 159: перечисление сущностей по имени типа для пробника техники (VextPlayerManager.cpp). Только чтение.
int EVBotsListEntitiesByName(const char* name, unsigned long* out, int cap) {
    for (const TypeRec& t : kTypes) {
        if (::strcmp(t.name, name) == 0) return ListEntities(t, out, cap);
    }
    return 0;
}

unsigned long EVBotsFindTeamBaseSpawn(unsigned int team) {
    unsigned long cpPtrs[64], spawnPtrs[256], vehiclePtrs[256];
    BaseCapturePoint points[64] = {};
    BaseCharacterSpawn spawns[256] = {};
    const int cpN = EVBotsListEntitiesByName("ServerCapturePointEntity", cpPtrs, 64);
    const int spawnN = EVBotsListEntitiesByName("ServerCharacterSpawnEntity", spawnPtrs, 256);
    const int vehicleN = EVBotsListEntitiesByName("ServerVehicleSpawnEntity", vehiclePtrs, 256);
    for (int i = 0; i < cpN; ++i) {
        __try {
            const unsigned char* cp = (const unsigned char*)(ULONG_PTR)cpPtrs[i];
            const unsigned char* data = *(const unsigned char* const*)(cp + 0x14);
            if (data == nullptr) continue;
            points[i].team = *(const unsigned int*)(cp + 0xB0);
            points[i].capturableType = *(const unsigned int*)(data + 0xA8);
            points[i].bus = *(const unsigned long*)(cp + 0x10);
        } __except (EXCEPTION_EXECUTE_HANDLER) { points[i] = {}; }
    }
    for (int i = 0; i < spawnN; ++i) {
        __try {
            const unsigned char* spawn = (const unsigned char*)(ULONG_PTR)spawnPtrs[i];
            const unsigned char* data = *(const unsigned char* const*)(spawn + 0x14);
            if (data == nullptr) continue;
            (void)*(volatile const unsigned long*)(data + 0x180);  // та же проверка ссылки, что перед NativeSpawnOne
            spawns[i].team = *(const unsigned int*)(data + 0x128);
            spawns[i].bus = *(const unsigned long*)(spawn + 0x10);
            spawns[i].ptr = spawnPtrs[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) { spawns[i] = {}; }
        for (int v = 0; spawns[i].ptr != 0 && v < vehicleN; ++v) {
            __try {
                if (*(const unsigned long*)(ULONG_PTR)(vehiclePtrs[v] + 0x10) == spawns[i].bus) {
                    spawns[i].vehicle = true;
                    break;
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) { }
        }
    }
    return SelectTeamBaseSpawn(team, points, cpN, spawns, spawnN);
}

bool RegisterVextEntityManager(lua_State* L) {
    return RegisterVextEntityManagerImpl(L);
}
}  // namespace evbots
