#include "RoundPlayerCounts.h"
#include "InlineHook.h"
#include "EVBotsLog.h"
#include <windows.h>
#include <cstring>

namespace evbots {
namespace {
struct Site {unsigned long address;unsigned char from[2],to[2];const char* name;};
const Site sites[]={
    {0x0113FE0F,{0x75,0x51},{0xEB,0x51},"roundStartPlayerCount accepts zero"},
    {0x0113FE81,{0x74,0x1E},{0xEB,0x1E},"roundStartPlayerCount preserves requested value"},
    {0x011400E6,{0x74,0x1E},{0xEB,0x1E},"roundRestartPlayerCount preserves requested value"}
};
struct Guard {unsigned long address;unsigned char bytes[16];unsigned size;};
const Guard guards[]={
    {0x0113FE06,{0x83,0xF8,0xFF,0x7E,0x54,0x8B,0xF8,0x85,0xC0,0x75,0x51},11},
    {0x0113FE7A,{0x80,0xB9,0x94,0,0,0,0,0x74,0x1E},9},
    {0x011400DF,{0x80,0xBA,0x94,0,0,0,0,0x74,0x1E},9},
    // Verify the native settings write sites and effective minimum1 getter;
    // explicit configuration0 is distinct from vu.DisablePreRound.
    {0x0113FEB1,{0xA1,0xB4,0xBF,0xB5,2,0x89,0x70,0x3C},8},
    {0x01140115,{0x8B,0x0D,0xB4,0xBF,0xB5,2,0x89,0x71,0x40},9},
    {0x012D76BC,{0x83,0xF8,1,0x7F,5,0xB8,1,0,0,0},10}
};
volatile LONG installed=0;
bool Match(unsigned long address,const void* expected,unsigned size) {
    __try {return std::memcmp(reinterpret_cast<void*>(address),expected,size)==0;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
}
bool InstallRoundPlayerCountCompatibility() {
    if(installed==1)return true;
    if(installed<0)return false;
    char value[4]={};
    if(GetEnvironmentVariableA("EVBOTS_ROUND_PLAYER_COUNTS",value,sizeof(value))==1 && value[0]=='0') {
        LOG_INFO("RoundPlayerCounts: disabled by EVBOTS_ROUND_PLAYER_COUNTS=0");return false;
    }
    if(reinterpret_cast<unsigned long>(GetModuleHandleA("bf3_server_Final.exe"))!=0x400000) {
        LOG_ERR("RoundPlayerCounts: verified dedicated server image is required");return false;
    }
    bool ready=true;
    for(const auto& guard:guards) {
        const bool one=Match(guard.address,guard.bytes,guard.size);ready=ready&&one;
        LOG_INFO("RoundPlayerCounts: signature %08lX %s",guard.address,one?"PASS":"FAIL");
    }
    for(const auto& site:sites) {
        const bool one=Match(site.address,site.from,sizeof(site.from));ready=ready&&one;
        LOG_INFO("RoundPlayerCounts: preflight %08lX %s %s",site.address,site.name,one?"PASS":"FAIL");
    }
    if(!ready) {InterlockedExchange(&installed,-1);LOG_ERR("RoundPlayerCounts: batch mismatch; no bytes written");return false;}
    unsigned applied=0;
    for(const auto& site:sites) {
        if(!PatchBytes(reinterpret_cast<void*>(site.address),site.from,site.to,sizeof(site.to),site.name)) {
            bool rollback=true;
            while(applied) {
                const auto& previous=sites[--applied];
                const bool restored=PatchBytes(reinterpret_cast<void*>(previous.address),previous.to,previous.from,sizeof(previous.from),"RoundPlayerCounts rollback");
                rollback=rollback&&restored;LOG_INFO("RoundPlayerCounts: rollback %08lX %s",previous.address,restored?"PASS":"FAIL");
            }
            InterlockedExchange(&installed,-1);
            LOG_ERR("RoundPlayerCounts: write refused; rollback %s",rollback?"PASS":"FAIL");return false;
        }
        ++applied;LOG_INFO("RoundPlayerCounts: applied %08lX %s",site.address,site.name);
    }
    InterlockedExchange(&installed,1);
    LOG_INFO("RoundPlayerCounts: READY 3/3; explicit RCON zero supported; PreRound getters unchanged");
    return true;
}
}
