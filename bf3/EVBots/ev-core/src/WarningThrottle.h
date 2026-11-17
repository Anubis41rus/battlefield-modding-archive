#pragma once
#include <cstdint>

// Called under the console filter's lock. The original file logger is unaffected.
struct WarningThrottle {
    struct Result { bool show; std::uint64_t suppressed; };
    bool started = false;
    std::uint64_t lastShown = 0;
    std::uint64_t skipped = 0;

    Result Observe(std::uint64_t now) {
        if (!started || now - lastShown >= 30000) {
            const auto count = skipped;
            started = true;
            lastShown = now;
            skipped = 0;
            return {true, count};
        }
        ++skipped;
        return {false, 0};
    }
};
