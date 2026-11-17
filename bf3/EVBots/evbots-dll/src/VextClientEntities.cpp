// Build 267: shared native Blueprint/EntityBus bindings. Client frame ownership
// is provided by NativeBlueprint::EnterClientFrame/LeaveClientFrame at the real tick.
#include "VextClientEntities.h"
#include "NativeBlueprint.h"
#include "NativeCollision.h"
#include "NativeSpatial.h"
#include "NativePlayerEvents.h"
#include "VextEntityEvents.h"
#include "VextResourceManager.h"
#include "VextClientSoldier.h"
#include "EngineResources.h"
#include "EVBotsLog.h"
#include "LuaRuntime.h"
#include <cmath>
#include <cstring>
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
namespace evbots {
void EVBotsPushLiveInstance(lua_State*,unsigned long);
namespace {
constexpr char kEntity[]="evbots.BlueprintEntity";
constexpr char kBus[]="evbots.BlueprintBus";
const char kCallbacks=0,kCallbackLifetime=0;
using blueprint::Handle;
struct LiveDc { unsigned long ptr,type; unsigned char shape,writable,owned,reserved; };
static_assert(sizeof(LiveDc)==12,"ResourceManager native userdata ABI");
Handle* Entity(lua_State* L,int i) { return static_cast<Handle*>(luaL_testudata(L,i,kEntity)); }
Handle* Bus(lua_State* L,int i) { return static_cast<Handle*>(luaL_testudata(L,i,kBus)); }
void PushHandle(lua_State* L,const Handle& h,const char* meta,int values=0) {
 auto* p=static_cast<Handle*>(lua_newuserdatauv(L,sizeof(Handle),values));
 *p=h; luaL_setmetatable(L,meta);
}
int Failure(lua_State* L,const char* operation,const char* error,bool nil=false) {
 LOG_WARN("Blueprint %s refused: %s",operation,error?error:"native call failed");
 if(nil) lua_pushnil(L); else lua_pushboolean(L,0);
 lua_pushstring(L,error?error:"native call failed"); return 2;
}
bool IsNativeStruct(lua_State* L,int index,const char* name) {
 auto* value=static_cast<LiveDc*>(luaL_testudata(L,index,"vext.LiveDataContainer"));
 if(!value || lua_rawlen(L,index)!=sizeof(LiveDc) || value->shape!=1) return false;
 eng::TypeDesc desc;
 return eng::Describe(value->type,&desc) && desc.name && std::strcmp(desc.name,name)==0;
}
bool ReadTransform(lua_State* L,int index,float out[16]) {
 index=lua_absindex(L,index);
 if(!lua_istable(L,index) && !IsNativeStruct(L,index,"LinearTransform")) return false;
 const char* rows[]={"left","up","forward","trans"};
 const char* cols[]={"x","y","z"};
 for(int r=0;r<4;++r) {
  lua_getfield(L,index,rows[r]);
  if(!lua_istable(L,-1) && !IsNativeStruct(L,-1,"Vec3")) { lua_pop(L,1); return false; }
  for(int c=0;c<3;++c) {
   lua_getfield(L,-1,cols[c]); int valid=0;
   const double x=lua_tonumberx(L,-1,&valid); lua_pop(L,1);
   if(!valid || !std::isfinite(x) || !std::isfinite(static_cast<float>(x))) { lua_pop(L,1); return false; }
   out[r*4+c]=static_cast<float>(x);
  }
  out[r*4+3]=0; lua_pop(L,1);
 }
 return true;
}
int CreateCommon(lua_State* L,bool single) {
 const char* operation=single?"CreateEntity":"CreateEntitiesFromBlueprint";
 auto* live=static_cast<LiveDc*>(luaL_testudata(L,1,"vext.LiveDataContainer"));
 if(!live || lua_rawlen(L,1)!=sizeof(LiveDc) || live->shape!=0)
  return Failure(L,operation,single?"expected a live native EntityData, not an offline EBX proxy":"expected a live native Blueprint, not an offline EBX proxy",true);
 blueprint::Params params;
 if(!lua_istable(L,2) && !IsNativeStruct(L,2,"LinearTransform"))
  return Failure(L,operation,"expected LinearTransform or EntityCreationParams",true);
 lua_getfield(L,2,"transform");
 if(lua_isnil(L,-1)) {
  lua_pop(L,1);
  if(!ReadTransform(L,2,params.transform)) return Failure(L,operation,"invalid LinearTransform",true);
 } else {
  const bool valid=ReadTransform(L,-1,params.transform); lua_pop(L,1);
  if(!valid) return Failure(L,operation,"invalid params.transform",true);
  lua_getfield(L,2,"variationNameHash");
  if(!lua_isnil(L,-1)) {
   int validNumber=0; const lua_Integer n=lua_tointegerx(L,-1,&validNumber);
   if(!validNumber || n<0 || static_cast<unsigned long long>(n)>0xffffffffULL) { lua_pop(L,1); return Failure(L,operation,"variationNameHash must be uint32",true); }
   params.variation=static_cast<std::uint32_t>(n);
  }
  lua_pop(L,1); lua_getfield(L,2,"networked"); params.networked=lua_toboolean(L,-1)!=0; lua_pop(L,1);
  lua_getfield(L,2,"parentRepresentative"); const bool parent=!lua_isnil(L,-1); lua_pop(L,1);
  if(parent && !single) return Failure(L,operation,"parentRepresentative lifetime is not implemented",true);
 }
 if(single) {
  Handle entity;const char* error=nullptr;
  if(!blueprint::CreateSingle(live->ptr,live->owned==1,params,&entity,&error))return Failure(L,operation,error,true);
  PushHandle(L,entity,kEntity);return 1;
 }
 blueprint::Result result; const char* error=nullptr;
 if(!blueprint::Create(live->ptr,params,&result,&error)) return Failure(L,"CreateEntitiesFromBlueprint",error,true);
 PushHandle(L,result.bus,kBus,1); lua_createtable(L,static_cast<int>(result.entities.size()),0);
 int i=0; for(const auto& entity:result.entities) { PushHandle(L,entity,kEntity); lua_rawseti(L,-2,++i); }
 lua_setiuservalue(L,-2,1); return 1;
}
int Create(lua_State* L) { return CreateCommon(L,false); }
int CreateSingle(lua_State* L) { return CreateCommon(L,true); }
int IsClientRealm(lua_State* L) { lua_pushboolean(L,blueprint::IsClient());return 1; }
int Init(lua_State* L) {
 auto* h=Entity(L,1); if(!h) return luaL_argerror(L,1,"native Entity expected");
 const int realm=static_cast<int>(luaL_checkinteger(L,2)); const char* error=nullptr;
 if(!blueprint::Init(*h,realm,lua_toboolean(L,3)!=0,lua_toboolean(L,4)!=0,&error)) return Failure(L,"Entity:Init",error);
 lua_pushboolean(L,1); return 1;
}
int Destroy(lua_State* L) {
 auto* h=Entity(L,1); if(!h) return luaL_argerror(L,1,"native Entity expected");
 const char* error=nullptr;
 if(!blueprint::Destroy(*h,&error)) return Failure(L,"Entity:Destroy",error);
 lua_pushboolean(L,1); return 1;
}
int FireEvent(lua_State* L) {
 auto* h=Entity(L,1); if(!h) return luaL_argerror(L,1,"native Entity expected");
 if(IsNativePlayerEvent(L,2))return FireNativePlayerEvent(L,2,*h);
 std::uint32_t id=0;
 if(lua_type(L,2)==LUA_TSTRING) {
  const char* s=lua_tostring(L,2); id=0x1505;
  while(*s) id=(id*33)^static_cast<std::uint32_t>(static_cast<signed char>(*s++));
 } else {
  if(lua_istable(L,2) || lua_isuserdata(L,2)) lua_getfield(L,2,"eventId"); else lua_pushvalue(L,2);
  int valid=0; const auto n=lua_tointegerx(L,-1,&valid); lua_pop(L,1);
  if(!valid || n<(-2147483647LL-1) || n>4294967295LL) return Failure(L,"Entity:FireEvent","expected an event name or int32 eventId");
  id=static_cast<std::uint32_t>(n);
 }
 if(id==0x075306FD) {
  std::uintptr_t type=0;
  if(!blueprint::IsClient() && blueprint::ReadEntity(*h,&type,nullptr,nullptr) && type==0x02C168DC)
   return FireNativeHumanUnspawn(L,*h);
 }
 const char* error=nullptr;
 if(!blueprint::FireEvent(*h,id,&error)) return Failure(L,"Entity:FireEvent",error);
 lua_pushboolean(L,1); return 1;
}
int ListRoundEntities(lua_State* L) {
 const char* name=luaL_checkstring(L,1);std::vector<Handle> entities;const char* error=nullptr;
 if(!blueprint::ListEntities(name,&entities,&error))return Failure(L,"GetIterator",error,true);
 lua_createtable(L,static_cast<int>(entities.size()),0);int n=0;
 for(const Handle& h:entities) { PushHandle(L,h,kEntity);lua_rawseti(L,-2,++n); }
 return 1;
}
int Is(lua_State* L) {
 const auto* h=Entity(L,1); std::uintptr_t ti=0;
 const bool alive=h && blueprint::ReadEntity(*h,&ti,nullptr,nullptr);
 lua_pushboolean(L,alive && eng::IsKindOfName(static_cast<unsigned long>(ti),luaL_checkstring(L,2))); return 1;
}
int Equals(lua_State* L) {
 const auto* a=Entity(L,1); const auto* b=Entity(L,2);
 if(!a || !b) { a=Bus(L,1); b=Bus(L,2); }
 lua_pushboolean(L,a && b && a->pointer==b->pointer && a->generation==b->generation); return 1;
}
bool Same(const Handle* a,const Handle& b) { return a && a->pointer==b.pointer && a->generation==b.generation; }
void PushCallbacks(lua_State* L) {
 lua_rawgetp(L,LUA_REGISTRYINDEX,&kCallbacks);
 if(!lua_istable(L,-1)) { lua_pop(L,1);lua_newtable(L);lua_pushvalue(L,-1);lua_rawsetp(L,LUA_REGISTRYINDEX,&kCallbacks); }
}
void PushOwner(lua_State* L) {
 const auto& folder=LuaRuntime::Instance().currentModFolder();
 const int n=WideCharToMultiByte(CP_UTF8,0,folder.data(),static_cast<int>(folder.size()),nullptr,0,nullptr,nullptr);
 std::string utf8(n,'\0');if(n)WideCharToMultiByte(CP_UTF8,0,folder.data(),static_cast<int>(folder.size()),utf8.data(),n,nullptr,nullptr);
 lua_pushlstring(L,utf8.data(),utf8.size());
}
std::wstring Owner(lua_State* L,int entry) {
 lua_getfield(L,entry,"owner");size_t len=0;const char* text=lua_tolstring(L,-1,&len);
 const int n=text?MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,static_cast<int>(len),nullptr,0):0;
 std::wstring folder(n,L'\0');if(n)MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text,static_cast<int>(len),folder.data(),n);
 lua_pop(L,1);return folder;
}
int RegisterCollision(lua_State* L) {
 auto* h=Entity(L,1);if(!h)return luaL_argerror(L,1,"native Entity expected");
 const bool context=lua_isfunction(L,3)!=0;const int callback=context?3:2;
 if(!lua_isfunction(L,callback))return luaL_argerror(L,callback,"collision callback function expected");
 const char* error=nullptr;
 if(!collision::Watch(*h,&error))return Failure(L,"RegisterCollisionCallback",error,true);
 PushCallbacks(L);const int callbacks=lua_gettop(L);
 lua_getfield(L,callbacks,"nextId");const auto id=lua_tointeger(L,-1)+1;lua_pop(L,1);
 if(id<=0 || id>0x7fffffff)return Failure(L,"RegisterCollisionCallback","callback identifier space exhausted",true);
 lua_pushinteger(L,id);lua_setfield(L,callbacks,"nextId");lua_createtable(L,0,6);
 lua_pushvalue(L,1);lua_setfield(L,-2,"entity");lua_pushvalue(L,callback);lua_setfield(L,-2,"callback");
 lua_pushboolean(L,1);lua_setfield(L,-2,"enabled");lua_pushboolean(L,context);lua_setfield(L,-2,"hasContext");
 if(context) { lua_pushvalue(L,2);lua_setfield(L,-2,"context"); }
 PushOwner(L);lua_setfield(L,-2,"owner");lua_rawseti(L,callbacks,id);lua_pushinteger(L,id);return 1;
}
int UnregisterCollision(lua_State* L) {
 auto* h=Entity(L,1);if(!h)return luaL_argerror(L,1,"native Entity expected");
 const auto id=luaL_checkinteger(L,2);PushCallbacks(L);const int callbacks=lua_gettop(L);
 lua_rawgeti(L,callbacks,id);
 if(!lua_istable(L,-1)) { lua_pushboolean(L,0);return 1; }
 lua_getfield(L,-1,"entity");const bool owned=Same(Entity(L,-1),*h);lua_pop(L,1);
 if(!owned) { lua_pushboolean(L,0);return 1; }
 lua_pushboolean(L,0);lua_setfield(L,-2,"enabled");lua_pop(L,1);lua_pushnil(L);lua_rawseti(L,callbacks,id);
 bool remaining=false;lua_pushnil(L);
 while(lua_next(L,callbacks)) {
  if(lua_istable(L,-1)) { lua_getfield(L,-1,"entity");remaining=remaining||Same(Entity(L,-1),*h);lua_pop(L,1); }
  lua_pop(L,1);
 }
 if(!remaining)collision::Unwatch(*h);
 lua_pushboolean(L,1);return 1;
}
int CallbackLifetimeGc(lua_State*) { collision::ClearWatches();return 0; }
int EntityIndex(lua_State* L) {
 auto* h=Entity(L,1); const char* key=luaL_checkstring(L,2);
 if(!h) { lua_pushnil(L); return 1; }
 if(blueprint::IsClient() && PushClientSoldierProperty(L,*h,key))return 1;
 if(PushVextEntityEventMethod(L,key))return 1;
 if(std::strcmp(key,"Init")==0) { lua_pushcfunction(L,Init); return 1; }
 if(std::strcmp(key,"Destroy")==0) { lua_pushcfunction(L,Destroy); return 1; }
 if(std::strcmp(key,"FireEvent")==0) { lua_pushcfunction(L,FireEvent); return 1; }
 if(std::strcmp(key,"RegisterCollisionCallback")==0) { lua_pushcfunction(L,RegisterCollision); return 1; }
 if(std::strcmp(key,"UnregisterCollisionCallback")==0) { lua_pushcfunction(L,UnregisterCollision); return 1; }
 if(std::strcmp(key,"Is")==0 || std::strcmp(key,"IsA")==0) { lua_pushcfunction(L,Is); return 1; }
 if(std::strcmp(key,"instanceId")==0) { lua_pushinteger(L,static_cast<lua_Integer>(h->pointer)); return 1; }
 if(std::strcmp(key,"isValid")==0) { lua_pushboolean(L,blueprint::IsAlive(*h)); return 1; }
 if(std::strcmp(key,"typeInfo")==0) {
  std::uintptr_t ti=0; eng::TypeDesc d;
  if(!blueprint::ReadEntity(*h,&ti,nullptr,nullptr) || !eng::Describe(static_cast<unsigned long>(ti),&d)) { lua_pushnil(L); return 1; }
  lua_createtable(L,0,1); lua_pushstring(L,d.name); lua_setfield(L,-2,"name"); return 1;
 }
 if(std::strcmp(key,"data")==0) {
  std::uintptr_t data=0;
  if(!blueprint::ReadEntity(*h,nullptr,&data,nullptr)) { lua_pushnil(L); return 1; }
  EVBotsPushLiveInstance(L,static_cast<unsigned long>(data)); return 1;
 }
 if(std::strcmp(key,"transform")==0 || std::strcmp(key,"worldTransform")==0) {
  float m[16];const char* error=nullptr;
  if(!spatial::GetTransform(*h,m,&error))return Failure(L,"Entity.transform",error,true);
  lua_getglobal(L,"LinearTransform");
  for(int r=0;r<4;++r) { lua_getglobal(L,"Vec3"); for(int c=0;c<3;++c) lua_pushnumber(L,m[r*4+c]); lua_call(L,3,1); }
  lua_call(L,4,1); return 1;
 }
 lua_pushnil(L); return 1;
}
int EntityNewIndex(lua_State* L) {
 const auto* h=Entity(L,1);const char* key=luaL_checkstring(L,2);
 if(!h)return luaL_argerror(L,1,"native Entity expected");
 if(blueprint::IsClient() && SetClientSoldierProperty(L,*h,key,3))return 0;
 if(std::strcmp(key,"transform")!=0)return luaL_error(L,"Entity.%s is not writable",key);
 float matrix[16];if(!ReadTransform(L,3,matrix))return luaL_error(L,"Entity.transform requires a finite LinearTransform");
 const char* error=nullptr;
 if(!spatial::SetTransform(*h,matrix,&error))return luaL_error(L,"Entity.transform refused: %s",error?error:"native setter failed");
 return 0;
}
int BusIndex(lua_State* L) {
 auto* h=Bus(L,1); const char* key=luaL_checkstring(L,2);
 if(!h) { lua_pushnil(L); return 1; }
 if(std::strcmp(key,"instanceId")==0) { lua_pushinteger(L,static_cast<lua_Integer>(h->pointer)); return 1; }
 if(std::strcmp(key,"isValid")==0) { lua_pushboolean(L,blueprint::IsBusAlive(*h)); return 1; }
 if(std::strcmp(key,"entities")==0) {
  lua_newtable(L); const int output=lua_gettop(L);
  if(blueprint::IsBusAlive(*h)) {
   lua_getiuservalue(L,1,1); const int input=lua_gettop(L); int n=0;
   for(lua_Integer i=1;i<=static_cast<lua_Integer>(lua_rawlen(L,input));++i) {
    lua_rawgeti(L,input,i); auto* e=Entity(L,-1);
    if(e && blueprint::IsAlive(*e)) lua_rawseti(L,output,++n); else lua_pop(L,1);
   }
   lua_pop(L,1);
  }
  return 1;
 }
 lua_pushnil(L); return 1;
}
int IsWrapper(lua_State* L) { lua_pushboolean(L,Entity(L,1)!=nullptr || Bus(L,1)!=nullptr); return 1; }
int Cast(lua_State* L) {
 const char* kind=luaL_checkstring(L,1);
 if(std::strcmp(kind,"PhysicsEntity")==0)kind=blueprint::IsClient()?"ClientPhysicsEntity":"ServerPhysicsEntity";
 if(std::strcmp(kind,"SoldierEntity")==0)kind=blueprint::IsClient()?"ClientSoldierEntity":"ServerSoldierEntity";
 if(std::strcmp(kind,"EntityBus")==0) { const auto* b=Bus(L,2); if(b && blueprint::IsBusAlive(*b)) { lua_pushvalue(L,2); return 1; } }
 else {
  const auto* e=Entity(L,2); std::uintptr_t type=0;
  if(e && blueprint::ReadEntity(*e,&type,nullptr,nullptr) && eng::IsKindOfName(static_cast<unsigned long>(type),kind)) { lua_pushvalue(L,2); return 1; }
 }
 lua_pushnil(L); return 1;
}
constexpr char kSource[]=R"LUA(
local create, isWrapper, cast, listRound, createSingle, isClient = ...
local names = {'EntityBus','EntityBusPeer','Entity','SpatialEntity','PhysicsEntity','SoundEntity','SoldierEntity'}
for _, name in ipairs(names) do
  local old = rawget(_G, name)
  if type(old) == 'table' then
    local mt = getmetatable(old) or {}
    local call = mt.__call
    mt.__call = function(self, value, ...)
      if isWrapper(value) then return cast(name, value) end
      if call then return call(self, value, ...) end
      return nil
    end
    setmetatable(old, mt)
  else
    _G[name] = function(value, ...)
      if isWrapper(value) then return cast(name, value) end
      if type(old) == 'function' then return old(value, ...) end
      return nil
    end
  end
end
if EntityCreationParams == nil then
  function EntityCreationParams()
    return {transform=LinearTransform(), variationNameHash=0, networked=false}
  end
end
function EntityManager:CreateEntitiesFromBlueprint(blueprint, params)
  return create(blueprint, params)
end
function EntityManager:CreateEntity(data, params)
  return createSingle(data, params)
end
-- Client classes use the stock exact-type iterator. Keep existing server
-- fun-bots tables/Next/RegisterEventCallback for InputRestriction, adding only
-- FireEvent captured against the current native weak generation.
local oldIterator = EntityManager.GetIterator
local roundTypes = {ServerTicketCounterEntity=true,ServerRoundOverEntity=true,ServerInputRestrictionEntity=true,
  ServerCharacterSpawnEntity=true,ServerHumanPlayerEntity=true,ServerVehicleSpawnEntity=true,LocatorEntity=true}
function EntityManager:GetIterator(name)
  local client = isClient()
  if not client and not roundTypes[name] then return oldIterator(self,name) end
  local entities, reason = listRound(name)
  if entities == nil then
    -- Existing fun-bots read/event subscriptions must still work when the new
    -- mutation layer is disabled or the caller is outside the engine frame.
    if not client and name == 'ServerInputRestrictionEntity' then return oldIterator(self,name) end
    if not client and (name == 'ServerCharacterSpawnEntity' or name == 'ServerVehicleSpawnEntity') then
      local old = oldIterator(self,name)
      return {Next=function()
        local value = old:Next()
        if value == nil then return nil end
        local oldFire = value._legacySpawnFire or value.FireEvent
        value._legacySpawnFire = oldFire
        value.FireEvent = function(self,event,...)
          if name == 'ServerVehicleSpawnEntity' or (EVBotsIsNativePlayerEvent ~= nil and EVBotsIsNativePlayerEvent(event)) then
            error(name..' has no current native weak handle: '..tostring(reason),2)
          end
          return oldFire(self,event,...)
        end
        return value
      end}
    end
    error('EntityManager:GetIterator: '..tostring(reason),2)
  end
  if not client and (name == 'ServerInputRestrictionEntity' or name == 'ServerCharacterSpawnEntity' or name == 'ServerVehicleSpawnEntity') then
    local byPointer = {}
    for _, entity in ipairs(entities) do byPointer[entity.instanceId] = entity end
    local old = oldIterator(self,name)
    local it = {}
    function it:Next()
      local value = old:Next()
      if value == nil then return nil end
      local handle = byPointer[value.instanceId]
      if handle == nil or not handle.isValid then error(name..' iterator has no current native weak handle',2) end
      if name == 'ServerCharacterSpawnEntity' then
        local oldFire = value._legacySpawnFire or value.FireEvent
        value._legacySpawnFire = oldFire
        -- Use real GUID and full blueprintTransform from the current native data.
        -- Keep legacy bot table/bus/transform and the existing bot spawn method.
        value.data = handle.data
        value.FireEvent = function(self,event,...)
          if EVBotsIsNativePlayerEvent ~= nil and EVBotsIsNativePlayerEvent(event) then
            return handle:FireEvent(event,...)
          end
          return oldFire(self,event,...)
        end
      else
        value.FireEvent = function(_,event,...) return handle:FireEvent(event,...) end
      end
      return value
    end
    return it
  end
  local index = 0
  return {Next=function()
    repeat
      index=index+1
      local entity=entities[index]
      if entity==nil or entity.isValid then return entity end
    until false
  end}
end
)LUA";
}
bool RegisterVextBlueprintEntities(lua_State* L) {
 const int top=lua_gettop(L);
 luaL_newmetatable(L,kEntity); lua_pushcfunction(L,EntityIndex); lua_setfield(L,-2,"__index");
 lua_pushcfunction(L,EntityNewIndex);lua_setfield(L,-2,"__newindex");
 lua_pushcfunction(L,Equals); lua_setfield(L,-2,"__eq"); lua_pop(L,1);
 luaL_newmetatable(L,kBus); lua_pushcfunction(L,BusIndex); lua_setfield(L,-2,"__index"); lua_pushcfunction(L,Equals); lua_setfield(L,-2,"__eq"); lua_pop(L,1);
 luaL_newmetatable(L,"evbots.CollisionCallbackLifetime");lua_pushcfunction(L,CallbackLifetimeGc);lua_setfield(L,-2,"__gc");lua_pop(L,1);
 lua_newuserdatauv(L,1,0);luaL_setmetatable(L,"evbots.CollisionCallbackLifetime");lua_rawsetp(L,LUA_REGISTRYINDEX,&kCallbackLifetime);
 if(luaL_loadbufferx(L,kSource,sizeof(kSource)-1,"=[native-blueprint]","t")==LUA_OK) {
  lua_pushcfunction(L,Create); lua_pushcfunction(L,IsWrapper); lua_pushcfunction(L,Cast);lua_pushcfunction(L,ListRoundEntities);
  lua_pushcfunction(L,CreateSingle);lua_pushcfunction(L,IsClientRealm);
  if(lua_pcall(L,6,0,0)==LUA_OK) { lua_settop(L,top); return true; }
 }
 LOG_ERR("Blueprint bindings: %s",lua_tostring(L,-1)); lua_settop(L,top); return false;
}
void PollVextBlueprintCollisions(lua_State* L) {
 if(!L || !blueprint::FrameActive())return;
 const int top=lua_gettop(L);PushCallbacks(L);const int callbacks=lua_gettop(L);
 // Remove dead subscriptions even when no new physics event arrived.
 lua_pushnil(L);
 while(lua_next(L,callbacks)) {
  if(lua_istable(L,-1)) {
   lua_getfield(L,-1,"entity");const auto* entity=Entity(L,-1);
   if(entity && !blueprint::IsAlive(*entity)) {
    collision::Unwatch(*entity);lua_pushboolean(L,0);lua_setfield(L,-3,"enabled");
    lua_pushvalue(L,-3);lua_pushnil(L);lua_rawset(L,callbacks);
   }
   lua_pop(L,1);
  }
  lua_pop(L,1);
 }
 collision::Snapshot value;
 for(unsigned event=0;event<1024 && collision::Poll(&value);++event) {
  Handle other;
  const bool usable=blueprint::IsAlive(value.self) && (!value.other || collision::OtherHandle(value,&other));
  if(!usable) { collision::Release(&value);continue; }
  lua_newtable(L);const int selected=lua_gettop(L);int count=0;lua_pushnil(L);
  while(lua_next(L,callbacks)) {
   if(lua_istable(L,-1)) {
    lua_getfield(L,-1,"entity");const bool match=Same(Entity(L,-1),value.self);lua_pop(L,1);
    if(match) { lua_pushvalue(L,-1);lua_rawseti(L,selected,++count); }
   }
   lua_pop(L,1);
  }
  if(count==0) { collision::Release(&value);lua_pop(L,1);continue; }
  lua_createtable(L,0,8);const int info=lua_gettop(L);
  if(value.other) { PushHandle(L,other,kEntity);lua_setfield(L,info,"entity"); }
  lua_pushnumber(L,value.speed);lua_setfield(L,info,"speed");lua_pushinteger(L,value.part);lua_setfield(L,info,"part");
  // Exact VU getter 10D467A0: ucomiss [info+40],[177C90E4=0], lahf/test ah,44.
  lua_pushboolean(L,value.speed!=0.0f);lua_setfield(L,info,"isInstantlyDestructible");
  const auto material=value.material;value.material=0;
  ResourcePushOwnedLive(L,static_cast<unsigned long>(material));lua_setfield(L,info,"material");
  const auto ownMaterial=value.ownMaterial;value.ownMaterial=0;
  ResourcePushOwnedLive(L,static_cast<unsigned long>(ownMaterial));lua_setfield(L,info,"ownMaterial");
  collision::Release(&value);
  lua_getglobal(L,"Vec3");for(float n:value.position)lua_pushnumber(L,n);lua_call(L,3,1);lua_setfield(L,info,"position");
  lua_getglobal(L,"Vec3");for(float n:value.normal)lua_pushnumber(L,n);lua_call(L,3,1);lua_setfield(L,info,"normal");
  for(int i=1;i<=count;++i) {
   lua_rawgeti(L,selected,i);const int entry=lua_gettop(L);lua_getfield(L,entry,"enabled");
   const bool enabled=lua_toboolean(L,-1)!=0;lua_pop(L,1);
   if(!enabled || !blueprint::IsAlive(value.self)) { lua_pop(L,1);continue; }
   const std::wstring owner=Owner(L,entry);
   lua_getfield(L,entry,"callback");lua_getfield(L,entry,"hasContext");const bool context=lua_toboolean(L,-1)!=0;lua_pop(L,1);
   if(context)lua_getfield(L,entry,"context");
   lua_getfield(L,entry,"entity");lua_pushvalue(L,info);
   const std::wstring previous=LuaRuntime::Instance().PushCurrentMod(owner);
   const int status=lua_pcall(L,context?3:2,0,0);
   LuaRuntime::Instance().PopCurrentMod(previous);
   if(status!=LUA_OK) { LOG_WARN("Collision callback: %s",lua_tostring(L,-1));lua_pop(L,1); }
   lua_pop(L,1);
  }
  lua_pop(L,2);
 }
 lua_settop(L,top);
}
}
