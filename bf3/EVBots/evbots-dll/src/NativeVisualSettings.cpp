#include "NativeVisualSettings.h"
#include "NativeVisualEnvironment.h"
#include "NativeBlueprint.h"
#include "InlineHook.h"
#include "EVBotsLog.h"
#include <windows.h>
#include <cstring>

namespace evbots { namespace visualsettings {
namespace {
using Ptr=std::uintptr_t;
using UpdateFn=void(__fastcall*)(void*,void*,void*);
using RenderFn=int(__fastcall*)(void*,void*,void*,void*,void*);
InlineHook updateHook,renderHook;
UpdateFn originalUpdate=nullptr;
RenderFn originalRender=nullptr;
volatile LONG colorEnabled=1,sunEnabled=1;
bool ready=false;
Ptr imageBase=0;
struct Signature {Ptr address;unsigned char bytes[16];};
const Signature signatures[]={
 {0x01799C20,{0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x81,0xEC,0x94,0x05,0,0,0x53,0x8B,0xD9,0x80}},
 {0x0178BFD0,{0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x81,0xEC,0x64,0x0C,0,0,0x53,0x56,0x57,0x8B}}
};
bool Fail(const char** error,const char* text) {if(error)*error=text;return false;}
Ptr Va(Ptr address) {return imageBase+address-0x400000;}
bool Matches(const Signature& signature) {
 __try {return std::memcmp(reinterpret_cast<const void*>(Va(signature.address)),signature.bytes,16)==0;}
 __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
void __fastcall Update(void* self,void*,void* view) {
 // VU core10113FE0: disabled ret4; enabled tail-calls original01799C20.
 if(InterlockedCompareExchange(&sunEnabled,0,0)) {
  const auto call=originalUpdate?originalUpdate:reinterpret_cast<UpdateFn>(updateHook.Trampoline());
  call(self,nullptr,view);
 }
}
int __fastcall Render(void* self,void*,void* view,void* arg2,void* arg3) {
 // VU core10114000: disabled eax0/retC; enabled original0178BFD0.
 if(!InterlockedCompareExchange(&sunEnabled,0,0))return 0;
 const auto call=originalRender?originalRender:reinterpret_cast<RenderFn>(renderHook.Trampoline());
 return call(self,nullptr,view,arg2,arg3);
}
bool Apply(const visual::State& state,bool enabled,const char** error) {
 if(state.name=="EffectEntity")return true;
 const auto* type=visual::FindComponent("colorCorrection");
 bool present=false;
 if(!visual::HasComponent(state.handle,type,&present,error))return false;
 if(!present)return true;
 visual::Value value;value.integer=enabled?1:0;
 return visual::WriteComponent(state.handle,type,visual::FindField(type,"enable"),value,error) &&
        visual::WriteComponent(state.handle,type,visual::FindField(type,"colorGradingEnable"),value,error);
}
// This entry is used only from a proven native init hook, which can run before
// the Lua frame marker. No public/Lua caller is allowed to supply a raw pointer.
bool ApplyNewState(Ptr state) {
 Ptr manager=0;bool locked=false,ok=false;
 __try {
  manager=*reinterpret_cast<const Ptr*>(Va(0x023B6844));
  if(manager) {
   EnterCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(manager));locked=true;
   const Ptr component=*reinterpret_cast<const Ptr*>(state+0x2C);
   if(component) {
    // core101F35C3..35E6; these are runtime flags, not ComponentData fields.
    *reinterpret_cast<unsigned char*>(component+0x3D)=0;
    *reinterpret_cast<unsigned char*>(component+0x3C)=0;
   }
   *reinterpret_cast<unsigned char*>(manager+0x18C0)=1;ok=true;
  }
 } __except(EXCEPTION_EXECUTE_HANDLER) {ok=false;}
 if(locked)LeaveCriticalSection(reinterpret_cast<CRITICAL_SECTION*>(manager));
 return ok;
}
}
bool ColorCorrectionEnabled() {return InterlockedCompareExchange(&colorEnabled,0,0)!=0;}
bool SunFlareEnabled() {return InterlockedCompareExchange(&sunEnabled,0,0)!=0;}
bool Initialize(const char** error) {
 if(error)*error=nullptr;
 if(ready)return true;
 char flag[4]={};
 if(GetEnvironmentVariableA("EVBOTS_VISUAL_SETTINGS",flag,sizeof(flag))==1 && flag[0]=='0')
  return Fail(error,"EVBOTS_VISUAL_SETTINGS=0");
 if(!blueprint::IsClient())return Fail(error,"visual settings native renderer requires client");
 imageBase=reinterpret_cast<Ptr>(GetModuleHandleA("bf3.exe"));
 if(!imageBase)return Fail(error,"visual settings native client image is unavailable");
 bool valid=true;
 for(const auto& signature:signatures) {
  const bool one=Matches(signature);valid=valid&&one;
  LOG_INFO("VisualSettings: signature %08X %s",static_cast<unsigned>(signature.address),one?"PASS":"FAIL");
 }
 if(!valid)return Fail(error,"visual setting renderer signature batch failed; no hooks installed");
 // Reuses the existing manager/state proof and early lifetime hooks.
 if(!visual::Initialize(error))return false;
 if(updateHook.Install(reinterpret_cast<void*>(Va(signatures[0].address)),reinterpret_cast<void*>(&Update),"visual.sunFlareUpdate")!=HookResult::Ok)
  return Fail(error,"sun flare update hook could not be installed");
 originalUpdate=reinterpret_cast<UpdateFn>(updateHook.Trampoline());
 if(renderHook.Install(reinterpret_cast<void*>(Va(signatures[1].address)),reinterpret_cast<void*>(&Render),"visual.sunFlareRender")!=HookResult::Ok) {
  const bool removed=updateHook.Remove();
  if(removed)originalUpdate=nullptr;
  return Fail(error,removed?"sun flare render hook failed; update hook rolled back":"sun flare render hook failed; update rollback failed");
 }
 originalRender=reinterpret_cast<RenderFn>(renderHook.Trampoline());
 ready=true;return true;
}
bool SetColorCorrectionEnabled(bool enabled,const char** error) {
 if(error)*error=nullptr;
 if(!ready)return Fail(error,"visual settings native hooks are not installed");
 if(!blueprint::EngineFrameActive())return Fail(error,"visual setting mutation requires client engine frame");
 // Like VU core101F3840, keep desired value even if there is no loaded manager.
 InterlockedExchange(&colorEnabled,enabled?1:0);
 std::vector<visual::State> states;
 if(!visual::GetStates(&states,error))return false;
 for(const auto& state:states)if(!Apply(state,enabled,error))return false;
 return visual::SetDirty(true,error);
}
bool SetSunFlareEnabled(bool enabled,const char** error) {
 if(error)*error=nullptr;
 if(!ready)return Fail(error,"visual settings native hooks are not installed");
 InterlockedExchange(&sunEnabled,enabled?1:0);return true;
}
void OnStateCreated(Ptr state,const char* name) {
 if(!ready || ColorCorrectionEnabled() || !state || !name || !std::strcmp(name,"EffectEntity"))return;
 if(!ApplyNewState(state))LOG_ERR("VisualSettings: failed to apply color correction policy to new native state");
}
} }
