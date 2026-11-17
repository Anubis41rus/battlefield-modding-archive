// ModdingInterface.cpp — реализация мод-системы EVBots.

#include "ModdingInterface.h"

#include <cstdio>
#include <cstdlib>
#include <intrin.h>  // _ReturnAddress (сборка 237)
#include <cstring>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <windows.h>

extern "C" {
#include "lua.h"
}

#include "EngineHooks.h"
#include "EngineResources.h"  // сборка 234: GUID узла для UI:PushScreen
#include "TeamCapacity.h"
#include "RoundPlayerCounts.h"
#include "NativeRconGameplay.h"
#include "EvNetBridge.h"
#include "InlineHook.h"
#include "SpmProbe.h"
#include "EVBotsLog.h"
#include "LuaRuntime.h"
#include "VextPlayerManager.h"
#include "VextResourceManager.h"
#include "VextRcon.h"  // сборка 198: раунд для Level:Loaded (mapList.getRounds)
#include "VextClientInput.h"  // сборка 228: InputManager клиента
#include "VextWebUi.h"  // сборка 228: WebUI модов поверх окна игры
#include "VextSpectator.h"
#include "NativeBlueprint.h"

#include <functional>
#include <vector>

namespace evbots {

namespace {
std::atomic<unsigned long> g_serverFrameThreadId{0};  // ★ 238-3: поток кадра сервера (0 — тик Lua не из кадра)
std::atomic<bool> g_serverFrameTickActive{false};     // ★ 238-3: перехват кадра поставлен (EVBOTS_SERVER_TICK=frame)
}  // namespace

bool EVBotsOnServerFrameThread() {
    const unsigned long id = g_serverFrameThreadId.load();
    return id != 0 && id == ::GetCurrentThreadId();
}

void EVBotsSetServerFrameThread(unsigned long threadId) { g_serverFrameThreadId = threadId; }

bool EVBotsServerFrameTickActive() { return g_serverFrameTickActive.load(); }

void EVBotsSetServerFrameTickActive() { g_serverFrameTickActive = true; }

namespace {

/// Флаг изоляции: EVBOTS_NO_HOOKS=1 пропускает установку наших detour'ов
/// (Lua/моды грузятся, движок не трогаем). Нужен, чтобы отделить краш,
/// вызываемый нашим перехватом, от краша самого движка/игры.
bool SkipEngineHooksRequested() {
    const char* v = std::getenv("EVBOTS_NO_HOOKS");
    return v != nullptr && v[0] == '1';
}

/// Чтение ЧИСЛОВОГО флага окружения: включено только если значение > 0.
/// ГРАБЛИ: ::GetEnvironmentVariableA(name, nullptr, 0) возвращает длину
/// строки, а не значение. Для "0" длина = 1, поэтому проверка `> 0`
/// включала перехватчики при выключенном флаге — из-за этого серия
/// изоляционных тестов была ложной и показывала «виноват пробник».
bool EVBotsEnvFlag(const char* name) {
    const char* v = std::getenv(name);
    if (v == nullptr) return false;
    return std::strtol(v, nullptr, 10) > 0;
}

// ── безопасный источник Engine:Update ───────────────────────────────────
// Хук tick на 0x00E5EF00 снят (краш из-за __thiscall vs __fastcall, см.
// EngineHooks.cpp). Вместо него Engine:Update рассылает фоновый таймер:
// движок честно обходит, а Lua-моды видят свои тики как в VU
// (всему миру хватает своих 30 Гц, нужных fun-bots).
// Частота 30 Гц (dt=1/30): fun-bots ждёт FIRST_SPAWN_DELAY=5.0 с
// (BotSpawner), т.е. ~150 тиков — таймер это покрывает.

/// Флаг изоляции таймера: EVBOTS_NO_UPDATE_TIMER=1 отключает рассылку
/// Engine:Update (нужно, чтобы на лету отключать именно этот новый механизм).
bool UpdateTimerSuppressed() {
    const char* v = std::getenv("EVBOTS_NO_UPDATE_TIMER");
    return v != nullptr && v[0] == '1';
}

std::atomic<bool> g_updateTimerStop{false};
std::thread g_updateTimerThread;
std::atomic<bool> g_serverLuaActive{false};
// 0: not deferred/test host, 1: pending native settings, 2: dispatching,
// 3: ready, 4: failed. TickGate serializes the actual Lua transition.
std::atomic<int> g_serverStartupState{0};

// Боевые перехватчики принадлежат процессу: их Install*Once не сбрасывается при
// выгрузке Lua. Снятие общего реестра ломает следующую загрузку модов. В тест-хосте
// реестр содержит проверочные цели, которые по контракту теста надо восстановить.
void ReleaseServerSessionHooks() {
    if (::GetModuleHandleA("bf3_server_Final.exe") != nullptr) {
        LOG_INFO("EVBots: Lua stopped; process-lifetime engine hooks retained");
        return;
    }
    const size_t removed = HookRegistry::Instance().RemoveAll();
    LOG_INFO("EVBots: test-host hooks removed %zu", removed);
}

// ★ Сборка 238: период тика = 1 / частота движка (ядро ev-core, как VU -high60/-high120: Engine:Update каждый кадр сервера).
float UpdateDt() {
    static const float dt = 1.0f / static_cast<float>(CoreFrequencyHz());
    return dt;
}
// ★ Сборка 156: dt — настоящее время между тиками (как в VU), не постоянные 1/30.
// Прогон 155: при создании ботов поток Lua тормозил (300 тиков за 48 с вместо 10), а мод считал время по
// тикам — «0,3 с между ботами» превращались в ~2 с, проверка команд «раз в 2 с» — в 3–5 с.
// Потолок 1 с — один долгий тик не должен сдвигать таймеры мода больше чем на секунду.
// (Сборка 168: флаг EVBOTS_FIXED_DT — dt = 1/30, как до 156, — удалён.)
constexpr float kMaxUpdateDt = 1.0f;

// ★ Сборка 188: тик по сроку, а не «сон 33 мс». sleep_for(33 мс) в Windows округляется вверх до шага системного таймера
// (15,6 мс) — 46,8 мс плюс работа тика: прогон 187 (23:54) — 300 тиков за ~14,5 с, ~20,7 Гц вместо 30. Мод считает время
// кадрами (BotManager: _CycleTimeL0 = 1/SharedUtils:GetTickrate() = 1/30, UpdateFast получает постоянный _L1CycleTime),
// поэтому руль техники (PidController без dt, VehicleMovement:UpdateYawVehicle) и газ обновлялись в 1,5 раза реже, чем у VU,
// где UpdateManager:Update идёт каждый кадр сервера. Ожидание — таймер высокого разрешения (Windows 10 1803+, только наш
// поток, частоту таймера процесса не меняем); нет таймера — sleep_until. Отстали (долгий тик) — без догоняющей пачки тиков,
// срок от «сейчас»: у движка долгий кадр тоже не повторяется. EVBOTS_TICK_DEADLINE=0 — как до 188.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
bool TickDeadlineEnabled() {
    const char* v = std::getenv("EVBOTS_TICK_DEADLINE");
    return !(v != nullptr && v[0] == '0');
}

class TickPacer {
public:
    TickPacer() : next_(std::chrono::steady_clock::now()) {
        timer_ = ::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    }
    ~TickPacer() {
        if (timer_ != nullptr) ::CloseHandle(timer_);
    }
    bool highRes() const { return timer_ != nullptr; }
    // Сборка 192: на сколько поток проснулся позже срока (вытеснение другими потоками / процессами), с.
    float lateness() const {
        const float late = std::chrono::duration<float>(std::chrono::steady_clock::now() - next_).count();
        return late > 0.0f ? late : 0.0f;
    }
    void wait() {
        using namespace std::chrono;
        next_ += duration_cast<steady_clock::duration>(duration<float>(UpdateDt()));
        const auto now = steady_clock::now();
        if (next_ <= now) {  // тик был дольше периода — следующий сразу, срок от «сейчас»
            next_ = now;
            return;
        }
        const long long left100ns = duration_cast<nanoseconds>(next_ - now).count() / 100;
        if (timer_ != nullptr) {
            LARGE_INTEGER due;
            due.QuadPart = -left100ns;  // относительное время, шаг 100 нс
            if (::SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) &&
                ::WaitForSingleObject(timer_, 1000) == WAIT_OBJECT_0) {
                return;
            }
        }
        std::this_thread::sleep_until(next_);
    }

private:
    HANDLE timer_ = nullptr;
    std::chrono::steady_clock::time_point next_;
};

// ★ 238-3: тело тика Lua сервера — общее для потока-таймера и кадра движка (EVBOTS_SERVER_TICK=frame, реестр А1).
// Статистика heartbeat — общая (тик идёт из одного места за раз: таймер не запускается, если стоит тик из кадра).
struct ServerTickStats {
    unsigned long long ticks = 0;
    int sinceBeat = 0;
    float maxDt = 0.0f;
    float tmWait = 0.0f, tmEng = 0.0f, tmMod = 0.0f;  // сборка 191: максимумы за 300 тиков
    double sumEng = 0.0, sumMod = 0.0;
    int slowTicks = 0;
    float tmLate = 0.0f, tmDrain = 0.0f;  // сборка 192: опоздание пробуждения и подгрузка ресурсов
    std::chrono::steady_clock::time_point beatStart = std::chrono::steady_clock::now();
};
ServerTickStats g_tick;
const char* g_tickSource = "таймер";

// ★ 239: задачи из чужих потоков (поток-ожидатель уровня), которые трогают Lua мода, — выполнить в тике сервера
// (в кадре движка при EVBOTS_SERVER_TICK=frame, как у VU). Пока тик не идёт — false: вызывающий делает сам.
std::mutex g_tickTasksMu;
std::vector<std::function<void()>> g_tickTasks;
std::atomic<bool> g_tickRunning{false};

bool RunOnServerTick(std::function<void()> task) {
    if (!g_tickRunning.load()) return false;
    std::lock_guard<std::mutex> lk(g_tickTasksMu);
    g_tickTasks.push_back(std::move(task));
    return true;
}

void RunTickTasks() {
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lk(g_tickTasksMu);
        tasks.swap(g_tickTasks);
    }
    for (auto& t : tasks) t();
}

void ServerTickOnce(float dt, float simDt) {
    if (!g_serverLuaActive.load()) return;
    const int startup = g_serverStartupState.load();
    if (startup != 0 && startup != 3) return;
    auto& lua = LuaRuntime::Instance();
    ServerTickStats& s = g_tick;
    g_tickRunning = true;
    RunTickTasks();
    if (dt > kMaxUpdateDt) dt = kMaxUpdateDt;
    if (simDt > kMaxUpdateDt) simDt = kMaxUpdateDt;
    if (dt > s.maxDt) s.maxDt = dt;
    {
        const auto td0 = std::chrono::steady_clock::now();
        ResourceDrainDeferred();  // ★ 161: load-хендлеры разделов, не успевших взять лок Lua
        const float drain = std::chrono::duration<float>(std::chrono::steady_clock::now() - td0).count();
        if (drain > s.tmDrain) s.tmDrain = drain;
    }
    // ★ Сборка 191: из чего складываются паузы тика (прогон 190: dt_max 0,2–0,25 с каждые 10 с, у VU пауз нет) —
    // ожидание лока Lua (его держат и хуки движка), Engine:Update (мост), UpdateManager:Update (мод).
    const auto tg0 = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::recursive_mutex> gate(lua.TickGate());
        if (!g_serverLuaActive.load()) return;
        const auto tg1 = std::chrono::steady_clock::now();
        EvNetDrain();  // ★ Сборка 219 (К4): NetEvents с клиентов — до Engine:Update
        lua.DispatchEngineEvent("Engine:Update",
                                { LuaRuntime::EngineEventArg::Num(dt),
                                  LuaRuntime::EngineEventArg::Num(simDt) });
        const auto tg2 = std::chrono::steady_clock::now();
        // Royale moves the gunship and starts its match logic in PreSim. The
        // timer fallback has no native frame context for these operations.
        if (EVBotsOnServerFrameThread()) {
            lua.DispatchEngineEvent("UpdateManager:Update",
                                    { LuaRuntime::EngineEventArg::Num(simDt),
                                      LuaRuntime::EngineEventArg::Num(0) });  // PreSim
        }
        // ★ Тики ботов: BotManager:OnUpdateManagerUpdate ждёт
        // 'UpdateManager:Update' с UpdatePass_PostFrame (=2). Без этого
        // боты спавнятся, но Update не идёт и SetLevel не вызывается.
        lua.DispatchEngineEvent("UpdateManager:Update",
                                { LuaRuntime::EngineEventArg::Num(dt),
                                  LuaRuntime::EngineEventArg::Num(2) });
        const auto tg3 = std::chrono::steady_clock::now();
        const float wWait = std::chrono::duration<float>(tg1 - tg0).count();
        const float wEng = std::chrono::duration<float>(tg2 - tg1).count();
        const float wMod = std::chrono::duration<float>(tg3 - tg2).count();
        if (wWait > s.tmWait) s.tmWait = wWait;
        if (wEng > s.tmEng) s.tmEng = wEng;
        if (wMod > s.tmMod) s.tmMod = wMod;
        s.sumEng += wEng;
        s.sumMod += wMod;
        if (wWait + wEng + wMod > 0.050f) ++s.slowTicks;
    }
    ++s.ticks;
    if (++s.sinceBeat >= 300) {  // 300 тиков, ~10 с при 30 Гц
        s.sinceBeat = 0;
        // Сборка 188: и настоящая частота тиков за последние 300 (у VU — тикрейт сервера, мод ждёт 30)
        const auto beatNow = std::chrono::steady_clock::now();
        const float beatSec = std::chrono::duration<float>(beatNow - s.beatStart).count();
        s.beatStart = beatNow;
        LOG_INFO("EVBots: heartbeat ticks=%llu lua=ready dt_max=%.3f hz=%.1f | макс: ждать лок %.0f мс, мост %.0f мс, мод %.0f мс; "
                 "в среднем мост %.1f мс, мод %.1f мс; тиков >50 мс %d; проснулся позже %.0f мс, ресурсы %.0f мс; тик: %s",
                 s.ticks, (double)s.maxDt, beatSec > 0.0f ? 300.0 / (double)beatSec : 0.0, s.tmWait * 1000.0,
                 s.tmEng * 1000.0, s.tmMod * 1000.0, s.sumEng * 1000.0 / 300.0, s.sumMod * 1000.0 / 300.0, s.slowTicks,
                 s.tmLate * 1000.0, s.tmDrain * 1000.0, g_tickSource);
        s.tmWait = s.tmEng = s.tmMod = 0.0f;
        s.sumEng = s.sumMod = 0.0;
        s.slowTicks = 0;
        s.tmLate = s.tmDrain = 0.0f;
        s.maxDt = 0.0f;
    }
}

void UpdateTimerLoop() {
    LOG_INFO("EVBots: Engine:Update timer started (%.0f Hz)",
             1.0 / UpdateDt());
    auto& lua = LuaRuntime::Instance();
    LOG_INFO("EVBots: Engine:Update dt = настоящее время");
    // Heartbeat раз в 10 с: на пустом сервере ни раундов, ни игроков —
    // в консоли не появилось бы НИЧЕГО про Lua, и это выглядит как «Lua
    // не работает». Периодический маркер снимает вопрос «живо ли».
    auto last = std::chrono::steady_clock::now();
    const bool deadline = TickDeadlineEnabled();
    TickPacer pacer;
    LOG_INFO("EVBots: тик Lua — %s, частота движка %d Гц", !deadline ? "сон 1/частота (как до 188, EVBOTS_TICK_DEADLINE=0)"
             : pacer.highRes() ? "по сроку 1/частота, таймер высокого разрешения" : "по сроку 1/частота, sleep_until (таймера высокого разрешения нет)",
             CoreFrequencyHz());
    // ★ Сборка 192: приоритет потока тика выше обычного. У VU UpdateManager:Update идёт в главном потоке движка; наш поток
    // обычного приоритета на одной машине с клиентом вытеснялся: прогон 191 — dt_max 0,14–0,19 с при работе тика 0–2 мс.
    // EVBOTS_TICK_PRIO=0 — обычный приоритет.
    {
        const char* pv = std::getenv("EVBOTS_TICK_PRIO");
        if (!(pv != nullptr && pv[0] == '0')) {
            const BOOL okPrio = ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
            LOG_INFO("EVBots: приоритет потока тика — выше обычного: %s", okPrio ? "да" : "НЕ удалось");
        }
    }
    while (!g_updateTimerStop.load()) {
        if (deadline) {
            pacer.wait();
            const float late = pacer.lateness();
            if (late > g_tick.tmLate) g_tick.tmLate = late;
        } else {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(static_cast<int>(UpdateDt() * 1000.0f)));
        }
        if (g_updateTimerStop.load()) {
            break;
        }
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        if (!lua.ready()) {
            continue;
        }
        ServerTickOnce(dt, dt);  // таймер: simDt = dt (своего шага симуляции у него нет)
    }
    LOG_INFO("EVBots: Engine:Update timer stopped");
}

bool ServerFrameTickRequested();  // ★ 238-3 (А1) — определения ниже, у тика клиента
bool InstallServerFrameTick();

/// Запустить источник Engine:Update сервера (один раз): кадр движка (EVBOTS_SERVER_TICK=frame) или фоновый таймер.
void StartUpdateTimer() {
    static bool s_frameTick = false;
    if (g_updateTimerThread.joinable() || s_frameTick) {
        return;  // уже запущен
    }
    if (UpdateTimerSuppressed()) {
        LOG_INFO("EVBots: EVBOTS_NO_UPDATE_TIMER=1 - Engine:Update "
                 "timer not enabled");
        return;
    }
    if (ServerFrameTickRequested()) {
        if (InstallServerFrameTick()) {
            s_frameTick = true;
            g_tickSource = "кадр движка";
            return;
        }
        LOG_WARN("EVBots: EVBOTS_SERVER_TICK=frame — перехват кадра не встал, тик Lua по таймеру, как раньше");
    }
    g_updateTimerStop = false;
    g_updateTimerThread = std::thread(UpdateTimerLoop);
}

/// Реализация с ровно пятью виртуальными методами.
class EVBotsModdingInterface final : public ModdingInterface {
public:
    bool Initialize() override {
        if (initialized_) {
            return true;
        }

        LOG_INFO("EVBots: mod system init");

        if (::GetModuleHandleA("bf3_server_Final.exe") != nullptr && !InstallTeamCapacity(true)) {
            LOG_ERR("LUA-MODE FAILED: TeamInfo expansion failed; mods are not started");
            return false;
        }
        if (::GetModuleHandleA("bf3_server_Final.exe") != nullptr && !InstallRoundPlayerCountCompatibility()) {
            LOG_ERR("LUA-MODE FAILED: round player count compatibility failed; mods are not started");
            return false;
        }

        // 1) таблица нативов движка: находим функции и заполняем структуру
        const bool engineFound = EngineHooks::ResolveNatives(&natives_);
        LOG_INFO("EVBots: engine %s, natives found %d",
                 engineFound ? "FOUND" : "not found (hardcoded addresses off)",
                 CountNatives());

        // 2) ставим хуки. В тестовом хосте они ведут на наши же функции,
        //    поэтому проверка работает без BF3.
        //    EVBOTS_NO_HOOKS=1 — пропускаем (изоляция: отделить наш код
        //    от краша движка/игры в логе RuntimeLog).
        if (SkipEngineHooksRequested()) {
            LOG_INFO("EVBots: EVBOTS_NO_HOOKS=1 - engine hooks NOT installed");
        } else {
            const int installed = EngineHooks::InstallAll(natives_);
            LOG_INFO("EVBots: hooks installed %d of %d", installed,
                     EngineHooks::kHookCount);

            // 2b) диагностика ServerPlayerManager: печатаем живых игроков
            //     (нужно, чтобы найти createPlayer для спавна ботов).
            //     Включается переменной окружения, чтобы не мешать ботам.
            // ВАЖНО: здесь читаем ЗНАЧЕНИЕ переменной, а не факт её наличия.
            // GetEnvironmentVariableA(name, nullptr, 0) возвращает длину,
            // и для "0" она равна 1 -> старая проверка `> 0` включала
            // перехватчики даже при выключенном флаге (ложные тесты).
            if (EVBotsEnvFlag("EVBOTS_SPM_PROBE") ||
                EVBotsEnvFlag("EVBOTS_SPM_MGR") ||
                EVBotsEnvFlag("EVBOTS_SPM_TICK") ||
                EVBotsEnvFlag("EVBOTS_SPM_SOLDIER") ||
                EVBotsEnvFlag("EVBOTS_SPM_ONCREATE"))
            {
                HMODULE self = ::GetModuleHandleA("bf3_server_Final.exe");
                if (self != nullptr)
                {
                    InstallSpmProbe((void*)self);
                }
            }
        }

        // 3) Lua-среда и моды (как veniceext: Lua + загрузчик Admin\Mods)
        auto& lua = LuaRuntime::Instance();
        const std::wstring modsRoot = LuaRuntime::DiscoverModsRoot();
        if (lua.Initialize(modsRoot, LuaContext::Server)) {
            const int found = lua.LoadMods();
            if (found < 0) {
                LOG_ERR("LUA-MODE FAILED: %s", lua.lastError().c_str());
                ReleaseServerSessionHooks();
                lua.Shutdown();
                return false;
            }
            if (found > 0) {
                if (lua.runInitScripts() < 0) {
                    LOG_ERR("LUA-MODE FAILED: ошибка инициализации мода; Lua остановлена");
                    ReleaseServerSessionHooks();
                    lua.Shutdown();
                    return false;
                }

                // ★ Сборка 192: сборщик мусора как у VU. veniceext FUN_10740460 (декомпилят re\vu\decomp\chunk00, загрузка
                // расширения прошла): lua_gc(L, LUA_GCINC=0xB, pause 0x69=105, stepmul 200, stepsize 0x10=16), затем дважды
                // lua_gc(L, LUA_GCCOLLECT=2). Lua та же — 5.4.2 (vubin\lua.dll, строка «Lua 5.4.2»). До 192 у нас были
                // умолчания 5.4 (pause 200, stepmul 100, stepsize 13): реже и длиннее шаги — паузы тика (прогон 191).
                {
                    std::lock_guard<std::recursive_mutex> gate(lua.TickGate());
                    if (lua.state() != nullptr) {
                        lua_gc(lua.state(), LUA_GCINC, 105, 200, 16);
                        lua_gc(lua.state(), LUA_GCCOLLECT);
                        lua_gc(lua.state(), LUA_GCCOLLECT);
                        LOG_INFO("EVBots: сборщик мусора Lua как у VU: инкрементальный, pause 105, stepmul 200, stepsize 16");
                    }
                }

                // В VU эти события рассылает veniceext по хукам движка после
                // загрузки модов. Мод подписывается на них в __init__.lua,
                // поэтому порядок: моды загружены -> Engine:Init -> Extension:Loaded.
                // Отсюда и начинается настоящая инициализация fun-bots
                // (RegisterEvents/RegisterHooks/...), и здесь же всплывает
                // следующий недостающий VEXT API.
                const bool nativeStartup = ::GetModuleHandleA("bf3_server_Final.exe") != nullptr;
                g_serverStartupState = nativeStartup ? 1 : 0;
                if (!nativeStartup &&
                    (!lua.DispatchEngineEvent("Engine:Init") || !lua.DispatchEngineEvent("Extension:Loaded"))) {
                    LOG_ERR("LUA-MODE FAILED: ошибка события запуска мода; Lua остановлена");
                    ReleaseServerSessionHooks();
                    lua.Shutdown();
                    return false;
                }

                // Маркер «как в Zeus»: по одному взгляду в EVBots.log видно,
                // что Lua-режим реально поднялся (загруженные моды + их версии).
                // Лаунчер ловит такие строки и выделяет их в окне сервера.
                std::string names;
                for (const auto& m : lua.mods()) {
                    if (!names.empty()) {
                        names += ", ";
                    }
                    names += m.name + " " + m.version;
                }
                if (nativeStartup)
                    LOG_INFO("LUA-MODE LOADED: %zu mod(s) [%s]; startup pending native resource/settings context",
                             lua.mods().size(), names.c_str());
                else
                    LOG_INFO("LUA-MODE READY: %zu mod(s) loaded [%s]", lua.mods().size(), names.c_str());
                for (const auto& m : lua.mods()) {
                    ConsolePrintf(m.error.empty() ? "info" : "error", "EVBots", "%s EVBots module '%s' %s.",
                                  m.error.empty() ? "Successfully loaded" : "Failed to load", m.name.c_str(),
                                  m.version.c_str());
                }

                // Безопасный источник Engine:Update — фоновый таймер вместо
                // снятого tick-хука (см. EngineHooks.cpp). Только на реальном
                // сервере: тест-хосту нужен ручной ввод тиков.
                if (::GetModuleHandleA("bf3_server_Final.exe") != nullptr) {
                    g_serverLuaActive = true;
                    EvNetBridgeStart(false);  // ★ Сборка 219 (К4): приём NetEvents от клиентов через ev-core
                    StartUpdateTimer();

                    // (Сборка 169: фейковый «Host» на старте — EVBOTS_EARLY_HOST / NO_FAKE_HOST — удалён по решению
                    // пользователя 16.10 «как у VU, Host убрать»: в список мода попадает сам человек из движка.)

                    // Сервер уже в IN_GAME — карта загружена. В VU уровень
                    // шлёт Level:Loaded из движка; на пустом сервере раунд не
                    // стартует (onNewRound не зовётся), поэтому шлём сами.
                    // Без этого fun-bots не начинает спавн: в
                    // BotSpawner.RegisterVars _FirstSpawnDelay=1e10, и первый
                    // спавн никогда не наступает.
                    // ВАЖНО: обработчик мода берёт p_LevelName и делает
                    // p_LevelName:gsub — при одном числе падает на
                    // "attempt to index a number value" (краш 11.10.2026).
                    // ★ Сборка 161: load-хендлеры мода (RegisterInstanceLoadHandler) — как в VU.
                    // Первый уровень загружен ДО модов, поэтому сначала догоняющий вызов по
                    // уже загруженным инстансам, затем перехват загрузки разделов на будущее.
                    LOG_INFO("EngineRes: вызов ResourceLoadHandlersStart (server init)");
                    ResourceLoadHandlersStart();
                    LOG_INFO("EngineRes: возврат ResourceLoadHandlersStart (server init)");
                    // ★ Сборка 185: имя («Levels/MP_012/MP_012») и режим — из движка, как VU (было всегда MP_001).
                    // VU шлёт Level:Loaded, когда уровень загружен. Прогон 185 (05:08): моды поднялись в 05:08:28, а
                    // движок начал грузить MP_012 в 05:08:33 — имя ещё не читалось, мод получил запасной MP_001.
                    // Истечение времени не заменяет сигнал движка или готовые сущности.
                    auto sendLevelLoaded = [] {
                        if (::GetModuleHandleW(L"bf3_server_Final.exe") != nullptr &&
                            std::getenv("EVBOTS_LEVEL_NAME") == nullptr && !EngineLevelReady()) {
                            LOG_WARN("EVBots: Level:Loaded отложен — имя уровня или GameMode ещё не прочитаны");
                            return;
                        }
                        char lvl[128];
                        char mode[64];
                        EngineLevelNameAndMode(lvl, sizeof(lvl), mode, sizeof(mode));
                        // ★ Сборка 198: round и roundsPerMap — у сервера (RCON mapList.getRounds → OK <текущий> <всего>), как
                        // VU (level_loaded.md). Было 1 и 2 всегда, а на смене раунда — счётчик процесса: fun-bots при
                        // TeamSwitchMode = SwitchForRoundTwo (p_Round ~= _LastRound, BotSpawner.lua:75) менял ботам команды на
                        // каждой загрузке, хотя в MapList по 1 раунду на карту. Не ответил — 0 и 1 (один раунд, без смены).
                        double round = 0.0, perMap = 1.0;
                        std::vector<std::string> resp;
                        if (EVBotsRconCommand({"mapList.getRounds"}, resp, 1500) && resp.size() >= 3 && resp[0] == "OK") {
                            round = std::atof(resp[1].c_str());
                            perMap = std::atof(resp[2].c_str());
                            LOG_INFO("EVBots: Level:Loaded — раунд %s из %s (mapList.getRounds)", resp[1].c_str(),
                                     resp[2].c_str());
                        } else {
                            LOG_WARN("EVBots: Level:Loaded — mapList.getRounds не ответил, раунд 0 из 1");
                        }
                        LuaRuntime::Instance().DispatchEngineEvent(
                            "Level:Loaded",
                            { LuaRuntime::EngineEventArg::Str(lvl),
                              LuaRuntime::EngineEventArg::Str(mode),
                              LuaRuntime::EngineEventArg::Num(round),
                              LuaRuntime::EngineEventArg::Num(perMap) });
                    };
                    // Готов = имя уровня читается И сущности уровня (точки спавна + флаги) есть и их число не меняется 1 с.
                    if (std::getenv("EVBOTS_LEVEL_NAME") != nullptr) {
                        sendLevelLoaded();
                    } else {
                        LOG_INFO("EVBots: Level:Loaded — ждём загрузки уровня и его сущностей (как VU)");
                        // Сборка 186 (прогон 20:01, MP_007 CQL): предел 20 с убран — большая карта грузится дольше,
                        // Level:Loaded ушёл при 0 сущностей, у мода 0 флагов: авиация с целью (0,0,0) не взлетала, мод
                        // убивал экипажи (GameDirector.lua:217). Теперь главный признак — сигнал движка server.onLevelLoaded
                        // (ServerMapSequencerLevelLoadedMessage, перехват RCON onMessage ставим заранее), как у VU.
                        EVBotsRconHookEarly();
                        const long seq0 = EVBotsLevelLoadedSeq();
                        std::thread([sendLevelLoaded, seq0] {
                            int waited = 0, stable = 0, last = -1, sinceSignal = -1;
                            const char* why = nullptr;
                            while (true) {
                                if (!LuaRuntime::Instance().ready()) return;
                                if (waited >= 900 && waited % 300 == 0) {
                                    LOG_WARN("EVBots: Level:Loaded: жду имя уровня и GameMode %.0f с; запасной Conquest не подставляю",
                                             waited / 10.0);
                                }
                                const bool ready = EngineLevelReady();
                                const int n = ready ? EVBotsEngineLevelEntityCount() : 0;
                                if (n > 0 && n == last) ++stable; else stable = 0;
                                last = n;
                                if (sinceSignal < 0 && EVBotsLevelLoadedSeq() != seq0) sinceSignal = 0;
                                if (sinceSignal >= 0) {
                                    // движок сказал «уровень загружен»: ждём, пока сущности не меняются 1 с (или 5 с, если их нет)
                                    if (stable >= 10) { why = "сигнал движка + сущности"; break; }
                                    if (ready && ++sinceSignal >= 50) { why = "сигнал движка, сущностей нет 5 с"; break; }
                                } else if (stable >= 30) {
                                    // запас: сигнал не пришёл (перехват не встал) — сущности не меняются 3 с
                                    why = "сущности не меняются 3 с (без сигнала движка)";
                                    break;
                                }
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                ++waited;
                            }
                            LOG_INFO("EVBots: уровень и сущности (%d) готовы через %.1f с (%s) — шлём Level:Loaded", last,
                                     waited / 10.0, why);
                            // сборка 211: сигнал первой карты учтён — следующий server.onLevelLoaded значит новый уровень
                            if (sinceSignal >= 0) EVBotsSetStartupLevelSeq(EVBotsLevelLoadedSeq());
                            // ★ 239: как VU — событие моду в кадре движка, не из этого потока (прогон 28.10: обработчик
                            // мода звал RCON:SendCommand из потока-ожидателя, мимо кадра; при тике из кадра это и порча
                            // данных движка, и RCON без ответа). Нет тика — шлём отсюда, как раньше.
                            if (!RunOnServerTick(sendLevelLoaded)) sendLevelLoaded();
                        }).detach();
                    }
                    // ★ Пути ботов (сборка 32, 13.10.2026).
                    // БЫЛО: здесь шли ДВА dispatch подряд — Level:Loaded и
                    // сразу NodeCollection:FinishedLoading. Разбор прогона 31
                    // показал, что это неверно: FinishedLoading МОД ШЛЁТ САМ
                    // из шага 50 своей стейт-машины загрузки путей
                    // (NodeCollection.lua:1375), когда SQL реально дочитан
                    // (7147 строк MP_001_ConquestSmall0_table). Наш досрочный
                    // уходил РАНЬШЕ — мод успевал только поставить
                    // m_MapCompletelyLoaded=true с ПУСТЫМ списком целей, и
                    // боты не получали SetLevel (0 строк INPUT в логе).
                    // ПРАВИЛЬНЫЙ порядок: Level:Loaded (мод сам запускает
                    // StartLoad на тиках Engine:Update) → ... → мод сам
                    // шлёт FinishedLoading → цели → движение.
                    // (Сборка 168: флаг EVBOTS_EARLY_NODES — досрочный FinishedLoading, вредит — удалён.)
                }
            } else if (modsRoot.empty()) {
                LOG_WARN("EVBots: Admin\\Mods folder not found - mods "
                         "not loaded (set EVBOTS_MODS)");
            }
        } else {
            LOG_ERR("LUA-MODE FAILED: Lua initialization failed");
            ReleaseServerSessionHooks();
            lua.Shutdown();
            return false;
        }

        // 4) без движка мод-система всё равно поднимается: Lua и моды
        //    должны грузиться и на сервере, и в тесте.
        initialized_ = true;
        return true;
    }

    void OnTick() override {
        ++state_.tickCount;
        OnTickTrampoline();
    }

    void OnNewRound() override {
        ++state_.roundCount;
        state_.tickCount = 0;
        state_.levelLoaded = true;

        LOG_INFO("EVBots: new round #%lu", state_.roundCount);

        // ★ Сборка 160 (фикс GameStall): кэши ассетов скана валидны только для своего
        // уровня — адреса объектов прошлого уровня уже не те. Сбрасываем позитивный
        // кэш, негативный и hint-регион, иначе боты нового уровня получат чужие объекты.
        evbots::ResetAssetCaches();

        // ★ Сборка 161: load-хендлеры для инстансов нового уровня, которые не поймал перехват.
        ResourceLoadHandlersNewRound();

        // Раунд начался — значит уровень загружен. VU шлёт Level:Loaded из
        // движка; без него fun-bots не стартует: в BotSpawner.RegisterVars
        // _FirstSpawnDelay = 1e10, а обнуляет его только OnLevelLoaded —
        // первый спавн иначе не наступит никогда. Обработчик мода берёт
        // p_LevelName строкой (p_LevelName:gsub) — шлём строки, а не числа.
        if (::GetModuleHandleW(L"bf3_server_Final.exe") == nullptr || EngineLevelReady()) {
            char lvl[128];
            char mode[64];
            EngineLevelNameAndMode(lvl, sizeof(lvl), mode, sizeof(mode));  // сборка 185: из движка, как VU
            LuaRuntime::Instance().DispatchEngineEvent(
                "Level:Loaded",
                { LuaRuntime::EngineEventArg::Str(lvl),
                  LuaRuntime::EngineEventArg::Str(mode),
                  LuaRuntime::EngineEventArg::Num(
                      static_cast<double>(state_.roundCount)),
                  LuaRuntime::EngineEventArg::Num(2.0) });
            // ★ Пути ботов каждый раунд (см. выше).
            LuaRuntime::Instance().DispatchEngineEvent(
                "NodeCollection:FinishedLoading", {});
        }

        OnNewRoundTrampoline();
    }

    void QueryState(EngineState* out) const override {
        if (out == nullptr) {
            return;
        }

        *out = state_;
        out->levelLoaded = state_.levelLoaded;
    }

    void Shutdown() override {
        if (!initialized_) {
            return;
        }

        // останавливаем фоновый таймер Engine:Update до снятия хуков, чтобы
        // поток не стучал в Lua во время выгрузки
        g_serverLuaActive = false;
        g_serverStartupState = 0;
        g_tickRunning = false;
        g_updateTimerStop = true;
        if (g_updateTimerThread.joinable()) {
            g_updateTimerThread.join();
        }
        // ★ Сборка 186: как VU — моды узнают о выгрузке (fun-bots убирает ботов и сохраняет настройки).
        LuaRuntime::Instance().DispatchEngineEvent("Extension:Unloading", {});

        ReleaseServerSessionHooks();
        LuaRuntime::Instance().Shutdown();
        {
            std::lock_guard<std::mutex> lock(g_tickTasksMu);
            g_tickTasks.clear();
        }
        initialized_ = false;
    }

    const EngineNatives& natives() const { return natives_; }

private:
    int CountNatives() const {
        int n = 0;
        if (natives_.onNewRound) ++n;
        if (natives_.tick) ++n;
        if (natives_.chatMessage) ++n;
        if (natives_.onDamage) ++n;
        return n;
    }

    EngineNatives natives_{};
    EngineState state_{};
    bool initialized_ = false;
};

/// Ленивый синглтон: создаётся при первом обращении.
ModdingInterface* Instance() {
    static EVBotsModdingInterface impl;
    return &impl;
}

}  // namespace

bool EnsureNativeModStartup() {
    auto& lua = LuaRuntime::Instance();
    std::lock_guard<std::recursive_mutex> gate(lua.TickGate());
    const int state = g_serverStartupState.load();
    if (state == 0 || state == 3) return true;
    if (state != 1 || !lua.ready()) return false;
    if (eng::FindSettings("GameModeSettings") == 0) {
        LOG_ERR("LUA-MODE FAILED: native resource callback has no GameModeSettings; startup remains pending");
        return false;
    }
    g_serverStartupState = 2;
    if (!lua.DispatchEngineEvent("Engine:Init") || !lua.DispatchEngineEvent("Extension:Loaded")) {
        g_serverStartupState = 4;
        g_serverLuaActive = false;
        LOG_ERR("LUA-MODE FAILED: native-context startup event failed; engine updates disabled");
        return false;
    }
    g_serverStartupState = 3;
    LOG_INFO("LUA-MODE READY: %zu mod(s) loaded; Engine:Init / Extension:Loaded delivered in native resource context",
             lua.mods().size());
    return true;
}

// Боевой автозапуск (по образцу botlayer.dll): активатор грузит нас удалённым
// CreateRemoteThread → LoadLibraryExW в процесс bf3_server_Final.exe, поэтому
// в DllMain нельзя делать тяжёлую работу (loader lock). Поднимаем фоновый
// поток, который после короткой паузы (движок успевает развернуться) вызовет
// Initialize(). В тест-хосте этот поток НЕ создаётся — там Initialize зовут
// вручную после подмены целей.
DWORD WINAPI AutostartThread(LPVOID) {
    ::Sleep(3000);
    LOG_INFO("EVBots: auto-start combat mode");
    ConsoleWrite("info", "EVBots", "Initializing the EVBots engine (VeniceEXT API)...");
    Instance()->Initialize();
    return 0;
}

bool EngineServerPresent() {
    return ::GetModuleHandleA("bf3_server_Final.exe") != nullptr;
}

// ── клиентский режим (сборка 215, К3) ─────────────────────────────────────

/// Клиент: в процессе bf3.exe и нет серверного exe.
bool EngineClientPresent() {
    return ::GetModuleHandleA("bf3.exe") != nullptr && !EngineServerPresent();
}

namespace {

std::atomic<bool> g_clientMode{false};
std::atomic<bool> g_clientStopRequested{false};

// .text у bf3.exe на диске зашифрован DRM; в памяти код расшифровывается через ~4 с после старта (лог activation:
// «код расшифрован через 4032 мс (проба по 0x0074d3c0)»). Ждём того же признака: по 0x0074D3C0 — байты расшифрованного
// кода. Эталон снят с zbf3.exe (расшифрованный снимок того же bf3.exe, timestamp 0x511C9356): mov eax,[esp+8];
// mov ecx,[esp+4]; mov [ecx*4+0x2360A38],eax; ret. Читаем через ReadProcessMemory — страница может быть ещё закрыта.
constexpr uintptr_t kDecryptProbeVa = 0x0074D3C0;
constexpr unsigned char kDecryptProbe[16] = {0x8B, 0x44, 0x24, 0x08, 0x8B, 0x4C, 0x24, 0x04,
                                             0x89, 0x04, 0x8D, 0x38, 0x0A, 0x36, 0x02, 0xC3};

bool ClientCodeDecrypted() {
    unsigned char buf[sizeof(kDecryptProbe)] = {};
    SIZE_T got = 0;
    if (!::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(kDecryptProbeVa), buf, sizeof(buf),
                             &got) ||
        got != sizeof(buf)) {
        return false;
    }
    return std::memcmp(buf, kDecryptProbe, sizeof(buf)) == 0;
}

/// Моды клиента. У VU клиент получает ext\client и ext\shared с сервера при входе (канал Harmony) — у нас это К4.
/// До К4: EVBOTS_CLIENT_MODS = папка Admin\Mods (стенд разработки); нет — Lua без модов.
std::wstring ClientModsRoot() {
    wchar_t env[MAX_PATH]{};
    const DWORD n = ::GetEnvironmentVariableW(L"EVBOTS_CLIENT_MODS", env, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    const DWORD attr = ::GetFileAttributesW(env);
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        LOG_WARN("EVBots: клиент: EVBOTS_CLIENT_MODS указывает не на папку — моды не грузим");
        return {};
    }
    return std::wstring(env, n);
}

// ── тик клиента: хук кадра игрового цикла, как у VU (К5 шаг 2, сборка 215) ──
// Факт (vu-core.dll, установщик хуков 10186EF0): VU ставит хук на 0x0040A470 — один кадр игрового цикла bf3.exe
// (`do {} while (frame(loop))` в 004171F0/00417270, поток GameLoop или главный). Обход VU 10196DF0: зовёт оригинал;
// вернул 0 — выход из игры, событий нет; иначе Engine:Update((float)[[loop+4]+0xBC]+0x58, (float)[...]+0x50).
// loop+4 = Main (ctor 00415E10, пишет [0x022E3790]), Main+0xBC = таймер кадра (ctor 01195870(.., 30, 1)), +0x58 —
// deltaTime, +0x50 — simulationDeltaTime (порядок аргументов Engine:Update по VU-Docs). Таблица хуков vu-core
// клиентская: на сервере 0x0040A470 — середина функции. Сверка пролога по zbf3.exe — ниже (правило 18).
constexpr uintptr_t kClientFrameVa = 0x0040A470;
constexpr unsigned char kClientFramePrologue[8] = {0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x04, 0x8B, 0x01};
constexpr uintptr_t kMainTimerOff = 0xBC;
constexpr uintptr_t kTimerDtOff = 0x58;
constexpr uintptr_t kTimerSimDtOff = 0x50;

InlineHook g_hookClientFrame;
std::atomic<unsigned long long> g_clientFrames{0};
std::atomic<bool> g_clientStartupPending{false};

/// Чтение dt из таймера кадра без C++-объектов (SEH). false — указатели ещё не готовы. timerOff — поле таймера у Main
/// (клиент 0xBC, сервер 0xDC).
bool ReadFrameDt(void* loop, uintptr_t timerOff, double* dt, double* simDt) {
    __try {
        const uintptr_t mainObj = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(loop) + 4);
        if (mainObj == 0) return false;
        const uintptr_t timer = *reinterpret_cast<uintptr_t*>(mainObj + timerOff);
        if (timer == 0) return false;
        *dt = *reinterpret_cast<double*>(timer + kTimerDtOff);
        *simDt = *reinterpret_cast<double*>(timer + kTimerSimDtOff);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadClientFrameDt(void* loop, double* dt, double* simDt) { return ReadFrameDt(loop, kMainTimerOff, dt, simDt); }

float ClampFrameDt(double v) {
    // в Lua — только конечные числа (NaN/inf роняют движок и мод), потолок 1 с — как у тика сервера
    if (!(v == v) || v < 0.0 || v > 1.0e6) return 0.0f;
    return v > kMaxUpdateDt ? kMaxUpdateDt : static_cast<float>(v);
}

// ── ★ 238-3 (реестр А1): тик Lua сервера из кадра игрового цикла, как у VU — пока за флагом EVBOTS_SERVER_TICK=frame ──
// Факт (Ghidra bf3_server_Final.exe, 26.10): 0x0040EFB0 — двойник клиентского кадра 0x0040A470 (у VU хук там): fastcall(loop),
// Main::update = vtable[+0x10] у [loop+4], PostMessageA 0x800C, счётчик [loop+8]; возврат — AL 1/0 (проверка 26.10: скелет тот
// же, у сервера свои доп. ветки; что [loop+4] у вызова 0x004256A3 — Main, не прослежено). Зовут его только через переходник 0x0040278E
// (`E9 1D C8 00 00`; вызовы 0x004256A3 и 0x00425222). Пролог 56 8B F1 80 3E 00 75 1E — в первых 8 байтах JNZ rel8, перехват
// ядра (кража 8 байт) не встаёт, поэтому переписываем смещение переходника: 8 байт одной атомарной записью ядра, хвост — как
// был. Main сервера: ctor 0x00423570 (vtable 0x024393D0), таймер кадра +0xDC (ctor 0x00849AB0(.., 30, 1); у клиента +0xBC),
// у таймера +0x58 dt (геттер 0x0083DDF0), +0x50 simDt (0x0083DDE0) — как у клиента. Не прогнано. Почему не по умолчанию —
// реестр А1/А2: RCON:SendCommand из Lua идёт по TCP к своему же серверу (если сервер разбирает RCON в главном потоке — Lua
// ждёт сам себя: в журнале строка «RCON: ответ … мс в потоке кадра»), операции движка из Lua — через очереди хука тика
// (у VU — синхронно), создание позднего бота в потоке движка не ждёт хук (RunLateCreateInHook).
constexpr uintptr_t kServerFrameVa = 0x0040EFB0;
constexpr uintptr_t kServerFrameThunkVa = 0x0040278E;
constexpr unsigned char kServerFrameThunk[5] = {0xE9, 0x1D, 0xC8, 0x00, 0x00};
constexpr unsigned char kServerFramePrologue[8] = {0x56, 0x8B, 0xF1, 0x80, 0x3E, 0x00, 0x75, 0x1E};
constexpr uintptr_t kServerMainTimerOff = 0xDC;
std::atomic<unsigned long long> g_serverFrames{0};

using ServerFrameFn = unsigned(__fastcall*)(void* loop);

unsigned __fastcall ServerFrameDetour(void* loop) {
    const unsigned keepRunning = reinterpret_cast<ServerFrameFn>(kServerFrameVa)(loop);
    if ((keepRunning & 0xFFu) == 0) return keepRunning;  // выход из игры — событий нет (как у VU)
    auto& lua = LuaRuntime::Instance();
    if (!g_serverLuaActive.load() || !lua.ready()) return keepRunning;
    double dtRaw = 0.0, simRaw = 0.0;
    if (!ReadFrameDt(loop, kServerMainTimerOff, &dtRaw, &simRaw)) return keepRunning;
    if (g_serverFrames.fetch_add(1) == 0) {
        EVBotsSetServerFrameThread(::GetCurrentThreadId());
        LOG_INFO("EVBots: сервер: ★ первый тик Lua из кадра движка (0x%08X): dt=%.4f simDt=%.4f, поток %lu",
                 static_cast<unsigned>(kServerFrameVa), dtRaw, simRaw, ::GetCurrentThreadId());
    }
    if (g_serverStartupState.load() == 1 && !ResourceEnsureNativeStartup()) return keepRunning;
    ServerTickOnce(ClampFrameDt(dtRaw), ClampFrameDt(simRaw));
    return keepRunning;
}

bool ReadCode(uintptr_t va, void* out, size_t n) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(va), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ★ 239 (28.10): тик Lua из кадра движка — ПО УМОЛЧАНИЮ, как у VU. В тике-таймере Lua (мост и мод) звала движок из своего
// потока параллельно с главным: порча солдата бота в технике и падения сервера (60 Гц — 2 из 2 прогонов 28.10, 238-5/6;
// 30 Гц — 234–236). RCON:SendCommand из кадра — в процессе (VextRcon.cpp, А2). EVBOTS_SERVER_TICK=timer — прежний таймер.
bool ServerFrameTickRequested() {
    const char* v = std::getenv("EVBOTS_SERVER_TICK");
    return v == nullptr || _stricmp(v, "timer") != 0;
}

/// Переходник кадра → наш обработчик. Сверка: переходник и пролог функции кадра (правило 18), иначе ничего не пишем.
bool InstallServerFrameTick() {
    unsigned char thunk[8] = {}, prologue[8] = {};
    if (!ReadCode(kServerFrameThunkVa, thunk, sizeof(thunk)) || !ReadCode(kServerFrameVa, prologue, sizeof(prologue)) ||
        std::memcmp(thunk, kServerFrameThunk, sizeof(kServerFrameThunk)) != 0 ||
        std::memcmp(prologue, kServerFramePrologue, sizeof(prologue)) != 0) {
        LOG_WARN("EVBots: сервер: тик из кадра — переходник 0x%08X или пролог 0x%08X не совпал, не ставлю",
                 static_cast<unsigned>(kServerFrameThunkVa), static_cast<unsigned>(kServerFrameVa));
        return false;
    }
    unsigned char repl[8];
    std::memcpy(repl, thunk, sizeof(repl));
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(&ServerFrameDetour) -
                                             static_cast<intptr_t>(kServerFrameThunkVa + 5));
    std::memcpy(repl + 1, &rel, 4);
    if (!PatchBytes(reinterpret_cast<void*>(kServerFrameThunkVa), thunk, repl, sizeof(repl),
                    "тик Lua: кадр сервера (переходник 0x0040278E)")) {
        LOG_WARN("EVBots: сервер: тик из кадра — запись переходника не удалась");
        return false;
    }
    EVBotsSetServerFrameTickActive();
    LOG_INFO("EVBots: сервер: ★ тик Lua — из кадра движка, как у VU (переходник 0x%08X -> %p, кадр 0x%08X; EVBOTS_SERVER_TICK=frame)",
             static_cast<unsigned>(kServerFrameThunkVa), reinterpret_cast<void*>(&ServerFrameDetour),
             static_cast<unsigned>(kServerFrameVa));
    return true;
}

// ── события клиента: слушатель сообщений движка, как MessageManager у VU (К5 шаг 3, сборка 215) ──
// Факт (veniceext 10C14D60): VU подписывает свой MessageManager на 55 категорий слотом 18E74F80 = 004A5D90.
// Факт (zbf3): подписка = thiscall 004A5D90(ecx = [0x022ED3B8], u32 категория, Listener*) — так же зовёт Main::Main
// (0041603A, категория Core); 004A5D90: mov ecx,[ecx]; jmp 004A58F0 (ret 8). Слушатель: vtable[0] — приём
// сообщения (thiscall, const Message&; у Main — 0040BEE0, ret 4), vtable[1] — приоритет (u16; у Main 0x64 = 00493A80).
// Сообщение: +4 — категория, +8 — тип (djb2-xor имени со знаком = MessageCategory/MessageType VU; так читает VU:
// 10C0F710 `[msg+4] == ClientPlayer`, 10C1553D `[msg+8]`, `[msg+4]`).
// Какие сообщения VU превращает в события (факт, re\client-k1\vu-message-events.txt):
//   ClientLevelLoadedMessage -> Level:Loaded, ClientLevelFinalizedMessage -> Level:Finalized, любое -> Engine:Message
//   (в конце обработчика 10C15090, вне условий — 10C1553D..10C1559A),
//   ClientPlayerConnect/Deleted/Killed/SwitchSquad/ChangedSquadLeaderStatus, ClientCharacterSpawnDone -> Player:*/Soldier:Spawn
//   (последние — когда будет PlayerManager клиента, шаг 4).
constexpr uintptr_t kMsgManagerHolderVa = 0x022ED3B8;        // Core (как Main::Main и VU)
constexpr uintptr_t kClientMsgManagerHolderVa = 0x02380B58;  // остальные категории клиента (VU 10C14D7F)
constexpr uint32_t kMsgCategoryCore = 0x7C813E9E;
constexpr uintptr_t kMsgSubscribeVa = 0x004A5D90;
constexpr unsigned char kMsgSubscribeCode[4] = {0x8B, 0x09, 0xE9, 0x59};  // mov ecx,[ecx]; jmp rel32 (-> 004A58F0)
constexpr uint32_t kMsgClientLevelLoaded = 0xDE75B306;     // ClientLevelLoadedMessage
constexpr uint32_t kMsgClientLevelFinalized = 0x6A31005F;  // ClientLevelFinalizedMessage

// Категории подписки — клиентские и общие из списка VU (25 из 54). VU в клиенте подписывает ещё 27 Server*, Statistics и
// VeniceNetwork — мы нет (реестр В13). djb2-xor, сверено с VU-Docs messagecategory.md.
constexpr uint32_t kClientMsgCategories[] = {
    0x7C813E9E,  // Core
    0xA23A0E7C,  // Client
    0xD265AF25,  // ClientCharacter
    0xC76F63C2,  // ClientCollision
    0x8FD32FEA,  // ClientConnection
    0xAA595C9D,  // ClientControllable
    0x197C291A,  // ClientControllableToComponents
    0x3C00EC66,  // ClientDynamicModel
    0x508F1EB6,  // ClientGameplay
    0xDAA58EAA,  // ClientLevel
    0xD65BFE6B,  // ClientMetrics
    0x78B0AA06,  // ClientOverlay
    0x24FE118F,  // ClientPlayer
    0x366FB6B6,  // ClientSoldier
    0xDAAC4DDF,  // ClientSound
    0xDABDEC47,  // ClientSpawn
    0xDABBBBAB,  // ClientState
    0x9148DBEB,  // ClientStaticModel
    0x22892FA4,  // ClientVehicle
    0x2862813E,  // ClientWeapon
    0xC67D277B,  // Network
    0x9C775C5C,  // Presence
    0xA378479B,  // Session
    0x005973B9,  // UI
    0x89062F07,  // UINetwork
};

struct EngineMessageView {
    void* vtable;
    uint32_t category;
    uint32_t type;
};

std::atomic<bool> g_msgListenerOn{false};
std::atomic<unsigned long long> g_engineMessages{0};

// ★ Сборка 258 (гость друга 02.11: «бах — всё зависло», Alt+F4, потом «вечная загрузка»; по серверу клиент замолчал через
// ~11 с после спавна). Сторож кадра клиента: отдельный поток раз в 1 с смотрит, когда игра последний раз входила в кадр;
// кадр стоит 5/30/120 с — строка «КЛИЕНТ ЗАВИС» со стадией (кадр движка или наш Lua), последним сообщением движка и стеком
// главного потока (адреса возврата модулей). Пишется в тихом журнале, файл сбрасывается писателем журнала раз в 50 мс.
std::atomic<unsigned long long> g_frameEnterMs{0};
std::atomic<int> g_frameStage{0};  // 0 между кадрами, 1 кадр движка, 2 наш Lua после кадра
std::atomic<unsigned long> g_frameThreadId{0};
std::atomic<uint32_t> g_lastMsgType{0};
std::atomic<uint32_t> g_lastMsgCategory{0};
std::atomic<unsigned long long> g_lastMsgMs{0};
std::atomic<bool> g_watchdogStarted{false};

/// Модуль+смещение адреса (вызывать, когда главный поток НЕ остановлен: GetModuleHandleEx берёт лок загрузчика).
void DescribeCodeAddr(uintptr_t a, char* out, size_t n) {
    HMODULE mod = nullptr;
    char path[MAX_PATH] = "?";
    if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCSTR>(a), &mod) &&
        mod != nullptr) {
        ::GetModuleFileNameA(mod, path, MAX_PATH);
        const char* name = std::strrchr(path, '\\');
        snprintf(out, n, "%s+0x%lX", name != nullptr ? name + 1 : path,
                 static_cast<unsigned long>(a - reinterpret_cast<uintptr_t>(mod)));
    } else {
        out[0] = '\0';
    }
}

/// Снимок стека главного потока: остановить, взять регистры и 2048 слов стека, отпустить; разбор — уже после.
void LogFrameThreadStack(unsigned long tid) {
    HANDLE h = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (h == nullptr) {
        LOG_WARN("EVBots: КЛИЕНТ ЗАВИС: поток кадра %lu не открыть (ошибка %lu)", tid, ::GetLastError());
        return;
    }
    static uintptr_t words[2048];
    SIZE_T got = 0;
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    bool ok = false;
    if (::SuspendThread(h) != static_cast<DWORD>(-1)) {
        if (::GetThreadContext(h, &ctx)) {
            ok = true;
            ::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(ctx.Esp)), words,
                                sizeof(words), &got);
            if (got == 0) {  // стек короче 8 КБ до конца региона — читаем постранично
                for (SIZE_T off = 0; off < sizeof(words); off += 4096) {
                    SIZE_T g = 0;
                    if (!::ReadProcessMemory(::GetCurrentProcess(),
                                             reinterpret_cast<LPCVOID>(static_cast<uintptr_t>(ctx.Esp) + off),
                                             reinterpret_cast<char*>(words) + off, 4096, &g) || g == 0)
                        break;
                    got += g;
                }
            }
        }
        ::ResumeThread(h);
    }
    ::CloseHandle(h);
    if (!ok) {
        LOG_WARN("EVBots: КЛИЕНТ ЗАВИС: регистры потока кадра %lu не взять", tid);
        return;
    }
    char where[200];
    DescribeCodeAddr(static_cast<uintptr_t>(ctx.Eip), where, sizeof(where));
    LOG_WARN("EVBots: КЛИЕНТ ЗАВИС: поток %lu EIP=%08lX (%s) ESP=%08lX EBP=%08lX", tid, static_cast<unsigned long>(ctx.Eip),
             where[0] ? where : "вне модулей", static_cast<unsigned long>(ctx.Esp), static_cast<unsigned long>(ctx.Ebp));
    const size_t n = got / sizeof(uintptr_t);
    int hits = 0;
    std::string line;
    for (size_t i = 0; i < n && hits < 28; ++i) {
        DescribeCodeAddr(words[i], where, sizeof(where));
        if (where[0] == '\0') continue;
        char item[240];
        snprintf(item, sizeof(item), "%s[%u]%s", line.empty() ? "" : " ", static_cast<unsigned>(i), where);
        line += item;
        ++hits;
    }
    LOG_WARN("EVBots: КЛИЕНТ ЗАВИС: стек (слово:модуль+смещение): %s", line.empty() ? "адресов модулей нет" : line.c_str());
}

DWORD WINAPI ClientWatchdogThread(LPVOID) {
    unsigned long long reportedFor = 0;  // g_frameEnterMs кадра, по которому уже писали
    int reportsForThis = 0;
    unsigned long long pulseAt = ::GetTickCount64() + 60000;
    unsigned long long framesAtPulse = 0;
    for (;;) {
        ::Sleep(1000);
        if (!g_clientMode.load()) continue;
        const unsigned long long now = ::GetTickCount64();
        const unsigned long long enter = g_frameEnterMs.load();
        const unsigned long long frames = g_clientFrames.load();
        if (frames == 0 || enter == 0) continue;
        const unsigned long long stall = now - enter;
        const int stage = g_frameStage.load();
        if (stall >= 5000) {
            if (reportedFor != enter) {
                reportedFor = enter;
                reportsForThis = 0;
            }
            const unsigned long long marks[] = {5000, 30000, 120000};
            if (reportsForThis < 3 && stall >= marks[reportsForThis]) {
                ++reportsForThis;
                const unsigned long long msgAge = g_lastMsgMs.load() ? now - g_lastMsgMs.load() : 0;
                LOG_WARN("EVBots: КЛИЕНТ ЗАВИС: нового кадра нет %.1f с (стадия: %s); кадров всего %llu; последнее сообщение "
                         "движка: категория %08X тип %08X %.1f с назад",
                         stall / 1000.0,
                         stage == 1   ? "внутри кадра движка"
                         : stage == 2 ? "Lua EVBots/мода после кадра"
                                      : "между кадрами — кадр не вызывается (загрузка/ожидание)",
                         frames,
                         static_cast<unsigned>(g_lastMsgCategory.load()), static_cast<unsigned>(g_lastMsgType.load()),
                         msgAge / 1000.0);
                const unsigned long tid = g_frameThreadId.load();
                if (tid != 0) LogFrameThreadStack(tid);
            }
        } else if (reportsForThis > 0 && reportedFor != enter) {
            LOG_WARN("EVBots: клиент: кадр ожил после остановки (писали %d раз)", reportsForThis);
            reportsForThis = 0;
        }
        if (now >= pulseAt) {
            LOG_INFO("EVBots: клиент: пульс: кадров за 60 с %llu (всего %llu), последнее сообщение движка тип %08X %.0f с назад",
                     frames - framesAtPulse, frames, static_cast<unsigned>(g_lastMsgType.load()),
                     g_lastMsgMs.load() ? (now - g_lastMsgMs.load()) / 1000.0 : -1.0);
            framesAtPulse = frames;
            pulseAt = now + 60000;
        }
    }
}

void StartClientWatchdogOnce() {
    if (g_watchdogStarted.exchange(true)) return;
    HANDLE h = ::CreateThread(nullptr, 0, ClientWatchdogThread, nullptr, 0, nullptr);
    if (h != nullptr) {
        ::CloseHandle(h);
        LOG_INFO("EVBots: клиент: ★ сторож кадра запущен (зависание кадра 5/30/120 с — в журнал со стеком; пульс раз в 60 с)");
    }
}

void __fastcall ClientMsgListenerOnMessage(void* self, void* edx, const EngineMessageView* msg);
unsigned int __fastcall ClientMsgListenerPriority(void* self, void* edx) {
    (void)self;
    (void)edx;
    return 0x64;  // как у слушателя Main
}

void* const kClientMsgListenerVtbl[3] = {reinterpret_cast<void*>(&ClientMsgListenerOnMessage),
                                         reinterpret_cast<void*>(&ClientMsgListenerPriority), nullptr};
struct ClientMsgListener {
    void* const* vtable;
};
ClientMsgListener g_clientMsgListener{kClientMsgListenerVtbl};

bool ReadEngineMessage(const EngineMessageView* msg, uint32_t* category, uint32_t* type) {
    __try {
        *category = msg->category;
        *type = msg->type;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// VU 10C0F710: Connect requires msg+2C, player at +28, id at player+3EC.
// Copy only the id while the message is alive; never retain its native pointer.
bool ReadClientConnectionMessage(const EngineMessageView* msg, bool connected, uint32_t* id) {
    __try {
        const auto* bytes = reinterpret_cast<const unsigned char*>(msg);
        if (connected && bytes[0x2C] == 0) return false;
        const uintptr_t player = *reinterpret_cast<const uintptr_t*>(bytes + 0x28);
        if (!player) return false;
        *id = *reinterpret_cast<const uint32_t*>(player + 0x3EC);
        return *id < 256;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ★ Сборка 234 (реестр В12): аргументы клиентского Level:Loaded(levelName, gameMode) — как VU. Факт (декомпилят
// veniceext 10C15090, ветка ClientLevelLoadedMessage): поля сообщения VU не читает; берёт X = 170D5160() =
// [[[0x022E3790]+0xB4]+0xC] (клиентский контекст, CLIENT-SLOTS ★★★), уровень = [X+0x10]; имя — [уровень+0x68] (char*),
// режим — значение пары с ключом "GameMode" в массиве [уровень+0x6C] (пары char* ключ/значение по 8 байт, число — [массив-4]).
constexpr uintptr_t kMainVarVa = 0x022E3790;

bool SafeCStr(uintptr_t p, std::string* out) {
    __try {
        if (p < 0x10000) return false;
        const char* s = reinterpret_cast<const char*>(p);
        size_t n = 0;
        while (n < 256 && s[n] != 0) ++n;
        if (n == 0 || n == 256) return false;
        out->assign(s, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadU32(uintptr_t a, uintptr_t* v) {
    __try {
        if (a < 0x10000) return false;
        *v = *reinterpret_cast<const uint32_t*>(a);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ClientLevelInfo(std::string* level, std::string* mode) {
    uintptr_t main = 0, client = 0, ctx = 0, lvl = 0, name = 0, arr = 0, count = 0;
    if (!ReadU32(kMainVarVa, &main) || !ReadU32(main + 0xB4, &client) || !ReadU32(client + 0xC, &ctx) ||
        !ReadU32(ctx + 0x10, &lvl) || lvl == 0)
        return false;
    if (ReadU32(lvl + 0x68, &name)) SafeCStr(name, level);
    if (ReadU32(lvl + 0x6C, &arr) && ReadU32(arr - 4, &count) && count <= 64) {
        for (uintptr_t i = 0; i < count; ++i) {
            uintptr_t k = 0, v = 0;
            std::string key;
            if (!ReadU32(arr + i * 8, &k) || !SafeCStr(k, &key) || key != "GameMode") continue;
            if (ReadU32(arr + i * 8 + 4, &v)) SafeCStr(v, mode);
            break;
        }
    }
    return !level->empty();
}

void __fastcall ClientMsgListenerOnMessage(void* self, void* edx, const EngineMessageView* msg) {
    (void)self;
    (void)edx;
    if (!g_msgListenerOn.load() || !g_clientMode.load() || msg == nullptr) return;
    uint32_t category = 0, type = 0;
    if (!ReadEngineMessage(msg, &category, &type)) return;
    // ★ 258: сторож кадра — последнее сообщение движка; сообщения уровня клиента (вход, загрузка, смена карты) — в тихий журнал
    g_lastMsgCategory = category;
    g_lastMsgType = type;
    g_lastMsgMs = ::GetTickCount64();
    if (category == 0xDAA58EAA) {
        LOG_INFO("EVBots: клиент: загрузка: сообщение уровня тип %08X (кадров %llu)", static_cast<unsigned>(type),
                 g_clientFrames.load());
    }
    auto& lua = LuaRuntime::Instance();
    if (!lua.ready()) return;
    // ★ Сборка 217: прогон 216 — Level:Loaded на клиенте не пришёл. Первые 40 сообщений и все ClientLevel — в журнал.
    const unsigned long long nMsg = g_engineMessages.fetch_add(1);
    if (nMsg < 40 || category == 0xDAA58EAA) {
        LOG_INFO("EVBots: клиент: ★ сообщение движка #%llu: категория %08X тип %08X", nMsg + 1,
                 static_cast<unsigned>(category), static_cast<unsigned>(type));
    }
    const double cat = static_cast<double>(static_cast<int32_t>(category));
    const double typ = static_cast<double>(static_cast<int32_t>(type));
    std::lock_guard<std::recursive_mutex> gate(lua.TickGate());
    std::string level, mode;
    const bool connectionMessage = category == 0x24FE118F && (type == 0x69E0643A || type == 0x608997B9);
    const bool levelMessage = type == kMsgClientLevelLoaded || type == kMsgClientLevelFinalized;
    const bool hasLevel = (levelMessage || connectionMessage) && ClientLevelInfo(&level, &mode);
    const bool loadingShell = hasLevel && _stricmp(level.c_str(), "Levels/Web_Loading/Web_Loading") == 0;
    if (connectionMessage && !loadingShell) {
        uint32_t playerId = 0;
        const bool connected = type == 0x69E0643A;
        if (ReadClientConnectionMessage(msg, connected, &playerId)) {
            LOG_INFO("EVBots: client Player:%s native message id=%u", connected ? "Connected" : "Deleted", playerId);
            lua.CallTableFunction("EVBotsClientBridge", "PlayerConnectionMessage",
                                 {LuaRuntime::EngineEventArg::Num(playerId), LuaRuntime::EngineEventArg::Num(connected ? 1 : 0)});
        }
    }
    // Web_Loading is the engine's loading shell, not a playable map. Publishing
    // its completion starts mod gameplay UI/timers before the real map loads.
    // Keep the raw Engine:Message delivery below for observers of native messages.
    if (loadingShell) {
        LOG_INFO("EVBots: клиент: служебный Web_Loading завершён; игровые Level-события не отправлены (тип %08X)",
                 static_cast<unsigned>(type));
    } else if (type == kMsgClientLevelLoaded) {
        // VU-Docs (client\event\level_loaded.md): Level:Loaded(levelName, gameMode). ★ 234: из уровня, как VU (выше).
        if (hasLevel) {
            LOG_INFO("EVBots: клиент: ClientLevelLoadedMessage -> Level:Loaded(%s, %s)", level.c_str(),
                     mode.empty() ? "нет GameMode" : mode.c_str());
            lua.DispatchEngineEvent("Level:Loaded", {LuaRuntime::EngineEventArg::Str(level),
                                                     mode.empty() ? LuaRuntime::EngineEventArg::Nil()
                                                                  : LuaRuntime::EngineEventArg::Str(mode)});
        } else {
            LOG_WARN("EVBots: клиент: ClientLevelLoadedMessage -> Level:Loaded без аргументов (уровень не прочитан)");
            lua.DispatchEngineEvent("Level:Loaded");
        }
    } else if (type == kMsgClientLevelFinalized) {
        LOG_INFO("EVBots: клиент: ★ ClientLevelFinalizedMessage -> Level:Finalized");
        lua.DispatchEngineEvent("Level:Finalized");
    }
    lua.CallTableFunction("EVBotsClientBridge", "EngineMessage",
                          {LuaRuntime::EngineEventArg::Num(cat), LuaRuntime::EngineEventArg::Num(typ)});
}

using MsgSubscribeFn = char(__fastcall*)(void* holder, void* edx, uint32_t category, void* listener);

/// Подписка — из потока кадра движка (как Main::Main — в потоке движка), один раз. false — менеджера ещё нет.
bool SubscribeClientMessages() {
    const char* off = std::getenv("EVBOTS_CLIENT_MESSAGES");
    if (off != nullptr && off[0] == '0') return true;  // выключено флагом — больше не пытаться
    unsigned char code[sizeof(kMsgSubscribeCode)] = {};
    SIZE_T n = 0;
    if (!::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(kMsgSubscribeVa), code, sizeof(code), &n) ||
        n != sizeof(code) || std::memcmp(code, kMsgSubscribeCode, sizeof(code)) != 0) {
        LOG_ERR("EVBots: клиент: код подписки 0x%08X не совпал с zbf3.exe — события движка не подключаем",
                static_cast<unsigned>(kMsgSubscribeVa));
        return true;
    }
    // ★ Сборка 218. Прогон 217 (20:32): приходили только сообщения Core (5 за 2 мин), ни одного Client* — хотя игрок был
    // в игре. Факт (veniceext 10C14D60, дизассемблер): VU подписывает Core на ecx=[0x022ED3B8], а все остальные категории —
    // на ДРУГОЙ менеджер: ecx=edi = [0x02380B58] у клиента (аргумент 0) или [0x0236B608] у сервера VU.
    void* coreHolder = nullptr;
    void* clientHolder = nullptr;
    __try {
        coreHolder = *reinterpret_cast<void**>(kMsgManagerHolderVa);
        clientHolder = *reinterpret_cast<void**>(kClientMsgManagerHolderVa);
        if (coreHolder == nullptr || *reinterpret_cast<void**>(coreHolder) == nullptr || clientHolder == nullptr ||
            *reinterpret_cast<void**>(clientHolder) == nullptr) {
            return false;  // менеджеров ещё нет — попробуем на следующем кадре
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    g_msgListenerOn = true;
    auto subscribe = reinterpret_cast<MsgSubscribeFn>(kMsgSubscribeVa);
    int ok = 0;
    for (uint32_t cat : kClientMsgCategories) {
        subscribe(cat == kMsgCategoryCore ? coreHolder : clientHolder, nullptr, cat, &g_clientMsgListener);
        ++ok;
    }
    // Отписки нет (функцию отписки не разбирали): после подписки движок держит указатель на наш слушатель, поэтому DLL
    // закрепляем в процессе — FreeLibrary не выгрузит её, и vtable слушателя не станет висячей.
    HMODULE self = nullptr;
    const BOOL pinned = ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                             reinterpret_cast<LPCWSTR>(&ClientMsgListenerOnMessage), &self);
    LOG_INFO("EVBots: клиент: ★ слушатель сообщений движка подписан на %d категорий (004A5D90, как MessageManager VU); "
             "DLL закреплена: %s", ok, pinned ? "да" : "НЕТ");
    return true;
}

std::atomic<bool> g_msgSubscribed{false};

/// ★ Сборка 228: вызовы WebUI.Call('DispatchEvent'|'DispatchEventLocal', …) страниц модов → события Lua. «Текущий мод» —
/// мод страницы (DispatchEventLocal у VU — только своему моду). Разбор и рассылка — __webui_on_call в VextClient.cpp.
void DrainWebUiCalls(LuaRuntime& lua) {
    std::vector<WebUiCall> calls;
    WebUiDrain(&calls);
    lua_State* L = lua.state();
    if (calls.empty() || L == nullptr) return;
    for (const WebUiCall& c : calls) {
        LOG_INFO("EVBots: WebUI: событие страницы в Lua: %s", c.json.c_str());
        const std::wstring prev = lua.PushCurrentMod(c.modFolder);
        lua_getglobal(L, "__webui_on_call");
        if (lua_isfunction(L, -1)) {
            lua_pushlstring(L, c.json.data(), c.json.size());
            if (lua_pcall(L, 1, 0, 0) != LUA_OK) {
                LOG_ERR("EVBots: WebUI: событие страницы упало в Lua: %s", lua_tostring(L, -1));
                lua_pop(L, 1);
            }
        } else {
            lua_pop(L, 1);
        }
        lua.PopCurrentMod(prev);
    }
}

/// Ввод кадра и Client:UpdateInput (до UpdateManager). Общий для хука кадра и проверки без игры (ClientFrameForTest).
void ClientInputFrame(LuaRuntime& lua, float dt) {
    WebUiFrameTick();  // ★ Сборка 229: курсор при меню — в потоке игры
    ClientInputPollFrame();
    lua.DispatchEngineEvent("Client:UpdateInput", {LuaRuntime::EngineEventArg::Num(dt)});
}

using ClientFrameFn = char(__fastcall*)(void* loop);

struct FrameStageReset {  // ★ 258: сторож кадра — «между кадрами» на любом выходе из детура
    ~FrameStageReset() { g_frameStage = 0; }
};

char __fastcall ClientFrameDetour(void* loop) {
    g_frameEnterMs = ::GetTickCount64();  // ★ 258: сторож кадра
    g_frameStage = 1;
    if (g_frameThreadId.load() == 0) g_frameThreadId = ::GetCurrentThreadId();
    FrameStageReset stageReset;
    // Native entity callbacks run inside the original frame too. Mark the
    // engine context here, but do not hold LuaGate across the native frame.
    struct BlueprintFrameScope {
        BlueprintFrameScope() { blueprint::EnterClientFrame(); }
        ~BlueprintFrameScope() { blueprint::LeaveClientFrame(); }
    } blueprintScope;
    const char keepRunning = reinterpret_cast<ClientFrameFn>(g_hookClientFrame.Trampoline())(loop);
    g_frameStage = 2;
    auto& lua = LuaRuntime::Instance();
    std::lock_guard<std::recursive_mutex> gate(lua.TickGate());
    TickVextSpectator();
    if (g_clientStopRequested.exchange(false)) StopClientMode();
    if (keepRunning == 0 || !g_clientMode.load()) return keepRunning;
    if (!lua.ready()) return keepRunning;
    double dtRaw = 0.0, simRaw = 0.0;
    if (!ReadClientFrameDt(loop, &dtRaw, &simRaw)) return keepRunning;
    const float dt = ClampFrameDt(dtRaw);
    const float simDt = ClampFrameDt(simRaw);
    if (!g_msgSubscribed.load() && SubscribeClientMessages()) g_msgSubscribed = true;
    if (g_clientFrames.fetch_add(1) == 0) {
        LOG_INFO("EVBots: клиент: ★ первый кадр движка (хук 0x%08X): dt=%.4f simDt=%.4f, поток %lu",
                 static_cast<unsigned>(kClientFrameVa), dt, simDt, ::GetCurrentThreadId());
        // Native TypeInfo is not initialized when the injection worker starts Lua.
        // Install the resource hooks on the engine thread after its first frame,
        // before Web_Loading and the multiplayer level begin loading resources.
    }
    if (g_clientStartupPending.load()) {
        // The first tick can precede renderer/settings initialization. VU's
        // GetSettings is a lookup too; don't deliver startup while it is nil.
        if (eng::FindSettings("DebugRenderSettings") == 0) {
            static bool waitingLogged = false;
            if (!waitingLogged) {
                waitingLogged = true;
                LOG_INFO("EVBots: client startup waiting for renderer settings");
            }
            return keepRunning;
        }
        g_clientStartupPending = false;
        if (!VerifyNativeSquadSizeClient()) {
            LOG_ERR("LUA-MODE FAILED (client): native squad-size constants differ");
            StopClientMode();
            return keepRunning;
        }
        // SettingsManager/TypeInfo do not exist on the injection worker yet.
        // Extension:Loaded handlers must see native settings before updates.
        if (!lua.DispatchEngineEvent("Engine:Init") || !lua.DispatchEngineEvent("Extension:Loaded")) {
            LOG_ERR("LUA-MODE FAILED (client): engine-frame startup event failed");
            StopClientMode();
            return keepRunning;
        }
        ResourceClientStart();
    }
    EvNetDrain();  // ★ Сборка 219 (К4): NetEvents с сервера — до Engine:Update
    DrainWebUiCalls(lua);  // ★ Сборка 228: события страниц WebUI — тоже до Engine:Update
    ResourceDrainDeferred();         // ★ Сборка 234 (В8): load-хендлеры, не успевшие взять лок Lua
    ResourcePartitionEventsDrain();  // ★ Сборка 234 (В8): Partition:Loaded
    lua.DispatchEngineEvent("Engine:Update",
                            {LuaRuntime::EngineEventArg::Num(dt), LuaRuntime::EngineEventArg::Num(simDt)});
    // ★ Сборка 234 (В2): хук Input:PreUpdate — до Client:UpdateInput (VextClient.cpp, Bridge.InputPreUpdate)
    lua.CallTableFunction("EVBotsClientBridge", "InputPreUpdate", {LuaRuntime::EngineEventArg::Num(dt)});
    // ★ Сборка 228: Client:UpdateInput(deltaTime) — ввод кадра для InputManager (VU-Docs client\event; реестр В17)
    ClientInputFrame(lua, dt);
    // UpdateManager:Update: у VU — каждая фаза кадра движка; источник фаз в VU не найден (не vu-core, хэша события
    // в veniceext нет). Пока — из того же кадра, фазы по порядку движка, реестр VU-DIVERGENCES В1а.
    lua.DispatchEngineEvent("UpdateManager:Update",
                            {LuaRuntime::EngineEventArg::Num(simDt), LuaRuntime::EngineEventArg::Num(0)});  // PreSim
    lua.DispatchEngineEvent("UpdateManager:Update",
                            {LuaRuntime::EngineEventArg::Num(simDt), LuaRuntime::EngineEventArg::Num(1)});  // PostSim
    lua.DispatchEngineEvent("UpdateManager:Update",
                            {LuaRuntime::EngineEventArg::Num(dt), LuaRuntime::EngineEventArg::Num(5)});  // PreFrame
    lua.DispatchEngineEvent("UpdateManager:Update",
                            {LuaRuntime::EngineEventArg::Num(dt), LuaRuntime::EngineEventArg::Num(2)});  // PostFrame
    // ★ Сборка 234 (В6): UI:DrawHud — место, где моды рисуют DebugRenderer (fun-bots: ники, редактор путей). У VU — из
    // отрисовки HUD (место не разобрано: хуки vu-core на отладочный рендер 004AD920…004FCD00); у нас — раз в кадр движка,
    // после фаз кадра. Команды копятся в отладочном рендере движка и рисуются его отрисовкой (00FB97A0). Реестр В6.
    lua.DispatchEngineEvent("UI:DrawHud");
    return keepRunning;
}

// ── ★ Сборка 234 (реестр В2): хук UI:PushScreen(screen, priority, parentGraph, stateNodeGuid) ──
// Факт (Ghidra клиента, 26.10): UI-менеджер (vtable 020ACC8C) метод 47 — PopScreen 00FF8D50 (`mov ecx,[ecx+0x28]` →
// 0101BAE0, разбор 23.10), метод 48 — 00FF8D70 `mov ecx,[ecx+0x28]; jmp 010246D0`. 010246D0 — запрос экрана:
// thiscall(ecx = UI-граф, const Guid* узел, DataContainer* экран ([+8] имя, [+0xC] узлы), u32 приоритет,
// DataContainer* родительский граф, a5) -> bool, ret 0x14 (a5 — назначение не подтверждено: читается и байтом
// (→ [запись+0x54]), и словом; передаём как есть); кладёт экран в очередь [this+0xAC..+0xC8]
// (010176E0, та же, что у PopScreen) или сразу 01023380 (строка «instance1.pushScreen»). Аргументы VU-Docs
// (client\hook\ui_pushscreen.md): screen = экран, priority, parentGraph = родитель, stateNodeGuid = узел.
// Lua — только если лок Lua свободен (try_lock): иначе оригинал без хука (журнал раз).
constexpr uintptr_t kPushScreenVa = 0x010246D0;
constexpr unsigned char kPushScreenPrologue[9] = {0x83, 0xEC, 0x44, 0x53, 0x55, 0x8B, 0xE9, 0x33, 0xDB};
using PushScreenFn = bool(__fastcall*)(void* self, void* edx, const unsigned char* guid, unsigned long screen,
                                       unsigned long priority, unsigned long parent, unsigned long a5);
InlineHook g_hookPushScreen;
thread_local int t_pushScreenDepth = 0;  // обработчик мода сам открыл экран — вложенный вызов без Lua
std::atomic<unsigned long> g_pushScreenCalls{0};
std::atomic<bool> g_pushScreenBusyLogged{false};

bool ReadGuid16(const unsigned char* p, eng::Guid16* out) {
    __try {
        std::memcpy(out->b, p, 16);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/// Цепочка обработчиков мода (EVBotsClientBridge.UIPushScreen). true — звать оригинал; false — Return (значение в rv).
bool PushScreenLua(lua_State* L, const unsigned char* guid, unsigned long screen, unsigned long priority,
                   unsigned long parent, bool* rv) {
    const int top = lua_gettop(L);
    bool callOriginal = true;
    lua_getglobal(L, "EVBotsClientBridge");
    if (lua_istable(L, -1)) {
        lua_getfield(L, -1, "UIPushScreen");
        if (lua_isfunction(L, -1)) {
            lua_pushinteger(L, static_cast<lua_Integer>(screen));
            lua_pushinteger(L, static_cast<lua_Integer>(priority));
            lua_pushinteger(L, static_cast<lua_Integer>(parent));
            eng::Guid16 g;
            char text[40] = {};
            if (guid != nullptr && ReadGuid16(guid, &g)) {
                eng::FormatGuid(g, text);
                lua_pushstring(L, text);
            } else {
                lua_pushnil(L);
            }
            if (lua_pcall(L, 4, 2, 0) == LUA_OK) {
                callOriginal = lua_toboolean(L, -2) != 0;
                *rv = lua_toboolean(L, -1) != 0;
            } else {
                const char* err = lua_tostring(L, -1);
                LOG_WARN("EVBots: клиент: UI:PushScreen: %s", err != nullptr ? err : "?");
            }
        }
    }
    lua_settop(L, top);
    return callOriginal;
}

bool __fastcall PushScreenDetour(void* self, void* edx, const unsigned char* guid, unsigned long screen,
                                 unsigned long priority, unsigned long parent, unsigned long a5) {
    const auto orig = reinterpret_cast<PushScreenFn>(g_hookPushScreen.Trampoline());
    if (g_pushScreenCalls.fetch_add(1) == 0) {
        LOG_INFO("EVBots: клиент: ★ первый PushScreen движка (экран %p, приоритет %lu), поток %lu",
                 reinterpret_cast<void*>(screen), priority, ::GetCurrentThreadId());
    }
    auto& lua = LuaRuntime::Instance();
    if (g_clientMode.load() && lua.ready() && screen != 0 && t_pushScreenDepth == 0) {
        std::unique_lock<std::recursive_mutex> gate(lua.TickGate(), std::try_to_lock);
        if (gate.owns_lock() && lua.state() != nullptr) {
            bool rv = false;
            ++t_pushScreenDepth;
            const bool callOriginal = PushScreenLua(lua.state(), guid, screen, priority, parent, &rv);
            --t_pushScreenDepth;
            if (!callOriginal) return rv;
        } else if (!g_pushScreenBusyLogged.exchange(true)) {
            LOG_WARN("EVBots: клиент: UI:PushScreen — Lua занята другим потоком, экран без хуков мода (поток %lu)",
                     ::GetCurrentThreadId());
        }
    }
    return orig(self, edx, guid, screen, priority, parent, a5);
}

/// Всё или ничего — пролог по zbf3.exe. EVBOTS_UI_PUSHSCREEN=0 — без хука.
void InstallPushScreenHook() {
    const char* off = std::getenv("EVBOTS_UI_PUSHSCREEN");
    if (off != nullptr && off[0] == '0') {
        LOG_WARN("EVBots: клиент: EVBOTS_UI_PUSHSCREEN=0 — хук UI:PushScreen не ставим");
        return;
    }
    unsigned char got[sizeof(kPushScreenPrologue)] = {};
    SIZE_T n = 0;
    if (!::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(kPushScreenVa), got, sizeof(got), &n) ||
        n != sizeof(got) || std::memcmp(got, kPushScreenPrologue, sizeof(got)) != 0) {
        LOG_ERR("EVBots: клиент: пролог PushScreen 0x%08X не совпал с zbf3.exe — хук UI:PushScreen НЕ ставим",
                static_cast<unsigned>(kPushScreenVa));
        return;
    }
    const HookResult r = g_hookPushScreen.Install(reinterpret_cast<void*>(kPushScreenVa),
                                                  reinterpret_cast<void*>(&PushScreenDetour), "ModdingInterface:g_hookPushScreen");
    if (r == HookResult::Ok) {
        LOG_INFO("EVBots: клиент: ★ хук UI:PushScreen 0x%08X установлен (украдено %zu б)",
                 static_cast<unsigned>(kPushScreenVa), g_hookPushScreen.StolenBytes());
    } else {
        LOG_ERR("EVBots: клиент: хук UI:PushScreen 0x%08X не установлен: %s", static_cast<unsigned>(kPushScreenVa),
                ToString(r));
    }
}

// ★ 239 (прогоны 28.10, 60 Гц: падение клиента при загрузке уровня в 2 из 4 запусков; 238-5 — то же): обработчик сетевого
// сообщения о игроке (тип 0xB3D832EE, диспетчер 0x00835211 → хвостовой переход) FUN_00833120 копирует 0x58 байт в запись
// игрока в буфере мира: ECX = [0x022B63B0] + [игрок+0x1920] → FUN_00813860 (запись в this+0x28). Глобал 0x022B63B0
// регистрируется статически (0x01EB79E0 → FUN_00a94ce0) и заполняется при создании мира; сообщение пришло раньше →
// AV запись 0x6B8 в bf3.exe+0x413870, через 12 с — AV чтение 0x1054 в +0x71FF1C (глобал 0x02373420 пуст) и выход.
// Защита: пока буфера мира нет — сообщение пропускаем (записи игрока ещё некуда лечь; следующие сообщения обновят).
// Пролог — по zbf3.exe (thiscall, 1 аргумент, ret 4), всё или ничего. EVBOTS_CLIENT_LOADGUARD=0 — без защиты.
// Отличие от VU — реестр Б19.
constexpr uintptr_t kPlayerDataMsgVa = 0x00833120;
constexpr uintptr_t kWorldDataGlobalVa = 0x022B63B0;
const unsigned char kPlayerDataMsgPrologue[] = {0x83, 0xEC, 0x10, 0x56, 0x57, 0x8B, 0x7C, 0x24, 0x1C};
InlineHook g_hookPlayerDataMsg;
std::atomic<long> g_playerDataMsgSkipped{0};
using PlayerDataMsgFn = void(__fastcall*)(void*, void*, void*);

std::atomic<long> g_playerDataMsgCalls{0};

// ★ 239 (прогон 09:25, факт): [0x022B63B0] — не указатель, а СМЕЩЕНИЕ записи (0x690); пустой была база данных игрока
// [игрок+0x1920] (FUN_00a20400), игрок — [сообщение+0x28] (FUN_016e8d10). Блок данных удалённого игрока ещё не выделен,
// а сообщение о нём уже пришло → запись в 0x6B8 (FUN_00813860), затем в 0x28 (FUN_011b83f0 из FUN_00a95060).
uintptr_t PlayerDataBaseSeh(const void* msg) {
    __try {
        const uintptr_t player = *reinterpret_cast<const uintptr_t*>(static_cast<const unsigned char*>(msg) + 0x28);
        if (player == 0) return 1;  // игрока нет — пусть решает сам движок
        return *reinterpret_cast<const uintptr_t*>(player + 0x1920);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

void __fastcall PlayerDataMsgDetour(void* self, void* edx, void* msg) {
    const uintptr_t base = msg != nullptr ? PlayerDataBaseSeh(msg) : 1;
    if (g_playerDataMsgCalls.fetch_add(1) < 4)
        LOG_INFO("EVBots: клиент: сообщение о игроке — база данных игрока %p, поток %lu", reinterpret_cast<void*>(base),
                 ::GetCurrentThreadId());
    if (base == 0) {
        const long n = ++g_playerDataMsgSkipped;
        if (n <= 3 || n % 200 == 0)
            LOG_WARN("EVBots: клиент: ★ сообщение о игроке до выделения его данных ([игрок+0x1920] пуст) — пропущено, "
                     "всего %ld (было падение bf3.exe+0x413870)", n);
        return;
    }
    reinterpret_cast<PlayerDataMsgFn>(g_hookPlayerDataMsg.Trampoline())(self, edx, msg);
}

// ★ 239 (прогон 09:21: защита на входе FUN_00833120 стояла, а падение то же — к записи глобал уже пуст): вторая защита в
// месте записи. FUN_00813860 thiscall(this, src): копирует 0x58 байт в this+0x28; this = [0x022B63B0] + смещение игрока —
// при пустом глобале это само смещение (0x690). this < 0x10000 — неверный указатель: копию пропускаем. Пролог
// 56 / 8B 74 24 08 / 57 / 8B C1 — 8 байт без относительных инструкций (по zbf3.exe). Вызывают 0x00833163 и 0x00831955.
constexpr uintptr_t kPlayerDataCopyVa = 0x00813860;
const unsigned char kPlayerDataCopyPrologue[] = {0x56, 0x8B, 0x74, 0x24, 0x08, 0x57, 0x8B, 0xC1, 0x8D, 0x78, 0x28};
InlineHook g_hookPlayerDataCopy;
std::atomic<long> g_playerDataCopySkipped{0};
using PlayerDataCopyFn = void(__fastcall*)(void*, void*, const void*);

void __fastcall PlayerDataCopyDetour(void* self, void* edx, const void* src) {
    if (reinterpret_cast<uintptr_t>(self) < 0x10000) {
        const long n = ++g_playerDataCopySkipped;
        if (n <= 3 || n % 200 == 0)
            LOG_WARN("EVBots: клиент: ★ запись данных игрока в %p (мир %p) — пропущена, всего %ld (было падение "
                     "bf3.exe+0x413870)", self, *reinterpret_cast<void* volatile*>(kWorldDataGlobalVa), n);
        return;
    }
    reinterpret_cast<PlayerDataCopyFn>(g_hookPlayerDataCopy.Trampoline())(self, edx, src);
}

void InstallPlayerDataCopyGuard() {
    unsigned char got[sizeof(kPlayerDataCopyPrologue)] = {};
    SIZE_T n = 0;
    if (!::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(kPlayerDataCopyVa), got, sizeof(got), &n) ||
        n != sizeof(got) || std::memcmp(got, kPlayerDataCopyPrologue, sizeof(got)) != 0) {
        LOG_ERR("EVBots: клиент: пролог 0x%08X не совпал с zbf3.exe — защиту записи данных игрока НЕ ставим",
                static_cast<unsigned>(kPlayerDataCopyVa));
        return;
    }
    const HookResult r = g_hookPlayerDataCopy.Install(reinterpret_cast<void*>(kPlayerDataCopyVa),
                                                      reinterpret_cast<void*>(&PlayerDataCopyDetour),
                                                      "ModdingInterface:g_hookPlayerDataCopy");
    if (r == HookResult::Ok) {
        LOG_INFO("EVBots: клиент: ★ защита загрузки: запись данных игрока 0x%08X проверяет указатель (украдено %zu б)",
                 static_cast<unsigned>(kPlayerDataCopyVa), g_hookPlayerDataCopy.StolenBytes());
    } else {
        LOG_ERR("EVBots: клиент: защита записи данных игрока 0x%08X не установлена: %s",
                static_cast<unsigned>(kPlayerDataCopyVa), ToString(r));
    }
}

void InstallPlayerDataMsgGuard() {
    const char* off = std::getenv("EVBOTS_CLIENT_LOADGUARD");
    if (off != nullptr && off[0] == '0') {
        LOG_WARN("EVBots: клиент: EVBOTS_CLIENT_LOADGUARD=0 — защита сообщения о игроке при загрузке не ставится");
        return;
    }
    unsigned char got[sizeof(kPlayerDataMsgPrologue)] = {};
    SIZE_T n = 0;
    if (!::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(kPlayerDataMsgVa), got, sizeof(got), &n) ||
        n != sizeof(got) || std::memcmp(got, kPlayerDataMsgPrologue, sizeof(got)) != 0) {
        LOG_ERR("EVBots: клиент: пролог 0x%08X не совпал с zbf3.exe — защиту сообщения о игроке НЕ ставим",
                static_cast<unsigned>(kPlayerDataMsgVa));
        return;
    }
    const HookResult r = g_hookPlayerDataMsg.Install(reinterpret_cast<void*>(kPlayerDataMsgVa),
                                                     reinterpret_cast<void*>(&PlayerDataMsgDetour),
                                                     "ModdingInterface:g_hookPlayerDataMsg");
    if (r == HookResult::Ok) {
        LOG_INFO("EVBots: клиент: ★ защита загрузки: сообщение о игроке 0x%08X ждёт буфер мира (украдено %zu б)",
                 static_cast<unsigned>(kPlayerDataMsgVa), g_hookPlayerDataMsg.StolenBytes());
        if (!g_hookPlayerDataCopy.IsInstalled()) InstallPlayerDataCopyGuard();
    } else {
        LOG_ERR("EVBots: клиент: защита сообщения о игроке 0x%08X не установлена: %s",
                static_cast<unsigned>(kPlayerDataMsgVa), ToString(r));
    }
}

/// Хук кадра клиента: всё или ничего — пролог должен совпасть байт в байт с zbf3.exe. EVBOTS_CLIENT_TICK=0 — без хука.
void InstallClientFrameHook() {
    if (g_hookClientFrame.IsInstalled()) return;
    const char* off = std::getenv("EVBOTS_CLIENT_TICK");
    if (off != nullptr && off[0] == '0') {
        LOG_WARN("EVBots: клиент: EVBOTS_CLIENT_TICK=0 — хук кадра не ставим, тика клиента нет");
        return;
    }
    unsigned char got[sizeof(kClientFramePrologue)] = {};
    SIZE_T n = 0;
    if (!::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(kClientFrameVa), got, sizeof(got), &n) ||
        n != sizeof(got) || std::memcmp(got, kClientFramePrologue, sizeof(got)) != 0) {
        LOG_ERR("EVBots: клиент: пролог 0x%08X не совпал с zbf3.exe (%02X %02X %02X %02X) — хук кадра НЕ ставим",
                static_cast<unsigned>(kClientFrameVa), got[0], got[1], got[2], got[3]);
        return;
    }
    const HookResult r = g_hookClientFrame.Install(reinterpret_cast<void*>(kClientFrameVa),
                                                   reinterpret_cast<void*>(&ClientFrameDetour), "ModdingInterface:g_hookClientFrame");
    if (r == HookResult::Ok) {
        // Реальный клиент держит хук кадра до конца процесса, в том числе для
        // завершения камеры после остановки Lua. FreeLibrary не должен выгрузить детур.
        HMODULE pinned = nullptr;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                  reinterpret_cast<LPCWSTR>(&ClientFrameDetour), &pinned)) {
            LOG_ERR("EVBots: клиент: не удалось закрепить DLL для хука кадра: %lu", ::GetLastError());
            g_hookClientFrame.Remove();
            return;
        }
        LOG_INFO("EVBots: клиент: ★ хук кадра 0x%08X установлен (украдено %zu б) — Engine:Update/UpdateManager:Update из движка",
                 static_cast<unsigned>(kClientFrameVa), g_hookClientFrame.StolenBytes());
    } else {
        LOG_ERR("EVBots: клиент: хук кадра 0x%08X не установлен: %s", static_cast<unsigned>(kClientFrameVa),
                ToString(r));
    }
}

// ★ Прогон 23.10 19:59 (215): клиент сказал «неверная лицензия»; в журнале активации — `.idata` не связан за 60 с (в удачном
// входе 14:03 защита awc связывает импорт через ~4,5 с после старта, уже ПОСЛЕ расшифровки кода). Мы ставили хук кадра через
// 0,2 с после расшифровки — до того, как awc закончил свою работу. Гипотеза: правка кода до конца работы awc ломает его
// проверку. Поэтому ждём того же признака, что и activation («контроль»): слот импорта KERNEL32!GetProcAddress в .idata
// bf3.exe (rva 0x1FBD4BC) связан — равен адресу GetProcAddress. Ожидание события защиты, не таймер.
constexpr uintptr_t kAwcBoundSlotVa = 0x00400000 + 0x01FBD4BC;

bool ClientImportsBound() {
    static const uintptr_t expected = reinterpret_cast<uintptr_t>(
        ::GetProcAddress(::GetModuleHandleA("kernel32.dll"), "GetProcAddress"));
    uintptr_t v = 0;
    SIZE_T got = 0;
    if (!::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(kAwcBoundSlotVa), &v, sizeof(v), &got) ||
        got != sizeof(v)) {
        return false;
    }
    return expected != 0 && v == expected;
}

DWORD WINAPI ClientAutostartThread(LPVOID) {
    const auto t0 = std::chrono::steady_clock::now();
    int waitedMs = 0;
    while (!ClientCodeDecrypted()) {
        if (waitedMs >= 120000) {
            LOG_ERR("EVBots: клиент: код bf3.exe не расшифрован за 120 с (проба 0x%08X) — клиентский Lua не запускаем",
                    static_cast<unsigned>(kDecryptProbeVa));
            return 0;
        }
        ::Sleep(100);
        waitedMs += 100;
    }
    const long long ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    LOG_INFO("EVBots: клиент: ★ код bf3.exe расшифрован (проба 0x%08X) через %lld мс после старта потока",
             static_cast<unsigned>(kDecryptProbeVa), ms);
    int boundMs = 0;
    while (!ClientImportsBound()) {
        if (boundMs >= 120000) {
            LOG_ERR("EVBots: клиент: защита не связала импорт bf3.exe за 120 с (слот 0x%08X) — клиентский Lua и хуки "
                    "не запускаем, чтобы не мешать проверке лицензии", static_cast<unsigned>(kAwcBoundSlotVa));
            return 0;
        }
        ::Sleep(50);
        boundMs += 50;
    }
    LOG_INFO("EVBots: клиент: ★ импорт bf3.exe связан защитой (слот 0x%08X) через %d мс после расшифровки — запускаем Lua",
             static_cast<unsigned>(kAwcBoundSlotVa), boundMs);
    StartClientMode(ClientModsRoot());
    return 0;
}

}  // namespace

bool StartClientMode(const std::wstring& modsRoot) {
    if (g_clientStopRequested.load()) return false;
    if (g_clientMode.load()) return true;
    if (EngineClientPresent() && !InstallTeamCapacity(false)) {
        LOG_ERR("LUA-MODE FAILED (client): TeamInfo expansion failed; mods are not started");
        return false;
    }
    auto& lua = LuaRuntime::Instance();
    if (!lua.Initialize(modsRoot, LuaContext::Client)) {
        LOG_ERR("EVBots: клиент: Lua не поднялся");
        return false;
    }
    if (modsRoot.empty()) {
        LOG_INFO("EVBots: клиент: моды не заданы (EVBOTS_CLIENT_MODS нет) — раздача ext\\client с сервера будет в К4");
    }
    const int found = lua.LoadMods();
    if (found < 0) {
        LOG_ERR("LUA-MODE FAILED (client): %s", lua.lastError().c_str());
        lua.Shutdown();
        g_clientMode = false;
        return false;
    }
    if (found > 0 && lua.runInitScripts() < 0) {
        LOG_ERR("LUA-MODE FAILED (client): ошибка инициализации мода; Lua остановлена");
        lua.Shutdown();
        g_clientMode = false;
        return false;
    }
    {
        // сборщик мусора как у VU (как на сервере, сборка 192)
        std::lock_guard<std::recursive_mutex> gate(lua.TickGate());
        if (lua.state() != nullptr) {
            lua_gc(lua.state(), LUA_GCINC, 105, 200, 16);
            lua_gc(lua.state(), LUA_GCCOLLECT);
        }
    }
    // ★ Сборка 228: WebUI клиента (оверлей над окном игры) — до Extension:Loaded: моды зовут WebUI:Init() в нём
    // (fun-bots UIViews:OnExtensionLoaded). В тест-хосте движка нет — не поднимаем. EVBOTS_WEBUI=0 — выкл.
    if (EngineClientPresent() || WebUiTestMode()) WebUiStart();
    // Порядок как на сервере (и у VU): моды загружены -> Engine:Init -> Extension:Loaded.
    const bool deferStartup = EngineClientPresent();
    if (!deferStartup && (!lua.DispatchEngineEvent("Engine:Init") || !lua.DispatchEngineEvent("Extension:Loaded"))) {
        LOG_ERR("LUA-MODE FAILED (client): ошибка события запуска мода; Lua остановлена");
        WebUiStop();
        lua.Shutdown();
        g_clientMode = false;
        return false;
    }
    g_clientStartupPending = deferStartup;
    g_clientMode = true;
    std::string names;
    for (const auto& m : lua.mods()) {
        if (!names.empty()) names += ", ";
        names += m.name + " " + m.version;
    }
    // Тик — из кадра движка (как у VU), не таймер. Уровня и игроков у клиента пока нет (К5 шаг 3–4).
    // В тест-хосте нет bf3.exe — хук не ставим.
    if (EngineClientPresent()) {
        InstallClientFrameHook();
        if (g_hookClientFrame.IsInstalled()) StartClientWatchdogOnce();  // ★ 258: сторож кадра
        if (!g_hookPushScreen.IsInstalled()) InstallPushScreenHook();  // ★ Сборка 234 (В2): UI:PushScreen
        if (!g_hookPlayerDataMsg.IsInstalled()) InstallPlayerDataMsgGuard();  // ★ 239: падение при загрузке (60 Гц)
        EvNetBridgeStart(true);  // ★ Сборка 219 (К4): канал к серверу через ev-core (если загружен)
    }
    if (g_msgSubscribed.load()) g_msgListenerOn = true;  // повторный старт: подписка в движке уже есть, просто включаем
    LOG_INFO("LUA-MODE READY (client): %zu mod(s) loaded [%s]; тик: %s; уровня и игроков клиента пока нет (К5)",
             lua.mods().size(), names.c_str(),
             g_hookClientFrame.IsInstalled() ? "хук кадра движка" : "нет");
    return true;
}

void StopClientMode() {
    if (EngineClientPresent() && g_hookClientFrame.IsInstalled() &&
        ::GetCurrentThreadId() != g_frameThreadId.load()) {
        g_clientStopRequested = true;
        return;
    }
    if (!g_clientMode.exchange(false)) return;
    g_clientStartupPending = false;
    auto& lua = LuaRuntime::Instance();
    std::lock_guard<std::recursive_mutex> gate(lua.TickGate());
    g_msgListenerOn = false;  // отписки нет (функцию отписки не разбирали): слушатель просто молчит
    WebUiStop();
    if (!EngineClientPresent() && g_hookClientFrame.IsInstalled()) g_hookClientFrame.Remove();
    LuaRuntime::Instance().DispatchEngineEvent("Extension:Unloading", {});
    StopVextSpectator();
    LuaRuntime::Instance().Shutdown();
    LOG_INFO("EVBots: клиент: Lua закрыт");
}

bool ClientModeActive() { return g_clientMode.load(); }

void ClientFrameForTest(float dt) {
    if (!g_clientMode.load() || !WebUiTestMode()) return;
    auto& lua = LuaRuntime::Instance();
    if (!lua.ready()) return;
    std::lock_guard<std::recursive_mutex> gate(lua.TickGate());
    TickVextSpectator();
    EvNetDrain();
    DrainWebUiCalls(lua);
    lua.DispatchEngineEvent("Engine:Update", {LuaRuntime::EngineEventArg::Num(dt), LuaRuntime::EngineEventArg::Num(dt)});
    ClientInputFrame(lua, dt);
}

ModdingInterface* GetInstance() {
    return Instance();
}

}  // namespace evbots

// ── единственный экспорт ───────────────────────────────────────────────────

extern "C" __declspec(dllexport) void* __cdecl GetModdingInterface() {
    // Возвращаем указатель на объект. У veniceext он 4 байта (одна ссылка
    // на vtable) — у нас объект с тем же интерфейсом, поэтому размер может
    // отличаться, а контракт (5 методов, без параметров) тот же.
    return static_cast<void*>(evbots::GetInstance());
}

// ★ Сборка 237: неверный параметр CRT (sprintf_s/strcpy_s без места и т.п.) по умолчанию завершает процесс мгновенно
// (fail-fast) — без VEH, без дампа Frostbite и Windows (WER выключен). Так беззвучно падал сервер 236-2 и 237 (строка
// журнала посадки в char[48]). Свой обработчик: строка в журнал, функция CRT возвращает ошибку, процесс живёт.
// Обработчик — у статической CRT этой DLL (/MT), чужие модули не затрагивает.
static void __cdecl CrtInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    static volatile LONG s_n = 0;
    if (InterlockedIncrement(&s_n) <= 50) {
        LOG_ERR("EVBots: CRT: неверный параметр (переполнение буфера sprintf_s/strcpy_s?) — вызов вернул ошибку, "
                "процесс не завершён; адрес возврата %p", _ReturnAddress());
    }
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            evbots::gModule = instance;
            DisableThreadLibraryCalls(instance);
            _set_invalid_parameter_handler(&CrtInvalidParameter);
            evbots::LogInit();
            LOG_INFO("EVBots: loaded into process, PID %lu",
                     GetCurrentProcessId());
            if (evbots::EngineServerPresent()) {
                LOG_INFO("EVBots: BF3 server detected - auto-start mod system");
                ::CreateThread(nullptr, 0, evbots::AutostartThread, nullptr, 0, nullptr);
            } else if (evbots::EngineClientPresent()) {
                // ★ Сборка 215 (К3): клиент. Загрузчик внедряет нас в подвешенный bf3.exe до расшифровки кода —
                // поток ждёт расшифровки и поднимает клиентский Lua. Серверных хуков в клиенте нет.
                LOG_INFO("EVBots: BF3 client (bf3.exe) detected - client mode, waiting for code decryption");
                ::CreateThread(nullptr, 0, evbots::ClientAutostartThread, nullptr, 0, nullptr);
            }
            break;

        case DLL_PROCESS_DETACH:
            if (reserved == nullptr) {
                // штатная выгрузка: снимаем хуки, иначе процесс упадёт
                if (evbots::ClientModeActive()) {
                    evbots::StopClientMode();
                } else {
                    evbots::GetInstance()->Shutdown();
                }
                // Lua restart сохраняет боевые хуки; выгрузка самой DLL — нет.
                evbots::HookRegistry::Instance().RemoveAll();
            }

            LOG_INFO("EVBots: unloaded");
            evbots::LogClose();
            break;

        default:
            break;
    }

    return TRUE;
}
