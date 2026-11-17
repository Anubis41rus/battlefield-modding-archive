#pragma once
#include <cstring>

namespace evbots {
inline bool HasCompleteLevelInfo(const char* name, const char* mode) {
    return name != nullptr && std::strncmp(name, "Levels/", 7) == 0 &&
           mode != nullptr && mode[0] != '\0';
}
}
