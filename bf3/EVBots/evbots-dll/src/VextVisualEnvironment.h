#pragma once
struct lua_State;
namespace evbots {
// Client-only. Call before loading mods/levels so native state names are captured.
bool RegisterVextVisualEnvironment(lua_State* L);
}
