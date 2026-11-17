// VextWebUi.h — WebUI клиента как у VU (VU-Docs client\library\webui.md, vext\guides\webui.md, modding\custom-ui.md),
// сборка 228 (25.10.2026).
//
// У VU WebUI — Coherent Gameface в vu-core: у каждого мода свой фрейм со страницей из его ui.vuic, Lua управляет им через
// WebUI:Init/Show/Hide/BringToFront/SendToBack/Enable|Disable|ResetKeyboard/Mouse/ExecuteJS, страница — через
// WebUI.Call('Show'|'EnableMouse'|… ) и шлёт события WebUI.Call('DispatchEvent'|'DispatchEventLocal', имя, данные).
// У нас (реестр В7): WebView2 (системный Edge) в своём потоке, окно-оверлей над окном игры (DirectComposition, прозрачный
// фон), фрейм = WebView2 со своим визуалом; страница — https://<мод>.webui/… из ui.vuic в памяти (без сети).
// Пока ни один фрейм не забрал мышь, оверлей прозрачен для мыши и не берёт фокус — игра управляется как обычно.
// EVBOTS_WEBUI=0 — не запускать.

#pragma once

#include <string>
#include <vector>

struct lua_State;

namespace evbots {

/// Поднять поток оверлея (окно игры ищется в нём). false — выключено или уже работает.
bool WebUiStart();
/// EVBOTS_WEBUI_TEST=1: WebUI и ввод клиента поднимаются и без bf3.exe (проверка без игры — webui-probe\client-harness).
bool WebUiTestMode();
/// Остановить поток оверлея (закрыть фреймы, окно, WebView2).
void WebUiStop();

/// ★ Сборка 229: клавиатуру забрал фрейм WebUI (EnableKeyboard) — InputManager игры клавиш не видит (как у VU).
/// Мышь без клавиатуры (колесо команд fun-bots на Alt, focusMouse) клавиши у мода не отнимает.
bool WebUiKeyboardCaptured();
/// ★ Сборка 229: раз в кадр движка (поток игры): пока фрейм держит мышь — курсор показан и не зажат, захват мыши игрой
/// снят; отпустил — вернуть курсор, как хотела игра (её ShowCursor во время меню копятся).
void WebUiFrameTick();

/// Событие страницы для Lua: JSON-массив аргументов WebUI.Call и папка мода фрейма.
struct WebUiCall {
    std::wstring modFolder;
    std::string json;
};
/// Забрать накопленные вызовы страниц (из кадра движка, под замком Lua).
void WebUiDrain(std::vector<WebUiCall>* out);

/// __webui_init(), __webui_cmd(имя[, аргумент]), __webui_size() -> w, h — для WebUI в VextClient.cpp; мод — текущий
/// (LuaRuntime::currentModFolder).
bool RegisterClientWebUi(lua_State* L);

}  // namespace evbots
