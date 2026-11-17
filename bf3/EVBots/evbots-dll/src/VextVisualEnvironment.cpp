#include "VextVisualEnvironment.h"
#include "NativeVisualEnvironment.h"
#include "NativeBlueprint.h"
#include "VextResourceManager.h"
#include "EVBotsLog.h"
#include <climits>
#include <cstring>
#include <new>
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
namespace evbots { namespace {
constexpr char kState[]="evbots.VisualEnvironmentState";
constexpr char kComponent[]="evbots.VisualEnvironmentComponent";
constexpr char kSnapshot[]="evbots.VisualEnvironmentSnapshot";
using Snapshot=std::vector<visual::State>;
struct View { visual::Handle state; const visual::Component* component; };
int Error(lua_State* L,const char* operation,const char* error) {
 return luaL_error(L,"VisualEnvironmentManager:%s: %s",operation,error?error:"native operation failed");
}
void PushValue(lua_State* L,visual::Kind kind,const visual::Value& value) {
 switch(kind) {
  case visual::Kind::Float:lua_pushnumber(L,value.number[0]);return;
  case visual::Kind::Boolean:lua_pushboolean(L,value.integer);return;
  case visual::Kind::Integer:lua_pushinteger(L,value.integer);return;
  case visual::Kind::Asset:
   if(value.asset)ResourcePushOwnedLive(L,static_cast<unsigned long>(value.asset));else lua_pushnil(L);
   return;
  default: {
   const int count=kind==visual::Kind::Vec4?4:kind==visual::Kind::Vec3?3:2;
   lua_getglobal(L,count==4?"Vec4":count==3?"Vec3":"Vec2");
   for(int i=0;i<count;++i)lua_pushnumber(L,value.number[i]);
   lua_call(L,count,1);return;
  }
 }
}
int ReadField(lua_State* L,const visual::Handle& state,const visual::Component* component,const visual::Field* field) {
 visual::Value value;const char* error=nullptr;
 if(!visual::Read(state,component,field,&value,&error))return Error(L,"read",error);
 PushValue(L,field->kind,value);return 1;
}
int StateIndex(lua_State* L) {
 auto* state=static_cast<visual::Handle*>(luaL_checkudata(L,1,kState));
 const char* key=luaL_checkstring(L,2);
 if(std::strcmp(key,"entityName")==0) {lua_getiuservalue(L,1,1);return 1;}
 if(const auto* field=visual::StateField(key))return ReadField(L,*state,nullptr,field);
 if(const auto* component=visual::FindComponent(key)) {
  bool present=false;const char* error=nullptr;
  if(!visual::HasComponent(*state,component,&present,&error))return Error(L,"read component",error);
  if(!present) {lua_pushnil(L);return 1;}
  auto* view=static_cast<View*>(lua_newuserdatauv(L,sizeof(View),0));*view={*state,component};luaL_setmetatable(L,kComponent);return 1;
 }
 lua_pushnil(L);return 1;
}
visual::Value Input(lua_State* L,visual::Kind kind) {
 visual::Value value;
 if(kind==visual::Kind::Asset)luaL_error(L,"runtime visual asset replacement is not implemented");
 if(kind==visual::Kind::Boolean) {luaL_checktype(L,3,LUA_TBOOLEAN);value.integer=lua_toboolean(L,3);}
 else if(kind==visual::Kind::Integer) {
  const lua_Integer integer=luaL_checkinteger(L,3);
  if(integer<INT_MIN || integer>INT_MAX)luaL_error(L,"visual environment value exceeds Int32");
  value.integer=static_cast<int>(integer);
 } else if(kind==visual::Kind::Vec2 || kind==visual::Kind::Vec3 || kind==visual::Kind::Vec4) {
  const char* names[]={"x","y","z","w"};const int count=kind==visual::Kind::Vec2?2:kind==visual::Kind::Vec3?3:4;
  for(int i=0;i<count;++i) {lua_getfield(L,3,names[i]);value.number[i]=static_cast<float>(luaL_checknumber(L,-1));lua_pop(L,1);}
 } else value.number[0]=static_cast<float>(luaL_checknumber(L,3));
 return value;
}
int StateNewIndex(lua_State* L) {
 auto* state=static_cast<visual::Handle*>(luaL_checkudata(L,1,kState));
 const char* key=luaL_checkstring(L,2);const auto* field=visual::StateField(key);
 if(!field || field->offset==12)return luaL_error(L,"VisualEnvironmentState.%s is read-only or unsupported",key);
 const visual::Value value=Input(L,field->kind);
 const char* error=nullptr;if(!visual::WriteState(*state,field,value,&error))return Error(L,"write state",error);
 return 0;
}
int StateEqual(lua_State* L) {
 const auto* a=static_cast<visual::Handle*>(luaL_testudata(L,1,kState));
 const auto* b=static_cast<visual::Handle*>(luaL_testudata(L,2,kState));
 lua_pushboolean(L,a && b && a->state==b->state && a->manager==b->manager && a->generation==b->generation);return 1;
}
int ComponentIndex(lua_State* L) {
 const auto* view=static_cast<View*>(luaL_checkudata(L,1,kComponent));
 const auto* field=visual::FindField(view->component,luaL_checkstring(L,2));
 if(!field) {lua_pushnil(L);return 1;}
 return ReadField(L,view->state,view->component,field);
}
int ComponentNewIndex(lua_State* L) {
 const auto* view=static_cast<View*>(luaL_checkudata(L,1,kComponent));const char* key=luaL_checkstring(L,2);
 const auto* field=visual::FindField(view->component,key);
 if(!field)return luaL_error(L,"unknown visual component field %s",key);
 const visual::Value value=Input(L,field->kind);const char* error=nullptr;
 if(!visual::WriteComponent(view->state,view->component,field,value,&error))return Error(L,"write component",error);
 return 0;
}
int SnapshotGC(lua_State* L) {
 auto* snapshot=static_cast<Snapshot*>(luaL_checkudata(L,1,kSnapshot));snapshot->~Snapshot();return 0;
}
bool PushStates(lua_State* L,const char** error) {
 // Lua owns the C++ snapshot before the first native/string allocation. Even
 // an allocation error while creating the result cannot longjmp past its owner.
 auto* states=new(lua_newuserdatauv(L,sizeof(Snapshot),0)) Snapshot;
 luaL_setmetatable(L,kSnapshot);const int snapshotIndex=lua_gettop(L);
 if(!visual::GetStates(states,error)) {lua_pop(L,1);return false;}
 lua_createtable(L,static_cast<int>(states->size()),0);lua_Integer index=1;
 for(const auto& state:*states) {
  auto* handle=static_cast<visual::Handle*>(lua_newuserdatauv(L,sizeof(visual::Handle),1));*handle=state.handle;
  luaL_setmetatable(L,kState);lua_pushlstring(L,state.name.data(),state.name.size());lua_setiuservalue(L,-2,1);
  lua_rawseti(L,-2,index++);
 }
 lua_remove(L,snapshotIndex);
 return true;
}
int GetStates(lua_State* L) {
 const char* error=nullptr;
 if(!PushStates(L,&error))return Error(L,"GetStates",error);
 return 1;
}
int GetDirty(lua_State* L) {
 bool value=false;const char* error=nullptr;if(!visual::GetDirty(&value,&error))return Error(L,"GetDirty",error);
 lua_pushboolean(L,value);return 1;
}
int SetDirty(lua_State* L) {
 luaL_checktype(L,2,LUA_TBOOLEAN);const char* error=nullptr;
 if(!visual::SetDirty(lua_toboolean(L,2)!=0,&error))return Error(L,"SetDirty",error);return 0;
}
int GetSun(lua_State* L) {
 float value=0;const char* error=nullptr;
 if(!visual::ReadSun(static_cast<unsigned>(lua_tointeger(L,lua_upvalueindex(1))),&value,&error))return Error(L,"GetSunRotation",error);
 lua_pushnumber(L,value);return 1;
}
int SetSun(lua_State* L) {
 const float value=static_cast<float>(luaL_checknumber(L,2));const char* error=nullptr;
 if(!visual::WriteSun(static_cast<unsigned>(lua_tointeger(L,lua_upvalueindex(1))),value,&error))return Error(L,"SetSunRotation",error);
 return 0;
}
} // namespace
bool RegisterVextVisualEnvironment(lua_State* L) {
 if(!blueprint::IsClient())return true;
 const char* error=nullptr;
 if(!visual::Initialize(&error)) {LOG_ERR("VisualEnvironment registration failed: %s",error?error:"unknown");return false;}
 luaL_newmetatable(L,kSnapshot);lua_pushcfunction(L,SnapshotGC);lua_setfield(L,-2,"__gc");lua_pop(L,1);
 luaL_newmetatable(L,kState);
 lua_pushcfunction(L,StateIndex);lua_setfield(L,-2,"__index");
 lua_pushcfunction(L,StateNewIndex);lua_setfield(L,-2,"__newindex");
 lua_pushcfunction(L,StateEqual);lua_setfield(L,-2,"__eq");lua_pop(L,1);
 luaL_newmetatable(L,kComponent);
 lua_pushcfunction(L,ComponentIndex);lua_setfield(L,-2,"__index");
 lua_pushcfunction(L,ComponentNewIndex);lua_setfield(L,-2,"__newindex");lua_pop(L,1);
 lua_newtable(L);
 lua_pushcfunction(L,GetStates);lua_setfield(L,-2,"GetStates");
 lua_pushcfunction(L,GetDirty);lua_setfield(L,-2,"GetDirty");
 lua_pushcfunction(L,SetDirty);lua_setfield(L,-2,"SetDirty");
 const char* gets[]={"GetSunRotationX","GetSunRotationY"};const char* sets[]={"SetSunRotationX","SetSunRotationY"};
 for(int axis=0;axis<2;++axis) {
  lua_pushinteger(L,axis);lua_pushcclosure(L,GetSun,1);lua_setfield(L,-2,gets[axis]);
  lua_pushinteger(L,axis);lua_pushcclosure(L,SetSun,1);lua_setfield(L,-2,sets[axis]);
 }
 lua_setglobal(L,"VisualEnvironmentManager");return true;
}
} // namespace evbots
