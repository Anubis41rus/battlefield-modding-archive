#pragma once
#include "NativeBlueprint.h"
struct lua_State;
namespace evbots {
// Read/write only a current generation-tracked ClientSoldierEntity.
// False means the property does not belong to this adapter; no stack change.
bool PushClientSoldierProperty(lua_State*,const blueprint::Handle&,const char*);
bool SetClientSoldierProperty(lua_State*,const blueprint::Handle&,const char*,int valueIndex);
// After BlueprintEntity registration, before mod execution. Exposes __cls_wrap.
bool RegisterVextClientSoldier(lua_State*);
}
