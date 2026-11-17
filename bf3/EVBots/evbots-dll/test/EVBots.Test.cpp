// EVBots.Test.cpp — 32-битный тест-хост для EVBots.dll.
//
// Цель: доказать, что DLL собирается, отдаёт контракт из 5 методов, ставит
// и снимает хуки, и что трамплин корректно возвращает управление в
// оригинал. Всё это проверяется БЕЗ запуска BF3: хуки ставятся на функции
// самого теста.
//
// Запуск: EVBots.Test.exe

#include <Windows.h>
#include <DbgHelp.h>
#pragma comment(lib, "Dbghelp.lib")

#include <cstdint>
#include <cstdio>
#include <cstring>
#include "../src/SpawnPolicy.h"
#include "../src/RushPolicy.h"
#include "../src/LevelPolicy.h"

namespace evbots {
struct EngineNatives;
}  // namespace evbots

// Импортируем из DLL через динамическую загрузку: это же проверяет, что
// экспорт ровно один и имеет нужную сигнатуру.
namespace {

int g_pass = 0;
int g_fail = 0;

void Check(bool ok, const char* what) {
    if (ok) {
        ++g_pass;
        printf("  [ OK ] %s\n", what);
    } else {
        ++g_fail;
        printf("  [FAIL] %s\n", what);
    }
}

void Section(const char* name) {
    printf("\n--- %s ---\n", name);
}

// ── ловушка падения ───────────────────────────────────────────────────────
//
// Если что-то рухнет, печатаем адрес и модуль: по нему видно, это наша DLL,
// сам тест или совсем чужой модуль (тогда причина — адрес движка).

LONG WINAPI CrashProbe(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    void* where = info->ExceptionRecord->ExceptionAddress;
    // Только тест-хост: снимок для разбора редких ошибок завершения вне игры.
    HANDLE dump = CreateFileW(L"EVBots.Test-crash.dmp", GENERIC_WRITE, FILE_SHARE_READ,
                             nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION exception{GetCurrentThreadId(), info, FALSE};
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump, MiniDumpNormal,
                         &exception, nullptr, nullptr);
        CloseHandle(dump);
    }

    HMODULE mod = nullptr;
    char modName[MAX_PATH] = "<неизвестно>";
    if (VirtualQuery(where, nullptr, 0) == 0) {
        // VirtualQuery с nullptr не работает — спросим узнать модуль иначе
    }
    __try {
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(where), &mod);
        GetModuleFileNameA(mod, modName, MAX_PATH);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        modName[0] = '?';
        modName[1] = 0;
    }

    printf("\n*** ПАДЕНИЕ: код 0x%08lX по адресу %p\n", code, where);
    printf("*** Модуль: %s\n", modName);
    printf("*** Это %s\n",
           strstr(modName, "evbots")   ? "наша DLL"
           : strstr(modName, "EVBots") ? "наш тест"
                                           : "другой модуль; причина определяется по стеку");
    return EXCEPTION_EXECUTE_HANDLER;
}

// ── функции-цели, которые будем перехватывать ────────────────────────────
//
// Ключевой момент: вызовы идут ЧЕРЕЗ указатели на volatile, иначе компилятор
// свернёт TargetAdd(3, 4) в константу 7 во время сборки, функция не будет
// вызвана вовсе, и перехват невозможно заметить. Это была первая ложь
// самопроверки: счётчики оставались нулевыми при вроде бы установленном хуке.
//
// Пролог у TargetMul начинается с imul (3 байта) — кража ровно 5 байт
// разрезала бы следующий mov. Это проверка, что кража идёт по границам
// инструкций.

int __fastcall TargetAdd(int a, int b) {
    volatile int local = a + b;
    local += 0;
    return local;
}

int __fastcall TargetMul(int a, int b) {
    int r = a * b;
    r += 0;
    return r;
}

// ★ 36-я: сигнатуры damage/chat соответствуют НАСТОЯЩИМ целям движка
// (см. EngineHooks.cpp): onDamage — thiscall(this,msg), 1 стековый аргумент,
// RET 4; handleMessage — 2 стековых, RET 8. Тест-заглушки обязаны
// повторять расклад стека, иначе проверка «детур форвардит стековые
// аргументы» ничего не проверяет.

/// thiscall(this, msg) эквивалентно __fastcall(this, edx, msg):
/// 1 стековый аргумент, callee пoпает (RET 4).
int __fastcall TargetDamage(int a, int b, int c) {
    int r = a - b;
    (void)c;  // 1-й стековый аргумент — имитируем msg
    r += 0;
    return r;
}

/// 2 стековых аргумента, callee пoпает (RET 8).
int __fastcall TargetChat(int a, int b, int c, int d) {
    int r = a ^ b;
    (void)c;
    (void)d;
    r += 0;
    return r;
}

typedef int(__fastcall* TargetFn)(int, int);
typedef int(__fastcall* TargetDamageFn)(int, int, int);
typedef int(__fastcall* TargetChatFn)(int, int, int, int);

volatile TargetFn g_pAdd = &TargetAdd;
volatile TargetFn g_pMul = &TargetMul;
volatile TargetDamageFn g_pDamage = &TargetDamage;
volatile TargetChatFn g_pChat = &TargetChat;

/// Значения, которые нельзя свернуть: берутся из volatile.
volatile int g_a = 3;
volatile int g_b = 4;
volatile int g_c = 5;
volatile int g_d = 6;

}  // namespace

/// Прямой вызов цели — так проверяем, что после снятия хука всё работает.
static int CallAdd(int a, int b) { return TargetAdd(a, b); }
static int CallMul(int a, int b) { return TargetMul(a, b); }
static int CallDamage(int a, int b, int c) { return TargetDamage(a, b, c); }
static int CallChat(int a, int b, int c, int d) {
    return TargetChat(a, b, c, d);
}

int main() {
    Check(evbots::RequireExactRushSpawn("RushLarge0", true, false, false) &&
          evbots::RequireExactRushSpawn("SquadRush0", true, false, false) &&
          !evbots::RequireExactRushSpawn("RushLarge0", true, false, true) &&
          !evbots::RequireExactRushSpawn("RushLarge0", true, true, false) &&
          !evbots::RequireExactRushSpawn("ConquestSmall0", true, false, false),
          "Rush: выбранная позиция не заменяется запасным спавнером; техника и явный Spawn сохраняются");
    Check(!evbots::HasCompleteLevelInfo("Levels/MP_011/MP_011", "") &&
          !evbots::HasCompleteLevelInfo(nullptr, "RushLarge0") &&
          evbots::HasCompleteLevelInfo("Levels/MP_011/MP_011", "RushLarge0") &&
          evbots::HasCompleteLevelInfo("Levels/XP3_Desert/XP3_Desert", "TankSuperiority0"),
          "режим: имя карты без GameMode не разрешает загрузку модов и спавн");
    Check(evbots::RushBaseDestroyedAccepted(evbots::kRushDestructBase, true, 3, 2) &&
          !evbots::RushBaseDestroyedAccepted(evbots::kRushIncreaseReinforce, true, 3, 2) &&
          !evbots::RushBaseDestroyedAccepted(evbots::kRushDestructBase, false, 3, 2),
          "Rush: завершение сектора отделено от пополнения жизней и событий других сущностей");
    Check(!evbots::RushBaseDestroyedAccepted(evbots::kRushDestructBase, true, 3, 3) &&
          !evbots::RushBaseDestroyedAccepted(evbots::kRushDestructBase, true, 0, -1) &&
          evbots::RushFinalBase(true, 0, false) && !evbots::RushFinalBase(true, 0, true) &&
          !evbots::RushFinalBase(false, 0, false) && !evbots::RushFinalBase(true, 1, false),
          "Rush: отклонённый вход не меняет сектор; final соответствует последней базе защитников");
    const evbots::BaseCapturePoint basePoints[] = {{30, 1, 0}, {200, 2, 1}, {100, 1, 1}};
    const evbots::BaseCharacterSpawn baseSpawns[] = {
        {1, 30, 1, false}, {2, 100, 1, true}, {3, 200, 1, false},
        {400, 100, 0, false}, {500, 200, 2, false}
    };
    Check(evbots::SelectTeamBaseSpawn(1, basePoints, 3, baseSpawns, 5) == 400 &&
          evbots::SelectTeamBaseSpawn(2, basePoints, 3, baseSpawns, 5) == 500,
          "HQ: захваченный флаг, чужая команда и точка техники не становятся базовым спавном");
    Check(evbots::SelectTeamBaseSpawn(1, basePoints, 1, baseSpawns, 5) == 0 &&
          evbots::SelectTeamBaseSpawn(0, basePoints, 3, baseSpawns, 5) == 0,
          "HQ: без подтверждённой базы нет запасного спавна по порядку адресов");
    Check(evbots::RequireBaseSpawn("TankSuperiority0", true, false, false),
          "TankSuperiority: позиция пехоты не открывает спавн на захваченном флаге");
    Check(!evbots::RequireBaseSpawn("TankSuperiority0", true, false, true) &&
          !evbots::RequireBaseSpawn("TankSuperiority0", true, true, false) &&
          !evbots::RequireBaseSpawn("TankSuperiority0", false, false, false),
          "TankSuperiority: прямая посадка в технику и явная точка сохраняются");
    Check(!evbots::RequireBaseSpawn("ConquestSmall0", true, false, false) &&
          !evbots::RequireBaseSpawn("RushLarge0", true, false, false) &&
          !evbots::RequireBaseSpawn("AirSuperiority0", true, false, false),
          "ограничение танкового спавна не меняет остальные режимы");
    // Вывод сразу, без буферизации: если процесс упадёт посреди проверки,
    // мы увидим, на каком пункте остановились, а не пустоту.
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetUnhandledExceptionFilter(CrashProbe);

    printf("=== EVBots.dll: проверка без BF3 ===\n");
    printf("PID %lu, архитектура: ", GetCurrentProcessId());
    printf("%d-бит\n", (int)(sizeof(void*) * 8));

    // ── 1. загрузка DLL ───────────────────────────────────────────────────
    Section("1. загрузка DLL");

    const char* path = "EVBots.dll";
    HMODULE dll = LoadLibraryA(path);
    if (dll == nullptr) {
        printf("  [FAIL] не удалось загрузить %s, ошибка %lu\n", path,
               GetLastError());
        printf("\nИТОГ: загрузка не удалась\n");
        return 1;
    }

    Check(true, "EVBots.dll загружена");

    // ── 2. экспорт GetModdingInterface ─────────────────────────────────────
    Section("2. контракт GetModdingInterface");

    auto getModding =
        reinterpret_cast<void*(__cdecl*)()>(GetProcAddress(dll, "GetModdingInterface"));
    Check(getModding != nullptr, "экспорт GetModdingInterface найден");

    if (getModding == nullptr) {
        FreeLibrary(dll);
        return 1;
    }

    void* obj = getModding();
    Check(obj != nullptr, "функция вернула объект");

    if (obj == nullptr) {
        FreeLibrary(dll);
        return 1;
    }

    // vtable: первое поле объекта
    void** vtable = *reinterpret_cast<void***>(obj);
    Check(vtable != nullptr, "у объекта есть vtable");

    if (vtable != nullptr) {
        Check(vtable[0] != nullptr, "метод [0] Initialize");
        Check(vtable[1] != nullptr, "метод [1] OnTick");
        Check(vtable[2] != nullptr, "метод [2] OnNewRound");
        Check(vtable[3] != nullptr, "метод [3] QueryState");
        Check(vtable[4] != nullptr, "метод [4] Shutdown");

        // У настоящей veniceext слот [5] ТОЖЕ не пустой: там лежит строка
        // "1.16". Так что проверка «vtable[5] == nullptr» неверна по определению.
        //
        // Правильный критерий: за пятью методами начинаются ЧУЖИЕ данные, то
        // есть адрес слота [5] не принадлежит коду нашей DLL.
        printf("  [инфо] метод [5] = %p\n", vtable[5]);
        HMODULE self = GetModuleHandleA("EVBots.dll");
        MEMORY_BASIC_INFORMATION mbi{};
        const bool inOurImage =
            self != nullptr &&
            VirtualQuery(vtable[5], &mbi, sizeof(mbi)) == sizeof(mbi) &&
            mbi.AllocationBase == self;
        Check(!inOurImage, "за 5 методами начинаются чужие данные, как у veniceext");

        // повторный вызов обязан вернуть тот же объект (ленивый синглтон)
        Check(getModding() == obj, "повторный вызов вернул тот же объект");

        // ── 3. Initialize ──────────────────────────────────────────────
        Section("3. Initialize");

        using InitFn = bool(__thiscall*)(void*);
        auto init = reinterpret_cast<InitFn>(vtable[0]);
        Check(init(obj), "Initialize() вернул true");

        // ── 4. хуки: ставим на функции теста ───────────────────────────
        Section("4. хуки на функции тест-хоста");

        // экспортируем из DLL то, чем пользуется тест
        auto setOverrides = reinterpret_cast<void(__cdecl*)(const void*)>(
            GetProcAddress(dll, "EVBotsTestSetHooks"));
        Check(setOverrides != nullptr, "найден тестовый хук EVBotsTestSetHooks");

        auto lastResult = reinterpret_cast<int(__cdecl*)(int)>(
            GetProcAddress(dll, "EVBotsTestLastHookResult"));
        Check(lastResult != nullptr, "найден EVBotsTestLastHookResult");

        auto counters = reinterpret_cast<unsigned long*(__cdecl*)()>(
            GetProcAddress(dll, "EVBotsTestCounters"));
        Check(counters != nullptr, "найден EVBotsTestCounters");

        auto trampoline = reinterpret_cast<void*(__cdecl*)()>(
            GetProcAddress(dll, "EVBotsTestTrampoline"));
        Check(trampoline != nullptr, "найден EVBotsTestTrampoline");

        if (setOverrides == nullptr || lastResult == nullptr ||
            counters == nullptr || trampoline == nullptr) {
            FreeLibrary(dll);
            return 1;
        }

        struct Override4 {
            void* onNewRound;
            void* tick;
            void* onDamage;
            void* chatMessage;
        };

        Override4 ov{};
        ov.onNewRound = reinterpret_cast<void*>(&TargetAdd);
        ov.tick = reinterpret_cast<void*>(&TargetMul);
        ov.onDamage = reinterpret_cast<void*>(&TargetDamage);
        ov.chatMessage = reinterpret_cast<void*>(&TargetChat);

        // ставим хуки ДО вызова Initialize, чтобы подменить цели
        setOverrides(&ov);

        // Initialize уже был вызван выше: он не нашёл bf3_server_Final
        // и хуки не поставил. Переинициализируем через Shutdown + Initialize.
        using ShutdownFn = void(__thiscall*)(void*);
        auto shutdown = reinterpret_cast<ShutdownFn>(vtable[4]);
        shutdown(obj);

        setOverrides(&ov);

        using InitFn3 = bool(__thiscall*)(void*);
        auto init3 = reinterpret_cast<InitFn3>(vtable[0]);
        const bool initOk = init3(obj);
        Check(initOk, "повторный Initialize() с переопределёнными целями");

        printf("  [инфо] результаты хуков:");
        for (int i = 0; i < 4; i++) {
            printf(" [%d]=%d", i, lastResult(i));
        }
        printf("\n  (0=установлен, 1=уже перехвачен, 2=неверный адрес, "
               "3=мало байт)\n");

        Check(lastResult(0) == 0, "хук onNewRound установлен");
        Check(lastResult(1) == 0, "хук tick установлен");
        Check(lastResult(2) == 0, "хук onDamage установлен");
        Check(lastResult(3) == 0, "хук chatMessage установлен");

        // ── 5. хук действительно перехватывает ─────────────────────────
        Section("5. перехват работает");

        // ── что реально лежит в памяти ──────────────────────────────────
        // При падении внутри перехвата важно увидеть сами байты: иначе
        // приходится гадать, правильно ли посчитан прыжок в трамплине.
        auto dump = [](const char* what, void* addr) {
            auto* p = reinterpret_cast<unsigned char*>(addr);
            printf("  %s @ %p:", what, p);
            for (int i = 0; i < 16; i++) {
                printf(" %02X", p[i]);
            }
            printf("\n");
        };

        void* tramp0 = trampoline();
        dump("патч цели 0", reinterpret_cast<void*>(ov.onNewRound));
        if (tramp0 != nullptr) {
            dump("трамплин  0", tramp0);
        }

        // Сверяем арифметику прыжка: патч обязан вести ровно в обходник.
        auto detourOf = reinterpret_cast<void*(__cdecl*)()>(
            GetProcAddress(dll, "EVBotsTestDetour"));
        auto stolenOf = reinterpret_cast<unsigned long(__cdecl*)()>(
            GetProcAddress(dll, "EVBotsTestStolenBytes"));
        if (detourOf != nullptr && stolenOf != nullptr) {
            void* detour = detourOf();
            auto* patched = reinterpret_cast<unsigned char*>(ov.onNewRound);

            int32_t rel32 = 0;
            std::memcpy(&rel32, patched + 1, 4);
            void* jumpTarget = patched + 5 + rel32;
            int32_t back32 = 0;
            // в трамплине прыжок стоит ПОСЛЕ украденных байт
            const unsigned long stolen = stolenOf();
            std::memcpy(&back32,
                        static_cast<uint8_t*>(tramp0) + stolen + 1, 4);
            void* backTarget =
                static_cast<uint8_t*>(tramp0) + stolen + 5 + back32;

            // кто кому принадлежит: без этого легко перепутать адреса
            // exe, DLL и кучи, а разница между ними тут больше гигабайта
            auto which = [](void* addr) {
                HMODULE mod = nullptr;
                char name[MAX_PATH] = "?";
                if (GetModuleHandleExA(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        static_cast<LPCSTR>(addr), &mod)) {
                    GetModuleFileNameA(mod, name, MAX_PATH);
                    const char* slash = strrchr(name, '\\');
                    return slash != nullptr ? slash + 1 : name;
                }
                return "куча/динамическая память";
            };

            printf("  exe  %p\n", (void*)GetModuleHandleA(nullptr));
            printf("  DLL  %p\n", (void*)dll);
            printf("  цель      %p  модуль: %s\n", ov.onNewRound,
                   which(ov.onNewRound));
            printf("  обходник  %p  модуль: %s\n", detour, which(detour));
            printf("  трамплин  %p  модуль: %s\n", tramp0, which(tramp0));
            printf("  патч ведёт на %p  -> %s\n", jumpTarget,
                   jumpTarget == detour ? "ВЕРНО" : "НЕВЕРНО");
            printf("  трамплин назад ведёт на %p\n", backTarget);
            printf("  украдено  %lu байт\n", stolenOf());
        }

        Check(g_pAdd(g_a, g_b) == 7, "TargetAdd(3,4) вернул 7");
        Check(g_pMul(g_a, g_b) == 12, "TargetMul(3,4) вернул 12");
        Check(g_pDamage(g_a, g_b, g_c) == -1,
              "TargetDamage(3,4,5) вернул -1");
        Check(g_pChat(g_a, g_b, g_c, g_d) == 7,
              "TargetChat(3,4,5,6) вернул 7");

        const unsigned long* c = counters();
        printf("  [инфо] счётчики: onNewRound=%lu tick=%lu onDamage=%lu "
               "chat=%lu\n", c[0], c[1], c[2], c[3]);

        Check(c[0] == 1, "onNewRound вызван через хук (1)");
        Check(c[1] == 1, "tick вызван через хук (1)");
        Check(c[2] == 1, "onDamage вызван через хук (1)");
        Check(c[3] == 1, "chatMessage вызван через хук (1)");

        // трамплин обязан выполнять оригинальный код
        void* tramp = trampoline();
        Check(tramp != nullptr, "трамплин создан");
        printf("  [инфо] трамплин onNewRound = %p\n", tramp);

        if (tramp != nullptr) {
            auto viaTramp = reinterpret_cast<TargetFn>(tramp);
            Check(viaTramp(g_a, g_b) == 7, "вызов через трамплин вернул 7");
        }

        // ── 6. многократные вызовы: состояние растёт ───────────────────
        Section("6. повторные вызовы");

        for (int i = 0; i < 100; i++) {
            g_pMul(i, 2);
        }

        c = counters();
        printf("  [инфо] tick=%lu после 100 доп. вызовов\n", c[1]);
        Check(c[1] >= 101, "счётчик tick вырос до >=101");

        // ── 7. вызовы через vtable ───────────────────────────────────────
        //
        // Здесь проверяется САМ КОНТРАКТ: слоты [1] и [2] действительно
        // ведут в реализацию. Счётчик хуков тут ни при чём — хуки считают
        // попадания в перехваченные функции, а не вызовы интерфейса.
        // Поэтому читаем состояние и смотрим, что оно меняется.
        Section("7. вызовы через vtable");

        auto stateOf = reinterpret_cast<void(__cdecl*)(unsigned long*)>(
            GetProcAddress(dll, "EVBotsTestState"));
        Check(stateOf != nullptr, "найден EVBotsTestState");

        unsigned long beforeState[3] = {};
        unsigned long afterState[3] = {};
        if (stateOf != nullptr) {
            stateOf(beforeState);
        }

        using NewRoundFn = void(__thiscall*)(void*);
        auto newRound = reinterpret_cast<NewRoundFn>(vtable[2]);
        newRound(obj);

        using TickFn = void(__thiscall*)(void*);
        auto tick = reinterpret_cast<TickFn>(vtable[1]);
        tick(obj);
        tick(obj);

        if (stateOf != nullptr) {
            stateOf(afterState);
            printf("  [инфо] состояние: было [тиков %lu, раундов %lu] -> "
                   "стало [тиков %lu, раундов %lu]\n",
                   beforeState[0], beforeState[1], afterState[0],
                   afterState[1]);

            Check(afterState[1] == beforeState[1] + 1,
                  "OnNewRound через vtable увеличил счётчик раундов");
            Check(afterState[0] == beforeState[0] + 2,
                  "OnTick через vtable увеличил счётчик тиков на 2");
            Check(afterState[2] == 1, "после OnNewRound уровень помечен загруженным");
        }

        // ── 8. снятие хуков ─────────────────────────────────────────────
        Section("8. Shutdown снимает хуки");

        shutdown(obj);

        c = counters();
        const unsigned long before = c[0];

        CallAdd(1, 1);
        CallAdd(1, 1);

        c = counters();
        Check(c[0] == before, "после Shutdown хуки не срабатывают");
        Check(CallAdd(6, 7) == 13, "функция работает как обычно");

        // ── 9. повторная установка ───────────────────────────────────────
        Section("9. повторная установка после снятия");

        setOverrides(&ov);
        Check(init3(obj), "Initialize() снова прошёл");
        Check(lastResult(0) == 0, "хуки ставятся повторно");

        // ── 9b. эмуляция загрузки уровня для модов ────────────────────────
        // У тест-хоста движка нет, поэтому движковые события загрузки уровня
        // рассылаем вручную — только чтобы fun-bots прошёл до спавна ботов.
        Section("9b. эмуляция Level:Loaded для модов");

        auto simulateLevel = reinterpret_cast<void(__cdecl*)(const char*, const char*, int, int)>(
            GetProcAddress(dll, "EVBotsTestSimulateLevel"));
        Check(simulateLevel != nullptr, "найден EVBotsTestSimulateLevel");

        if (simulateLevel != nullptr) {
            simulateLevel("Levels/MP_001/MP_001", "ConquestLarge0", 1, 2);
        }

        // ── 9c. эмуляция тиков движка для спавна ботов ─────────────────────
        // fun-bots спавнит ботов в OnEngineUpdate после FIRST_SPAWN_DELAY
        // (Registry.BOT_SPAWN.FIRST_SPAWN_DELAY = 5.0). Рассылаем тики с
        // шагом 0.05, чтобы покрыть 5-секундную задержку (100+ тиков) и
        // выудить следующий стоппер.
        Section("9c. эмуляция Engine:Update для спавна ботов");

        auto simulateTicks = reinterpret_cast<void(__cdecl*)(int, double)>(
            GetProcAddress(dll, "EVBotsTestSimulateTicks"));
        Check(simulateTicks != nullptr, "найден EVBotsTestSimulateTicks");

        auto addHuman = reinterpret_cast<void(__cdecl*)(const char*, int, int)>(
            GetProcAddress(dll, "EVBotsTestAddHumanPlayer"));
        Check(addHuman != nullptr, "найден EVBotsTestAddHumanPlayer");

        // Без живого игрока fun-bots не спавнит ботов (BotSpawner.lua:480).
        // Добавляем одного «человека», потом тикаем до первого спавна.
        if (addHuman != nullptr) {
            addHuman("TestHuman", 2, 1);
        }

        if (simulateTicks != nullptr) {
            simulateTicks(600, 0.05);  // ~30 секунд игрового времени
        }

        g_pMul(g_a, g_b);
        c = counters();
        Check(c[1] > 0, "перехват работает после повторной установки");

        // ── 9d. К4 (сборка 219): упаковка NetEvents и приём сервером ─────────
        Section("9d. NetEvents по каналу (К4)");
        {
            auto luaTrueS = reinterpret_cast<int(__cdecl*)(const char*)>(GetProcAddress(dll, "EVBotsTestLuaTrue"));
            if (luaTrueS != nullptr) {
                Check(luaTrueS("(function() local g = Guid('67A2C146-9CC0-E7EC-5227-B2DCB9D316C1') "
                               "local d = CharacterSpawnReferenceObjectData(g) "
                               "return not rawequal(d,g) and d.typeInfo.name == 'CharacterSpawnReferenceObjectData' "
                               "and d.instanceGuid == g and CharacterSpawnReferenceObjectData(d) == d end)()") == 1,
                      "CharacterSpawnReferenceObjectData: Guid constructs data instead of an identity cast");
                Check(luaTrueS("SkyComponentData.typeInfo.name == 'SkyComponentData' and "
                               "type(SkyComponentData.typeInfo.fields) == 'table' and "
                               "SkyComponentData.typeInfo == SkyComponentData.typeInfo") == 1,
                      "Frostbite classes expose static TypeInformation");
                Check(luaTrueS("(function() local g = MathUtils:RandomGuid() local n = g:ToString('N') "
                               "return getmetatable(g) == getmetatable(Guid()) and #n == 32 "
                               "and n:match('^[0-9A-F]+$') ~= nil and n ~= string.rep('0', 32) "
                               "and #g:ToString() == 36 and g:ToString():gsub('-', '') == n "
                               "and g:ToString('B') == '{' .. g:ToString() .. '}' "
                               "and g:ToString('P') == '(' .. g:ToString() .. ')' "
                               "and Guid(n, 'N') == g and g:Clone() == g and not rawequal(g:Clone(), g) end)()") == 1,
                      "RandomGuid: nonzero Guid, D/N/B/P formats, roundtrip and independent clone");
                Check(luaTrueS("(function() local seen = {} for i=1,4096 do "
                               "local n = MathUtils:RandomGuid():ToString('N') "
                               "if seen[n] then return false end seen[n] = true end return true end)()") == 1,
                      "RandomGuid: 4096 repeated calls have no duplicate GUIDs");
                Check(luaTrueS("(function() local teams = {} for i=1,256 do "
                               "local id repeat id = MathUtils:RandomGuid():ToString('N'):sub(1, 4) "
                               "until teams[id] == nil if #id ~= 4 or not id:match('^[0-9A-F]+$') then return false end "
                               "teams[id] = true end return true end)()") == 1,
                      "RandomGuid: Royale CreateId expression creates short team IDs");
                Check(luaTrueS("(function() local t = {'reason', 0.8, false, {1, 2}} "
                               "local ok = pcall(print, t, 'PRINT-ARRAY-END') "
                               "return ok and t[1] == 'reason' and t[2] == 0.8 and t[3] == false and t['1'] == nil end)()") == 1,
                      "print: numeric table keys remain valid during iteration");
                Check(luaTrueS("(function() local t = {} for i=1,40 do t[i]=i end "
                               "local ok = pcall(print, {nested=t, sibling={3,4}}, t, 'PRINT-LIMIT-END') "
                               "return ok and #t == 40 and t[40] == 40 end)()") == 1,
                      "print: truncating nested tables preserves the Lua stack");
                Check(luaTrueS("(function() local p = PlayerManager:GetPlayerByName('TestHuman') "
                               "return p ~= nil and p:GetName() == p.name end)()") == 1,
                      "Player:GetName supports table-based VU compatibility helpers");
                Check(luaTrueS("PlayerManager:FadeInAll(0.5) == false and PlayerManager:FadeOutAll(2) == false") == 1,
                      "FadeAll refuses native calls outside the server engine frame");
                Check(luaTrueS("(function() local a = pcall(PlayerManager.FadeOutAll, PlayerManager, -1) "
                               "local b = pcall(PlayerManager.FadeInAll, PlayerManager, 0/0) "
                               "local c = pcall(PlayerManager.FadeInAll, PlayerManager, math.huge) "
                               "return not a and not b and not c end)()") == 1,
                      "FadeAll rejects negative and non-finite durations");
                Check(luaTrueS("(function() local p = PlayerManager:GetPlayerByName('TestHuman') "
                               "return p:Fade(1, false) == false and p:Fade(0, true) == false end)()") == 1,
                      "Player:Fade refuses native calls outside the server engine frame");
                Check(luaTrueS("(function() local p = PlayerManager:GetPlayerByName('TestHuman') "
                               "local a = pcall(p.Fade, p, -1, false) local b = pcall(p.Fade, p, 0/0, false) "
                               "local c = pcall(p.Fade, p, math.huge, true) return not a and not b and not c end)()") == 1,
                      "Player:Fade rejects negative and non-finite durations");
                // один тип — одна проверка: при провале сразу видно, какой
                static const char* kCodecCases[][2] = {
                    {"5", "a == 5 and math.type(a) == 'integer'"},
                    {"2.5", "a == 2.5"},
                    {"'я'", "a == 'я'"},
                    {"true", "a == true"},
                    {"Vec3(1,2,3)", "a == Vec3(1,2,3)"},
                    {"Vec2(4,5)", "a.x == 4 and a.y == 5"},
                    {"Vec4(1,2,3,4)", "a.x == 1 and a.w == 4"},
                    {"Quat(0,0,0,1)", "a.w == 1"},
                    {"Guid('0B40AC87-7AFF-EDDE-949A-FCBF42CF7126')", "a == Guid('0B40AC87-7AFF-EDDE-949A-FCBF42CF7126')"},
                    {"{x = 1, [2] = {Vec3(7,8,9)}}", "a.x == 1 and a[2][1] == Vec3(7,8,9)"},
                    {"LinearTransform()", "a.left == Vec3(1,0,0) and a.trans == Vec3(0,0,0)"},
                    {"QuatTransform()", "a.rotation.w == 1 and a.transAndScale.w == 1"},
                    {"AxisAlignedBox(Vec3(1,1,1), Vec3(2,2,2))", "a.max == Vec3(2,2,2)"},
                };
                for (const auto& kc : kCodecCases) {
                    char expr[512], what[160];
                    sprintf_s(expr, "(function() local k, f, name, n, args = EVNetCodec.Decode(EVNetCodec.EncodeEvent(1, 'Ev', nil, %s)) "
                              "local a = args[2] return k == EVNetCodec.KIND_EVENT and f == 1 and name == 'Ev' and n == 2 "
                              "and args[1] == nil and (%s) end)()", kc[0], kc[1]);
                    sprintf_s(what, "кодек NetEvents туда-обратно: %s", kc[0]);
                    Check(luaTrueS(expr) == 1, what);
                }
                Check(luaTrueS("(function() local ok = pcall(EVNetCodec.EncodeEvent, 0, 'Ev', function() end) "
                               "local k, name = EVNetCodec.Decode(EVNetCodec.EncodeHello('KOTuk1')) "
                               "return not ok and k == EVNetCodec.KIND_HELLO and name == 'KOTuk1' end)()") == 1,
                      "кодек: функцию передать нельзя (как VU), приветствие с именем");
                // приём: соединение 7, приветствие TestHuman, событие -> обработчик (player, args) как в guides\netevents.md
                Check(luaTrueS("(function() local got "
                               "local e = NetEvents:Subscribe('K4:Test', function(p, x, v) got = {p = p, x = x, v = v} end) "
                               "NetEvents._OnNet(7, 1, '127.0.0.1:5555') "
                               "NetEvents._OnNet(7, 2, EVNetCodec.EncodeHello('TestHuman')) "
                               "NetEvents._OnNet(7, 2, EVNetCodec.EncodeEvent(0, 'K4:Test', 42, Vec3(1,1,1))) "
                               "NetEvents._OnNet(7, 3, '') e:Unsubscribe() "
                               "return got ~= nil and got.p ~= nil and got.p.name == 'TestHuman' and got.x == 42 and got.v == Vec3(1,1,1) end)()") == 1,
                      "сервер: NetEvent с клиента -> обработчик(player, ...) с игроком по приветствию");
                Check(luaTrueS("(function() local p = PlayerManager:CreatePlayer('LevelResetTest', 1, 0) "
                               "local nb = NativeBridge local calls = 0 "
                               "NativeBridge = { ReadU32 = function() return 1 end, "
                               "HumanPtr = function() calls = calls + 1 return 222 end } "
                               "rawset(p, 'onlineId', 42) rawset(p, '_humanPtr', 111) "
                               "rawset(p, '_respBody', true) rawset(p, 'clientSettingsSent', true) "
                               "rawset(p, '_modWaitSince', 1) rawset(p, '_modWaitSaid', true) "
                               "local old = EVBotsHumanPtr(p) PlayerManager._LevelReset() "
                               "local clean = rawget(p, '_humanPtr') == nil and rawget(p, '_respBody') == nil "
                               "and not p.clientSettingsSent and rawget(p, '_modWaitSince') == nil "
                               "and rawget(p, '_modWaitSaid') == nil "
                               "local fresh = EVBotsHumanPtr(p) NativeBridge = nb "
                               "rawset(p, 'onlineId', 0) PlayerManager:DeletePlayer(p) "
                               "return old == 111 and clean and fresh == 222 and calls == 1 end)()") == 1,
                      "смена уровня: читаемый старый указатель человека сброшен, настройки запрашиваются заново");
                Check(luaTrueS("(function() local nb = NativeBridge local calls = 0 "
                               "local p = {name = 'InputRetryTest', nativePtr = 123} "
                               "NativeBridge = {BotOwnInput = function() calls = calls + 1 return nil, calls > 30 end} "
                               "for i = 1, 30 do EVBotsEnsureOwnInput(p) end "
                               "local waiting = p.ownInputOn == nil and p._ownInputFails == 30 "
                               "EVBotsEnsureOwnInput(p) EVBotsEnsureOwnInput(p) NativeBridge = nb "
                               "return waiting and calls == 31 and p.ownInputOn == 123 and p._ownInputFails == nil end)()") == 1,
                      "ввод: после 30 отказов повтор продолжается и отмечается только успешная установка");
                Check(luaTrueS("(function() local gd, bm = g_GameDirector, g_BotManager local position = Vec3(100, 10, 200) "
                               "local chosen, searched local bot = {SetVarsWay = function(_, _, random, path, point, invert) "
                               "chosen = random and path == 7 and point == 9 and invert == false end} "
                               "g_GameDirector = {FindClosestPath = function(_, pos, veh, detailed, _, increment) "
                               "searched = pos == position and not veh and detailed and increment == 1 "
                               "return {PathIndex = 7, PointIndex = 9} end} "
                               "g_BotManager = {GetBotByName = function(_, name) if name == 'BasePathTest' then return bot end end} "
                               "local result = EVBotsAlignBaseSpawn({name = 'BasePathTest'}, position) "
                               "g_GameDirector, g_BotManager = gd, bm return result and searched and chosen end)()") == 1,
                      "танковый режим: маршрут после базового спавна ищется от фактической позиции тела");
                Check(luaTrueS("(function() local h1 = PlayerManager:CreatePlayer('RushHuman1', 1, 1) "
                               "local h2 = PlayerManager:CreatePlayer('RushHuman2', 2, 1) "
                               "rawset(h1, 'onlineId', 901) rawset(h2, 'onlineId', 902) "
                               "rawset(h1, '_humanPtr', 9011) rawset(h2, '_humanPtr', 9022) "
                               "local original = NativeBridge local nb = {} NativeBridge = nb local got = {} "
                               "nb.PollDeadBots = function() return {} end "
                               "nb.ReadU32 = function(ptr, off) if ptr == 9011 or ptr == 9022 then return off == 0 and 1 or 0 end return 0 end "
                               "nb.PollStatEvents = function() return {{9011, 86, 0, 0, 1}, {9022, 87, 0, 0, 1}} end "
                               "local sub = Events:Subscribe('ScoringSystem:StatEvent', function(actor, _, ev) "
                               "if ev == 86 or ev == 87 then got[ev] = actor end end) "
                               "local ok = pcall(Events._DispatchEngine, Events, 'Engine:Update', 0.5, 0.5) "
                               "sub:Unsubscribe() NativeBridge = original "
                               "local result = ok and got[86] == h1 and got[87] == h2 "
                               "rawset(h1, 'onlineId', 0) rawset(h2, 'onlineId', 0) "
                               "PlayerManager:DeletePlayer(h1) PlayerManager:DeletePlayer(h2) return result end)()") == 1,
                      "Rush: постановка и снятие заряда двумя людьми доходят через тик до ScoringSystem:StatEvent");
            }
        }

        shutdown(obj);

        // ── 10. клиентский режим (сборка 215, К3) ───────────────────────────
        // В bf3.exe серверного Initialize нет: поднимается клиентский Lua (LuaContext::Client) с клиентскими
        // библиотеками VU и модами ext\shared + ext\client. Серверный рантайм выше уже остановлен.
        Section("10. клиентский режим (К3)");

        auto clientMode = reinterpret_cast<int(__cdecl*)(const wchar_t*)>(GetProcAddress(dll, "EVBotsTestClientMode"));
        auto stopClient = reinterpret_cast<void(__cdecl*)()>(GetProcAddress(dll, "EVBotsTestStopClientMode"));
        auto luaTrue = reinterpret_cast<int(__cdecl*)(const char*)>(GetProcAddress(dll, "EVBotsTestLuaTrue"));
        Check(clientMode != nullptr && stopClient != nullptr && luaTrue != nullptr,
              "найдены EVBotsTestClientMode / StopClientMode / LuaTrue");
        if (clientMode != nullptr && stopClient != nullptr && luaTrue != nullptr) {
            // моды: EVBOTS_MODS, иначе Admin\Mods стенда (только чтение: клиентская часть fun-bots на диск не пишет)
            wchar_t mods[MAX_PATH] = {};
            if (GetEnvironmentVariableW(L"EVBOTS_MODS", mods, MAX_PATH) == 0) {
                const wchar_t* stand = L"C:\\Games\\BF-Portable\\Battlefield_3_Server\\Instance\\Admin\\Mods";
                if (GetFileAttributesW(stand) != INVALID_FILE_ATTRIBUTES) wcscpy_s(mods, stand);
            }
            wchar_t coreOnlyFlag[4] = {};
            const bool coreOnly = GetEnvironmentVariableW(L"EVBOTS_TEST_CORE_ONLY", coreOnlyFlag, 4) == 1 && coreOnlyFlag[0] == L'1';
            const bool withMods = !coreOnly && mods[0] != 0;
            if (coreOnly) printf("  [SKIP] fun-bots integration: EVBOTS_TEST_CORE_ONLY=1; core APIs only\n");
            printf("  [инфо] моды клиента: %ls\n", withMods ? mods : L"(нет — Lua без модов)");

            Check(clientMode(withMods ? mods : nullptr) == 1, "клиентский Lua поднялся");
            Check(luaTrue("SharedUtils:IsClientModule() == true and SharedUtils:IsServerModule() == false") == 1,
                  "SharedUtils: клиентский модуль");
            Check(luaTrue("(function() local a = MathUtils:RandomGuid() local b = MathUtils:RandomGuid() "
                          "return a ~= b and #a:ToString('N') == 32 and Guid(a:ToString()) == a end)()") == 1,
                  "RandomGuid: available after client Lua initialization");
            Check(luaTrue("type(InputManager) == 'table' and InputManager:WentKeyDown(1) == false "
                          "and InputManager:GetLevel(0) == 0") == 1,
                  "InputManager по VU-Docs (bool/float)");
            Check(luaTrue("PlayerManager:GetLocalPlayer() == nil and type(PlayerManager:GetPlayers()) == 'table' "
                          "and PlayerManager.CreatePlayer == nil") == 1,
                  "PlayerManager клиента: GetLocalPlayer nil, серверного CreatePlayer нет");
            Check(luaTrue("rawget(_G, 'RCON') == nil and rawget(_G, 'SQL') == nil and rawget(_G, 'NativeBridge') == nil "
                          "and rawget(_G, 'TicketManager') == nil") == 1,
                  "серверных библиотек и моста в клиенте нет");
            Check(luaTrue("NetEvents.Send ~= nil and NetEvents.SendLocal ~= nil and NetEvents.SendTo == nil "
                          "and NetEvents.Broadcast == nil") == 1,
                  "NetEvents клиента (Send*, без SendTo/Broadcast)");
            Check(luaTrue("(function() local got, ctx; local o = {} "
                          "NetEvents:Subscribe('EVBotsTest', function(a) got = a end) "
                          "NetEvents:Subscribe('EVBotsTest', o, function(self, a) ctx = (self == o) and a end) "
                          "local n = NetEvents:_Deliver('EVBotsTest', 42) NetEvents:Send('EVBotsTest', 1) "
                          "return n == 2 and got == 42 and ctx == 42 end)()") == 1,
                  "NetEvents:Subscribe (оба варианта) и доставка входящего");
            Check(luaTrue("ClientUtils:GetCameraTransform() == nil and RaycastManager:Raycast(nil, nil, 0) == nil "
                          "and #RaycastManager:CollisionRaycast(nil, nil, 1, 0, 0) == 0") == 1,
                  "ClientUtils/RaycastManager: nil и пустой массив по VU-Docs");
            Check(luaTrue("WebUI:QuoteString('a\"b') == '\"a\\\\\"b\"'") == 1, "WebUI:QuoteString экранирует");
            Check(luaTrue("ResourceManager:SearchForDataContainer('x') == nil and ResourceManager:SearchForInstanceByGuid(nil) == nil "
                          "and ResourceManager:PollBundleOperation(0) == true and ResourceManager:EndLoadData(0) == false "
                          "and ResourceManager:BeginLoadData(3, {}) == 0") == 1,
                  "ResourceManager клиента: типы возврата по VU-Docs");
            Check(luaTrue("(function() local o = {} local n0 = #ResourceManager._handlers "
                          "local a = ResourceManager:RegisterPartitionLoadHandler(nil, function() end) "
                          "local b = ResourceManager:RegisterInstanceLoadHandlerOnce(nil, nil, o, function(self) end) "
                          "local h = ResourceManager._handlers b:Deregister() "
                          "return type(a.Deregister) == 'function' and #h == n0 + 2 and h[n0 + 2].hasContext "
                          "and h[n0 + 2].once and h[n0 + 2].alive == false and h[n0 + 1].alive end)()") == 1,
                  "ResourceManager: обработчики загрузки сохраняются, Deregister снимает");
            Check(luaTrue("EntityManager:GetIterator('ClientSoldierEntity'):Next() == nil and EntityManager:CreateEntity(nil, nil) == nil "
                          "and EntityManager.TraverseAllEntities ~= nil") == 1,
                  "EntityManager клиента: пустой итератор, CreateEntity nil");
            // К5 шаг 3 (215): Engine:Message из слушателя сообщений движка — Message { category, type } как у VU
            // (shared\type\message.md), значения = MessageType/MessageCategory (djb2-xor со знаком).
            Check(luaTrue("(function() local got = nil "
                          "local e = Events:Subscribe('Engine:Message', function(m) got = m end) "
                          "EVBotsClientBridge.EngineMessage(MessageCategory.ClientState, MessageType.ClientStateEnteredIngameMessage) "
                          "e:Unsubscribe() "
                          "return got ~= nil and got.type == MessageType.ClientStateEnteredIngameMessage "
                          "and got.type == -169333312 and got.category == -625230933 "
                          "and not pcall(function() got.type = 1 end) end)()") == 1,
                  "Engine:Message клиента: Message.type/category как у VU, только чтение");
            Check(luaTrue("(function() local seen={} local first=nil local second=nil "
                          "local a=Events:Subscribe('Player:Connected',function(p) seen[#seen+1]='c'..p.id "
                          "if first==nil then first=p else second=p end end) "
                          "local b=Events:Subscribe('Player:Deleted',function(p) seen[#seen+1]='d'..p.id end) "
                          "local q=EVBotsClientBridge.PlayerConnectionMessage "
                          "q(7,1) q(7,1) q(7,0) q(7,0) q(7,1) "
                          "local deferred=#seen==0 Events:_DispatchEngine('Engine:Update',0.01) "
                          "q(7,0) Events:_DispatchEngine('Engine:Update',0.01) a:Unsubscribe() b:Unsubscribe() "
                          "return deferred and table.concat(seen,',')=='c7,d7,c7,d7' and first~=second end)()") == 1,
                  "Client connection events: deferred, ordered, deduplicated and renewed after deletion");

            // Все глобалы API VU, которые берёт ext\Client + ext\Shared fun-bots (собрано по коду мода и VU-Docs 23.10):
            // библиотеки, типы-конструкторы, перечисления. Пропуск = «attempt to index a nil value» в моде.
            // (WeaponFiring сюда не входит: в моде он только в строке пути 'Weapon.Object.WeaponFiring.PrimaryFire',
            // а у типа в VU-Docs shared\type нет конструктора.)
            static const char* kVuGlobals[] = {
                "AlternateSpawnEntityData", "CharacterPoseType", "ClientUtils", "Console", "DebugRenderer", "Events",
                "Guid", "Hooks", "InputConceptIdentifiers", "InputDeviceKeys", "InputManager", "MaterialFlags",
                "MathUtils", "MessageCategory", "MessageType", "NetEvents", "PlayerManager", "RayCastFlags", "RaycastManager",
                "RegistryContainer", "ResourceCompartment", "ResourceManager", "SharedUtils", "TeamId", "UIScreenAsset",
                "UpdatePass", "Vec2", "Vec3", "Vec4", "WebUI"};
            int missing = 0;
            printf("  [инфо] нет в клиентском Lua:");
            for (const char* g : kVuGlobals) {
                char expr[128];
                sprintf_s(expr, "rawget(_G, '%s') ~= nil", g);
                if (luaTrue(expr) != 1) {
                    printf(" %s", g);
                    ++missing;
                }
            }
            printf("%s\n", missing == 0 ? " (всё есть)" : "");
            Check(missing == 0, "все глобалы VU, нужные клиентской части fun-bots, определены");
            // fb-классы как у VU (VU-Docs fb\registrycontainer.md): Foo(), Foo(guid), Foo(other), Foo(nil) — ошибка
            Check(luaTrue("(function() local a = RegistryContainer() local g = Guid('0B40AC87-7AFF-EDDE-949A-FCBF42CF7126') "
                          "local b = UIScreenAsset(g) local c = AlternateSpawnEntityData(a) "
                          "local ok = pcall(RegistryContainer, nil) "
                          "return a.typeInfo.name == 'RegistryContainer' and a:Is('RegistryContainer') and b.instanceGuid == g "
                          "and c == a and not ok and type(SoldierWeaponUnlockAsset) ~= 'nil' end)()") == 1,
                  "fb-классы VU: конструктор, по Guid, приведение, nil — ошибка");
            if (withMods) {
                Check(luaTrue("rawget(_G, 'FunBotClient') ~= nil and rawget(_G, 'Registry') ~= nil") == 1,
                      "fun-bots: ext\\shared и ext\\client выполнены (FunBotClient, Registry)");
            }
            stopClient();
            Check(luaTrue("true") == -2, "клиентский Lua закрыт");
        }
    }

    printf("\n=== ИТОГ: успешно %d, провалено %d ===\n", g_pass, g_fail);

    const int rc = g_fail == 0 ? 0 : 1;
    FreeLibrary(dll);
    printf("код возврата: %d\n", rc);
    return rc;
}
