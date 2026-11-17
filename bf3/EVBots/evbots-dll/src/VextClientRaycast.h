// VextClientRaycast.h — лучи клиента из движка bf3.exe (К5 шаг 4).
#pragma once

struct lua_State;

namespace evbots {

/// __clr_collision, __clr_types — для RaycastManager клиента (VextClient.cpp). Только клиент (настоящий bf3.exe).
bool RegisterClientRaycast(lua_State* L);
// Install after both the client Lua library and native BlueprintEntity metatable.
// Uses the engine SpatialQuery and returns generation-tracked native Entities.
bool InstallClientSpatialRaycast(lua_State* L);
// Same installation order; calls the PhysicsManager raycaster's native interface.
bool InstallClientPhysicsRaycast(lua_State* L);

}  // namespace evbots
