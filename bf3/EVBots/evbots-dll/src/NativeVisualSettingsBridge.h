#pragma once
struct lua_State;
namespace evbots {
// After Events, NetEvents, RCON and NativeBlueprint. Registers its own frame callbacks.
bool RegisterNativeVisualSettings(lua_State* L);
}
