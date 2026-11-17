// VextConfig.cpp — Config, SpawnMethod, TeamSwitchModes из fun-bots.
//
// Таблицы соответствуют файлам:
//   ext/shared/Config.lua
//   ext/shared/Constants/SpawnMethods.lua
//   ext/shared/Constants/TeamSwitchModes.lua
//
#include "VextConfig.h"

#include <string>

#include "EVBotsLog.h"

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

namespace evbots {
namespace {

constexpr char kVextConfigSource[] = R"LUA(
-- ═══════════════════════════════════════════════════════════════════════════
-- SpawnMethod — enum из ext/shared/Constants/SpawnMethods.lua
-- ═══════════════════════════════════════════════════════════════════════════
_G.SpawnMethod = {
    SpawnSoldierAt = 0,
    Spawn          = 1,
    SpawnOnTdm     = 2,
}

-- ═══════════════════════════════════════════════════════════════════════════
-- TeamSwitchModes — enum из ext/shared/Constants/TeamSwitchModes.lua
-- ═══════════════════════════════════════════════════════════════════════════
_G.TeamSwitchModes = {
    NoTeamSwitch       = 0,
    SwitchForRoundTwo  = 1,
    AlwaysSwitchTeams  = 2,
    SwitchTeamsRandomly = 3,
}

-- ═══════════════════════════════════════════════════════════════════════════
-- Config — ext/shared/Config.lua (значения по умолчанию)
-- ═══════════════════════════════════════════════════════════════════════════
_G.Config = {

    -- GENERAL
    BotWeapon     = 0,   -- BotWeapons.Auto (enum elsewhere)
    BotKit        = 0,   -- BotKits.RANDOM_KIT
    BotColor      = 0,   -- BotColors.RANDOM_COLOR
    BotTeamNames  = false,

    -- DIFFICULTY
    BotAimWorsening       = 0.2,
    BotSniperAimWorsening = 0.1,
    BotSupportAimWorsening = 0.3,
    BotWorseningSkill     = 0.40,
    BotSniperWorseningSkill = 0.30,
    ReactionTime          = 0.2,
    DamageFactorAssault   = 0.8,
    DamageFactorCarabine  = 0.8,
    DamageFactorLMG       = 0.8,
    DamageFactorPDW       = 0.8,
    DamageFactorSniper    = 0.8,
    DamageFactorShotgun   = 0.8,
    DamageFactorPistol    = 0.8,
    DamageFactorKnife     = 1.5,
    VehicleAimWorsening        = 0.05,
    VehicleAAAimWorsening      = 0.9,
    VehicleGunshipAimWorsening = 0.8,
    VehicleChopperAimWorsening = 0.5,
    VehiclePlaneAimWorsening   = 0.02,
    DamageFactorVehicles   = 1.0,

    -- SPAWN
    SpawnMode                = 0,  -- SpawnModes.balanced_teams
    BalancePlayersIgnoringBots = false,
    TeamSwitchMode           = 1,  -- TeamSwitchModes.SwitchForRoundTwo
    SpawnInBothTeams         = true,
    InitNumberOfBots         = 10,
    NewBotsPerNewPlayer      = 1.6,
    FactorPlayerTeamCount    = 1.0,
    BotTeam                  = 0,  -- TeamId.TeamNeutral
    MaxAssaultBots          = -1,
    MaxEngineerBots          = -1,
    MaxSupportBots           = -1,
    MaxReconBots             = -1,
    AdditionalBotSpawnDelay  = 0.5,
    BotMaxHealth             = 100.0,
    KeepVehicleSeatForPlayer = true,

    -- SPAWNLIMITS
    MaxBotsPerTeamDefault = 32,
    MaxBotsPerTeamTdm     = 32,
    MaxBotsPerTeamTdmc    = 8,
    MaxBotsPerTeamSdm     = 5,
    MaxBotsPerTeamCl      = 32,
    MaxBotsPerTeamCs      = 16,
    MaxBotsPerTeamCal     = 32,
    MaxBotsPerTeamCas     = 16,
    MaxBotsPerTeamRl      = 24,
    MaxBotsPerTeamCtf     = 24,
    MaxBotsPerTeamD       = 12,
    MaxBotsPerTeamGm      = 12,
    MaxBotsPerTeamS       = 12,

    -- BEHAVIOUR
    FovForShooting             = 160,
    FovVerticleForShooting     = 90,
    MaxShootDistance           = 70,
    MaxShootDistanceSniper     = 150,
    MaxShootDistanceMissileAir = 350,
    MaxDistanceShootBack       = 150,
    MaxDistanceShootBackSniper = 400,
    MaxShootDistancePistol     = 20,
    BotAttackMode              = 0,  -- BotAttackModes.RandomNotSet
    ShootBackIfHit             = true,
    BotsAttackBots             = true,
    BotsAttackPlayers          = true,
    MeleeAttackIfClose         = true,
    BotCanKillHimself         = false,
    TeleportIfStuck           = true,
    BotsRevive                 = true,
    BotsReviveBots             = true,
    BotsThrowGrenades          = true,
    BotsDeploy                 = true,
    DeployCycle                = 90,
    SnipersAttackChoppers     = false,

    -- VEHICLE
    UseVehicles                  = true,
    UseAirVehicles               = true,
    UseJets                      = true,
    MaxBotsPerVehicle            = 3,
    FovVehicleForShooting       = 180,
    FovVerticleVehicleForShooting = 60,
    FovVerticleChopperForShooting = 80,
    FovVehicleAAForShooting      = 360,
    FovVerticleVehicleAAForShooting = 160,
    MaxShootDistanceGunship      = 1000,
    MaxShootDistanceVehicles     = 375,
    MaxShootDistanceNoAntiAir    = 175,
    VehicleWaitForPassengersTime = 7.0,
    ChopperDriversAttack         = false,
    AABots                       = false,
    MaxDistanceAABots            = 375,
    EnableParadrop               = false,
    SpawnInMobileRespawnVehicles = true,

    -- WEAPONS
    UseRandomWeapon    = true,
    AssaultWeaponSet  = 0,  -- WeaponSets.Custom
    EngineerWeaponSet  = 0,
    SupportWeaponSet   = 0,
    ReconWeaponSet     = 0,
    AssaultWeapon      = "M416",
    EngineerWeapon     = "M4A1",
    SupportWeapon      = "M249",
    ReconWeapon        = "L96",
    Pistol             = "MP412Rex",
    Knife              = "Razor",

    -- TRACE
    DebugTracePaths  = false,
    WaypointRange    = 100,
    DrawWaypointLines = true,
    LineRange        = 50,
    DrawWaypointIDs  = true,
    TextRange        = 15,
    DrawSpawnPoints  = false,
    SpawnPointRange  = 150,
    TraceDelta       = 0.35,
    NodesPerCycle    = 2048,

    -- ADVANCED
    DistanceForDirectAttack   = 6,
    MeleeAttackCoolDown       = 3.5,
    AimForHead               = false,
    AimForHeadSniper         = false,
    AimForHeadSupport        = false,
    JumpWhileShooting        = true,
    JumpWhileMoving          = true,
    OverWriteBotSpeedMode    = 0,  -- BotMoveSpeeds.NoMovement
    OverWriteBotAttackMode   = 0,
    SpeedFactor              = 1.0,
    SpeedFactorAttack       = 0.6,
    SpeedFactorVehicleAttack = 0.6,
    MoveSidewards            = true,
    MaxStraigtCycle          = 10.0,
    MaxSideCycle            = 5.0,
    MinMoveCycle            = 0.3,
    DefendObjectives        = true,

    -- EXPERT
    BotFirstShotDelay          = 0.15,
    BotMinTimeShootAtPlayer    = 0.7,
    BotVehicleMinTimeShootAtPlayer = 4.0,
    BotFireModeDuration        = 4.5,
    BotVehicleFireModeDuration = 9.0,
    MaximunYawPerSec           = 550,
    TargetDistanceWayPoint     = 0.8,
    TargetDistanceWayPointAirVehicles = 16.0,
    KeepOneSlotForPlayers      = true,
    DistanceToSpawnBots        = 30,
    HeightDistanceToSpawn      = 2.8,
    DistanceToSpawnReduction  = 8,
    MaxTrysToSpawnAtDistance   = 3,
    AttackWayBots              = true,
    RespawnWayBots             = true,
    SpawnMethod                = 2,  -- SpawnMethod.SpawnOnTdm

    -- OTHER
    DisableUserInterface = false,
    AllowCommForAll      = true,
    DisableChatCommands = false,
    DisableRCONCommands  = false,
    IgnorePermissions    = false,
    Language             = nil,
}

return { Config = _G.Config, SpawnMethod = _G.SpawnMethod, TeamSwitchModes = _G.TeamSwitchModes }
)LUA";

bool RegisterVextConfigImpl(lua_State* L) {
    const int kSourceLen = static_cast<int>(sizeof(kVextConfigSource) - 1);
    if (luaL_loadbufferx(L, kVextConfigSource, kSourceLen,
                         "=[vextconfig]", "t") != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: VextConfig не собрался: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        const char* err = lua_tostring(L, -1);
        LOG_ERR("EVBots: VextConfig не выполнился: %s",
                err != nullptr ? err : "?");
        lua_pop(L, 1);
        return false;
    }
    if (!lua_istable(L, -1)) {
        LOG_ERR("EVBots: VextConfig вернул не таблицу");
        lua_pop(L, 1);
        return false;
    }

    // Таблица содержит три ключа: Config, SpawnMethod, TeamSwitchModes.
    // Кладём каждую в _G (глобал). Прямой доступ по относительному -1
    // (без lua_next; результат pcall — на вершине стека, внизу может
    // лежать мусор от предыдущих регистраторов).
    static const char* const kNames[] = {"Config", "SpawnMethod", "TeamSwitchModes"};
    for (size_t i = 0; i < 3; ++i) {
        lua_getfield(L, -1, kNames[i]);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            continue;
        }
        lua_setglobal(L, kNames[i]);
    }
    lua_pop(L, 1);  // убрать таблицу результатов

    LOG_INFO("EVBots: Config/SpawnMethod/TeamSwitchModes подключены");
    return true;
}

}  // namespace
}  // namespace evbots

bool evbots::RegisterVextConfig(lua_State* L) {
    return evbots::RegisterVextConfigImpl(L);
}
