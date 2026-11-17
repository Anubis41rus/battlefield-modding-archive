// EngineResources.cpp — нативная база данных ресурсов движка (сборка 161). Описание — в EngineResources.h.
//
// Разбор адресов (bf3_server_Final.exe, база 0x400000, без ASLR; сверено capstone 16.10.2026):
//   [0x2B16064 + c*4]      — Compartment* отсеков 0..204 (ResourceManager::m_compartments)
//   [Compartment + 0x50]   — домен БД отсека (0x54FBF0 отдаёт именно его)
//   [0x2B17760]            — менеджер БД (0x54FBE0)
//   0x5771E0 thiscall(domain, const char* name) ret 4    — раздел по имени (лок, хеш djb2/tolower)
//   0x5703E0 thiscall(partition)                          — [p+0x4C] главный инстанс
//   0x579BB0 thiscall(partition, const Guid*) ret 4       — инстанс раздела по GUID (лок, хеш)
//   0x57C170 thiscall(dbm, DataContainer*) ret 4          — раздел инстанса (ctrToPartitionMap)
//   0x570430 thiscall(domain)                             — число разделов домена ([+0x84..+0x88])
//   0x579C50 thiscall(domain, Partition** out, int n) ret 8 — копия списка разделов (лок, memcpy)
//   0x468990 thiscall(dc)                                 — getInstanceGuid: [dc+6]&0x100 ? dc-0x10 : 0
//   0x554190 thiscall(Compartment*, const char* name, a2, a3, a4) ret 0x10 — onBundlePartition
// Гипотезы (сверяются в рантайме, итог — в логе): GUID раздела по [p+0x0C] (геттер 0x56C1D0);
// у массива TypeInfo элемента по [*InfoData+0x10].
// 0x54FE50 (lookupDataContainer по GUID) сам НЕ зовём: он берёт временный аллокатор движка
// (0x43F550), а Lua у нас крутится в своём потоке-таймере — повторяем его цикл своими силами.

#include "EngineResources.h"

#include <Windows.h>
#include <intrin.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

#include "InlineHook.h"
#include "EVBotsLog.h"
#include "LuaRuntime.h"
#include "VextResourceManager.h"

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace evbots {
namespace eng {
namespace {

constexpr unsigned long kImageBase = 0x00400000UL;
constexpr int kCompartmentCount = 205;

struct Prologue {
    unsigned long va;
    const char* name;
    unsigned char bytes[10];
};

// ★ Сборка 234 (реестр В8): адреса — таблицей на exe. Сервер — bf3_server_Final.exe (как было, сборки 161–163);
// клиент — bf3.exe (zbf3.exe, разбор 26.10: функции найдены по байтам серверных с маской адресов и сверены по
// вызовам в Ghidra клиента — MODLOG «Сборка 234»). Выбор — по модулю процесса в Ready(); прологи — «всё или ничего».
struct Addrs {
    const char* exe;
    unsigned long compartments;  ///< [x + c*4] — Compartment* отсеков 0..204
    unsigned long dbManager;     ///< [x] — менеджер БД
    unsigned long findPartitionByName, primaryInstance, findInPartition, partitionOf, partitionCount, partitionList;
    unsigned long getInstanceGuid, getDbManager, onBundlePartition;
    unsigned long endPostChunks;  ///< 0 — не найден (у клиента): второй перехват не ставим
    unsigned long settingsManager, settingsFind, lock, unlock;
    unsigned long imageEnd, codeEnd;  ///< границы образа и кода (проверка vtable/getType)
    const Prologue* prologues;
    unsigned prologueCount;
};

// Сервер (bf3_server_Final.exe, сверено capstone 16.10.2026):
// ★ Прогон 161: уровень грузится НЕ через onBundlePartition (перехват молчал, а в отсеке 3 появилось
// 4490 разделов). Общая точка всех веток — конец загрузки бандла в отсек:
// Compartment::endPostChunks (строки «Compartment_endPostChunks», «Done updating resource systems for
// post-chunk update»), thiscall без аргументов, `ret` без операнда (0x562092).
// ★ Сборка 163: fb::SettingsManager. Экземпляр — [0x2AF7DD4] (так его берут 0x88DC21 и 0x53FC1D).
// getContainer(TypeInfo*) = 0x53FBC0 → внутренний 0x53F7A0: EnterCriticalSection(mgr+0x20), хеш-карта mgr+0x60
// (корзины +4, их число +8, хеш = TypeInfo* % число), узел {TypeInfo* ключ, контейнер, next}. 0x53FBC0 сам не
// зовём — он трогает TLS движка (fs:[0x2C] → +0x110), которого у нашего потока Lua может не быть.
// Эталон — первые 10 байт каждой функции из файла exe (правило 18).
const Prologue kServerPrologues[] = {
    {0x005771E0UL, "Domain::findPartitionByName", {0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x77, 0x40, 0x83, 0xC6, 0x10}},
    {0x005703E0UL, "Partition::primaryInstance", {0x8B, 0x41, 0x4C, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC}},
    {0x00579BB0UL, "Partition::findInstance(guid)", {0x83, 0xEC, 0x08, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x47, 0x08}},
    {0x0057C170UL, "DbManager::partitionOf(dc)", {0x56, 0x57, 0x8B, 0xF9, 0x8D, 0x77, 0x10, 0x8B, 0xCE, 0xE8}},
    {0x00570430UL, "Domain::partitionCount", {0x53, 0x56, 0x8B, 0xD9, 0x57, 0x8B, 0x7B, 0x40, 0x83, 0xC7}},
    {0x00579C50UL, "Domain::copyPartitions", {0x51, 0x53, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x77, 0x40, 0x83}},
    {0x00468990UL, "DataContainer::getInstanceGuid", {0xB8, 0x00, 0x01, 0x00, 0x00, 0x66, 0x85, 0x41, 0x06, 0x74}},
    {0x0054FBE0UL, "ResourceManager_getDatabaseManager", {0xA1, 0x60, 0x77, 0xB1, 0x02, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC}},
    {0x00554190UL, "Compartment::onBundlePartition", {0x81, 0xEC, 0xA8, 0x00, 0x00, 0x00, 0x80, 0x3D, 0xA8, 0xA0}},
    {0x00561CC0UL, "Compartment::endPostChunks", {0x81, 0xEC, 0x50, 0x01, 0x00, 0x00, 0x53, 0x55, 0x56, 0x33}},
    {0x0053F7A0UL, "SettingsManager::findContainer", {0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1, 0x57, 0x8D, 0x7E, 0x20}},
    {0x00493360UL, "lock(CriticalSection)", {0x51, 0xFF, 0x15, 0x64, 0xD6, 0xBD, 0x02, 0xC3, 0xCC, 0xCC}},
    {0x00493370UL, "unlock(CriticalSection)", {0x51, 0xFF, 0x15, 0x4C, 0xD6, 0xBD, 0x02, 0xC3, 0xCC, 0xCC}},
};
const Addrs kServer = {
    "bf3_server_Final.exe", 0x02B16064UL, 0x02B17760UL,
    0x005771E0UL, 0x005703E0UL, 0x00579BB0UL, 0x0057C170UL, 0x00570430UL, 0x00579C50UL,
    0x00468990UL, 0x0054FBE0UL, 0x00554190UL,
    0x00561CC0UL,
    0x02AF7DD4UL, 0x0053F7A0UL, 0x00493360UL, 0x00493370UL,
    0x02D01000UL, 0x02428000UL,
    kServerPrologues, sizeof(kServerPrologues) / sizeof(kServerPrologues[0]),
};

// Клиент (zbf3.exe, 26.10.2026). Факты (дизассемблер / Ghidra клиента):
//   отсеки [0x0230B9C4 + c*4], домен [отсек+0x50] — 004C1364/004C136F (поиск инстанса по GUID 004C1340, аналог
//   серверного 0x54FE50: тот же цикл partitionCount 004DDD50 → copyPartitions 004E6200 → findInstance 004E6160,
//   родитель отсека — [отсек+0x4C]); менеджер БД [0x0230D0C0] — геттер 004C1270 `mov eax,[0x230D0C0]; ret`, им
//   пользуется partitionOf 004E83D0 (00A4E8E0..E7); onBundlePartition 004C5080 (thiscall, 4 аргумента: лок
//   [this+0x10] 0046FF80, домен [this+0x50], findPartitionByName 004E3EB0, сравнение с [this+0x48]) — строк
//   профилировщика в клиенте нет; SettingsManager [0x022ED35C] (00FB478B: `mov ecx,[0x22ED35C]; push TypeInfo;
//   call 0049EB60` → внутренний 0049E940, совпал с серверным 0x53F7A0 на 50 байт); lock/unlock 0046FF80/0046FF90.
//   primaryInstance 004DDD00 (`mov eax,[ecx+0x4C]; ret`, рядом с partitionCount — как на сервере).
//   17.11: endPostChunks=004D0DE0, native thiscall без аргументов. Сверены cleanup массивов
//   +258/+268/+118, lock/unlock +10 и вызов 004D1BA2 после poll resource jobs(stage2).
const Prologue kClientPrologues[] = {
    {0x004E3EB0UL, "Domain::findPartitionByName", {0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x77, 0x40, 0x83, 0xC6, 0x10}},
    {0x004DDD00UL, "Partition::primaryInstance", {0x8B, 0x41, 0x4C, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC}},
    {0x004E6160UL, "Partition::findInstance(guid)", {0x83, 0xEC, 0x08, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x47, 0x08}},
    {0x004E83D0UL, "DbManager::partitionOf(dc)", {0x56, 0x57, 0x8B, 0xF9, 0x8D, 0x77, 0x10, 0x8B, 0xCE, 0xE8}},
    {0x004DDD50UL, "Domain::partitionCount", {0x53, 0x56, 0x8B, 0xD9, 0x57, 0x8B, 0x7B, 0x40, 0x83, 0xC7}},
    {0x004E6200UL, "Domain::copyPartitions", {0x51, 0x53, 0x56, 0x57, 0x8B, 0xF9, 0x8B, 0x77, 0x40, 0x83}},
    {0x00448490UL, "DataContainer::getInstanceGuid", {0xB8, 0x00, 0x01, 0x00, 0x00, 0x66, 0x85, 0x41, 0x06, 0x74}},
    {0x004C1270UL, "ResourceManager_getDatabaseManager", {0xA1, 0xC0, 0xD0, 0x30, 0x02, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC}},
    {0x004C5080UL, "Compartment::onBundlePartition", {0x81, 0xEC, 0x90, 0x00, 0x00, 0x00, 0x55, 0x56, 0x8B, 0xF1}},
    {0x004D0DE0UL, "Compartment::endPostChunks", {0x83, 0xEC, 0x18, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF9, 0x8D}},
    {0x0049E940UL, "SettingsManager::findContainer", {0x83, 0xEC, 0x08, 0x56, 0x8B, 0xF1, 0x57, 0x8D, 0x7E, 0x20}},
    {0x0046FF80UL, "lock(CriticalSection)", {0x51, 0xFF, 0x15, 0x0C, 0xD7, 0x3B, 0x02, 0xC3, 0xCC, 0xCC}},
    {0x0046FF90UL, "unlock(CriticalSection)", {0x51, 0xFF, 0x15, 0x08, 0xD7, 0x3B, 0x02, 0xC3, 0xCC, 0xCC}},
};
const Addrs kClient = {
    "bf3.exe", 0x0230B9C4UL, 0x0230D0C0UL,
    0x004E3EB0UL, 0x004DDD00UL, 0x004E6160UL, 0x004E83D0UL, 0x004DDD50UL, 0x004E6200UL,
    0x00448490UL, 0x004C1270UL, 0x004C5080UL,
    0x004D0DE0UL,
    0x022ED35CUL, 0x0049E940UL, 0x0046FF80UL, 0x0046FF90UL,
    0x0248A000UL, 0x0205E000UL,  // конец образа — bf3.exe (SizeOfImage 0x208A000; у zbf3 — 0x2089000), конец кода — ctr
    kClientPrologues, sizeof(kClientPrologues) / sizeof(kClientPrologues[0]),
};

const Addrs* g_a = &kServer;  // выбирается в Ready()

#define kVaCompartments (g_a->compartments)
#define kVaDbManager (g_a->dbManager)
#define kVaFindPartitionByName (g_a->findPartitionByName)
#define kVaPrimaryInstance (g_a->primaryInstance)
#define kVaFindInPartition (g_a->findInPartition)
#define kVaPartitionOf (g_a->partitionOf)
#define kVaPartitionCount (g_a->partitionCount)
#define kVaPartitionList (g_a->partitionList)
#define kVaGetInstanceGuid (g_a->getInstanceGuid)
#define kVaGetDbManager (g_a->getDbManager)
#define kVaOnBundlePartition (g_a->onBundlePartition)
#define kVaEndPostChunks (g_a->endPostChunks)
#define kVaSettingsManager (g_a->settingsManager)
#define kVaSettingsFind (g_a->settingsFind)
#define kVaLock (g_a->lock)
#define kVaUnlock (g_a->unlock)

using ThisCall0 = unsigned long(__fastcall*)(unsigned long self, unsigned long edx);
using ThisCall1 = unsigned long(__fastcall*)(unsigned long self, unsigned long edx, unsigned long a1);
using ThisCall2 = unsigned long(__fastcall*)(unsigned long self, unsigned long edx, unsigned long a1,
                                             unsigned long a2);
using OnBundlePartitionFn = unsigned long(__fastcall*)(unsigned long self, unsigned long edx,
                                                       const char* name, unsigned long a2,
                                                       unsigned long a3, unsigned long a4);

unsigned char* g_base = nullptr;
volatile LONG g_ready = -1;  // -1 — не проверяли, 0 — нет, 1 — да
volatile LONG g_pgCheck = 0;

inline unsigned long Va(unsigned long va) {
    return (unsigned long)(ULONG_PTR)g_base + (va - kImageBase);
}

bool EnvOff(const char* name) {
    char v[8] = {};
    const DWORD n = ::GetEnvironmentVariableA(name, v, (DWORD)sizeof(v));
    return n > 0 && v[0] == '0';
}

bool ReadU32(unsigned long addr, unsigned long* out) {
    __try {
        *out = *(const unsigned long*)(ULONG_PTR)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadU16(unsigned long addr, unsigned* out) {
    __try {
        *out = *(const unsigned short*)(ULONG_PTR)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadU8(unsigned long addr, unsigned* out) {
    __try {
        *out = *(const unsigned char*)(ULONG_PTR)addr;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool PrologueOk(const Prologue& p) {
    __try {
        return std::memcmp((const void*)(ULONG_PTR)Va(p.va), p.bytes, sizeof(p.bytes)) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Сборка 267. Источник: veniceext SHA256 c34ad182...912ee8:
// Mount 10BFCAE0 -> slot 18E75020; AddRegistry 10BFBAE0 -> 18E74FB4.
// vu-core 101A1940 отправляет Level:LoadResources ДО original, 101A8D90 отправляет
// Level:RegisterEntityResources ПОСЛЕ original. Это синхронные фазы, не события кадра.
// Все адреса и 16 байт ниже сверены capstone 11.11 по zbf3.exe / bf3_server_Final.exe.
struct ResourceMutationAddrs {
    unsigned long mount, mountManager, addRegistry, loadResources, registerResources, loadBundles;
    unsigned levelNameOffset;
    unsigned long registryActive, registryLock, registryExit, postMarshal;
    unsigned char bytes[8][16];
};

const ResourceMutationAddrs kServerMutation = {
    0x0054A9F0, 0x00524DB0, 0x006040C0, 0x0085AE80, 0x0083E400, 0x00562550, 0x6C,
    0x02B1BBCC, 0x02B1C430, 0x006045B0, 0x00588780,
    {
        {0xE8,0xBB,0xA3,0xFD,0xFF,0x8B,0x4C,0x24,0x0C,0x8B,0x10,0x8B,0x12,0x51,0x8B,0x4C},
        {0xA1,0xD4,0x75,0xAF,0x02,0x85,0xC0,0x75,0x27,0x6A,0x70,0xE8,0xB0,0x6B,0xF0,0xFF},
        {0x81,0xEC,0x80,0x01,0x00,0x00,0x8B,0x84,0x24,0x84,0x01,0x00,0x00,0x8D,0x48,0x08},
        {0x81,0xEC,0x9C,0x06,0x00,0x00,0x80,0x3D,0xA8,0xA0,0xAC,0x02,0x00,0x53,0x55,0x56},
        {0x56,0x33,0xF6,0x80,0x3D,0xA8,0xA0,0xAC,0x02,0x00,0x74,0x2A,0xE8,0x0F,0x08,0xC9},
        {0x56,0xB8,0x60,0x60,0xB1,0x02,0xB9,0x01,0x00,0x00,0x00,0xF0,0x0F,0xC1,0x08,0x8B},
        {0x83,0xEC,0x30,0xB9,0x30,0xC4,0xB1,0x02,0xE8,0xA3,0xED,0xE8,0xFF,0x83,0x3D,0xCC},
        {0x56,0x8B,0xF1,0x8B,0x46,0x34,0x8B,0x8E,0xB4,0x00,0x00,0x00,0x8B,0x96,0xDC,0x02},
    }
};
const ResourceMutationAddrs kClientMutation = {
    0x004BC7C0, 0x004A7510, 0x005760F0, 0x011A29B0, 0x0118DAD0, 0x004D1260, 0x68,
    0x0235166C, 0x02351A00, 0x005764B0, 0x004F23B0,
    {
        {0xE8,0x4B,0xAD,0xFE,0xFF,0x8B,0x4C,0x24,0x0C,0x8B,0x10,0x8B,0x12,0x51,0x8B,0x4C},
        {0xA1,0xDC,0xB1,0x2F,0x02,0xC3,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC,0xCC},
        {0x83,0xEC,0x4C,0x8B,0x44,0x24,0x50,0x53,0x8D,0x48,0x08,0x55,0x89,0x4C,0x24,0x44},
        {0x81,0xEC,0xA0,0x00,0x00,0x00,0x53,0x56,0x6A,0x00,0x8B,0xF1,0x8B,0x46,0x68,0x6A},
        {0x8B,0x44,0x24,0x04,0x8B,0x48,0x24,0x6A,0x03,0x51,0xE8,0x11,0x86,0x3E,0xFF,0x83},
        {0x56,0xB8,0xC0,0xB9,0x30,0x02,0xB9,0x01,0x00,0x00,0x00,0xF0,0x0F,0xC1,0x08,0x8B},
        {0x83,0xEC,0x30,0xB9,0x00,0x1A,0x35,0x02,0xE8,0xC3,0x9A,0xEF,0xFF,0x83,0x2D,0x6C},
        {0x56,0x8B,0xF1,0x8B,0x46,0x34,0x8B,0x8E,0xB4,0x00,0x00,0x00,0x8B,0x96,0xDC,0x02},
    }
};
const ResourceMutationAddrs* g_mutation = nullptr;
InlineHook g_hookLoadResources, g_hookRegisterResources, g_hookLoadBundles, g_hookRegistryExit, g_hookPostMarshal;
std::set<unsigned long> g_registryPins;
void* g_trampLoadResources = nullptr;
void* g_trampRegisterResources = nullptr;
void* g_trampLoadBundles = nullptr;
LevelLoadResourcesFn g_onLoadResources = nullptr;
LevelRegisterResourcesFn g_onRegisterResources = nullptr;
ResourceLoadBundlesFn g_onLoadBundles = nullptr;
volatile LONG g_resourceHooksReady = 0;
// Static proof: D:\Games-cache\royale\terrain-hook-mapping.txt.
// Return() cancels resource loading; a null Resource is not a valid cancellation result.
struct TerrainAddrs {
    unsigned long load,visualLoad,visualPostLoad,loadingInfo,terrainType,visualType;
    unsigned char bytes[4][16];
};
const TerrainAddrs kServerTerrain={0x00CE0090,0x010BA320,0x010BA540,0x0084AAC0,0x02C13408,0x02C1B534,
    {{0x83,0xEC,0x74,0x53,0x56,0x33,0xF6,0x80,0x3D,0xA8,0xA0,0xAC,0x02,0x00,0x89,0x4C},
     {0x81,0xEC,0x18,0x01,0x00,0x00,0x53,0x89,0x4C,0x24,0x08,0x55,0x8D,0x4C,0x24,0x10},
     {0x51,0x8D,0x0C,0x24,0xE8,0xD7,0x03,0x37,0xFF,0xE8,0xD2,0x02,0x49,0xFF,0x8B,0x4C},
     {0x53,0x56,0x8B,0xF1,0x57,0x8D,0x7E,0x10,0x8B,0xCF,0xE8,0x91,0x88,0xC4,0xFF,0x8D}}};
const TerrainAddrs kClientTerrain={0x00B04600,0x009D47C0,0x009D4940,0x01196180,0x023E6108,0x023E2E2C,
    {{0x83,0xEC,0x68,0x8B,0x44,0x24,0x7C,0x53,0x55,0x56,0x57,0x89,0x4C,0x24,0x20,0x50},
     {0x81,0xEC,0x10,0x01,0x00,0x00,0x53,0x55,0x56,0x68,0x00,0x01,0x00,0x00,0x8D,0x44},
     {0xE8,0xEB,0x7C,0xAE,0xFF,0x8B,0x4C,0x24,0x04,0x80,0xB9,0x99,0x0C,0x00,0x00,0x00},
     {0x53,0x56,0x8B,0xF1,0x57,0x8D,0x7E,0x10,0x8B,0xCF,0xE8,0xF1,0x9D,0x2D,0xFF,0x8D}}};
const TerrainAddrs* g_terrain=nullptr;
InlineHook g_hookTerrain,g_hookVisualTerrain,g_hookVisualTerrainPost;
InlineHook g_hookLoadingInfo;
void* g_trampTerrain=nullptr;
void* g_trampVisualTerrain=nullptr;
void* g_trampVisualTerrainPost=nullptr;
void* g_trampLoadingInfo=nullptr;
// Область разрешения принадлежит текущему потоку engine callback. Events:Dispatch из Lua
// и отложенные обработчики не могут открыть её самостоятельно.
enum class ResourcePhase { None, Load, InstanceLoad, Register, Bundles };
thread_local ResourcePhase g_resourcePhase = ResourcePhase::None;
thread_local unsigned long g_identityLevel=0;
thread_local bool g_identityActive=false;

// Names are never cached across native callbacks. The Level pointer belongs to
// the current engine load; the loading-info receiver itself is NOT a Level.
class NativeIdentityScope {
public:
    explicit NativeIdentityScope(unsigned long level) : previous_(g_identityLevel), active_(g_identityActive) {
        g_identityLevel=level;
        g_identityActive=true;
    }
    ~NativeIdentityScope() { g_identityLevel=previous_; g_identityActive=active_; }
private:
    unsigned long previous_;
    bool active_;
};

unsigned long NativeLoadingLevel() {
    unsigned long main=0,owner=0,context=0,level=0;
    if (g_a==&kServer) {
        // D6714E publishes the new Level at Server+7C before D671BD calls load;
        // D6726F clears it afterwards. Server+74 may still be the previous level.
        if (!ReadU32(Va(0x02877210),&main) || !ReadU32(main+0xD0,&owner) ||
            !ReadU32(owner+0x7C,&level)) return 0;
    } else if (g_a==&kClient) {
        // Same verified chain as ModdingInterface::ClientLevelInfo.
        if (!ReadU32(Va(0x022E3790),&main) || !ReadU32(main+0xB4,&owner) ||
            !ReadU32(owner+0xC,&context) || !ReadU32(context+0x10,&level)) return 0;
    }
    return level;
}

class ResourcePhaseScope {
public:
    explicit ResourcePhaseScope(ResourcePhase phase) : previous_(g_resourcePhase) { g_resourcePhase = phase; }
    ~ResourcePhaseScope() { g_resourcePhase = previous_; }
private:
    ResourcePhase previous_;
};

bool MutationBytesMatch(unsigned long va, const unsigned char* bytes) {
    __try {
        return std::memcmp((const void*)(ULONG_PTR)Va(va), bytes, 16) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CopyEngineText(unsigned long ptr, char* out, size_t cap) {
    if (ptr < 0x10000 || cap == 0) return false;
    __try {
        for (size_t i = 0; i < cap; ++i) {
            out[i] = ((const char*)(ULONG_PTR)ptr)[i];
            if (out[i] == '\0') return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    out[0] = '\0';
    return false;
}

// Resource ABI: exactly three virtual slots, no virtual destructor. A cancelled
// resource owns no terrain data. Its native type and refcount remain valid until
// the resource manager releases it. Storage belongs exclusively to our heap.
struct CancelledTerrainResource {
    const void* const* vtable;
    volatile LONG references;
    unsigned long type;
};
static_assert(sizeof(CancelledTerrainResource)==12,"x86 Resource ABI");
unsigned long __fastcall CancelledTerrainType(CancelledTerrainResource* p,void*) { return p->type; }
unsigned long __fastcall CancelledTerrainAddRef(CancelledTerrainResource* p,void*) {
    return (unsigned long)::InterlockedIncrement(&p->references);
}
unsigned long __fastcall CancelledTerrainRelease(CancelledTerrainResource* p,void*) {
    const LONG count=::InterlockedDecrement(&p->references);
    if (count==0) ::HeapFree(::GetProcessHeap(),0,p);
    return (unsigned long)count;
}
const void* const kCancelledTerrainVtable[]={
    (const void*)&CancelledTerrainType,(const void*)&CancelledTerrainAddRef,(const void*)&CancelledTerrainRelease};
unsigned long MakeCancelledTerrain(bool visual) {
    auto* p=(CancelledTerrainResource*)::HeapAlloc(::GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(CancelledTerrainResource));
    if (!p) return 0;
    p->vtable=kCancelledTerrainVtable;
    p->type=Va(visual?g_terrain->visualType:g_terrain->terrainType);
    return (unsigned long)(ULONG_PTR)p;
}
bool IsCancelledTerrain(unsigned long resource) {
    unsigned long table=0;
    return ReadU32(resource,&table) && table==(unsigned long)(ULONG_PTR)kCancelledTerrainVtable;
}

bool ApplyTerrainHook(lua_State* L,const char* hook,const char* asset,bool* cancel) {
    struct Restore { lua_State* state; int top; ~Restore() { lua_settop(state,top); } } restore{L,lua_gettop(L)};
    lua_getglobal(L,"Hooks");
    if (!lua_istable(L,-1)) return false;
    lua_getfield(L,-1,"_FireEx");
    if (!lua_isfunction(L,-1)) return false;
    lua_pushstring(L,hook);
    lua_pushstring(L,asset);
    if (lua_pcall(L,2,3,0)!=LUA_OK) {
        LOG_ERR("EngineRes: %s Lua fault: %s",hook,lua_tostring(L,-1));
        return false;
    }
    const char* mode=lua_tostring(L,-2);
    if (!mode || (std::strcmp(mode,"pass")!=0 && std::strcmp(mode,"return")!=0)) return false;
    *cancel=std::strcmp(mode,"return")==0;
    // assetName is read-only in VU; a Pass result cannot redirect the native loader.
    return true;
}
bool TerrainLoadCancelled(bool visual,const char* asset,bool* cancelled) {
    NativeIdentityScope identity(g_identityActive?g_identityLevel:NativeLoadingLevel());
    auto& runtime=LuaRuntime::Instance();
    std::unique_lock<std::recursive_mutex> lock(runtime.TickGate(),std::defer_lock);
    const ULONGLONG deadline=::GetTickCount64()+100;
    while (!lock.try_lock()) {
        if (::GetTickCount64()>=deadline) return false;
        ::Sleep(1);
    }
    lua_State* L=runtime.state();
    return L && ApplyTerrainHook(L,visual?"VisualTerrain:Load":"Terrain:Load",asset,cancelled);
}
using TerrainLoadNativeFn=unsigned long(__fastcall*)(unsigned long,unsigned long,unsigned long,unsigned long,
    unsigned long,unsigned long,unsigned long,unsigned long);
unsigned long TerrainLoad(bool visual,unsigned long self,unsigned long edx,unsigned long a1,unsigned long a2,
    unsigned long name,unsigned long a4,unsigned long a5,unsigned long a6) {
    const auto original=(TerrainLoadNativeFn)(visual?g_trampVisualTerrain:g_trampTerrain);
    if (g_resourceHooksReady==1 && !EnvOff("EVBOTS_RESOURCE_MUTATION") && !EnvOff("EVBOTS_TERRAIN_HOOKS")) {
        char asset[4096]={}; bool cancel=false;
        if (!CopyEngineText(name,asset,sizeof(asset)) || !TerrainLoadCancelled(visual,asset,&cancel)) {
            LOG_ERR("EngineRes: %s delivery failed; original loader unchanged",visual?"VisualTerrain:Load":"Terrain:Load");
        } else if (cancel) {
            const unsigned long resource=MakeCancelledTerrain(visual);
            if (resource) {
                LOG_INFO("EngineRes: %s cancelled asset %s",visual?"VisualTerrain:Load":"Terrain:Load",asset);
                return resource;
            }
            LOG_ERR("EngineRes: cancelled Resource allocation failed; original loader unchanged");
        }
    }
    return original(self,edx,a1,a2,name,a4,a5,a6);
}
unsigned long __fastcall DetourTerrainLoad(unsigned long self,unsigned long edx,unsigned long a1,unsigned long a2,
    unsigned long name,unsigned long a4,unsigned long a5,unsigned long a6) {
    return TerrainLoad(false,self,edx,a1,a2,name,a4,a5,a6);
}
unsigned long __fastcall DetourVisualTerrainLoad(unsigned long self,unsigned long edx,unsigned long a1,unsigned long a2,
    unsigned long name,unsigned long a4,unsigned long a5,unsigned long a6) {
    return TerrainLoad(true,self,edx,a1,a2,name,a4,a5,a6);
}
void __fastcall DetourVisualTerrainPost(unsigned long self,unsigned long edx,unsigned long resource,
    unsigned long a2,unsigned long a3) {
    // This guard must survive env changes / mutation faults while cancelled
    // resources still exist. Stock postLoad writes +C99/+CD0 of real VisualTerrain.
    if (IsCancelledTerrain(resource)) return;
    using Fn=void(__fastcall*)(unsigned long,unsigned long,unsigned long,unsigned long,unsigned long);
    ((Fn)g_trampVisualTerrainPost)(self,edx,resource,a2,a3);
}
bool DeliverLoadingInfo(const char* screenInfo) {
    NativeIdentityScope identity(NativeLoadingLevel());
    auto& runtime=LuaRuntime::Instance();
    std::unique_lock<std::recursive_mutex> lock(runtime.TickGate(),std::defer_lock);
    const ULONGLONG deadline=::GetTickCount64()+100;
    while (!lock.try_lock()) {
        if (::GetTickCount64()>=deadline) return false;
        ::Sleep(1);
    }
    // The synchronous loading boundary must observe every preceding partition.
    // The flush is bounded and reports loss/pending work instead of allowing the
    // mod to construct its WorldPart from a partial partition set.
    if (!ResourceFlushPartitionEvents()) return false;
    return runtime.DispatchEngineEvent("Level:LoadingInfo",{LuaRuntime::EngineEventArg::Str(screenInfo)});
}
void __fastcall DetourLoadingInfo(unsigned long self,unsigned long edx,unsigned long screenInfo) {
    // vu-core101AA340 dispatches before original01196180. Both level state
    // machines call this with "Registering entity resources" before registration.
    if (g_resourceHooksReady==1 && !EnvOff("EVBOTS_RESOURCE_MUTATION")) {
        char text[4096]={};
        if (!CopyEngineText(screenInfo,text,sizeof(text)) || !DeliverLoadingInfo(text))
            LOG_ERR("EngineRes: Level:LoadingInfo delivery failed; no deferred substitute");
    }
    using Fn=void(__fastcall*)(unsigned long,unsigned long,unsigned long);
    ((Fn)g_trampLoadingInfo)(self,edx,screenInfo);
}

// Passive client counterpart of Level.OutputChecksumDebugLog. The compiler
// inlines 0118DAF0 into the main walk at 0119A350, so observe its two serializers
// from BOTH verified call sites. cdecl, two args, return value preserved.
InlineHook g_hookChecksumName, g_hookChecksumGuid;
volatile LONG g_checksumTraceCount = 0;
using ChecksumWriteFn = unsigned long(__cdecl*)(unsigned long, unsigned long);
void TraceChecksumInput(bool isGuid, unsigned long state, unsigned long value) {
    const LONG index = InterlockedIncrement(&g_checksumTraceCount);
    if (index <= 100000) {
        char text[256] = {};
        if (isGuid) {
            Guid16 guid = {};
            bool valid = true;
            for (unsigned i = 0; i < 4; ++i) {
                unsigned long word = 0;
                if (!ReadU32(value + i * 4, &word)) { valid = false; break; }
                std::memcpy(guid.b + i * 4, &word, 4);
            }
            if (valid) FormatGuid(guid, text);
            else strcpy_s(text, "<unreadable>");
        } else if (!CopyEngineText(value, text, sizeof(text))) strcpy_s(text, "<unreadable>");
        LOG_INFO("CHECKSUM-%s n=%ld ctx=%08lX value=%s", isGuid ? "GUID" : "TYPE", index, state, text);
    } else if (index == 100001) {
        LOG_WARN("CHECKSUM-INPUT truncated after 100000 writes");
    }
}
unsigned long __cdecl DetourChecksumName(unsigned long state, unsigned long name) {
    const auto caller = reinterpret_cast<unsigned long>(_ReturnAddress());
    if (caller == Va(0x0119A38C) || caller == Va(0x0118DB00)) TraceChecksumInput(false, state, name);
    return reinterpret_cast<ChecksumWriteFn>(g_hookChecksumName.Trampoline())(state, name);
}
unsigned long __cdecl DetourChecksumGuid(unsigned long state, unsigned long guid) {
    const auto caller = reinterpret_cast<unsigned long>(_ReturnAddress());
    if (caller == Va(0x0119A39A) || caller == Va(0x0118DB12)) TraceChecksumInput(true, state, guid);
    return reinterpret_cast<ChecksumWriteFn>(g_hookChecksumGuid.Trampoline())(state, guid);
}

void InstallChecksumTrace() {
    const char* enabled = std::getenv("EVBOTS_CHECKSUM_TRACE");
    if (g_a != &kClient || enabled == nullptr || std::strcmp(enabled, "1") != 0 ||
        g_hookChecksumName.IsInstalled()) return;
    const struct { unsigned long address; unsigned char bytes[16]; } signatures[] = {
        {0x0046E0F0,{0x8B,0x54,0x24,0x08,0x56,0x8B,0xC2,0x57,0x8D,0x70,0x01,0xEB,0x03,0x8D,0x49,0x00}},
        {0x0046E180,{0x56,0x8B,0x74,0x24,0x08,0x8B,0x06,0x8B,0x50,0x20,0x57,0x8B,0x7C,0x24,0x10,0x8B}},
        {0x0119A387,{0xE8,0x64,0x3D,0x2D,0xFF,0x83,0xC4,0x08,0x85,0xFF,0x74,0x0A,0x57,0x55,0xE8,0xE6}},
        {0x0119A395,{0xE8,0xE6,0x3D,0x2D,0xFF,0x83,0xC4,0x08,0x8B,0x16,0x8B,0x02,0x8B,0xCE,0xFF,0xD0}},
        {0x0118DAFB,{0xE8,0xF0,0x05,0x2E,0xFF,0x8B,0x44,0x24,0x10,0x83,0xC4,0x08,0x85,0xC0,0x74,0x0A}},
        {0x0118DB0D,{0xE8,0x6E,0x06,0x2E,0xFF,0x83,0xC4,0x08,0x5E,0xC2,0x0C,0x00,0xCC,0xCC,0xCC,0xCC}},
    };
    bool matches = true;
    for (const auto& signature : signatures) {
        const bool match = MutationBytesMatch(signature.address, signature.bytes);
        LOG_INFO("CHECKSUM-INPUT: signature %08lX %s", signature.address, match ? "PASS" : "FAIL");
        matches = matches && match;
    }
    if (!matches) { LOG_ERR("CHECKSUM-INPUT: serializer/caller signature mismatch; observers not installed"); return; }
    const HookResult nameResult = g_hookChecksumName.Install(reinterpret_cast<void*>(Va(0x0046E0F0)),
        reinterpret_cast<void*>(&DetourChecksumName), "EngineResources:ChecksumNameTrace");
    if (nameResult != HookResult::Ok) {
        LOG_ERR("CHECKSUM-INPUT: name observer failed: %s", ToString(nameResult)); return;
    }
    const HookResult guidResult = g_hookChecksumGuid.Install(reinterpret_cast<void*>(Va(0x0046E180)),
        reinterpret_cast<void*>(&DetourChecksumGuid), "EngineResources:ChecksumGuidTrace");
    if (guidResult != HookResult::Ok) {
        const bool reverted = g_hookChecksumName.Remove();
        LOG_ERR("CHECKSUM-INPUT: GUID observer failed: %s; name rollback=%s", ToString(guidResult), reverted ? "ok" : "FAILED");
        return;
    }
    LOG_INFO("CHECKSUM-INPUT: serializers and four call sites PASS; observers installed");
}

bool ReadLoadIdentity(unsigned long level, char* name, size_t nameCap, char* mode, size_t modeCap) {
    unsigned long ptr = 0, opts = 0, count = 0;
    if (!name || !mode || nameCap==0 || modeCap==0) return false;
    name[0] = mode[0] = '\0';
    if (level<0x10000 || g_a==nullptr) return false;
    const unsigned nameOffset=g_a==&kServer?kServerMutation.levelNameOffset:kClientMutation.levelNameOffset;
    if (!ReadU32(level + nameOffset, &ptr) ||
        !CopyEngineText(ptr, name, nameCap) || name[0] == '\0') return false;
    if (!ReadU32(level + nameOffset + 4, &opts) || opts < 0x10000 ||
        !ReadU32(opts - 4, &count) || count > 64) return false;
    for (unsigned long i = 0; i < count; ++i) {
        char key[64] = {};
        if (!ReadU32(opts + i * 8, &ptr) || !CopyEngineText(ptr, key, sizeof(key))) continue;
        if (std::strcmp(key, "GameMode") != 0) continue;
        return ReadU32(opts + i * 8 + 4, &ptr) && CopyEngineText(ptr, mode, modeCap);
    }
    // VU core101A1940: default mode is empty; missing GameMode still dispatches
    // E660B83E at101A1B11 before original. Web_Loading must reach mod cleanup.
    return true;
}

using LoadResourcesNativeFn = bool(__fastcall*)(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long);
bool __fastcall DetourLoadResources(unsigned long self, unsigned long edx, unsigned long a1,
                                   unsigned long a2, unsigned long dedicated) {
    NativeIdentityScope identity(self);
    char name[512] = {}, mode[128] = {};
    if (g_resourceHooksReady == 1 && g_onLoadResources != nullptr) {
        if (ReadLoadIdentity(self, name, sizeof(name), mode, sizeof(mode))) {
            ResourcePhaseScope phase(ResourcePhase::Load);
            if (!g_onLoadResources(name, mode, (dedicated & 0xFF) != 0)) {
                LOG_ERR("EngineRes: Level:LoadResources(%s, %s) НЕ доставлен; повтор после начала загрузки запрещён", name, mode);
            }
        } else {
            LOG_ERR("EngineRes: Level:LoadResources — имя/options недоступны; событие не подменяю");
        }
    }
    return ((LoadResourcesNativeFn)g_trampLoadResources)(self, edx, a1, a2, dedicated);
}

using RegisterResourcesNativeFn = void(__stdcall*)(unsigned long);
void __stdcall DetourRegisterResources(unsigned long levelData) {
    NativeIdentityScope identity(NativeLoadingLevel());
    ((RegisterResourcesNativeFn)g_trampRegisterResources)(levelData);
    if (g_resourceHooksReady == 1 && g_onRegisterResources != nullptr) {
        ResourcePhaseScope phase(ResourcePhase::Register);
        if (!g_onRegisterResources(levelData)) {
            LOG_ERR("EngineRes: Level:RegisterEntityResources НЕ доставлен; регистрация модовых ресурсов пропущена");
        }
    }
}

// vu-core 101A1C90: original имеет 6 cdecl аргументов. На 101A2040 VU дописывает
// nullptr после списка, 101A206C вызывает original, 101A2090..101A20B8 освобождает
// КАЖДУЮ строку и массив. Поэтому их владение заканчивается после синхронного вызова,
// даже если сама операция загрузки асинхронна. arg5/arg6 и outOperation не меняем.
using LoadBundlesNativeFn = unsigned long(__cdecl*)(unsigned long outOperation, int compartment,
    const char* const* bundles, int count, unsigned long arg5, unsigned long arg6);

bool CopyBundleNames(const char* const* names, int count, std::vector<std::string>* out) {
    constexpr int kMaxBundles = 4096;
    if (count < 0 || count > kMaxBundles || (count != 0 && names == nullptr)) return false;
    out->reserve((size_t)count);
    for (int i = 0; i < count; ++i) {
        unsigned long ptr = 0;
        char text[4096] = {};
        if (!ReadU32((unsigned long)(ULONG_PTR)names + (unsigned long)i * 4, &ptr) ||
            !CopyEngineText(ptr, text, sizeof(text)) || text[0] == '\0') return false;
        out->emplace_back(text);
    }
    return true;
}

unsigned long __cdecl DetourLoadBundles(unsigned long outOperation, int compartment,
    const char* const* bundles, int count, unsigned long arg5, unsigned long arg6) {
    const auto original = (LoadBundlesNativeFn)g_trampLoadBundles;
    if (g_resourceHooksReady != 1 || g_onLoadBundles == nullptr || EnvOff("EVBOTS_RESOURCE_MUTATION"))
        return original(outOperation, compartment, bundles, count, arg5, arg6);

    std::vector<std::string> replacement;
    if (!CopyBundleNames(bundles, count, &replacement)) {
        LOG_ERR("EngineRes: LoadBundles — входной массив не прочитан; hook пропущен, original без замены");
        return original(outOperation, compartment, bundles, count, arg5, arg6);
    }
    int replacementCompartment = compartment;
    bool applied = false;
    {
        NativeIdentityScope identity(g_identityActive?g_identityLevel:NativeLoadingLevel());
        ResourcePhaseScope phase(ResourcePhase::Bundles);
        applied = g_onLoadBundles(&replacement, &replacementCompartment);
    }
    if (!applied) {
        LOG_ERR("EngineRes: ResourceManager:LoadBundles не обработан; original без замены списка");
        return original(outOperation, compartment, bundles, count, arg5, arg6);
    }
    std::vector<const char*> pointers;
    pointers.reserve(replacement.size() + 1);
    for (const auto& name : replacement) pointers.push_back(name.c_str());
    pointers.push_back(nullptr);
    if ((int)replacement.size() != count || replacementCompartment != compartment) {
        LOG_INFO("EngineRes: LoadBundles Pass: %d -> %u bundles, compartment %d -> %d; вызов original синхронно",
                 count, (unsigned)replacement.size(), compartment, replacementCompartment);
    }
    return original(outOperation, replacementCompartment, pointers.data(), (int)replacement.size(), arg5, arg6);
}

bool MutationFailure(const char** error, const char* message) {
    if (error != nullptr) *error = message;
    return false;
}

// SEH отдельно от функций с C++-деструкторами. Это защита диагностики, не откат
// частично выполненной операции движка: после fault весь новый слой закрывается.
bool CallNativeMount(const char* name, int hint, bool optional) {
    __try {
        using GetManagerFn = unsigned long(__cdecl*)();
        const unsigned long manager = ((GetManagerFn)(ULONG_PTR)Va(g_mutation->mountManager))();
        unsigned long vt = 0, fn = 0;
        if (manager == 0 || !ReadU32(manager, &vt) || !ReadU32(vt, &fn) ||
            !IsExecutable((void*)(ULONG_PTR)fn)) return false;
        using MountFn = void(__cdecl*)(const char*, int, bool);
        ((MountFn)(ULONG_PTR)Va(g_mutation->mount))(name, hint, optional);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool CallNativeAddRegistry(unsigned long registry, int compartment) {
    __try {
        using AddRegistryFn = void(__cdecl*)(unsigned long, int);
        ((AddRegistryFn)(ULONG_PTR)Va(g_mutation->addRegistry))(registry, compartment);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

class RegistryLockScope {
public:
    RegistryLockScope() : gate_((CRITICAL_SECTION*)(ULONG_PTR)Va(g_mutation->registryLock)) {
        ::EnterCriticalSection(gate_);
    }
    ~RegistryLockScope() { ::LeaveCriticalSection(gate_); }
    RegistryLockScope(const RegistryLockScope&)=delete;
    RegistryLockScope& operator=(const RegistryLockScope&)=delete;
private:
    CRITICAL_SECTION* gate_;
};

bool RegistrySnapshot(unsigned long registry, std::set<unsigned long>* objects) {
    const unsigned long type=TypeOf(registry);
    const char* fields[]={"EntityRegistry","AssetRegistry","BlueprintRegistry","ReferenceObjectRegistry"};
    objects->insert(registry);
    for (const char* name:fields) {
        FieldDesc field;
        TypeDesc array,element;
        unsigned long first=0,count=0;
        if (!FindField(type,name,&field) || !Describe(field.type,&array) || array.kind!=kArray ||
            !Describe(array.element,&element) || element.kind!=kClass ||
            !ReadU32(registry+field.offset,&first) || !first || (first&3) ||
            !ReadU32(first-4,&count) || count>1000000) return false;
        for (unsigned long i=0;i<count;++i) {
            unsigned long object=0;
            if (!ReadU32(first+i*4,&object)) return false;
            if (object) {
                if ((object&3) || !TypeOf(object)) return false;
                objects->insert(object);
            }
        }
    }
    return true;
}

bool PinRegistrySnapshot(const std::set<unsigned long>& objects) {
    std::vector<unsigned long> acquired;
    acquired.reserve(objects.size());
    for (unsigned long object:objects) {
        if (g_registryPins.find(object)!=g_registryPins.end()) continue;
        if (!ResourceRetainLive(object)) {
            for (unsigned long pin:acquired) {
                g_registryPins.erase(pin);
                ResourceQueueRelease(pin);
            }
            return false;
        }
        acquired.push_back(object);
        g_registryPins.insert(object);
    }
    return true;
}

void QueueRegistryPins() {
    // Native pointer/id tables borrow these objects; Lua GC precedes their final clear.
    std::set<unsigned long> pins;
    pins.swap(g_registryPins);
    for (unsigned long object:pins) ResourceQueueRelease(object);
    if (!pins.empty()) LOG_INFO("EngineRes: final registry exit: %u pins queued after native table clear",(unsigned)pins.size());
}

using RegistryExitNativeFn=void(__cdecl*)(unsigned long);
void __cdecl DetourRegistryExit(unsigned long argument) {
    auto* gate=(CRITICAL_SECTION*)(ULONG_PTR)Va(g_mutation->registryLock);
    ::EnterCriticalSection(gate);
    __try {
        unsigned long before=0,after=0;
        const bool haveBefore=ReadU32(Va(g_mutation->registryActive),&before);
        ((RegistryExitNativeFn)g_hookRegistryExit.Trampoline())(argument);
        if (haveBefore && before==1 && ReadU32(Va(g_mutation->registryActive),&after) && after==0)
            QueueRegistryPins();
    } __finally { ::LeaveCriticalSection(gate); }
}

unsigned long DomainOf(int comp) {
    unsigned long c = 0, d = 0;
    if (!ReadU32(Va(kVaCompartments) + (unsigned long)comp * 4, &c) || c == 0) return 0;
    if (!ReadU32(c + 0x50, &d)) return 0;
    return d;
}

unsigned long CallFindPartitionByName(unsigned long domain, const char* name) {
    __try {
        return ((ThisCall1)(ULONG_PTR)Va(kVaFindPartitionByName))(domain, 0, (unsigned long)(ULONG_PTR)name);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

unsigned long CallFindInPartition(unsigned long partition, const Guid16* g) {
    __try {
        return ((ThisCall1)(ULONG_PTR)Va(kVaFindInPartition))(partition, 0, (unsigned long)(ULONG_PTR)g);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

unsigned long CallPartitionOf(unsigned long dc) {
    __try {
        const unsigned long dbm = *(const unsigned long*)(ULONG_PTR)Va(kVaDbManager);
        if (dbm == 0) return 0;
        return ((ThisCall1)(ULONG_PTR)Va(kVaPartitionOf))(dbm, 0, dc);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

unsigned long CallPartitionCount(unsigned long domain) {
    __try {
        return ((ThisCall0)(ULONG_PTR)Va(kVaPartitionCount))(domain, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

unsigned long CallPartitionList(unsigned long domain, unsigned long* out, unsigned long n) {
    __try {
        return ((ThisCall2)(ULONG_PTR)Va(kVaPartitionList))(domain, 0, (unsigned long)(ULONG_PTR)out, n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

unsigned long CallGetType(unsigned long dc) {
    __try {
        const unsigned long vt = *(const unsigned long*)(ULONG_PTR)dc;
        if (vt < Va(kImageBase) || vt >= Va(g_a->imageEnd)) return 0;  // vtable — в образе exe
        const unsigned long fn = *(const unsigned long*)(ULONG_PTR)vt;
        if (fn < Va(0x00401000UL) || fn >= Va(g_a->codeEnd)) return 0;  // код — в .text/ctr
        return ((ThisCall0)(ULONG_PTR)fn)(dc, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int HexVal(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int Stricmp(const char* a, const char* b) {
    return ::_stricmp(a, b);
}

/// Перебор разделов всех отсеков: fn(partition) → true — стоп.
/// Буфер — свой на каждый вызов: зовут из потока Lua и из потока загрузки.
template <typename Fn>
unsigned long ForEachPartition(Fn fn) {
    for (int c = 0; c < kCompartmentCount; ++c) {
        const unsigned long domain = DomainOf(c);
        if (domain == 0) continue;
        const unsigned long n = CallPartitionCount(domain);
        if (n == 0 || n > 200000) continue;
        const unsigned long cap = n + 256;  // между count и copy раздел мог добавиться
        unsigned long* buf = (unsigned long*)std::malloc(cap * sizeof(unsigned long));
        if (buf == nullptr) continue;
        const unsigned long got = CallPartitionList(domain, buf, cap);
        unsigned long r = 0;
        for (unsigned long i = 0; i < got && i < cap && r == 0; ++i) {
            if (buf[i] != 0) r = fn(buf[i]);
        }
        std::free(buf);
        if (r != 0) return r;
    }
    return 0;
}

// ── перехват загрузки раздела ───────────────────────────────────────────────
InlineHook g_hookBundlePartition;
void* g_trampBundlePartition = nullptr;
WatchedPartitionLoadedFn g_onPartition = nullptr;
PartitionLoadedFn g_onAnyPartition = nullptr;  // ★ Сборка 234: Partition:Loaded (все разделы)
volatile LONG g_hookCalls = 0;
volatile LONG g_hookMatches = 0;

constexpr int kWatchMax = 128;
Guid16 g_watchPart[kWatchMax];
volatile LONG g_watchPartCount = 0;
Guid16 g_watchInst[kWatchMax];
volatile LONG g_watchInstCount = 0;
CRITICAL_SECTION g_watchGate;
volatile LONG g_watchGateInit = 0;

void EnsureWatchGate() {
    if (InterlockedCompareExchange(&g_watchGateInit, 1, 0) == 0) {
        ::InitializeCriticalSection(&g_watchGate);
        InterlockedExchange(&g_watchGateInit, 2);
    }
    while (g_watchGateInit != 2) ::Sleep(0);
}

bool SameGuid(const Guid16& a, const Guid16& b) {
    return std::memcmp(a.b, b.b, 16) == 0;
}

bool PartitionWatched(unsigned long p) {
    Guid16 pg;
    const bool havePg = PartitionGuid(p, &pg);
    const LONG check = g_pgCheck;
    EnsureWatchGate();
    bool hit = false;
    ::EnterCriticalSection(&g_watchGate);
    if (havePg && check >= 0) {
        for (LONG i = 0; i < g_watchPartCount && !hit; ++i) hit = SameGuid(g_watchPart[i], pg);
    }
    if (!hit && check < 0) {
        // GUID раздела по +0x0C не подтвердился — ищем отслеживаемые инстансы прямо в разделе.
        for (LONG i = 0; i < g_watchInstCount && !hit; ++i) hit = CallFindInPartition(p, &g_watchInst[i]) != 0;
    }
    ::LeaveCriticalSection(&g_watchGate);
    return hit;
}

InlineHook g_hookEndPostChunks;
void* g_trampEndPostChunks = nullptr;
volatile LONG g_endPostCalls = 0;

// При restart новый раздел может занять тот же адрес. Native PostMarshal
// открывает доставку только для реально перезагруженного раздела.
struct PartitionDelivery {
    unsigned long partition = 0, generation = 0;
};
constexpr int kDeliveredMax = 512;
PartitionDelivery g_delivered[kDeliveredMax];
SRWLOCK g_deliveryGate = SRWLOCK_INIT;
int g_deliveredCount = 0;
int g_deliveredNext = 0;
unsigned long g_deliveryGeneration = 0;

unsigned long TakeDelivered(unsigned long p) {
    AcquireSRWLockExclusive(&g_deliveryGate);
    for (int i = 0; i < g_deliveredCount; ++i) {
        auto& entry = g_delivered[i];
        if (entry.partition != p) continue;
        const unsigned long generation = entry.generation ? 0 : ++g_deliveryGeneration;
        if (generation) entry.generation = generation;
        ReleaseSRWLockExclusive(&g_deliveryGate);
        return generation;
    }
    const unsigned long generation = ++g_deliveryGeneration;
    g_delivered[g_deliveredNext] = {p, generation};
    g_deliveredNext = (g_deliveredNext + 1) % kDeliveredMax;
    if (g_deliveredCount < kDeliveredMax) ++g_deliveredCount;
    ReleaseSRWLockExclusive(&g_deliveryGate);
    return generation;
}

void ReopenPartitionDelivery(unsigned long p) {
    AcquireSRWLockExclusive(&g_deliveryGate);
    for (int i = 0; i < g_deliveredCount; ++i) {
        auto& entry = g_delivered[i];
        if (entry.partition == p) {
            entry.generation = 0;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_deliveryGate);
}

using ThisCallVoid = void(__fastcall*)(unsigned long self, unsigned long edx);

bool ProbeUnresolvedCallback(unsigned long domain) {
    if(g_a!=&kServer || !domain || domain>0xFFFFFF9FUL)return false;
    unsigned long buckets=0,bucketCount=0,sentinel=0;
    if(!ReadU32(domain+0x20,&buckets) || !buckets || buckets>0xFFFFFF00UL ||
        !ReadU32(domain+0x24,&bucketCount) || bucketCount>65536 ||
        buckets>0xFFFFFFFFUL-bucketCount*4 || !ReadU32(buckets+bucketCount*4,&sentinel))return false;
    unsigned nodes=0,records=0;
    for(unsigned long bucket=0;bucket<bucketCount && bucket<64 && nodes<64 && records<256;++bucket) {
        unsigned long node=0;
        if(!ReadU32(buckets+bucket*4,&node))return false;
        while(node && node!=sentinel && nodes<64 && records<256) {
            ++nodes;
            unsigned long first=0,last=0;
            if(node>0xFFFFFF9FUL || !ReadU32(node+0x30,&first) || !ReadU32(node+0x34,&last) ||
                last<first || (last-first)%0x10)return false;
            for(unsigned long record=first;record<last && records<256;record+=0x10) {
                ++records;
                unsigned long receiver=0,vtable=0,callback=0;
                if(!ReadU32(record+4,&receiver) || !receiver || !ReadU32(receiver,&vtable) ||
                    !vtable || vtable>0xFFFFFFEFUL || !ReadU32(vtable+0x10,&callback) || !callback)continue;
                unsigned long slot=0,raw=0,resolved=0;
                const bool slotReadable=ReadU32(record,&slot) && slot && ReadU32(slot,&raw);
                const unsigned long proxy=slotReadable && (raw&1)?raw&~3UL:0;
                bool resolvedReadable=slotReadable;
                if(raw&1)resolvedReadable=proxy && proxy<=0xFFFFFFEFUL && ReadU32(proxy+0x10,&resolved);
                else resolved=raw;
                LOG_INFO("EngineRes: EBX unresolved callback domain=%08lX node=%08lX record=%08lX receiver=%08lX vtable=%08lX callback=%08lX slot=%08lX slotReadable=%u raw=%08lX proxy=%08lX resolved=%08lX resolvedReadable=%u buckets=%lu scannedNodes=%u scannedRecords=%u",
                    domain,node,record,receiver,vtable,callback,slot,slotReadable?1u:0u,
                    raw,proxy,resolved,resolvedReadable?1u:0u,bucketCount,nodes,records);
                return true;
            }
            if(!ReadU32(node+0x60,&node))return false;
        }
    }
    return false;
}

void ProbeDomainSerialization(unsigned long domain) {
    if(g_a!=&kServer || !domain)return;
    unsigned long manager=0,globalManager=0,owner=0,recursion=0,lockCount=0;
    const bool managerReadable=ReadU32(domain+0x40,&manager) && manager;
    const bool globalReadable=ReadU32(Va(kVaDbManager),&globalManager);
    const unsigned long gate=manager+0x10;
    const bool lockReadable=managerReadable &&
        ReadU32(gate+FIELD_OFFSET(CRITICAL_SECTION,OwningThread),&owner) &&
        ReadU32(gate+FIELD_OFFSET(CRITICAL_SECTION,RecursionCount),&recursion) &&
        ReadU32(gate+FIELD_OFFSET(CRITICAL_SECTION,LockCount),&lockCount);
    const unsigned long thread=::GetCurrentThreadId();
    LOG_INFO("EngineRes: EBX serialization domain=%08lX manager=%08lX managerReadable=%u globalManager=%08lX globalReadable=%u sameManager=%u gate=%08lX lockReadable=%u owner=%08lX thread=%08lX recursion=%lu lockCount=%ld ownerMatchesThread=%u",
        domain,manager,managerReadable?1u:0u,globalManager,globalReadable?1u:0u,
        managerReadable && globalReadable && manager==globalManager?1u:0u,gate,
        lockReadable?1u:0u,owner,thread,recursion,(long)lockCount,
        lockReadable && recursion && owner==thread?1u:0u);
}

void ProbeDomainArena(unsigned long domain) {
    if(g_a!=&kServer || !domain)return;
    ProbeDomainSerialization(domain);
    const unsigned long context=domain+0xBC;
    unsigned long parent=0,head=0,checkpoint=0,blockSize=0,flags=0,next=0;
    unsigned mode=0;
    const bool readable=ReadU32(context,&parent) && ReadU32(context+4,&head) &&
        ReadU32(context+8,&checkpoint) && ReadU32(context+0xC,&blockSize) && ReadU8(context+0x10,&mode);
    const bool allocatorReadable=readable && parent && ReadU32(parent+4,&flags) && ReadU32(parent+8,&next);
    LOG_INFO("EngineRes: EBX arena domain=%08lX context=%08lX readable=%u parent=%08lX allocatorReadable=%u flags=%08lX next=%08lX head=%08lX checkpoint=%08lX blockSize=%lu virtualMode=%u",
        domain,context,readable?1u:0u,parent,allocatorReadable?1u:0u,flags,next,head,checkpoint,blockSize,mode);
}

void __fastcall DetourPostMarshal(unsigned long self, unsigned long edx) {
    ((ThisCallVoid)g_hookPostMarshal.Trampoline())(self,edx);
    unsigned long partition=0,domain=0,first=0,last=0;
    unsigned partitionFlags=0,domainFlags=0;
    const bool hasPartition=ReadU32(self+4,&partition) && partition;
    if (hasPartition) ReopenPartitionDelivery(partition);
    const bool readable=hasPartition &&
        ReadU32(partition+8,&domain) && domain &&
        ReadU8(partition+(g_a==&kServer?0xB0:0x5C),&partitionFlags) &&
        ReadU8(domain+0x3C,&domainFlags) && ReadU32(partition+0x3C,&first) && ReadU32(partition+0x40,&last);
    static volatile LONG callbackSeen=0,callbackProbes=0;
    if(g_a==&kServer && readable && InterlockedCompareExchange(&callbackSeen,0,0)==0 &&
        InterlockedIncrement(&callbackProbes)<=2048 && ProbeUnresolvedCallback(domain))
        InterlockedExchange(&callbackSeen,1);
    const unsigned combination=((partitionFlags&1)?2u:0u)+((domainFlags&8)?1u:0u);
    static volatile LONG seen=0,calls=0;
    const LONG mask=1L<<combination;
    const bool fresh=(InterlockedOr(&seen,mask)&mask)==0;
    if (InterlockedIncrement(&calls)<=12 || fresh || !readable) {
        LOG_INFO("EngineRes: EBX post-marshal reader=%08lX partition=%08lX domain=%08lX readable=%u partitionFlags=%02X domainFlags=%02X instances=%lu clearRefs=%u",
            self,partition,domain,readable?1u:0u,partitionFlags,domainFlags,
            last>=first?(last-first)/4:0,readable && (partitionFlags&1) && !(domainFlags&8)?1u:0u);
        ProbeDomainArena(domain);
    }
}

void __fastcall DetourEndPostChunks(unsigned long self, unsigned long edx) {
    ((ThisCallVoid)g_trampEndPostChunks)(self, edx);
    const LONG calls = InterlockedIncrement(&g_endPostCalls);
    if (g_onPartition == nullptr || (g_watchPartCount == 0 && g_watchInstCount == 0)) return;
    unsigned long domain = 0;
    if (!ReadU32(self + 0x50, &domain) || domain == 0) return;
    const unsigned long n = CallPartitionCount(domain);
    if (n == 0 || n > 200000) return;
    const unsigned long cap = n + 256;
    unsigned long* buf = (unsigned long*)std::malloc(cap * sizeof(unsigned long));
    if (buf == nullptr) return;
    const unsigned long got = CallPartitionList(domain, buf, cap);
    std::vector<PartitionDelivery> hits;
    for (unsigned long i = 0; i < got && i < cap; ++i) {
        if (buf[i] == 0 || !PartitionWatched(buf[i])) continue;
        const unsigned long generation = TakeDelivered(buf[i]);
        if (generation) hits.push_back({buf[i], generation});
    }
    std::free(buf);
    if (!hits.empty() || calls <= 3) {
        LOG_INFO("EngineRes: конец загрузки бандла (#%ld, отсек %p, разделов %lu, поток %lu): "
                 "отслеживаемых новых разделов %d",
                 (long)calls, (void*)(ULONG_PTR)self, got, (unsigned long)::GetCurrentThreadId(), static_cast<int>(hits.size()));
    }
    ResourcePhaseScope phase(ResourcePhase::InstanceLoad);
    for (const auto& hit : hits) {
        InterlockedIncrement(&g_hookMatches);
        g_onPartition(hit.partition, "", hit.generation);
    }
}

void DeliverPreloadedWatchedPartitionsImpl() {
    if (!g_onPartition || (!g_watchPartCount && !g_watchInstCount)) return;
    // Never hold a domain/watch lock while calling Lua. Handlers may register
    // additional watches; another pass picks those up, with the normal de-dup.
    unsigned total = 0;
    for (unsigned pass = 0; pass < kWatchMax * 2; ++pass) {
        std::vector<PartitionDelivery> fresh;
        ForEachPartition([&fresh](unsigned long partition) -> unsigned long {
            if (!PartitionWatched(partition)) return 0;
            const unsigned long generation = TakeDelivered(partition);
            if (generation) fresh.push_back({partition, generation});
            return 0;
        });
        if (fresh.empty()) break;
        ResourcePhaseScope phase(ResourcePhase::InstanceLoad);
        for (const auto& hit : fresh) {
            InterlockedIncrement(&g_hookMatches);
            g_onPartition(hit.partition, "", hit.generation);
            ++total;
        }
    }
    LOG_INFO("EngineRes: preloaded watched partition catch-up delivered=%u (native InstanceLoad, de-duplicated)", total);
    // Read-only proof of the runtime settings object, as consumed by server
    // 1164A20/116A050: information(+C), sizes(+4), GameModeSize stride1C,
    // teams(+10). This distinguishes patching the EBX asset from its live copy.
    const unsigned long settings = FindSettings("GameModeSettings");
    int layout = 0;
    const unsigned long asset = FindInstanceByGuid("AD413546-DEAF-8115-B89C-D666E801C67A", &layout);
    auto teamCount = [](unsigned long object) -> unsigned long {
        unsigned long information=0,sizes=0,teams=0,count=0;
        if (!object || !ReadU32(object+0xC,&information) || information<0x10000 ||
            !ReadU32(information-4,&count) || !count || count>128 ||
            !ReadU32(information+4,&sizes) || sizes<0x10000 ||
            !ReadU32(sizes-4,&count) || count<3 || count>128 ||
            !ReadU32(sizes+2*0x1C+0x10,&teams) || teams<0x10000 ||
            !ReadU32(teams-4,&count)) return 0;
        return count;
    };
    LOG_INFO("EngineRes: GameModeSettings after catch-up runtime=%p teams=%lu asset=%p teams=%lu same=%u",
        (void*)(ULONG_PTR)settings,teamCount(settings),(void*)(ULONG_PTR)asset,teamCount(asset),settings && settings==asset?1u:0u);
}

unsigned long __fastcall DetourOnBundlePartition(unsigned long self, unsigned long edx, const char* name,
                                                 unsigned long a2, unsigned long a3, unsigned long a4) {
    const unsigned long r =
        ((OnBundlePartitionFn)g_trampBundlePartition)(self, edx, name, a2, a3, a4);
    const LONG calls = InterlockedIncrement(&g_hookCalls);
    if (calls == 1) {
        LOG_INFO("EngineRes: ★ первая загрузка раздела после перехвата: «%s», поток %lu",
                 name != nullptr ? name : "?", (unsigned long)::GetCurrentThreadId());
    }
    if (name == nullptr) return r;
    const bool any = g_onAnyPartition != nullptr;
    if (!any && (g_onPartition == nullptr || (g_watchPartCount == 0 && g_watchInstCount == 0))) return r;
    unsigned long domain = 0;
    if (!ReadU32(self + 0x50, &domain) || domain == 0) return r;
    const unsigned long p = CallFindPartitionByName(domain, name);
    if (p == 0) return r;
    if (any) g_onAnyPartition(p, name);  // ★ Сборка 234: Partition:Loaded — каждый раздел
    if (g_onPartition == nullptr || (g_watchPartCount == 0 && g_watchInstCount == 0)) return r;
    if (!PartitionWatched(p)) return r;
    const unsigned long generation = TakeDelivered(p);
    if (!generation) return r;
    InterlockedIncrement(&g_hookMatches);
    ResourcePhaseScope phase(ResourcePhase::InstanceLoad);
    g_onPartition(p, name, generation);
    return r;
}

}  // namespace

bool ParseGuid(const char* text, Guid16* win, Guid16* raw) {
    if (text == nullptr) return false;
    unsigned char t[16];
    int n = 0;
    int hi = -1;
    for (const char* s = text; *s != '\0'; ++s) {
        if (*s == '-' || *s == '{' || *s == '}' || *s == '(' || *s == ')' || *s == ' ') continue;
        const int v = HexVal(*s);
        if (v < 0) return false;
        if (hi < 0) {
            hi = v;
        } else {
            if (n >= 16) return false;
            t[n++] = (unsigned char)(hi * 16 + v);
            hi = -1;
        }
    }
    if (n != 16 || hi >= 0) return false;
    if (raw != nullptr) std::memcpy(raw->b, t, 16);
    if (win != nullptr) {
        win->b[0] = t[3]; win->b[1] = t[2]; win->b[2] = t[1]; win->b[3] = t[0];
        win->b[4] = t[5]; win->b[5] = t[4];
        win->b[6] = t[7]; win->b[7] = t[6];
        std::memcpy(win->b + 8, t + 8, 8);
    }
    return true;
}

void FormatGuid(const Guid16& g, char* out) {
    const unsigned char* b = g.b;
    std::snprintf(out, 37, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
                  b[3], b[2], b[1], b[0], b[5], b[4], b[7], b[6], b[8], b[9], b[10], b[11], b[12], b[13],
                  b[14], b[15]);
}

bool Ready() {
    if (g_ready >= 0) return g_ready == 1;
    g_base = reinterpret_cast<unsigned char*>(::GetModuleHandleA(kServer.exe));
    g_a = &kServer;
    if (g_base == nullptr) {
        // ★ Сборка 234: клиент (bf3.exe) — своя таблица адресов (В8)
        g_base = reinterpret_cast<unsigned char*>(::GetModuleHandleA(kClient.exe));
        g_a = &kClient;
    }
    if (g_base == nullptr) {
        InterlockedExchange(&g_ready, 0);  // тест-хост: слоя нет, без лога
        return false;
    }
    if (EnvOff("EVBOTS_ENGINE_RES")) {
        LOG_INFO("EngineRes: выключен (EVBOTS_ENGINE_RES=0) — поиск ассетов сканом, ResourceManager по индексу");
        InterlockedExchange(&g_ready, 0);
        return false;
    }
    int bad = 0;
    for (unsigned i = 0; i < g_a->prologueCount; ++i) {
        const Prologue& p = g_a->prologues[i];
        if (!PrologueOk(p)) {
            ++bad;
            LOG_ERR("EngineRes: %s: пролог %s (VA 0x%08lX) НЕ совпал с эталоном", g_a->exe, p.name, p.va);
        }
    }
    if (bad != 0) {
        LOG_ERR("EngineRes: %d прологов не совпало — слой ВЫКЛЮЧЕН целиком (правило 18)", bad);
        InterlockedExchange(&g_ready, 0);
        return false;
    }
    int comps = 0;
    for (int c = 0; c < kCompartmentCount; ++c) comps += DomainOf(c) != 0 ? 1 : 0;
    LOG_INFO("EngineRes: ★ %s: прологи %u/%u совпали; отсеков с доменом: %d — нативный поиск ресурсов включён",
             g_a->exe, g_a->prologueCount, g_a->prologueCount, comps);
    InterlockedExchange(&g_ready, 1);
    return true;
}

bool InstallResourceLifecycleHooks(LevelLoadResourcesFn load, LevelRegisterResourcesFn registration,
                                   ResourceLoadBundlesFn bundles) {
    if (!Ready() || load == nullptr || registration == nullptr || bundles == nullptr) {
        LOG_WARN("EngineRes: InstallResourceLifecycleHooks: пропуск (Ready=%d load=%p reg=%p bundles=%p)",
                 Ready() ? 1 : 0, (void*)load, (void*)registration, (void*)bundles);
        return false;
    }
    if (EnvOff("EVBOTS_RESOURCE_MUTATION") || EnvOff("EVBOTS_TERRAIN_HOOKS")) {
        LOG_INFO("EngineRes: resource mutation выключен: EVBOTS_RESOURCE_MUTATION=0 или EVBOTS_TERRAIN_HOOKS=0");
        return false;
    }
    if (g_resourceHooksReady == 1) {
        LOG_INFO("EngineRes: InstallResourceLifecycleHooks: уже установлено");
        return true;
    }
    if (g_resourceHooksReady < 0) {
        LOG_WARN("EngineRes: InstallResourceLifecycleHooks: ранее не удалось (g_resourceHooksReady=%d)", (int)g_resourceHooksReady);
        return false;
    }
    LOG_INFO("EngineRes: InstallResourceLifecycleHooks: проверка native адресов (%s)...", g_a ? g_a->exe : "?");
    g_mutation = g_a == &kServer ? &kServerMutation : &kClientMutation;
    g_terrain = g_a == &kServer ? &kServerTerrain : &kClientTerrain;
    const unsigned long addresses[] = {g_mutation->mount, g_mutation->mountManager, g_mutation->addRegistry,
                                      g_mutation->loadResources, g_mutation->registerResources, g_mutation->loadBundles,
                                      g_mutation->registryExit, g_mutation->postMarshal};
    const char* names[] = {"MountSuperBundle", "MountManager", "AddRegistry", "Level::loadResources",
                          "Level::registerEntityResources", "ResourceManager::beginLoadData", "NetworkDataRegistry::exitLevel",
                          "StreamingPartitionReader::marshal"};
    bool allMatch = true;
    for (unsigned i = 0; i < 8; ++i) {
        const bool match = MutationBytesMatch(addresses[i], g_mutation->bytes[i]);
        LOG_INFO("EngineRes: %s %s 0x%08lX, 16 байт: %s", g_a->exe, names[i], addresses[i], match ? "PASS" : "FAIL");
        allMatch = allMatch && match;
    }
    const unsigned long terrainAddresses[]={g_terrain->load,g_terrain->visualLoad,g_terrain->visualPostLoad,g_terrain->loadingInfo};
    const char* terrainNames[]={"Terrain:Load","VisualTerrain:Load","VisualTerrain::postLoad","Level:LoadingInfo"};
    for (unsigned i=0;i<4;++i) {
        const bool match=MutationBytesMatch(terrainAddresses[i],g_terrain->bytes[i]);
        LOG_INFO("EngineRes: %s %s 0x%08lX, 16 bytes: %s",g_a->exe,terrainNames[i],terrainAddresses[i],match?"PASS":"FAIL");
        allMatch=allMatch && match;
    }
    TypeDesc terrainDesc,visualDesc;
    const bool types=Describe(Va(g_terrain->terrainType),&terrainDesc) && terrainDesc.kind==kClass &&
        std::strcmp(terrainDesc.name,"TerrainStreamingTree")==0 &&
        Describe(Va(g_terrain->visualType),&visualDesc) && visualDesc.kind==kClass &&
        std::strcmp(visualDesc.name,"VisualTerrain")==0;
    LOG_INFO("EngineRes: native Terrain/VisualTerrain TypeInfo: %s",types?"PASS":"FAIL");
    allMatch=allMatch && types;
    if (!allMatch) {
        InterlockedExchange(&g_resourceHooksReady, -1);
        LOG_ERR("EngineRes: фазы загрузки не ставлю — весь набор native адресов должен совпасть");
        return false;
    }
    g_onLoadResources = load;
    g_onRegisterResources = registration;
    g_onLoadBundles = bundles;
    struct PendingHook { InlineHook* hook; unsigned long va; void* detour; void** trampoline; const char* name; };
    PendingHook pending[]={
        {&g_hookRegistryExit,g_mutation->registryExit,(void*)&DetourRegistryExit,nullptr,"EngineResources:RegistryExit"},
        {&g_hookPostMarshal,g_mutation->postMarshal,(void*)&DetourPostMarshal,nullptr,"EngineResources:PostMarshal"},
        {&g_hookLoadResources,g_mutation->loadResources,(void*)&DetourLoadResources,&g_trampLoadResources,"EngineResources:LevelLoadResources"},
        {&g_hookRegisterResources,g_mutation->registerResources,(void*)&DetourRegisterResources,&g_trampRegisterResources,"EngineResources:LevelRegisterResources"},
        {&g_hookLoadBundles,g_mutation->loadBundles,(void*)&DetourLoadBundles,&g_trampLoadBundles,"EngineResources:ResourceLoadBundles"},
        {&g_hookTerrain,g_terrain->load,(void*)&DetourTerrainLoad,&g_trampTerrain,"EngineResources:TerrainLoad"},
        {&g_hookVisualTerrain,g_terrain->visualLoad,(void*)&DetourVisualTerrainLoad,&g_trampVisualTerrain,"EngineResources:VisualTerrainLoad"},
        {&g_hookVisualTerrainPost,g_terrain->visualPostLoad,(void*)&DetourVisualTerrainPost,&g_trampVisualTerrainPost,"EngineResources:VisualTerrainPostLoad"},
        {&g_hookLoadingInfo,g_terrain->loadingInfo,(void*)&DetourLoadingInfo,&g_trampLoadingInfo,"EngineResources:LevelLoadingInfo"}};
    for (unsigned i=0;i<sizeof(pending)/sizeof(pending[0]);++i) {
        const HookResult result=pending[i].hook->Install((void*)(ULONG_PTR)Va(pending[i].va),pending[i].detour,pending[i].name);
        if (result!=HookResult::Ok) {
            InterlockedExchange(&g_resourceHooksReady,-1);
            LOG_ERR("EngineRes: %s install %s; rollback complete resource/terrain set",pending[i].name,ToString(result));
            for (unsigned j=i;j>0;--j) {
                const bool reverted=pending[j-1].hook->Remove();
                LOG_INFO("EngineRes: rollback %s: %s",pending[j-1].name,reverted?"PASS":"FAIL");
            }
            return false;
        }
        if (pending[i].trampoline) *pending[i].trampoline=pending[i].hook->Trampoline();
    }
    InterlockedExchange(&g_resourceHooksReady, 1);
    InstallChecksumTrace();
    LOG_INFO("EngineRes: фазы Level:LoadResources (до original) / RegisterEntityResources (после original) подключены; "
             "Mount/AddRegistry разрешены только внутри своего синхронного engine callback; LoadBundles Pass, "
             "Terrain/VisualTerrain Return, cancelled Resource lifetime и Level:LoadingInfo подключены");
    return true;
}

bool IsInNativeLevelLoadResources() {
    return g_resourceHooksReady==1 && g_resourcePhase==ResourcePhase::Load && !EnvOff("EVBOTS_RESOURCE_MUTATION");
}

bool NativeLevelIdentity(char* name,size_t nameCap,char* mode,size_t modeCap,bool* authoritative) {
    if (authoritative) *authoritative=g_identityActive;
    if (!name || !mode || nameCap==0 || modeCap==0) return false;
    name[0]=mode[0]='\0';
    if (!Ready()) return false;
    if (g_identityActive) return ReadLoadIdentity(g_identityLevel,name,nameCap,mode,modeCap);
    if (g_a!=&kClient) return false;  // existing server getter remains authoritative outside load
    if (authoritative) *authoritative=true;
    return ReadLoadIdentity(NativeLoadingLevel(),name,nameCap,mode,modeCap);
}

bool MountSuperBundle(const char* name, int mediaHint, bool optional, const char** error) {
    if (error != nullptr) *error = nullptr;
    if (name == nullptr || *name == '\0') return MutationFailure(error, "superbundle name is empty");
    if (mediaHint != 0 && mediaHint != 1) return MutationFailure(error, "invalid MediaHint (expected 0 or 1)");
    if (!Ready() || g_resourceHooksReady != 1 || EnvOff("EVBOTS_RESOURCE_MUTATION"))
        return MutationFailure(error, "native resource lifecycle is unavailable or disabled");
    if (g_resourcePhase != ResourcePhase::Load)
        return MutationFailure(error, "MountSuperBundle requires the native Level:LoadResources callback");
    if (!CallNativeMount(name, mediaHint, optional)) {
        InterlockedExchange(&g_resourceHooksReady, -1);
        LOG_ERR("EngineRes: MountSuperBundle(%s) — manager/fault; операции записи закрыты, откат движка не гарантируется", name);
        return MutationFailure(error, "native mount failed; resource mutations disabled");
    }
    LOG_INFO("EngineRes: MountSuperBundle(%s, %d, %d) — native void-вызов завершён (это не проверка содержимого бандла)",
             name, mediaHint, optional ? 1 : 0);
    return true;
}

bool AddRegistry(unsigned long registry, int compartment, const char** error) {
    if (error != nullptr) *error = nullptr;
    if (!Ready() || g_resourceHooksReady != 1 || EnvOff("EVBOTS_RESOURCE_MUTATION"))
        return MutationFailure(error, "native resource lifecycle is unavailable or disabled");
    if (g_resourcePhase!=ResourcePhase::Load && g_resourcePhase!=ResourcePhase::InstanceLoad &&
        g_resourcePhase!=ResourcePhase::Register)
        return MutationFailure(error, "AddRegistry requires a synchronous native load or registration callback");
    if (compartment < 0 || compartment >= kCompartmentCount)
        return MutationFailure(error, "resource compartment is out of range");
    RegistryLockScope lock;
    if (g_resourceHooksReady!=1 || EnvOff("EVBOTS_RESOURCE_MUTATION"))
        return MutationFailure(error, "native resource lifecycle is unavailable or disabled");
    unsigned long active=0;
    if (!ReadU32(Va(g_mutation->registryActive),&active) || active==0)
        return MutationFailure(error, "NetworkDataRegistry is outside initLevel/exitLevel");
    if (registry == 0 || !IsKindOfName(TypeOf(registry), "RegistryContainer"))
        return MutationFailure(error, "expected a live RegistryContainer");
    std::set<unsigned long> objects;
    if (!RegistrySnapshot(registry,&objects) || !PinRegistrySnapshot(objects))
        return MutationFailure(error, "registered resource snapshot is unavailable or cannot be retained");
    if (!CallNativeAddRegistry(registry, compartment)) {
        InterlockedExchange(&g_resourceHooksReady, -1);
        LOG_ERR("EngineRes: AddRegistry(%p, %d) — native fault; операции записи закрыты, откат движка не гарантируется",
                (void*)(ULONG_PTR)registry, compartment);
        return MutationFailure(error, "native registry operation failed; resource mutations disabled");
    }
    LOG_INFO("EngineRes: AddRegistry(%p, %d) — native void-вызов завершён", (void*)(ULONG_PTR)registry, compartment);
    return true;
}

unsigned long FindPartitionByName(const char* name, int* compOut) {
    if (!Ready() || name == nullptr || *name == '\0') return 0;
    // Отсек Game (3) — первым: там оружие, киты, перки уровня.
    static const int kOrder[] = {3, 0};
    for (int c : kOrder) {
        const unsigned long d = DomainOf(c);
        if (d == 0) continue;
        const unsigned long p = CallFindPartitionByName(d, name);
        if (p != 0) {
            if (compOut != nullptr) *compOut = c;
            return p;
        }
    }
    for (int c = 0; c < kCompartmentCount; ++c) {
        if (c == 3 || c == 0) continue;
        const unsigned long d = DomainOf(c);
        if (d == 0) continue;
        const unsigned long p = CallFindPartitionByName(d, name);
        if (p != 0) {
            if (compOut != nullptr) *compOut = c;
            return p;
        }
    }
    return 0;
}

unsigned long PrimaryInstance(unsigned long partition) {
    unsigned long v = 0;
    if (partition == 0 || !ReadU32(partition + 0x4C, &v)) return 0;
    return v;
}

unsigned long FindAssetByName(const char* name) {
    return PrimaryInstance(FindPartitionByName(name, nullptr));
}

unsigned long FindInstanceInPartition(unsigned long partition, const char* guidText) {
    if (!Ready() || partition == 0) return 0;
    Guid16 win, raw;
    if (!ParseGuid(guidText, &win, &raw)) return 0;
    unsigned long r = CallFindInPartition(partition, &win);
    if (r == 0) r = CallFindInPartition(partition, &raw);
    return r;
}

unsigned long FindInstanceByGuid(const char* guidText, int* layoutOut) {
    if (!Ready()) return 0;
    Guid16 win, raw;
    if (!ParseGuid(guidText, &win, &raw)) return 0;
    static volatile LONG s_layout = 0;  // раскладка, на которой уже был успех: второй проход не нужен
    for (int pass = 1; pass <= 2; ++pass) {
        if (s_layout != 0 && s_layout != pass) continue;
        const Guid16* g = pass == 1 ? &win : &raw;
        const unsigned long r = ForEachPartition([g](unsigned long p) { return CallFindInPartition(p, g); });
        if (r != 0) {
            InterlockedExchange(&s_layout, pass);
            if (layoutOut != nullptr) *layoutOut = pass;
            return r;
        }
    }
    return 0;
}

// ★ Сборка 234: инстансы раздела — обход его хеш-таблицы GUID → инстанс, той же, по которой ищет findInstance.
// Факт (zbf3 004E6160 → 004E1380 → 004DF330; сервер 0x579BB0 — тот же код): лок [[p+8]+0x40]+0x10 (lock/unlock),
// таблица p+0x1C: +4 корзины, +8 их число; узел {+0 Guid* ключ, +4 DataContainer*, +8 следующий}.
unsigned PartitionInstancesLocked(unsigned long p, unsigned long* out, unsigned cap) {
    unsigned n = 0;
    __try {
        const unsigned long buckets = *(const unsigned long*)(ULONG_PTR)(p + 0x1C + 4);
        const unsigned long count = *(const unsigned long*)(ULONG_PTR)(p + 0x1C + 8);
        for (unsigned long b = 0; b < count && b < 1000000 && n < cap; ++b) {
            unsigned long node = *(const unsigned long*)(ULONG_PTR)(buckets + b * 4);
            for (int guard = 0; node != 0 && guard < 100000 && n < cap; ++guard) {
                const unsigned long dc = *(const unsigned long*)(ULONG_PTR)(node + 4);
                if (dc != 0) out[n++] = dc;
                node = *(const unsigned long*)(ULONG_PTR)(node + 8);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

unsigned PartitionInstances(unsigned long p, unsigned long* out, unsigned cap) {
    if (!Ready() || p == 0 || out == nullptr || cap == 0) return 0;
    unsigned long d = 0, dm = 0;
    if (!ReadU32(p + 8, &d) || d == 0 || !ReadU32(d + 0x40, &dm) || dm == 0) return 0;
    const unsigned long cs = dm + 0x10;
    using LockFn = void(__fastcall*)(unsigned long cs, unsigned long edx);
    __try {
        ((LockFn)(ULONG_PTR)Va(kVaLock))(cs, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    const unsigned n = PartitionInstancesLocked(p, out, cap);
    __try {
        ((LockFn)(ULONG_PTR)Va(kVaUnlock))(cs, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

unsigned long PartitionOfInstance(unsigned long dc) {
    if (!Ready() || dc == 0) return 0;
    return CallPartitionOf(dc);
}

void ProbeInstanceArena(unsigned long dc) {
    if (!Ready() || g_a!=&kServer || !dc) return;
    const unsigned long partition=CallPartitionOf(dc);
    unsigned long domain=0;
    const bool readable=partition && ReadU32(partition+8,&domain) && domain;
    LOG_INFO("EngineRes: EBX instance arena instance=%08lX partition=%08lX domain=%08lX readable=%u",
        dc,partition,domain,readable?1u:0u);
    if (readable) ProbeDomainArena(domain);
}

bool ReadReferenceTarget(unsigned long slot,unsigned long* target) {
    if (!target) return false;
    *target=0;
    unsigned long value=0;
    if (!ReadU32(slot,&value)) return false;
    if (value&1) {
        const unsigned long proxy=value&~3UL;
        if (proxy>0xFFFFFFEFUL || !ReadU32(proxy+0x10,&value)) return false;
    }
    if (value&3) return false;
    *target=value;
    return true;
}

namespace {
bool __fastcall CollectReferenceSlot(std::vector<unsigned long>* slots,void*,unsigned long slot) {
    slots->push_back(slot);
    return true;
}
}

bool CollectReferenceSlots(unsigned long instance,std::vector<unsigned long>* slots) {
    if (!slots) return false;
    slots->clear();
    if (!Ready() || !instance || (instance&3)) return false;
    const unsigned long visitor=g_a==&kServer?0x0047C860:0x0045B570;
    const unsigned char serverHead[]={0x51,0x53,0x55,0x8B,0xD9,0x8B,0x03,0x8B,0x10,0x56,0x57,0xFF,0xD2,0x89,0x44,0x24};
    const unsigned char clientHead[]={0x51,0x53,0x55,0x56,0x57,0x8B,0xF9,0x8B,0x07,0x8B,0x10,0xFF,0xD2,0x89,0x44,0x24};
    if (!MutationBytesMatch(visitor,g_a==&kServer?serverHead:clientHead)) return false;
    __try {
        using Fn=void(__thiscall*)(void*,unsigned long,unsigned long);
        ((Fn)(ULONG_PTR)Va(visitor))((void*)(ULONG_PTR)instance,
            (unsigned long)(ULONG_PTR)slots,(unsigned long)(ULONG_PTR)&CollectReferenceSlot);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        slots->clear();
        return false;
    }
}

bool PartitionGuid(unsigned long partition, Guid16* out) {
    if (partition == 0) return false;
    __try {
        std::memcpy(out->b, (const void*)(ULONG_PTR)(partition + 0x0C), 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool InstanceGuid(unsigned long dc, Guid16* out) {
    if (dc == 0) return false;
    __try {
        if ((*(const unsigned short*)(ULONG_PTR)(dc + 6) & 0x100) == 0) return false;
        std::memcpy(out->b, (const void*)(ULONG_PTR)(dc - 0x10), 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

unsigned long FindPartitionByGuid(const char* guidText) {
    if (!Ready() || g_pgCheck <= 0) return 0;
    Guid16 win;
    if (!ParseGuid(guidText, &win, nullptr)) return 0;
    return ForEachPartition([&win](unsigned long p) -> unsigned long {
        Guid16 g;
        return (PartitionGuid(p, &g) && SameGuid(g, win)) ? p : 0;
    });
}

int PartitionGuidCheck() {
    return g_pgCheck;
}

void ReportPartitionGuidCheck(bool ok) {
    const LONG prev = InterlockedExchange(&g_pgCheck, ok ? 1 : -1);
    if (prev == 0 || prev != (ok ? 1 : -1)) {
        LOG_INFO("EngineRes: сверка «GUID раздела по +0x0C» — %s", ok ? "★ ПОДТВЕРЖДЕНО" : "НЕ совпало (режим поиска инстансов)");
    }
}

namespace {

using LockFn = void(__fastcall*)(unsigned long cs, unsigned long edx);

/// Перебор карты TypeInfo* → контейнер под локом менеджера. Возвращает контейнер типа с именем name.
unsigned long SettingsScan(unsigned long mgr, const char* name) {
    const unsigned long cs = mgr + 0x20;
    unsigned long found = 0;
    volatile bool locked = false;  // читается после исключения — volatile
    __try {
        ((LockFn)(ULONG_PTR)Va(kVaLock))(cs, 0);
        locked = true;
        const unsigned long buckets = *(const unsigned long*)(ULONG_PTR)(mgr + 0x60 + 4);
        const unsigned long count = *(const unsigned long*)(ULONG_PTR)(mgr + 0x60 + 8);
        for (unsigned long b = 0; b < count && b < 65536 && found == 0; ++b) {
            unsigned long node = *(const unsigned long*)(ULONG_PTR)(buckets + b * 4);
            for (int guard = 0; node != 0 && guard < 4096 && found == 0; ++guard) {
                const unsigned long key = *(const unsigned long*)(ULONG_PTR)node;
                const unsigned long val = *(const unsigned long*)(ULONG_PTR)(node + 4);
                unsigned long info = 0, np = 0;
                if (key != 0 && ReadU32(key, &info) && info != 0 && ReadU32(info, &np) && np != 0 &&
                    ::_stricmp((const char*)(ULONG_PTR)np, name) == 0) {
                    found = val;
                }
                node = *(const unsigned long*)(ULONG_PTR)(node + 8);
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        found = 0;
    }
    if (locked) {
        __try {
            ((LockFn)(ULONG_PTR)Va(kVaUnlock))(cs, 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return found;
}

}  // namespace

unsigned long FindSettings(const char* typeName) {
    if (!Ready() || typeName == nullptr || *typeName == '\0') return 0;
    unsigned long mgr = 0;
    if (!ReadU32(Va(kVaSettingsManager), &mgr) || mgr == 0) return 0;
    return SettingsScan(mgr, typeName);
}

void DeliverPreloadedWatchedPartitions() {
    // Caller has entered the synchronous native Level:LoadResources callback
    // and delivered one-shot startup, so Extension:Loaded watches now exist.
    if (g_resourceHooksReady!=1 || g_resourcePhase!=ResourcePhase::Load) {
        LOG_ERR("EngineRes: preloaded catch-up requires native Level:LoadResources phase");
        return;
    }
    DeliverPreloadedWatchedPartitionsImpl();
}

// ── рефлексия ──────────────────────────────────────────────────────────────

unsigned long TypeOf(unsigned long dc) {
    if (!Ready() || dc == 0) return 0;
    return CallGetType(dc);
}

bool Describe(unsigned long typeInfo, TypeDesc* out) {
    if (typeInfo == 0 || out == nullptr) return false;
    TypeDesc d;
    d.typeInfo = typeInfo;
    unsigned long info = 0, namePtr = 0;
    unsigned flags = 0, size = 0, al = 0, fc = 0;
    if (!ReadU32(typeInfo, &info) || info == 0) return false;
    if (!ReadU32(info, &namePtr) || !ReadU16(info + 4, &flags) || !ReadU16(info + 6, &size) ||
        !ReadU8(info + 0x0C, &al) || !ReadU8(info + 0x0D, &fc)) {
        return false;
    }
    d.info = info;
    d.name = namePtr != 0 ? (const char*)(ULONG_PTR)namePtr : "";
    d.kind = (int)((flags >> 4) & 0x1F);
    d.size = size;
    d.alignment = al;
    d.fieldCount = fc;
    unsigned long v = 0;
    switch (d.kind) {
        case kClass:
            if (ReadU32(info + 0x10, &v)) d.super = v;
            if (ReadU32(info + 0x18, &v)) d.fields = v;
            break;
        case kValueType:
            if (ReadU32(info + 0x14, &v)) d.fields = v;
            break;
        case kEnum:
            if (ReadU32(info + 0x10, &v)) d.fields = v;
            break;
        case kArray:
        case kFixedArray:
            if (ReadU32(info + 0x10, &v)) d.element = v;
            d.fieldCount = 0;
            break;
        default:
            d.fieldCount = 0;
            break;
    }
    // Имя проверяем на читаемость (SEH): дальше его отдают в Lua как строку.
    __try {
        volatile char c0 = d.name[0];
        (void)c0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        d.name = "";
    }
    *out = d;
    return true;
}

bool FindField(unsigned long typeInfo, const char* name, FieldDesc* out) {
    if (name == nullptr) return false;
    unsigned long t = typeInfo;
    for (int depth = 0; t != 0 && depth < 32; ++depth) {
        TypeDesc d;
        if (!Describe(t, &d)) return false;
        if (d.kind != kClass && d.kind != kValueType) return false;
        for (unsigned i = 0; i < d.fieldCount && d.fields != 0; ++i) {
            const unsigned long fa = d.fields + i * 12;
            unsigned long np = 0, ft = 0;
            unsigned fl = 0, off = 0;
            if (!ReadU32(fa, &np) || !ReadU16(fa + 4, &fl) || !ReadU16(fa + 6, &off) || !ReadU32(fa + 8, &ft)) {
                break;
            }
            if (np == 0) continue;
            bool same = false;
            __try {
                same = Stricmp((const char*)(ULONG_PTR)np, name) == 0;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                same = false;
            }
            if (same) {
                out->name = (const char*)(ULONG_PTR)np;
                out->flags = fl;
                out->offset = off;
                out->type = ft;
                return true;
            }
        }
        if (d.kind != kClass) return false;
        t = d.super;
    }
    return false;
}

bool IsKindOfName(unsigned long typeInfo, const char* name) {
    unsigned long t = typeInfo;
    for (int depth = 0; t != 0 && depth < 32; ++depth) {
        TypeDesc d;
        if (!Describe(t, &d)) return false;
        if (Stricmp(d.name, name) == 0) return true;
        if (d.kind != kClass) return false;
        t = d.super;
    }
    return false;
}

// ── загрузка разделов ───────────────────────────────────────────────────────

void SetAnyPartitionCallback(PartitionLoadedFn cb) {
    g_onAnyPartition = cb;
}

bool IsCurrentPartitionDelivery(unsigned long partition, unsigned long generation) {
    if (!partition || !generation) return false;
    bool current = false;
    AcquireSRWLockShared(&g_deliveryGate);
    for (int i = 0; i < g_deliveredCount; ++i) {
        const auto& entry = g_delivered[i];
        if (entry.partition == partition) {
            current = entry.generation == generation;
            break;
        }
    }
    ReleaseSRWLockShared(&g_deliveryGate);
    return current;
}

bool InstallPartitionHook(WatchedPartitionLoadedFn cb) {
    if (!Ready()) return false;
    if (g_hookBundlePartition.IsInstalled()) {
        g_onPartition = cb;
        return true;
    }
    if (EnvOff("EVBOTS_LOAD_HOOK")) {
        LOG_INFO("EngineRes: перехват загрузки разделов выключен (EVBOTS_LOAD_HOOK=0) — только догоняющий вызов");
        return false;
    }
    const bool clientEndPost = g_a == &kClient && !EnvOff("EVBOTS_CLIENT_POSTCHUNKS");
    if (clientEndPost) {
        const unsigned char expected[16] = {0x83,0xEC,0x18,0x53,0x55,0x56,0x57,0x8B,0xF9,0x8D,0x4F,0x10,0x89,0x7C,0x24,0x10};
        const bool match = MutationBytesMatch(kVaEndPostChunks, expected);
        LOG_INFO("EngineRes: client endPostChunks signature %08lX %s", kVaEndPostChunks, match ? "PASS" : "FAIL");
        if (!match) return false;
    }
    g_onPartition = cb;
    void* target = (void*)(ULONG_PTR)Va(kVaOnBundlePartition);
    const HookResult r = g_hookBundlePartition.Install(target, (void*)&DetourOnBundlePartition, "EngineResources:g_hookBundlePartition");
    if (r != HookResult::Ok) {
        LOG_ERR("EngineRes: перехват onBundlePartition (VA 0x%08lX) не встал: %s", kVaOnBundlePartition,
                ToString(r));
        return false;
    }
    g_trampBundlePartition = g_hookBundlePartition.Trampoline();
    LOG_INFO("EngineRes: ★ Compartment::onBundlePartition (VA 0x%08lX) перехвачен (украдено %u байт) — "
             "load-хендлеры мода срабатывают при загрузке раздела, как в VU",
             kVaOnBundlePartition, (unsigned)g_hookBundlePartition.StolenBytes());
    if (kVaEndPostChunks == 0) {
        LOG_INFO("EngineRes: %s: endPostChunks не найден — разделы отдаются только из onBundlePartition (реестр В8)",
                 g_a->exe);
        return true;
    }
    if (g_a == &kClient && !clientEndPost) {
        LOG_INFO("EngineRes: client endPostChunks disabled (EVBOTS_CLIENT_POSTCHUNKS=0)");
        return true;
    }
    const HookResult r2 = g_hookEndPostChunks.Install((void*)(ULONG_PTR)Va(kVaEndPostChunks),
                                                      (void*)&DetourEndPostChunks, "EngineResources:g_hookEndPostChunks");
    if (r2 != HookResult::Ok) {
        LOG_ERR("EngineRes: перехват endPostChunks (VA 0x%08lX) не встал: %s", kVaEndPostChunks, ToString(r2));
        if (clientEndPost) {
            const bool reverted = g_hookBundlePartition.Remove();
            LOG_ERR("EngineRes: client partition hooks rollback=%s", reverted ? "ok" : "FAILED");
            return false;
        }
        return true;
    }
    g_trampEndPostChunks = g_hookEndPostChunks.Trampoline();
    LOG_INFO("EngineRes: ★ Compartment::endPostChunks (VA 0x%08lX) перехвачен (украдено %u байт) — "
             "разделы бандла отдаются моду в конце его загрузки",
             kVaEndPostChunks, (unsigned)g_hookEndPostChunks.StolenBytes());
    return true;
}

void WatchPartition(const char* partitionGuidText) {
    Guid16 g;
    if (!ParseGuid(partitionGuidText, &g, nullptr)) return;
    EnsureWatchGate();
    ::EnterCriticalSection(&g_watchGate);
    bool have = false;
    for (LONG i = 0; i < g_watchPartCount && !have; ++i) have = SameGuid(g_watchPart[i], g);
    if (!have && g_watchPartCount < kWatchMax) {
        g_watchPart[g_watchPartCount] = g;
        InterlockedIncrement(&g_watchPartCount);
    }
    ::LeaveCriticalSection(&g_watchGate);
}

void WatchInstance(const char* instanceGuidText) {
    Guid16 g;
    if (!ParseGuid(instanceGuidText, &g, nullptr)) return;
    EnsureWatchGate();
    ::EnterCriticalSection(&g_watchGate);
    bool have = false;
    for (LONG i = 0; i < g_watchInstCount && !have; ++i) have = SameGuid(g_watchInst[i], g);
    if (!have && g_watchInstCount < kWatchMax) {
        g_watchInst[g_watchInstCount] = g;
        InterlockedIncrement(&g_watchInstCount);
    }
    ::LeaveCriticalSection(&g_watchGate);
}

unsigned long PartitionHookCalls() {
    return (unsigned long)g_hookCalls + (unsigned long)g_endPostCalls;
}

unsigned long PartitionHookMatches() {
    return (unsigned long)g_hookMatches;
}

}  // namespace eng
}  // namespace evbots
