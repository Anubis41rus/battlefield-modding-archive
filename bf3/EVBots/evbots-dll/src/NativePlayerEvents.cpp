#include "NativePlayerEvents.h"
#include "NativeEntityEvents.h"
#include "EngineResources.h"
#include "TeamCapacity.h"
#include "EVBotsLog.h"
#include <windows.h>
#include <intrin.h>
#include <cstring>
#include <new>
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
namespace evbots {
unsigned long EVBotsEngineHumanByName(const char* name);
bool EVBotsEngineIsHuman(unsigned long player);
namespace playerevents {
struct Event {
    alignas(16) unsigned char native[0x20]={};
    std::uintptr_t player=0;
    std::uint64_t epoch=0;
    Params params;
    Event* next=nullptr;
    bool built=false;
    volatile LONG released=0,inFlight=0;
};
namespace {
using Ptr=std::uintptr_t;
constexpr unsigned kLimit=1024;
constexpr Ptr kCtor=0x00DD8660,kDtor=0x00DD8720,kEventVtable=0x025BABEC;
constexpr Ptr kSpawnVtable=0x025BBBC0,kSpawnHandler=0x00DE03C0;
constexpr Ptr kHumanVtable=0x025BB6C8,kHumanHandler=0x00DDF360;
constexpr std::uint32_t kSpawnId=0x0DC320DE,kUnspawnId=0x075306FD;
SRWLOCK g_lock=SRWLOCK_INIT;
Event* g_release=nullptr;
unsigned g_count=0;
std::uint64_t g_epoch=1;
bool g_running=false;
volatile LONG g_verified=0,g_faulted=0;
bool Fail(const char** out,const char* reason) {if(out)*out=reason;return false;}
bool Read(Ptr at,void* out,unsigned size) {
    if(!at)return false;
    __try {std::memcpy(out,reinterpret_cast<const void*>(at),size);return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool Head(Ptr at,const unsigned char* bytes,unsigned size) {
    unsigned char live[24]={};return size<=sizeof(live) && Read(at,live,size) && !std::memcmp(bytes,live,size);
}
bool Disabled() {char text[8]={};return GetEnvironmentVariableA("EVBOTS_HUMAN_PLAYER_EVENTS",text,sizeof(text))==1 && text[0]=='0';}
bool Verify(const char** error) {
    if(g_verified==1)return true;
    if(g_verified<0)return Fail(error,"native player event signature batch failed");
    if(reinterpret_cast<Ptr>(GetModuleHandleA("bf3_server_Final.exe"))!=0x400000)
        return Fail(error,"native human player events require the verified dedicated server");
    struct Signature {Ptr at;unsigned char bytes[16];};
    static const Signature batch[]={
        {kCtor,{0x8b,0x44,0x24,0x08,0x56,0x50,0x8b,0xf1,0xe8,0x23,0x84,0xaa,0xff,0x8a,0x54,0x24}},
        {kDtor,{0x56,0x8b,0xf1,0x8b,0x46,0x18,0x85,0xc0,0x74,0x19,0x8d,0x48,0x04,0x83,0xca,0xff}},
        {0x00D771E0,{0x51,0x8b,0x44,0x24,0x0c,0x53,0x56,0x57,0xc7,0x44,0x24,0x0c,0,0,0,0}},
        {kSpawnHandler,{0x81,0xec,0x34,0x01,0,0,0x53,0x8b,0x9c,0x24,0x3c,0x01,0,0,0x56,0x57}},
        {kHumanHandler,{0x56,0x57,0x8b,0x7c,0x24,0x0c,0x57,0x8b,0xf1,0xe8,0x62,0xa0,0xa6,0xff,0xb1,0x01}},
        {0x0083D5C0,{0xa1,0x74,0xaa,0xb6,0x02,0x64,0x8b,0x0d,0x2c,0,0,0,0x8b,0x14,0x81,0x8b}},
        {0x0083D5E0,{0xa1,0x74,0xaa,0xb6,0x02,0x64,0x8b,0x0d,0x2c,0,0,0,0x8b,0x14,0x81,0x8b}}
    };
    bool ok=true;
    for(const auto& signature:batch) {
        const bool one=Head(signature.at,signature.bytes,sizeof(signature.bytes));ok=ok&&one;
        LOG_INFO("PlayerEvents: signature %08X %s",static_cast<unsigned>(signature.at),one?"PASS":"FAIL");
    }
    Ptr typeFn=0,dtor=0;
    static const unsigned char typeHead[]={0xb8,0xec,0x67,0xc1,0x02,0xc3};
    ok=ok && Read(kEventVtable,&typeFn,4) && typeFn==0x00DC9DB0 && Head(typeFn,typeHead,sizeof(typeHead)) &&
        Read(kEventVtable+0x10,&dtor,4) && dtor==kDtor;
    InterlockedExchange(&g_verified,ok?1:-1);
    return ok || Fail(error,"native player event bytes/slots differ; no calls performed");
}
// The player weak constructor may allocate. A worker with a Lua gate is not an
// engine frame and cannot supply the engine TLS/allocator state.
bool Frame(Ptr* context,const char** error) {
    if(blueprint::IsClient() || !blueprint::EngineFrameActive())return Fail(error,"human player events require the actual dedicated engine frame");
    Ptr index=0,tls=0,physics=0,ctx=0;
    const Ptr vector=__readfsdword(0x2C);
    if(!Read(0x02B6AA74,&index,4) || !vector || index>4096 || !Read(vector+index*4,&tls,4))
        return Fail(error,"player event engine TLS is unavailable");
    if(!tls || !Read(tls+0x66C,&physics,4) || !physics)return Fail(error,"player event engine thread is not initialized");
    if(!Read(0x02B3EE2C,&ctx,4) || !ctx)return Fail(error,"player event server context is unavailable");
    if(context)*context=ctx;
    return true;
}
bool Ready(const char** error) {
    if(Disabled())return Fail(error,"EVBOTS_HUMAN_PLAYER_EVENTS=0");
    if(g_faulted)return Fail(error,"native human player event fault; further dispatch disabled");
    return Verify(error) && Frame(nullptr,error);
}
bool Construct(Event* event,const Params& params,const char** error) {
    Ptr context=0,previous=0;bool switched=false,ok=false;
    if(!Frame(&context,error))return false;
    __try {
        previous=reinterpret_cast<Ptr(__cdecl*)()>(0x0083D5E0)();
        reinterpret_cast<void(__cdecl*)(Ptr)>(0x0083D5C0)(context);switched=true;
        using Ctor=void*(__thiscall*)(void*,Ptr,std::uint32_t,int,int,int,int,int,int,int);
        reinterpret_cast<Ctor>(kCtor)(event->native,event->player,params.id,params.flags[0],params.flags[1],
            params.flags[2],params.flags[3],params.flags[4],params.flags[5],params.team);
        event->built=true;ok=true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {InterlockedExchange(&g_faulted,1);Fail(error,"native ServerPlayerEvent construction failed");}
    if(switched) __try {reinterpret_cast<void(__cdecl*)(Ptr)>(0x0083D5C0)(previous);}
    __except(EXCEPTION_EXECUTE_HANDLER) {ok=false;InterlockedExchange(&g_faulted,1);Fail(error,"restoring player event context failed");}
    return ok;
}
bool DestroyNative(Event* event) {
    if(!event->built)return true;
    // Use the real destructor; XADD/JNE tests the resulting refcount, not the old
    // count placed in EDX. Never emulate the player token as an Entity weak peer.
    __try {reinterpret_cast<void*(__thiscall*)(void*,int)>(kDtor)(event->native,0);event->built=false;return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {
        InterlockedExchange(&g_faulted,1);LOG_ERR("PlayerEvents: native event destructor fault; token is not retried");return false;
    }
}
bool PlayerAlive(const Event* event) {
    if(!event)return false;
    AcquireSRWLockShared(&g_lock);const bool current=g_running && event->epoch==g_epoch;ReleaseSRWLockShared(&g_lock);
    Ptr weak=0,owner=0;
    return current && event->built && !event->released &&
        Read(reinterpret_cast<Ptr>(event->native)+0x18,&weak,4) && weak && Read(weak,&owner,4) &&
        owner==event->player+8 && EVBotsEngineIsHuman(static_cast<unsigned long>(event->player));
}
bool Handler(Ptr entity,Ptr expectedVtable,Ptr expectedHandler,Ptr* handler,const char** error) {
    Ptr table=0;
    if(!Read(entity,&table,4) || table!=expectedVtable || !Read(table+0x10,handler,4))return Fail(error,"human event target type/vtable mismatch");
    if(*handler!=expectedHandler && !entityevents::OwnsHandler(table+0x10,*handler,expectedHandler))
        return Fail(error,"human event target has an unknown onEvent hook; no handler bypassed");
    return true;
}
struct Dispatch {Event* event;bool entered=false;};
bool DispatchSpawn(Ptr target,void* user,const char** error) {
    auto& dispatch=*static_cast<Dispatch*>(user);
    if(!PlayerAlive(dispatch.event))return Fail(error,"ServerPlayerEvent player disconnected or its lifetime changed");
    Ptr handler=0,data=0;unsigned blocked=0;unsigned char enabled=0;
    if(!Handler(target,kSpawnVtable,kSpawnHandler,&handler,error))return false;
    if(!Read(target+0x14,&data,4) || !data || !Read(data+0x180,&blocked,4) || blocked ||
        !Read(target+0xD18,&enabled,1) || !enabled)return Fail(error,"native character spawn point is disabled or not an event spawn");
    dispatch.entered=true;
    reinterpret_cast<void(__thiscall*)(Ptr,void*)>(handler)(target,dispatch.event->native);
    // Native onEvent only enqueues; this does not claim a soldier exists yet.
    static unsigned logged=0;
    if(logged++<20)LOG_INFO("PlayerEvents: native Spawn submitted player=%08X point=%08X data=%08X",
        static_cast<unsigned>(dispatch.event->player),static_cast<unsigned>(target),static_cast<unsigned>(data));
    return true;
}
struct UnspawnDispatch {bool entered=false;};
bool DispatchUnspawn(Ptr target,void* user,const char** error) {
    Ptr handler=0;unsigned char enabled=0;
    if(!Handler(target,kHumanVtable,kHumanHandler,&handler,error))return false;
    if(!Read(target+0x0A,&enabled,1) || !(enabled&1))return Fail(error,"native ServerHumanPlayerEntity is disabled");
    auto& dispatch=*static_cast<UnspawnDispatch*>(user);dispatch.entered=true;
    // Existing EntityEvent dispatch invokes the current slot and preserves our
    // registered callback cancellation/mutation before the stock handler.
    return true;
}
}
void Start() {AcquireSRWLockExclusive(&g_lock);g_running=true;ReleaseSRWLockExclusive(&g_lock);}
Event* Create(Ptr player,const Params& params,const char** error) {
    if(error)*error=nullptr;
    if(!Ready(error) || !player || !EVBotsEngineIsHuman(static_cast<unsigned long>(player))) {
        if(error && !*error)*error="ServerPlayerEvent requires a current human player";return nullptr;
    }
    AcquireSRWLockExclusive(&g_lock);const bool allowed=g_running && g_count<kLimit;
    const auto epoch=g_epoch;if(allowed)++g_count;ReleaseSRWLockExclusive(&g_lock);
    if(!allowed) {Fail(error,"human event lifetime capacity exhausted or stopped");return nullptr;}
    auto* event=new(std::nothrow) Event;
    if(!event) {AcquireSRWLockExclusive(&g_lock);--g_count;ReleaseSRWLockExclusive(&g_lock);Fail(error,"human event allocation failed");return nullptr;}
    event->player=player;event->epoch=epoch;event->params=params;
    if(!Construct(event,params,error) || !PlayerAlive(event)) {Release(event);return nullptr;}
    return event;
}
bool Alive(const Event* event) {return PlayerAlive(event);}
bool Fire(Event* event,const blueprint::Handle& target,const char** error) {
    if(error)*error=nullptr;
    if(!Ready(error))return false;
    if(!PlayerAlive(event))return Fail(error,"ServerPlayerEvent has no current human weak reference");
    std::uint32_t id=0;
    if(!Read(reinterpret_cast<Ptr>(event->native)+4,&id,4) || id!=kSpawnId)
        return Fail(error,"this native human adapter supports Spawn events on character spawn points");
    Dispatch dispatch={event};
    InterlockedIncrement(&event->inFlight);
    const bool ok=blueprint::WithEntityContext(target,&DispatchSpawn,&dispatch,error);
    Read(reinterpret_cast<Ptr>(event->native)+4,&event->params.id,4);
    InterlockedDecrement(&event->inFlight);
    if(!ok && dispatch.entered)InterlockedExchange(&g_faulted,1);
    return ok;
}
bool Unspawn(const blueprint::Handle& target,const char** error) {
    if(error)*error=nullptr;
    if(!Ready(error))return false;
    UnspawnDispatch dispatch;
    if(!blueprint::WithEntityContext(target,&DispatchUnspawn,&dispatch,error))return false;
    const bool ok=blueprint::FireEvent(target,kUnspawnId,error);
    if(!ok && dispatch.entered)InterlockedExchange(&g_faulted,1);
    return ok;
}
void Release(Event* event) {
    if(!event || InterlockedCompareExchange(&event->released,1,0)!=0)return;
    AcquireSRWLockExclusive(&g_lock);event->next=g_release;g_release=event;ReleaseSRWLockExclusive(&g_lock);
}
void Poll() {
    if(g_verified!=1 || !Frame(nullptr,nullptr))return;
    // A disabled feature must still release references it already acquired.
    static const unsigned char dtorHead[]={0x56,0x8b,0xf1,0x8b,0x46,0x18};
    if(!Head(kDtor,dtorHead,sizeof(dtorHead)))return;
    AcquireSRWLockExclusive(&g_lock);Event* list=g_release;g_release=nullptr;ReleaseSRWLockExclusive(&g_lock);
    while(list) {
        Event* next=list->next;
        if(list->inFlight) {AcquireSRWLockExclusive(&g_lock);list->next=g_release;g_release=list;ReleaseSRWLockExclusive(&g_lock);}
        else {DestroyNative(list);delete list;AcquireSRWLockExclusive(&g_lock);--g_count;ReleaseSRWLockExclusive(&g_lock);}
        list=next;
    }
}
void Shutdown() {
    AcquireSRWLockExclusive(&g_lock);g_running=false;++g_epoch;ReleaseSRWLockExclusive(&g_lock);
    Poll();
}
}
namespace {
constexpr char kPlayerEvent[]="evbots.ServerPlayerEvent";
struct EventUserdata {playerevents::Event* event;};
EventUserdata* EventValue(lua_State* L,int index) {return static_cast<EventUserdata*>(luaL_testudata(L,index,kPlayerEvent));}
std::uint32_t Hash(const char* text) {std::uint32_t id=0x1505;while(*text)id=(id*33)^static_cast<std::uint32_t>(static_cast<signed char>(*text++));return id;}
int EventGc(lua_State* L) {auto* value=EventValue(L,1);if(value && value->event) {playerevents::Release(value->event);value->event=nullptr;}return 0;}
int EventIndex(lua_State* L) {
    const auto* value=EventValue(L,1);const char* key=luaL_checkstring(L,2);
    if(!value || !value->event) {lua_pushnil(L);return 1;}
    const auto& params=value->event->params;
    if(!std::strcmp(key,"eventId")) {lua_pushinteger(L,static_cast<std::int32_t>(params.id));return 1;}
    if(!std::strcmp(key,"team")) {lua_pushinteger(L,params.team);return 1;}
    if(!std::strcmp(key,"player")) {if(playerevents::Alive(value->event))lua_getiuservalue(L,1,1);else lua_pushnil(L);return 1;}
    if(!std::strcmp(key,"typeInfo")) {lua_createtable(L,0,1);lua_pushliteral(L,"ServerPlayerEvent");lua_setfield(L,-2,"name");return 1;}
    const char* names[]={"sendToPlayerOnly","sendToHostOnly","sendToTeamOnly","invertPlayerFilter","invertTeamFilter","forwardToSpectators"};
    for(unsigned i=0;i<6;++i)if(!std::strcmp(key,names[i])) {lua_pushboolean(L,params.flags[i]);return 1;}
    lua_pushnil(L);return 1;
}
int CreateEvent(lua_State* L) {
    playerevents::Params params;
    if(lua_type(L,1)==LUA_TSTRING)params.id=Hash(lua_tostring(L,1));
    else {const auto id=luaL_checkinteger(L,1);if(id<(-2147483647LL-1) || id>4294967295LL)return luaL_argerror(L,1,"eventId must fit int32");params.id=static_cast<std::uint32_t>(id);}
    luaL_checktype(L,2,LUA_TTABLE);
    for(unsigned i=0;i<6;++i) {luaL_checktype(L,static_cast<int>(i)+3,LUA_TBOOLEAN);params.flags[i]=lua_toboolean(L,static_cast<int>(i)+3)!=0;}
    const auto team=luaL_checkinteger(L,9);
    if(team<0 || team>(TeamCapacityInstalled()?127:16))return luaL_argerror(L,9,"invalid player event team");
    params.team=static_cast<int>(team);
    lua_getfield(L,2,"name");const char* name=luaL_checkstring(L,-1);
    const auto player=EVBotsEngineHumanByName(name);lua_pop(L,1);
    // Allocate userdata before taking a native reference: Lua allocation failure
    // cannot orphan an event that has already retained the player weak token.
    auto* value=static_cast<EventUserdata*>(lua_newuserdatauv(L,sizeof(EventUserdata),1));value->event=nullptr;
    luaL_setmetatable(L,kPlayerEvent);lua_pushvalue(L,2);lua_setiuservalue(L,-2,1);
    const char* error=nullptr;value->event=playerevents::Create(player,params,&error);
    if(!value->event)return luaL_error(L,"ServerPlayerEvent: %s",error?error:"native player capture failed");
    return 1;
}
int IsEvent(lua_State* L) {lua_pushboolean(L,EventValue(L,1)!=nullptr);return 1;}
constexpr char kSource[]=R"LUA(
local create = ...
local old = ServerPlayerEvent
function ServerPlayerEvent(name, player, ...)
  if player ~= nil and tonumber(player.onlineId or 0) == 0 then return old(name,player,...) end
  if player == nil or PlayerManager:GetPlayerById(player.id) ~= player then
    error('ServerPlayerEvent: player is no longer the current Lua player',2)
  end
  return create(name,player,...)
end
)LUA";
}
bool IsNativePlayerEvent(lua_State* L,int index) {return EventValue(L,index)!=nullptr;}
int FireNativePlayerEvent(lua_State* L,int index,const blueprint::Handle& target) {
    auto* value=EventValue(L,index);const char* error=nullptr;
    if(!value || !playerevents::Fire(value->event,target,&error))return luaL_error(L,"Entity:FireEvent: %s",error?error:"native human player event expected");
    lua_pushboolean(L,1);return 1;
}
int FireNativeHumanUnspawn(lua_State* L,const blueprint::Handle& target) {
    const char* error=nullptr;if(!playerevents::Unspawn(target,&error))return luaL_error(L,"Entity:FireEvent: %s",error?error:"native human unspawn failed");
    lua_pushboolean(L,1);return 1;
}
bool RegisterNativePlayerEvents(lua_State* L) {
    const int top=lua_gettop(L);
    luaL_newmetatable(L,kPlayerEvent);lua_pushcfunction(L,EventGc);lua_setfield(L,-2,"__gc");
    lua_pushliteral(L,"ServerPlayerEvent");lua_setfield(L,-2,"__metatable");
    lua_pushcfunction(L,EventIndex);lua_setfield(L,-2,"__index");lua_pop(L,1);
    lua_pushcfunction(L,IsEvent);lua_setglobal(L,"EVBotsIsNativePlayerEvent");
    if(luaL_loadbufferx(L,kSource,sizeof(kSource)-1,"=[native-player-events]","t")==LUA_OK) {
        lua_pushcfunction(L,CreateEvent);
        if(lua_pcall(L,1,0,0)==LUA_OK) {playerevents::Start();lua_settop(L,top);return true;}
    }
    LOG_ERR("PlayerEvents bindings: %s",lua_tostring(L,-1));lua_settop(L,top);return false;
}
}
