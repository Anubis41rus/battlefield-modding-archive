// EVBotsLog.cpp

#include "EVBotsLog.h"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace evbots {

HMODULE gModule = nullptr;

namespace {

FILE* g_file = nullptr;
bool g_ready = false;
bool g_ready_failed = false;

// ★ Сборка 167: запись лога — в отдельном потоке. Прогон 166: NativeBridge.TestLog стоял 3,8 с (строка уходила в окно
// консоли сервера: fputs+fflush в CONOUT$ блокируется, пока в окне выделен текст или консоль занята) — поток Lua стоял,
// все боты замирали. Теперь LogWrite только кладёт строку в буфер; файл и консоль пишет поток-писатель.
// ERR — файл сразу (при падении строка не должна потеряться), консоль — всё равно из писателя.
SRWLOCK g_qLock = SRWLOCK_INIT;
char* g_qFile = nullptr;   // накопленный текст для файла
size_t g_qFileLen = 0, g_qFileCap = 0;
char* g_qCon = nullptr;    // накопленный текст для консоли
size_t g_qConLen = 0, g_qConCap = 0;
volatile LONG g_qDropped = 0;
volatile LONG g_writerStarted = 0;
HANDLE g_qEvent = nullptr;
CRITICAL_SECTION g_fileCs;  // сам вывод в файл (писатель и синхронный ERR)
volatile LONG g_fileCsInit = 0;
const size_t kQueueMax = 8u * 1024u * 1024u;

// ★ Сборка 206: логи модов — logs\<мод>.log. Очередь и файл на каждый мод; файл открывает поток-писатель.
char g_logDir[512] = {};
struct ModLog {
    char name[64];
    FILE* file;
    bool failed;
    char* q;
    size_t len, cap;
};
ModLog g_mods[8] = {};
int g_modsN = 0;

bool QAppend(char*& buf, size_t& len, size_t& cap, const char* s, size_t n) {
    if (len + n > kQueueMax) return false;
    if (len + n > cap) {
        size_t nc = cap ? cap * 2 : 64 * 1024;
        while (nc < len + n) nc *= 2;
        char* nb = (char*)realloc(buf, nc);
        if (nb == nullptr) return false;
        buf = nb;
        cap = nc;
    }
    memcpy(buf + len, s, n);
    len += n;
    return true;
}

/// Создаёт каталог вместе со всеми родителями.
///
/// CreateDirectoryA создаёт ТОЛЬКО один уровень: если папки «EVBots» нет,
/// то «EVBots\logs» не создастся, fopen вернёт ошибку, и лог молча
/// останется пустым. Именно так лог и потерялся при первом запуске.
bool MakeDirs(const char* path) {
    char buf[512];
    const size_t len = strlen(path);
    if (len == 0 || len >= sizeof(buf)) {
        return false;
    }

    memcpy(buf, path, len + 1);

    // проходим по всем слешам и создаём накопленное
    for (char* p = buf + 1; *p != 0; ++p) {
        if (*p != '\\' && *p != '/') {
            continue;
        }

        const char sep = *p;
        *p = '\0';
        CreateDirectoryA(buf, nullptr);
        *p = sep;
    }

    // и последний компонент тоже — цикл выше остановился на предпоследнем
    // слеше, поэтому сам каталог из paths не был создан
    CreateDirectoryA(buf, nullptr);

    const DWORD attrs = GetFileAttributesA(buf);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

}  // namespace

/// Значимая строка попадает и в консоль игрового сервера. Инициализация
/// библиотек (~40 строк «подключено») остаётся только в файле — иначе
/// консоль bf3_server_Final снова утонет в шуме, и Lua-статуса в ней не
/// найти (ровно та жалоба, с которой всё началось).
bool IsStdoutNotable(const char* level, const char* message) {
    if (strcmp(level, "WARN") == 0 || strcmp(level, "ERR ") == 0) {
        return true;
    }

    static const char* kMarkers[] = {
        "LUA-MODE READY",      "Lua ready",        "mod system init",
        "found mods",          "mod '",            "engine ",
        "natives found",       "hooks installed",  "loaded into process",
        "BF3 server detected", "auto-start",       "hook",
        "target ",             "onNewRound",       "new round",
        "Engine:Update timer", "test live player", "bot",
        "Bot",                 "unloaded",         "shutdown",
        "Lua closed",          "ran ",
        "heartbeat",           "Level:Loaded",      "[VeniceEXT]",
        "Loading VeniceEXT module", "Successfully loaded",
    };

    for (const char* marker : kMarkers) {
        if (strstr(message, marker) != nullptr) {
            return true;
        }
    }
    return false;
}

/// Дублирует строку в консоль игрового сервера. Без консоли (процесс без
/// окна) — молчит, пишем только в файл.
///
/// ВАЖНО: у bf3_server_Final stdout перенаправлен в dedicated.log (pipe), а
/// консольное окно он строит сам через AllocConsole. Поэтому пишем не в
/// stdout, а прямо в CONOUT$ — это всегда окно сервера.
void ConsoleEcho(const char* line) {
    static bool logged = false;
    if (GetConsoleWindow() == nullptr) {
        if (!logged) {
            logged = true;
            LogWrite("DBG", "console echo OFF: no console window");
        }
        return;
    }

    static FILE* con = nullptr;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (fopen_s(&con, "CONOUT$", "w") != 0 || con == nullptr) {
            con = nullptr;
            LogWrite("DBG", "console echo OFF: CONOUT$ open failed, using stdout");
        } else {
            LogWrite("DBG", "console echo ON: writing to CONOUT$");
        }
    }

    if (con != nullptr) {
        fputs(line, con);
        fflush(con);
        return;
    }

    // обычная консоль без перенаправления — пишем через stdout
    fputs(line, stdout);
    fflush(stdout);
}

void EnsureFileCs() {
    if (InterlockedCompareExchange(&g_fileCsInit, 1, 0) == 0) {
        InitializeCriticalSectionAndSpinCount(&g_fileCs, 1000);
        InterlockedExchange(&g_fileCsInit, 2);
    }
    while (g_fileCsInit != 2) SwitchToThread();
}

/// Забирает накопленное и пишет: файл (withFile — только под g_fileCs) и/или консоль (только поток-писатель).
void DrainLocked(bool withFile, bool withConsole) {
    char* f = nullptr;
    size_t fl = 0;
    char* c = nullptr;
    size_t cl = 0;
    AcquireSRWLockExclusive(&g_qLock);
    if (withFile && g_qFileLen != 0) {
        f = (char*)malloc(g_qFileLen);
        if (f != nullptr) {
            memcpy(f, g_qFile, g_qFileLen);
            fl = g_qFileLen;
            g_qFileLen = 0;
        }
    }
    if (withConsole && g_qConLen != 0) {
        c = (char*)malloc(g_qConLen + 1);
        if (c != nullptr) {
            memcpy(c, g_qCon, g_qConLen);
            c[g_qConLen] = '\0';
            cl = g_qConLen;
            g_qConLen = 0;
        }
    }
    ReleaseSRWLockExclusive(&g_qLock);
    if (f != nullptr) {
        if (g_file != nullptr) {
            fwrite(f, 1, fl, g_file);
            fflush(g_file);
        }
        free(f);
    }
    if (c != nullptr) {
        (void)cl;
        ConsoleEcho(c);
        free(c);
    }
}

void RotateIfBig(const char* file, const char* dir, const char* base);

// ★ Сборка 215 (К3): клиент (bf3.exe) и сервер грузят одну EVBots.dll из <комплект>\activation — у клиента свои файлы
// (EVBots-client.log, <мод>-client.log), иначе два процесса пишут в один журнал, а run\status.ps1 / tests.ps1 читают
// EVBots.log как журнал сервера.
const char* ProcLogSuffix() {
    static const char* s = (GetModuleHandleA("bf3.exe") != nullptr && GetModuleHandleA("bf3_server_Final.exe") == nullptr)
                               ? "-client"
                               : "";
    return s;
}

/// ★ Сборка 206: забирает очереди модов и пишет в logs\<мод>.log. Только под g_fileCs.
void DrainModsLocked() {
    for (int i = 0; i < g_modsN; ++i) {
        ModLog& m = g_mods[i];
        char* buf = nullptr;
        size_t n = 0;
        AcquireSRWLockExclusive(&g_qLock);
        if (m.len != 0) {
            buf = (char*)malloc(m.len);
            if (buf != nullptr) {
                memcpy(buf, m.q, m.len);
                n = m.len;
                m.len = 0;
            }
        }
        ReleaseSRWLockExclusive(&g_qLock);
        if (buf == nullptr) continue;
        if (m.file == nullptr && !m.failed && g_logDir[0] != 0) {
            char path[700];
            char base[96];
            snprintf(base, sizeof(base), "%s%s", m.name, ProcLogSuffix());
            snprintf(path, sizeof(path), "%s\\%s.log", g_logDir, base);
            RotateIfBig(path, g_logDir, base);
            m.file = fopen(path, "ab");
            if (m.file == nullptr) {
                m.failed = true;
            } else {
                SYSTEMTIME st{};
                GetLocalTime(&st);
                fprintf(m.file, "\r\n\r\n==== %s: вывод мода, EVBots %04u-%02u-%02u %02u:%02u:%02u (PID %lu) ====\r\n", m.name,
                        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, GetCurrentProcessId());
            }
        }
        if (m.file != nullptr) {
            fwrite(buf, 1, n, m.file);
            fflush(m.file);
        }
        free(buf);
    }
}

DWORD WINAPI LogWriterThread(LPVOID) {
    for (;;) {
        WaitForSingleObject(g_qEvent, 50);
        EnsureFileCs();
        EnterCriticalSection(&g_fileCs);
        DrainLocked(true, false);
        DrainModsLocked();
        LeaveCriticalSection(&g_fileCs);
        DrainLocked(false, true);  // консоль — вне лока файла: её запись может встать надолго
        const LONG dropped = InterlockedExchange(&g_qDropped, 0);
        if (dropped != 0 && g_file != nullptr) {
            fprintf(g_file, "[лог] переполнение очереди: потеряно строк %ld\r\n", dropped);
            fflush(g_file);
        }
    }
}

void StartWriterOnce() {
    if (InterlockedCompareExchange(&g_writerStarted, 1, 0) != 0) return;
    g_qEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HANDLE h = CreateThread(nullptr, 0, LogWriterThread, nullptr, 0, nullptr);
    if (h != nullptr) {
        CloseHandle(h);
    } else {
        InterlockedExchange(&g_writerStarted, 3);  // писателя нет — пишем синхронно
    }
}

static bool LogVerbose();

/// ★ Сборка 167: большой лог (прогон 166: 78 МБ) — при запуске переименовать в evbots-<дата-время>.log (не удаляем).
void RotateIfBig(const char* file, const char* dir, const char* base) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExA(file, GetFileExInfoStandard, &fa)) return;
    const unsigned long long size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    // ★ Сборка 238 (пользователь: «старых логов полно»): раньше — 64 МБ и новый файл с датой на каждую ротацию, без удаления
    // (за неделю ~600 МБ). Теперь 32 МБ и один предыдущий файл <имя>-prev.log: на журнал не больше ~64 МБ.
    // ★ 267 (установщик раздаётся друзьям): тихий журнал ротируется на 8 МБ — с одним -prev не больше ~16 МБ на процесс.
    const unsigned long long limit = (LogVerbose() ? 32ULL : 8ULL) * 1024ULL * 1024ULL;
    if (size < limit) return;
    char to[700];
    snprintf(to, sizeof(to), "%s\\%s-prev.log", dir, base);
    DeleteFileA(to);
    MoveFileA(file, to);
}

void LogInit() {
    if (g_ready || g_ready_failed) {
        return;
    }

    // Пишем в корень комплекта: stand\logs\EVBots.log.
    // Каталог считаем ОТ МЕСТА самой DLL (GetModuleFileNameW на gModule),
    // а не от текущего каталога процесса: сервер/тест запускаются из разных
    // мест, а lог должен переживать любой запуск. DLL лежит в
    // <комплект>\activation\, поэтому поднимаемся на один уровень вверх.
    wchar_t dllPath[MAX_PATH]{};
    if (gModule == nullptr || GetModuleFileNameW(gModule, dllPath, MAX_PATH) == 0) {
        g_ready_failed = true;
        return;
    }

    // отрезаем имя файла DLL
    wchar_t* lastSlash = nullptr;
    for (wchar_t* p = dllPath; *p != 0; ++p) {
        if (*p == L'\\' || *p == L'/') {
            lastSlash = p;
        }
    }

    char dir[512];
    if (lastSlash != nullptr) {
        *lastSlash = L'\0';  // <комплект>\activation

        // запоминаем конец актуального корня (после ...\activation)
        wchar_t* actSlash = nullptr;
        for (wchar_t* p = dllPath; *p != 0; ++p) {
            if (*p == L'\\' || *p == L'/') {
                actSlash = p;
            }
        }
        if (actSlash != nullptr) {
            *actSlash = L'\0';  // <комплект>
        }

        snprintf(dir, sizeof(dir), "%S\\logs", dllPath);
    } else {
        // необычный случай: никакого пути — полагаемся на рабочий каталог
        snprintf(dir, sizeof(dir), "logs");
    }

    if (!MakeDirs(dir)) {
        g_ready_failed = true;
        OutputDebugStringA(
            "EVBots: cannot create log directory, file logging disabled\r\n");
        return;
    }

    char file[600];
    char base[32];
    snprintf(base, sizeof(base), "EVBots%s", ProcLogSuffix());
    snprintf(file, sizeof(file), "%s\\%s.log", dir, base);
    strcpy_s(g_logDir, dir);

    RotateIfBig(file, dir, base);
    g_file = fopen(file, "ab");
    if (g_file == nullptr) {
        g_ready_failed = true;
        OutputDebugStringA(
            "EVBots: cannot open log file, logging disabled\r\n");
        return;
    }

    g_ready = true;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    fprintf(g_file,
            "\r\n\r\n==== EVBots loaded %04u-%02u-%02u %02u:%02u:%02u "
            "(PID %lu) ====\r\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
            GetCurrentProcessId());
    fflush(g_file);
}

void LogClose() {
    // писатель мог быть уже снят системой (выход процесса) — забираем остаток сами, лок только «попробовать»
    if (g_fileCsInit == 2 && TryEnterCriticalSection(&g_fileCs)) {
        DrainLocked(true, false);
        DrainModsLocked();
        for (int i = 0; i < g_modsN; ++i) {
            if (g_mods[i].file != nullptr) {
                fclose(g_mods[i].file);
                g_mods[i].file = nullptr;
            }
        }
        LeaveCriticalSection(&g_fileCs);
    }
    if (g_file != nullptr) {
        ConsoleEcho("==== EVBots unloaded ====\r\n");
        fprintf(g_file, "==== EVBots unloaded ====\r\n");
        fflush(g_file);
        fclose(g_file);
        g_file = nullptr;
    }

    g_ready = false;
    g_ready_failed = false;
}

// ★ Сборка 211: тихий журнал по умолчанию (просьба пользователя 22.10: «у друга много логов — убрать лишние»). Пишутся
// предупреждения и ошибки, а из INFO/DBG — только важное: раунды и уровни, вход людей, загрузка модов, отмеченное ★, падения.
// Периодическая диагностика («ДИАГ …», ИКОНКИ, ПЕРКИ, СЧЁТ, TRACE, КУРС, SpawnPick) не пишется. Подробный журнал (стенд) —
// EVBOTS_LOG_VERBOSE=1 (ставит stand\run\common.ps1).
static bool LogVerbose() {
    static int s_v = -1;
    if (s_v < 0) {
        char v[4] = {};
        const DWORD n = ::GetEnvironmentVariableA("EVBOTS_LOG_VERBOSE", v, (DWORD)sizeof(v));
        s_v = (n == 1 && v[0] == '1') ? 1 : 0;
        // ★ 242: или файл-флаг activation\flags\EVBOTS_LOG_VERBOSE рядом с DLL (сервер из лаунчера — без переменных стенда)
        HMODULE self = nullptr;
        char path[MAX_PATH] = {};
        if (s_v == 0 &&
            ::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCSTR>(&LogVerbose), &self) &&
            ::GetModuleFileNameA(self, path, MAX_PATH) > 0) {
            char* slash = std::strrchr(path, '\\');
            if (slash != nullptr && (size_t)(slash - path) + 32 < MAX_PATH) {
                std::strcpy(slash + 1, "flags\\EVBOTS_LOG_VERBOSE");
                s_v = ::GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES ? 1 : 0;
            }
        }
    }
    return s_v == 1;
}

static bool QuietDrop(const char* level, const char* message) {
    if (LogVerbose() || level == nullptr || message == nullptr) return false;
    // ★ 256 (комплект другу 31.10: «только самые нужные логи»): предупреждения, которые идут на каждом спавне и ошибкой не
    // являются, — промах поиска ассета (мод так перебирает варианты внешности/кита, как VU — nil) и таймер выбора мода.
    if (level[0] == 'W' && (strstr(message, "не найдено в индексе EBX") != nullptr ||
                            strstr(message, "выбор мода не пришёл") != nullptr))
        return true;
    if (level[0] == 'W' || level[0] == 'E') return false;  // WARN / WRN / ERR — всегда
    if (strstr(message, "ДИАГ вход") != nullptr) return false;
    // ★ 258 (комплект другу 03.11: «боты после захвата всех точек перестают воевать», «вечная загрузка» у гостя): в тихом
    // журнале — сводки по точкам, целям мода, бою и стрельбе (раз в 10–30 с, ≈0,4 МБ/ч) и сторож/пульс клиента.
    static const char* kDiagKeep[] = {"ФЛАГИ:", "ДИАГ цели", "ДИАГ стрельба", "БОЙ 10с", "ДИАГ движение:", "флаг ",
                                      "LifeCounter:BaseDestroyed", "TankSuperiority0:",
                                      "ScoringSystem:StatEvent 86 ", "ScoringSystem:StatEvent 87 ",
                                      "диагностика Bot:ShootAt", "лучи: bot-bot", "клиент: пульс", "клиент: загрузка",
                                      "КЛИЕНТ ЗАВИС", "клиент: кадр ожил"};
    for (const char* k : kDiagKeep) {
        if (strstr(message, k) != nullptr) return false;
    }
    // ★ 251 (комплект другу 29.10: «только то, что нужно от крашей»): периодические профили тика (20 с), строки клиента «★ …»
    // раз в 10 с и контроль стека — только в подробном журнале. «ДИАГ неверный путь» тоже (причина пилотов закрыта в 250).
    static const char* kDrop[] = {"ДИАГ", "ИКОНКИ", "ПЕРКИ", "СЧЁТ", "TRACE", "КУРС", "SpawnPick", "ЖИЗНЬ", "ПАДЕНИЕ",
                                  "ПАРАШЮТ", "СПАВН проверка", "БОЙ 10с", "Lua-профиль", "API-профиль", "строки Lua",
                                  "тик моста", "★ клиент: игроки", "★ клиент: лучи", "контрольный луч", "★ клиент: список ботов",
                                  "★ сообщение движка", "стек Lua на входе", "лучи за 20 с", "зарегистрирована команда",
                                  // 256: повторы на каждом спавне/команде мода (попадали по «человек»/«RCON»)
                                  "conn=", "слотов человека", "БЛИЗКО", "SpmProbe", "RCON: в процессе", "ОПЫТ ввод",
                                  "ВВОД: свой объект", "ВЫБОР бота", "АССЕТ «",
                                  // 267: самотесты раунда нужны стенду, в комплекте это сотни строк на прогон.
                                  // Итог «TESTS-DONE» остаётся: у него нет пробела после «TEST».
                                  "TEST "};
    for (const char* d : kDrop) {
        if (strstr(message, d) != nullptr) return true;
    }
    static const char* kKeep[] = {"★", "LUA-MODE READY", "loaded into process", "unloaded", "РАУНД", "УРОВЕНЬ", "Level:Loaded",
                                  "Level:Destroy", "ворота спавна", "выгружает уровень", "человек", "ЧЕЛОВЕК", "уровень для модов",
                                  "уровень и сущности", "TEST", "VEH", "PURE CALL", "исключение", "heartbeat", "fun-bots",
                                  "mod '", "Successfully loaded", "RCON", "кит +0x480"};
    for (const char* k : kKeep) {
        if (strstr(message, k) != nullptr) return false;
    }
    return true;
}

// ★ Сборка 267 (комплект раздают дальше): периодическая диагностика в тихом журнале — не чаще раза в 5 минут на вид.
// События (флаг взят, зависание/оживание клиента, вход человека) идут как шли; стенд (verbose) не затронут.
static bool QuietThrottle(const char* message) {
    struct Entry {
        const char* prefix;
        DWORD last;
    };
    static Entry s_entries[] = {
        {"ФЛАГИ:", 0},          {"ДИАГ цели", 0},      {"ДИАГ стрельба", 0},      {"БОЙ 10с", 0},
        {"ДИАГ движение:", 0},  {"клиент: пульс", 0},  {"диагностика Bot:ShootAt", 0}, {"лучи: bot-bot", 0},
    };
    const DWORD now = GetTickCount();
    for (Entry& e : s_entries) {
        if (strstr(message, e.prefix) == nullptr) continue;
        if (e.last != 0 && now - e.last < 5u * 60u * 1000u) return true;
        e.last = now;
        return false;
    }
    return false;
}

// ★ Сборка 251: предел журнала на один запуск процесса. 29.10 клиент 248 за 1 ч 15 мин записал 3,2 ГБ (вечный цикл traceback,
// исправлен в 249); ротация в RotateIfBig — только при запуске. Сверх предела — только ошибки/предупреждения, с двойного —
// ничего, одной строкой отметка в файле. Подробный журнал (стенд) — прежние 256 МБ; ★ 267: тихий — 8 МБ на запуск (было 32).
namespace {
volatile LONG64 g_sessBytes = 0;
volatile LONG g_capNoted = 0;
}  // namespace
static bool CapDrop(const char* level, size_t n) {
    const LONG64 soft = LogVerbose() ? 256LL * 1024LL * 1024LL : 8LL * 1024LL * 1024LL;
    const LONG64 now = InterlockedExchangeAdd64(&g_sessBytes, (LONG64)n) + (LONG64)n;
    if (now <= soft) return false;
    if (now <= soft * 2 && level != nullptr && (level[0] == 'W' || level[0] == 'E')) return false;
    if (InterlockedExchange(&g_capNoted, 1) == 0 && g_file != nullptr) {
        // одна строка, мимо очереди — дальше журнал этого запуска пишет только ошибки (или молчит после двойного предела)
        EnsureFileCs();
        EnterCriticalSection(&g_fileCs);
        fputs("[лог] предел журнала на запуск: дальше только ошибки и предупреждения\r\n", g_file);
        fflush(g_file);
        LeaveCriticalSection(&g_fileCs);
    }
    return true;
}

void LogWrite(const char* level, const char* message) {
    if (!g_ready) {
        LogInit();
    }
    if (QuietDrop(level, message)) return;
    if (message != nullptr && !LogVerbose() && QuietThrottle(message)) return;
    if (CapDrop(level, message != nullptr ? strlen(message) + 24 : 24)) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);

    char line[1024];
    snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] %s %s\r\n", st.wHour,
             st.wMinute, st.wSecond, st.wMilliseconds, level, message);

    // отладочный вывод — только под отладчиком (без него это лишний системный вызов на строку)
    if (IsDebuggerPresent()) OutputDebugStringA(line);

    StartWriterOnce();
    const bool notable = IsStdoutNotable(level, message);
    if (g_writerStarted == 3) {  // писателя нет — как раньше, синхронно
        if (g_file != nullptr) {
            fputs(line, g_file);
            fflush(g_file);
        }
        if (notable) ConsoleEcho(line);
        return;
    }
    const size_t n = strlen(line);
    AcquireSRWLockExclusive(&g_qLock);
    const bool okF = QAppend(g_qFile, g_qFileLen, g_qFileCap, line, n);
    // значимые строки — ещё и в консоль игрового сервера, чтобы статус Lua/модов/раундов был виден там же,
    // где лог bf3_server_Final
    if (notable) QAppend(g_qCon, g_qConLen, g_qConCap, line, n);
    ReleaseSRWLockExclusive(&g_qLock);
    if (!okF) InterlockedIncrement(&g_qDropped);
    if (strcmp(level, "ERR ") == 0) {
        // ошибка (в т.ч. VEH перед падением) — в файл сразу; лок файла ждём не дольше ~50 мс
        EnsureFileCs();
        for (int i = 0; i < 50; ++i) {
            if (TryEnterCriticalSection(&g_fileCs)) {
                DrainLocked(true, false);
                LeaveCriticalSection(&g_fileCs);
                break;
            }
            Sleep(1);
        }
    }
    if (g_qEvent != nullptr) SetEvent(g_qEvent);
}

void LogPrintf(const char* level, const char* fmt, ...) {
    char buf[900];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    LogWrite(level, buf);
}

// ★ Сборка 217: консоль окна сервера — через ev-core.dll (аналог vu-core). ev-core грузится загрузчиком раньше нас.
namespace {
using EvCoreConsoleWriteFn = void(__cdecl*)(const char*, const char*, const char*);
EvCoreConsoleWriteFn EvCoreConsole() {
    static EvCoreConsoleWriteFn fn = [] {
        HMODULE core = ::GetModuleHandleA("ev-core.dll");
        return core != nullptr
                   ? reinterpret_cast<EvCoreConsoleWriteFn>(::GetProcAddress(core, "EvCoreConsoleWrite"))
                   : nullptr;
    }();
    return fn;
}
}  // namespace

void ConsoleWrite(const char* level, const char* source, const char* text, bool alsoLog) {
    if (text == nullptr) return;
    if (alsoLog) LogPrintf("INFO", "★ консоль [%s] %s", source != nullptr ? source : "", text);
    if (EvCoreConsoleWriteFn fn = EvCoreConsole()) fn(level, source, text);
}

void ConsolePrintf(const char* level, const char* source, const char* fmt, ...) {
    char buf[900];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    ConsoleWrite(level, source, buf, true);
}

// ★ Сборка 139: тестовые строки диагностики — в основной лог (как раньше), уровень INFO.
namespace {
struct EnvCacheEntry {
    char name[64];
    char value[64];
    DWORD len;
};
SRWLOCK g_envLock = SRWLOCK_INIT;
EnvCacheEntry g_env[96];
int g_envN = 0;
}  // namespace

DWORD EnvGetCached(const char* name, char* buf, DWORD cap) {
    if (buf != nullptr && cap > 0) buf[0] = '\0';
    if (name == nullptr || strlen(name) >= sizeof(g_env[0].name)) return ::GetEnvironmentVariableA(name, buf, cap);
    AcquireSRWLockShared(&g_envLock);
    for (int i = 0; i < g_envN; ++i) {
        if (strcmp(g_env[i].name, name) == 0) {
            const DWORD len = g_env[i].len;
            if (buf != nullptr && cap > 0 && len < cap) memcpy(buf, g_env[i].value, len + 1);
            ReleaseSRWLockShared(&g_envLock);
            return len;
        }
    }
    ReleaseSRWLockShared(&g_envLock);
    char v[64] = {};
    DWORD len = ::GetEnvironmentVariableA(name, v, (DWORD)sizeof(v));
    if (len >= sizeof(v)) return ::GetEnvironmentVariableA(name, buf, cap);  // длинное значение — без кэша
    AcquireSRWLockExclusive(&g_envLock);
    if (g_envN < (int)(sizeof(g_env) / sizeof(g_env[0]))) {
        EnvCacheEntry& e = g_env[g_envN];
        strcpy_s(e.name, name);
        memcpy(e.value, v, len + 1);
        e.len = len;
        ++g_envN;
    }
    ReleaseSRWLockExclusive(&g_envLock);
    if (buf != nullptr && cap > 0 && len < cap) memcpy(buf, v, len + 1);
    return len;
}

void LogModWrite(const char* mod, const char* level, const char* message) {
    // имя файла — только безопасные символы; пустое имя — в общий лог, как раньше
    char name[64] = {};
    size_t k = 0;
    for (const char* p = mod; p != nullptr && *p != 0 && k + 1 < sizeof(name); ++p) {
        const char ch = *p;
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
                        ch == '_' || ch == '.';
        name[k++] = ok ? ch : '_';
    }
    if (k == 0 || strcmp(name, "EVBots") == 0) {
        LogPrintf(level, "[VeniceEXT] %s", message);
        return;
    }
    if (!g_ready) LogInit();
    // ★ 251: тихий журнал (комплект друга) — от мода в файл только предупреждения и ошибки; общий предел запуска
    if (!LogVerbose() && level != nullptr && level[0] != 'W' && level[0] != 'E') return;
    if (CapDrop(level, message != nullptr ? strlen(message) + 40 : 40)) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[1024];
    snprintf(line, sizeof(line), "[%02u:%02u:%02u.%03u] %s [VeniceEXT] [%s] %s\r\n", st.wHour, st.wMinute, st.wSecond,
             st.wMilliseconds, level, name, message);
    const size_t n = strlen(line);
    StartWriterOnce();
    bool added = false, first = false;
    AcquireSRWLockExclusive(&g_qLock);
    ModLog* m = nullptr;
    for (int i = 0; i < g_modsN; ++i) {
        if (strcmp(g_mods[i].name, name) == 0) m = &g_mods[i];
    }
    if (m == nullptr && g_modsN < (int)(sizeof(g_mods) / sizeof(g_mods[0]))) {
        m = &g_mods[g_modsN++];
        strcpy_s(m->name, name);
        first = true;
    }
    if (m != nullptr) added = QAppend(m->q, m->len, m->cap, line, n);
    // в консоль сервера — как раньше (у VU вывод мода виден в консоли сервера)
    QAppend(g_qCon, g_qConLen, g_qConCap, line, n);
    ReleaseSRWLockExclusive(&g_qLock);
    if (first) LogPrintf("INFO", "EVBots: вывод мода %s -> logs\\%s%s.log", name, name, ProcLogSuffix());
    if (m == nullptr) {
        LogPrintf(level, "[VeniceEXT] [%s] %s", name, message);  // больше 8 модов — в общий лог
        return;
    }
    if (!added) InterlockedIncrement(&g_qDropped);
    if (g_writerStarted == 3) {  // писателя нет — синхронно
        EnsureFileCs();
        EnterCriticalSection(&g_fileCs);
        DrainModsLocked();
        LeaveCriticalSection(&g_fileCs);
    }
    if (g_qEvent != nullptr) SetEvent(g_qEvent);
}

void LogTestWrite(const char* message) {
    LogWrite("INFO ", message);
}

void LogTestPrintf(const char* fmt, ...) {
    char buf[900];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    LogTestWrite(buf);
}

}  // namespace evbots
