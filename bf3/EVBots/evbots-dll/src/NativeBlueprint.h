// Native Blueprint factory and lifetime tracking, build 267. No Lua or game startup here.
#pragma once
#include <cstdint>
#include <vector>

namespace evbots { namespace blueprint {
struct Handle {
    std::uintptr_t pointer = 0;
    std::uint64_t generation = 0;
};
struct Params {
    // Frostbite LinearTransform: four padded Vec3 rows, 16-byte aligned at the call site.
    float transform[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,0};
    std::uint32_t variation = 0;
    bool networked = false;
    std::uintptr_t parentRepresentative = 0;
};
struct Result { Handle bus; std::vector<Handle> entities; };

// Call from the real client frame callback immediately around its Lua work, including
// timers/net events. Never call this from a worker merely to bypass the thread guard.
void EnterClientFrame();
void LeaveClientFrame();

// Installs the bus lifecycle hook after checking every native entrypoint as one batch.
// EVBOTS_BLUEPRINT=0 disables creation/mutation. Does not start or inject a game.
bool Ready(const char** error);
bool Create(std::uintptr_t liveBlueprint, const Params& params, Result* out, const char** error);
// ownedData must come from a retained native DataContainer userdata, never a Lua flag.
bool CreateSingle(std::uintptr_t liveEntityData, bool ownedData, const Params& params, Handle* out, const char** error);
bool IsAlive(const Handle& entity);
bool IsBusAlive(const Handle& bus);
bool Init(const Handle& entity, int realm, bool enableAllowed, bool isGhost, const char** error);
bool Destroy(const Handle& entity, const char** error);
bool FireEvent(const Handle& entity, std::uint32_t id, const char** error);
bool ReadEntity(const Handle& entity, std::uintptr_t* type, std::uintptr_t* data, float* transform12);
// Exact-type snapshot for verified server round/player-event classes. Uses the stock
// iterator under the frame/TLS guard; other names fail instead of returning fake empty data.
bool ListRoundEntities(const char* typeName,std::vector<Handle>* out,const char** error);
// Client: resolve an exact native Entity class. Server: preserve the round-control adapter.
bool ListEntities(const char* typeName,std::vector<Handle>* out,const char** error);
using EntityContextCallback = bool(*)(std::uintptr_t,void*,const char**);
// The raw peer is valid only during callback; context is restored even after native SEH.
bool WithEntityContext(const Handle& entity,EntityContextCallback callback,void* user,const char** error);
bool IsClient();
bool FrameActive();
// Frame ownership only, independent of optional Blueprint factory activation.
bool EngineFrameActive();
// Native event queues retain weak tokens only; they never hold a level/bus alive.
std::uintptr_t CaptureWeakPeer(std::uintptr_t peer);
void DropWeakPeer(std::uintptr_t token);
void DrainWeakPeers();
bool AdoptWeakPeer(std::uintptr_t peer,std::uintptr_t token,Handle* out);
} }
