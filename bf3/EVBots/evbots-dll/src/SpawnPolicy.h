#pragma once
#include <cstddef>
#include <cstring>

namespace evbots {
inline bool IsRushMode(const char* mode) {
    return mode != nullptr && (std::strcmp(mode, "RushLarge0") == 0 || std::strcmp(mode, "SquadRush0") == 0);
}
// SpawnSoldierAt в Rush не заменяется произвольным спавнером при отказе создания на выбранной модом базе.
inline bool RequireExactRushSpawn(const char* mode, bool hasPosition, bool explicitPoint, bool vehicle) {
    return IsRushMode(mode) && hasPosition && !explicitPoint && !vehicle;
}
// В танковом превосходстве захват единственного флага не открывает пехотный спавн на нём.
// Явная точка движка и прямая посадка в технику сохраняют свой путь.
inline bool RequireBaseSpawn(const char* mode, bool hasPosition, bool explicitPoint, bool vehicle) {
    return mode != nullptr && std::strcmp(mode, "TankSuperiority0") == 0 &&
           hasPosition && !explicitPoint && !vehicle;
}

struct BaseCapturePoint {
    unsigned long bus;
    unsigned int team;
    unsigned int capturableType;
};
struct BaseCharacterSpawn {
    unsigned long ptr;
    unsigned long bus;
    unsigned int team;
    bool vehicle;
};
// HQ определяется типом и командой флага, а его спавнер — общей шиной.
// Адрес последнего спавна человека и порядок выделения памяти здесь не участвуют.
inline unsigned long SelectTeamBaseSpawn(unsigned int team, const BaseCapturePoint* points, std::size_t pointCount,
                                        const BaseCharacterSpawn* spawns, std::size_t spawnCount) {
    if (team == 0) return 0;
    for (std::size_t p = 0; p < pointCount; ++p) {
        if (points[p].capturableType != 1 || points[p].team != team || points[p].bus == 0) continue;
        for (std::size_t s = 0; s < spawnCount; ++s) {
            const auto& spawn = spawns[s];
            if (spawn.ptr != 0 && spawn.bus == points[p].bus && !spawn.vehicle &&
                (spawn.team == 0 || spawn.team == team)) return spawn.ptr;
        }
    }
    return 0;
}
}
