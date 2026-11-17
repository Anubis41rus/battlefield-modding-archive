// VextClientInput.cpp — см. VextClientInput.h.
//
// У VU InputManager читает ввод движка. У нас — состояние клавиш Windows раз в кадр движка (GetAsyncKeyState), потому что
// fun-bots нужны только «нажата / нажалась / отпустилась» (InputManager:WentKeyDown — 42 места, WentKeyUp — 2). Коды
// InputDeviceKeys — сканкоды DirectInput (IDK_F12 = 88 = DIK_F12, IDK_ArrowUp = 200 = DIK_UP): сканкод → виртуальная
// клавиша через MapVirtualKey (≥ 0x80 — расширенный, префикс E0). Реестр В17: клавиши — не ввод движка; концепты
// (GetLevel/IsDown/WentDown/WentUp) — из движка с 234.
// Ввод видят только когда на переднем плане окно игры нашего процесса: открытое меню WebUI (окно EVBotsWebUi) забирает
// клавиатуру и мышь себе — как у VU, где при вводе в WebUI игра клавиш не получает.

#include "VextClientInput.h"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <cmath>
#include <climits>

#include "VextWebUi.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
}

namespace evbots {
namespace {

constexpr int kKeys = 256;
constexpr int kButtons = 8;

uint8_t g_vk[kKeys];  // InputDeviceKeys -> VK (0 — нет)
bool g_vkReady = false;
bool g_keyNow[kKeys], g_keyPrev[kKeys];
bool g_btnNow[kButtons], g_btnPrev[kButtons];
POINT g_cursor{0, 0};

void BuildVkTable() {
    std::memset(g_vk, 0, sizeof(g_vk));
    for (int idk = 1; idk < kKeys; ++idk) {
        const UINT sc = idk >= 0x80 ? (0xE000u | static_cast<UINT>(idk & 0x7F)) : static_cast<UINT>(idk);
        g_vk[idk] = static_cast<uint8_t>(::MapVirtualKeyW(sc, MAPVK_VSC_TO_VK_EX));
    }
    // DIK, которые MapVirtualKey не знает или путает
    g_vk[0xC5] = VK_PAUSE;  // DIK_PAUSE
    g_vk[0xB7] = VK_SNAPSHOT;  // DIK_SYSRQ
    g_vkReady = true;
}

/// Клавиши видны моду: на переднем плане окно нашего процесса (игра или меню WebUI) и клавиатуру не забрал фрейм WebUI.
/// ★ Сборка 229: прежде меню на переднем плане скрывало все клавиши — колесо команд fun-bots (Alt зажат → мод включает мышь
/// → меню берёт фокус → мод видел «Alt отпущен» → закрывал колесо → снова «нажат») мерцало (слова пользователя, прогон 228).
/// У VU мышь без клавиатуры (focusMouse) клавиши у игры не отнимает.
bool GameHasFocus() {
    const HWND fg = ::GetForegroundWindow();
    if (fg == nullptr) return false;
    DWORD pid = 0;
    ::GetWindowThreadProcessId(fg, &pid);
    return pid == ::GetCurrentProcessId() && !WebUiKeyboardCaptured();
}

constexpr int kMouseVk[kButtons] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2, 0, 0, 0};

int CheckCode(lua_State* L, int max) {
    const lua_Integer v = luaL_checkinteger(L, 1);
    return (v >= 0 && v < max) ? static_cast<int>(v) : -1;
}

int l_im_key(lua_State* L) {
    const int k = CheckCode(L, kKeys);
    const bool now = k >= 0 && g_keyNow[k], prev = k >= 0 && g_keyPrev[k];
    lua_pushboolean(L, now);
    lua_pushboolean(L, now && !prev);
    lua_pushboolean(L, !now && prev);
    return 3;
}

int l_im_mouse(lua_State* L) {
    const int b = CheckCode(L, kButtons);
    const bool now = b >= 0 && g_btnNow[b], prev = b >= 0 && g_btnPrev[b];
    lua_pushboolean(L, now);
    lua_pushboolean(L, now && !prev);
    lua_pushboolean(L, !now && prev);
    return 3;
}

int l_im_cursor(lua_State* L) {
    lua_pushinteger(L, g_cursor.x);
    lua_pushinteger(L, g_cursor.y);
    return 2;
}

int l_im_set_cursor(lua_State* L) {
    const lua_Number x = luaL_checknumber(L, 1), y = luaL_checknumber(L, 2);
    luaL_argcheck(L, std::isfinite(x) && x >= INT_MIN && x <= INT_MAX, 1, "cursor coordinate out of range");
    luaL_argcheck(L, std::isfinite(y) && y >= INT_MIN && y <= INT_MAX, 2, "cursor coordinate out of range");
    const HWND window = ::GetForegroundWindow();
    DWORD pid = 0;
    if (window == nullptr || ::GetWindowThreadProcessId(window, &pid) == 0 || pid != ::GetCurrentProcessId()) return 0;
    POINT point{static_cast<LONG>(x), static_cast<LONG>(y)};
    const POINT local = point;
    if (::ClientToScreen(window, &point) && ::SetCursorPos(point.x, point.y)) g_cursor = local;
    return 0;
}

// ★ Сборка 234 (реестр В17): концепты ввода из движка, как у VU (CLIENT-SLOTS ★: слот 18E74F38 = 00FBFFC0 — геттер
// `mov eax,[0x2384EB8]; ret`, менеджер понятий ввода, vtable 02144658). Факт (дизассемблер zbf3, 26.10):
//   +0x14 GetLevel  01033A30 — thiscall(concept, 0) -> float (ST0), ret 8;
//   +0x20 IsDown    01033A80 — thiscall(concept, 0) -> bool (AL), ret 8;
//   +0x28 WentDown  01033AD0 — thiscall(concept) -> bool, ret 4;
//   +0x30 WentUp    01033B10 — thiscall(concept) -> bool, ret 4.
// ConceptUndefined (0x7C) и пустая таблица движок сам отдаёт как 0/false. Зовём из потока движка (Lua клиента — кадр).
constexpr uintptr_t kConceptMgrVarVa = 0x02384EB8;
constexpr uintptr_t kConceptMgrVtable = 0x02144658;

using ConceptLevelFn = float(__fastcall*)(void* self, void* edx, int concept, int arg);
using ConceptBool2Fn = bool(__fastcall*)(void* self, void* edx, int concept, int arg);
using ConceptBool1Fn = bool(__fastcall*)(void* self, void* edx, int concept);

/// Менеджер понятий ввода (nullptr — нет или vtable не та).
void* ConceptMgr() {
    __try {
        void* m = *reinterpret_cast<void**>(kConceptMgrVarVa);
        if (m == nullptr || *reinterpret_cast<uintptr_t*>(m) != kConceptMgrVtable) return nullptr;
        return m;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* VFunc(void* obj, int off) { return reinterpret_cast<void*>(*reinterpret_cast<uintptr_t*>(*reinterpret_cast<uintptr_t*>(obj) + off)); }

// kind: 0 GetLevel, 1 IsDown, 2 WentDown, 3 WentUp. false — движка нет (тест-хост) или исключение.
bool CallConcept(int kind, int concept, float* level, bool* flag) {
    __try {
        void* m = ConceptMgr();
        if (m == nullptr) return false;
        switch (kind) {
            case 0: *level = reinterpret_cast<ConceptLevelFn>(VFunc(m, 0x14))(m, nullptr, concept, 0); break;
            case 1: *flag = reinterpret_cast<ConceptBool2Fn>(VFunc(m, 0x20))(m, nullptr, concept, 0); break;
            case 2: *flag = reinterpret_cast<ConceptBool1Fn>(VFunc(m, 0x28))(m, nullptr, concept); break;
            default: *flag = reinterpret_cast<ConceptBool1Fn>(VFunc(m, 0x30))(m, nullptr, concept); break;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

/// __im_concept(вид, концепт) -> значение | nil (движка нет — Lua оставляет заглушку).
int l_im_concept(lua_State* L) {
    const int kind = static_cast<int>(luaL_checkinteger(L, 1));
    const int concept = static_cast<int>(luaL_checkinteger(L, 2));
    float level = 0.0f;
    bool flag = false;
    if (concept < 0 || !CallConcept(kind, concept, &level, &flag)) {
        lua_pushnil(L);
        return 1;
    }
    if (kind == 0) {
        lua_pushnumber(L, level == level ? level : 0.0f);  // NaN — 0
    } else {
        lua_pushboolean(L, flag);
    }
    return 1;
}

}  // namespace

void ClientInputPollFrame() {
    if (!g_vkReady) BuildVkTable();
    std::memcpy(g_keyPrev, g_keyNow, sizeof(g_keyNow));
    std::memcpy(g_btnPrev, g_btnNow, sizeof(g_btnNow));
    const bool focus = GameHasFocus();
    // ★ Сборка 230: клавиша, зажатая, пока клавиши были скрыты (меню держало клавиатуру), — не «новое нажатие», пока её
    // не отпустят. Прогон 229: F12 закрывал меню (страница), а мод тут же видел «F12 нажат» и открывал снова — 6 раз.
    static bool s_focusPrev = false;
    static bool s_held[kKeys];
    for (int i = 0; i < kKeys; ++i) {
        const bool phys = g_vk[i] != 0 && (::GetAsyncKeyState(g_vk[i]) & 0x8000) != 0;
        if (focus && !s_focusPrev) s_held[i] = phys;
        if (!phys) s_held[i] = false;
        g_keyNow[i] = focus && phys && !s_held[i];
    }
    s_focusPrev = focus;
    // ★ Сборка 242: повтор «отпущена» через 0,5 с, если клавишу не нажали снова. Колесо команд fun-bots (Alt): мод по
    // WentKeyDown шлёт серверу запрос, по WentKeyUp закрывает — но если ответ сервера пришёл ПОСЛЕ отпускания (Alt-Tab:
    // фокус ушёл через миг после нажатия; прогон 241: запрос 14:54:12.922, ответ 14:54:13.158), мод открывает колесо с мышью
    // уже без Alt, и оно висит до следующего Alt — клики «Дислокации» уходят в меню. Второе «отпущена» закрывает такое колесо;
    // мод без открытого колеса его не замечает (UIClient.lua:324). Реестр VU-DIVERGENCES.
    static ULONGLONG s_upAt[kKeys];
    const ULONGLONG nowMs = ::GetTickCount64();
    for (int i = 0; i < kKeys; ++i) {
        if (g_keyNow[i]) {
            s_upAt[i] = 0;
        } else if (g_keyPrev[i]) {
            s_upAt[i] = nowMs;
        } else if (s_upAt[i] != 0 && nowMs - s_upAt[i] >= 500) {
            s_upAt[i] = 0;
            g_keyPrev[i] = true;  // в этом кадре l_im_key отдаст WentKeyUp
        }
    }
    for (int i = 0; i < kButtons; ++i)
        g_btnNow[i] = focus && kMouseVk[i] != 0 && (::GetAsyncKeyState(kMouseVk[i]) & 0x8000) != 0;
    POINT p;
    if (::GetCursorPos(&p)) {
        const HWND fg = ::GetForegroundWindow();
        if (fg != nullptr) ::ScreenToClient(fg, &p);
        g_cursor = p;
    }
}

bool RegisterClientInput(lua_State* L) {
    lua_pushcfunction(L, l_im_key);
    lua_setglobal(L, "__im_key");
    lua_pushcfunction(L, l_im_mouse);
    lua_setglobal(L, "__im_mouse");
    lua_pushcfunction(L, l_im_cursor);
    lua_setglobal(L, "__im_cursor");
    lua_pushcfunction(L, l_im_set_cursor);
    lua_setglobal(L, "__im_set_cursor");
    lua_pushcfunction(L, l_im_concept);
    lua_setglobal(L, "__im_concept");
    return true;
}

}  // namespace evbots
