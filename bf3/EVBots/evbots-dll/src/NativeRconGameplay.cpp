// VU SuppressionMultiplier: core101E712C -> global1087430C ->
// Server:SuppressEnemies101CFA70, seventh cdecl argument. Dedicated counterpart
// 01338C70 is attributed by SuppressionUtil_suppressEnemies tag and both callers.
// See D:/Games-cache/re/royale-entry-20261117/rcon-gameplay-report.md.
#include "NativeRconGameplay.h"
#include "InlineHook.h"
#include "EngineResources.h"
#include "EVBotsLog.h"
#include <atomic>
#include <cstdint>
#include <cstring>
#include <xmmintrin.h>
extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace evbots {
namespace {
InlineHook g_suppressionHook;
std::atomic<unsigned> g_suppressionPercent{100};
using SuppressFn = void(__cdecl*)(uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,uintptr_t,float);

void __cdecl SuppressEnemies(uintptr_t a1,uintptr_t a2,uintptr_t a3,uintptr_t a4,uintptr_t a5,uintptr_t a6,float modifier) {
    auto original = reinterpret_cast<SuppressFn>(g_suppressionHook.Trampoline());
    // InlineHook publishes the trampoline before making the detour reachable.
    if (!original) { LOG_ERR("Suppression: missing native trampoline"); return; }
    const float scale=static_cast<float>(g_suppressionPercent.load())/100.0f;
    original(a1,a2,a3,a4,a5,a6,modifier*scale);
}

bool SuppressionCodeMatches(uintptr_t base, bool installed) {
    __try {
        const unsigned char prologue[]={0x81,0xEC,0xEC,0,0,0,0x53,0x33,0xDB,0x55,0x56};
        const unsigned char consumer[]={0xD8,0x8C,0x24,0x18,0x01,0,0};
        const unsigned char discard[]={0x85,0xC0,0x74,0x10};
        const unsigned char callerCleanup[]={0x83,0xC4,0x1C};
        if (!installed && std::memcmp(reinterpret_cast<void*>(base+0xF38C70),prologue,sizeof(prologue))) return false;
        if (std::memcmp(reinterpret_cast<void*>(base+0xF38F09),consumer,sizeof(consumer)) ||
            std::memcmp(reinterpret_cast<void*>(base+0xF38F40),discard,sizeof(discard))) return false;
        for (uintptr_t offset : {uintptr_t(0xEC66E8),uintptr_t(0xEC6818)}) {
            const auto* call=reinterpret_cast<const unsigned char*>(base+offset);
            if (call[0]!=0xE8 || base+offset+5+*reinterpret_cast<const int32_t*>(call+1)!=base+0xF38C70 ||
                std::memcmp(call+9,callerCleanup,sizeof(callerCleanup))) return false;
        }
        const auto* tag=reinterpret_cast<const unsigned char*>(base+0xF38C9E);
        return tag[0]==0x68 && *reinterpret_cast<const uintptr_t*>(tag+1)==base+0x2296E48 &&
               std::strcmp(reinterpret_cast<const char*>(base+0x2296E48),"SuppressionUtil_suppressEnemies")==0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

const char* SuppressionAccess(bool set, unsigned requested, int* actual) {
    if (!EVBotsOnServerFrameThread() && !eng::IsInNativeLevelLoadResources())
        return "requires native engine frame or Level:LoadResources callback";
    const uintptr_t base=reinterpret_cast<uintptr_t>(::GetModuleHandleW(L"bf3_server_Final.exe"));
    if (!base) return "dedicated server is unavailable";
    char disabled[4]={};
    if (::GetEnvironmentVariableA("EVBOTS_SUPPRESSION",disabled,sizeof(disabled))==1 && disabled[0]=='0')
        return "EVBOTS_SUPPRESSION=0";
    if (!SuppressionCodeMatches(base,g_suppressionHook.IsInstalled())) return "native suppression code mismatch";
    if (set && requested!=100 && !g_suppressionHook.IsInstalled()) {
        HMODULE pinned=nullptr;
        if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(&SuppressEnemies),&pinned))
            return "cannot pin native suppression callback";
        if (g_suppressionHook.Install(reinterpret_cast<void*>(base+0xF38C70),
                                     reinterpret_cast<void*>(&SuppressEnemies),"Rcon:SuppressionMultiplier")!=HookResult::Ok)
            return "native suppression hook installation failed";
    }
    if (set) {
        g_suppressionPercent.store(requested);
        LOG_INFO("RCON native vu.SuppressionMultiplier=%u scale=%.9g hook=%s",requested,
                 static_cast<double>(static_cast<float>(requested)/100.0f),g_suppressionHook.IsInstalled()?"installed":"stock100");
    }
    // VU getter101E6F45: float scale * 100, then CVTTSS2SI (including its
    // signed-indefinite result for an overflowing percentage).
    const float reflected=(static_cast<float>(g_suppressionPercent.load())/100.0f)*100.0f;
    *actual=_mm_cvtt_ss2si(_mm_set_ss(reflected));
    return nullptr;
}

int SuppressionLua(lua_State* L) {
    const bool set=!lua_isnoneornil(L,1);
    const lua_Integer n=set?luaL_checkinteger(L,1):0;
    if(n<0 || static_cast<unsigned long long>(n)>0xFFFFFFFFULL)
        return luaL_argerror(L,1,"expected unsigned 32-bit percentage");
    int value=0;
    const char* error=SuppressionAccess(set,static_cast<unsigned>(n),&value);
    lua_pushboolean(L,error==nullptr);
    if(error) lua_pushstring(L,error); else lua_pushinteger(L,value);
    return 2;
}

bool SquadClientCodeMatches(uintptr_t base) {
    __try {
        const unsigned char stride[]={0x3D,0x80,0,0,0};
        const unsigned char eaxFour[]={0x83,0xF8,4};
        const unsigned char esiFour[]={0x83,0xFE,4};
        return !std::memcmp(reinterpret_cast<void*>(base+0x4637DE),stride,sizeof(stride)) &&
               !std::memcmp(reinterpret_cast<void*>(base+0x51CC7B),eaxFour,sizeof(eaxFour)) &&
               !std::memcmp(reinterpret_cast<void*>(base+0x51D9DA),esiFour,sizeof(esiFour)) &&
               !std::memcmp(reinterpret_cast<void*>(base+0x51DA10),esiFour,sizeof(esiFour));
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SquadServerCodeMatches(uintptr_t base) {
    __try {
        const auto* join=reinterpret_cast<const unsigned char*>(base+0xD8DD91);
        const unsigned char indexed[]={0x8B,0x44,0xC2,4,0x3B,0xC8,0x72,0x58};
        const auto* settings=reinterpret_cast<const unsigned char*>(base+0xD64A44);
        const unsigned char infoLoad[]={0x8B,0x70,0x0C};
        const unsigned char infoStride[]={0x8D,0x14,0x80,0x8D,0x04,0xD6};
        const unsigned char sizes[]={0x8B,0x7E,4,0x8B,0x47,0xFC,0x8D,0x0C,0xC5,0,0,0,0,0x2B,0xC8,0x8D,0x1C,0x8F};
        return join[0]==0x8B && join[1]==0x15 && *reinterpret_cast<const uintptr_t*>(join+2)==base+0x245BB38 &&
               !std::memcmp(join+6,indexed,sizeof(indexed)) && settings[0]==0xA1 &&
               *reinterpret_cast<const uintptr_t*>(settings+1)==base+0x275D7A4 &&
               !std::memcmp(settings+5,infoLoad,sizeof(infoLoad)) &&
               !std::memcmp(settings+20,infoStride,sizeof(infoStride)) &&
               !std::memcmp(reinterpret_cast<void*>(base+0xD64A75),sizes,sizeof(sizes));
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool TeamRowsFour(uintptr_t rows,unsigned count,bool* sawTeam) {
    // Caller is inside the SEH reader. Team zero is the neutral/non-playable row.
    for(unsigned i=1;i<count;++i) {
        const unsigned* entry=reinterpret_cast<const unsigned*>(rows+i*8);
        if(entry[0]==0) continue;
        if(entry[1]!=4) return false;
        *sawTeam=true;
    }
    return true;
}

const char* ReadSquadFour(uintptr_t base,uintptr_t settings) {
    __try {
        const uintptr_t cached=*reinterpret_cast<const uintptr_t*>(base+0x275D7A4);
        if(cached && cached!=settings) return "native GameModeSettings singleton mismatch";
        const uintptr_t info=*reinterpret_cast<const uintptr_t*>(settings+0xC);
        if(info<0x10000) return "GameModeSettings information is not ready";
        const unsigned infoCount=*reinterpret_cast<const unsigned*>(info-4);
        if(!infoCount || infoCount>16) return "invalid GameModeSettings information array";
        bool sawTeam=false;
        for(unsigned i=0;i<infoCount;++i) {
            const uintptr_t sizes=*reinterpret_cast<const uintptr_t*>(info+i*0x28+4);
            if(sizes<0x10000) return "GameModeSettings sizes are not ready";
            const unsigned sizeCount=*reinterpret_cast<const unsigned*>(sizes-4);
            if(!sizeCount || sizeCount>128) return "invalid GameModeSettings sizes array";
            for(unsigned j=0;j<sizeCount;++j) {
                const uintptr_t teams=*reinterpret_cast<const uintptr_t*>(sizes+j*0x1C+0x10);
                if(teams<0x10000) return "GameModeSettings teams are not ready";
                const unsigned count=*reinterpret_cast<const unsigned*>(teams-4);
                if(count>128) return "invalid GameModeSettings team array";
                if(!TeamRowsFour(teams,count,&sawTeam)) return "configured native squad size differs from four";
            }
        }
        if(!sawTeam) return "no playable team settings";
        // If the engine has populated its current-level table, it must agree.
        const uintptr_t begin=*reinterpret_cast<const uintptr_t*>(base+0x245BB38);
        const uintptr_t end=*reinterpret_cast<const uintptr_t*>(base+0x245BB3C);
        if(end<begin || (end-begin)%8 || end-begin>128*8) return "invalid native squad table";
        if(end!=begin && !TeamRowsFour(begin,static_cast<unsigned>((end-begin)/8),&sawTeam))
            return "active native squad size differs from four";
        return nullptr;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return "native squad settings are unreadable"; }
}

const char* SquadFourAccess() {
    if(!EVBotsOnServerFrameThread() && !eng::IsInNativeLevelLoadResources()) return "requires native engine callback";
    const uintptr_t base=reinterpret_cast<uintptr_t>(::GetModuleHandleW(L"bf3_server_Final.exe"));
    if(!base || !eng::Ready()) return "dedicated settings are unavailable";
    if(!SquadServerCodeMatches(base)) return "native squad consumer layout mismatch";
    const unsigned long settings=eng::FindSettings("GameModeSettings");
    if(!settings || !eng::IsKindOfName(eng::TypeOf(settings),"GameModeSettings")) return "GameModeSettings unavailable";
    return ReadSquadFour(base,settings);
}

int SquadFourLua(lua_State* L) {
    const char* error=SquadFourAccess();
    lua_pushboolean(L,error==nullptr);
    if(error) lua_pushstring(L,error); else {
        lua_pushinteger(L,4);
        LOG_INFO("RCON native vu.SquadSize=4 (GameModeSettings and active table readback verified; no-op)");
    }
    return 2;
}

const char kGameplayLua[]=R"LUA(
local native,squadFour=...
RCON:RegisterCommand('vu.SuppressionMultiplier', RemoteCommandFlag.RequiresLogin, function(_,args)
  if #args>1 then return {'InvalidArguments'} end
  local value=nil
  if #args==1 then
    if not args[1]:match('^%d+$') then return {'InvalidArguments'} end
    value=tonumber(args[1])
    if not value or value<0 or value>4294967295 or value%1~=0 then return {'InvalidArguments'} end
  end
  local ok,result=native(value)
  if not ok then return {'ERROR',result} end
  if value==nil then return {'OK',tostring(result)} end
  return {'OK'}
end)
RCON:RegisterCommand('vu.SquadSize', RemoteCommandFlag.RequiresLogin, function(_,args)
  if #args>1 then return {'InvalidArguments'} end
  if #args==1 then
    if not args[1]:match('^%d+$') or tonumber(args[1])==0 then return {'InvalidArguments'} end
    if tonumber(args[1])~=4 then return {'Unsupported','only verified native squad size four is supported'} end
  end
  local ok,result=squadFour()
  if not ok then return {'ERROR',result} end
  if #args==0 then return {'OK',tostring(result)} end
  return {'OK'}
end)
)LUA";
}

bool RegisterNativeRconGameplay(lua_State* L) {
    if(luaL_loadbufferx(L,kGameplayLua,sizeof(kGameplayLua)-1,"=[native-rcon-gameplay]","t")!=LUA_OK) {
        LOG_ERR("RCON gameplay compile: %s",lua_tostring(L,-1));lua_pop(L,1);return false;
    }
    lua_pushcfunction(L,&SuppressionLua);
    lua_pushcfunction(L,&SquadFourLua);
    if(lua_pcall(L,2,0,0)!=LUA_OK) {
        LOG_ERR("RCON gameplay register: %s",lua_tostring(L,-1));lua_pop(L,1);return false;
    }
    return true;
}

bool VerifyNativeSquadSizeClient() {
    const uintptr_t base=reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr));
    const bool ok=base && SquadClientCodeMatches(base);
    LOG_INFO("RCON native client squad size four constants: %s",ok?"PASS":"FAIL");
    return ok;
}
}
