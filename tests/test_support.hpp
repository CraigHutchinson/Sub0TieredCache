#pragma once

/** @file test_support.hpp
 *  @brief Minimal shared harness for the offline test executables: check/run bookkeeping and the global
 *         allocation counter behind the "zero hot-path allocations" gate. Copied in shape (not in
 *         wording) from Sub0MemPage's own tests/test_support.hpp -- mirroring its test harness style is
 *         explicit guidance for this project (AGENTS.md/STYLE_GUIDE.md), and there is no shared
 *         test-support target to depend on instead (see docs/integration-plan.md's "Contract feedback to
 *         Sub0MemPage (T0)" section).
 *
 *  No framework on purpose: the core has no third-party dependencies (STYLE_GUIDE.md) and the tests
 *  follow the same rule. allocation_counter.cpp replaces global operator new in every test executable.
 */

#include "fake_backend.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>

namespace sub0tieredcache::test {

/// Total global operator-new calls so far in this process (all threads).
[[nodiscard]] std::uint64_t allocation_count() noexcept;

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

/** @brief Drives a sub0mempage::test::FakeBackend from a background thread so a test's foreground
 *  thread can call a blocking Table operation (resolve_into/wait) without deadlocking against a
 *  transport that only completes when told to. Row order across distinct in-flight fetches is
 *  intentionally not controlled by this helper -- tests that need a specific completion order or a
 *  specific failure mode drive the FakeBackend by hand instead (see e.g. the codec-failure tests).
 */
class BackgroundCompleter {
public:
    explicit BackgroundCompleter(sub0mempage::test::FakeBackend& backend)
        : backend_(backend), thread_([this] { pump(); }) {}
    BackgroundCompleter(const BackgroundCompleter&) = delete;
    BackgroundCompleter& operator=(const BackgroundCompleter&) = delete;
    ~BackgroundCompleter() {
        stop_.store(true, std::memory_order_relaxed);
        thread_.join();
    }

private:
    void pump() {
        while (!stop_.load(std::memory_order_relaxed)) {
            backend_.complete_all_newest_first();
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
        backend_.complete_all_newest_first(); // drain anything submitted just before stop()
    }

    sub0mempage::test::FakeBackend& backend_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace sub0tieredcache::test
