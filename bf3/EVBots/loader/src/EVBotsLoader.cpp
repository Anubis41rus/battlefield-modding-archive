// EVBotsLoader.cpp — загрузчик EVBots: запускает bf3.exe / bf3_server_Final.exe подвешенным, внедряет наши DLL
// (LoadLibraryW удалённым потоком, по одной, по порядку) и отпускает главный поток.
//
// Заменяет activation-loader.exe старого комплекта (BF3LegacyLan/BF3NativeBots, исходников нет). Поведение снято
// дизассемблером старого загрузчика 23.10.2026 (MODLOG «Свой загрузчик EVBots») и повторено, кроме двух отличий:
//   1) DLL можно несколько (--dll повторяется). Клиенту первой всегда идёт activation.dll (обход DRM, подмена DNS),
//      потом указанные --dll. Старый внедрял ровно одну, и --dll вытеснял activation.dll;
//   2) у сервера по умолчанию EVBots.dll, а не botlayer.dll (нативные боты старого проекта не используются).
// Остальное как у старого: окружение для activation.dll (BF3ACT_*), каталог logs\ рядом с загрузчиком,
// отказ работать с правами администратора, сторож клиента (--guard), аргументы игры — после `--` без изменений.
//
// Весь код x86: bf3.exe 32-битный, адрес LoadLibraryW берётся из своего kernel32 (у WOW64-процессов база та же).

#include <windows.h>
#include <tlhelp32.h>

#pragma comment(lib, "user32.lib")  // AllowSetForegroundWindow (239)

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitNotFound = 3;
constexpr int kExitCreate = 4;
constexpr int kExitInject = 5;

constexpr DWORD kInjectWaitMs = 60000;

// ── вывод ─────────────────────────────────────────────────────────────────────

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

void Say(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    fputs("evbots-loader: ", stdout);
    fputs(buf, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

// Журнал сторожа: «ЧЧ:ММ:СС.мсс [tid] текст», UTF-8, дописывается.
void LogLine(const std::wstring& path, const char* fmt, ...) {
    if (path.empty()) return;
    char text[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    char line[1200];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%02u:%02u:%02u.%03u [%lu] %s\r\n", t.wHour, t.wMinute, t.wSecond,
                t.wMilliseconds, GetCurrentThreadId(), text);
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(h, line, (DWORD)strlen(line), &wr, nullptr);
    CloseHandle(h);
}

// ── пути ──────────────────────────────────────────────────────────────────────

// ★ 267 (комплект раздают дальше): журнал загрузчика/сторожа — один файл вместо файла на каждый запуск (в установке
// накопилось 77 activation-* и 38 guard-* по 4 КБ). Прежний уходит в <имя>-prev.log; если файл занят другим процессом,
// MoveFileEx не сработает — просто дописываем в текущий.
void RotateLog(const std::wstring& path) {
    if (path.size() <= 4) return;
    const std::wstring prev = path.substr(0, path.size() - 4) + L"-prev.log";
    MoveFileExW(path.c_str(), prev.c_str(), MOVEFILE_REPLACE_EXISTING);
}

std::wstring SelfPath() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return (n == 0 || n >= MAX_PATH) ? std::wstring() : std::wstring(buf, n);
}

std::wstring DirOf(const std::wstring& p) {
    const size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? std::wstring(L".") : p.substr(0, k);
}

std::wstring FullPath(const std::wstring& p) {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetFullPathNameW(p.c_str(), MAX_PATH, buf, nullptr);
    return (n == 0 || n >= MAX_PATH) ? p : std::wstring(buf, n);
}

bool IsAbsolute(const std::wstring& p) {
    return (p.size() > 2 && p[1] == L':') || (p.size() > 1 && p[0] == L'\\' && p[1] == L'\\');
}

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool SameText(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), (int)a.size(), b.c_str(), (int)b.size(), TRUE) == CSTR_EQUAL;
}

bool StartsWithI(const wchar_t* s, const wchar_t* prefix) {
    const int n = (int)wcslen(prefix);
    return (int)wcslen(s) >= n && CompareStringOrdinal(s, n, prefix, n, TRUE) == CSTR_EQUAL;
}

// ── командная строка ──────────────────────────────────────────────────────────

// Хвост исходной командной строки после первого отдельного токена `--` (кавычки игры сохраняются как есть).
// found = false, если `--` нет.
std::wstring RawTailAfterDashDash(bool& found) {
    const wchar_t* s = GetCommandLineW();
    found = false;
    bool quoted = false;
    const wchar_t* tokStart = nullptr;
    for (const wchar_t* p = s;; ++p) {
        const wchar_t c = *p;
        const bool end = (c == 0) || (!quoted && (c == L' ' || c == L'\t'));
        if (!end && tokStart == nullptr) tokStart = p;
        if (c == L'"') quoted = !quoted;
        if (end && tokStart != nullptr) {
            if (p - tokStart == 2 && tokStart[0] == L'-' && tokStart[1] == L'-') {
                found = true;
                const wchar_t* t = p;
                while (*t == L' ' || *t == L'\t') ++t;
                return std::wstring(t);
            }
            tokStart = nullptr;
        }
        if (c == 0) break;
    }
    return std::wstring();
}

// ── права ─────────────────────────────────────────────────────────────────────

bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION e = {};
    DWORD n = 0;
    const BOOL ok = GetTokenInformation(tok, TokenElevation, &e, sizeof(e), &n);
    CloseHandle(tok);
    return ok && e.TokenIsElevated != 0;
}

// ── окружение для activation.dll ──────────────────────────────────────────────

const wchar_t* const kOwnVars[] = {L"EALaunchCode=", L"ContentId=",  L"BF3ACT_MODE=",
                                   L"BF3ACT_LOG=",   L"BF3ACT_DRM=", L"BF3ACT_DLF_SRC="};

// Как у старого загрузчика: свои переменные первыми, из родительского окружения эти же имена вычищаются.
std::vector<wchar_t> BuildEnv(bool client, const std::wstring& log, const std::wstring& drm, const std::wstring& dlf) {
    std::vector<std::wstring> own;
    if (client) {
        own.push_back(L"EALaunchCode=1");
        own.push_back(L"ContentId=71067");
    }
    own.push_back(std::wstring(L"BF3ACT_MODE=") + (client ? L"client" : L"dedicated"));
    own.push_back(L"BF3ACT_LOG=" + log);
    own.push_back(L"BF3ACT_DRM=" + drm);
    if (!dlf.empty()) own.push_back(L"BF3ACT_DLF_SRC=" + dlf);

    std::vector<wchar_t> block;
    for (const auto& v : own) {
        block.insert(block.end(), v.begin(), v.end());
        block.push_back(0);
    }
    wchar_t* env = GetEnvironmentStringsW();
    if (env != nullptr) {
        for (const wchar_t* p = env; *p != 0; p += wcslen(p) + 1) {
            bool skip = false;
            for (const wchar_t* pre : kOwnVars) {
                if (StartsWithI(p, pre)) skip = true;
            }
            if (skip) continue;
            block.insert(block.end(), p, p + wcslen(p) + 1);
        }
        FreeEnvironmentStringsW(env);
    }
    block.push_back(0);
    return block;
}

// ── внедрение ─────────────────────────────────────────────────────────────────

bool InjectOne(HANDLE proc, const std::wstring& dll, DWORD& moduleLow) {
    moduleLow = 0;
    const SIZE_T bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote == nullptr) return false;
    bool ok = false;
    if (WriteProcessMemory(proc, remote, dll.c_str(), bytes, nullptr)) {
        auto fn = reinterpret_cast<LPTHREAD_START_ROUTINE>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
        HANDLE th = fn ? CreateRemoteThread(proc, nullptr, 0, fn, remote, 0, nullptr) : nullptr;
        if (th != nullptr) {
            if (WaitForSingleObject(th, kInjectWaitMs) == WAIT_OBJECT_0 && GetExitCodeThread(th, &moduleLow)) {
                ok = moduleLow != 0;
            }
            CloseHandle(th);
        }
    }
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    return ok;
}

// ── сторож клиента ────────────────────────────────────────────────────────────
// Пока жив клиент, снимает <игра>\Core\activation.exe (окно активации EA). Выходит, когда bf3.exe и
// EACoreServer.exe ушли и прошло grace мс, или по потолку max мс.

std::wstring ImagePath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (h == nullptr) return std::wstring();
    wchar_t buf[MAX_PATH];
    DWORD n = MAX_PATH;
    std::wstring r;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) r.assign(buf, n);
    CloseHandle(h);
    return r;
}

int RunGuard(const std::wstring& game, DWORD bf3Pid, const std::wstring& log, DWORD pollMs, DWORD graceMs,
             DWORD maxMs) {
    const std::wstring coreAct = FullPath(game + L"\\Core\\activation.exe");
    const std::wstring coreEa = FullPath(game + L"\\Core\\EACoreServer.exe");
    LogLine(log, "сторож: старт game=%s bf3_pid=%lu poll=%lu grace=%lu max=%lu", Utf8(game).c_str(), bf3Pid, pollMs,
            graceMs, maxMs);
    const ULONGLONG start = GetTickCount64();
    ULONGLONG goneSince = 0;
    int killed = 0;
    for (;;) {
        bool bf3Alive = false, eaAlive = false;
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe = {};
            pe.dwSize = sizeof(pe);
            for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
                if (pe.th32ProcessID == bf3Pid) bf3Alive = true;
                if (_wcsicmp(pe.szExeFile, L"activation.exe") == 0) {
                    const std::wstring img = ImagePath(pe.th32ProcessID);
                    if (!img.empty() && SameText(img, coreAct)) {
                        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                        if (h != nullptr) {
                            if (TerminateProcess(h, 0)) {
                                ++killed;
                                LogLine(log, "сторож: завершён activation.exe pid=%lu путь=%s", pe.th32ProcessID,
                                        Utf8(img).c_str());
                            }
                            CloseHandle(h);
                        }
                    }
                } else if (_wcsicmp(pe.szExeFile, L"EACoreServer.exe") == 0) {
                    const std::wstring img = ImagePath(pe.th32ProcessID);
                    if (img.empty() || SameText(img, coreEa)) eaAlive = true;
                }
            }
            CloseHandle(snap);
        }
        const ULONGLONG now = GetTickCount64();
        if (bf3Alive || eaAlive) {
            goneSince = 0;
        } else if (goneSince == 0) {
            goneSince = now;
        } else if (now - goneSince >= graceMs) {
            LogLine(log, "сторож: bf3.exe и EACoreServer.exe ушли + grace %lu мс, выход (убито всего %d)", graceMs,
                    killed);
            return kExitOk;
        }
        if (now - start >= maxMs) {
            LogLine(log, "сторож: потолок %lu мс истёк, выход (убито всего %d)", maxMs, killed);
            return kExitOk;
        }
        // ★ 239: снимок всех процессов раз в 50 мс все 20 минут — ~10 % ядра на FX-4300 (прогон 28.10). activation.exe
        // игра запускает на старте (проверка лицензии, ~3 с после правок кода, MODLOG 238-4) — часто опрашиваем первые
        // 3 минуты, дальше раз в 500 мс.
        Sleep(now - start < 180000 ? pollMs : (pollMs > 500 ? pollMs : 500));
    }
}

void StartGuard(const std::wstring& self, const std::wstring& game, DWORD bf3Pid, const std::wstring& logDir) {
    const std::wstring log = logDir + L"\\guard.log";
    RotateLog(log);
    std::wstring cmd = L"\"" + self + L"\" --guard --game \"" + game + L"\" --bf3-pid " + std::to_wstring(bf3Pid) +
                       L" --log \"" + log + L"\"";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Say("guard up pid=%lu log=%s", pi.dwProcessId, Utf8(log).c_str());
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else {
        Say("could not start the guard (%lu); running without it", GetLastError());
    }
}

// ── разбор аргументов ─────────────────────────────────────────────────────────

void Usage() {
    fputs(
        "usage: evbots-loader (--client|--dedicated) --game <dir> [--exe <name>] [--dll <name>]... [--no-activation]\n"
        "                     [--wait] [--log <file>] [--drm-bypass|--drm-strict] [--license-artifact <path>]\n"
        "                     [--no-activation-guard] [--] <game args...>\n"
        "  client: activation.dll is injected first (unless --no-activation), then every --dll in order.\n"
        "  dedicated: every --dll in order; default EVBots.dll.\n"
        "  DLL names are relative to the loader folder unless absolute.\n",
        stdout);
}

DWORD ToDword(const wchar_t* s) { return (DWORD)wcstoul(s, nullptr, 10); }

int GuardMain(int argc, wchar_t** argv) {
    std::wstring game, log;
    DWORD pid = 0, poll = 50, grace = 4000, maxMs = 1200000;
    for (int i = 2; i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool hasVal = i + 1 < argc;
        if (a == L"--game" && hasVal) game = argv[++i];
        else if (a == L"--bf3-pid" && hasVal) pid = ToDword(argv[++i]);
        else if (a == L"--log" && hasVal) log = argv[++i];
        else if (a == L"--poll-ms" && hasVal) poll = ToDword(argv[++i]);
        else if (a == L"--grace-ms" && hasVal) grace = ToDword(argv[++i]);
        else if (a == L"--max-ms" && hasVal) maxMs = ToDword(argv[++i]);
        else {
            Say("--guard: unexpected arg near %s", Utf8(a).c_str());
            return kExitUsage;
        }
    }
    if (game.empty() || pid == 0) {
        Say("--guard needs --game <dir> and --bf3-pid <pid>");
        return kExitUsage;
    }
    return RunGuard(game, pid, log, poll == 0 ? 50 : poll, grace, maxMs);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 2 && wcscmp(argv[1], L"--guard") == 0) return GuardMain(argc, argv);

    if (IsElevated()) {
        Say("refusing to run elevated; bf3.exe does not start under an admin token. Run without elevation "
            "(from an admin session: explorer.exe <script>).");
        return kExitUsage;
    }

    int mode = 0;  // 1 = dedicated, 2 = client
    std::wstring game, exe, log, drm = L"0", dlf;
    std::vector<std::wstring> dlls;
    bool wait = false, noGuard = false, noActivation = false, drmFlag = false, sawDashDash = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        const bool hasVal = i + 1 < argc;
        if (a == L"--") { sawDashDash = true; break; }
        if (a == L"--client") mode = 2;
        else if (a == L"--dedicated") mode = 1;
        else if (a == L"--game" && hasVal) game = argv[++i];
        else if (a == L"--exe" && hasVal) exe = argv[++i];
        else if (a == L"--dll" && hasVal) dlls.push_back(argv[++i]);
        else if (a == L"--wait") wait = true;
        else if (a == L"--log" && hasVal) log = argv[++i];
        else if (a == L"--drm-bypass") { drm = L"1"; drmFlag = true; }
        else if (a == L"--drm-strict") { drm = L"strict"; drmFlag = true; }
        else if (a == L"--license-artifact" && hasVal) dlf = argv[++i];
        else if (a == L"--no-activation-guard") noGuard = true;
        else if (a == L"--no-activation") noActivation = true;
        else if (a == L"-h" || a == L"--help") { Usage(); return kExitOk; }
        else {
            Say("unexpected arg near %s; put game arguments after `--` (quoting is preserved only there)",
                Utf8(a).c_str());
            return kExitUsage;
        }
    }
    if (mode == 0 || game.empty()) {
        Usage();
        return kExitUsage;
    }
    const bool client = mode == 2;
    if (!dlf.empty() && drm != L"strict") {
        Say("--license-artifact requires --drm-strict (activation.dll reads it only under strict mode)");
        return kExitUsage;
    }
    if (!client && (drmFlag || !dlf.empty())) {
        Say("%s belongs to the retail bf3.exe activation layer; the dedicated server has no DRM",
            drmFlag ? "--drm-bypass/--drm-strict" : "--license-artifact");
        return kExitUsage;
    }

    const std::wstring self = SelfPath();
    if (self.empty()) {
        Say("GetModuleFileName failed");
        return kExitNotFound;
    }
    const std::wstring here = DirOf(self);
    game = FullPath(game);
    if (exe.empty()) exe = client ? L"bf3.exe" : L"bf3_server_Final.exe";
    const std::wstring exePath = game + L"\\" + exe;
    if (!FileExists(exePath)) {
        Say("%s not found in %s", Utf8(exe).c_str(), Utf8(game).c_str());
        return kExitNotFound;
    }

    // Список DLL: клиенту — activation.dll первой; серверу по умолчанию EVBots.dll.
    std::vector<std::wstring> inject;
    if (client && !noActivation) inject.push_back(L"activation.dll");
    if (dlls.empty() && !client) dlls.push_back(L"EVBots.dll");
    for (const auto& d : dlls) {
        bool dup = false;
        for (const auto& x : inject) dup = dup || SameText(x, d);
        if (!dup) inject.push_back(d);
    }
    for (auto& d : inject) {
        d = IsAbsolute(d) ? d : here + L"\\" + d;
        if (!FileExists(d)) {
            Say("%s not next to the loader (%s)", Utf8(d).c_str(), Utf8(here).c_str());
            return kExitNotFound;
        }
    }

    const std::wstring logDir = here + L"\\logs";
    CreateDirectoryW(logDir.c_str(), nullptr);
    if (log.empty()) {
        log = logDir + L"\\activation.log";
        RotateLog(log);
    } else {
        log = FullPath(log);
    }
    if (!dlf.empty()) dlf = FullPath(dlf);

    bool hasTail = false;
    const std::wstring tail = RawTailAfterDashDash(hasTail);
    (void)sawDashDash;
    std::wstring cmd = L"\"" + exePath + L"\"";
    if (hasTail && !tail.empty()) cmd += L" " + tail;
    if (cmd.size() >= 32000) {
        Say("game command line longer than 32000 chars");
        return kExitUsage;
    }

    std::vector<wchar_t> env = BuildEnv(client, log, drm, dlf);
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exePath.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT,
                        env.data(), game.c_str(), &si, &pi)) {
        Say("CreateProcess failed (%lu)", GetLastError());
        return kExitCreate;
    }
    // ★ 239: игра — потомок фонового загрузчика и не может стать передним окном; полноэкранное окно без фокуса
    // сворачивается, клиент не грузится, пока его не развернуть (прогоны 28.10 — каждый запуск из лаунчера). Загрузчик
    // запущен лаунчером по клику и сам такое право имеет — передаём его игре.
    if (!AllowSetForegroundWindow(pi.dwProcessId)) Say("AllowSetForegroundWindow: %lu (не критично)", GetLastError());

    std::string names;
    for (const auto& d : inject) {
        DWORD mod = 0;
        if (!InjectOne(pi.hProcess, d, mod)) {
            Say("injection of %s failed - terminating %s instead of running it without our layer", Utf8(d).c_str(),
                Utf8(exe).c_str());
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return kExitInject;
        }
        Say("injected %s (module 0x%08lX)", Utf8(d).c_str(), mod);
        if (!names.empty()) names += ",";
        names += Utf8(d.substr(d.find_last_of(L"\\/") + 1));
    }

    if (client && !noGuard) StartGuard(self, game, pi.dwProcessId, logDir);

    if (ResumeThread(pi.hThread) == (DWORD)-1) {
        Say("ResumeThread failed (%lu)", GetLastError());
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return kExitInject;
    }
    Say("started %s pid=%lu mode=%s inject=ok dlls=%s log=%s", Utf8(exe).c_str(), pi.dwProcessId,
        client ? "client" : "dedicated", names.c_str(), Utf8(log).c_str());

    if (wait) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        if (GetExitCodeProcess(pi.hProcess, &code)) Say("%s exited with code %lu", Utf8(exe).c_str(), code);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return kExitOk;
}
