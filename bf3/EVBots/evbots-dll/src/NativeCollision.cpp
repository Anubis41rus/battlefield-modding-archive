// Build 267. VU notification registration: server 10D049A0/10D04B00,
// client 10CC5E20/10CC5F80 -> 109ED250(vtable index 3E / 3C).
// Dedicated ServerBangerEntity maps 3E to 40, independently confirmed by
// caller 00D9C30F and vtable 025ADC10+100 -> 00D81740. Client: +F0 ->00F89440.
#include "NativeCollision.h"
#include "EngineResources.h"
#include "InlineHook.h"
#include "EVBotsLog.h"
#include "VextResourceManager.h"
#include <unordered_map>
#include <cstring>
namespace evbots { namespace blueprint {
// Implemented by the Blueprint lifetime layer; none of these expose raw pointers
// to Lua. Capture requires existing engine TLS and only retains a weak token.
std::uintptr_t CaptureWeakPeer(std::uintptr_t peer);
void DropWeakPeer(std::uintptr_t token);
bool AdoptWeakPeer(std::uintptr_t peer,std::uintptr_t token,Handle* out);
} namespace collision { namespace {
using Ptr=std::uintptr_t;
using CollisionFn=void(__fastcall*)(void*,void*,void*);
InlineHook g_hook;
CollisionFn g_original=nullptr;
Ptr g_base=0;
bool g_client=false;
int g_ready=0;
SRWLOCK g_gate=SRWLOCK_INIT;
std::unordered_map<Ptr,blueprint::Handle> g_watch;
constexpr unsigned kCapacity=1024;
Snapshot g_queue[kCapacity];
std::vector<Ptr> g_retiredWeak;
unsigned g_read=0,g_write=0;
std::uint32_t g_epoch=1;
volatile LONG g_overflow=0,g_badInfo=0;
Ptr Va(Ptr p) { return g_base+p-0x400000; }
bool Fail(const char** e,const char* why) { if(e)*e=why;return false; }
bool Disabled() { char v[4]={};return EnvGetCached("EVBOTS_COLLISIONS",v,sizeof(v))==1 && v[0]=='0'; }
bool ReadInfo(void* raw,Snapshot* out) {
 __try {
  const auto* p=static_cast<const unsigned char*>(raw);
  out->other=*reinterpret_cast<const Ptr*>(p+(g_client?0x40:0));
  out->material=*reinterpret_cast<const Ptr*>(p+(g_client?0:0xC));
  out->ownMaterial=*reinterpret_cast<const Ptr*>(p+(g_client?4:0x10));
  std::memcpy(out->position,p+(g_client?0x10:0x20),12);
  std::memcpy(out->normal,p+(g_client?0x20:0x30),12);
  out->speed=*reinterpret_cast<const float*>(p+(g_client?0x30:0x40));
  out->part=*reinterpret_cast<const std::uint32_t*>(p+(g_client?0x4C:0x44));
  return true;
 } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void DeferRelease(Snapshot* value) {
 // Physics/Lua-close threads may only enqueue; native allocator frees happen
 // when Poll runs on the proven engine frame, or in ResourceDrainDeferred.
 if(value->otherWeak) {
  AcquireSRWLockExclusive(&g_gate);g_retiredWeak.push_back(value->otherWeak);ReleaseSRWLockExclusive(&g_gate);
  value->otherWeak=0;
 }
 ResourceQueueRelease(static_cast<unsigned long>(value->material));value->material=0;
 ResourceQueueRelease(static_cast<unsigned long>(value->ownMaterial));value->ownMaterial=0;
}
void Capture(Ptr self,void* info) {
 Snapshot value;
 AcquireSRWLockShared(&g_gate);
 const auto i=g_watch.find(self);
 if(i!=g_watch.end()) value.self=i->second;
 value.epoch=g_epoch;
 ReleaseSRWLockShared(&g_gate);
 if(!value.self.pointer) return;
 if(!ReadInfo(info,&value)) { InterlockedIncrement(&g_badInfo);return; }
 const Ptr material=value.material,ownMaterial=value.ownMaterial;
 value.material=value.ownMaterial=0;
 if(material) {
  if(!ResourceRetainLive(static_cast<unsigned long>(material))) { InterlockedIncrement(&g_badInfo);return; }
  value.material=material;
 }
 if(ownMaterial) {
  if(!ResourceRetainLive(static_cast<unsigned long>(ownMaterial))) { DeferRelease(&value);InterlockedIncrement(&g_badInfo);return; }
  value.ownMaterial=ownMaterial;
 }
 if(value.other) {
  value.otherWeak=blueprint::CaptureWeakPeer(value.other);
  if(!value.otherWeak) { DeferRelease(&value);InterlockedIncrement(&g_badInfo);return; }
 }
 AcquireSRWLockExclusive(&g_gate);
 const unsigned next=(g_write+1)%kCapacity;
 const bool full=next==g_read || value.epoch!=g_epoch;
 if(!full) { g_queue[g_write]=value; g_write=next; }
 ReleaseSRWLockExclusive(&g_gate);
 if(full) { DeferRelease(&value); InterlockedIncrement(&g_overflow); }
}
void __fastcall OnCollision(void* self,void* edx,void* info) {
 Capture(reinterpret_cast<Ptr>(self),info);
 // Royale registers notification callbacks and never blocks/mutates the contact.
 // Original physics behavior always executes; mutable VU CollisionInfo is separate.
 g_original(self,edx,info);
}
bool Exact(Ptr address,const unsigned char* bytes,unsigned count) {
 __try { return std::memcmp(reinterpret_cast<const void*>(Va(address)),bytes,count)==0; }
 __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Ready(const char** error) {
 if(Disabled()) return Fail(error,"EVBOTS_COLLISIONS=0");
 if(g_ready==1) return true;
 if(g_ready<0) return Fail(error,"collision signature/hook batch is unavailable");
 if(!blueprint::Ready(error)) return false;
 g_client=blueprint::IsClient();
 g_base=reinterpret_cast<Ptr>(GetModuleHandleA(g_client?"bf3.exe":"bf3_server_Final.exe"));
 if(!g_base) return Fail(error,"BF3 executable unavailable for collisions");
 const Ptr method=g_client?0x00F89440:0x00D81740;
 const Ptr slot=g_client?0x02143810+0xF0:0x025ADC10+0x100;
 static const unsigned char clientHead[]={0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x8B,0x45,0x08,0xF3,0x0F,0x10,0x40,0x30,0xF2,0x0F};
 static const unsigned char serverHead[]={0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x81,0xEC,0xD4,0x01,0x00,0x00,0xA1,0x2C,0xEE,0xB3};
 const Ptr target=Va(method);
 const bool body=Exact(method,g_client?clientHead:serverHead,16);
 const bool table=Exact(slot,reinterpret_cast<const unsigned char*>(&target),sizeof(target));
 LOG_INFO("Collision signature %08X: %s; vtable slot %08X: %s",static_cast<unsigned>(method),body?"match":"MISMATCH",static_cast<unsigned>(slot),table?"match":"MISMATCH");
 if(!body || !table) { g_ready=-1;return Fail(error,"collision bytes/slot mismatch; no collision hook installed"); }
 if(g_hook.Install(reinterpret_cast<void*>(target),reinterpret_cast<void*>(&OnCollision),"Blueprint:Collision")!=HookResult::Ok) {
  if(g_hook.IsInstalled())g_hook.Remove();g_ready=-1;return Fail(error,"collision hook installation failed");
 }
 g_original=reinterpret_cast<CollisionFn>(g_hook.Trampoline()); g_ready=1;return true;
}
bool UsesSupportedMethod(Ptr peer) {
 __try {
  const Ptr vtable=*reinterpret_cast<const Ptr*>(peer);
  const Ptr method=*reinterpret_cast<const Ptr*>(vtable+(g_client?0xF0:0x100));
  return method==Va(g_client?0x00F89440:0x00D81740);
 } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} // namespace
bool Watch(const blueprint::Handle& h,const char** error) {
 if(!Ready(error))return false;
 Ptr type=0;
 if(!blueprint::ReadEntity(h,&type,nullptr,nullptr) || !eng::IsKindOfName(static_cast<unsigned long>(type),g_client?"ClientPhysicsEntity":"ServerPhysicsEntity"))
  return Fail(error,"a live PhysicsEntity on the engine frame is required");
 if(!UsesSupportedMethod(h.pointer))return Fail(error,"this PhysicsEntity collision override is not mapped yet");
 AcquireSRWLockExclusive(&g_gate);g_watch[h.pointer]=h;ReleaseSRWLockExclusive(&g_gate);return true;
}
void Unwatch(const blueprint::Handle& h) {
 AcquireSRWLockExclusive(&g_gate);
 const auto i=g_watch.find(h.pointer);
 if(i!=g_watch.end() && i->second.generation==h.generation)g_watch.erase(i);
 ReleaseSRWLockExclusive(&g_gate);
}
void ClearWatches() {
 AcquireSRWLockExclusive(&g_gate);g_watch.clear();++g_epoch;
 while(g_read!=g_write) {
  Snapshot& value=g_queue[g_read];
  if(value.otherWeak)g_retiredWeak.push_back(value.otherWeak);
  ResourceQueueRelease(static_cast<unsigned long>(value.material));
  ResourceQueueRelease(static_cast<unsigned long>(value.ownMaterial));
  value=Snapshot{};g_read=(g_read+1)%kCapacity;
 }
 ReleaseSRWLockExclusive(&g_gate);
}
bool Poll(Snapshot* out) {
 if(!out || !blueprint::FrameActive())return false;
 blueprint::DrainWeakPeers();
 std::vector<Ptr> retired;
 AcquireSRWLockExclusive(&g_gate);retired.swap(g_retiredWeak);ReleaseSRWLockExclusive(&g_gate);
 for(Ptr token:retired)blueprint::DropWeakPeer(token);
 const LONG overflow=InterlockedExchange(&g_overflow,0),bad=InterlockedExchange(&g_badInfo,0);
 if(overflow || bad)LOG_WARN("Collision notifications dropped: queue=%ld invalid info/TLS=%ld",overflow,bad);
 AcquireSRWLockExclusive(&g_gate);
 const bool have=g_read!=g_write;
 if(have) { *out=g_queue[g_read];g_queue[g_read]=Snapshot{};g_read=(g_read+1)%kCapacity;if(out->epoch!=g_epoch)out->self={}; }
 ReleaseSRWLockExclusive(&g_gate);return have;
}
void Release(Snapshot* value) {
 if(!value)return;
 if(!blueprint::FrameActive()) { DeferRelease(value);return; }
 blueprint::DropWeakPeer(value->otherWeak);value->otherWeak=0;
 ResourceQueueRelease(static_cast<unsigned long>(value->material));value->material=0;
 ResourceQueueRelease(static_cast<unsigned long>(value->ownMaterial));value->ownMaterial=0;
}
bool OtherHandle(const Snapshot& value,blueprint::Handle* out) {
 return value.other && value.otherWeak && blueprint::AdoptWeakPeer(value.other,value.otherWeak,out);
}
} }
