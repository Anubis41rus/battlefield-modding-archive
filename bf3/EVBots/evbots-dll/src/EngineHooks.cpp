// EngineHooks.cpp — хуки движка и обходные функции.

#include "EngineHooks.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "EVBotsLog.h"

namespace evbots {
namespace {

HookCounters g_counters{};
EngineHooks::Overrides g_overrides{};
HookResult g_results[EngineHooks::kHookCount];
bool g_resultsValid = false;

/// Флаг отключения конкретного хука движка: EVBOTS_NO_HOOK_<ИМЯ>=1
/// (например EVBOTS_NO_HOOK_TICK). Нужен, чтобы точечно изолировать,
/// какой из наших детуров валит сервер.
bool HookSuppressed(const char* hookName) {
    char varname[64];
    std::snprintf(varname, sizeof(varname), "EVBOTS_NO_HOOK_%s", hookName);
    const char* v = std::getenv(varname);

    if (v != nullptr && v[0] == '1') {
        LOG_INFO("EVBots: hook %s disabled by %s=1 (isolation)",
                 hookName, varname);
        return true;
    }

    return false;
}

/// Готовые хуки. Статические, чтобы адреса detour'ов были стабильны.
InlineHook g_hookOnNewRound;
InlineHook g_hookTick;
InlineHook g_hookOnDamage;
InlineHook g_hookChat;

}  // namespace

// ── обходные функции: сюда попадает управление из трамплина ───────────────
//
// Определения живут прямо в namespace evbots, а не в безымянном: их
// объявления есть в EngineHooks.h. Если бы определение оказалось в
// безымянном пространстве, одноимённая функция появилась бы там как
// вторая — и взятие её адреса давало бы «overloaded-function».

void OnNewRoundTrampoline() {
    if (g_counters.onNewRound < 8) {
        LOG_INFO("EVBots: onNewRound trampoline (round %lu)",
                 g_counters.onNewRound);
    }
}

void OnTickTrampoline() {
    // тик вызывается тысячи раз в секунду — молчим
}

void BridgeAfterClientDamage();  // сборка 153, VextPlayerManager.cpp
void BridgeOnChatMessage(void* msg, void* sender);  // сборка 159, VextPlayerManager.cpp (входящий чат)

void OnDamageTrampoline() {
    if (g_counters.onDamage < 4) {
        LOG_INFO("EVBots: onDamage trampoline (%lu)", g_counters.onDamage);
    }
    // Сборка 153: урон от клиента человека применён внутри onMessage — сверка HP, стрелок = человек (Soldier:Damage)
    BridgeAfterClientDamage();
}

void ChatMessageTrampoline() {
    if (g_counters.chatMessage < 4) {
        LOG_INFO("EVBots: chatMessage trampoline (%lu)",
                 g_counters.chatMessage);
    }
}

// ── наши detour-функции ────────────────────────────────────────────────────
//
// Перехват обязан пропустить вызов в исходный код, иначе функция просто
// перестаёт работать. Оригинал вызывается через трамплин — там лежит
// украденная часть пролога и прыжок назад в тело функции.
//
// Про сигнатуры: в самопроверке цели имеют вид int __fastcall (int, int),
// а обходники — void __fastcall (void*, void*). На уровне соглашения вызова
// это одно и то же (значения в ECX/EDX), поэтому прямой вызов трамплина с
// теми же двумя аргументами корректен. В боевой работе у каждой цели будет
// своя настоящая сигнатура — она появится вместе с привязкой к движку.

/// Вызывает оригинал цели через её трамплин — вариант без стековых
/// аргументов (оригинал RET 0): __fastcall(self, args) — 2 регистра.
/// Работает для onNewRound и tick (проверено стабильной работой 33-й).
/// Возвращает то, что вернул оригинал (на x86 — EAX).
static int CallOriginal(int hookIndex, void* self, void* args) {
    void* tramp = EngineHooks::TrampolineOf(hookIndex);

    if (tramp == nullptr) {
        LOG_WARN("EVBots: detour %d called, but trampoline is empty - "
                 "original not called", hookIndex);
        return 0;
    }

    using Fn = int(__fastcall*)(void*, void*);
    const int result = reinterpret_cast<Fn>(tramp)(self, args);

    LOG_INFO("EVBots: detour %d calls original via trampoline %p, "
             "original returned %d", hookIndex, tramp, result);

    return result;
}

/// Вызывает оригинал с 1 стековым аргументом (оригинал RET 4).
///
/// ★ 36-я, грабля 13.10: у onDamage настоящая сигнатура thiscall(this, msg),
/// RET 4 — оригинал сам пoпает 1 стековый аргумент. Старый детур звал
/// __fastcall(self, args) (0 стековых) → оригинал пoпал 4 байта, которых мы
/// не передали → ESP уезжает на +4 → возврат в мусор (EIP в куче) → смерть
/// без дампа. Форвардим настоящие аргументы.
static int CallOriginalStk1(int hookIndex, void* self, void* edx, void* arg1) {
    void* tramp = EngineHooks::TrampolineOf(hookIndex);

    if (tramp == nullptr) {
        LOG_WARN("EVBots: detour %d called, but trampoline is empty - "
                 "original not called", hookIndex);
        return 0;
    }

    // thiscall(this, 1 стековый аргумент): ECX=self, 1 арг на стеке,
    // callee пoпает его сам (RET 4). __fastcall(self, edx, arg1) даёт
    // ровно такой расклад стека.
    using Fn = int(__fastcall*)(void*, void*, void*);
    const int result = reinterpret_cast<Fn>(tramp)(self, edx, arg1);

    LOG_INFO("EVBots: detour %d calls original via trampoline %p, "
             "original returned %d", hookIndex, tramp, result);

    return result;
}

/// Вариант для целей с 2 стековыми аргументами (RET 8).
static int CallOriginal2(int hookIndex, void* self, void* edx, void* arg1,
                         void* arg2) {
    void* tramp = EngineHooks::TrampolineOf(hookIndex);

    if (tramp == nullptr) {
        LOG_WARN("EVBots: detour %d called, but trampoline is empty - "
                 "original not called", hookIndex);
        return 0;
    }

    using Fn = int(__fastcall*)(void*, void*, void*, void*);
    const int result = reinterpret_cast<Fn>(tramp)(self, edx, arg1, arg2);

    LOG_INFO("EVBots: detour %d calls original via trampoline %p, "
             "original returned %d", hookIndex, tramp, result);

    return result;
}

int __fastcall DetourOnNewRound(void* self, void* args) {
    ++g_counters.onNewRound;

    // ★ Смена сторон на новом раунде (сборка 85, прогон 83). onNewRound (0x0117EA40) при флаге [0x0285BABC]
    // (его ставит registerTeams 0x0118CFB0 из данных режима — поле TeamSwitch) и «не первом» раунде меняет
    // ВСЕМ игрокам команду 1<->2. У нас второй новый раунд приходит, когда заходит человек (конец ожидания
    // игроков): все 15 ботов разом перелетали, мод видел перекос и создавал 16-го, а лишнего потом лишь «убивал».
    // Подавляем только саму смену сторон: флаг сбрасывается на время вызова и возвращается. EVBOTS_ROUND_SWAP=1 —
    // как в движке.
    unsigned char* swapFlag = nullptr;
    unsigned char swapWas = 0;
    {
        char v[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_ROUND_SWAP", v, (DWORD)sizeof(v));
        unsigned char* exe = reinterpret_cast<unsigned char*>(::GetModuleHandleA("bf3_server_Final.exe"));
        if (!(n == 1 && v[0] == '1') && exe != nullptr) {
            swapFlag = exe + (0x0285BABCUL - 0x00400000UL);
            swapWas = *swapFlag;
            *swapFlag = 0;
        }
    }

    // onNewRound живое в 33-й работало с двухрегистровым вызовом.
    const int originalResult = CallOriginal(0, self, args);
    if (swapFlag != nullptr) {
        *swapFlag = swapWas;
        LOG_INFO("EVBots: onNewRound — смена сторон подавлена (флаг TeamSwitch был %u)", (unsigned)swapWas);
    }
    OnNewRoundTrampoline();

    ModdingInterface* modding = GetInstance();
    EngineState state{};
    modding->QueryState(&state);

    LOG_INFO("EVBots: intercepted onNewRound, total calls %lu",
             g_counters.onNewRound);

    return originalResult;
}

int __fastcall DetourOnTick(void* self, void* args) {
    ++g_counters.tick;

    // tick не ставится (natives.tick=nullptr, см. ResolveTargets) — путь
    // мёртвый, оставлен для совместимости.
    const int originalResult = CallOriginal(1, self, args);
    OnTickTrampoline();
    return originalResult;
}

int __fastcall DetourOnDamage(void* self, void* edx, void* arg1) {
    ++g_counters.onDamage;

    // Настоящая сигнатура (статика 13.10): thiscall(this, msg), RET 4 —
    // 1 стековый аргумент. Детур получает его 3-м параметром.
    const int originalResult = CallOriginalStk1(2, self, edx, arg1);
    OnDamageTrampoline();
    return originalResult;
}

int __fastcall DetourChatMessage(void* self, void* edx, void* arg1,
                                 void* arg2) {
    ++g_counters.chatMessage;

    // Сборка 159: до оригинала читаем сообщение (M — arg1, отправитель — arg2) для Player:Chat (EVBOTS_CHAT_EVENTS).
    BridgeOnChatMessage(arg1, arg2);

    // handleMessage: RET 8 → 2 стековых аргумента + ECX.
    const int originalResult =
        CallOriginal2(3, self, edx, arg1, arg2);
    ChatMessageTrampoline();
    return originalResult;
}

namespace {

// ── цели проверки механизма ───────────────────────────────────────────────
//
// Это НЕ поверхность событий EVBots. Здесь только адреса из botlayer,
// взятые как готовая точка для самопроверки перехвата: они уже сверены
// байт в байт с bf3_server_Final.exe (stand\HOOK-TABLE.md), поэтому
// механизм можно проверить настоящими адресами движка, а не заглушками.
//
// Поверхность событий для Lua-модов — см. stand\PHASE1-MAP.md:
//   хуки    Bulletentity:collision, Player:requestjoin, Entityfactory:create,
//            Player:selectteam, Entityfactory:createfromblueprint,
//            Server:suppressenemies, Player:findbestsquad, Soldier:damage,
//            Player:findbestteam + 4 shared;
//   события 26 штук, на которые подписан fun-bots;
//   API     51 вызов менеджеров.
//
// Адреса функций движка для них ещё не найдены — это работа фазы 4.

struct KnownAddress {
    const char* name;
    void* address;
    const char* signature;
};

const KnownAddress kServerTargets[] = {
    {"ServerPlayerSquadManager::onNewRound", (void*)0x0117EA40UL,
     "8B 44 24 04 8B 0D D8 D7 B5 02 (1 occurrence - unique anchor)"},
    // ВНИМАНИЕ: здесь RVA, а не адреса из Ghidra. Ghidra отдаёт VA
    // (ImageBase 0x400000 уже включён), а Install складывает moduleBase.
    // Если подставить VA как RVA, цель уедет на +0x400000 в заглушки
    // C3 CC CC... — хук «встанет», но никогда не сработает.
    {"ServerPlayerManager::update", (void*)0x00A5EF00UL,
     "83 EC 08 56 8B F1 (пролог ServerPlayerManager::update)"},
    {"ServerSoldierHealthModuleImpl::onDamage", (void*)0x01183000UL,
     "55 8B EC 83 E4 F0 (10819 occurrences)"},
    {"ServerChatManager::handleMessage", (void*)0x00E4DAF0UL,
     "81 EC B4 02 00 00 (21 occurrences)"},
};

}  // namespace

const HookCounters& Counters() {
    return g_counters;
}

void ResetCounters() {
    std::memset(&g_counters, 0, sizeof(g_counters));
}

void* TestFirstTrampoline() {
    return g_hookOnNewRound.Trampoline();
}

StolenInfo TestFirstStolen() {
    return StolenInfo{g_hookOnNewRound.StolenBytes(),
                       g_hookOnNewRound.StolenInstructions()};
}

int LuaVersionNumber() {
    return 0;  // Lua подключается позже, см. LuaRuntime.cpp
}

void EngineHooks::SetOverrides(const Overrides& o) {
    g_overrides = o;
}

const EngineHooks::Overrides& EngineHooks::GetOverrides() {
    return g_overrides;
}

HookResult EngineHooks::LastResult(int index) {
    if (index < 0 || index >= kHookCount || !g_resultsValid) {
        return HookResult::BadTarget;
    }

    return g_results[index];
}

void* EngineHooks::TrampolineOf(int index) {
    switch (index) {
        case 0:
            return g_hookOnNewRound.Trampoline();
        case 1:
            return g_hookTick.Trampoline();
        case 2:
            return g_hookOnDamage.Trampoline();
        case 3:
            return g_hookChat.Trampoline();
        default:
            return nullptr;
    }
}

void EngineHooks::ClearLastResults() {
    std::memset(g_results, 0, sizeof(g_results));
    g_resultsValid = false;
}

bool EngineHooks::ResolveNatives(EngineNatives* out) {
    if (out == nullptr) {
        return false;
    }

    *out = EngineNatives{};

    // 1) явные переопределения (тестовый хост)
    if (g_overrides.any()) {
        out->onNewRound = g_overrides.onNewRound;
        out->tick = g_overrides.tick;
        out->onDamage = g_overrides.onDamage;
        out->chatMessage = g_overrides.chatMessage;
        LOG_INFO("EVBots: targets set by override (test/tool)");
        return true;
    }

    // 2) находим образ выделенного сервера BF3 в текущем процессе
    //    без него работать не с чем: клиент — это другой бинарь, а хуки
    //    адресованы именно bf3_server_Final.exe.
    HMODULE server = GetModuleHandleA("bf3_server_Final.exe");
    if (server == nullptr) {
        LOG_INFO("EVBots: bf3_server_Final.exe not in process - engine "
                 "hooks skipped");
        return false;
    }

    LOG_INFO("EVBots: bf3_server_Final.exe found at %p",
             reinterpret_cast<void*>(server));

    // 3) подставляем цели. Модуль загружен по своему ImageBase, поэтому
    //    абсолютные адреса из HOOK-TABLE.md нужно пересчитать через RVA.
    //
    // ВАЖНО (fix 10.10): адрес 0x00E5EF00 (ServerPlayer::update) СЮДА НЕ
    // СТАВИТСЯ. По декомпиляции это __thiscall(this, param на стеке), а наш
    // detour — __fastcall(ECX/EDX): трамплин зовёт оригинал со сдвинутым
    // стеком → краш сервера (см. re\server-decompile.txt, FUN_00e5ef00).
    // Engine:Update жителям fun-bots рассылает фоновый таймер
    // (ModdingInterface::StartUpdateTimer), а не хук tick.
    for (const KnownAddress& t : kServerTargets) {
        auto abs = reinterpret_cast<uintptr_t>(t.address);
        const auto rva = abs - 0x00400000UL;  // ImageBase bf3_server_Final.exe
        auto* real = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(server) + rva);

        LOG_INFO("EVBots: target %s = %p (RVA 0x%08lX), signature: %s",
                 t.name, real, rva, t.signature);

        if (std::strcmp(t.name, "ServerPlayerSquadManager::onNewRound") == 0) {
            out->onNewRound = real;
        } else if (std::strcmp(t.name, "ServerPlayer::update") == 0) {
            // намеренно пропускаем — см. комментарий выше (краш через
            // несовпадение соглашений вызова), tick остаётся nullptr
        } else if (std::strcmp(t.name,
                                "ServerSoldierHealthModuleImpl::onDamage") == 0) {
            out->onDamage = real;
        } else if (std::strcmp(t.name, "ServerChatManager::handleMessage") == 0) {
            out->chatMessage = real;
        }
    }

    return out->Any();
}

// Сборка 138: «раунд без ожидания игроков» как у VU (vu.DisablePreRound). VU ставит перехваты на геттеры
// «сколько игроков нужно для старта / для рестарта раунда» и отдаёт 0. В нашем сервере это две функции
// (разбор re\srvdis.py): VA 0x012D7620 (roundStartPlayerCount, минимум 1) и 0x012D76D0 (roundRestartPlayerCount),
// обе читают vars.* через [0x2B5BFB4]+0x3C/+0x40, результат кладёт в PreRound-объект [+0xAC]/[+0xB0] функция 0x012E0D20.
// Патч: первые 3 байта `mov eax,[0x2AF7DD4]` -> `xor eax,eax; ret`. «Всё или ничего»: сначала сверка 8 байт обеих
// функций. EVBOTS_PREROUND_PATCH=1 — включить (по умолчанию выкл.: раунд всё равно ждёт входа человека, как у VU).
static void ApplyPreRoundPatches() {
    char e[4] = {};
    const DWORD en = ::GetEnvironmentVariableA("EVBOTS_PREROUND_PATCH", e, (DWORD)sizeof(e));
    if (!(en == 1 && e[0] == '1')) {
        LOG_INFO("EVBots: патчи предраунда выключены (включить: EVBOTS_PREROUND_PATCH=1)");
        return;
    }
    unsigned char* base = (unsigned char*)::GetModuleHandleA(nullptr);
    if (base == nullptr) return;
    static const unsigned long kRva[2] = {0x00ED7620UL, 0x00ED76D0UL};
    static const unsigned char kFrom[8] = {0xA1, 0xD4, 0x7D, 0xAF, 0x02, 0x83, 0xEC, 0x18};
    static const unsigned char kTo[3] = {0x31, 0xC0, 0xC3};
    for (int i = 0; i < 2; ++i) {
        bool ok = false;
        __try {
            ok = std::memcmp(base + kRva[i], kFrom, sizeof(kFrom)) == 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ok = false;
        }
        if (!ok) {
            LOG_INFO("EVBots: патч предраунда: байты по RVA %08lX не те — НИЧЕГО не пишу", kRva[i]);
            return;
        }
    }
    int n = 0;
    for (int i = 0; i < 2; ++i) {
        unsigned char* at = base + kRva[i];
        // сборка 237: запись — ядром (ev-core), со сверкой тех же байтов
        if (!PatchBytes(at, kFrom, kTo, sizeof(kTo), "патч предраунда")) continue;
        ++n;
        LOG_INFO("EVBots: патч предраунда: VA %p — геттер числа игроков отдаёт 0", at);
    }
    LOG_INFO("EVBots: ★ патчи предраунда: %d из 2", n);
}

int EngineHooks::InstallAll(const EngineNatives& natives) {
    struct Target {
        InlineHook* hook;
        void* target;
        void* detour;
        const char* name;
    };

    // Приведение адреса detour-функции к void*.
    //
    // Соглашение вывода параметров шаблонов/лямбд на указателях с явно
    // заданным __fastcall даёт неоднозначность, а одношаговый (void*)&функция
    // компилятор не берёт. Поэтому идём каноническим путём Win32: в целое
    // число, затем обратно. Именно так пишут переходы в перехватчиках.
    void* detourNewRound =
        (void*)(ULONGLONG)&DetourOnNewRound;
    void* detourTick =
        (void*)(ULONGLONG)&DetourOnTick;
    void* detourDamage =
        (void*)(ULONGLONG)&DetourOnDamage;
    void* detourChat =
        (void*)(ULONGLONG)&DetourChatMessage;

    const Target targets[] = {
        {&g_hookOnNewRound, natives.onNewRound, detourNewRound,
         "onNewRound"},
        {&g_hookTick, natives.tick, detourTick, "tick"},
        {&g_hookOnDamage, natives.onDamage, detourDamage, "onDamage"},
        {&g_hookChat, natives.chatMessage, detourChat, "chatMessage"},
    };

    int installed = 0;

    for (int i = 0; i < kHookCount; i++) {
        if (targets[i].target == nullptr) {
            g_results[i] = HookResult::BadTarget;
            LOG_INFO("EVBots: hook %s not installed - target not found",
                     targets[i].name);
            continue;
        }

        if (HookSuppressed(targets[i].name)) {
            g_results[i] = HookResult::BadTarget;
            continue;
        }

        const HookResult r =
            targets[i].hook->Install(targets[i].target, targets[i].detour, targets[i].name);
        g_results[i] = r;

        LOG_INFO("EVBots: hook %s at %p - %s", targets[i].name,
                 targets[i].target, ToString(r));

        if (r == HookResult::Ok) {
            installed++;
        }
    }

    ApplyPreRoundPatches();
    g_resultsValid = true;
    return installed;
}

}  // namespace evbots