// Build 267, static evidence: veniceext 10C83C60 / 10C8CDF0 (c34ad182...912ee8),
// zbf3.exe 8201c5e1...42b9, dedicated f39f7623...5470. No VU-private bus fields:
// VU appends its entity list beyond the stock allocation. Use the native factory
// output vector and native weak tokens instead; never read those VU-only fields.
#include "NativeBlueprint.h"
#include "EngineResources.h"
#include "InlineHook.h"
#include "VextEntityManager.h"
#include "EVBotsLog.h"
#include <intrin.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace evbots { namespace blueprint { namespace {
using Ptr = std::uintptr_t;
struct Signature { Ptr va; unsigned char bytes[16]; };
enum Entry { ParamsCtor, CreateBus, PeerCtor, PeerDtor, BusDtor, EntityInit, EntityDestroy, SetContext, GetContext,
  WeakAcquire, WeakFree, VectorFree, RegistryId, RegistryLookup, IdCompartment, GetSubLevel, NetworkBus, IteratorBegin, IteratorNext, SingleFactory };
const Signature kClient[] = {
 {0x00F89130,{0x8B,0x44,0x24,0x08,0x53,0x55,0x56,0x8B,0xE9,0x33,0xDB,0x57,0x53,0x89,0x5D,0x00}},
 {0x00FA4A60,{0x8B,0x4C,0x24,0x0C,0x33,0xC0,0x38,0x05,0xA4,0x13,0x38,0x02,0x0F,0x94,0xC0,0x33}},
 {0x0054B890,{0x8B,0xC1,0x33,0xC9,0x55,0x8B,0x6C,0x24,0x08,0x56,0x57,0xC7,0x00,0xF0,0x8D,0x07}},
 {0x0054B920,{0x56,0x8B,0xF1,0x8D,0x4E,0x04,0xC7,0x06,0xF0,0x8D,0x07,0x02,0xE8,0x9F,0xD4,0xFD}},
 {0x00552770,{0x83,0xEC,0x08,0x56,0x8B,0xF1,0x8B,0x46,0x14,0x8B,0xC8,0x81,0xE1,0x00,0x00,0x00}},
 {0x005459D0,{0x8B,0x41,0x08,0x8B,0xD0,0xC1,0xEA,0x03,0xF6,0xC2,0x01,0x75,0x0F,0x0D,0x08,0x08}},
 {0x007A64B0,{0x8B,0x44,0x24,0x08,0x8B,0x4C,0x24,0x04,0x50,0x51,0x8B,0x0D,0xBC,0xF4,0x34,0x02}},
 {0x0118D020,{0xA1,0xE0,0x79,0x39,0x02,0x64,0x8B,0x0D,0x2C,0x00,0x00,0x00,0x8B,0x14,0x81,0x8B}},
 {0x0118D040,{0xA1,0xE0,0x79,0x39,0x02,0x64,0x8B,0x0D,0x2C,0x00,0x00,0x00,0x8B,0x14,0x81,0x8B}},
 {0x0058C400,{0x56,0x8B,0xF1,0x57,0x8B,0x06,0x8B,0xC8,0x83,0xE0,0xFE,0x83,0xC9,0x01,0x8B,0xD0}},
 {0x00425930,{0x56,0x8B,0x74,0x24,0x08,0x85,0xF6,0x74,0x35,0x8D,0x44,0x24,0x08,0x50,0x56,0xE8}},
 {0x004957E0,{0x8B,0x44,0x24,0x04,0x8B,0x09,0x50,0xE8,0xA4,0x2A,0xF8,0xFF,0xC2,0x08,0x00,0xCC}},
 {0x00574D60,{0x53,0x8B,0x5C,0x24,0x0C,0x56,0x8B,0xF1,0x85,0xDB,0x75,0x0B,0x8B,0x44,0x24,0x0C}},
 {0x00574CF0,{0x83,0x7C,0x24,0x04,0x00,0x57,0x8B,0xF9,0x75,0x06,0x33,0xC0,0x5F,0xC2,0x04,0x00}},
 {0x005629F0,{0x56,0x8B,0x74,0x24,0x08,0xB9,0x00,0x1A,0x35,0x02,0xC1,0xEE,0x18,0xE8,0x7E,0xD5}},
 {0x011961D0,{0x8B,0x54,0x24,0x04,0x8B,0xC1,0x39,0x50,0x38,0x74,0x1E,0x8B,0x48,0x2C,0x85,0xC9}},
 {0x005567E0,{0x56,0x8B,0x74,0x24,0x08,0x57,0x6A,0x10,0x8B,0xF9,0x8B,0x4E,0x34,0x6A,0x28,0xE8}},
  {0x00549A90,{0x0F,0xB7,0x81,0x34,0x08,0x00,0x00,0x8B,0x54,0x24,0x04,0x0F,0xB7,0x52,0x14,0x2B}},
  {0x00549A40,{0x83,0x79,0x04,0x00,0x74,0x36,0x8B,0x41,0x10,0x3B,0x41,0x14,0x72,0x31,0x8B,0x51}},
  {0x00F9AAF0,{0x83,0xEC,0x18,0x80,0x3D,0xA4,0x13,0x38,0x02,0x00,0x75,0x06,0x33,0xC0,0x83,0xC4}},
};
const Signature kServer[] = {
 {0x00D90080,{0x8B,0x44,0x24,0x08,0x53,0x55,0x56,0x8B,0xE9,0x57,0x33,0xDB,0x6A,0x01,0x89,0x5D}},
 {0x00DB3530,{0x33,0xC0,0x38,0x05,0x78,0x35,0xB4,0x02,0x53,0x8B,0x5C,0x24,0x10,0x0F,0x94,0xC0}},
 {0x005D1B40,{0x55,0x56,0x8B,0xF1,0x33,0xC9,0xC7,0x06,0x68,0xE2,0x46,0x02,0xC7,0x46,0x04,0x00}},
 {0x005D1C00,{0x81,0xEC,0x30,0x01,0x00,0x00,0x56,0x8B,0xF1,0x8B,0x46,0x08,0xC1,0xE8,0x03,0xC7}},
 {0x005DBE10,{0x83,0xEC,0x08,0x56,0x8B,0xF1,0x8B,0x46,0x14,0x8B,0xC8,0x81,0xE1,0x00,0x00,0x00}},
 {0x005D4840,{0x81,0xEC,0x3C,0x03,0x00,0x00,0x56,0x8B,0xF1,0xF7,0x46,0x08,0x00,0x01,0x00,0x00}},
 {0x00945D10,{0x8B,0x44,0x24,0x08,0x8B,0x4C,0x24,0x04,0x50,0x51,0x8B,0x0D,0xF0,0x98,0xB1,0x02}},
 {0x0083D5C0,{0xA1,0x74,0xAA,0xB6,0x02,0x64,0x8B,0x0D,0x2C,0x00,0x00,0x00,0x8B,0x14,0x81,0x8B}},
 {0x0083D5E0,{0xA1,0x74,0xAA,0xB6,0x02,0x64,0x8B,0x0D,0x2C,0x00,0x00,0x00,0x8B,0x14,0x81,0x8B}},
 {0x0061CBA0,{0x56,0x8B,0xF1,0x57,0x8B,0x06,0x8B,0xC8,0x83,0xE0,0xFE,0x83,0xC9,0x01,0x8B,0xD0}},
 {0x0043EBE0,{0x56,0x8B,0x74,0x24,0x08,0x85,0xF6,0x74,0x37,0x8D,0x44,0x24,0x08,0x50,0x56,0xE8}},
 {0x00532A60,{0x56,0x8B,0xF1,0x83,0x3E,0x00,0x75,0x21,0x68,0x7A,0x59,0x43,0x02,0x68,0x44,0x69}},
 {0x00602A30,{0x81,0xEC,0x88,0x01,0x00,0x00,0x55,0x8B,0xAC,0x24,0x94,0x01,0x00,0x00,0x56,0x8B}},
 {0x00602930,{0x81,0xEC,0x30,0x01,0x00,0x00,0x83,0xBC,0x24,0x34,0x01,0x00,0x00,0x00,0x57,0x8B}},
 {0x005EFBB0,{0x81,0xEC,0x30,0x01,0x00,0x00,0x56,0x8B,0xB4,0x24,0x38,0x01,0x00,0x00,0xB9,0x30}},
 {0x0084AB10,{0x8B,0x54,0x24,0x04,0x8B,0xC1,0x39,0x50,0x38,0x74,0x1E,0x8B,0x48,0x2C,0x85,0xC9}},
 {0x005E1BA0,{0x56,0x8B,0x74,0x24,0x08,0x57,0x6A,0x00,0x6A,0x10,0x8B,0xF9,0x8B,0x4E,0x34,0x6A}},
  {0x005CF340,{0x0F,0xB7,0x81,0x34,0x08,0x00,0x00,0x81,0xEC,0x30,0x01,0x00,0x00,0x53,0x8B,0x9C}},
  {0x005CF2F0,{0x83,0x79,0x04,0x00,0x74,0x36,0x8B,0x41,0x10,0x3B,0x41,0x14,0x72,0x31,0x8B,0x51}},
  {0x00DA36E0,{0x83,0xEC,0x18,0x80,0x3D,0x78,0x35,0xB4,0x02,0x00,0x75,0x06,0x33,0xC0,0x83,0xC4}},
};
const Signature* g_code = nullptr;
Ptr g_base = 0;
bool g_client = false;
int g_ready = 0;
InlineHook g_busDtorHook;
using DtorFn = void(__fastcall*)(void*,void*);
DtorFn g_busDtor = nullptr;
SRWLOCK g_lock = SRWLOCK_INIT;
struct Record { std::uint64_t generation; Ptr vtable; Ptr bus; Ptr weak; };
std::unordered_map<Ptr,Record> g_entities, g_buses;
std::vector<Ptr> g_pendingWeak;
std::uint64_t g_nextGeneration = 1;
struct PeerPair { Ptr peer; std::uint32_t flags; };
// VU 10C8BF80, native append 00952A20/009F9F00, native grow 00950550/009F6F90.
struct Collector { PeerPair* begin; PeerPair* end; PeerPair* capacity; Ptr allocator; PeerPair* fixed; PeerPair entries[64]; };
static_assert(offsetof(Collector,entries)==0x14 && sizeof(PeerPair)==8,"x86 factory collector ABI");
thread_local unsigned g_clientFrame = 0;
volatile LONG g_clientThread = 0;

Ptr Va(Ptr p) { return g_base + p - 0x400000; }
Ptr U32(Ptr p) { return *reinterpret_cast<const std::uint32_t*>(p); }
bool ReadU32(Ptr p,Ptr* out) {
 __try { *out=U32(p); return true; }
 __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Fail(const char** e, const char* text) { if (e) *e=text; return false; }
bool Disabled() { char v[4]={}; return EnvGetCached("EVBOTS_BLUEPRINT",v,sizeof(v))==1 && v[0]=='0'; }
bool Frame() {
 return g_client ? (g_clientFrame!=0 && static_cast<DWORD>(g_clientThread)==GetCurrentThreadId())
                 : (EVBotsServerFrameTickActive() && EVBotsOnServerFrameThread());
}
bool SafeCode(const Signature& s) {
 __try { return std::memcmp(reinterpret_cast<void*>(Va(s.va)),s.bytes,sizeof(s.bytes))==0; }
 __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void ReleaseWeak(Ptr weak) {
 if(!weak) return;
 __try {
  if(InterlockedDecrement(reinterpret_cast<volatile LONG*>(weak+4))==0)
   reinterpret_cast<void(__cdecl*)(Ptr)>(Va(g_code[WeakFree].va))(weak);
 } __except(EXCEPTION_EXECUTE_HANDLER) { LOG_ERR("Blueprint: weak-token release failed"); }
}
bool WeakAlive(Ptr weak,Ptr peer) {
 __try { return weak && U32(weak)==peer+4; }
 __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
Ptr AcquireWeak(Ptr peer) {
 Ptr result=0;
 __try {
  using Fn=Ptr*(__fastcall*)(void*,void*,Ptr*,Ptr);
  reinterpret_cast<Fn>(Va(g_code[WeakAcquire].va))(reinterpret_cast<void*>(peer+4),nullptr,&result,peer);
 } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
 return result;
}
void InvalidateBus(Ptr p) {
 AcquireSRWLockExclusive(&g_lock);
 g_buses.erase(p);
 for (auto i=g_entities.begin();i!=g_entities.end();) {
  if(i->second.bus==p) { ReleaseWeak(i->second.weak); i=g_entities.erase(i); } else ++i;
 }
 ReleaseSRWLockExclusive(&g_lock);
}
void __fastcall BusDtorDetour(void* self,void* edx) { InvalidateBus(reinterpret_cast<Ptr>(self)); g_busDtor(self,edx); }

// 02B43578 gates creation only (SingleFactory 00DA36E0, CreateBus 00DB3530).
// Stock IteratorBegin 005CF340 / Next 005CF2F0 never read it, so traversal must not.
bool FactoryEnabled() {
 return *reinterpret_cast<const unsigned char*>(Va(g_client?0x023813A4:0x02B43578))!=0;
}
bool Context(Ptr* context,Ptr* level,Ptr* root,const char** error) {
 if(!Frame()) return Fail(error,"Blueprint requires the real engine frame thread");
 __try {
  const Ptr index=U32(Va(g_client?0x023979E0:0x02B6AA74));
  const Ptr tlsVector=__readfsdword(0x2C);
  if(!tlsVector || index>4096) return Fail(error,"engine static TLS is unavailable");
  const Ptr tls=U32(tlsVector+index*4);
  if(!tls) return Fail(error,"engine static TLS block is unavailable");
  // Stock HavokSingleton indexes its per-thread table using these TLS fields.
  // Physics entity operations retain this guard; ordinary DC allocation does not.
  if(!U32(tls+(g_client?0x230:0x66C))) return Fail(error,"Havok physics thread memory is not initialized on this thread");
  const Ptr world=U32(Va(g_client?0x023813A0:0x02B3EE30));
  if(!world || *reinterpret_cast<const unsigned char*>(world+0x83C))
   return Fail(error,"entity world is unavailable or locked for traversal");
  Ptr ctx=0;
  if(g_client) { const Ptr main=U32(Va(0x022E3790)); if(main) { const Ptr client=U32(main+0xB4); if(client) ctx=U32(client+0xC); } }
  else ctx=U32(Va(0x02B3EE2C));
  if(!ctx) return Fail(error,"game context is unavailable");
  const Ptr lev=U32(ctx+0x10);
  if(!lev || *reinterpret_cast<const unsigned char*>(lev+0x44)) return Fail(error,"level is unavailable or destroying");
  const Ptr bus=U32(lev+(g_client?0x5C:0x60));
  if(!bus || !U32(bus+0x10)) return Fail(error,"level root bus is unavailable");
  if(U32(bus+4)!=lev) return Fail(error,"root bus does not belong to the active level");
  *context=ctx; *level=lev; *root=bus; return true;
 } __except(EXCEPTION_EXECUTE_HANDLER) { return Fail(error,"game context/TLS read failed"); }
}
using ContextGetFn=Ptr(__cdecl*)();
using ContextSetFn=void(__cdecl*)(Ptr);
// Dedicated getId asserts on an unknown pointer. Preflight both sorted maps under
// the same recursive native critical section before entering that function.
Ptr FindMap(Ptr begin,Ptr end,Ptr key) {
 if(!begin || end<begin || ((end-begin)&7) || end-begin>8*1024*1024 || (begin&3)) return 0;
 const Ptr limit=end;
 while(begin<end) {
  const Ptr mid=begin+((end-begin)/16)*8;
  if(U32(mid)<key) begin=mid+8; else end=mid;
 }
 return begin<limit && U32(begin)==key ? U32(begin+4) : 0;
}
bool NetworkPreflight(Ptr* blueprint,Ptr level,Ptr* sublevel,const char** error) {
 if(g_client) return Fail(error,"networked Blueprint creation belongs to the server realm");
 auto* lock=reinterpret_cast<CRITICAL_SECTION*>(Va(0x02B1C430));
 bool locked=false,ok=false;
 __try {
  if(!U32(Va(0x02B1BBCC))) return Fail(error,"network registry is outside initLevel/exitLevel");
  EnterCriticalSection(lock); locked=true;
  if(!U32(Va(0x02B1BBCC))) Fail(error,"network registry became inactive");
  else {
   const Ptr registry=Va(0x02845478);
   const Ptr id=FindMap(U32(registry+0x14),U32(registry+0x18),*blueprint);
   const Ptr data=id?FindMap(U32(registry),U32(registry+4),id):0;
   unsigned compartment=0;
   if(data) for(unsigned i=0;i<202;++i) if(U32(Va(0x02B1BBD0)+i*4)==(id>>24)) { compartment=i+3; break; }
   if(!id || !data || !compartment) Fail(error,"Blueprint is not present in the active network registry/compartment table");
   else {
    Ptr checkedId=0;
    using IdFn=Ptr*(__fastcall*)(void*,void*,Ptr*,Ptr);
    reinterpret_cast<IdFn>(Va(g_code[RegistryId].va))(reinterpret_cast<void*>(registry),nullptr,&checkedId,*blueprint);
    const Ptr checkedData=reinterpret_cast<Ptr(__fastcall*)(void*,void*,Ptr)>(Va(g_code[RegistryLookup].va))(reinterpret_cast<void*>(registry),nullptr,id);
    const Ptr checkedComp=reinterpret_cast<Ptr(__cdecl*)(Ptr)>(Va(g_code[IdCompartment].va))(id);
    if(checkedId!=id || checkedData!=data || checkedComp!=compartment) Fail(error,"network registry changed during its locked preflight");
    else {
     const Ptr sub=reinterpret_cast<Ptr(__fastcall*)(void*,void*,Ptr)>(Va(g_code[GetSubLevel].va))(reinterpret_cast<void*>(level),nullptr,compartment);
     if(!sub || *reinterpret_cast<const unsigned char*>(sub+0x44) || !U32(sub+0x34)) Fail(error,"network Blueprint sublevel/allocator is unavailable");
     else if(!U32(Va(0x02B19948))) Fail(error,"network entity manager is unavailable");
     else { *blueprint=data; *sublevel=sub; ok=true; }
    }
   }
  }
 } __except(EXCEPTION_EXECUTE_HANDLER) { Fail(error,"network Blueprint preflight raised an exception"); }
 if(locked) LeaveCriticalSection(lock);
 return ok;
}
void FreeCollector(Collector* c) {
 __try {
  if(c->begin && c->begin!=c->fixed) {
   if(!c->allocator) { LOG_ERR("Blueprint: dynamic collector has no allocator; refusing invalid free"); return; }
   using Fn=void(__fastcall*)(Ptr*,void*,void*,unsigned);
   reinterpret_cast<Fn>(Va(g_code[VectorFree].va))(&c->allocator,nullptr,c->begin,static_cast<unsigned>(reinterpret_cast<Ptr>(c->capacity)-reinterpret_cast<Ptr>(c->begin)));
  }
 } __except(EXCEPTION_EXECUTE_HANDLER) { LOG_ERR("Blueprint: native collector release failed"); }
 c->begin=c->end=c->capacity=c->fixed;
}
// Only POD locals in functions containing SEH. Always restore the previous context.
bool NativeCreate(Ptr blueprint,const Params& p,Ptr context,Ptr level,Ptr root,Ptr sublevel,Collector* collector,Ptr* result,const char** error) {
 alignas(16) unsigned char params[0xD0]={};
 alignas(16) float transform[16];
 std::memcpy(transform,p.transform,sizeof(transform));
 Ptr previous=0,networkRoot=0; bool switched=false; bool ok=false;
 __try {
  previous=reinterpret_cast<ContextGetFn>(Va(g_code[GetContext].va))();
  reinterpret_cast<ContextSetFn>(Va(g_code[SetContext].va))(context); switched=true;
  if(p.networked) {
   using NetFn=Ptr(__fastcall*)(void*,void*,Ptr,Ptr,Ptr,Ptr);
   root=reinterpret_cast<NetFn>(Va(g_code[NetworkBus].va))(reinterpret_cast<void*>(U32(Va(0x02B19948))),nullptr,sublevel,root,0,0);
   networkRoot=root;
  }
  if(!root) Fail(error,"native network bus allocation failed");
  else {
  using FactoryCtor=void*(__fastcall*)(void*,void*,Ptr,Ptr,const float*);
  reinterpret_cast<FactoryCtor>(Va(g_code[ParamsCtor].va))(params,nullptr,0,root,transform);
  *reinterpret_cast<Collector**>(params+8)=collector;
  *reinterpret_cast<std::uint32_t*>(params+(g_client?0xA0:0x9C))=p.variation;
  if(g_client) *reinterpret_cast<std::uint32_t*>(params+0x94)=static_cast<std::uint32_t>(U32(level+0x94));
  else {
   params[6]=p.networked?0:1;
   *reinterpret_cast<std::uint32_t*>(params+0x94)=static_cast<std::uint32_t>(U32(context+0x3C));
  }
  using Factory=Ptr*(__cdecl*)(Ptr*,void*,Ptr,Ptr);
  reinterpret_cast<Factory>(Va(g_code[CreateBus].va))(result,params,blueprint,p.parentRepresentative);
  ok=true;
  }
  // The network root starts at refcount zero (005DFED0). A successful child
  // bus takes its parent reference. Clean up an unused root on a normal failure.
  if(networkRoot && U32(networkRoot+0x10)==0) BusDtorDetour(reinterpret_cast<void*>(networkRoot),nullptr);
 } __except(EXCEPTION_EXECUTE_HANDLER) { Fail(error,"native Blueprint factory raised an exception"); }
 if(switched) __try { reinterpret_cast<ContextSetFn>(Va(g_code[SetContext].va))(previous); }
 __except(EXCEPTION_EXECUTE_HANDLER) { ok=false; Fail(error,"restoring engine context failed"); }
 return ok;
}
// VU 10C84524..593 / 10C8D6F4..737: the single-data overload uses this
// constructor and variation/context fields, without the Blueprint networked flag.
bool SingleCreatorReady(Ptr data,const char** error) {
 if(g_client)return true;
 // Dedicated 00DA0AE7 obtains this creator through EntityData vtable+1C;
 // 00DA0B43 asserts (possibly int3) when its initialized flag +1C is clear.
 const Ptr getter=U32(U32(data)+0x1C);
 if(!IsExecutable(reinterpret_cast<void*>(getter)))return Fail(error,"EntityData creator getter is not executable");
 const Ptr creator=reinterpret_cast<Ptr(__fastcall*)(void*,void*)>(getter)(reinterpret_cast<void*>(data),nullptr);
 if(!creator || !*reinterpret_cast<const unsigned char*>(creator+0x1C))return Fail(error,"native EntityData creator is unavailable or not initialized");
 return true;
}
bool SingleDataReady(Ptr data,bool ownedData,const char** error) {
 const auto type=eng::TypeOf(static_cast<unsigned long>(data));
 if(!data || !type || !eng::IsKindOfName(type,"EntityData"))return Fail(error,"a live native EntityData is required");
 Ptr flags=0;
 if(!ReadU32(data+4,&flags) || (flags&0x08000000))return Fail(error,"EntityData is marked destroyed");
 if(ownedData) {
  if(!(flags&0xFFFF))return Fail(error,"owned EntityData has no retained native reference");
 } else {
  eng::Guid16 guid={};char text[40]={};
  const auto partition=eng::PartitionOfInstance(static_cast<unsigned long>(data));
  if(!partition || !eng::InstanceGuid(static_cast<unsigned long>(data),&guid))
   return Fail(error,"borrowed EntityData must belong to a live partition/GUID");
  eng::FormatGuid(guid,text);
  if(eng::FindInstanceInPartition(partition,text)!=data)
   return Fail(error,"borrowed EntityData is no longer current in its partition/GUID");
 }
 return true;
}
bool NativeCreateSingle(Ptr data,const Params& p,Ptr context,Ptr level,Ptr root,Ptr* result,const char** error) {
 alignas(16) unsigned char params[0xD0]={};
 alignas(16) float transform[16];std::memcpy(transform,p.transform,sizeof(transform));
 Ptr previous=0;bool switched=false,ok=false;
 __try {
  previous=reinterpret_cast<ContextGetFn>(Va(g_code[GetContext].va))();
  reinterpret_cast<ContextSetFn>(Va(g_code[SetContext].va))(context);switched=true;
  if(SingleCreatorReady(data,error)) {
   using Ctor=void*(__fastcall*)(void*,void*,Ptr,Ptr,const float*);
   reinterpret_cast<Ctor>(Va(g_code[ParamsCtor].va))(params,nullptr,data,root,transform);
   *reinterpret_cast<std::uint32_t*>(params+(g_client?0xA0:0x9C))=p.variation;
   *reinterpret_cast<std::uint32_t*>(params+0x94)=static_cast<std::uint32_t>(U32(g_client?level+0x94:context+0x3C));
   *result=reinterpret_cast<Ptr(__cdecl*)(void*)>(Va(g_code[SingleFactory].va))(params);
   ok=true;
  }
 } __except(EXCEPTION_EXECUTE_HANDLER) { Fail(error,"native single-entity factory raised an exception"); }
 if(switched) __try { reinterpret_cast<ContextSetFn>(Va(g_code[SetContext].va))(previous); }
 __except(EXCEPTION_EXECUTE_HANDLER) { ok=false;Fail(error,"restoring engine context after single creation failed"); }
 return ok;
}
void ReleaseTemporary(Ptr bus) {
 if(!bus) return;
 __try {
  auto* refs=reinterpret_cast<std::uint32_t*>(bus+0x10);
  if(*refs && --*refs==0) BusDtorDetour(reinterpret_cast<void*>(bus),nullptr);
 } __except(EXCEPTION_EXECUTE_HANDLER) { LOG_ERR("Blueprint: temporary bus release failed at %08X",static_cast<unsigned>(bus)); }
}
Ptr PeerBus(Ptr peer) {
 __try {
  const Ptr f=U32(U32(peer)+(g_client?0x18:0x1C));
  if(!IsExecutable(reinterpret_cast<void*>(f))) return 0;
  return reinterpret_cast<Ptr(__fastcall*)(void*,void*)>(f)(reinterpret_cast<void*>(peer),nullptr);
 } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}
bool Check(const Handle& h,bool bus,Record* out=nullptr) {
 if(!h.pointer || !h.generation) return false;
 bool found=false;
 AcquireSRWLockShared(&g_lock);
 const auto& map=bus?g_buses:g_entities;
 const auto i=map.find(h.pointer);
 if(i!=map.end() && i->second.generation==h.generation && (bus || WeakAlive(i->second.weak,h.pointer))) { if(out) *out=i->second; found=true; }
 ReleaseSRWLockShared(&g_lock);
 return found;
}
bool EntityAccess(const Handle& h,Ptr* context,const char** error) {
 Ptr level=0,root=0; Record record={};
 if(Disabled()) return Fail(error,"EVBOTS_BLUEPRINT=0");
 if(!Context(context,&level,&root,error)) return false;
 if(!Check(h,false,&record)) return Fail(error,"entity handle was invalidated by native destruction");
 __try {
  if(U32(h.pointer)!=record.vtable) return Fail(error,"entity vtable no longer matches its generation");
  if(U32(h.pointer+8)&0x80000) return Fail(error,"entity is pending native destruction");
  const Ptr bus=PeerBus(h.pointer);
  if(bus!=record.bus || !bus || *reinterpret_cast<unsigned char*>(U32(bus+4)+0x44))
   return Fail(error,"entity bus is no longer active");
 } __except(EXCEPTION_EXECUTE_HANDLER) { return Fail(error,"entity memory is no longer readable"); }
 return true;
}
Ptr __fastcall EventType(void*,void*) { return Va(g_client?0x023C6F78:0x02BE74F4); }
void* g_eventVtable[]={reinterpret_cast<void*>(&EventType)};
struct NativeEvent { void** vtable; std::uint32_t id; std::uint32_t flags; };
static_assert(sizeof(NativeEvent)==12,"x86 EntityEvent ABI");
bool Mutate(const Handle& h,Ptr context,int operation,int realm,bool enable,bool ghost,std::uint32_t event,const char** error) {
 Ptr previous=0; bool switched=false; bool ok=false;
 unsigned char init[24]={};
 *reinterpret_cast<std::uint32_t*>(init+8)=static_cast<std::uint32_t>(realm);
 init[g_client?0x10:0x14]=enable?1:0; if(g_client) init[0x11]=ghost?1:0;
 NativeEvent ev={g_eventVtable,event,0};
 __try {
  previous=reinterpret_cast<ContextGetFn>(Va(g_code[GetContext].va))();
  reinterpret_cast<ContextSetFn>(Va(g_code[SetContext].va))(context); switched=true;
  if(operation==0) reinterpret_cast<void(__fastcall*)(void*,void*,void*)>(Va(g_code[EntityInit].va))(reinterpret_cast<void*>(h.pointer),nullptr,init);
  else if(operation==1) reinterpret_cast<void(__cdecl*)(Ptr,bool)>(Va(g_code[EntityDestroy].va))(h.pointer,true);
  else {
   const Ptr f=U32(U32(h.pointer)+0x10);
   if(!IsExecutable(reinterpret_cast<void*>(f))) Fail(error,"entity onEvent is not executable");
   else { reinterpret_cast<void(__fastcall*)(void*,void*,NativeEvent*)>(f)(reinterpret_cast<void*>(h.pointer),nullptr,&ev); ok=true; }
  }
  if(operation!=2) ok=true;
 } __except(EXCEPTION_EXECUTE_HANDLER) { Fail(error,"native entity lifecycle call raised an exception"); }
 if(switched) __try { reinterpret_cast<ContextSetFn>(Va(g_code[SetContext].va))(previous); }
 __except(EXCEPTION_EXECUTE_HANDLER) { ok=false; Fail(error,"restoring engine context failed"); }
 return ok;
}
} // namespace

void EnterClientFrame() {
 const LONG tid=static_cast<LONG>(GetCurrentThreadId());
 const LONG owner=InterlockedCompareExchange(&g_clientThread,tid,0);
 if(owner==0 || owner==tid) ++g_clientFrame;
}
void LeaveClientFrame() { if(g_clientFrame) --g_clientFrame; }
bool IsClient() { return g_code?g_client:(GetModuleHandleA("bf3.exe")!=nullptr && GetModuleHandleA("bf3_server_Final.exe")==nullptr); }
bool FrameActive() { return g_ready==1 && Frame(); }
bool EngineFrameActive() {
 return (g_clientFrame!=0 && static_cast<DWORD>(g_clientThread)==GetCurrentThreadId()) ||
        (EVBotsServerFrameTickActive() && EVBotsOnServerFrameThread());
}
Ptr CaptureWeakPeer(Ptr peer) {
 if(!peer || g_ready!=1) return 0;
 __try {
  const Ptr index=U32(Va(g_client?0x023979E0:0x02B6AA74));
  const Ptr vector=__readfsdword(0x2C);
  if(!vector || index>4096) return 0;
  const Ptr tls=U32(vector+index*4);
  if(!tls || !U32(tls+(g_client?0x230:0x66C)) || (U32(peer+8)&0x80000)) return 0;
 } __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
 const Ptr weak=AcquireWeak(peer);
  if(!WeakAlive(weak,peer)) { DropWeakPeer(weak);return 0; }
 return weak;
}
void DropWeakPeer(Ptr token) {
 if(!token || g_ready!=1)return;
 if(Frame()) { ReleaseWeak(token);return; }
 AcquireSRWLockExclusive(&g_lock);g_pendingWeak.push_back(token);ReleaseSRWLockExclusive(&g_lock);
}
void DrainWeakPeers() {
 if(g_ready!=1 || !Frame())return;
 std::vector<Ptr> pending;
 AcquireSRWLockExclusive(&g_lock);pending.swap(g_pendingWeak);ReleaseSRWLockExclusive(&g_lock);
 for(Ptr token:pending)ReleaseWeak(token);
}
bool AdoptWeakPeer(Ptr peer,Ptr token,Handle* out) {
 if(!out || !WeakAlive(token,peer))return false;
 Ptr context=0,level=0,root=0;
 if(!Context(&context,&level,&root,nullptr))return false;
 Ptr flags=0;
 if(!ReadU32(peer+8,&flags) || (flags&0x80000))return false;
 const auto type=eng::TypeOf(static_cast<unsigned long>(peer));
 if(!type || !eng::IsKindOfName(type,"Entity"))return false;
 const Ptr bus=PeerBus(peer);Ptr vtable=0;
 if(!bus || !ReadU32(peer,&vtable))return false;
 AcquireSRWLockExclusive(&g_lock);
 auto existing=g_entities.find(peer);
 if(existing!=g_entities.end() && WeakAlive(existing->second.weak,peer)) {
  *out={peer,existing->second.generation};ReleaseSRWLockExclusive(&g_lock);return true;
 }
 InterlockedIncrement(reinterpret_cast<volatile LONG*>(token+4));
 Record record={g_nextGeneration++,vtable,bus,token};
 if(existing!=g_entities.end())ReleaseWeak(existing->second.weak);
 g_entities[peer]=record;*out={peer,record.generation};
 ReleaseSRWLockExclusive(&g_lock);return true;
}
bool Ready(const char** error) {
 if(Disabled()) return Fail(error,"EVBOTS_BLUEPRINT=0");
 if(g_ready==1) return true;
 if(g_ready<0) return Fail(error,"native Blueprint signature/hook batch is unavailable");
 const HMODULE server=GetModuleHandleA("bf3_server_Final.exe");
 const HMODULE client=GetModuleHandleA("bf3.exe");
 if(!server && !client) return Fail(error,"BF3 executable is unavailable");
 g_client=server==nullptr; g_base=reinterpret_cast<Ptr>(server?server:client); g_code=g_client?kClient:kServer;
 if(!Frame()) return Fail(error,"Blueprint hook install requires the real engine frame thread");
 bool match=true;
 for(unsigned i=0;i<sizeof(kClient)/sizeof(kClient[0]);++i) {
  const bool one=SafeCode(g_code[i]) || (!g_client && i==SingleFactory && EVBotsIsOwnEntityFactoryHook()); match=match&&one;
  LOG_INFO("Blueprint signature %08X: %s",static_cast<unsigned>(g_code[i].va),one?"match":"MISMATCH");
 }
 if(!match) { g_ready=-1; return Fail(error,"Blueprint entrypoint bytes do not match; no hooks installed"); }
 const bool bus=g_busDtorHook.Install(reinterpret_cast<void*>(Va(g_code[BusDtor].va)),reinterpret_cast<void*>(&BusDtorDetour),"Blueprint:BusDtor")==HookResult::Ok;
 if(bus) g_busDtor=reinterpret_cast<DtorFn>(g_busDtorHook.Trampoline());
 if(!bus) {
  if(g_busDtorHook.IsInstalled()) g_busDtorHook.Remove();
  g_ready=-1; return Fail(error,"Blueprint hook batch failed and was rolled back");
 }
 g_ready=1; LOG_INFO("Blueprint: native factory/lifetime tracking ready (%s)",g_client?"client":"server"); return true;
}

bool Create(Ptr liveBlueprint,const Params& params,Result* out,const char** error) {
 if(error) *error=nullptr;
 if(!out) return Fail(error,"missing Blueprint output");
 *out=Result{};
 if(!Ready(error)) return false;
 for(float n:params.transform) if(!std::isfinite(n)) return Fail(error,"Blueprint transform contains a non-finite value");
 Ptr context=0,level=0,root=0;
 if(!Context(&context,&level,&root,error)) return false;
 if(!FactoryEnabled()) return Fail(error,"native entity factory is not initialized");
 const auto ti=eng::TypeOf(static_cast<unsigned long>(liveBlueprint));
 if(!liveBlueprint || !ti || !eng::IsKindOfName(ti,"Blueprint")) return Fail(error,"a live native Blueprint is required");
 Ptr dcFlags=0;
 if(!ReadU32(liveBlueprint+4,&dcFlags) || (dcFlags&0x08000000)) return Fail(error,"Blueprint is marked destroyed");
 // Borrowed ResourceManager wrappers do not yet carry level generations.
 // Require a current partition/GUID round-trip before dereferencing a blueprint
 // in the factory. This also rejects clones not yet added to their partition.
 eng::Guid16 guid={}; char guidText[40]={};
 const auto partition=eng::PartitionOfInstance(static_cast<unsigned long>(liveBlueprint));
 if(!partition || !eng::InstanceGuid(static_cast<unsigned long>(liveBlueprint),&guid))
  return Fail(error,"Blueprint must be registered in a live partition with an exported GUID");
 eng::FormatGuid(guid,guidText);
 if(eng::FindInstanceInPartition(partition,guidText)!=liveBlueprint)
  return Fail(error,"Blueprint is no longer the current instance for its partition/GUID");
 if(params.parentRepresentative) return Fail(error,"parentRepresentative requires a separately validated native representative");
 Ptr sublevel=0;
 if(params.networked && !NetworkPreflight(&liveBlueprint,level,&sublevel,error)) return false;
 Collector collector={};
 collector.begin=collector.end=collector.fixed=collector.entries;
 collector.capacity=collector.entries+64;
 Ptr bus=0;
 const bool called=NativeCreate(liveBlueprint,params,context,level,root,sublevel,&collector,&bus,error);
 if(!called || !bus) { FreeCollector(&collector); ReleaseTemporary(bus); return called?Fail(error,"native Blueprint factory returned no bus"):false; }
 // The native temporary ref is held until all native reads below finish.
 Result made;
 const Ptr begin=reinterpret_cast<Ptr>(collector.begin),end=reinterpret_cast<Ptr>(collector.end),cap=reinterpret_cast<Ptr>(collector.capacity);
 if(!begin || end<begin || cap<end || ((end-begin)&7) || end-begin>8*65536) {
  FreeCollector(&collector); ReleaseTemporary(bus);
  return Fail(error,"native Blueprint collector returned an invalid span");
 }
 bool valid=true;
 for(auto* entry=collector.begin;entry!=collector.end;++entry) {
   Ptr peer=0;
   if(!ReadU32(reinterpret_cast<Ptr>(entry),&peer)) { valid=false; break; }
   if(!peer || PeerBus(peer)!=bus) continue;
   const auto type=eng::TypeOf(static_cast<unsigned long>(peer));
   if(!type) { valid=false; break; }
   // VU EntityBus.entities (10DF4600) exposes Entity, excluding bare peers such
   // as the factory's dummy bus owner. Those peers still keep the native bus alive.
   if(!eng::IsKindOfName(type,"Entity")) continue;
   const Ptr weak=AcquireWeak(peer);
   if(!WeakAlive(weak,peer)) { ReleaseWeak(weak); valid=false; break; }
   Record r={g_nextGeneration++,0,bus,weak};
   if(!ReadU32(peer,&r.vtable)) { ReleaseWeak(weak); valid=false; break; }
   AcquireSRWLockExclusive(&g_lock);
   const auto old=g_entities.find(peer);
   if(old!=g_entities.end()) ReleaseWeak(old->second.weak);
   g_entities[peer]=r;
   ReleaseSRWLockExclusive(&g_lock);
   made.entities.push_back({peer,r.generation});
 }
 FreeCollector(&collector);
 if(!valid) {
  // Do not turn a partially observed native result into a successful Lua bus.
  InvalidateBus(bus);
  ReleaseTemporary(bus);
  LOG_ERR("Blueprint: native bus %08X had an invalid peer/weak token; creation result rejected",static_cast<unsigned>(bus));
  return Fail(error,"native Blueprint peer validation failed after creation");
 }
 Record r={g_nextGeneration++,0,0,0};
 if(!ReadU32(bus,&r.vtable)) { InvalidateBus(bus); ReleaseTemporary(bus); return Fail(error,"native bus is no longer readable"); }
 AcquireSRWLockExclusive(&g_lock); g_buses[bus]=r; ReleaseSRWLockExclusive(&g_lock);
 made.bus={bus,r.generation};
 ReleaseTemporary(bus); // VU does exactly this before returning its non-owning wrapper.
 if(!Check(made.bus,true)) return Fail(error,"native bus was destroyed when the temporary factory reference was released");
 *out=std::move(made);
 LOG_INFO("Blueprint: created native bus %08X, %u native collector peers, networked=%d",static_cast<unsigned>(bus),static_cast<unsigned>(out->entities.size()),params.networked?1:0);
 return true;
}
bool CreateSingle(Ptr data,bool ownedData,const Params& params,Handle* out,const char** error) {
 if(error)*error=nullptr;
 if(!out)return Fail(error,"missing single-entity output");
 *out=Handle{};
 if(!Ready(error))return false;
 if(!SafeCode(g_code[SingleFactory]) && (g_client || !EVBotsIsOwnEntityFactoryHook()))
  return Fail(error,"single-entity factory entry changed or has a foreign hook");
 for(float n:params.transform)if(!std::isfinite(n))return Fail(error,"entity transform contains a non-finite value");
 Ptr context=0,level=0,root=0;
 if(!Context(&context,&level,&root,error))return false;
 if(!FactoryEnabled())return Fail(error,"native entity factory is not initialized");
 if(!SingleDataReady(data,ownedData,error))return false;
 if(params.parentRepresentative)return Fail(error,"single EntityData does not accept a parent representative");
 Ptr raw=0;
 const bool called=NativeCreateSingle(data,params,context,level,root,&raw,error);
 if(!raw)return called?Fail(error,"native single-entity factory returned nil"):false;
 // The creator owns initialization/registration. The wrapper owns only a weak
 // token, exactly as other engine entities; never add an implicit Entity::Init.
 const Ptr weak=CaptureWeakPeer(raw);Handle made;
 const bool adopted=called && weak && AdoptWeakPeer(raw,weak,&made);
 DropWeakPeer(weak);
 if(!adopted) {
  const auto actual=eng::TypeOf(static_cast<unsigned long>(raw));
  const char* cleanupError=nullptr;
  Ptr entityFlags=0;
  const bool cleaned=actual && eng::IsKindOfName(actual,"Entity") && ReadU32(raw+8,&entityFlags) &&
   ((entityFlags&0x80000)!=0 || Mutate({raw,0},context,1,0,false,false,0,&cleanupError));
  LOG_ERR("Blueprint single: rejected raw entity %08X; native destroy %s (%s)",static_cast<unsigned>(raw),
   cleaned?"requested":"FAILED",cleanupError?cleanupError:"weak adoption failed");
  return Fail(error,cleaned?"single-entity weak adoption failed; native destruction requested":"single-entity weak adoption and native cleanup failed (see raw address in log)");
 }
 *out=made;
 LOG_INFO("Blueprint single: created native entity %08X",static_cast<unsigned>(raw));
 return true;
}
bool WithEntityContext(const Handle& h,EntityContextCallback callback,void* user,const char** error) {
 if(error)*error=nullptr;
 if(!callback)return Fail(error,"missing native entity callback");
 Ptr context=0;
 if(!Ready(error) || !EntityAccess(h,&context,error))return false;
 Ptr previous=0;bool switched=false,ok=false;
 __try {
  previous=reinterpret_cast<ContextGetFn>(Va(g_code[GetContext].va))();
  reinterpret_cast<ContextSetFn>(Va(g_code[SetContext].va))(context);switched=true;
  ok=callback(h.pointer,user,error);
 } __except(EXCEPTION_EXECUTE_HANDLER) { Fail(error,"native entity callback raised an exception"); }
 if(switched) __try { reinterpret_cast<ContextSetFn>(Va(g_code[SetContext].va))(previous); }
 __except(EXCEPTION_EXECUTE_HANDLER) { ok=false;Fail(error,"restoring engine context after callback failed"); }
 return ok;
}
bool IsAlive(const Handle& h) {
 Ptr flags=0;
 return Check(h,false) && ReadU32(h.pointer+8,&flags) && !(flags&0x80000);
}
bool IsBusAlive(const Handle& h) { return Check(h,true); }
bool Init(const Handle& h,int realm,bool enable,bool ghost,const char** error) {
 if(realm<0 || realm>4) return Fail(error,"invalid entity realm");
 Ptr context=0; return EntityAccess(h,&context,error) && Mutate(h,context,0,realm,enable,ghost,0,error);
}
bool Destroy(const Handle& h,const char** error) {
 Ptr context=0; return EntityAccess(h,&context,error) && Mutate(h,context,1,0,false,false,0,error);
}
bool FireEvent(const Handle& h,std::uint32_t event,const char** error) {
 Ptr context=0; return EntityAccess(h,&context,error) && Mutate(h,context,2,0,false,false,event,error);
}
namespace {
struct RoundType { const char* name;Ptr type; };
const RoundType kRoundTypes[]={
 {"ServerTicketCounterEntity",0x02C230C0}, // static registration 0227BCA0, getType 012CE090
 {"ServerRoundOverEntity",0x02C23070},     // static registration 0227BBC0, getType 012CDF90
 {"ServerInputRestrictionEntity",0x02C16904},
 {"ServerCharacterSpawnEntity",0x02C16B84}, // DD8660/DE03C0 player Spawn event
 {"ServerHumanPlayerEntity",0x02C168DC},    // DCA620 getType; DDF360 UnSpawnAllSoldiers
 {"ServerVehicleSpawnEntity",0x02C17AD0},   // D4A8A0 getType, registration 02272100
 {"LocatorEntity",0x02BE7AE8}              // 005E46C0/00D94DD0 getType, init02263CE0
};
// Stock iterator is 0x1C bytes: container/segment, type index/end, cursor/count,
// byte activeOnly at +18. begin returns the output buffer, ret 0C; next ret.
struct NativeIterator { Ptr words[6];unsigned char active;unsigned char pad[3]; };
static_assert(sizeof(NativeIterator)==0x1C,"stock typed entity iterator ABI");
bool IteratorPreflight(Ptr world,Ptr type,const char** error) {
 __try {
  const Ptr begin=U32(world+0x820),end=U32(world+0x824);
  const unsigned base=*reinterpret_cast<const unsigned short*>(world+0x834);
  const unsigned id=*reinterpret_cast<const unsigned short*>(type+0x14);
  if(!begin || (begin&3) || end<begin || ((end-begin)&7) || end-begin>1024*1024 || id<base || id-base>=(end-begin)/8)
   return Fail(error,"entity type is not registered in the active world");
  Ptr segment=U32(begin+(id-base)*8);unsigned total=0,segments=0;
  while(segment) {
   if((segment&3) || ++segments>4096)return Fail(error,"native entity segment chain is invalid");
   const Ptr array=U32(segment),arrayEnd=U32(segment+4),active=U32(segment+0x1C);
   if((array&3) || arrayEnd<array || ((arrayEnd-array)&3) || active>(arrayEnd-array)/4)
    return Fail(error,"native entity segment bounds are invalid");
   const Ptr count=(arrayEnd-array)/4;
   if(count>65536-total || (count && !array))return Fail(error,"native entity snapshot exceeds its bounded span");
   total+=static_cast<unsigned>(count);segment=U32(segment+0x18);
  }
  return true;
 } __except(EXCEPTION_EXECUTE_HANDLER) { return Fail(error,"native entity iterator preflight read failed"); }
}
bool BeginIterator(Ptr world,Ptr type,NativeIterator* it) {
 __try {
  using Fn=NativeIterator*(__fastcall*)(void*,void*,Ptr,NativeIterator*,bool);
  // GetIterator включает отключённые сущности: Royale посылает им Spawn/Activate.
  // Server Next 005CF308: true берёт только +1C, false — весь [begin,end).
  return reinterpret_cast<Fn>(Va(g_code[IteratorBegin].va))(reinterpret_cast<void*>(world),nullptr,type,it,false)==it;
 } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool NextIterator(NativeIterator* it,Ptr* peer) {
 __try { *peer=reinterpret_cast<Ptr(__fastcall*)(void*,void*)>(Va(g_code[IteratorNext].va))(it,nullptr);return true; }
 __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
}
bool ListRoundEntities(const char* name,std::vector<Handle>* out,const char** error) {
 return ListEntities(name,out,error);
}
bool ListEntities(const char* name,std::vector<Handle>* out,const char** error) {
 if(error)*error=nullptr;
 if(!out || !name)return Fail(error,"missing entity iterator output/type");
 out->clear();
 if(!Ready(error))return false;
 Ptr type=0;
 if(g_client) {
  Ptr next=0;
  if(!ReadU32(Va(0x022EA470),&next))return Fail(error,"native TypeInfo list is unavailable");
  for(unsigned n=0;next && n<65536;++n) {
   eng::TypeDesc candidate;
   if(eng::Describe(static_cast<unsigned long>(next),&candidate) && candidate.name && std::strcmp(candidate.name,name)==0) {type=next;break;}
   if(!ReadU32(next+4,&next))return Fail(error,"native TypeInfo list read failed");
  }
  if(!type || !eng::IsKindOfName(static_cast<unsigned long>(type),"Entity"))return Fail(error,"requested client class is not a native Entity type");
 } else {
  for(const auto& candidate:kRoundTypes)if(std::strcmp(name,candidate.name)==0)type=Va(candidate.type);
  if(!type)return Fail(error,"entity type is outside the round-control adapter");
 }
 eng::TypeDesc desc;
 if(!eng::Describe(static_cast<unsigned long>(type),&desc) || !desc.name || std::strcmp(desc.name,name)!=0)
  return Fail(error,"native entity TypeInfo name mismatch");
 Ptr context=0,level=0,root=0,world=0;
 if(!Context(&context,&level,&root,error) || !ReadU32(Va(g_client?0x023813A0:0x02B3EE30),&world) || !world || !IteratorPreflight(world,type,error))return false;
 NativeIterator iterator={};
 if(!BeginIterator(world,type,&iterator))return Fail(error,"native entity iterator begin failed");
 std::vector<Handle> result;
 for(unsigned n=0;n<=65536;++n) {
  Ptr peer=0;
  if(!NextIterator(&iterator,&peer))return Fail(error,"native entity iterator next failed");
  if(!peer) { *out=std::move(result);return true; }
  if(n==65536)return Fail(error,"native entity iterator exceeded its bounded snapshot");
  if(eng::TypeOf(static_cast<unsigned long>(peer))!=type)return Fail(error,"native iterator returned a different entity type");
  Ptr flags=0;
  if(!ReadU32(peer+8,&flags))return Fail(error,"native iterator returned an unreadable entity");
  if(flags&0x80000)continue;
  const Ptr weak=AcquireWeak(peer);Handle h;
  const bool adopted=weak && AdoptWeakPeer(peer,weak,&h);
  ReleaseWeak(weak);
  if(!adopted)return Fail(error,"native iterator entity weak ownership failed");
  result.push_back(h);
 }
 return Fail(error,"native entity iterator did not terminate");
}
bool ReadEntity(const Handle& h,Ptr* type,Ptr* data,float* transform12) {
 Ptr context=0; if(!EntityAccess(h,&context,nullptr)) return false;
 const auto ti=eng::TypeOf(static_cast<unsigned long>(h.pointer));
 if(type) *type=ti;
 // Retail client/VU data accessor is vtable+1C. Debug dedicated has an
 // additional virtual before the bus/data pair: +1C returns EntityBus*,
 // +20 returns DataContainer* (ServerCharacterSpawnEntity 00E3BD20).
 // Use each executable's slot, not VU's slot on both builds. Simple Entity
 // data is +10; SpatialEntity data is +14, so retain the native accessor.
 if(data && !eng::IsKindOfName(ti,"Entity")) return false;
 if(transform12 && !eng::IsKindOfName(ti,"SpatialEntity")) return false;
 __try {
  if(data) {
   *data=0;
   const Ptr table=U32(h.pointer),getter=U32(table+(g_client?0x1C:0x20));
   MEMORY_BASIC_INFORMATION page={};
   if(!getter || !VirtualQuery(reinterpret_cast<const void*>(getter),&page,sizeof(page)) ||
      page.State!=MEM_COMMIT || page.Type!=MEM_IMAGE || reinterpret_cast<Ptr>(page.AllocationBase)!=g_base ||
      (page.Protect&(PAGE_GUARD|PAGE_NOACCESS)) || !IsExecutable(reinterpret_cast<void*>(getter)))return false;
   // Complete native accessors verified in both existing PEs. They do not
   // allocate or change TLS/context. Other overrides need their own proof.
   const auto* code=reinterpret_cast<const unsigned char*>(getter);
   const bool field=code[0]==0x8B && code[1]==0x41 && (code[2]==0x10 || code[2]==0x14) && code[3]==0xC3;
   const bool empty=code[0]==0x33 && code[1]==0xC0 && code[2]==0xC3;
   if(!field && !empty)return false;
   using Getter=Ptr(__fastcall*)(void*,void*);
   const Ptr result=reinterpret_cast<Getter>(getter)(reinterpret_cast<void*>(h.pointer),nullptr);
   if(result && !eng::IsKindOfName(eng::TypeOf(static_cast<unsigned long>(result)),"DataContainer"))return false;
   *data=result;
  }
  if(transform12) {
   Ptr t=U32(h.pointer+0x1C); if(!t) return false;
   if(U32(h.pointer+8)&0x40000) t+=0x10+*reinterpret_cast<const unsigned char*>(t+6)*0x50;
   for(unsigned row=0;row<4;++row) for(unsigned col=0;col<3;++col) transform12[row*3+col]=*reinterpret_cast<const float*>(t+row*16+col*4);
  }
  return true;
 } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
} }
