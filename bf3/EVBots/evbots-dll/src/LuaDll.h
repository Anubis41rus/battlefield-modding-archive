// LuaDll.h — Lua в отдельной lua.dll (как у VU).
#pragma once

namespace evbots {

/// Загрузить lua.dll из папки EVBots.dll (один раз). false — её нет: Lua-режим запускать нельзя.
bool EnsureLuaDll();

}  // namespace evbots
