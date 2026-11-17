// Actual VU ClientSoldier/ClientControllable/Ragdoll bindings, build 267 follow-up.
// Evidence: VU 107E86C0 (registration), 107E7F40 (health), 107E8070
// (interactive-down), 10E40C00/10E40FC0 (teamId), 10C1EBF0 (active bone).
#include "VextClientSoldier.h"
#include "EngineResources.h"
#include "TeamCapacity.h"
#include <windows.h>
#include <cmath>
#include <cstring>
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
namespace evbots { namespace {
using Ptr=std::uintptr_t;
using blueprint::Handle;
constexpr char kRagdoll[]="evbots.ClientRagdollComponent";
constexpr char kWeapons[]="evbots.ClientSoldierWeaponsComponent";
constexpr char kWeapon[]="evbots.ClientSoldierWeapon";
struct RagdollHandle { Handle soldier; Ptr component; };
struct WeaponHandle { RagdollHandle owner; Ptr weapon; };
enum class Field { Health,Team,Player,Interactive,Dead,Alive,ManDown,Ragdoll,Weapons,CurrentWeapon,WeaponSlot,WeaponValue,SetTeam,Bone };
struct Request {
 Field field; double number=0; bool boolean=false; Ptr pointer=0; unsigned id=0;
 Ptr expectedComponent=0; int bone=0; float transform[8]={}; bool present=false;
 Ptr expectedWeapon=0;int weaponField=0;char name[512]={};
};
Ptr U32(Ptr p) { return *reinterpret_cast<const unsigned long*>(p); }
bool NativeMethod(Ptr object,unsigned slot,Ptr* fn) {
 if(!object)return false;
 const Ptr table=U32(object);if(!table)return false;
 *fn=U32(table+slot);if(!*fn)return false;
 MEMORY_BASIC_INFORMATION info={};
 const Ptr module=reinterpret_cast<Ptr>(GetModuleHandleA("bf3.exe"));
 return module && VirtualQuery(reinterpret_cast<const void*>(*fn),&info,sizeof(info)) &&
   info.State==MEM_COMMIT && info.Type==MEM_IMAGE && reinterpret_cast<Ptr>(info.AllocationBase)==module &&
   (info.Protect&(PAGE_EXECUTE|PAGE_EXECUTE_READ|PAGE_EXECUTE_READWRITE|PAGE_EXECUTE_WRITECOPY)) &&
   !(info.Protect&(PAGE_GUARD|PAGE_NOACCESS));
}
bool OwnerId(Ptr soldier,unsigned* id) {
 // VU107E81D4: direct owner, then its weak counterpart (ClientPlayer weak offset8).
 Ptr player=U32(soldier+0xB8);
 if(!player) { const Ptr weak=U32(soldier+0x238); const Ptr slot=weak?U32(weak):0;player=slot>=8?slot-8:0; }
 if(!player)return false;
 const Ptr main=U32(0x022E3790),client=main?U32(main+0xB4):0,pm=client?U32(client+0x1C):0;
 if(!pm)return false;
 const unsigned candidate=static_cast<unsigned>(U32(player+0x3EC));
 const Ptr slots=U32(pm+0xC0);
 if(!slots || candidate>=U32(pm+8) || U32(slots+candidate*4)!=player)return false;
 *id=candidate;return true;
}
bool Access(Ptr soldier,void* opaque,const char** error) {
 auto* request=static_cast<Request*>(opaque);
 __try {
  switch(request->field) {
   case Field::Health:
    request->number=*reinterpret_cast<const float*>(soldier+0x20);
    if(!std::isfinite(request->number)) { *error="non-finite native soldier health";return false; }
    break;
   case Field::Team: request->number=U32(soldier+0x78);break;
   case Field::SetTeam: *reinterpret_cast<unsigned*>(soldier+0x78)=request->id;break;
   case Field::Player: request->present=OwnerId(soldier,&request->id);break;
   case Field::Ragdoll: request->pointer=U32(soldier+0x368);break;
   case Field::Weapons: request->pointer=U32(soldier+0x35C);break;
   case Field::CurrentWeapon:case Field::WeaponSlot:case Field::WeaponValue: {
    // VU107F7100/107F7170: real slot handler and native weapon vector.
    const Ptr component=U32(soldier+0x35C);
    if(!component || component!=request->expectedComponent)break;
    const Ptr handler=U32(component+0xD4);
    const int slot=handler?*reinterpret_cast<const int*>(handler+4):11;
    if(request->field==Field::WeaponSlot) { request->number=slot;request->present=true;break; }
    const Ptr begin=U32(component+0xBC),end=U32(component+0xC0);
    if(!begin || end<begin || ((end-begin)&3) || end-begin>4096)break;
    Ptr weapon=0;
    if(request->field==Field::CurrentWeapon) {
     if(slot<0 || static_cast<unsigned>(slot)>=(end-begin)/4)break;
     weapon=U32(begin+static_cast<unsigned>(slot)*4);request->pointer=weapon;request->present=weapon!=0;break;
    }
    for(Ptr item=begin;item!=end;item+=4)if(U32(item)==request->expectedWeapon) { weapon=request->expectedWeapon;break; }
    if(!weapon)break;
    request->present=true;
    if(request->weaponField==3) {
     // VU10DA1057..10DA110F: weapon data -> tagged unlock asset -> name string.
     const Ptr weaponData=U32(weapon+0x14);unsigned long unlock=0;
     if(!weaponData || !eng::ReadReferenceTarget(static_cast<unsigned long>(weaponData+0x13C),&unlock) || !unlock ||
        (*reinterpret_cast<const unsigned short*>(unlock+6)&0x800))break;
     const char* name=reinterpret_cast<const char*>(U32(unlock+8));if(!name)break;
     unsigned i=0;for(;i+1<sizeof(request->name) && name[i];++i)request->name[i]=name[i];
     if(name[i]) { *error="native weapon name exceeds supported buffer";return false; }
     request->name[i]=0;break;
    }
    // VU10DA1200/1240/0F80: corrected firing state, not a cached server value.
    const Ptr firing=U32(weapon+0x160);if(!firing)break;
    if(request->weaponField<2)request->number=*reinterpret_cast<const int*>(firing+(request->weaponField==0?0xF4:0xF8));
    else {
     const Ptr modes=U32(firing+0x11C);const unsigned mode=static_cast<unsigned>(U32(firing+0x110));
     if(!modes || mode>255) { *error="native fire-logic array unavailable";return false; }
     request->number=*reinterpret_cast<const int*>(modes+mode*4);
    }
    break;
   }
   case Field::Bone: {
    const Ptr ragdoll=U32(soldier+0x368);
    if(!ragdoll || ragdoll!=request->expectedComponent)break;
    const Ptr data=U32(ragdoll+0x14);if(!data)break;
    unsigned long asset=0;
    if(!eng::ReadReferenceTarget(static_cast<unsigned long>(data+0x60),&asset) || !asset ||
       (*reinterpret_cast<const unsigned short*>(asset+6)&0x800))break;
    const Ptr active=U32(ragdoll+0x50),bones=U32(asset+0xC);
    if(!active || bones<4)break;
    const unsigned count=static_cast<unsigned>(U32(bones-4));
    if(request->bone<0 || count>65536 || static_cast<unsigned>(request->bone)>=count)break;
    std::memcpy(request->transform,reinterpret_cast<const void*>(active+static_cast<unsigned>(request->bone)*0x20),0x20);
    for(float value:request->transform)if(!std::isfinite(value)) { *error="non-finite active bone transform";return false; }
    request->present=true;break;
   }
   default: {
    Ptr object=0;unsigned slot=0;
    if(request->field==Field::ManDown) { object=soldier+0xF0;slot=0xC; }
    else {
     object=U32(soldier+0x354);
     slot=request->field==Field::Interactive?0x20:(request->field==Field::Dead?0x14:0xC);
    }
    // VU returns false when the health/state component has not been created.
    if(!object)break;
    Ptr fn=0;if(!NativeMethod(object,slot,&fn)) { *error="soldier state method outside native client image";return false; }
    using BoolFn=bool(__fastcall*)(void*,void*);
    request->boolean=reinterpret_cast<BoolFn>(fn)(reinterpret_cast<void*>(object),nullptr);
    break;
   }
  }
  return true;
 } __except(EXCEPTION_EXECUTE_HANDLER) { *error="native soldier field access failed";return false; }
}
bool IsSoldier(const Handle& soldier) {
 Ptr type=0;
 return blueprint::IsClient() && blueprint::ReadEntity(soldier,&type,nullptr,nullptr) &&
  eng::IsKindOfName(static_cast<unsigned long>(type),"ClientSoldierEntity");
}
bool Read(const Handle& soldier,Request* request,const char** error) {
 return IsSoldier(soldier) && blueprint::WithEntityContext(soldier,Access,request,error);
}
int ActiveWorldTransform(lua_State* L) {
 const auto* handle=static_cast<RagdollHandle*>(luaL_checkudata(L,1,kRagdoll));
 const lua_Integer bone=luaL_checkinteger(L,2);
 if(bone<0 || bone>65535) { lua_pushnil(L);return 1; }
 Request request;request.field=Field::Bone;request.bone=static_cast<int>(bone);request.expectedComponent=handle->component;
 const char* error=nullptr;
 if(!Read(handle->soldier,&request,&error)) {
  if(error)return luaL_error(L,"RagdollComponent:GetActiveWorldTransform: %s",error);
  lua_pushnil(L);return 1;
 }
 if(!request.present) { lua_pushnil(L);return 1; }
 lua_getglobal(L,"QuatTransform");lua_getglobal(L,"Quat");
 for(int i=0;i<4;++i)lua_pushnumber(L,request.transform[i]);lua_call(L,4,1);
 lua_getglobal(L,"Vec4");for(int i=4;i<8;++i)lua_pushnumber(L,request.transform[i]);lua_call(L,4,1);
 lua_call(L,2,1);return 1;
}
int Wrap(lua_State* L) {
 const lua_Integer value=luaL_checkinteger(L,1);
 if(value<=0 || static_cast<unsigned long long>(value)>0xffffffffULL) { lua_pushnil(L);return 1; }
 const Ptr pointer=static_cast<Ptr>(value),weak=blueprint::CaptureWeakPeer(pointer);
 Handle handle;
 const bool adopted=weak && blueprint::AdoptWeakPeer(pointer,weak,&handle);
 blueprint::DropWeakPeer(weak);
 if(!adopted || !IsSoldier(handle)) { lua_pushnil(L);return 1; }
 auto* out=static_cast<Handle*>(lua_newuserdatauv(L,sizeof(Handle),0));*out=handle;
 luaL_setmetatable(L,"evbots.BlueprintEntity");return 1;
}
int WeaponsIndex(lua_State* L) {
 const auto* owner=static_cast<RagdollHandle*>(luaL_checkudata(L,1,kWeapons));const char* key=luaL_checkstring(L,2);
 Request request;request.expectedComponent=owner->component;
 if(std::strcmp(key,"currentWeapon")==0)request.field=Field::CurrentWeapon;
 else if(std::strcmp(key,"currentWeaponSlot")==0)request.field=Field::WeaponSlot;
 else { lua_pushnil(L);return 1; }
 const char* error=nullptr;
 if(!Read(owner->soldier,&request,&error)) {
  if(error)return luaL_error(L,"ClientSoldierWeaponsComponent.%s: %s",key,error);
  lua_pushnil(L);return 1;
 }
 if(!request.present) { lua_pushnil(L);return 1; }
 if(request.field==Field::WeaponSlot) { lua_pushinteger(L,static_cast<lua_Integer>(request.number));return 1; }
 auto* result=static_cast<WeaponHandle*>(lua_newuserdatauv(L,sizeof(WeaponHandle),0));
 result->owner=*owner;result->weapon=request.pointer;luaL_setmetatable(L,kWeapon);return 1;
}
int WeaponIndex(lua_State* L) {
 const auto* handle=static_cast<WeaponHandle*>(luaL_checkudata(L,1,kWeapon));const char* key=luaL_checkstring(L,2);
 Request request;request.field=Field::WeaponValue;request.expectedComponent=handle->owner.component;request.expectedWeapon=handle->weapon;
 if(std::strcmp(key,"primaryAmmo")==0)request.weaponField=0;
 else if(std::strcmp(key,"secondaryAmmo")==0)request.weaponField=1;
 else if(std::strcmp(key,"fireLogic")==0)request.weaponField=2;
 else if(std::strcmp(key,"name")==0)request.weaponField=3;
 else { lua_pushnil(L);return 1; }
 const char* error=nullptr;
 if(!Read(handle->owner.soldier,&request,&error)) {
  if(error)return luaL_error(L,"ClientSoldierWeapon.%s: %s",key,error);
  lua_pushnil(L);return 1;
 }
 if(!request.present) { lua_pushnil(L);return 1; }
 if(request.weaponField==3)lua_pushstring(L,request.name);else lua_pushinteger(L,static_cast<lua_Integer>(request.number));return 1;
}
} // namespace

bool PushClientSoldierProperty(lua_State* L,const Handle& soldier,const char* key) {
 Request request;
 if(std::strcmp(key,"health")==0)request.field=Field::Health;
 else if(std::strcmp(key,"teamId")==0)request.field=Field::Team;
 else if(std::strcmp(key,"player")==0)request.field=Field::Player;
 else if(std::strcmp(key,"isInteractiveManDown")==0)request.field=Field::Interactive;
 else if(std::strcmp(key,"isDead")==0)request.field=Field::Dead;
 else if(std::strcmp(key,"isAlive")==0 || std::strcmp(key,"alive")==0)request.field=Field::Alive;
 else if(std::strcmp(key,"isManDown")==0)request.field=Field::ManDown;
 else if(std::strcmp(key,"ragdollComponent")==0)request.field=Field::Ragdoll;
 else if(std::strcmp(key,"weaponsComponent")==0)request.field=Field::Weapons;
 else return false;
 if(!IsSoldier(soldier))return false;
 const char* error=nullptr;
 if(!blueprint::WithEntityContext(soldier,Access,&request,&error)) {
  luaL_error(L,"ClientSoldier.%s: %s",key,error?error:"native read failed");return true;
 }
 switch(request.field) {
  case Field::Health:lua_pushnumber(L,request.number);break;
  case Field::Team:lua_pushinteger(L,static_cast<lua_Integer>(request.number));break;
  case Field::Player:
   if(!request.present)lua_pushnil(L);
   else {
    lua_getglobal(L,"PlayerManager");lua_getfield(L,-1,"GetPlayerById");lua_pushvalue(L,-2);
    lua_pushinteger(L,request.id);lua_call(L,2,1);lua_remove(L,-2);
   }
   break;
  case Field::Ragdoll:case Field::Weapons:
   if(!request.pointer)lua_pushnil(L);
   else {
    auto* result=static_cast<RagdollHandle*>(lua_newuserdatauv(L,sizeof(RagdollHandle),0));
    result->soldier=soldier;result->component=request.pointer;luaL_setmetatable(L,request.field==Field::Ragdoll?kRagdoll:kWeapons);
   }
   break;
  default:lua_pushboolean(L,request.boolean);break;
 }
 return true;
}
bool SetClientSoldierProperty(lua_State* L,const Handle& soldier,const char* key,int index) {
 if(std::strcmp(key,"teamId")!=0 || !IsSoldier(soldier))return false;
 const lua_Integer team=luaL_checkinteger(L,index);
 if(team<0 || team>(TeamCapacityInstalled()?127:16)) { luaL_argerror(L,index,"TeamId outside installed native capacity");return true; }
 Request request;request.field=Field::SetTeam;request.id=static_cast<unsigned>(team);const char* error=nullptr;
 if(!blueprint::WithEntityContext(soldier,Access,&request,&error))luaL_error(L,"ClientSoldier.teamId: %s",error?error:"native write failed");
 return true;
}
bool RegisterVextClientSoldier(lua_State* L) {
 luaL_getmetatable(L,"evbots.BlueprintEntity");const bool ready=lua_istable(L,-1);lua_pop(L,1);if(!ready)return false;
 luaL_newmetatable(L,kRagdoll);lua_newtable(L);lua_pushcfunction(L,ActiveWorldTransform);
 lua_setfield(L,-2,"GetActiveWorldTransform");lua_setfield(L,-2,"__index");lua_pop(L,1);
 luaL_newmetatable(L,kWeapons);lua_pushcfunction(L,WeaponsIndex);lua_setfield(L,-2,"__index");lua_pop(L,1);
 luaL_newmetatable(L,kWeapon);lua_pushcfunction(L,WeaponIndex);lua_setfield(L,-2,"__index");lua_pop(L,1);
 lua_pushcfunction(L,Wrap);lua_setglobal(L,"__cls_wrap");return true;
}
} // namespace evbots
