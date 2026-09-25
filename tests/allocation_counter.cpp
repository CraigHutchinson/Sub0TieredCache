// Replaces global operator new/delete so tests can measure, not assume, that hot paths never allocate.
// Copied in shape from Sub0MemPage's own tests/allocation_counter.cpp (STYLE_GUIDE.md: mirror its test
// harness style; there is no shared test-support target yet -- see the "Contract feedback to Sub0MemPage
// (T0)" section of docs/integration-plan.md).

#include "test_support.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

// GCC cannot see that the replacements below are a matched malloc/free pair once inlined.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

namespace {
std::atomic<std::uint64_t> g_allocations{0};
}

std::uint64_t sub0tieredcache::test::allocation_count() noexcept { return g_allocations.load(); }

void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size)) {
        return p;
    }
    throw std::bad_alloc();
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
