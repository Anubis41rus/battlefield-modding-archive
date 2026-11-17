#pragma once
struct lua_State;
namespace evbots {
// Register after the weak Entity metatable. Start does not patch the engine;
// a proven native vtable is patched only when a real entity subscribes.
bool RegisterVextEntityEvents(lua_State* L);
// EntityIndex integration: pushes a method if the key belongs to this adapter.
bool PushVextEntityEventMethod(lua_State* L,const char* key);
// Poll only inside the real engine frame, under the Lua gate. Synchronous native
// delivery separately uses try_lock; no worker waits on Lua for cancellation.
void PollVextEntityEvents(lua_State* L);
void ShutdownVextEntityEvents(lua_State* L);
}
