// EngineResources.h — нативная база данных ресурсов движка BF3 (сервер; с 234 и клиент — своя таблица адресов zbf3)
// для EVBots.
//
// ★ Сборка 161 (16.10.2026): «как у VU» — вместо скана памяти и офлайн-индекса EBX
// ходим туда же, куда ходит сам движок, когда разрешает инстансы:
//   * раздел по имени — домен отсека, хеш-таблица имён (fb::ResourceManager_lookupDataContainer);
//   * инстанс по GUID — обход разделов отсека и его родителей (0x54FE50);
//   * раздел инстанса — карта ctrToPartition менеджера БД (0x57C170);
//   * рефлексия типов — TypeInfo / *InfoData движка (имена, виды, смещения полей);
//   * загрузка раздела — Compartment::onBundlePartition (0x554190), точка для load-хендлеров VU.
// Все адреса сверены дизассемблером по bf3_server_Final.exe (capstone), разбор — MODLOG
// «2026-10-16 … нативный поиск ассетов» и «Сборка 161». Правило 18: каждый пролог
// сверяется в рантайме, «всё или ничего» — при любом несовпадении слой выключается.
// EVBOTS_ENGINE_RES=0 — выключить весь слой (старое поведение: скан + офлайн-индекс).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace evbots {
namespace eng {

/// GUID в памяти движка — 16 байт в раскладке Windows GUID (Data1..Data3 little-endian).
struct Guid16 {
    unsigned char b[16];
};

/// Разобрать текст «XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX» (дефисы/скобки не важны).
/// win — раскладка Windows GUID, raw — байты в порядке текста. false — не GUID.
bool ParseGuid(const char* text, Guid16* win, Guid16* raw);

/// Текст из 16 байт в раскладке Windows GUID (верхний регистр, с дефисами). out — 37 байт.
void FormatGuid(const Guid16& g, char* out);

/// Слой доступен: процесс — сервер BF3, прологи всех функций совпали, флаг не выключен.
bool Ready();

// Сборка 267: операции записи ресурсов выполняются только синхронно из соответствующей
// фазы загрузки движка. Перенос в очередь кадра изменил бы порядок загрузки VU.
using LevelLoadResourcesFn = bool (*)(const char* levelName, const char* gameMode, bool dedicated);
using LevelRegisterResourcesFn = bool (*)(unsigned long levelData);
/// Синхронный LoadBundles: callback заменяет список/compartment только при true.
/// Строки и массив char* принадлежат detour и живут до возврата original.
using ResourceLoadBundlesFn = bool (*)(std::vector<std::string>* bundles, int* compartment);
bool InstallResourceLifecycleHooks(LevelLoadResourcesFn load, LevelRegisterResourcesFn registration,
                                   ResourceLoadBundlesFn bundles);
// Тот же all-or-nothing набор устанавливает Terrain:Load / VisualTerrain:Load.
// assetName read-only; Return отменяет native load с refcounted cancelled Resource,
// VisualTerrain postLoad пропускает только наш cancelled Resource. Оба env-флага
// EVBOTS_RESOURCE_MUTATION=0 / EVBOTS_TERRAIN_HOOKS=0 выключают установку набора.
// Level:LoadingInfo доставляется из native setLoadingInfo до original, синхронно;
// в том числе "Registering entity resources" до native регистрации и её события.
/// Только синхронный native Level:LoadResources callback текущего потока.
/// Lua Events:Dispatch сам по себе эту область разрешения открыть не может.
bool IsInNativeLevelLoadResources();
// Только из синхронного LoadResources callback, после one-shot startup мода.
void DeliverPreloadedWatchedPartitions();
/// Current native load's Level identity, or the live client Level outside load.
/// authoritative=true means a missing identity must remain nil (no old-level fallback).
/// Server callers outside a native loading scope retain their existing getter.
bool NativeLevelIdentity(char* name,size_t nameCap,char* mode,size_t modeCap,bool* authoritative);

/// false + error — native-вызов не выполнен либо завершился исключением. Успех означает,
/// что void-операция движка вернулась; он не доказывает наличие всех ассетов superbundle.
bool MountSuperBundle(const char* name, int mediaHint, bool optional, const char** error);
bool AddRegistry(unsigned long registry, int compartment, const char** error);

// ── поиск ──────────────────────────────────────────────────────────────────

/// Раздел по имени (регистр не важен — хеш движка с tolower). Перебор отсеков 0..204.
/// compOut — номер отсека, где нашёлся (может быть nullptr).
unsigned long FindPartitionByName(const char* name, int* compOut);

/// Главный инстанс раздела ([раздел+0x4C]).
unsigned long PrimaryInstance(unsigned long partition);

/// Ассет по имени: главный инстанс раздела с этим именем (0 — нет).
unsigned long FindAssetByName(const char* name);

/// Инстанс по GUID (любой отсек). layoutOut: 1 — раскладка Windows, 2 — порядок текста.
unsigned long FindInstanceByGuid(const char* guidText, int* layoutOut);

/// Инстанс по GUID внутри одного раздела.
unsigned long FindInstanceInPartition(unsigned long partition, const char* guidText);

/// ★ Сборка 234: инстансы раздела (хеш-таблица GUID → инстанс раздела, под локом домена). Число записанных в out.
/// Порядок — порядок таблицы, не порядок файла EBX.
unsigned PartitionInstances(unsigned long partition, unsigned long* out, unsigned cap);

/// Раздел, которому принадлежит инстанс (0 — не найден).
unsigned long PartitionOfInstance(unsigned long dc);
void ProbeInstanceArena(unsigned long dc);
bool ReadReferenceTarget(unsigned long slot,unsigned long* target);
// The caller owns native resource serialization; visiting does not pin backing.
bool CollectReferenceSlots(unsigned long instance,std::vector<unsigned long>* slots);

/// GUID раздела ([раздел+0x0C], гипотеза, сверяется SelfCheckPartitionGuid).
bool PartitionGuid(unsigned long partition, Guid16* out);

/// GUID инстанса (есть только у экспортируемых: флаг 0x100 в [dc+6], GUID в dc-0x10).
bool InstanceGuid(unsigned long dc, Guid16* out);

/// Раздел по GUID (перебор разделов отсеков; только после подтверждения +0x0C).
unsigned long FindPartitionByGuid(const char* guidText);

/// Сверка гипотезы «GUID раздела по +0x0C»: 0 — не проверено, 1 — подтверждено, -1 — нет.
int PartitionGuidCheck();
void ReportPartitionGuidCheck(bool ok);

/// ★ Сборка 163: контейнер настроек по имени типа — fb::SettingsManager ([0x2AF7DD4]), карта
/// TypeInfo* → контейнер (+0x60, лок +0x20). Так движок сам получает SyncedGameSettings (0x887120 → 0x53FBC0).
/// Это основа ResourceManager:GetSettings как в VU. 0 — нет такого.
unsigned long FindSettings(const char* typeName);

// ── рефлексия ──────────────────────────────────────────────────────────────

/// Виды типов: (flags >> 4) & 0x1F у *InfoData.
enum Kind : int {
    kVoid = 0, kDbObject = 1, kValueType = 2, kClass = 3, kArray = 4, kFixedArray = 5,
    kString = 6, kCString = 7, kEnum = 8, kFileRef = 9, kBoolean = 10, kInt8 = 11, kUint8 = 12,
    kInt16 = 13, kUint16 = 14, kInt32 = 15, kUint32 = 16, kInt64 = 17, kUint64 = 18,
    kFloat32 = 19, kFloat64 = 20, kGuid = 21, kSHA1 = 22, kResourceRef = 23,
};

struct TypeDesc {
    unsigned long typeInfo = 0;  ///< объект TypeInfo
    unsigned long info = 0;      ///< его *InfoData
    const char* name = "";
    int kind = 0;
    unsigned size = 0;
    unsigned alignment = 0;
    unsigned fieldCount = 0;
    unsigned long fields = 0;    ///< массив FieldInfoData (12 байт на поле)
    unsigned long super = 0;     ///< TypeInfo родителя (только класс)
    unsigned long element = 0;   ///< TypeInfo элемента (только массив, гипотеза +0x10)
};

struct FieldDesc {
    const char* name = "";
    unsigned flags = 0;
    unsigned offset = 0;
    unsigned long type = 0;      ///< TypeInfo поля
};

/// TypeInfo объекта: vtable[0]() = getType().
unsigned long TypeOf(unsigned long dc);

/// Описание типа по объекту TypeInfo. false — память не читается.
bool Describe(unsigned long typeInfo, TypeDesc* out);

/// Поле по имени (регистр не важен), с обходом родителей класса.
bool FindField(unsigned long typeInfo, const char* name, FieldDesc* out);

/// Тип или его предок называется name (регистр не важен).
bool IsKindOfName(unsigned long typeInfo, const char* name);

// ── загрузка разделов (load-хендлеры) ───────────────────────────────────────

/// Вызывается ПОСЛЕ того, как движок положил раздел в домен отсека.
using PartitionLoadedFn = void (*)(unsigned long partition, const char* name);
using WatchedPartitionLoadedFn = void (*)(unsigned long partition, const char* name, unsigned long generation);

/// Поставить перехват Compartment::onBundlePartition (один раз). false — не встал.
bool InstallPartitionHook(WatchedPartitionLoadedFn cb);
/// Отложенный callback относится только к тому экземпляру раздела, который его породил.
bool IsCurrentPartitionDelivery(unsigned long partition, unsigned long generation);

/// ★ Сборка 234: колбэк на КАЖДЫЙ раздел, загруженный через onBundlePartition (поток загрузки движка) — для
/// события Partition:Loaded. Ставить до InstallPartitionHook. nullptr — выключить.
void SetAnyPartitionCallback(PartitionLoadedFn cb);

/// Отслеживать раздел (GUID из RegisterInstanceLoadHandler/RegisterPartitionLoadHandler).
void WatchPartition(const char* partitionGuidText);
/// Отслеживать инстанс (для режима без подтверждённого GUID раздела).
void WatchInstance(const char* instanceGuidText);

/// Сколько раз сработал перехват / сколько разделов совпало (для лога).
unsigned long PartitionHookCalls();
unsigned long PartitionHookMatches();

}  // namespace eng
}  // namespace evbots
