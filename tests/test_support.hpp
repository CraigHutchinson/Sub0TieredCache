#pragma once

/** @file test_support.hpp
 *  @brief Minimal shared harness for the offline test executables: check/run bookkeeping only.
 *
 *  No framework on purpose, matching Sub0MemPage's own precedent and this project's own
 *  STYLE_GUIDE.md ("no third-party dependencies"): the tests follow the same rule as the core.
 */

#include <iostream>

namespace sub0tieredcache::test {

inline unsigned g_checks = 0;
inline unsigned g_failures = 0;

inline void check(bool condition, const char* description) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

/// Runs one test function, naming it first so a crash or hang points at the test.
inline void run(void (*test)(), const char* name) {
    std::cerr << "-- " << name << '\n';
    test();
}

/// Prints the tally and returns the process exit code.
[[nodiscard]] inline int finish() {
    std::cout << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}

} // namespace sub0tieredcache::test
