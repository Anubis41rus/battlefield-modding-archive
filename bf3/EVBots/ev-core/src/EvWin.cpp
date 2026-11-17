// EvWin.cpp — окно игры и Raw Input мыши на время меню (сборка 237, 26.10.2026). Как у VU: подмену оконной процедуры
// игры и регистрацию Raw Input держит ядро (у VU RegisterRawInputDevices — в vu-core), EVBots.dll (меню WebUI) только
// решает, что делать с сообщением. Перенесено из evbots-dll\src\VextWebUi.cpp (GameWndProc, Release/RestoreGameRawMouse).

#include <windows.h>

#include <atomic>
#include <vector>

#include "EvCore.h"

namespace {

/// Обработчик сообщений окна игры от EVBots.dll: 1 — сообщение обработано (*result — ответ), 0 — отдать игре.
using EvWndHandler = int(__cdecl*)(HWND, UINT, WPARAM, LPARAM, LRESULT*);

std::atomic<LONG_PTR> g_origProc{0};
std::atomic<EvWndHandler> g_handler{nullptr};
std::atomic<HWND> g_wnd{nullptr};

RAWINPUTDEVICE g_gameRawMouse{};
std::atomic<bool> g_gameRawMouseSaved{false};

LRESULT CALLBACK CoreGameWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (EvWndHandler fn = g_handler.load()) {
        LRESULT r = 0;
        if (fn(h, m, w, l, &r) != 0) return r;
    }
    return ::CallWindowProcW(reinterpret_cast<WNDPROC>(g_origProc.load()), h, m, w, l);
}

}  // namespace

/// Подменить оконную процедуру окна игры; сообщения сначала получает handler. 1 — подменена.
extern "C" __declspec(dllexport) int __cdecl EvWndSubclass(HWND wnd, EvWndHandler handler) {
    if (wnd == nullptr || handler == nullptr) return 0;
    g_handler = handler;
    if (g_wnd.load() == wnd && g_origProc.load() != 0) return 1;
    const LONG_PTR prev = ::SetWindowLongPtrW(wnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&CoreGameWndProc));
    if (prev == 0) {
        FileLog("[win] окно игры %p: оконная процедура НЕ подменена (%lu)", static_cast<void*>(wnd), ::GetLastError());
        return 0;
    }
    g_origProc = prev;
    g_wnd = wnd;
    FileLog("[win] окно игры %p: оконная процедура подменена (поток окна %lu)", static_cast<void*>(wnd),
            ::GetWindowThreadProcessId(wnd, nullptr));
    return 1;
}

/// Вернуть оконную процедуру игры (если поверх нас её никто не подменил). 1 — возвращена.
extern "C" __declspec(dllexport) int __cdecl EvWndUnsubclass(HWND wnd) {
    g_handler = nullptr;
    if (wnd == nullptr || g_wnd.load() != wnd || g_origProc.load() == 0) return 0;
    int ok = 0;
    if (::GetWindowLongPtrW(wnd, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&CoreGameWndProc)) {
        ::SetWindowLongPtrW(wnd, GWLP_WNDPROC, g_origProc.load());
        ok = 1;
    }
    FileLog("[win] окно игры %p: оконная процедура %s", static_cast<void*>(wnd), ok ? "возвращена" : "не возвращена (подменена поверх)");
    g_wnd = nullptr;
    // g_origProc не сбрасываем: если поверх нас кто-то подменил, наш обработчик ещё может получить сообщение
    return ok;
}

/// Снять Raw Input мыши игры (usage 1/2 с RIDEV_NOLEGACY) — Windows снова шлёт окну мышиные сообщения (меню).
/// Как игра в своём режиме курсора (zbf3 FUN_00fc7830: RIDEV_REMOVE). Только в потоке окна игры.
/// 1 — снята сейчас (первая строка — в журнал), 0 — нечего снимать.
extern "C" __declspec(dllexport) int __cdecl EvRawMouseRelease() {
    UINT n = 0;
    ::GetRegisteredRawInputDevices(nullptr, &n, sizeof(RAWINPUTDEVICE));
    if (n == 0) return 0;
    std::vector<RAWINPUTDEVICE> v(n);
    if (::GetRegisteredRawInputDevices(v.data(), &n, sizeof(RAWINPUTDEVICE)) == static_cast<UINT>(-1)) return 0;
    for (const RAWINPUTDEVICE& d : v) {
        if (d.usUsagePage != 1 || d.usUsage != 2 || (d.dwFlags & RIDEV_NOLEGACY) != RIDEV_NOLEGACY) continue;
        RAWINPUTDEVICE off{1, 2, RIDEV_REMOVE, nullptr};
        const BOOL ok = ::RegisterRawInputDevices(&off, 1, sizeof(off));
        if (!g_gameRawMouseSaved.load())
            FileLog("[win] Raw Input мыши игры (флаги 0x%08lX, окно %p) снят на время меню: %s", d.dwFlags,
                    static_cast<void*>(d.hwndTarget), ok ? "да" : "НЕТ");
        if (ok) {
            g_gameRawMouse = d;
            g_gameRawMouseSaved = true;
            return 1;
        }
        return 0;
    }
    return 0;
}

/// Вернуть регистрацию Raw Input мыши игры. 1 — возвращена, 0 — не была снята, -1 — RegisterRawInputDevices отказал.
extern "C" __declspec(dllexport) int __cdecl EvRawMouseRestore() {
    if (!g_gameRawMouseSaved.exchange(false)) return 0;
    const BOOL ok = ::RegisterRawInputDevices(&g_gameRawMouse, 1, sizeof(g_gameRawMouse));
    FileLog("[win] Raw Input мыши игры возвращён: %s", ok ? "да" : "НЕТ");
    return ok ? 1 : -1;
}
