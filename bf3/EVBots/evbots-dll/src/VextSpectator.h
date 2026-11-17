#pragma once

struct lua_State;

namespace evbots {

// Client only. Register before mod initializers; geometry/PlayerManager must be
// ready before methods run. State getters may read the last engine snapshot from
// the startup worker. Camera/transform operations stay on the engine thread.
bool RegisterVextSpectator(lua_State* L);

// Call on the client engine thread before Lua Engine:Update, including when Lua
// is stopped, so deferred Stop requests can restore the native camera.
void TickVextSpectator();

// On the engine thread: restores the native player view immediately. On another
// thread: queues restoration for TickVextSpectator. Call after Level:Destroy
// handlers, before client Lua shutdown/restart and before removing the tick hook.
void StopVextSpectator();

}  // namespace evbots
