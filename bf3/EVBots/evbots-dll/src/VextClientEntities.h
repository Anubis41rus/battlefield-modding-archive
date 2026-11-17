#pragma once
struct lua_State;
namespace evbots {
// Shared server/client bindings. Register after EntityManager, Spatial and FB
// class bindings; otherwise their later registration would replace the casts.
bool RegisterVextBlueprintEntities(lua_State* L);
// Call from the regular Lua tick while the owning engine-frame scope is active.
void PollVextBlueprintCollisions(lua_State* L);
}
