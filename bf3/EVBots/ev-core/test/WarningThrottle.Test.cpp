#include "../src/WarningThrottle.h"
#include <cstdio>

int main() {
    WarningThrottle throttle;
    auto result = throttle.Observe(0);
    if (!result.show || result.suppressed != 0) return 1;
    for (unsigned i = 1; i <= 12000; ++i)
        if (throttle.Observe(i).show) return 2;
    result = throttle.Observe(29999);
    if (result.show) return 3;
    result = throttle.Observe(30000);
    if (!result.show || result.suppressed != 12001) return 4;
    result = throttle.Observe(60000);
    if (!result.show || result.suppressed != 0) return 5;
    if (throttle.Observe(60000).show) return 6;
    result = throttle.Observe(3600000);
    if (!result.show || result.suppressed != 1) return 7;
    WarningThrottle other;
    if (!other.Observe(3600000).show) return 8;
    std::puts("WarningThrottle PASS: first, burst, boundary, reset, idle, independent streams");
    return 0;
}
