#pragma once

/** @file mirror_backend.hpp
 *  @brief T3 integration: a sub0mempage FillBackend adapter over one or more registered
 *         `remote::Mirror` instances, so a Table can be sharded over remote sources exactly the same
 *         way it is sharded over local files (local_file_source.hpp) or a deterministic fake.
 *
 *  Structurally a twin of sub0mempage::LocalFileBackend (docs/integration-plan.md's T3 row): a bounded,
 *  preallocated ring-buffer queue plus a small worker-thread pool. `submit()` follows the exact same
 *  backend contract every Sub0MemPage FillBackend must (transfer.hpp's FillBackendRef comment): never
 *  blocks on I/O, never allocates, returns false only when the bounded queue is full or shutdown() has
 *  been called, and every accepted request gets exactly one terminal delivery, made from a worker
 *  thread, never inline in submit(). Administrative (may block/allocate; never on the hot path):
 *  create(), register_mirror(), shutdown() and the destructor.
 *
 *  Ownership: a registered `remote::Mirror` is caller-owned and must outlive both this backend and every
 *  request it has ever accepted for that source -- exactly LocalFileBackend's own contract for a
 *  registered file handle, not a new rule invented here.
 *
 *  Status mapping (task brief: "Map remote::Status to sub0mempage::Status (io_error/short_read/
 *  cancelled)"): remote::Status::ok delivers ok with the actual byte count; remote::Status::truncated
 *  (the connection closed before the promised body length arrived) delivers ok with the actual
 *  (necessarily shorter) byte count too -- transfer.hpp's own detail::classify_fill is the single place
 *  that turns "ok but bytes < requested" into Status::short_read, matching LocalFileBackend's EOF-clipped
 *  read convention exactly, so a genuinely short remote body is reported the same way a genuinely short
 *  local read is. Every other remote::Status (connect_failed, timeout, server_error, a validator
 *  mismatch, ...) delivers Status::io_error with zero bytes -- Mirror aggregates a multi-chunk read
 *  internally, so bytes already copied into `dest` before a later chunk failed are never published (R14):
 *  reporting a genuine failure as io_error, not as a misleadingly small "successful" read, is what makes
 *  transfer.hpp discard it outright rather than trying to interpret a partial count. shutdown()'s own
 *  cancellation of still-queued requests delivers Status::cancelled, matching LocalFileBackend.
 */

#include "sub0tieredcache/remote/mirror.hpp"

#include <sub0mempage/transfer.hpp>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace sub0tieredcache::remote {

/// Bounded worker-pool sizing, matching sub0mempage::LocalFileBackendConfig's own shape (R9/AGENTS.md
/// sec 1: `queue_capacity`/`max_sources` are preallocated bounds, not hints).
struct MirrorBackendConfig {
    std::uint32_t workers = 1;
    std::uint32_t queue_capacity = 0;
    std::uint32_t max_sources = 0;
};

/// Observability (AGENTS.md sec 9), matching sub0mempage::LocalFileBackendStats's own field shape plus
/// the remote-specific counts Mirror itself does not already expose per-request.
struct MirrorBackendStats {
    std::uint64_t accepted = 0;
    std::uint64_t rejected_queue_full = 0;
    std::uint64_t completed_ok = 0;
    std::uint64_t short_reads = 0;    ///< remote::Status::truncated, downstream-classified as short_read.
    std::uint64_t errors = 0;        ///< Any other non-ok remote::Status, or an unknown SourceId.
    std::uint64_t cancelled = 0;     ///< shutdown()'s cancellation of still-queued requests.
    std::uint64_t bytes_read = 0;
    std::uint32_t queue_high_water = 0;
    std::uint32_t in_flight = 0;
};

/** @brief Portable worker-pool FillBackend over registered remote::Mirror instances. See the file
 *  comment for scope, the status mapping and shutdown semantics.
 *
 *  Non-movable and non-copyable (worker threads and every WorkItem's CompletionSink close over `this`
 *  indirectly through this object's own address), hence create() returns a unique_ptr, matching
 *  LocalFileBackend/SlotPool/TransferSet's own shape.
 */
class MirrorBackend {
    struct Passkey {};

public:
    [[nodiscard]] static std::expected<std::unique_ptr<MirrorBackend>, sub0mempage::Status>
    create(const MirrorBackendConfig& config);

    MirrorBackend(Passkey, const MirrorBackendConfig& config);
    MirrorBackend(const MirrorBackend&) = delete;
    MirrorBackend& operator=(const MirrorBackend&) = delete;
    ~MirrorBackend();

    /** @brief Administrative: records `mirror` under `source`. `mirror` is caller-owned and must outlive
     *  this backend and every request submitted against `source`. Not safe to call concurrently with
     *  another register_mirror() naming the same `source`, nor with a submit() naming a `source` still
     *  mid-registration -- register every source before handing FillBackendRef(*this) to a pool/transfer
     *  set, exactly as sub0mempage::LocalFileBackend::register_file documents for itself.
     *  @return ok; invalid_argument if `source` is already registered; ticket_exhausted if the
     *          registration table (sized by `max_sources`) is full.
     */
    [[nodiscard]] sub0mempage::Status register_mirror(sub0mempage::SourceId source, Mirror& mirror);

    /// Backend contract (sub0mempage transfer.hpp FillBackendRef): see the file comment. `false` means
    /// only "the bounded queue is full or shutdown() has been called"; an unknown SourceId is still
    /// accepted (delivered as an error from a worker, never inline -- matching LocalFileBackend).
    [[nodiscard]] bool submit(const sub0mempage::FillRequest& request) noexcept;

    /// Administrative: mirrors LocalFileBackend::shutdown's semantics exactly. Idempotent.
    void shutdown();

    [[nodiscard]] MirrorBackendStats stats() const noexcept;

private:
    struct SourceEntry {
        sub0mempage::SourceId source{};
        Mirror* mirror = nullptr; // non-owning
    };

    struct WorkItem {
        sub0mempage::FillRequest request;
        Mirror* mirror = nullptr;
        bool source_known = false;
    };

    void worker_loop();
    void perform(WorkItem& item);
    [[nodiscard]] Mirror* find_source_locked(sub0mempage::SourceId source) const noexcept;

    std::vector<SourceEntry> sources_; ///< Reserved to max_sources at construction; append-only.
    std::vector<WorkItem> ring_;       ///< Fixed-size ring buffer, sized to queue_capacity.
    std::size_t head_ = 0;
    std::size_t ring_size_ = 0;
    bool shutting_down_ = false;
    std::vector<std::thread> workers_;
    MirrorBackendStats stats_;

    mutable std::mutex mutex_;
    std::condition_variable cv_; ///< Signalled on submit() and on shutdown(); workers wait on it.
};

// ---------------------------------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------------------------------

inline std::expected<std::unique_ptr<MirrorBackend>, sub0mempage::Status>
MirrorBackend::create(const MirrorBackendConfig& config) {
    if (config.workers == 0 || config.queue_capacity == 0 || config.max_sources == 0) {
        return std::unexpected(sub0mempage::Status::invalid_argument);
    }
    return std::make_unique<MirrorBackend>(Passkey{}, config);
}

inline MirrorBackend::MirrorBackend(Passkey, const MirrorBackendConfig& config) : ring_(config.queue_capacity) {
    sources_.reserve(config.max_sources);
    workers_.reserve(config.workers);
    // Threads start only after every member above is fully constructed and sized -- matches
    // LocalFileBackend's own constructor-ordering comment for why this is safe.
    for (std::uint32_t i = 0; i < config.workers; ++i) {
        workers_.emplace_back(&MirrorBackend::worker_loop, this);
    }
}

inline MirrorBackend::~MirrorBackend() {
    shutdown(); // idempotent; guarantees no worker still writes a destination (see file comment).
}

inline sub0mempage::Status MirrorBackend::register_mirror(sub0mempage::SourceId source, Mirror& mirror) {
    const std::scoped_lock lock(mutex_);
    for (const SourceEntry& entry : sources_) {
        if (entry.source == source) {
            return sub0mempage::Status::invalid_argument; // duplicate registration
        }
    }
    if (sources_.size() == sources_.capacity()) {
        // Reusing ticket_exhausted for "this bounded administrative table is full", matching
        // LocalFileBackend::register_file's own precedent (no dedicated status exists).
        return sub0mempage::Status::ticket_exhausted;
    }
    sources_.push_back({.source = source, .mirror = &mirror});
    return sub0mempage::Status::ok;
}

inline Mirror* MirrorBackend::find_source_locked(sub0mempage::SourceId source) const noexcept {
    for (const SourceEntry& entry : sources_) {
        if (entry.source == source) {
            return entry.mirror;
        }
    }
    return nullptr;
}

inline bool MirrorBackend::submit(const sub0mempage::FillRequest& request) noexcept {
    const std::scoped_lock lock(mutex_);
    if (shutting_down_ || ring_size_ == ring_.size()) {
        ++stats_.rejected_queue_full;
        return false;
    }
    WorkItem& item = ring_[(head_ + ring_size_) % ring_.size()];
    item.request = request;
    item.mirror = find_source_locked(request.source);
    item.source_known = item.mirror != nullptr;
    ++ring_size_;
    ++stats_.accepted;
    stats_.queue_high_water = std::max(stats_.queue_high_water, static_cast<std::uint32_t>(ring_size_));
    cv_.notify_one();
    return true;
}

inline void MirrorBackend::worker_loop() {
    for (;;) {
        WorkItem item;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return ring_size_ != 0 || shutting_down_; });
            if (ring_size_ == 0) {
                return; // shutting_down_ and the queue is drained: nothing left to read
            }
            item = std::move(ring_[head_]);
            head_ = (head_ + 1) % ring_.size();
            --ring_size_;
            ++stats_.in_flight; // still under the lock: visible to stats() the instant this item starts
        }
        perform(item);
    }
}

inline void MirrorBackend::perform(WorkItem& item) {
    if (!item.source_known) {
        // Unknown SourceId, decided at submit() time; see LocalFileBackend's identical precedent. Never
        // delivered inline in submit().
        item.request.sink.deliver(item.request.token, {.status = sub0mempage::Status::invalid_argument, .bytes = 0});
        const std::scoped_lock lock(mutex_);
        ++stats_.errors;
        --stats_.in_flight;
        return;
    }

    // Mirror::read is blocking and thread-safe (its own internal locking coalesces concurrent fetches of
    // the same chunk); calling it from a worker thread, never from submit() itself, is exactly the same
    // "blocking reads on worker threads" shape LocalFileBackend accepts for its own M3 baseline.
    const Mirror::ReadOutcome outcome =
        item.mirror->read(item.request.source_offset, item.request.destination.size(), item.request.destination);

    sub0mempage::Status status = sub0mempage::Status::io_error;
    std::uint64_t bytes = 0;
    if (outcome.status == Status::ok) {
        status = sub0mempage::Status::ok;
        bytes = outcome.bytes;
    } else if (outcome.status == Status::truncated) {
        // A genuinely short remote body: report ok with the actual (shorter) byte count and let
        // transfer.hpp's own detail::classify_fill turn "ok but bytes < requested" into short_read --
        // the same convention LocalFileBackend uses for an EOF-clipped local read (see file comment).
        status = sub0mempage::Status::ok;
        bytes = outcome.bytes;
    } else {
        // Any other failure (connect/timeout/server error, a validator mismatch, retries exhausted, a
        // caller-shape mistake): Mirror never publishes a partial chunk on these, so neither do we --
        // report a real failure, not a misleadingly small "successful" read (R14).
        status = sub0mempage::Status::io_error;
        bytes = 0;
    }

    item.request.sink.deliver(item.request.token, {.status = status, .bytes = bytes});

    const std::scoped_lock lock(mutex_);
    stats_.bytes_read += bytes;
    --stats_.in_flight;
    if (status != sub0mempage::Status::ok) {
        ++stats_.errors;
    } else if (bytes == item.request.destination.size()) {
        ++stats_.completed_ok;
    } else {
        ++stats_.short_reads;
    }
}

inline void MirrorBackend::shutdown() {
    std::vector<WorkItem> queued;
    {
        const std::scoped_lock lock(mutex_);
        if (shutting_down_) {
            return; // idempotent: already drained (or draining) by an earlier call
        }
        shutting_down_ = true;
        queued.reserve(ring_size_);
        while (ring_size_ != 0) {
            queued.push_back(std::move(ring_[head_]));
            head_ = (head_ + 1) % ring_.size();
            --ring_size_;
        }
        cv_.notify_all(); // wake every worker so idle ones observe shutting_down_ and exit
    }
    // Delivered outside the lock, like every other completion (CompletionSink::deliver may run
    // arbitrary pool/transfer-set code that takes its own lock). Each of these requests was accepted by
    // submit() and had not yet reached a worker, so this is its one and only terminal delivery.
    for (WorkItem& item : queued) {
        item.request.sink.deliver(item.request.token, {.status = sub0mempage::Status::cancelled, .bytes = 0});
        const std::scoped_lock lock(mutex_);
        ++stats_.cancelled;
    }
    // Any request a worker had already dequeued finishes perform() normally (its own single delivery)
    // and the worker then exits its loop once the drained queue is empty; join() waits for exactly that.
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
}

inline MirrorBackendStats MirrorBackend::stats() const noexcept {
    const std::scoped_lock lock(mutex_);
    return stats_;
}

} // namespace sub0tieredcache::remote
