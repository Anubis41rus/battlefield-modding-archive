#pragma once
namespace evbots {
// VU-compatible explicit zero RCON settings. Does not disable PreRound or alter
// its effective getters. Install before the first native RCON command/level.
bool InstallRoundPlayerCountCompatibility();
}
