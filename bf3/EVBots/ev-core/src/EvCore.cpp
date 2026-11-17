// EvCore.cpp — ev-core.dll: ядро EVBots, аналог vu-core.dll у Venice Unleashed (решение пользователя 23.10.2026).
//
// Разделение как у VU: vu-core.dll — хуки движка, окно, связь с облаком; veniceext.dll — Lua и API модов
// (veniceext берёт у vu-core логгер: GetModuleHandle("vu-core.dll") + GetProcAddress("GetConsoleLogger")).
// У нас: ev-core.dll — хуки движка и окно сервера; EVBots.dll — Lua и API модов, пишет в консоль через EvCoreConsoleWrite.
//
// Сейчас (сборка ev-core 1): окно выделенного сервера bf3_server_Final.exe как у VU —
//   * консоль: свои строки «[дата] [info] [EVBots] …»; движковые строки Debug/Info скрыты (как у VU: в его окне только
//     его строки), Warning и выше — видны. EVCORE_CONSOLE_ENGINE = all | warn (по умолчанию) | none;
//   * панель: Master: EV, GameState: Online вместо battlefield-3-pc / IN_GAME.
// Адреса — bf3_server_Final.exe (у VU сервер — клиентский bf3.exe в режиме дедика, его адреса нам не подходят), разбор —
// ext\SERVER-WINDOW-2026-10-23.md. Каждое место сверяется по байтам перед правкой (правило 18), всё или ничего,
// EVCORE_WINDOW=0 — без правок. Код игры не патчим: меняем только адрес в переходнике и указатель в vtable.

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>  // _set_invalid_parameter_handler (сборка 237)
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "EvCore.h"
#include "WarningThrottle.h"

namespace {
HMODULE g_module = nullptr;
std::mutex g_logGate;
std::wstring g_logPath;
}  // namespace

void FileLog(const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_logGate);
    if (g_logPath.empty()) return;
    FILE* f = _wfopen(g_logPath.c_str(), L"ab");
    if (f == nullptr) return;
    SYSTEMTIME t;
    ::GetLocalTime(&t);
    fprintf(f, "[%02u:%02u:%02u.%03u] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputs("\r\n", f);
    fclose(f);
}

namespace {

void InitLogPath() {
    wchar_t dll[MAX_PATH]{};
    ::GetModuleFileNameW(g_module, dll, MAX_PATH);
    std::wstring dir(dll);
    const size_t slash = dir.find_last_of(L"\\/");
    dir = slash == std::wstring::npos ? L"." : dir.substr(0, slash);
    ::CreateDirectoryW((dir + L"\\logs").c_str(), nullptr);
    g_logPath = dir + L"\\logs\\ev-core.log";
}

bool ProcessIs(const char* exe) { return ::GetModuleHandleA(exe) != nullptr; }

bool ReadOwn(uintptr_t va, void* out, size_t n) {
    SIZE_T got = 0;
    return ::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<LPCVOID>(va), out, n, &got) && got == n;
}

bool WriteOwn(uintptr_t va, const void* data, size_t n) {
    DWORD old = 0;
    if (!::VirtualProtect(reinterpret_cast<void*>(va), n, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(reinterpret_cast<void*>(va), data, n);
    DWORD tmp = 0;
    ::VirtualProtect(reinterpret_cast<void*>(va), n, old, &tmp);
    ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(va), n);
    return true;
}

// ── адреса bf3_server_Final.exe (timestamp и разбор — ext\SERVER-WINDOW-2026-10-23.md) ─────────────────────────────
// Приёмник журнала окна: FUN_00421300(record) (ret 4). Вход только через переходник 0x00401CEE: jmp 0x00421300.
//   record[0] -> {int level; const char* file; int line; …}, record[1] — канал ("GameLoop"), record[2] — текст.
//   level 2 — «сырая» строка без префикса (печатается record[2] как есть); 1 Debug, 4 Info, 8 Warning, 0x10 Assert,
//   0x20 Error, 0x80 Validate, 0x100 FatalAssert, 0x200 Crash.
//   Строки уходят в очередь окна под замком [0x028772AC] (FUN_00421030); пока замка нет (окно не создано) — строки
//   окну не попадают, поэтому свои строки до этого копим.
constexpr uintptr_t kLogThunk = 0x00401CEE;
constexpr unsigned char kLogThunkCode[5] = {0xE9, 0x0D, 0xF6, 0x01, 0x00};  // jmp 0x00421300
constexpr uintptr_t kLogSink = 0x00421300;
constexpr unsigned char kLogSinkCode[5] = {0xB8, 0x3C, 0x81, 0x00, 0x00};   // mov eax, 0x813C (alloca)
constexpr uintptr_t kConsoleLockVa = 0x028772AC;
// Поля панели: виртуальный FUN_01143170(this, list) (thiscall, ret 4) в vtable по слоту 0x02648D70 (.rdata).
//   Добавляет в кольцевой список (голова list: [0] первый, [1] последний) узлы 0x28 б: [0] след, [1] пред,
//   +8 ключ, +0x18 значение; строка = {char* begin; char* end; char* cap; arena}.
constexpr uintptr_t kPanelSlot = 0x02648D70;
constexpr uintptr_t kPanelFn = 0x01143170;

constexpr int kRawLine = 2;

struct EngineLogInfo {
    int level;
    const char* file;
    int line;
    int reserved[13];
};
struct EngineLogRecord {
    EngineLogInfo* info;
    const char* channel;
    const char* text;
};

using LogSinkFn = int(__fastcall*)(void* ecx, void* edx, EngineLogRecord* rec);

enum class EngineFilter { All, Warn, None };
EngineFilter g_filter = EngineFilter::Warn;
std::atomic<bool> g_consoleHooked{false};
std::mutex g_pendingGate;
std::vector<std::string> g_pending;  // свои строки до появления консоли окна

// Прогон 23.10 20:32 (ev-core 1): свои строки ушли в очередь, когда замок уже был, а окна ещё не было — PostMessage(0)
// не прошёл, флаг «перерисовка заказана» [0x02877261] остался 1, и окно больше не обновлялось (консоль пустая; ручной
// PostMessage(окно, 0x8003) — и всё появилось). Поэтому «готово» = замок И окно ([0x02877250], HWND из FUN_00421030).
constexpr uintptr_t kConsoleHwndVa = 0x02877250;

bool ConsoleReady() {
    uintptr_t lock = 0, hwnd = 0;
    return ReadOwn(kConsoleLockVa, &lock, sizeof(lock)) && lock != 0 && ReadOwn(kConsoleHwndVa, &hwnd, sizeof(hwnd)) &&
           hwnd != 0 && ::IsWindow(reinterpret_cast<HWND>(hwnd));
}

int CallSinkRaw(void* ecx, const char* ansi) {
    EngineLogInfo info{};
    info.level = kRawLine;
    info.file = "";
    EngineLogRecord rec{&info, "EVBots", ansi};
    return reinterpret_cast<LogSinkFn>(kLogSink)(ecx, nullptr, &rec);
}

void FlushPending(void* ecx) {
    std::vector<std::string> lines;
    {
        std::lock_guard<std::mutex> lock(g_pendingGate);
        if (g_pending.empty()) return;
        lines.swap(g_pending);
    }
    for (const std::string& l : lines) CallSinkRaw(ecx, l.c_str());
}

bool KeepEngineLine(const EngineLogRecord* rec) {
    if (g_filter == EngineFilter::All) return true;
    int level = 0;
    __try {
        level = rec->info->level;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
    if (level == kRawLine) return true;
    if (g_filter == EngineFilter::None) return false;
    return level >= 8;  // Warning, Assert, Error, Validate, FatalAssert, Crash
}

// 261: only the two verified terrain warnings; never suppress asserts/errors.
// Returning zero still happens in the engine. This controls console noise only.
int GroundWarningKind(const EngineLogRecord* rec) {
    __try {
        if (rec == nullptr || rec->info == nullptr || rec->info->level != 8 ||
            rec->info->file == nullptr || rec->text == nullptr) return -1;
        if (rec->info->line != 672 && rec->info->line != 686) return -1;
        if (std::strstr(rec->info->file, "HavokPhysicsManager.cpp") == nullptr) return -1;
        if (std::strcmp(rec->text, "No TerrainPhysicsEntity object when calling groundHeight method") != 0) return -1;
        return rec->info->line == 672 ? 0 : 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool ShowGroundWarning(void* ecx, const EngineLogRecord* rec) {
    // Explicit verbose console mode preserves every engine message.
    if (g_filter == EngineFilter::All) return true;
    const int kind = GroundWarningKind(rec);
    if (kind < 0) return true;
    static std::mutex gate;
    static WarningThrottle throttles[2];
    WarningThrottle::Result result;
    {
        std::lock_guard<std::mutex> lock(gate);
        result = throttles[kind].Observe(::GetTickCount64());
    }
    if (result.show && result.suppressed != 0) {
        char summary[256];
        snprintf(summary, sizeof(summary),
            "[EV-Core] groundHeight%s: %llu repeated console warnings suppressed in 30s; full details in RuntimeLog. Physics issue remains.",
            kind == 1 ? "Clamped" : "", static_cast<unsigned long long>(result.suppressed));
        CallSinkRaw(ecx, summary);
    }
    return result.show;
}

int __fastcall LogSinkDetour(void* ecx, void* edx, EngineLogRecord* rec) {
    (void)edx;
    if (ConsoleReady()) FlushPending(ecx);
    if (rec != nullptr && !KeepEngineLine(rec)) return 0;
    if (!ShowGroundWarning(ecx, rec)) return 0;
    return reinterpret_cast<LogSinkFn>(kLogSink)(ecx, nullptr, rec);
}

std::string Utf8ToAnsi(const char* utf8) {
    const int wn = ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (wn <= 0) return utf8;
    std::wstring w(static_cast<size_t>(wn), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), wn);
    const int an = ::WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (an <= 0) return utf8;
    std::string a(static_cast<size_t>(an), '\0');
    ::WideCharToMultiByte(CP_ACP, 0, w.c_str(), -1, a.data(), an, nullptr, nullptr);
    a.resize(static_cast<size_t>(an - 1));
    return a;
}

/// «[2026-10-23 20:31:05:12] [info] [EVBots] текст» — формат строк VU («[2021-08-14 17:21:04:00] [info] [VeniceEXT] …»).
std::string FormatLine(const char* level, const char* source, const char* text) {
    SYSTEMTIME t;
    ::GetLocalTime(&t);
    char head[96];
    snprintf(head, sizeof(head), "[%04u-%02u-%02u %02u:%02u:%02u:%02u] [%s] ", t.wYear, t.wMonth, t.wDay, t.wHour,
             t.wMinute, t.wSecond, t.wMilliseconds / 10, (level && *level) ? level : "info");
    std::string line(head);
    if (source != nullptr && *source != '\0') {
        line += '[';
        line += source;
        line += "] ";
    }
    line += Utf8ToAnsi(text != nullptr ? text : "");
    return line;
}

void ConsoleWrite(const char* level, const char* source, const char* text) {
    const std::string line = FormatLine(level, source, text);
    if (!g_consoleHooked.load() || !ConsoleReady()) {
        std::lock_guard<std::mutex> lock(g_pendingGate);
        if (g_pending.size() < 512) g_pending.push_back(line);
        return;
    }
    FlushPending(nullptr);
    CallSinkRaw(nullptr, line.c_str());
}

// ── панель ───────────────────────────────────────────────────────────────────────────────────────────────────────

struct EngineString {
    char* begin;
    char* end;
    char* cap;
    void* arena;
};
struct PanelNode {
    PanelNode* next;
    PanelNode* prev;
    EngineString key;
    EngineString value;
};

using PanelFn = void(__fastcall*)(void* self, void* edx, PanelNode* list);

bool StrIs(const EngineString& s, const char* lit) {
    const size_t n = std::strlen(lit);
    return s.begin != nullptr && s.end >= s.begin && static_cast<size_t>(s.end - s.begin) == n &&
           std::memcmp(s.begin, lit, n) == 0;
}

/// Значение переписываем в его же буфере (новое короче — память движка не выделяем и не освобождаем).
void SetInPlace(EngineString& s, const char* lit) {
    const size_t n = std::strlen(lit);
    if (s.begin == nullptr || s.cap < s.begin || static_cast<size_t>(s.cap - s.begin) < n + 1) return;
    std::memcpy(s.begin, lit, n);
    s.begin[n] = '\0';
    s.end = s.begin + n;
}

void PatchPanelValues(PanelNode* head) {
    __try {
        int guard = 0;
        for (PanelNode* n = head->next; n != nullptr && n != head && guard < 64; n = n->next, ++guard) {
            if (StrIs(n->key, "Master")) {
                SetInPlace(n->value, "EV");
            } else if (StrIs(n->key, "GameState")) {
                SetInPlace(n->value, StrIs(n->value, "OFFLINE") ? "Offline" : "Online");
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __fastcall PanelDetour(void* self, void* edx, PanelNode* list) {
    (void)edx;
    reinterpret_cast<PanelFn>(kPanelFn)(self, nullptr, list);
    if (list != nullptr) PatchPanelValues(list);
}

// ── установка ────────────────────────────────────────────────────────────────────────────────────────────────────

bool EnvIs(const char* name, const char* value) {
    char v[16]{};
    const DWORD n = ::GetEnvironmentVariableA(name, v, sizeof(v));
    return n > 0 && n < sizeof(v) && _stricmp(v, value) == 0;
}

void InstallServerWindow() {
    if (EnvIs("EVCORE_WINDOW", "0")) {
        FileLog("EVCORE_WINDOW=0 — окно сервера не трогаем");
        return;
    }
    if (EnvIs("EVCORE_CONSOLE_ENGINE", "all")) g_filter = EngineFilter::All;
    if (EnvIs("EVCORE_CONSOLE_ENGINE", "none")) g_filter = EngineFilter::None;

    // правило 18: все места сверяются до первой записи; не совпало хоть одно — не пишем ничего
    unsigned char thunk[5]{}, sink[5]{};
    uintptr_t slot = 0;
    const bool okThunk = ReadOwn(kLogThunk, thunk, 5) && std::memcmp(thunk, kLogThunkCode, 5) == 0;
    const bool okSink = ReadOwn(kLogSink, sink, 5) && std::memcmp(sink, kLogSinkCode, 5) == 0;
    const bool okSlot = ReadOwn(kPanelSlot, &slot, 4) && slot == kPanelFn;
    FileLog("сверка: переходник журнала 0x%08X %s, приёмник 0x%08X %s, слот панели 0x%08X %s (=0x%08X)",
            static_cast<unsigned>(kLogThunk), okThunk ? "ок" : "НЕ совпал", static_cast<unsigned>(kLogSink),
            okSink ? "ок" : "НЕ совпал", static_cast<unsigned>(kPanelSlot), okSlot ? "ок" : "НЕ совпал",
            static_cast<unsigned>(slot));
    if (!okThunk || !okSink || !okSlot) {
        FileLog("окно сервера: не все места совпали с bf3_server_Final.exe — ничего не меняем");
        return;
    }
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(&LogSinkDetour) - (kLogThunk + 5));
    const uintptr_t panel = reinterpret_cast<uintptr_t>(&PanelDetour);
    const bool w1 = WriteOwn(kLogThunk + 1, &rel, 4);
    const bool w2 = WriteOwn(kPanelSlot, &panel, 4);
    g_consoleHooked = w1;
    FileLog("окно сервера: консоль %s, панель %s; движковые строки: %s", w1 ? "перехвачена" : "НЕ перехвачена",
            w2 ? "перехвачена" : "НЕ перехвачена",
            g_filter == EngineFilter::All ? "все" : g_filter == EngineFilter::Warn ? "Warning и выше" : "скрыты");
}

DWORD WINAPI StartThread(LPVOID) {
    if (ProcessIs("bf3_server_Final.exe")) {
        InstallServerWindow();
        ConsoleWrite("info", "EV-Core", "Initializing the EV core v1...");
        ConsoleWrite("info", "EV-Core", "Local mode: EV backend (LAN), no online checks.");
        // К4: канал NetEvents, как Harmony у VU («Monitored Harmony server listening on 0.0.0.0:7948»)
        const int port = evcore::NetServerStart(-1);
        if (port > 0) {
            char line[96];
            snprintf(line, sizeof(line), "Harmony server listening on 0.0.0.0:%d.", port);
            ConsoleWrite("info", "EV-Core", line);
        } else {
            ConsoleWrite("error", "EV-Core", "Harmony server failed to start - NetEvents to clients will not work.");
        }
    } else {
        FileLog("клиентский процесс — пока ничего не делаем (хуки клиента переедут сюда из EVBots.dll)");
    }
    return 0;
}

}  // namespace

// ── экспорт для EVBots.dll (как GetConsoleLogger у vu-core) ───────────────────────────────────────────────────────

/// Строка в консоль окна сервера: level — "info" | "warning" | "error", source — "EVBots" / имя мода, text — UTF-8.
extern "C" __declspec(dllexport) void __cdecl EvCoreConsoleWrite(const char* level, const char* source,
                                                                  const char* text) {
    ConsoleWrite(level, source, text);
}

// 4 — очередь отправки канала (25.10); 5 — перехваты и правка памяти движка перенесены из EVBots.dll (сборка 237, 26.10);
// 6 — окно игры и Raw Input меню (EvWin.cpp), отказ на относительных переходах в прологе, обработчик CRT (сборка 238)
// 7: verified external CALL rel32 relocation; internal calls/branches remain rejected.
extern "C" __declspec(dllexport) int __cdecl EvCoreVersion() { return 7; }

// Сборка 237: неверный параметр CRT — строка в журнал вместо мгновенного завершения процесса (fail-fast), как в EVBots.dll.
static void __cdecl CrtInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    static volatile LONG s_n = 0;
    if (InterlockedIncrement(&s_n) <= 50) FileLog("CRT: неверный параметр — вызов вернул ошибку, процесс не завершён");
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = instance;
        ::DisableThreadLibraryCalls(instance);
        _set_invalid_parameter_handler(&CrtInvalidParameter);
        // Указатели на наш код остаются в движке (переходник журнала, vtable панели) — DLL не выгружаем.
        HMODULE self = nullptr;
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                             reinterpret_cast<LPCWSTR>(&DllMain), &self);
        InitLogPath();
        FileLog("==== ev-core.dll %d загружен, PID %lu ====", EvCoreVersion(), ::GetCurrentProcessId());
        // Сборка 238: частота 30/60/120 Гц как у VU — сервер ещё подвешен загрузчиком, перехваты встают до таймеров движка
        evcore::FreqInit();
        HANDLE t = ::CreateThread(nullptr, 0, StartThread, nullptr, 0, nullptr);
        if (t != nullptr) ::CloseHandle(t);
    }
    return TRUE;
}
