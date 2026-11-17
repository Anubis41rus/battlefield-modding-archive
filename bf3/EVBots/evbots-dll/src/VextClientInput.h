// VextClientInput.h — ввод клиента для InputManager (VU-Docs client\library\inputmanager.md), сборка 228.
#pragma once

struct lua_State;

namespace evbots {

/// Опрос клавиатуры и мыши раз в кадр движка (до Client:UpdateInput). Клавиши видны только когда окно игры на переднем
/// плане: при открытом меню (WebUI с вводом) ввод идёт странице, как у VU.
void ClientInputPollFrame();

/// __im_key(idk) -> isDown, wentDown, wentUp; __im_mouse(button) -> то же для кнопок мыши; __im_cursor() -> x, y.
bool RegisterClientInput(lua_State* L);

}  // namespace evbots
