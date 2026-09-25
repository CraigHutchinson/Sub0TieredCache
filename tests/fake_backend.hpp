#pragma once

/** @file fake_backend.hpp
 *  @brief Deterministic in-memory FillBackend for the M2 state-machine gates (transfer-contract.md
 *         "Portable test gates"). The test decides when, in what order and with what result each
 *         accepted request completes, so every race the contract names can be forced on demand.
 *
 *  Source bytes are a pure function of offset (source_byte). NOTE ON WHAT THIS IS AND ISN'T AN ORACLE
 *  FOR: `source_byte` is a legitimate independent oracle for "did the transport deliver the requested
 *  raw bytes" -- it is a fixed formula, not this project's own code. It is NOT, and must never become,
 *  an oracle for representation conversion: bf16->f32 correctness is checked in
 *  tests/row_cache_tests.cpp against a bit-shift reference written independently of codec.hpp, per the
 *  cross-project plan's "no copied implementation becomes its own correctness oracle" rule.
 *
 *  Copied verbatim (byte-for-byte) from Sub0MemPage's own tests/fake_backend.hpp, which is not installed
 *  or exported by that project (see docs/integration-plan.md's "Contract feedback to Sub0MemPage (T0)"
 *  section -- this copy should be deleted in favour of a real `sub0mempage::testing` dependency once
 *  that lands). Bounded like a real backend: submit() fails once `capacity` requests are pending, and
 *  never allocates (storage is reserved at construction).
 */

#include <sub0mempage/transfer.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace sub0mempage::test {

[[nodiscard]] constexpr std::byte source_byte(std::uint64_t offset) noexcept {
    return static_cast<std::byte>((offset * 131u + (offset >> 9)) & 0xffu);
}

/// How the fake terminates a request.
enum class FakeCompletion : std::uint8_t { full, short_read, io_error, cancelled };

class FakeBackend {
public:
    explicit FakeBackend(std::size_t capacity) : capacity_(capacity) { pending_.reserve(capacity); }

    [[nodiscard]] bool submit(const FillRequest& request) noexcept {
        const std::scoped_lock lock(mutex_);
        if (pending_.size() == capacity_) {
            ++rejected_;
            return false;
        }
        pending_.push_back(request);
        ++accepted_;
        return true;
    }

    [[nodiscard]] std::size_t pending() const {
        const std::scoped_lock lock(mutex_);
        return pending_.size();
    }
    [[nodiscard]] std::uint64_t accepted() const {
        const std::scoped_lock lock(mutex_);
        return accepted_;
    }

    /// Completes the `position`-th pending request (0 = oldest). Delivery happens outside the fake's
    /// lock, as a real completion thread would deliver it. Returns false if nothing is pending there.
    bool complete(std::size_t position, FakeCompletion how = FakeCompletion::full) {
        FillRequest request;
        {
            const std::scoped_lock lock(mutex_);
            if (position >= pending_.size()) {
                return false;
            }
            request = pending_[position];
            pending_.erase(pending_.begin() + static_cast<std::ptrdiff_t>(position));
            last_ = request;
        }
        deliver(request, how);
        return true;
    }

    bool complete_newest(FakeCompletion how = FakeCompletion::full) {
        const std::size_t count = pending();
        return count != 0 && complete(count - 1, how);
    }

    void complete_all_newest_first() {
        while (complete_newest()) {
        }
    }

    /// Re-delivers the most recently completed request's token: the duplicate-completion race.
    void redeliver_last(FakeCompletion how = FakeCompletion::full) {
        std::optional<FillRequest> last;
        {
            const std::scoped_lock lock(mutex_);
            last = last_;
        }
        if (last) {
            deliver(*last, how);
        }
    }

private:
    static void deliver(const FillRequest& request, FakeCompletion how) {
        std::uint64_t written = request.destination.size();
        if (how == FakeCompletion::short_read) {
            written = written / 2;
        } else if (how != FakeCompletion::full) {
            written = 0;
        }
        for (std::uint64_t i = 0; i < written; ++i) {
            request.destination[i] = source_byte(request.source_offset + i);
        }
        const Status status = how == FakeCompletion::io_error    ? Status::io_error
                              : how == FakeCompletion::cancelled ? Status::cancelled
                                                                 : Status::ok;
        request.sink.deliver(request.token, {.status = status, .bytes = written});
    }

    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::vector<FillRequest> pending_;
    std::optional<FillRequest> last_;
    std::uint64_t accepted_ = 0;
    std::uint64_t rejected_ = 0;
};

} // namespace sub0mempage::test
