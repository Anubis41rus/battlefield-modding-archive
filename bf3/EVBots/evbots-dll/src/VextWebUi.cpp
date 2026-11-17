// VextWebUi.cpp — см. VextWebUi.h. Проба движка вне игры — EVBots-dev\webui-probe (MODLOG «Меню F12 — проба А»),
// проверка без игры — webui-probe\build\overlay-test.exe.
//
// Потоки: Lua (кадр движка) кладёт команды в очередь и будит поток WebUI сообщением окну; поток WebUI (STA, своя очередь
// сообщений — её требует WebView2) создаёт фреймы и исполняет команды; вызовы страниц копятся в другой очереди, кадр
// движка забирает их (WebUiDrain) и рассылает в Lua как события.
// Окно-оверлей: WS_POPUP, владелец — окно игры (над ним по z-порядку, сворачивается с ним), WS_EX_NOREDIRECTIONBITMAP —
// своей картинки нет, рисуют только визуалы DirectComposition фреймов, фон прозрачный попиксельно. Пока ни один фрейм не
// забрал мышь/клавиатуру — WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE: мышь и фокус идут игре.

#include "VextWebUi.h"

#include <windows.h>
#include <shlwapi.h>
#include <wrl.h>

#pragma warning(push, 0)
#include <dcomp.h>

#include "WebView2.h"
#include "WebView2EnvironmentOptions.h"
#pragma warning(pop)

#include <atomic>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "EVBotsLog.h"
#include "GameAssetFiles.h"
#include "InlineHook.h"
#include "LuaRuntime.h"
#include "Vuic.h"
#include "WebUiPresent.h"

#include <wincodec.h>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace evbots {
namespace {

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

constexpr UINT kTimerFollow = 1;
constexpr UINT kTimerCapture = 2;  // ★ 231: снимок страницы для меню в кадре игры
constexpr UINT kMsgQuit = WM_APP + 1;
constexpr UINT kMsgCmd = WM_APP + 2;
constexpr UINT kMsgNativeMouse = WM_APP + 3;

struct Frame {
    std::wstring modFolder;
    std::string modName;
    std::wstring origin;  // https://<мод>.webui/
    vuic::Archive vuic;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<ICoreWebView2CompositionController> comp;
    ComPtr<ICoreWebView2Controller> ctl;
    ComPtr<ICoreWebView2> web;
    bool creating = false;
    bool loaded = false;
    bool visible = false;
    int mouseOverride = 0;  // 0: follow native UI, 1: EnableMouse, -1: DisableMouse; ResetMouse restores 0.
    bool keyboard = false;
    std::vector<std::wstring> pendingJs;  // ExecuteJS до загрузки страницы — исполнить после
    int served = 0, missing = 0, external = 0, calls = 0;
    int layerId = 0;         // ★ 231: слой меню в кадре игры (WebUiPresent)
    bool capturing = false;  // ★ 231: CapturePreview в пути
    bool layerShown = false;
    // ★ 239: страница была полностью прозрачной (меню закрыто) — следующий снимок не раньше этого срока (GetTickCount64)
    ULONGLONG idleUntil = 0;
    bool loggedShot = false;
};

struct Cmd {
    std::wstring mod;
    std::string op;
    std::string arg;
};

struct State {
    HWND game = nullptr;
    HWND overlay = nullptr;
    bool interactive = false;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> root;
    ComPtr<ICoreWebView2Environment> env;
    bool envCreating = false;
    std::wstring dataDir;
    std::vector<std::unique_ptr<Frame>> frames;  // порядок = z-порядок (последний — сверху)
};

State* g_s = nullptr;
std::thread g_thread;
std::mutex g_lifecycleGate;
bool g_stopping = false;  // под g_lifecycleGate; ожидание потока идёт без этого mutex
std::atomic<bool> g_running{false};
std::atomic<HWND> g_overlay{nullptr};
std::atomic<int> g_screenW{0}, g_screenH{0};
std::atomic<bool> g_keysCaptured{false};   // ★ 229: клавиатуру забрал фрейм (для InputManager, WM_INPUT игры)
std::atomic<bool> g_mouseCaptured{false};  // ★ 229: мышь забрал фрейм (курсор и WM_INPUT игры глушатся)
std::atomic<bool> g_nativeMouseEnabled{false};
std::atomic<HWND> g_gameWnd{nullptr};
std::atomic<long> g_rawMouseDropped{0}, g_rawKeysDropped{0}, g_cursorCallsBlocked{0};

std::mutex g_cmdGate;
std::deque<Cmd> g_cmds;
std::mutex g_callGate;
std::vector<WebUiCall> g_calls;

void ClearSessionQueues() {
    {
        std::lock_guard<std::mutex> lock(g_cmdGate);
        g_cmds.clear();
    }
    {
        std::lock_guard<std::mutex> lock(g_callGate);
        g_calls.clear();
    }
}

std::string Utf8(const wchar_t* w) {
    if (w == nullptr) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 1) ::WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    if (n > 0) s.resize(n - 1);
    return s;
}

std::wstring Wide(const std::string& s) {
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 1) ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    if (n > 0) w.resize(n - 1);
    return w;
}

const wchar_t* Mime(const std::string& path) {
    static const std::map<std::string, const wchar_t*> m = {
        {".html", L"text/html; charset=utf-8"}, {".js", L"text/javascript; charset=utf-8"},
        {".css", L"text/css; charset=utf-8"},  {".png", L"image/png"},
        {".svg", L"image/svg+xml"},            {".ttf", L"font/ttf"},
        {".json", L"application/json"},        {".woff2", L"font/woff2"}};
    const std::string p = path.substr(0, path.find_first_of("?#"));
    const size_t dot = p.rfind('.');
    if (dot != std::string::npos) {
        std::string ext = p.substr(dot);
        for (char& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        auto it = m.find(ext);
        if (it != m.end()) return it->second;
    }
    return L"application/octet-stream";
}

/// Первый строковый элемент JSON-массива (имя операции WebUI.Call). Пусто — не строка.
std::string FirstString(const std::string& json) {
    size_t i = json.find('[');
    if (i == std::string::npos) return {};
    ++i;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    if (i >= json.size() || json[i] != '"') return {};
    std::string out;
    for (++i; i < json.size() && json[i] != '"'; ++i) {
        if (json[i] == '\\' && i + 1 < json.size()) ++i;
        out.push_back(json[i]);
    }
    return out;
}

// WebUI у VU: страница зовёт WebUI.Call(операция, …). Стиль «как Gameface» (реестр В7): Gameface не рисует маркеры списков
// (проба А) и полосы прокрутки документа. ★ 229: окно-диалог не выше экрана, длинный список вкладки настроек прокручивается
// внутри — прогон 228: при окне 1280×720 диалог SETTINGS (центр по top:50% + translate) был выше экрана, вкладки уходили за
// верхний край. Файлы мода не меняются — стиль добавляется в документ.
constexpr wchar_t kShim[] =
    L"window.WebUI = window.WebUI || { Call: function() {"
    L"  try { window.chrome.webview.postMessage(JSON.stringify(Array.prototype.slice.call(arguments))); } catch (e) {}"
    L"} };"
    L"window.WebUI.GameAssetUrl = function(path) {"
    L" return location.origin + '/__fb/' + String(path).replace(/^fb:\\/\\//i, '').toLowerCase() + '.png';"
    L"};"
    L"window.addEventListener('error', function(e) {"
    L"  window.chrome.webview.postMessage(JSON.stringify(['__js_error', String(e.message), String(e.filename), e.lineno]));"
    L"});"
    L"document.addEventListener('DOMContentLoaded', function() {"
    L"  var s = document.createElement('style'); s.textContent = 'ul, ol { list-style: none; }"
    L" html, body { overflow: hidden !important; }"
    L" ::-webkit-scrollbar { width: 8px; height: 8px; } ::-webkit-scrollbar-track { background: transparent; }"
    L" ::-webkit-scrollbar-thumb { background: rgba(255, 255, 255, 0.3); }"
    L" ui-view[data-type=\"dialog\"] { max-height: 100vh; max-width: 100vw; }"
    L" ui-view[data-name=\"settings\"] ui-tabs nav ul { flex-wrap: wrap; }"
    L" ui-view[data-name=\"settings\"] ui-tabs nav ul li label { min-width: 0; padding: 5px 10px; }"
    L" ui-view[data-name=\"settings\"] ui-tabs figure { max-height: calc(100vh - 300px); overflow-y: auto;"
    L" overflow-x: hidden; }';"
    L"  document.head.insertBefore(s, document.head.firstChild);"
    L"});";

/// Окно игры: самое большое видимое окно верхнего уровня нашего процесса (не наше).
HWND FindGameWindow() {
    struct Ctx {
        DWORD pid;
        HWND best;
        long area;
    } ctx{::GetCurrentProcessId(), nullptr, 0};
    ::EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            auto* c = reinterpret_cast<Ctx*>(p);
            DWORD pid = 0;
            ::GetWindowThreadProcessId(h, &pid);
            if (pid != c->pid || !::IsWindowVisible(h) || ::GetWindow(h, GW_OWNER) != nullptr) return TRUE;
            wchar_t cls[64];
            ::GetClassNameW(h, cls, 64);
            if (wcscmp(cls, L"EVBotsWebUi") == 0) return TRUE;
            RECT r;
            ::GetClientRect(h, &r);
            const long a = (r.right - r.left) * (r.bottom - r.top);
            if (a > c->area) {
                c->area = a;
                c->best = h;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&ctx));
    return ctx.area >= 320 * 240 ? ctx.best : nullptr;
}

// ── ★ 229: ввод игры, пока фрейм держит мышь/клавиатуру (как у VU: игра в это время не крутит камеру и не прячет курсор) ──
// Импорт zbf3: мышь — Raw Input (RegisterRawInputDevices, GetRawInputData → WM_INPUT), курсор — ShowCursor, ClipCursor,
// SetCursorPos; у vu-core тоже RegisterRawInputDevices. DirectInput8 (DINPUT8) не трогаем — реестр В18.

// ★ 230: курсор — перехват в win32u.dll (NtUserClipCursor / NtUserSetCursorPos / NtUserShowCursor). В 229 подменяли
// таблицу импорта bf3.exe — не совпало ни одно место (прогон 229: «НЕТ» ×3; гипотеза — защита игры подставила свои
// переходники). user32!ClipCursor и т.д. в WOW64 — переходник `jmp [user32 → win32u]` (SysWOW64\user32.dll), поэтому
// перехват в win32u ловит вызов игры, как бы ни был связан её импорт. Пролог win32u: `mov eax, imm32; mov edx, imm32;
// call edx` — без относительных переходов, InlineHook переносит его как есть. VU (импорт vu-core) курсорных функций
// Windows не берёт — реестр В18.
using ClipCursorFn = BOOL(WINAPI*)(const RECT*);
using SetCursorPosFn = BOOL(WINAPI*)(int, int);
using ShowCursorFn = int(WINAPI*)(BOOL);
InlineHook g_hookClip, g_hookSetPos, g_hookShow;
thread_local int t_gameShowPending = 0;  // ShowCursor этого потока, пропущенные при меню: применить, когда меню отпустит

ClipCursorFn RealClip() {
    return g_hookClip.IsInstalled() ? reinterpret_cast<ClipCursorFn>(g_hookClip.Trampoline()) : &::ClipCursor;
}
SetCursorPosFn RealSetPos() {
    return g_hookSetPos.IsInstalled() ? reinterpret_cast<SetCursorPosFn>(g_hookSetPos.Trampoline()) : &::SetCursorPos;
}
ShowCursorFn RealShow() {
    return g_hookShow.IsInstalled() ? reinterpret_cast<ShowCursorFn>(g_hookShow.Trampoline()) : &::ShowCursor;
}

BOOL WINAPI HookClipCursor(const RECT* r) {
    if (g_mouseCaptured.load()) {
        ++g_cursorCallsBlocked;
        return RealClip()(nullptr);
    }
    return RealClip()(r);
}

BOOL WINAPI HookSetCursorPos(int x, int y) {
    if (g_mouseCaptured.load()) {
        ++g_cursorCallsBlocked;
        return TRUE;
    }
    return RealSetPos()(x, y);
}

int WINAPI HookShowCursor(BOOL show) {
    if (g_mouseCaptured.load()) {
        ++g_cursorCallsBlocked;
        t_gameShowPending += show ? 1 : -1;
        return show ? 0 : -1;  // «показан» / «скрыт» — циклы игры вида while (ShowCursor(..)) завершатся
    }
    for (; t_gameShowPending > 0; --t_gameShowPending) RealShow()(TRUE);  // пропущенное при меню — как хотела игра
    for (; t_gameShowPending < 0; ++t_gameShowPending) RealShow()(FALSE);
    return RealShow()(show);
}

bool WebUiTestModeLocal();

/// Курсор игры — в настоящем клиенте (bf3.exe) и в проверке без игры. EVBOTS_WEBUI_CURSOR=0 — не перехватывать.
void HookGameCursor() {
    if (::GetModuleHandleW(L"bf3.exe") == nullptr && !WebUiTestModeLocal()) return;
    wchar_t v[4] = L"";
    if (::GetEnvironmentVariableW(L"EVBOTS_WEBUI_CURSOR", v, 4) > 0 && v[0] == L'0') {
        LOG_INFO("EVBots: WebUI: курсор игры не перехватываем (EVBOTS_WEBUI_CURSOR=0)");
        return;
    }
    HMODULE w32u = ::GetModuleHandleW(L"win32u.dll");
    if (w32u == nullptr) {
        LOG_WARN("EVBots: WebUI: win32u.dll нет в процессе — курсор игры не перехвачен");
        return;
    }
    const HookResult a = g_hookClip.Install(reinterpret_cast<void*>(::GetProcAddress(w32u, "NtUserClipCursor")),
                                            reinterpret_cast<void*>(&HookClipCursor), "VextWebUi:g_hookClip");
    const HookResult b = g_hookSetPos.Install(reinterpret_cast<void*>(::GetProcAddress(w32u, "NtUserSetCursorPos")),
                                              reinterpret_cast<void*>(&HookSetCursorPos), "VextWebUi:g_hookSetPos");
    const HookResult c = g_hookShow.Install(reinterpret_cast<void*>(::GetProcAddress(w32u, "NtUserShowCursor")),
                                            reinterpret_cast<void*>(&HookShowCursor), "VextWebUi:g_hookShow");
    LOG_INFO("EVBots: WebUI: курсор игры — win32u: NtUserClipCursor %s, NtUserSetCursorPos %s, NtUserShowCursor %s",
             ToString(a), ToString(b), ToString(c));
}

/// ★ 230: курсор показан и не зажат, пока фрейм держит мышь — в потоке окна игры (кадр движка у BF3 в другом потоке,
/// прогон 229: 11824 против 9728, а счётчик ShowCursor у Windows свой у каждого потока). Отпустил — вернуть как было.
void SyncGameCursor() {
    thread_local int shown = 0;
    if (g_mouseCaptured.load()) {
        RealClip()(nullptr);
        // ShowCursor also increments an already-visible cursor. Count the
        // final successful show and acquire visibility only once per capture.
        if (shown == 0) {
            do {
                ++shown;
            } while (RealShow()(TRUE) < 0 && shown < 32);
        }
    } else {
        for (; shown > 0; --shown) RealShow()(FALSE);
    }
}

// ★ 231: мышь игры — Raw Input с RIDEV_NOLEGACY (факт, zbf3 FUN_00fc7700/FUN_00fc7830: usage 1/2, флаги 0x30, окно игры):
// Windows не шлёт окнам процесса WM_MOUSEMOVE и кнопки — меню их не получало (прогоны 229–230). В своём режиме курсора
// игра сама снимает регистрацию мыши (FUN_00fc7830: флаги RIDEV_REMOVE, окно NULL) — делаем так же, пока фрейм держит
// мышь, и возвращаем регистрацию игры, когда отпустит. Только в потоке окна игры. EVBOTS_WEBUI_RAWMOUSE=0 — не трогать.
// ★ Сборка 237: регистрацию Raw Input держит ядро (ev-core\src\EvWin.cpp, как vu-core у VU); здесь — когда и что с курсором.
std::atomic<long> g_menuMouseMsgs{0};

bool RawMouseSwitchOff() {
    wchar_t v[4] = L"";
    return ::GetEnvironmentVariableW(L"EVBOTS_WEBUI_RAWMOUSE", v, 4) > 0 && v[0] == L'0';
}

using CoreRawMouseFn = int(__cdecl*)();
CoreRawMouseFn CoreRawMouse(const char* name) { return reinterpret_cast<CoreRawMouseFn>(CoreExport(name)); }

void ReleaseGameRawMouse() {
    if (RawMouseSwitchOff()) return;
    static const CoreRawMouseFn release = CoreRawMouse("EvRawMouseRelease");
    if (release != nullptr) release();  // строка «снят на время меню» — в ev-core.log
}

void RestoreGameRawMouse() {
    static const CoreRawMouseFn restore = CoreRawMouse("EvRawMouseRestore");
    const int r = restore != nullptr ? restore() : 0;
    if (r == 0) return;  // не была снята
    const bool ok = r > 0;
    // ★ 232: игра была в бою (мышь — Raw Input с NOLEGACY, курсор скрыт), а её ShowCursor(FALSE) при меню мы пропустили —
    // курсор оставался по центру, пока игра снова не позовёт ShowCursor (жалоба пользователя 25.10, после Alt). Скрываем
    // сразу, в потоке окна игры; пропущенные вызовы этого потока — не нужны. Сначала снять свои показы (SyncGameCursor),
    // затем прятать видимый курсор только в боевом режиме. Каждый ShowCursor(FALSE) меняет счётчик, даже если
    // курсор уже скрыт. При переходе в штатное меню состояние курсора остаётся у движка.
    SyncGameCursor();
    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    const bool cursorKnown = ::GetCursorInfo(&ci) != FALSE;
    bool cursorVisible = cursorKnown && (ci.flags & CURSOR_SHOWING) != 0;
    if (cursorVisible && !g_nativeMouseEnabled.load()) {
        int left = 0;
        for (int i = 0; i < 32 && (left = RealShow()(FALSE)) >= 0; ++i) {
        }
        cursorVisible = left >= 0;
    }
    t_gameShowPending = 0;
    // ★ 233: в 232 счётчик опускали (журнал: −1/−2), а стрелка оставалась (скриншот пользователя 19:15): в бою игра прячет
    // курсор своим состоянием — FUN_00fc2480 (zbf3, факт): +0x72 показ, +0x64 курсор (в бою 0), затем PostMessage окну
    // игры 0x8006 и 0x8005 — оконная процедура игры применяет их (SetCursor). Без мышиных сообщений (NOLEGACY) игра сама
    // не перерисует курсор до Alt+Tab (FUN_00fc7a30: SetCursor при активации). Просим игру применить её же состояние.
    if (HWND g = g_gameWnd.load()) {
        ::PostMessageW(g, 0x8006, 0, 0);
        ::PostMessageW(g, 0x8005, 0, 0);
    }
    LOG_INFO("EVBots: WebUI: Raw Input мыши игры возвращён: %s; курсор %s, игре — 0x8006/0x8005; мышиных "
             "сообщений меню за показ %ld", ok ? "да" : "НЕТ", cursorKnown ? (cursorVisible ? "видим" : "скрыт") : "не прочитан",
             g_menuMouseMsgs.exchange(0));
}

/// Окно игры: при меню — Raw Input мыши/клавиатуры игре не отдаём, мышиные сообщения игры (если она держит захват мыши)
/// передаём меню. Работает в потоке окна игры. ★ Сборка 237: оконную процедуру игры подменяет ядро (EvWndSubclass,
/// ev-core\src\EvWin.cpp), это — обработчик: 1 — сообщение обработано (*res — ответ), 0 — отдать игре.
int __cdecl GameWndHandler(HWND h, UINT m, WPARAM w, LPARAM l, LRESULT* res) {
    const bool mouse = g_mouseCaptured.load(), keys = g_keysCaptured.load();
    thread_local bool lastMouse = false;
    if (mouse != lastMouse) {
        if (mouse)
            ReleaseGameRawMouse();
        else
            RestoreGameRawMouse();
    }
    if (mouse != lastMouse || (mouse && (m == WM_SETCURSOR || m == WM_MOUSEMOVE || m == WM_INPUT))) {
        lastMouse = mouse;
        SyncGameCursor();
    }
    if (m == WM_INPUT && (mouse || keys)) {
        RAWINPUTHEADER hdr{};
        UINT sz = sizeof(hdr);
        if (::GetRawInputData(reinterpret_cast<HRAWINPUT>(l), RID_HEADER, &hdr, &sz, sizeof(RAWINPUTHEADER)) == sizeof(hdr)) {
            if (hdr.dwType == RIM_TYPEMOUSE && mouse) {
                ++g_rawMouseDropped;
                ReleaseGameRawMouse();  // ★ 231: игра вернула регистрацию при открытом меню — снова снять
                *res = ::DefWindowProcW(h, m, w, l);
                return 1;
            }
            if (hdr.dwType == RIM_TYPEKEYBOARD && keys) {
                ++g_rawKeysDropped;
                *res = ::DefWindowProcW(h, m, w, l);
                return 1;
            }
        }
    }
    if (mouse && m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) {
        if (HWND o = g_overlay.load()) {
            ::PostMessageW(o, m, w, l);  // оверлей — дочернее окно в (0, 0): клиентские координаты совпадают
            *res = 0;
            return 1;
        }
    }
    return 0;  // игре
}

using CoreWndSubclassFn = int(__cdecl*)(HWND, int(__cdecl*)(HWND, UINT, WPARAM, LPARAM, LRESULT*));
using CoreWndUnsubclassFn = int(__cdecl*)(HWND);

/// ★ 229: оверлей — дочернее окно игры: координаты от угла клиентской области игры.
void Follow() {
    State& s = *g_s;
    if (!::IsWindow(s.game)) return;
    RECT r;
    ::GetClientRect(s.game, &r);
    const int w = r.right - r.left, h = r.bottom - r.top;
    g_screenW = w;
    g_screenH = h;
    RECT cur;
    ::GetClientRect(s.overlay, &cur);
    if (cur.right != w || cur.bottom != h) {
        ::SetWindowPos(s.overlay, HWND_TOP, 0, 0, w, h, SWP_NOACTIVATE);
        for (auto& f : s.frames)
            if (f->ctl) f->ctl->put_Bounds(RECT{0, 0, w, h});
    }
}

Frame* TopWith(bool Frame::*flag) {
    for (auto it = g_s->frames.rbegin(); it != g_s->frames.rend(); ++it)
        if ((*it)->visible && (*it)->loaded && (**it).*flag) return it->get();
    return nullptr;
}

Frame* MouseOwner() {
    if (!g_s) return nullptr;
    const bool native = g_nativeMouseEnabled.load();
    for (auto it = g_s->frames.rbegin(); it != g_s->frames.rend(); ++it) {
        Frame& f = **it;
        if (f.visible && f.loaded && (f.mouseOverride > 0 || (f.mouseOverride == 0 && native))) return &f;
    }
    return nullptr;
}

/// Кому идёт ввод: верхний видимый фрейм с мышью / клавиатурой. Никому — оверлей прозрачен для мыши, фокус у игры.
/// ★ 229: передний план НЕ меняется (прежде SetForegroundWindow(оверлей) — полноэкранная BF3 выпадала в окно, прогон 228):
/// оверлей — дочернее окно игры, фокус клавиатуры уходит в него без смены активного окна верхнего уровня. Мышь игры
/// (Raw Input) и курсор глушатся в окне игры (GameWndHandler) и в кадре движка (WebUiFrameTick), пока фрейм держит ввод.
void ApplyInput() {
    State& s = *g_s;
    bool anyVisible = false;
    for (auto& f : s.frames) anyVisible = anyVisible || f->visible;
    if (anyVisible && !::IsWindowVisible(s.overlay)) {
        Follow();
        ::ShowWindow(s.overlay, SW_SHOWNA);
    } else if (!anyVisible && ::IsWindowVisible(s.overlay)) {
        ::ShowWindow(s.overlay, SW_HIDE);
    }
    Frame* mouse = MouseOwner();
    Frame* keys = TopWith(&Frame::keyboard);
    g_keysCaptured = keys != nullptr;
    const bool hasMouse = mouse != nullptr;
    const bool mouseChanged = g_mouseCaptured.exchange(hasMouse) != hasMouse;
    if (mouseChanged && !hasMouse && ::GetCapture() == s.overlay) ::ReleaseCapture();
    const bool want = mouse != nullptr || keys != nullptr;
    if (want && !s.interactive) {
        ::EnableWindow(s.overlay, TRUE);
        s.interactive = true;
        LOG_INFO("EVBots: WebUI: ввод — меню (мышь %s, клавиатура %s)", mouse ? mouse->modName.c_str() : "-",
                 keys ? keys->modName.c_str() : "-");
    } else if (!want && s.interactive) {
        ::EnableWindow(s.overlay, FALSE);
        s.interactive = false;
        LOG_INFO("EVBots: WebUI: ввод — игре");
    }
    // Mouse capture can change while the overlay still owns the keyboard.
    if (mouseChanged) ::PostMessageW(s.game, WM_NULL, 0, 0);
    if (keys != nullptr) {
        if (::GetFocus() != s.overlay) ::SetFocus(s.overlay);  // очереди ввода связаны (дочернее окно) — можно из потока WebUI
        keys->ctl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    } else if (::GetFocus() == s.overlay || ::IsChild(s.overlay, ::GetFocus())) {
        ::SetFocus(s.game);
    }
}

void Serve(Frame& f, ICoreWebView2WebResourceRequestedEventArgs* args) {
    ComPtr<ICoreWebView2WebResourceRequest> req;
    args->get_Request(&req);
    LPWSTR uriW = nullptr;
    req->get_Uri(&uriW);
    const std::wstring uri = uriW ? uriW : L"";
    ::CoTaskMemFree(uriW);
    ComPtr<ICoreWebView2WebResourceResponse> resp;
    if (uri.rfind(f.origin, 0) != 0) {
        if (++f.external <= 20) LOG_WARN("EVBots: WebUI %s: внешний запрос заблокирован: %s", f.modName.c_str(),
                                         Utf8(uri.c_str()).c_str());
        g_s->env->CreateWebResourceResponse(nullptr, 403, L"Blocked", L"", &resp);
        args->put_Response(resp.Get());
        return;
    }
    const std::string path = Utf8(uri.c_str() + f.origin.size());
    std::string body, err;
    bool found = false;
    if (path.rfind("__fb/", 0) == 0) found = ReadCachedGameAsset(path, &body, &err);
    else {
        const int at = f.vuic.FindPath(path.empty() ? "index.html" : path);
        found = at >= 0 && f.vuic.Extract(static_cast<size_t>(at), &body, &err);
    }
    if (!found) {
        ++f.missing;
        if (path != "favicon.ico") LOG_WARN("EVBots: WebUI %s: ресурс не найден: %s %s", f.modName.c_str(), path.c_str(), err.c_str());
        g_s->env->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"", &resp);
    } else {
        ++f.served;
        IStream* st = ::SHCreateMemStream(reinterpret_cast<const BYTE*>(body.data()), static_cast<UINT>(body.size()));
        const std::wstring headers = std::wstring(L"Content-Type: ") + Mime(path);
        g_s->env->CreateWebResourceResponse(st, 200, L"OK", headers.c_str(), &resp);
        if (st) st->Release();
    }
    args->put_Response(resp.Get());
}

void ApplyOp(Frame& f, const std::string& op, const std::string& arg);

/// WebUI.Call страницы: события — в Lua, операции ввода/показа — сразу (vext\guides\webui.md).
void OnPageCall(Frame& f, const std::string& json) {
    const std::string op = FirstString(json);
    ++f.calls;
    if (op == "DispatchEvent" || op == "DispatchEventLocal") {
        std::lock_guard<std::mutex> lock(g_callGate);
        if (g_calls.size() < 4096) g_calls.push_back(WebUiCall{f.modFolder, json});
        return;
    }
    if (op == "__js_error") {
        LOG_WARN("EVBots: WebUI %s: ошибка JS: %s", f.modName.c_str(), json.c_str());
        return;
    }
    static const char* const kOps[] = {"Show", "Hide", "BringToFront", "SendToBack", "EnableKeyboard", "EnableMouse",
                                       "DisableKeyboard", "DisableMouse", "ResetKeyboard", "ResetMouse"};
    for (const char* k : kOps) {
        if (op == k) {
            ApplyOp(f, op, {});
            return;
        }
    }
    LOG_WARN("EVBots: WebUI %s: неизвестный WebUI.Call: %s", f.modName.c_str(), json.c_str());
}

void ExecJs(Frame& f, const std::wstring& js) {
    if (!f.loaded) {
        f.pendingJs.push_back(js);
        return;
    }
    f.web->ExecuteScript(js.c_str(), nullptr);
}

void Reorder(Frame& f, bool front) {
    State& s = *g_s;
    for (size_t i = 0; i < s.frames.size(); ++i) {
        if (s.frames[i].get() != &f) continue;
        auto keep = std::move(s.frames[i]);
        s.frames.erase(s.frames.begin() + static_cast<long>(i));
        if (front) s.frames.push_back(std::move(keep));
        else s.frames.insert(s.frames.begin(), std::move(keep));
        break;
    }
    if (f.visual) {
        s.root->RemoveVisual(f.visual.Get());
        s.root->AddVisual(f.visual.Get(), front ? TRUE : FALSE, nullptr);
        s.dcomp->Commit();
    }
}

void ApplyOp(Frame& f, const std::string& op, const std::string& arg) {
    f.idleUntil = 0;  // ★ 239: команда мода (ExecuteJS, Show, ввод) — страница могла измениться, снимок сразу
    if (op == "Show" || op == "Hide") {
        f.visible = op == "Show";
        if (f.ctl) f.ctl->put_IsVisible(f.visible ? TRUE : FALSE);
    } else if (op == "BringToFront" || op == "SendToBack") {
        Reorder(f, op == "BringToFront");
    } else if (op == "EnableKeyboard") {
        f.keyboard = true;
    } else if (op == "DisableKeyboard" || op == "ResetKeyboard") {
        f.keyboard = false;
    } else if (op == "EnableMouse") {
        f.mouseOverride = 1;
    } else if (op == "DisableMouse") {
        f.mouseOverride = -1;
    } else if (op == "ResetMouse") {
        f.mouseOverride = 0;
    } else if (op == "ExecuteJS") {
        ExecJs(f, Wide(arg));
        return;
    } else {
        return;
    }
    ApplyInput();
}

void OnFrameCreated(Frame& f, ICoreWebView2CompositionController* c) {
    State& s = *g_s;
    f.comp = c;
    f.comp.As(&f.ctl);
    f.ctl->get_CoreWebView2(&f.web);
    s.dcomp->CreateVisual(&f.visual);
    s.root->AddVisual(f.visual.Get(), TRUE, nullptr);
    f.comp->put_RootVisualTarget(f.visual.Get());
    s.dcomp->Commit();
    RECT r;
    ::GetClientRect(s.overlay, &r);
    f.ctl->put_Bounds(r);
    f.ctl->put_IsVisible(f.visible ? TRUE : FALSE);
    ComPtr<ICoreWebView2Controller2> c2;
    if (SUCCEEDED(f.ctl.As(&c2))) c2->put_DefaultBackgroundColor(COREWEBVIEW2_COLOR{0, 0, 0, 0});
    // Как Gameface: никаких браузерных клавиш и окон. F12 в WebView2 по умолчанию открывает DevTools (проверка без игры:
    // F12 в открытом меню fun-bots вывел окно Chrome_WidgetWin_1 на передний план), F5/Ctrl+R — перезагрузка.
    ComPtr<ICoreWebView2Settings> st;
    if (SUCCEEDED(f.web->get_Settings(&st))) {
        st->put_AreDevToolsEnabled(FALSE);
        st->put_AreDefaultContextMenusEnabled(FALSE);
        st->put_IsZoomControlEnabled(FALSE);
        st->put_IsStatusBarEnabled(FALSE);
        ComPtr<ICoreWebView2Settings3> st3;
        if (SUCCEEDED(st.As(&st3))) st3->put_AreBrowserAcceleratorKeysEnabled(FALSE);
    }
    Frame* fp = &f;
    EventRegistrationToken tok;
    f.web->AddWebResourceRequestedFilter(L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
    f.web->add_WebResourceRequested(Callback<ICoreWebView2WebResourceRequestedEventHandler>(
                                        [fp](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* a) -> HRESULT {
                                            Serve(*fp, a);
                                            return S_OK;
                                        }).Get(),
                                    &tok);
    f.web->add_WebMessageReceived(Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                                      [fp](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                                          LPWSTR m = nullptr;
                                          a->TryGetWebMessageAsString(&m);
                                          OnPageCall(*fp, Utf8(m));
                                          ::CoTaskMemFree(m);
                                          return S_OK;
                                      }).Get(),
                                  &tok);
    f.web->add_NavigationCompleted(Callback<ICoreWebView2NavigationCompletedEventHandler>(
                                       [fp](ICoreWebView2*, ICoreWebView2NavigationCompletedEventArgs* a) -> HRESULT {
                                           BOOL ok = FALSE;
                                           a->get_IsSuccess(&ok);
                                           fp->loaded = true;
                                           LOG_INFO("EVBots: WebUI %s: ★ страница загружена: %s (файлов %d), отложенных "
                                                    "ExecuteJS %zu",
                                                    fp->modName.c_str(), ok ? "да" : "нет", fp->served, fp->pendingJs.size());
                                           for (const auto& js : fp->pendingJs) fp->web->ExecuteScript(js.c_str(), nullptr);
                                           fp->pendingJs.clear();
                                           ApplyInput();
                                           return S_OK;
                                       }).Get(),
                                   &tok);
    f.comp->add_CursorChanged(Callback<ICoreWebView2CursorChangedEventHandler>(
                                  [](ICoreWebView2CompositionController* cc, IUnknown*) -> HRESULT {
                                      HCURSOR h = nullptr;
                                      if (SUCCEEDED(cc->get_Cursor(&h)) && h != nullptr) ::SetCursor(h);
                                      return S_OK;
                                  }).Get(),
                              &tok);
    f.web->AddScriptToExecuteOnDocumentCreated(kShim, nullptr);
    f.web->Navigate((f.origin + L"index.html").c_str());
}

void CreatePendingFrames() {
    State& s = *g_s;
    if (!s.env) return;
    ComPtr<ICoreWebView2Environment3> env3;
    if (FAILED(s.env.As(&env3))) {
        LOG_ERR("EVBots: WebUI: WebView2 Runtime без визуального хостинга (Environment3) — WebUI не будет");
        return;
    }
    for (auto& up : s.frames) {
        Frame* f = up.get();
        if (f->comp || f->creating) continue;
        f->creating = true;
        env3->CreateCoreWebView2CompositionController(
            s.overlay, Callback<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler>(
                           [f](HRESULT r, ICoreWebView2CompositionController* c) -> HRESULT {
                               f->creating = false;
                               if (FAILED(r) || c == nullptr) {
                                   LOG_ERR("EVBots: WebUI %s: фрейм WebView2 не создан (hr 0x%08lX)", f->modName.c_str(), r);
                                   return S_OK;
                               }
                               OnFrameCreated(*f, c);
                               return S_OK;
                           }).Get());
    }
}

void EnsureEnvironment() {
    State& s = *g_s;
    if (s.env || s.envCreating) {
        CreatePendingFrames();
        return;
    }
    s.envCreating = true;
    auto opts = Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    // без сети: фоновые службы Edge выключены; страницы — только из ui.vuic (Serve)
    opts->put_AdditionalBrowserArguments(
        L"--disable-background-networking --disable-component-update --disable-domain-reliability --disable-sync "
        L"--no-pings --disable-client-side-phishing-detection "
        L"--disable-features=msSmartScreenProtection,AutofillServerCommunication,OptimizationHints,MediaRouter");
    const HRESULT hr = ::CreateCoreWebView2EnvironmentWithOptions(
        nullptr, s.dataDir.c_str(), opts.Get(),
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT r, ICoreWebView2Environment* env) -> HRESULT {
                g_s->envCreating = false;
                if (FAILED(r) || env == nullptr) {
                    LOG_ERR("EVBots: WebUI: среда WebView2 не создана (hr 0x%08lX) — нет WebView2 Runtime?", r);
                    return S_OK;
                }
                g_s->env = env;
                LPWSTR ver = nullptr;
                env->get_BrowserVersionString(&ver);
                LOG_INFO("EVBots: WebUI: среда WebView2 %s", Utf8(ver).c_str());
                ::CoTaskMemFree(ver);
                CreatePendingFrames();
                return S_OK;
            }).Get());
    if (FAILED(hr)) {
        s.envCreating = false;
        LOG_ERR("EVBots: WebUI: CreateCoreWebView2EnvironmentWithOptions hr 0x%08lX", hr);
    }
}

Frame* FindFrame(const std::wstring& mod) {
    for (auto& f : g_s->frames)
        if (f->modFolder == mod) return f.get();
    return nullptr;
}

void DoInit(const std::wstring& mod) {
    State& s = *g_s;
    if (FindFrame(mod) != nullptr) return;  // повторный Init — страница уже есть
    auto f = std::make_unique<Frame>();
    static int s_nextLayer = 0;
    f->layerId = ++s_nextLayer;
    f->modFolder = mod;
    const size_t slash = mod.find_last_of(L"\\/");
    f->modName = Utf8((slash == std::wstring::npos ? mod : mod.substr(slash + 1)).c_str());
    for (char& c : f->modName) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    f->origin = L"https://" + Wide(f->modName) + L".webui/";
    std::string err;
    const std::wstring path = mod + L"\\ui.vuic";
    if (!f->vuic.Open(path, &err)) {
        LOG_ERR("EVBots: WebUI %s: Init — ui.vuic не открыт (%s): %s", f->modName.c_str(), Utf8(path.c_str()).c_str(),
                err.c_str());
        return;
    }
    LOG_INFO("EVBots: WebUI %s: Init — ui.vuic %zu файлов", f->modName.c_str(), f->vuic.Entries().size());
    s.frames.push_back(std::move(f));
    EnsureEnvironment();
}

void ProcessCommands() {
    std::deque<Cmd> batch;
    {
        std::lock_guard<std::mutex> lock(g_cmdGate);
        batch.swap(g_cmds);
    }
    for (const Cmd& c : batch) {
        if (c.op == "Init") {
            DoInit(c.mod);
            continue;
        }
        Frame* f = FindFrame(c.mod);
        if (f == nullptr) {
            LOG_WARN("EVBots: WebUI: %s до Init мода — пропущено", c.op.c_str());
            continue;
        }
        ApplyOp(*f, c.op, c.arg);
    }
}

bool InitComposition() {
    State& s = *g_s;
    HMODULE dc = ::LoadLibraryW(L"dcomp.dll");
    using CreateFn = HRESULT(WINAPI*)(IUnknown*, REFIID, void**);
    auto create = dc ? reinterpret_cast<CreateFn>(::GetProcAddress(dc, "DCompositionCreateDevice2")) : nullptr;
    if (create == nullptr) {
        LOG_ERR("EVBots: WebUI: нет DCompositionCreateDevice2 (dcomp.dll) — WebUI не будет");
        return false;
    }
    HRESULT hr = create(nullptr, __uuidof(IDCompositionDevice), reinterpret_cast<void**>(s.dcomp.GetAddressOf()));
    if (SUCCEEDED(hr)) hr = s.dcomp->CreateTargetForHwnd(s.overlay, TRUE, &s.target);
    if (SUCCEEDED(hr)) hr = s.dcomp->CreateVisual(&s.root);
    if (SUCCEEDED(hr)) hr = s.target->SetRoot(s.root.Get());
    if (SUCCEEDED(hr)) hr = s.dcomp->Commit();
    if (FAILED(hr)) {
        LOG_ERR("EVBots: WebUI: DirectComposition не поднялась (hr 0x%08lX) — WebUI не будет", hr);
        return false;
    }
    return true;
}

// ── ★ 231: меню в кадре игры (полноэкранный режим, WebUiPresent) ──────────────────────────────────────────────────────
// Страница — CapturePreview WebView2 (PNG с прозрачностью) → WIC 32bppPBGRA → слой WebUiPresent. Один снимок в пути на
// фрейм, таймер 33 мс: частота — сколько успевает WebView2. Ввод остаётся у дочернего окна (оно на месте и в
// полноэкранном, просто не видно), курсор — системный.

IWICImagingFactory* Wic() {
    static IWICImagingFactory* f = nullptr;  // поток WebUI, не освобождаем (выгрузка — после CoUninitialize)
    if (f == nullptr)
        ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    return f;
}

bool DecodePng(IStream* st, int* w, int* h, std::vector<uint8_t>* px) {
    IWICImagingFactory* wic = Wic();
    if (wic == nullptr) return false;
    const LARGE_INTEGER zero{};
    st->Seek(zero, STREAM_SEEK_SET, nullptr);
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> fr;
    ComPtr<IWICFormatConverter> cv;
    if (FAILED(wic->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &fr)) || FAILED(wic->CreateFormatConverter(&cv)) ||
        FAILED(cv->Initialize(fr.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom)))
        return false;
    UINT uw = 0, uh = 0;
    cv->GetSize(&uw, &uh);
    if (uw == 0 || uh == 0 || uw > 8192 || uh > 8192) return false;
    px->resize(static_cast<size_t>(uw) * uh * 4);
    if (FAILED(cv->CopyPixels(nullptr, uw * 4, static_cast<UINT>(px->size()), px->data()))) return false;
    *w = static_cast<int>(uw);
    *h = static_cast<int>(uh);
    return true;
}

Frame* FindLayer(int id) {
    if (g_s == nullptr) return nullptr;
    for (auto& f : g_s->frames)
        if (f->layerId == id) return f.get();
    return nullptr;
}

void CaptureFrame(Frame& f, int z) {
    ComPtr<IStream> st;
    if (FAILED(::CreateStreamOnHGlobal(nullptr, TRUE, &st))) return;
    f.capturing = true;
    const int id = f.layerId;
    const HRESULT hr = f.web->CapturePreview(
        COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, st.Get(),
        Callback<ICoreWebView2CapturePreviewCompletedHandler>([st, id, z](HRESULT r) -> HRESULT {
            Frame* fr = FindLayer(id);
            if (fr == nullptr) return S_OK;
            fr->capturing = false;
            if (FAILED(r)) {
                static bool logged = false;
                if (!logged) LOG_WARN("EVBots: WebUI %s: снимок страницы не сделан (hr 0x%08lX)", fr->modName.c_str(), r);
                logged = true;
                return S_OK;
            }
            if (!fr->visible || !webui_present::Wanted()) return S_OK;
            int w = 0, h = 0;
            std::vector<uint8_t> px;
            if (!DecodePng(st.Get(), &w, &h, &px)) return S_OK;
            size_t clear = 0;
            for (size_t i = 3; i < px.size(); i += 4) clear += px[i] == 0;
            if (!fr->loggedShot) {
                fr->loggedShot = true;
                LOG_INFO("EVBots: WebUI %s: меню в кадре игры — снимок %dx%d, прозрачных точек %zu%%", fr->modName.c_str(),
                         w, h, px.empty() ? 0 : clear * 100 / (px.size() / 4));
            }
            // ★ 239: страница мода видима всегда, а пока меню закрыто — полностью прозрачна. Раньше снимок 1360×768 (PNG →
            // разбор → 4 МБ в видеокарту) шёл каждые 33 мс и всё время игры: на FX-4300 (прогон 28.10, 120 Гц) процесс
            // WebView2 ~40 % ядра плюс разбор PNG в клиенте. Прозрачная — слой убрать, следующий снимок через 250 мс.
            if (clear == px.size() / 4) {
                if (fr->layerShown) {
                    fr->layerShown = false;
                    webui_present::HideLayer(id);
                }
                fr->idleUntil = ::GetTickCount64() + 250;
                return S_OK;
            }
            fr->idleUntil = 0;
            fr->layerShown = true;
            webui_present::SetLayer(id, z, w, h, std::move(px));
            return S_OK;
        }).Get());
    if (FAILED(hr)) f.capturing = false;
}

void CaptureTick() {
    State& s = *g_s;
    const bool want = webui_present::Wanted();
    static bool lastWant = false;
    if (want != lastWant && s.root) {
        // в кадре рисует WebUiPresent — DirectComposition уводим за край (если композиция кадр всё же показывает — без
        // двойного меню); фрейм остаётся в дереве, страница рисуется дальше
        lastWant = want;
        s.root->SetOffsetX(want ? 100000.0f : 0.0f);
        s.dcomp->Commit();
        LOG_INFO("EVBots: WebUI: меню рисуется %s", want ? "в кадре игры (Present)" : "окном (DirectComposition)");
    }
    for (size_t i = 0; i < s.frames.size(); ++i) {
        Frame& f = *s.frames[i];
        if (want && f.visible && f.loaded && f.web) {
            if (!f.capturing && (s.interactive || ::GetTickCount64() >= f.idleUntil)) CaptureFrame(f, static_cast<int>(i));
        } else if (f.layerShown) {
            f.layerShown = false;
            webui_present::HideLayer(f.layerId);
        }
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (Frame* f = MouseOwner(); f != nullptr && f->comp) {
        COREWEBVIEW2_MOUSE_EVENT_KIND kind{};
        bool mouse = true;
        UINT32 data = 0;
        switch (m) {
        case WM_MOUSEMOVE: kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MOVE; break;
        case WM_LBUTTONDOWN: kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOWN; break;
        case WM_LBUTTONUP: kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_UP; break;
        case WM_LBUTTONDBLCLK: kind = COREWEBVIEW2_MOUSE_EVENT_KIND_LEFT_BUTTON_DOUBLE_CLICK; break;
        case WM_RBUTTONDOWN: kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_DOWN; break;
        case WM_RBUTTONUP: kind = COREWEBVIEW2_MOUSE_EVENT_KIND_RIGHT_BUTTON_UP; break;
        case WM_MBUTTONDOWN: kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_DOWN; break;
        case WM_MBUTTONUP: kind = COREWEBVIEW2_MOUSE_EVENT_KIND_MIDDLE_BUTTON_UP; break;
        case WM_MOUSEWHEEL:
            kind = COREWEBVIEW2_MOUSE_EVENT_KIND_WHEEL;
            data = static_cast<UINT32>(static_cast<int>(GET_WHEEL_DELTA_WPARAM(w)));
            break;
        default: mouse = false; break;
        }
        if (mouse) {
            ++g_menuMouseMsgs;
            POINT p{static_cast<short>(LOWORD(l)), static_cast<short>(HIWORD(l))};
            if (m == WM_MOUSEWHEEL) ::ScreenToClient(h, &p);
            if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN) ::SetCapture(h);
            if (m == WM_LBUTTONUP || m == WM_RBUTTONUP) ::ReleaseCapture();
            f->comp->SendMouseInput(kind, static_cast<COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS>(GET_KEYSTATE_WPARAM(w)), data,
                                    p);
            return 0;
        }
    }
    switch (m) {
    case WM_SETCURSOR:
        if (Frame* f = MouseOwner(); f != nullptr && f->comp && LOWORD(l) == HTCLIENT) {
            HCURSOR c = nullptr;
            if (SUCCEEDED(f->comp->get_Cursor(&c)) && c != nullptr) {
                ::SetCursor(c);
                return TRUE;
            }
        }
        break;
    case WM_TIMER:
        if (w == kTimerFollow && g_s != nullptr && ::IsWindow(g_s->game)) Follow();
        if (w == kTimerCapture && g_s != nullptr) CaptureTick();
        return 0;
    case kMsgCmd:
        if (g_s != nullptr) ProcessCommands();
        return 0;
    case kMsgNativeMouse:
        if (g_s != nullptr) ApplyInput();
        return 0;
    case kMsgQuit:
        ::DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(h, m, w, l);
}

void ThreadMain() {
    ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    State st;
    g_s = &st;
    // ★ 238-5: ждём окно, пока клиент жив (раньше — 120 с и отказ навсегда). Прогон 238-5 22:42:51: окна «нет» 120 с —
    // полноэкранная игра свёрнута (Alt+Tab, клиентская область 0×0; гипотеза), и меню F12 пропало до перезапуска.
    // Окно появляется событием Windows, проверка — опрос раз в 200 мс; срок 120 с — только строка в журнал.
    // ★ Сборка 243: окно игры при запуске из лаунчера стартует свёрнутым (прогоны 239, 242), и клиент не грузится, пока его не
    // развернуть (правка загрузчика AllowSetForegroundWindow в 239 не помогла). Один раз за запуск: свёрнутое окно верхнего
    // уровня нашего процесса — развернуть (SW_RESTORE), как делали руками.
    static bool s_restored = false;
    for (int i = 0; g_running.load() && (st.game = FindGameWindow()) == nullptr; ++i) {
        if (!s_restored && i >= 10) {
            struct R { DWORD pid; HWND h; } r{::GetCurrentProcessId(), nullptr};
            ::EnumWindows(
                [](HWND h, LPARAM p) -> BOOL {
                    auto* c = reinterpret_cast<R*>(p);
                    DWORD pid = 0;
                    ::GetWindowThreadProcessId(h, &pid);
                    if (pid != c->pid || !::IsWindowVisible(h) || !::IsIconic(h) || ::GetWindow(h, GW_OWNER) != nullptr) return TRUE;
                    wchar_t cls[64];
                    ::GetClassNameW(h, cls, 64);
                    if (wcscmp(cls, L"EVBotsWebUi") == 0) return TRUE;
                    c->h = h;
                    return FALSE;
                },
                reinterpret_cast<LPARAM>(&r));
            if (r.h != nullptr) {
                s_restored = true;
                ::ShowWindow(r.h, SW_RESTORE);
                ::SetForegroundWindow(r.h);
                LOG_INFO("EVBots: WebUI: ★ окно игры %p было свёрнуто при запуске — развёрнуто", static_cast<void*>(r.h));
            }
        }
        if (i == 600) LOG_WARN("EVBots: WebUI: окно игры (видимое, от 320×240) не найдено за 120 с — свёрнуто? Жду дальше");
        ::Sleep(200);
    }
    if (st.game == nullptr || !g_running.load()) {
        // клиент выгружается — WebUI не нужен
    } else {
        const LONG style = ::GetWindowLongW(st.game, GWL_STYLE);
        LOG_INFO("EVBots: WebUI: окно игры %p, стиль 0x%08lX (%s)", static_cast<void*>(st.game), style,
                 (style & WS_CAPTION) ? "с рамкой" : "без рамки");
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.lpszClassName = L"EVBotsWebUi";
        wc.style = CS_DBLCLKS;
        ::RegisterClassW(&wc);
        // ★ 229: дочернее окно игры (не отдельное окно верхнего уровня): фокус в меню не снимает активность с BF3 —
        // полноэкранный режим не слетает (прогон 228). Пока ввод не нужен — прозрачно для мыши и выключено.
        // Без WS_EX_LAYERED: дочернее «слоёное» окно Windows создаёт только приложению с манифестом Windows 8+ (у bf3.exe
        // его нет; проверка без игры: CreateWindowEx вернул NULL). Прозрачность для мыши — выключенное окно (WS_DISABLED).
        st.overlay = ::CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"EVBots WebUI",
                                       WS_CHILD | WS_CLIPSIBLINGS | WS_DISABLED, 0, 0, 640, 480, st.game, nullptr,
                                       wc.hInstance, nullptr);
        if (st.overlay != nullptr) {
            g_gameWnd = st.game;
            // сборка 237: оконную процедуру игры подменяет ядро (ev-core), сюда приходят сообщения через обработчик
            const auto sub = reinterpret_cast<CoreWndSubclassFn>(CoreExport("EvWndSubclass"));
            const bool subOk = sub != nullptr && sub(st.game, &GameWndHandler) != 0;
            LOG_INFO("EVBots: WebUI: оверлей — дочернее окно игры; оконная процедура игры %s (ядро), поток окна игры %lu",
                     subOk ? "перехвачена" : "НЕ перехвачена", ::GetWindowThreadProcessId(st.game, nullptr));
            HookGameCursor();
        }
        wchar_t local[MAX_PATH] = L"";
        ::GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        st.dataDir = std::wstring(local) + L"\\EVBots\\webview2";
        if (st.overlay != nullptr && InitComposition()) {
            g_overlay = st.overlay;
            // Stop мог прийти между обнаружением окна и публикацией g_overlay.
            // После публикации Stop уже может разбудить GetMessage через kMsgQuit.
            if (g_running.load()) {
                Follow();
                ::SetTimer(st.overlay, kTimerFollow, 50, nullptr);
                if (webui_present::Install()) ::SetTimer(st.overlay, kTimerCapture, 33, nullptr);  // ★ 231
                LOG_INFO("EVBots: WebUI: оверлей готов (%dx%d) — ждём WebUI:Init модов", g_screenW.load(), g_screenH.load());
                ProcessCommands();  // Init, пришедшие до готовности окна
                MSG msg;
                while (g_running.load() && ::GetMessageW(&msg, nullptr, 0, 0) > 0) {
                    ::TranslateMessage(&msg);
                    ::DispatchMessageW(&msg);
                }
            }
        } else if (st.overlay == nullptr) {
            LOG_ERR("EVBots: WebUI: окно оверлея не создано (%lu)", ::GetLastError());
        }
        if (st.overlay != nullptr && ::IsWindow(st.overlay)) ::DestroyWindow(st.overlay);
        g_overlay = nullptr;
        g_mouseCaptured = false;
        g_keysCaptured = false;
        // вернуть оконную процедуру игры (ядро, если поверх нас её никто не подменил)
        if (const auto unsub = reinterpret_cast<CoreWndUnsubclassFn>(CoreExport("EvWndUnsubclass"))) unsub(st.game);
        for (auto& f : st.frames)
            LOG_INFO("EVBots: WebUI %s: итог — отдано %d, нет в архиве %d, внешних запросов %d, WebUI.Call %d",
                     f->modName.c_str(), f->served, f->missing, f->external, f->calls);
        LOG_INFO("EVBots: WebUI: ввод игры при меню — Raw Input мыши отброшено %ld, клавиатуры %ld, вызовов курсора игры %ld",
                 g_rawMouseDropped.load(), g_rawKeysDropped.load(), g_cursorCallsBlocked.load());
        long presents = 0, drawn = 0;
        webui_present::Stats(&presents, &drawn);
        LOG_INFO("EVBots: WebUI в кадре: Present %ld, кадров с меню %ld", presents, drawn);
    }
    st.frames.clear();
    st.env.Reset();
    st.root.Reset();
    st.target.Reset();
    st.dcomp.Reset();
    g_s = nullptr;
    g_gameWnd = nullptr;
    ::CoUninitialize();
    g_running = false;
}

void PushCmd(const std::wstring& mod, const char* op, std::string arg) {
    {
        std::lock_guard<std::mutex> lock(g_cmdGate);
        if (g_cmds.size() < 8192) g_cmds.push_back(Cmd{mod, op, std::move(arg)});
    }
    if (HWND o = g_overlay.load()) ::PostMessageW(o, kMsgCmd, 0, 0);
}

int l_webui_init(lua_State* L) {
    const std::wstring mod = LuaRuntime::Instance().currentModFolder();
    if (!g_running.load() || mod.empty()) {
        lua_pushboolean(L, 0);
        return 1;
    }
    PushCmd(mod, "Init", {});
    lua_pushboolean(L, 1);
    return 1;
}

int l_webui_cmd(lua_State* L) {
    const char* op = luaL_checkstring(L, 1);
    size_t n = 0;
    const char* arg = luaL_optlstring(L, 2, "", &n);
    const std::wstring mod = LuaRuntime::Instance().currentModFolder();
    if (g_running.load() && !mod.empty()) PushCmd(mod, op, std::string(arg, n));
    return 0;
}

int l_webui_size(lua_State* L) {
    lua_pushinteger(L, g_screenW.load());
    lua_pushinteger(L, g_screenH.load());
    return 2;
}

}  // namespace

bool WebUiKeyboardCaptured() { return g_keysCaptured.load(); }

namespace {
bool WebUiTestModeLocal() { return WebUiTestMode(); }

bool NativeMouseEnabled() {
    // Stock MouseInput UI action: table22B6698 -> factory8EBAB0 -> action8F80E0.
    // UI singleton getter FF6120; the action writes UI+ A821 and A822 at 8F813E/44.
    // Read only, from the engine frame; no native patch/call or window-state guess.
    if (reinterpret_cast<uintptr_t>(GetModuleHandleA("bf3.exe")) != 0x400000) return false;
    static int verified = 0;
    __try {
        if (!verified) {
            char flag[4] = {};
            if (GetEnvironmentVariableA("EVBOTS_WEBUI_NATIVE_MOUSE", flag, sizeof(flag)) == 1 && flag[0] == '0') {
                verified = -1;
                return false;
            }
            const unsigned char getter[] = {0xA1,0xE0,0x5C,0x38,0x02,0xC3};
            const unsigned char action[] = {0x8B,0xCE,0x88,0x9E,0x21,0xA8,0,0,0x88,0x96,0x22,0xA8,0,0,0xE8,0x51};
            verified = std::memcmp(reinterpret_cast<void*>(0x00FF6120), getter, sizeof(getter)) == 0 &&
                       std::memcmp(reinterpret_cast<void*>(0x008F813C), action, sizeof(action)) == 0 ? 1 : -1;
            LOG_INFO("WebUI: native MouseInput layout signatures %s", verified == 1 ? "PASS" : "FAIL");
        }
        if (verified < 0) return false;
        const uintptr_t ui = *reinterpret_cast<const uintptr_t*>(0x02385CE0);
        return ui && *reinterpret_cast<const unsigned char*>(ui + 0xA821) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
}  // namespace

void WebUiFrameTick() {
    const bool nativeMouse = NativeMouseEnabled();
    if (g_nativeMouseEnabled.exchange(nativeMouse) != nativeMouse) {
        LOG_INFO("WebUI: native MouseInput %s", nativeMouse ? "enabled" : "disabled");
        if (const HWND overlay = g_overlay.load()) ::PostMessageW(overlay, kMsgNativeMouse, 0, 0);
    }
    // ★ 230: курсор — в потоке окна игры (SyncGameCursor в GameWndHandler); здесь только запись, в каких потоках кадр и окно
    static bool threadLogged = false;
    const HWND game = g_gameWnd.load();
    if (!threadLogged && game != nullptr) {
        threadLogged = true;
        const DWORD wt = ::GetWindowThreadProcessId(game, nullptr);
        LOG_INFO("EVBots: WebUI: кадр движка в потоке %lu, окно игры — в потоке %lu (%s)", ::GetCurrentThreadId(), wt,
                 wt == ::GetCurrentThreadId() ? "один поток" : "разные потоки");
    }
}

bool WebUiTestMode() {
    wchar_t v[8] = L"";
    return ::GetEnvironmentVariableW(L"EVBOTS_WEBUI_TEST", v, 8) > 0 && v[0] == L'1';
}

bool WebUiStart() {
    std::lock_guard<std::mutex> lifecycle(g_lifecycleGate);
    wchar_t v[8] = L"";
    if (::GetEnvironmentVariableW(L"EVBOTS_WEBUI", v, 8) > 0 && v[0] == L'0') {
        LOG_INFO("EVBots: WebUI: выключено (EVBOTS_WEBUI=0)");
        return false;
    }
    if (g_stopping) return false;
    if (g_thread.joinable()) {
        if (::WaitForSingleObject(g_thread.native_handle(), 0) != WAIT_OBJECT_0) {
            LOG_WARN("EVBots: WebUI: предыдущий поток ещё работает — повторный старт отложен");
            return false;
        }
        g_thread.join();
    }
    ClearSessionQueues();
    g_running = true;
    g_thread = std::thread(ThreadMain);
    return true;
}

void WebUiStop() {
    HANDLE th = nullptr;
    {
        std::lock_guard<std::mutex> lifecycle(g_lifecycleGate);
        g_running = false;
        if (g_stopping) return;
        if (!g_thread.joinable()) {
            ClearSessionQueues();
            return;
        }
        g_stopping = true;
        th = g_thread.native_handle();
    }
    if (HWND o = g_overlay.load()) ::PostMessageW(o, kMsgQuit, 0, 0);
    // ★ 229: оверлей — дочернее окно игры, его закрытие шлёт синхронные сообщения потоку окна игры. Если ждём отсюда
    // (поток игры) простым join — взаимоблокировка (проверка без игры висела при выходе). Ждём, разбирая сообщения; не
    // дольше 10 с. Незавершённый поток сохраняем joinable: новый Start не создаст
    // второй поток поверх его g_s/g_overlay, а следующий Stop сможет дождаться его.
    const ULONGLONG end = ::GetTickCount64() + 10000;
    bool completed = false;
    for (;;) {
        const DWORD r = ::MsgWaitForMultipleObjects(1, &th, FALSE, 100, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) { completed = true; break; }
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        if (::GetTickCount64() > end) {
            LOG_WARN("EVBots: WebUI: поток меню не закрылся за 10 с — повторный старт запрещён до его завершения");
            break;
        }
    }
    {
        std::lock_guard<std::mutex> lifecycle(g_lifecycleGate);
        if (completed) {
            g_thread.join();
            ClearSessionQueues();
        }
        g_stopping = false;
    }
}

void WebUiDrain(std::vector<WebUiCall>* out) {
    std::lock_guard<std::mutex> lock(g_callGate);
    out->swap(g_calls);
    g_calls.clear();
}

bool RegisterClientWebUi(lua_State* L) {
    lua_pushcfunction(L, l_webui_init);
    lua_setglobal(L, "__webui_init");
    lua_pushcfunction(L, l_webui_cmd);
    lua_setglobal(L, "__webui_cmd");
    lua_pushcfunction(L, l_webui_size);
    lua_setglobal(L, "__webui_size");
    return true;
}

}  // namespace evbots
