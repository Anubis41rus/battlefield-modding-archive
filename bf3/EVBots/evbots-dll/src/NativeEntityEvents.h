#pragma once
#include "NativeBlueprint.h"
#include <cstdint>

namespace evbots { namespace entityevents {
struct Ticket { std::uint64_t watch=0,firstSequence=0; };
struct Snapshot {
    blueprint::Handle entity;
    std::uintptr_t weak=0;
    std::uint64_t watch=0,sequence=0,epoch=0;
    std::uint32_t eventId=0;
    char type[96]={};
};
// true: delivered synchronously; false: original executes and notification queues.
// A synchronous dispatcher may change eventId or request cancellation.
using DispatchFn=bool(*)(Snapshot*,bool* cancel);
void Start(DispatchFn dispatcher);
bool Watch(const blueprint::Handle& entity,Ticket* ticket,const char** error);
void Unwatch(const blueprint::Handle& entity,std::uint64_t watch);
bool Poll(Snapshot* out);
bool Current(const Snapshot& value);
void Release(Snapshot* value);
// Recognize exactly our installed thunk and its verified stock predecessor.
// Callers still dispatch through currentHandler, preserving callbacks.
bool OwnsHandler(std::uintptr_t slot,std::uintptr_t currentHandler,std::uintptr_t stockHandler);
// Removes subscriptions, releases queued weak references, restores owned slots.
// Hook records stay alive so an already-entered native thunk can call its original.
void Shutdown();
} }
