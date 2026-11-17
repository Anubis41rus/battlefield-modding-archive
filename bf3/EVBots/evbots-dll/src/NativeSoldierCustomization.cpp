#include "NativeSoldierCustomization.h"
#include "NativeSpatial.h"
#include "EngineResources.h"
#include "VextResourceManager.h"
#include "EVBotsLog.h"
#include "InlineHook.h"
#include <windows.h>
#include <cstring>
#include <cmath>

namespace evbots {
bool EVBotsEngineIsHuman(unsigned long player);
namespace soldiercustom {
namespace {
using Ptr = std::uintptr_t;
constexpr unsigned kCapacity = 256;
struct Request {
    Target target;
    Ptr data = 0;
    bool enteredNative = false, transform = false;
    float matrix[16] = {};
};
SRWLOCK g_lock = SRWLOCK_INIT;
Request g_queue[kCapacity];
unsigned g_read = 0, g_count = 0;
bool g_running = false, g_active = false;
volatile LONG g_ready = 0;
bool Fail(const char** error, const char* text) { if(error)*error=text;return false; }
bool Disabled() {
    char text[8]={};return GetEnvironmentVariableA("EVBOTS_SOLDIER_CUSTOMIZATION",text,sizeof(text))==1 && text[0]=='0';
}
bool Read(Ptr address,void* out,unsigned bytes) {
    if(!address)return false;
    __try { std::memcpy(out,reinterpret_cast<const void*>(address),bytes);return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Head(Ptr address,const unsigned char* expected,unsigned size) {
    unsigned char actual[24]={};return size<=sizeof(actual) && Read(address,actual,size) && !std::memcmp(actual,expected,size);
}
bool Ready(const char** error,bool customization=true) {
    if(customization && Disabled())return Fail(error,"EVBOTS_SOLDIER_CUSTOMIZATION=0");
    if(g_ready==1)return true;
    if(g_ready<0)return Fail(error,"native soldier customization signature batch failed");
    if(reinterpret_cast<Ptr>(GetModuleHandleA("bf3_server_Final.exe"))!=0x400000)
        return Fail(error,"native customization requires the verified dedicated server");
    // VU 109BBF20 -> slot18E74FE8/client00AB0B90. Dedicated callsite
    // 012E2877 pushes data, body, player; caller cleans 12 bytes. Third arg is
    // CustomizeSoldierData*, NOT the bool claimed by an old decomp header.
    static const unsigned char apply[]={0x55,0x8b,0xec,0x83,0xe4,0xf8,0x81,0xec,0x0c,0x06,0,0,0x53,0x8b,0x5d,0x0c,0x56,0x57,0x8b,0xbb,0x4c,0x05,0,0};
    static const unsigned char caller[]={0x8b,0x8f,0xc4,0x03,0,0,0x85,0xc9,0x74,0xcb,0x50,0x51,0x57,0xe8,0x71,0xfa,0xb4,0xff,0x83,0xc4,0x0c};
    static const unsigned char owner[]={0x8b,0xbe,0x44,0x01,0,0,0x85,0xff};
    static const unsigned char tail[]={0x5f,0x5e,0x5b,0x8b,0xe5,0x5d,0xc3};
    struct Signature { Ptr address; const unsigned char* bytes;unsigned size; };
    const Signature batch[]={{0xE322F0,apply,sizeof(apply)},{0x12E286D,caller,sizeof(caller)},
        {0xE114A1,owner,sizeof(owner)},{0xE3302E,tail,sizeof(tail)}};
    bool valid=true;
    for(const auto& item:batch) {
        const bool one=Head(item.address,item.bytes,item.size);valid=valid&&one;
        LOG_INFO("SoldierCustomization: signature %08X %s",static_cast<unsigned>(item.address),one?"PASS":"FAIL");
    }
    InterlockedExchange(&g_ready,valid?1:-1);
    return valid || Fail(error,"native soldier customization bytes differ; no call performed");
}
bool TransformReady(const char** error) {
    char flag[8]={};
    if(GetEnvironmentVariableA("EVBOTS_SPATIAL_TRANSFORM",flag,sizeof(flag))==1 && flag[0]=='0')
        return Fail(error,"EVBOTS_SPATIAL_TRANSFORM=0");
    // VU Soldier setter109BD490 calls virtual+54. Dedicated ServerSoldier uses
    // virtual+5C=843570 and +AC=E0BE80. NativeSpatial verifies complete getter,
    // setter and copy/notification functions before its first write.
    static const unsigned char changed[]={0x55,0x8b,0xec,0x83,0xe4,0xf0,0x83,0xec,0x14,0x53,0x56,0x57,0x8b,0xf9,0x8b,0x57};
    Ptr callback=0;
    return (Read(0x025C3948+0xAC,&callback,4) && callback==0x00E0BE80 && Head(callback,changed,sizeof(changed))) ||
        Fail(error,"ServerSoldierEntity onTransformChanged signature/slot is not verified");
}
bool Owner(Ptr body,const Target& target,const char** error) {
    Ptr current=0,owner=0,vtable=0;
    if(!EVBotsEngineIsHuman(static_cast<unsigned long>(target.player)))return Fail(error,"soldier player is no longer a connected human");
    if(!Read(target.player+0x3C4,&current,4) || current!=body || !Read(body+0x144,&owner,4) || owner!=target.player)
        return Fail(error,"soldier body/owner changed; stale customization refused");
    if(!Read(body,&vtable,4) || vtable!=0x025C3948 || eng::TypeOf(static_cast<unsigned long>(body))!=0x02C174D0)
        return Fail(error,"body is not the verified native ServerSoldierEntity");
    return true;
}
bool ValidateTarget(Ptr body,void* user,const char** error) { return Owner(body,*static_cast<Target*>(user),error); }
bool Array(Ptr address,Ptr* data,unsigned* count) {
    return Read(address,data,4) && *data>=4 && Read(*data-4,count,4) && *count<=4096;
}
bool ValidateData(Ptr data,const char** error) {
    if(!eng::IsKindOfName(eng::TypeOf(static_cast<unsigned long>(data)),"CustomizeSoldierData"))
        return Fail(error,"ApplyCustomization expects native CustomizeSoldierData");
    // Native E328xx indexes a 12-entry stack array by WeaponSlot and explicitly
    // asserts on NotDefined (11); E32Dxx asserts when a weapon has >5 unlocks.
    // Check before the function starts mutating/replicating the soldier.
    Ptr weapons=0;unsigned count=0;
    if(!Array(data+0x20,&weapons,&count))return Fail(error,"customization weapons array is invalid or exceeds 4096 entries");
    for(unsigned i=0;i<count;++i) {
        struct Entry { Ptr weapon; unsigned slot; Ptr unlocks; } value={};
        static_assert(sizeof(Entry)==12,"native UnlockWeaponAndSlot x86 ABI");
        unsigned unlockCount=0;
        if(!Read(weapons+i*sizeof(Entry),&value,sizeof(value)) || value.slot>=11)
            return Fail(error,"customization weapon slot must be 0..10 (NotDefined would assert)");
        if(!value.unlocks || !Read(value.unlocks-4,&unlockCount,4) || unlockCount>5)
            return Fail(error,"customization weapon unlocks must be a valid array of at most five entries");
    }
    return true;
}
bool Apply(Ptr body,void* user,const char** error) {
    auto& request=*static_cast<Request*>(user);
    if(!Owner(body,request.target,error))return false;
    if(request.transform) {
        if(!TransformReady(error))return false;
        return spatial::SetTransform(request.target.soldier,request.matrix,error);
    }
    if(!ValidateData(request.data,error))return false;
    // WithEntityContext sets/restores the real server context and catches SEH.
    // The native function applies visual/unlock/weapon flags and emits native
    // messages itself. We pass the complete DC without rewriting the mod's data.
    using ApplyFn=void(__cdecl*)(Ptr,Ptr,Ptr);
    request.enteredNative=true;
    reinterpret_cast<ApplyFn>(0x00E322F0)(request.target.player,body,request.data);
    return true;
}
bool ApplyRequest(Request& request,const char** error) {
    const bool ok=Ready(error,!request.transform) && blueprint::WithEntityContext(request.target.soldier,&Apply,&request,error);
    if(!ok && request.enteredNative) {
        InterlockedExchange(&g_ready,-1);
        LOG_ERR("SoldierCustomization: native call/context restore failed; further calls disabled, partial changes cannot be rolled back");
    }
    return ok;
}
void Release(Request& request) {
    if(request.data)ResourceQueueRelease(static_cast<unsigned long>(request.data));
    request.data=0;
}
bool TargetValid(const Target& target,const char** error) {
    if(!target.player || !target.soldier.pointer || !target.soldier.generation || !blueprint::IsAlive(target.soldier))
        return Fail(error,"native human soldier target is stale");
    return true;
}

constexpr Ptr kPrePhysicsEntrySlot = 0x025C3948 + 0x144;
constexpr Ptr kPrePhysicsEntry = 0x00E0A7C0;
constexpr Ptr kCopyInput = 0x008CB150;
constexpr Ptr kDestroyInput = 0x008C1270;
constexpr Ptr kSetInputLevel = 0x008BEF00;
constexpr Ptr kEntryInputVtable = 0x024F1354;
constexpr unsigned kToggleParachute = 36;
constexpr ULONGLONG kParachuteRequestMs = 1000;
struct ParachuteRequest {
    Target target;
    ULONGLONG expires = 0;
};
ParachuteRequest g_parachuteRequests[kCapacity];
volatile LONG g_pendingParachutes = 0, g_parachuteFault = 0;
bool g_parachuteInstalled = false;

// g_lock protects the queue, including replacement of a request from the same player.
void EraseParachute(unsigned index) {
    const auto last = static_cast<unsigned>(InterlockedDecrement(&g_pendingParachutes));
    g_parachuteRequests[index] = g_parachuteRequests[last];
    g_parachuteRequests[last] = {};
}
void ExpireParachutes(ULONGLONG now) {
    for (unsigned i = 0; i < static_cast<unsigned>(g_pendingParachutes);) {
        if (g_parachuteRequests[i].expires > now) {
            ++i;
        } else {
            LOG_WARN("HumanSoldier: parachute request expired for %08X before native input update",
                static_cast<unsigned>(g_parachuteRequests[i].target.soldier.pointer));
            EraseParachute(i);
        }
    }
}
Target TakeParachute(Ptr body) {
    Target target;
    AcquireSRWLockExclusive(&g_lock);
    ExpireParachutes(GetTickCount64());
    for (unsigned i = 0; i < static_cast<unsigned>(g_pendingParachutes); ++i) {
        if (g_parachuteRequests[i].target.soldier.pointer != body) continue;
        target = g_parachuteRequests[i].target;
        EraseParachute(i);
        break;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return target;
}

bool CopyWithParachute(void* copy, Ptr source, bool* copied) {
    __try {
        reinterpret_cast<void*(__thiscall*)(void*, Ptr)>(kCopyInput)(copy, source);
        *copied = true;
        reinterpret_cast<void(__thiscall*)(void*, unsigned, float)>(kSetInputLevel)(copy, kToggleParachute, 1.0f);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void DestroyInputCopy(void* copy) {
    __try { reinterpret_cast<void(__thiscall*)(void*)>(kDestroyInput)(copy); }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_parachuteFault, 1);
        LOG_ERR("HumanSoldier: native parachute input destruction failed; requests disabled");
    }
}
void __fastcall DetourPrePhysicsEntry(Ptr body, void*, unsigned entry, Ptr input) {
    const auto original = reinterpret_cast<void(__thiscall*)(Ptr, unsigned, Ptr)>(kPrePhysicsEntry);
    Ptr inputVtable = 0;
    if (entry != 0 || !InterlockedCompareExchange(&g_pendingParachutes, 0, 0) || g_parachuteFault ||
        !Read(input, &inputVtable, sizeof(inputVtable)) || inputVtable != kEntryInputVtable) {
        original(body, entry, input);
        return;
    }
    const Target target = TakeParachute(body);
    Ptr controlled = 0;
    if (!TargetValid(target, nullptr) || !Owner(body, target, nullptr) ||
        !Read(target.player + 0x3E8, &controlled, sizeof(controlled)) || controlled != body) {
        original(body, entry, input);
        return;
    }

    // The stock caller uses the same copy ctor/dtor for its stack input, then
    // invokes this slot. Storage covers the last native member at +0x2F8.
    alignas(16) unsigned char copy[0x300] = {};
    bool copied = false;
    if (!CopyWithParachute(copy, input, &copied)) {
        if (copied) DestroyInputCopy(copy);
        InterlockedExchange(&g_parachuteFault, 1);
        LOG_ERR("HumanSoldier: native parachute input copy failed; requests disabled");
        original(body, entry, input);
        return;
    }
    __try {
        original(body, entry, reinterpret_cast<Ptr>(copy));
    } __finally { DestroyInputCopy(copy); }
    LOG_INFO("HumanSoldier: parachute input passed to native update for %08X", static_cast<unsigned>(body));
}

bool InstallParachuteSlot(const char** error) {
    if (g_parachuteInstalled) return true;
    if (reinterpret_cast<Ptr>(GetModuleHandleA("bf3_server_Final.exe")) != 0x400000)
        return Fail(error, "parachute input requires the verified dedicated server");
    // bf3_server_Final: prePhysicsUpdateEntry(this, entry, const EntryInputState&), ret 8.
    // Check both the input consumer and the native stack-copy path before patching the slot.
    constexpr unsigned char entryHead[] = {0x81,0xec,0x34,0x03,0,0,0x53,0x55,0x33,0xdb,0x56,0x33,0xed,0x89,0x5c,0x24};
    constexpr unsigned char copyHead[] = {0x8b,0x44,0x24,0x04,0x56,0x50,0x8b,0xf1,0xe8,0x83,0xe2,0xff,0xff,0x8b,0xc6,0x5e};
    constexpr unsigned char copyImpl[] = {0x53,0x8b,0xd9,0xc7,0x03,0x54,0x13,0x4f,0x02,0x55,0x8b,0x6c,0x24,0x0c,0xf3,0x0f};
    constexpr unsigned char destroyHead[] = {0x53,0x56,0x8b,0xd9,0x57,0x8d,0xb3,0xc0,0x02,0,0,0xbf,0x03,0,0,0};
    constexpr unsigned char levelHead[] = {0x8b,0x44,0x24,0x08,0x53,0x56,0x57,0x8b,0xf9,0x8b,0xc8,0x81,0xe1,0,0,0x80};
    constexpr unsigned char caller[] = {0x8b,0x16,0x8b,0x92,0x44,0x01,0,0,0x8d,0x84,0x24,0xc0,0,0,0,0x50};
    const struct { Ptr address; const unsigned char* bytes; unsigned size; } signatures[] = {
        {kPrePhysicsEntry, entryHead, sizeof(entryHead)}, {kCopyInput, copyHead, sizeof(copyHead)},
        {0x008C93E0, copyImpl, sizeof(copyImpl)}, {kDestroyInput, destroyHead, sizeof(destroyHead)},
        {kSetInputLevel, levelHead, sizeof(levelHead)}, {0x00E34441, caller, sizeof(caller)},
    };
    bool valid = true;
    for (const auto& signature : signatures) {
        const bool one = Head(signature.address, signature.bytes, signature.size);
        LOG_INFO("HumanSoldier: parachute signature %08X %s", static_cast<unsigned>(signature.address), one ? "PASS" : "FAIL");
        valid = valid && one;
    }
    Ptr slot = 0;
    if (!valid || !Read(kPrePhysicsEntrySlot, &slot, sizeof(slot)) || slot != kPrePhysicsEntry)
        return Fail(error, "native parachute input bytes or vtable slot differ");
    g_parachuteInstalled = PatchPointer(reinterpret_cast<void*>(kPrePhysicsEntrySlot),
        reinterpret_cast<void*>(&DetourPrePhysicsEntry), reinterpret_cast<void*>(kPrePhysicsEntry),
        nullptr, "HumanSoldier:prePhysicsUpdateEntry");
    if (!g_parachuteInstalled) return Fail(error, "native parachute input slot could not be installed");
    LOG_INFO("HumanSoldier: parachute input slot %08X installed", static_cast<unsigned>(kPrePhysicsEntrySlot));
    return true;
}

bool Dispatch(Request& request,bool* queued,const char** error) {
    const bool frame=blueprint::FrameActive();
    AcquireSRWLockExclusive(&g_lock);
    // Preserve order across worker submissions and native callback reentry. A
    // frame caller must not overtake a prior customization and then have its
    // rotation overwritten when that earlier operation finally drains.
    const bool immediate=g_running && frame && !g_active && !g_count;
    const bool accepted=g_running && (immediate || g_count<kCapacity);
    if(immediate)g_active=true;
    else if(accepted) {g_queue[(g_read+g_count)%kCapacity]=request;++g_count;request.data=0;}
    ReleaseSRWLockExclusive(&g_lock);
    if(immediate) {
        const bool applied=ApplyRequest(request,error);
        AcquireSRWLockExclusive(&g_lock);g_active=false;ReleaseSRWLockExclusive(&g_lock);
        Release(request);return applied;
    }
    Release(request);
    if(!accepted)return Fail(error,"native human soldier queue is full or stopped");
    if(queued)*queued=true;
    return true;
}
}
void Start() { AcquireSRWLockExclusive(&g_lock);g_running=true;ReleaseSRWLockExclusive(&g_lock); }
bool CaptureHuman(Ptr player,Ptr expectedBody,Target* out,const char** error) {
    if(error)*error=nullptr;
    if(!out || !player || !expectedBody)return Fail(error,"missing player or expected soldier body");
    if(!Ready(error,false) || !blueprint::Ready(error))return false;
    if(blueprint::IsClient() || !blueprint::FrameActive())return Fail(error,"capturing a soldier requires the actual dedicated engine frame");
    Target target;target.player=player;
    if(!Owner(expectedBody,target,error))return false;
    const Ptr weak=blueprint::CaptureWeakPeer(expectedBody);
    if(!weak)return Fail(error,"native soldier weak reference could not be acquired");
    const bool adopted=blueprint::AdoptWeakPeer(expectedBody,weak,&target.soldier);
    blueprint::DropWeakPeer(weak);
    if(!adopted)return Fail(error,"native soldier lifetime could not be tracked");
    if(!blueprint::WithEntityContext(target.soldier,&ValidateTarget,&target,error))return false;
    *out=target;return true;
}
bool ReadHumanState(Ptr player,HumanState* out,const char** error) {
    if(error)*error=nullptr;
    if(!out || !player)return Fail(error,"missing player or native state output");
    if(!Ready(error,false) || !blueprint::Ready(error))return false;
    if(blueprint::IsClient() || !blueprint::FrameActive())return Fail(error,"human state readback requires the actual dedicated engine frame");
    if(!EVBotsEngineIsHuman(static_cast<unsigned long>(player)))return Fail(error,"state readback requires a connected human");
    Ptr body=0;
    if(!Read(player+0x3C4,&body,4))return Fail(error,"native player soldier slot is unreadable");
    HumanState state;state.target.player=player;
    if(body) {
        if(!CaptureHuman(player,body,&state.target,error) ||
            !spatial::GetTransform(state.target.soldier,state.matrix,error) ||
            !blueprint::WithEntityContext(state.target.soldier,&ValidateTarget,&state.target,error))return false;
    } else {
        Ptr current=0;
        if(!EVBotsEngineIsHuman(static_cast<unsigned long>(player)) ||
            !Read(player+0x3C4,&current,4) || current)
            return Fail(error,"native player body state changed during readback");
    }
    *out=state;return true;
}
bool Submit(const Target& target,Ptr data,bool ownedData,bool* queued,const char** error) {
    if(error)*error=nullptr;
    if(queued)*queued=false;
    if(!Ready(error))return false;
    if(!TargetValid(target,error))return false;
    if(!ownedData)return Fail(error,"customization requires an owned native DC (construct or Clone it first)");
    if(!data || !ResourceRetainLive(static_cast<unsigned long>(data)))return Fail(error,"customization native reference could not be retained");
    Request request={target,data};
    return Dispatch(request,queued,error);
}
bool SubmitTransform(const Target& target,const float* matrix16,bool* queued,const char** error) {
    if(error)*error=nullptr;
    if(queued)*queued=false;
    if(!Ready(error,false) || !TransformReady(error) || !TargetValid(target,error))return false;
    if(!matrix16)return Fail(error,"missing human soldier transform");
    Request request;request.target=target;request.transform=true;
    for(unsigned row=0;row<4;++row)for(unsigned col=0;col<3;++col) {
        const auto value=matrix16[row*4+col];
        if(!std::isfinite(value))return Fail(error,"human soldier transform must contain finite vectors");
        request.matrix[row*4+col]=value;
    }
    return Dispatch(request,queued,error);
}
bool RequestParachute(const Target& target, const char** error) {
    if (error) *error = nullptr;
    char flag[8] = {};
    if (GetEnvironmentVariableA("EVBOTS_HUMAN_PARACHUTE", flag, sizeof(flag)) == 1 && flag[0] == '0')
        return Fail(error, "EVBOTS_HUMAN_PARACHUTE=0");
    if (g_parachuteFault) return Fail(error, "native parachute input is disabled after a fault");
    if (!blueprint::FrameActive() || blueprint::IsClient())
        return Fail(error, "parachute input requires the dedicated engine frame");
    if (!TargetValid(target, error) || !Owner(target.soldier.pointer, target, error)) return false;

    AcquireSRWLockExclusive(&g_lock);
    bool accepted = false;
    if (!g_running) Fail(error, "native human soldier operations are stopped");
    else if (InstallParachuteSlot(error)) {
        const ULONGLONG now = GetTickCount64();
        ExpireParachutes(now);
        const unsigned size = static_cast<unsigned>(g_pendingParachutes);
        unsigned i = 0;
        for (; i < size; ++i) if (g_parachuteRequests[i].target.player == target.player) break;
        if (i < kCapacity) {
            g_parachuteRequests[i] = {target, now + kParachuteRequestMs};
            if (i == size) InterlockedIncrement(&g_pendingParachutes);
            accepted = true;
        } else Fail(error, "native parachute request queue is full");
    }
    ReleaseSRWLockExclusive(&g_lock);
    return accepted;
}
void Poll() {
    if(!blueprint::FrameActive())return;
    // Bound work, including requests submitted recursively from native events.
    for(unsigned i=0;i<kCapacity;++i) {
        Request request;
        AcquireSRWLockExclusive(&g_lock);
        const bool available=g_running && !g_active && g_count;
        if(available) {request=g_queue[g_read];g_queue[g_read]={};g_read=(g_read+1)%kCapacity;--g_count;g_active=true;}
        ReleaseSRWLockExclusive(&g_lock);
        if(!available)break;
        const char* error=nullptr;
        if(!ApplyRequest(request,&error))LOG_ERR("HumanSoldier: queued %s refused for %08X: %s",
            request.transform?"transform":"customization",static_cast<unsigned>(request.target.soldier.pointer),error?error:"native operation failed");
        AcquireSRWLockExclusive(&g_lock);g_active=false;ReleaseSRWLockExclusive(&g_lock);
        Release(request);
    }
}
void Shutdown() {
    Request pending[kCapacity];unsigned count=0;
    AcquireSRWLockExclusive(&g_lock);g_running=false;
    InterlockedExchange(&g_pendingParachutes, 0);
    if (g_parachuteInstalled) {
        const bool restored = PatchPointer(reinterpret_cast<void*>(kPrePhysicsEntrySlot),
            reinterpret_cast<void*>(kPrePhysicsEntry), reinterpret_cast<void*>(&DetourPrePhysicsEntry),
            nullptr, "HumanSoldier:restore prePhysicsUpdateEntry");
        g_parachuteInstalled = !restored;
        LOG_INFO("HumanSoldier: parachute input slot restore %s", restored ? "PASS" : "FAIL");
    }
    while(g_count) {pending[count++]=g_queue[g_read];g_queue[g_read]={};g_read=(g_read+1)%kCapacity;--g_count;}
    ReleaseSRWLockExclusive(&g_lock);
    for(unsigned i=0;i<count;++i)Release(pending[i]);
}
} }
