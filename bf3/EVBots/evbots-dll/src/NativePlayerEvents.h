#pragma once
#include "NativeBlueprint.h"

struct lua_State;
namespace evbots {
// ServerPlayerEvent retains the engine's own player weak reference. The legacy
// bot constructor stays in place; only real human players use this userdata.
bool RegisterNativePlayerEvents(lua_State* L);
bool IsNativePlayerEvent(lua_State* L,int index);
// Returns a Lua boolean or raises: false native dispatch is never bot fallback.
int FireNativePlayerEvent(lua_State* L,int index,const blueprint::Handle& target);
int FireNativeHumanUnspawn(lua_State* L,const blueprint::Handle& target);
namespace playerevents {
struct Event;
struct Params { std::uint32_t id=0;bool flags[6]={};int team=0; };
void Start();
Event* Create(std::uintptr_t player,const Params& params,const char** error);
bool Fire(Event* event,const blueprint::Handle& target,const char** error);
bool Unspawn(const blueprint::Handle& target,const char** error);
bool Alive(const Event* event);
// GC never enters the engine. Poll performs destruction only on a native frame.
void Release(Event* event);
void Poll();
// Invalidates outstanding userdata; queued native releases drain on a frame.
// On final process shutdown without another frame, bounded nodes are retained.
void Shutdown();
}
}
