#pragma once

namespace evbots {
constexpr unsigned long kRushDestructBase = 0x324B3FE6UL;
constexpr unsigned long kRushIncreaseReinforce = 0x2CFC2C6CUL;

// ServerLifeCounterEntity::onEvent (012DC140): только принятый DestructBase уменьшает BaseCount.
inline bool RushBaseDestroyedAccepted(unsigned long eventId, bool lifeCounter, int before, int after) {
    return lifeCounter && eventId == kRushDestructBase && before > 0 && after == before - 1;
}
inline bool RushFinalBase(bool accepted, int remaining, bool attacker) {
    return accepted && remaining == 0 && !attacker;
}
}
