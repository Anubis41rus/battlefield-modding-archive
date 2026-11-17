#include "VextEntityEvents.h"
#include "NativeEntityEvents.h"
#include "LuaRuntime.h"
#include "EVBotsLog.h"
#include <Windows.h>
#include <cstring>
#include <limits>
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
namespace evbots { namespace {
constexpr char kEntity[]="evbots.BlueprintEntity";
constexpr char kLifetime[]="evbots.EntityEventLifetime";
const char kCallbacks=0,kSentinel=0;
lua_State* g_state=nullptr;
using blueprint::Handle;
struct CallbackMeta { Handle entity;entityevents::Ticket ticket; };
Handle* Entity(lua_State* L,int i) {
    if(lua_rawlen(L,i)!=sizeof(Handle))return nullptr;
    return static_cast<Handle*>(luaL_testudata(L,i,kEntity));
}
bool Same(const Handle& a,const Handle& b) {return a.pointer==b.pointer && a.generation==b.generation;}
CallbackMeta* Meta(lua_State* L,int entry) {
    lua_getfield(L,entry,"native");
    auto* value=lua_type(L,-1)==LUA_TUSERDATA && lua_rawlen(L,-1)==sizeof(CallbackMeta)
        ?static_cast<CallbackMeta*>(lua_touserdata(L,-1)):nullptr;
    lua_pop(L,1);return value;
}
bool Enabled(lua_State* L,int entry) {lua_getfield(L,entry,"enabled");const bool enabled=lua_toboolean(L,-1)!=0;lua_pop(L,1);return enabled;}
void PushCallbacks(lua_State* L) {lua_rawgetp(L,LUA_REGISTRYINDEX,&kCallbacks);}
void PushOwner(lua_State* L) {
    const auto& folder=LuaRuntime::Instance().currentModFolder();
    const int n=WideCharToMultiByte(CP_UTF8,0,folder.data(),static_cast<int>(folder.size()),nullptr,0,nullptr,nullptr);
    std::string text(n,'\0');if(n)WideCharToMultiByte(CP_UTF8,0,folder.data(),static_cast<int>(folder.size()),text.data(),n,nullptr,nullptr);
    lua_pushlstring(L,text.data(),text.size());
}
std::wstring Owner(lua_State* L,int entry) {
    lua_getfield(L,entry,"owner");size_t length=0;const char* text=lua_tolstring(L,-1,&length);
    const int count=text && length<=32768?MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,static_cast<int>(length),nullptr,0):0;
    std::wstring folder(count,L'\0');if(count)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,static_cast<int>(length),folder.data(),count);
    lua_pop(L,1);return folder;
}
int Failure(lua_State* L,const char* why) {
    LOG_WARN("EntityEvents: subscription refused: %s",why?why:"native event bridge unavailable");
    lua_pushnil(L);lua_pushstring(L,why?why:"native event bridge unavailable");return 2;
}
bool Remaining(lua_State* L,int callbacks,const CallbackMeta& target) {
    bool found=false;lua_pushnil(L);
    while(lua_next(L,callbacks)) {
        if(lua_istable(L,-1)) {const auto* m=Meta(L,lua_gettop(L));found=found || (m && m->ticket.watch==target.ticket.watch && Same(m->entity,target.entity) && Enabled(L,lua_gettop(L)));}
        lua_pop(L,1);
    }
    return found;
}
int Register(lua_State* L) {
    const auto* entity=Entity(L,1);if(!entity)return luaL_argerror(L,1,"native weak Entity expected");
    const bool hasContext=lua_isfunction(L,3)!=0;const int callback=hasContext?3:2;
    luaL_checktype(L,callback,LUA_TFUNCTION);
    if(g_state!=L)return Failure(L,"entity event binding is not initialized");
    PushCallbacks(L);const int callbacks=lua_gettop(L);
    lua_getfield(L,callbacks,"nextId");const lua_Integer id=lua_tointeger(L,-1)+1;lua_pop(L,1);
    if(id<=0 || id>0x7fffffff)return Failure(L,"callback identifier space exhausted");
    entityevents::Ticket ticket;const char* error=nullptr;
    if(!entityevents::Watch(*entity,&ticket,&error))return Failure(L,error);
    lua_pushinteger(L,id);lua_setfield(L,callbacks,"nextId");lua_createtable(L,0,7);const int entry=lua_gettop(L);
    auto* meta=static_cast<CallbackMeta*>(lua_newuserdatauv(L,sizeof(CallbackMeta),0));*meta={*entity,ticket};lua_setfield(L,entry,"native");
    lua_pushvalue(L,1);lua_setfield(L,entry,"entity");lua_pushvalue(L,callback);lua_setfield(L,entry,"callback");
    lua_pushboolean(L,1);lua_setfield(L,entry,"enabled");lua_pushboolean(L,hasContext);lua_setfield(L,entry,"hasContext");
    if(hasContext) {lua_pushvalue(L,2);lua_setfield(L,entry,"context");}
    PushOwner(L);lua_setfield(L,entry,"owner");lua_rawseti(L,callbacks,id);lua_pushinteger(L,id);return 1;
}
int Unregister(lua_State* L) {
    const auto* entity=Entity(L,1);if(!entity)return luaL_argerror(L,1,"native weak Entity expected");
    const lua_Integer id=luaL_checkinteger(L,2);
    if(g_state!=L)return 0;
    PushCallbacks(L);const int callbacks=lua_gettop(L);lua_rawgeti(L,callbacks,id);
    if(!lua_istable(L,-1))return 0;
    const int entry=lua_gettop(L);const auto* meta=Meta(L,entry);
    if(!meta || !Same(meta->entity,*entity) || Owner(L,entry)!=LuaRuntime::Instance().currentModFolder())return 0;
    const CallbackMeta saved=*meta;
    lua_pushboolean(L,0);lua_setfield(L,entry,"enabled");lua_pushnil(L);lua_rawseti(L,callbacks,id);
    if(!Remaining(L,callbacks,saved))entityevents::Unwatch(saved.entity,saved.ticket.watch);
    return 0;
}
void Prune(lua_State* L,int callbacks) {
    lua_pushnil(L);
    while(lua_next(L,callbacks)) {
        if(lua_istable(L,-1)) {
            const int entry=lua_gettop(L);const auto* meta=Meta(L,entry);
            if(meta && !blueprint::IsAlive(meta->entity)) {
                entityevents::Unwatch(meta->entity,meta->ticket.watch);
                lua_pushboolean(L,0);lua_setfield(L,entry,"enabled");
                lua_pushvalue(L,-2);lua_pushnil(L);lua_rawset(L,callbacks);
            }
        }
        lua_pop(L,1);
    }
}
bool Deliver(lua_State* L,entityevents::Snapshot* value,bool synchronous) {
    const int top=lua_gettop(L);bool cancel=false;
    if(!entityevents::Current(*value) || !blueprint::IsAlive(value->entity))return false;
    PushCallbacks(L);const int callbacks=lua_gettop(L);
    lua_newtable(L);const int selected=lua_gettop(L);int count=0;lua_pushnil(L);
    while(lua_next(L,callbacks)) {
        if(lua_istable(L,-1)) {
            const auto* meta=Meta(L,lua_gettop(L));
            if(meta && meta->ticket.watch==value->watch && meta->ticket.firstSequence<=value->sequence && Same(meta->entity,value->entity)) {
                lua_pushvalue(L,-1);lua_rawseti(L,selected,++count);
            }
        }
        lua_pop(L,1);
    }
    lua_createtable(L,0,2);const int event=lua_gettop(L);
    // VU EntityEvent.eventId and MathUtils:FNVHash are signed int32 values.
    // EnterFromGame/ExitUIGraph both have the high bit set; unsigned exposure
    // makes Royale's Escape callback silently miss both comparisons.
    lua_pushinteger(L,static_cast<std::int32_t>(value->eventId));lua_setfield(L,event,"eventId");lua_pushstring(L,value->type);lua_setfield(L,event,"type");
    for(int i=1;i<=count;++i) {
        lua_rawgeti(L,selected,i);const int entry=lua_gettop(L);
        if(!Enabled(L,entry) || !entityevents::Current(*value) || !blueprint::IsAlive(value->entity)) {lua_pop(L,1);continue;}
        const std::wstring owner=Owner(L,entry);
        lua_getfield(L,entry,"callback");lua_getfield(L,entry,"hasContext");const bool context=lua_toboolean(L,-1)!=0;lua_pop(L,1);
        if(context)lua_getfield(L,entry,"context");
        lua_getfield(L,entry,"entity");lua_pushvalue(L,event);
        auto& runtime=LuaRuntime::Instance();const std::wstring previous=runtime.PushCurrentMod(owner);
        const int status=lua_pcall(L,context?3:2,1,0);
        runtime.PopCurrentMod(previous);
        if(status!=LUA_OK)LOG_WARN("EntityEvents callback failed: %s",lua_tostring(L,-1));
        else if(lua_isboolean(L,-1) && !lua_toboolean(L,-1)) {
            if(synchronous)cancel=true;
            else {
                lua_getfield(L,entry,"warnedDeferredCancel");const bool warned=lua_toboolean(L,-1)!=0;lua_pop(L,1);
                if(!warned) {LOG_WARN("EntityEvents: deferred callback returned false after native onEvent; cancellation was unavailable");lua_pushboolean(L,1);lua_setfield(L,entry,"warnedDeferredCancel");}
            }
        }
        lua_pop(L,2); // callback result/error and selected entry
    }
    lua_getfield(L,event,"eventId");int integer=0;const lua_Integer changed=lua_tointegerx(L,-1,&integer);
    if(integer && changed>=(-2147483647LL-1) && changed<=4294967295LL) {
        if(synchronous)value->eventId=static_cast<std::uint32_t>(changed);
        else if(value->eventId!=static_cast<std::uint32_t>(changed))LOG_WARN("EntityEvents: deferred eventId change cannot mutate an already-delivered native event");
    } else if(synchronous)LOG_WARN("EntityEvents: invalid eventId mutation ignored");
    lua_settop(L,top);return cancel;
}
bool TrySynchronous(entityevents::Snapshot* value,bool* cancel) {
    if(!value || !cancel || !blueprint::FrameActive())return false;
    auto& runtime=LuaRuntime::Instance();std::unique_lock<std::recursive_mutex> gate(runtime.TickGate(),std::try_to_lock);
    if(!gate.owns_lock() || !runtime.state() || runtime.state()!=g_state)return false;
    *cancel=Deliver(runtime.state(),value,true);return true;
}
int LifetimeGc(lua_State* L) {
    if(g_state==L) {entityevents::Shutdown();g_state=nullptr;}
    return 0;
}
}
bool RegisterVextEntityEvents(lua_State* L) {
    if(!L)return false;
    if(g_state==L)return true;
    entityevents::Shutdown();g_state=L;
    lua_newtable(L);lua_rawsetp(L,LUA_REGISTRYINDEX,&kCallbacks);
    luaL_newmetatable(L,kLifetime);lua_pushcfunction(L,LifetimeGc);lua_setfield(L,-2,"__gc");lua_pop(L,1);
    lua_newuserdatauv(L,1,0);luaL_setmetatable(L,kLifetime);lua_rawsetp(L,LUA_REGISTRYINDEX,&kSentinel);
    entityevents::Start(&TrySynchronous);return true;
}
bool PushVextEntityEventMethod(lua_State* L,const char* key) {
    if(!key)return false;
    if(std::strcmp(key,"RegisterEventCallback")==0) {lua_pushcfunction(L,Register);return true;}
    if(std::strcmp(key,"UnregisterEventCallback")==0) {lua_pushcfunction(L,Unregister);return true;}
    return false;
}
void PollVextEntityEvents(lua_State* L) {
    if(!L || !blueprint::FrameActive())return;
    auto& runtime=LuaRuntime::Instance();std::lock_guard<std::recursive_mutex> gate(runtime.TickGate());
    if(g_state!=L)return;
    const int top=lua_gettop(L);PushCallbacks(L);Prune(L,lua_gettop(L));lua_settop(L,top);
    entityevents::Snapshot value;
    for(unsigned i=0;i<4096 && entityevents::Poll(&value);++i) {Deliver(L,&value,false);entityevents::Release(&value);}
    lua_settop(L,top);
}
void ShutdownVextEntityEvents(lua_State* L) {
    if(L && g_state!=L)return;
    entityevents::Shutdown();g_state=nullptr;
    if(L) {lua_pushnil(L);lua_rawsetp(L,LUA_REGISTRYINDEX,&kCallbacks);}
}
}
