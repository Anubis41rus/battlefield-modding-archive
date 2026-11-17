// Build267: EntityBusPeer::onEvent is vtable+10, thiscall(self,EntityEvent*),ret4.
// VU-Docs shared/type/entitybuspeer; existing VextEntityManager event hook and
// native handlers independently establish eventId+4. No VU appended entity data.
#include "NativeEntityEvents.h"
#include "EngineResources.h"
#include "InlineHook.h"
#include "EVBotsLog.h"
#include <array>
#include <cstring>
#include <utility>

namespace evbots { namespace entityevents { namespace {
using Ptr=std::uintptr_t;
using OnEventFn=void(__fastcall*)(void*,void*,void*);
constexpr unsigned kHooks=128,kWatches=4096,kQueue=4096;
struct Hook { Ptr slot=0;OnEventFn original=nullptr;bool installed=false; };
struct WatchRecord { blueprint::Handle entity;Ptr weak=0;std::uint64_t serial=0; };
Hook g_hooks[kHooks];
WatchRecord g_watches[kWatches];
Snapshot g_queue[kQueue];
SRWLOCK g_gate=SRWLOCK_INIT;
unsigned g_hookCount=0,g_read=0,g_count=0;
std::uint64_t g_epoch=1,g_nextWatch=1,g_nextSequence=1;
DispatchFn g_dispatch=nullptr;
bool g_enabled=false;
volatile LONG g_overflow=0,g_badInfo=0;
bool Same(const blueprint::Handle& a,const blueprint::Handle& b) {
    return a.pointer==b.pointer && a.generation==b.generation;
}
bool Fail(const char** out,const char* why) { if(out)*out=why;return false; }
bool Disabled() { char v[4]={};return EnvGetCached("EVBOTS_ENTITY_EVENTS",v,sizeof(v))==1 && v[0]=='0'; }
bool SafeRead(Ptr from,void* to,size_t length) {
    __try { std::memcpy(to,reinterpret_cast<const void*>(from),length);return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool WeakAlive(Ptr weak,Ptr peer) {
    Ptr owner=0;return weak && SafeRead(weak,&owner,sizeof(owner)) && owner==peer+4;
}
bool RetainWeak(Ptr weak) {
    __try { InterlockedIncrement(reinterpret_cast<volatile LONG*>(weak+4));return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool ReadAt(HANDLE file,DWORD offset,void* out,DWORD size) {
    LARGE_INTEGER position={};position.QuadPart=offset;DWORD got=0;
    return SetFilePointerEx(file,position,nullptr,FILE_BEGIN) && ReadFile(file,out,size,&got,nullptr) && got==size;
}
// The shipped bf3.exe has encrypted .text while its vtables are unchanged.
// These plaintext heads/slots are independently pinned to the existing zbf3.exe
// analysis; no dump or external code file is needed on the player's machine.
bool ClientHead(Ptr slot,Ptr handler,const unsigned char* live) {
    struct Head {Ptr slot,handler;unsigned char bytes[16];};
    static const Head heads[]={
        {0x02143F10,0x0101B290,{0x51,0x56,0x8B,0x74,0x24,0x0C,0x57,0x8B,0xF9,0x56,0x89,0x7C,0x24,0x0C,0xE8,0x3D}}, // ClientUIGraphEntity
        {0x02089F00,0x00665DD0,{0x83,0xEC,0x1C,0x53,0x55,0x56,0x8B,0xF1,0x8B,0x4E,0x24,0x57,0x85,0xC9,0x74,0x10}}, // SoundEntity
        {0x021AD118,0x011D0660,{0x80,0x79,0x14,0x00,0x74,0x09,0x8B,0x41,0x10,0x80,0x78,0x10,0x00,0x75,0x1B,0x8B}}, // EventSplitterEntity: Royale revive callbacks
    };
    for(const auto& head:heads)if(slot==head.slot && handler==head.handler)return std::memcmp(live,head.bytes,16)==0;
    return false;
}
// Verify the live vtable slot against the installed PE and all first16 handler
// bytes against its plaintext or an explicitly pinned decrypted client head.
// Reject other hooks instead of overwriting their chain.
bool StockSlot(Ptr slot,Ptr handler,const char** error) {
    const auto module=GetModuleHandleA(blueprint::IsClient()?"bf3.exe":"bf3_server_Final.exe");
    const Ptr base=reinterpret_cast<Ptr>(module);
    if(!base || slot<base || handler<base)return Fail(error,"event slot/handler is outside the stock image");
    wchar_t path[32768];const DWORD length=GetModuleFileNameW(module,path,32768);
    if(!length || length>=32768)return Fail(error,"stock executable path is unavailable");
    const HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
        nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return Fail(error,"stock executable cannot be read for signature verification");
    IMAGE_DOS_HEADER dos={};IMAGE_NT_HEADERS32 nt={};
    bool valid=ReadAt(file,0,&dos,sizeof(dos)) && dos.e_magic==IMAGE_DOS_SIGNATURE && dos.e_lfanew>0 &&
        dos.e_lfanew<1024*1024 && ReadAt(file,static_cast<DWORD>(dos.e_lfanew),&nt,sizeof(nt)) &&
        nt.Signature==IMAGE_NT_SIGNATURE && nt.FileHeader.Machine==IMAGE_FILE_MACHINE_I386 &&
        nt.OptionalHeader.Magic==IMAGE_NT_OPTIONAL_HDR32_MAGIC && nt.OptionalHeader.ImageBase==base &&
        nt.FileHeader.NumberOfSections>0 && nt.FileHeader.NumberOfSections<=96;
    DWORD slotOffset=0,codeOffset=0;
    if(valid) {
        const DWORD table=static_cast<DWORD>(dos.e_lfanew)+sizeof(DWORD)+sizeof(IMAGE_FILE_HEADER)+nt.FileHeader.SizeOfOptionalHeader;
        for(unsigned i=0;i<nt.FileHeader.NumberOfSections;++i) {
            IMAGE_SECTION_HEADER s={};
            if(!ReadAt(file,table+i*sizeof(s),&s,sizeof(s))) {valid=false;break;}
            const Ptr srva=slot-base,crva=handler-base;
            if(srva>=s.VirtualAddress && srva-s.VirtualAddress+4<=s.SizeOfRawData && (s.Characteristics&IMAGE_SCN_MEM_READ))
                slotOffset=s.PointerToRawData+static_cast<DWORD>(srva-s.VirtualAddress);
            if(crva>=s.VirtualAddress && crva-s.VirtualAddress+16<=s.SizeOfRawData && (s.Characteristics&IMAGE_SCN_MEM_EXECUTE))
                codeOffset=s.PointerToRawData+static_cast<DWORD>(crva-s.VirtualAddress);
        }
    }
    Ptr expected=0;unsigned char bytes[16]={},live[16]={};
    valid=valid && slotOffset && codeOffset && ReadAt(file,slotOffset,&expected,4) && expected==handler &&
        ReadAt(file,codeOffset,bytes,sizeof(bytes)) && SafeRead(handler,live,sizeof(live)) &&
        (std::memcmp(bytes,live,sizeof(bytes))==0 || (blueprint::IsClient() && ClientHead(slot,handler,live)));
    CloseHandle(file);
    LOG_INFO("EntityEvents: slot %08X handler %08X stock slot/16byte head %s",static_cast<unsigned>(slot),
        static_cast<unsigned>(handler),valid?"PASS":"FAIL");
    return valid || Fail(error,"event vtable/handler differs from the stock executable; existing hooks preserved");
}
bool EventInfo(void* raw,Snapshot* value) {
    if(!raw || !SafeRead(reinterpret_cast<Ptr>(raw)+4,&value->eventId,4))return false;
    eng::TypeDesc type;
    if(!eng::Describe(eng::TypeOf(static_cast<unsigned long>(reinterpret_cast<Ptr>(raw))),&type) || !type.name)return false;
    __try {
        for(unsigned i=0;i<sizeof(value->type);++i) {
            value->type[i]=type.name[i];if(!value->type[i])return true;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return false;
}
bool ReplaceEventId(void* raw,std::uint32_t oldId,std::uint32_t id) {
    __try {
        auto* field=reinterpret_cast<std::uint32_t*>(static_cast<unsigned char*>(raw)+4);
        if(*field!=oldId)return false;
        *field=id;return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
void Dispatch(unsigned index,void* self,void* edx,void* raw) {
    // Each thunk owns its original, even if the entity changes vtable or teardown
    // restores the slot while this call is in flight. Never hold g_gate in Lua/native.
    const auto original=g_hooks[index].original;
    Snapshot value;DispatchFn synchronous=nullptr;
    AcquireSRWLockExclusive(&g_gate);
    if(g_enabled)for(const auto& record:g_watches) {
        if(record.entity.pointer!=reinterpret_cast<Ptr>(self) || !record.serial)continue;
        // Avoid an unbounded stream of pending weak releases when a native worker
        // continues producing events after the bounded observation queue is full.
        if(g_count==kQueue && !blueprint::FrameActive()) {InterlockedIncrement(&g_overflow);break;}
        if(WeakAlive(record.weak,record.entity.pointer) && RetainWeak(record.weak)) {
            value.entity=record.entity;value.weak=record.weak;value.watch=record.serial;
            value.epoch=g_epoch;value.sequence=g_nextSequence++;synchronous=g_dispatch;
        }
        break;
    }
    ReleaseSRWLockExclusive(&g_gate);
    bool delivered=false,cancel=false;
    if(value.weak && !Disabled()) {
        if(!EventInfo(raw,&value)) {InterlockedIncrement(&g_badInfo);Release(&value);}
        else if(synchronous && blueprint::FrameActive()) {
            const std::uint32_t originalId=value.eventId;
            delivered=synchronous(&value,&cancel);
            if(delivered && !cancel && originalId!=value.eventId && !ReplaceEventId(raw,originalId,value.eventId))
                LOG_ERR("EntityEvents: synchronous eventId mutation failed; native event unchanged");
        }
    } else if(value.weak)Release(&value);
    if(delivered && !cancel && !WeakAlive(value.weak,value.entity.pointer)) {
        cancel=true;LOG_WARN("EntityEvents: callback destroyed its peer; native onEvent is no longer safe");
    }
    if(!delivered || !cancel)original(self,edx,raw);
    if(!value.weak)return;
    if(delivered) {Release(&value);return;}
    AcquireSRWLockExclusive(&g_gate);
    bool watched=false;
    if(g_enabled && value.epoch==g_epoch)for(const auto& record:g_watches)
        if(record.serial==value.watch && Same(record.entity,value.entity)) {watched=true;break;}
    if(watched && g_count<kQueue) {
        g_queue[(g_read+g_count)%kQueue]=value;++g_count;value.weak=0;
    } else if(watched)InterlockedIncrement(&g_overflow);
    ReleaseSRWLockExclusive(&g_gate);
    Release(&value);
}
template<size_t I> void __fastcall Thunk(void* self,void* edx,void* event) {Dispatch(static_cast<unsigned>(I),self,edx,event);}
template<size_t... I> constexpr std::array<OnEventFn,sizeof...(I)> Thunks(std::index_sequence<I...>) {return {{&Thunk<I>...}};}
const auto g_thunks=Thunks(std::make_index_sequence<kHooks>{});
}
void Start(DispatchFn dispatcher) {
    AcquireSRWLockExclusive(&g_gate);g_dispatch=dispatcher;g_enabled=true;ReleaseSRWLockExclusive(&g_gate);
}
bool Watch(const blueprint::Handle& entity,Ticket* ticket,const char** error) {
    if(error)*error=nullptr;
    if(!ticket || Disabled())return Fail(error,"event callbacks are disabled or output is missing");
    std::uintptr_t type=0;
    if(!blueprint::Ready(error) || !blueprint::ReadEntity(entity,&type,nullptr,nullptr))return Fail(error,"event callback requires a live entity on the native frame");
    Ptr table=0,handler=0;
    if(!SafeRead(entity.pointer,&table,4) || !table || !SafeRead(table+0x10,&handler,4) || !handler)
        return Fail(error,"native event vtable is unreadable");
    const Ptr weak=blueprint::CaptureWeakPeer(entity.pointer);
    if(!weak)return Fail(error,"native entity weak reference unavailable");
    bool needsVerification=true;
    AcquireSRWLockShared(&g_gate);
    for(unsigned i=0;i<g_hookCount;++i)if(g_hooks[i].slot==table+0x10 && g_hooks[i].installed)needsVerification=false;
    ReleaseSRWLockShared(&g_gate);
    const bool verified=!needsVerification || StockSlot(table+0x10,handler,error);
    if(!verified) {blueprint::DropWeakPeer(weak);return false;}
    bool ok=false,inserted=false;Ptr oldWeak=0;
    AcquireSRWLockExclusive(&g_gate);
    unsigned freeWatch=kWatches,hook=kHooks;
    if(g_enabled) {
        for(unsigned i=0;i<kWatches;++i) {
            if(!g_watches[i].serial && freeWatch==kWatches)freeWatch=i;
            if(Same(g_watches[i].entity,entity) && g_watches[i].serial) {
                ticket->watch=g_watches[i].serial;ticket->firstSequence=g_nextSequence;ok=true;break;
            }
            if(g_watches[i].serial && g_watches[i].entity.pointer==entity.pointer && !WeakAlive(g_watches[i].weak,entity.pointer))freeWatch=i;
        }
        if(!ok && freeWatch!=kWatches) {
            for(unsigned i=0;i<g_hookCount;++i)if(g_hooks[i].slot==table+0x10) {hook=i;break;}
            if(hook==kHooks && g_hookCount<kHooks) {
                hook=g_hookCount++;g_hooks[hook].slot=table+0x10;g_hooks[hook].original=reinterpret_cast<OnEventFn>(handler);
            }
            if(hook!=kHooks) {
                Hook& entry=g_hooks[hook];
                if(entry.installed && handler!=reinterpret_cast<Ptr>(g_thunks[hook]))Fail(error,"event slot changed after installation");
                else if(!entry.installed && handler!=reinterpret_cast<Ptr>(entry.original))Fail(error,"event slot changed before reinstallation");
                else {
                    if(!entry.installed)entry.installed=PatchPointer(reinterpret_cast<void*>(entry.slot),reinterpret_cast<void*>(g_thunks[hook]),
                        reinterpret_cast<void*>(entry.original),nullptr,"EntityEvents:onEvent");
                    if(entry.installed) {
                        oldWeak=g_watches[freeWatch].weak;
                        g_watches[freeWatch]={entity,weak,g_nextWatch++};
                        ticket->watch=g_watches[freeWatch].serial;ticket->firstSequence=g_nextSequence;ok=true;inserted=true;
                    } else Fail(error,"event vtable patch failed without replacing the original");
                }
            } else if(!error || !*error)Fail(error,"native event hook capacity exhausted");
        } else if(!ok)Fail(error,"native event subscription capacity exhausted");
    } else Fail(error,"native event callbacks are not started");
    // CaptureWeakPeer returns another reference even when the token address is
    // identical. Only a newly inserted record consumes this capture reference.
    ReleaseSRWLockExclusive(&g_gate);
    if(oldWeak)blueprint::DropWeakPeer(oldWeak);
    if(!inserted)blueprint::DropWeakPeer(weak);
    return ok;
}
void Unwatch(const blueprint::Handle& entity,std::uint64_t watch) {
    Ptr weak=0;AcquireSRWLockExclusive(&g_gate);
    for(auto& record:g_watches)if(record.serial==watch && Same(record.entity,entity)) {weak=record.weak;record={};break;}
    ReleaseSRWLockExclusive(&g_gate);if(weak)blueprint::DropWeakPeer(weak);
}
bool Current(const Snapshot& value) {
    bool match=false;AcquireSRWLockShared(&g_gate);
    if(g_enabled && value.epoch==g_epoch)for(const auto& record:g_watches)
        if(record.serial==value.watch && Same(record.entity,value.entity)) {match=WeakAlive(value.weak,value.entity.pointer);break;}
    ReleaseSRWLockShared(&g_gate);return match;
}
bool Poll(Snapshot* out) {
    if(!out || !blueprint::FrameActive())return false;
    const LONG dropped=InterlockedExchange(&g_overflow,0),bad=InterlockedExchange(&g_badInfo,0);
    if(dropped || bad)LOG_WARN("EntityEvents: bounded queue dropped=%ld, unreadable events=%ld",dropped,bad);
    AcquireSRWLockExclusive(&g_gate);const bool have=g_count!=0;
    if(have) {*out=g_queue[g_read];g_queue[g_read]={};g_read=(g_read+1)%kQueue;--g_count;}
    ReleaseSRWLockExclusive(&g_gate);return have;
}
void Release(Snapshot* value) {if(value && value->weak) {blueprint::DropWeakPeer(value->weak);value->weak=0;}}
bool OwnsHandler(Ptr slot,Ptr currentHandler,Ptr stockHandler) {
    bool match=false;
    AcquireSRWLockShared(&g_gate);
    for(unsigned i=0;i<g_hookCount;++i) {
        const auto& hook=g_hooks[i];
        if(hook.slot==slot && hook.installed && reinterpret_cast<Ptr>(hook.original)==stockHandler &&
            reinterpret_cast<Ptr>(g_thunks[i])==currentHandler) {match=true;break;}
    }
    ReleaseSRWLockShared(&g_gate);return match;
}
void Shutdown() {
    // No native allocation/destruction on a Lua-close worker: DropWeakPeer queues
    // the final release until a valid engine frame when necessary.
    Ptr retired[kWatches+kQueue];unsigned count=0;
    AcquireSRWLockExclusive(&g_gate);g_enabled=false;g_dispatch=nullptr;++g_epoch;
    for(auto& record:g_watches) {if(record.weak)retired[count++]=record.weak;record={};}
    while(g_count) {Snapshot& value=g_queue[g_read];if(value.weak)retired[count++]=value.weak;value={};g_read=(g_read+1)%kQueue;--g_count;}
    for(unsigned i=0;i<g_hookCount;++i) {
        Hook& entry=g_hooks[i];if(!entry.installed)continue;
        const bool restored=PatchPointer(reinterpret_cast<void*>(entry.slot),reinterpret_cast<void*>(entry.original),
            reinterpret_cast<void*>(g_thunks[i]),nullptr,"EntityEvents:restore onEvent");
        if(restored)entry.installed=false;
        LOG_INFO("EntityEvents: restore slot %08X %s",static_cast<unsigned>(entry.slot),restored?"PASS":"FAIL (other owner preserved)");
    }
    ReleaseSRWLockExclusive(&g_gate);
    for(unsigned i=0;i<count;++i)blueprint::DropWeakPeer(retired[i]);
}
} }
