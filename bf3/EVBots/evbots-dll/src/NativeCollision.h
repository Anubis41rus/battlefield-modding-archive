#pragma once
#include "NativeBlueprint.h"
#include <cstdint>
namespace evbots { namespace collision {
// Snapshot of a real native collision. No Lua is invoked on the physics thread.
// otherWeak owns one native weak-token reference until Release(). Both materials
// own intrusive DC references, transferred to Lua or queued for engine release.
struct Snapshot {
 blueprint::Handle self;
 std::uintptr_t other=0,otherWeak=0,material=0,ownMaterial=0;
 float position[3]={},normal[3]={},speed=0;
 std::uint32_t part=0;
 std::uint32_t epoch=0;
};
bool Watch(const blueprint::Handle& entity,const char** error);
void Unwatch(const blueprint::Handle& entity);
void ClearWatches(); // Lua shutdown queues all references; no engine frees here.
bool Poll(Snapshot* out);
void Release(Snapshot* value);
// Copies into the caller's userdata; only on the existing engine/Lua frame.
bool OtherHandle(const Snapshot& value,blueprint::Handle* out);
} }
