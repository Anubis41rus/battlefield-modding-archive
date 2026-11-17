#include "NativeVisualSettingsBridge.h"
#include "NativeVisualSettings.h"
#include "NativeBlueprint.h"
#include "EngineResources.h"
#include "InlineHook.h"
#include "LuaRuntime.h"
#include "EVBotsLog.h"
#include <windows.h>
#include <cstdio>
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}
namespace evbots {
namespace {
SRWLOCK settingsLock=SRWLOCK_INIT;
bool color=true,sun=true;
unsigned revision=1;
char session[64]={};
int ServerAccess(lua_State* L) {
 if(blueprint::IsClient())return luaL_error(L,"visual settings server access on client");
 const int key=static_cast<int>(luaL_checkinteger(L,1));
 if(key<0 || key>2)return luaL_error(L,"invalid visual setting key");
 if(key && lua_gettop(L)>1) {
  luaL_checktype(L,2,LUA_TBOOLEAN);
  if(!EVBotsOnServerFrameThread() && !eng::IsInNativeLevelLoadResources())
   return luaL_error(L,"visual settings require server engine context");
 }
 AcquireSRWLockExclusive(&settingsLock);
 if(!session[0])sprintf_s(session,"%lu-%llu",GetCurrentProcessId(),GetTickCount64());
 if(key && lua_gettop(L)>1) {
  bool& target=key==1?color:sun;
  const bool value=lua_toboolean(L,2)!=0;
  if(target!=value) {target=value;++revision;}
 }
 const bool colorValue=color,sunValue=sun;const unsigned rev=revision;
 ReleaseSRWLockExclusive(&settingsLock);
 lua_pushstring(L,session);lua_pushinteger(L,rev);
 lua_pushboolean(L,colorValue);lua_pushboolean(L,sunValue);return 4;
}
int ClientApply(lua_State* L) {
 luaL_checktype(L,1,LUA_TBOOLEAN);luaL_checktype(L,2,LUA_TBOOLEAN);
 const int mask=static_cast<int>(luaL_checkinteger(L,3));
 if(mask<0 || mask>3)return luaL_error(L,"invalid visual settings mask");
 const char* error=nullptr;
 bool ok=blueprint::IsClient() && blueprint::EngineFrameActive();
 if(!ok)error="visual settings require client engine frame";
 if(ok)ok=visualsettings::Initialize(&error);
 if(ok && (mask&1))ok=visualsettings::SetColorCorrectionEnabled(lua_toboolean(L,1)!=0,&error);
 if(ok && (mask&2))ok=visualsettings::SetSunFlareEnabled(lua_toboolean(L,2)!=0,&error);
 lua_pushboolean(L,ok);if(error)lua_pushstring(L,error);else lua_pushnil(L);return 2;
}
const char kVisualSettingsLua[]=R"LUA(
local server,native,state=...
local request,snapshot='__ev_visual_settings_request','__ev_visual_settings_snapshot'
for _,handle in ipairs(state.handles or {}) do handle:Unsubscribe() end
for _,handle in ipairs(state.commands or {}) do RCON:DeregisterCommand(handle) end
state.handles={};state.commands={}
local function subscribe(source,name,fn) state.handles[#state.handles+1]=source:Subscribe(name,fn) end
if server then
 local function send(player)
  local session,revision,color,sun=native(0)
  if player then NetEvents:SendTo(snapshot,player,1,session,revision,color,sun)
  else NetEvents:Broadcast(snapshot,1,session,revision,color,sun) end
 end
 subscribe(NetEvents,request,function(player,...)
  if player~=nil and select('#',...)==1 and (...)==1 then send(player) end
 end)
 subscribe(Events,'Engine:Update',function()
  local session,revision=native(0)
  if state.sentSession~=session or state.sentRevision~=revision then
   send();state.sentSession=session;state.sentRevision=revision
  end
 end)
 state.sentSession=nil
 for key,name in ipairs({'vu.ColorCorrectionEnabled','vu.SunFlareEnabled'}) do
  state.commands[#state.commands+1]=RCON:RegisterCommand(name,RemoteCommandFlag.RequiresLogin,function(_,args)
   if type(args)~='table' or #args>1 then return {'InvalidArguments'} end
   if #args==1 then
    if args[1]~='true' and args[1]~='false' then return {'InvalidArguments'} end
    native(key,args[1]=='true');return {'OK'}
   end
   local _,_,color,sun=native(0)
   local value=color;if key==2 then value=sun end
   return {'OK',tostring(value)}
  end)
 end
else
 local function requestSnapshot() NetEvents:Send(request,1) end
 local elapsed=0
 -- Requests also recover channel reconnects where no new Level:Loaded is emitted.
 subscribe(Events,'Engine:Update',function(dt)
  if type(dt)~='number' or dt<0 or dt~=dt then return end
  elapsed=elapsed+dt
  if elapsed>=5 then elapsed=0;requestSnapshot() end
 end)
 subscribe(NetEvents,snapshot,function(...)
  if select('#',...)~=5 then return end
  local version,session,revision,color,sun=...
  if version~=1 or type(session)~='string' or #session<1 or #session>64 or
   type(revision)~='number' or revision<1 or revision>4294967295 or revision%1~=0 or
   type(color)~='boolean' or type(sun)~='boolean' then return end
  if session==state.session and state.revision and revision<state.revision then return end
  if session~=state.session then state.appliedColor=nil;state.appliedSun=nil end
  state.session=session;state.revision=revision;state.color=color;state.sun=sun;state.pending=true;elapsed=0
 end)
 subscribe(Events,'Level:Destroy',function() state.pending=false;state.appliedColor=nil;state.appliedSun=nil end)
 subscribe(Events,'Level:Loaded',function() state.pending=state.session~=nil;requestSnapshot() end)
 subscribe(Events,'Client:UpdateInput',function()
  if not state.pending then return end
  local mask=(state.appliedColor~=state.color and 1 or 0)+(state.appliedSun~=state.sun and 2 or 0)
  if mask==0 then state.pending=false;return end
  local ok,error=native(state.color,state.sun,mask)
  if ok then state.appliedColor=state.color;state.appliedSun=state.sun;state.pending=false;state.error=nil
  elseif error~=state.error then print('[VisualSettings] pending: '..tostring(error));state.error=error end
 end)
 state.pending=state.session~=nil
 requestSnapshot()
end
)LUA";
}
bool RegisterNativeVisualSettings(lua_State* L) {
 const int top=lua_gettop(L);
 if(luaL_loadbufferx(L,kVisualSettingsLua,sizeof(kVisualSettingsLua)-1,"=[native-visual-settings]","t")!=LUA_OK) {
  LOG_ERR("VisualSettings compile: %s",lua_tostring(L,-1));lua_settop(L,top);return false;
 }
 // Registration follows the Lua realm, also in the API test host without a game module.
 // Native mutations retain their independent engine/frame checks above.
 const bool client=LuaRuntime::Instance().context()==LuaContext::Client;
 lua_pushboolean(L,!client);lua_pushcfunction(L,client?ClientApply:ServerAccess);
 lua_getfield(L,LUA_REGISTRYINDEX,"EVBots.NativeVisualSettings");
 if(lua_isnil(L,-1)) {
  lua_pop(L,1);lua_newtable(L);lua_pushvalue(L,-1);
  lua_setfield(L,LUA_REGISTRYINDEX,"EVBots.NativeVisualSettings");
 }
 const std::wstring previous=LuaRuntime::Instance().PushCurrentMod(L"");
 const int result=lua_pcall(L,3,0,0);
 LuaRuntime::Instance().PopCurrentMod(previous);
 if(result!=LUA_OK)LOG_ERR("VisualSettings register: %s",lua_tostring(L,-1));
 lua_settop(L,top);return result==LUA_OK;
}
}
