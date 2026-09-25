#pragma once

/** @file row_cache.hpp
 *  @brief T0: a bounded in-memory row cache over Sub0MemPage's M2 explicit-destination transport.
 *
 *  STATUS: T0 (docs/integration-plan.md's delivery table). Implements the acceptance row for T0:
 *  duplicate/order/bounds, budgets, RowLease lifetime, generations, codec failure -- host domain only,
 *  no GPU, no network, no disk tier, no compute engine. Real local-file transport is T1
 *  (docs/integration-plan.md); T0's transport is whatever `sub0mempage::FillBackendRef` the caller
 *  supplies, exercised in this project's own tests with a deterministic fake backend copied from
 *  Sub0MemPage's own tests/fake_backend.hpp (never installed by Sub0MemPage -- see
 *  docs/integration-plan.md's "Contract feedback to Sub0MemPage (T0)" section for why this is a
 *  test-support gap, not a design choice).
 *
 *  Architecture, one Table at a time (RowCache below is just a named collection of these):
 *
 *  - Resident-row key = (row_index, source_generation) (docs/integration-plan.md "Output and cache
 *    contract"); the device/context field that key shape reserves for later is omitted here, not
 *    encoded as a dummy "host" tag, since T0 is host-only end to end (AGENTS.md sec 2 -- no need to bake
 *    in a field no caller can vary yet). Two resident slots for the same row_index can coexist across a
 *    live generation transition (R4): an old lease keeps its slot resident on its captured generation
 *    while a fresh fetch for the new generation claims a different slot under the same row_index.
 *  - Slot state machine, deliberately the same shape as Sub0MemPage's own SlotPool (mirrored per
 *    AGENTS.md/STYLE_GUIDE.md's "mirror Sub0MemPage's style" instruction, not reinvented):
 *
 *        Free --admit--> Filling --ok--> Ready --evict (unpinned only)--> Free
 *                            |
 *                            +--error/short/codec-fail--> Free, or Failed while pins > 0
 *                                                          (Failed --last unpin--> Free)
 *
 *    Filling is never an eviction victim regardless of pins (mirrors SlotPool's claim_victim, which
 *    skips any non-ready state outright) -- this is what "never evicts leased/filling rows" means.
 *  - Coalescing (R5): the thread that finds a slot Free becomes its *finisher* -- it submits the
 *    transport request, blocks on it (finishers only ever run from resolve_into/wait, both allowed to
 *    block by R2), runs the codec if this table converts, and publishes Ready/Failed. Every other
 *    thread that finds the same slot Filling just waits on the table's own condition variable for the
 *    state to leave Filling; nobody else calls into the transport for that row, so exactly one live
 *    fetch exists per (row, generation) at a time.
 *  - try_get (R11) only ever reads state==Ready: it never touches the transport claim, never runs a
 *    codec, and never blocks -- a Filling row is reported as a miss even if the raw bytes already
 *    arrived, because publishing still requires the codec step this call is forbidden from doing.
 *  - Transport ownership: identity-representation tables submit straight into the cache's own output
 *    reservation via one `sub0mempage::TransferSet` (the explicit-destination mode -- docs/transfer-
 *    contract.md "Two uses of one transfer scheduler"). Converting tables read raw encoded bytes into a
 *    second, small, bounded TransferSet ("scratch"), run the codec into the output reservation, then
 *    release the scratch claim -- never retaining an unowned source pointer past the codec call (R6).
 *
 *  Allocation: everything the hot paths touch (slots_, the row index, ticket/waiter tables, the scratch
 *  free-list) is sized once in Table::create(); try_get and a resolve_into that only hits already-Ready
 *  rows perform no heap allocation (verified in tests/allocation_tests.cpp, mirroring Sub0MemPage's own
 *  gate).
 */

#include "codec.hpp"
#include "status.hpp"

#include <sub0mempage/transfer.hpp>
#include <sub0mempage/transfer_set.hpp>

#include <algorithm>
#include <bit>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <expected>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace sub0tieredcache {

using Clock = sub0mempage::Clock;
using Deadline = sub0mempage::Deadline;

/// How a table's output row relates to its source row (REQUIREMENTS.md R6).
enum class Representation : std::uint8_t {
    identity,    ///< Output bytes are exactly the source bytes; output_row_bytes == source_row_bytes.
    bf16_to_f32, ///< Built-in generic scalar widening (codec.hpp); output_row_bytes == source_row_bytes * 2.
    custom,      ///< A caller-registered Codec; TableConfig::codec must be non-null.
};

/** @brief Type-erased, non-owning row->source-extent adapter (REQUIREMENTS.md R7).
 *
 *  "A caller supplies an offset-resolution callback (row_index -> (shard, byte_offset)); the core only
 *  ever calls it." Mirrors sub0mempage::FillBackendRef's shape deliberately: a function pointer plus a
 *  non-owning context, not std::function, so resolving a row's extent never allocates. The callback
 *  itself must be prevalidated/bounded pure address arithmetic (AGENTS.md sec 2) -- it is called with no
 *  table lock held, but from a thread that may go on to block on I/O, so it must not itself block.
 */
class RowExtentResolverRef {
public:
    using Fn = std::expected<sub0mempage::ByteRange, Status> (*)(void*, std::uint64_t) noexcept;

    constexpr RowExtentResolverRef() noexcept = default;
    constexpr RowExtentResolverRef(void* context, Fn fn) noexcept : context_(context), fn_(fn) {}

    template <class Resolver>
        requires(!std::same_as<std::remove_cv_t<Resolver>, RowExtentResolverRef>) &&
                requires(Resolver& resolver, std::uint64_t row) {
                    { resolver.resolve_extent(row) } noexcept -> std::same_as<std::expected<sub0mempage::ByteRange, Status>>;
                }
    explicit RowExtentResolverRef(Resolver& resolver) noexcept
        : context_(&resolver),
          fn_([](void* self, std::uint64_t row) noexcept {
              return static_cast<Resolver*>(self)->resolve_extent(row);
          }) {}

    [[nodiscard]] bool valid() const noexcept { return fn_ != nullptr; }
    [[nodiscard]] std::expected<sub0mempage::ByteRange, Status> resolve(std::uint64_t row_index) const noexcept {
        return fn_(context_, row_index);
    }

private:
    void* context_ = nullptr;
    Fn fn_ = nullptr;
};

class Table;

/** @brief Move-only, owning pin on one resident output row (R11). Never an unowned view.
 *
 *  Storage stays valid until release, including while another thread is still filling a *different*
 *  slot -- a lease's own slot is Ready for as long as the lease is held (Ready+pinned is never an
 *  eviction victim). Release never blocks on I/O and never drains anything (mirrors
 *  sub0mempage::Lease::reset's contract exactly).
 */
class RowLease {
public:
    RowLease() noexcept = default;
    RowLease(RowLease&& other) noexcept { *this = std::move(other); }
    RowLease& operator=(RowLease&& other) noexcept;
    RowLease(const RowLease&) = delete;
    RowLease& operator=(const RowLease&) = delete;
    ~RowLease() { reset(); }

    [[nodiscard]] std::span<const std::byte> bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::uint64_t row_index() const noexcept { return row_index_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] bool is_held() const noexcept { return table_ != nullptr; }

    /// Unpins the row (no-op on an empty lease). Terminates on a lock failure, matching
    /// sub0mempage::Lease -- a release that cannot complete would leave the row pinned forever.
    void reset() noexcept;

private:
    friend class Table;

    Table* table_ = nullptr; // non-owning; the table must outlive every lease (checked at destruction)
    std::uint32_t slot_ = 0;
    std::uint32_t epoch_ = 0;
    std::span<const std::byte> bytes_;
    std::uint64_t row_index_ = 0;
    std::uint64_t generation_ = 0;
};

/// Move-only handle on a prefetch batch. Names which rows to observe with wait(); dropping it early
/// does not cancel anything in flight (mirrors sub0mempage::Ticket).
class PrefetchTicket {
public:
    PrefetchTicket() noexcept = default;
    PrefetchTicket(PrefetchTicket&& other) noexcept { *this = std::move(other); }
    PrefetchTicket& operator=(PrefetchTicket&& other) noexcept;
    PrefetchTicket(const PrefetchTicket&) = delete;
    PrefetchTicket& operator=(const PrefetchTicket&) = delete;
    ~PrefetchTicket() { reset(); }

    [[nodiscard]] bool is_held() const noexcept { return table_ != nullptr; }
    void reset() noexcept;

private:
    friend class Table;

    Table* table_ = nullptr;
    std::uint32_t record_ = 0;
    std::uint32_t generation_ = 0;
};

/// Per-row tally from wait(). `status` is ok only if every named row filled; timeout if any is still
/// pending; otherwise the first failure encountered in request order.
struct WaitOutcome {
    Status status = Status::ok;
    std::uint32_t filled = 0;
    std::uint32_t failed = 0;
    std::uint32_t pending = 0;
    std::uint32_t evicted = 0; ///< Prefetched but reclaimed (unpinned) before this wait observed it (R6).
};

/// Observability snapshot (R10). Nothing's behaviour depends on it.
struct TableStats {
    std::uint64_t hits = 0;            ///< try_get/resolve_into calls satisfied by an already-Ready row.
    std::uint64_t misses = 0;          ///< try_get calls that found no already-Ready row.
    std::uint64_t fetches = 0;         ///< Live transport fetches actually started (not coalesced).
    std::uint64_t coalesced = 0;       ///< Requests that joined an already in-flight fetch (R5).
    std::uint64_t conversions = 0;     ///< Successful codec conversions run.
    std::uint64_t codec_failures = 0;
    std::uint64_t evictions = 0;
    std::uint64_t resident = 0;        ///< Slots currently Ready.
    std::uint64_t leased = 0;          ///< Slots currently pinned (>0 leases outstanding).
    std::uint64_t failed = 0;          ///< Terminal transport/codec failures observed.
};

/// Registration parameters for one table (an administrative call: may allocate, may block).
struct TableConfig {
    std::uint64_t row_count = 0;              ///< Addressable row_index range is [0, row_count).
    std::uint64_t source_row_bytes = 0;       ///< Encoded row width the adapter's extents must have.
    std::uint64_t output_row_bytes = 0;       ///< Published row width (== source_row_bytes iff identity).
    Representation representation = Representation::identity;
    Codec* codec = nullptr;                   ///< Required iff representation == custom; unused otherwise.
    sub0mempage::SourceId source{};           ///< MemPage source identity for the current generation.
    std::uint64_t source_bytes = 0;           ///< Total addressable extent of that source.
    std::uint64_t generation = 0;             ///< Initial source generation (R4).
    RowExtentResolverRef resolve_extent;      ///< row_index -> encoded ByteRange (R1, R7).
    std::span<std::byte> output_storage;      ///< Caller-owned; size == budget_rows * output_row_bytes.
    std::uint32_t budget_rows = 0;            ///< Resident-row capacity (R12); the cache's whole budget.
    std::span<std::byte> scratch_storage;     ///< Caller-owned; size == scratch_rows*source_row_bytes.
    std::uint32_t scratch_rows = 0;           ///< Bounded raw-byte staging for conversion (0 if identity).
    std::uint32_t max_tickets = 0;            ///< Bound on live + dropped-but-in-flight prefetch batches.
    std::uint32_t max_batch_rows = 0;         ///< Bound on rows in one prefetch/resolve_into call.
};

namespace detail {

[[nodiscard]] constexpr Status from_mempage(sub0mempage::Status status) noexcept {
    using M = sub0mempage::Status;
    switch (status) {
    case M::ok: return Status::ok;
    case M::pending: return Status::pending;
    case M::not_resident: return Status::not_resident;
    case M::pool_exhausted: return Status::pool_exhausted;
    case M::batch_too_large: return Status::batch_too_large;
    case M::queue_exhausted: return Status::pool_exhausted;
    case M::ticket_exhausted: return Status::ticket_exhausted;
    case M::out_of_range: return Status::out_of_range;
    case M::empty_range: return Status::empty_range;
    case M::invalid_argument: return Status::invalid_argument;
    case M::busy: return Status::busy;
    case M::short_read: return Status::short_read;
    case M::io_error: return Status::io_error;
    case M::cancelled: return Status::cancelled;
    case M::timeout: return Status::timeout;
    case M::declined: return Status::declined;
    }
    return Status::io_error;
}

} // namespace detail

/** @brief One registered table: an independent, bounded, host-resident row cache.
 *
 *  Non-movable (it is the identity every RowLease/PrefetchTicket points back to), hence create() returns
 *  a unique_ptr. Destroying a table with outstanding leases, tickets or in-flight fetches terminates
 *  (mirrors Sub0MemPage's SlotPool/TransferSet exactly): drain() first on an administrative path.
 */
class Table {
    struct Passkey {};

public:
    [[nodiscard]] static std::expected<std::unique_ptr<Table>, Status> create(const TableConfig& config,
                                                                              sub0mempage::FillBackendRef backend);

    Table(Passkey, const TableConfig& config, sub0mempage::FillBackendRef backend);
    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;
    ~Table();

    /** @brief Hint: start fetches for every row in `rows` not already Ready. Never blocks on I/O (R2).
     *  Does not pin anything -- "a prefetch ticket ... is not a row pin" (docs/integration-plan.md).
     */
    [[nodiscard]] std::expected<PrefetchTicket, Status> prefetch(std::span<const std::uint64_t> rows) noexcept;

    /// Blocks until every row the ticket names is terminal (or evicted/reclaimed) or `deadline` passes.
    /// Observes only; does not pin (R2 allows this call to block).
    [[nodiscard]] WaitOutcome wait(const PrefetchTicket& ticket, Deadline deadline = std::nullopt) noexcept;

    /** @brief Pin every row of `rows` in order, fetching/converting misses, and block until all are
     *  ready (R2, R3). All-or-nothing (R14): on any failure no lease from this call is left held.
     *  Duplicates in `rows` are honoured (each gets its own lease on the same underlying slot).
     *  @param out Receives one lease per row in request order; must hold at least `rows.size()`. Leases
     *             already held in the entries this call writes are released first.
     *  @return Number of leases written (always rows.size() on success).
     */
    [[nodiscard]] std::expected<std::size_t, Status> resolve_into(std::span<const std::uint64_t> rows,
                                                                   std::span<RowLease> out) noexcept;

    /// Non-blocking, no I/O, no conversion (R11). A hit only if the row is already Ready on the current
    /// generation; a row still filling is reported as a miss, not awaited.
    [[nodiscard]] std::optional<RowLease> try_get(std::uint64_t row_index) noexcept;

    /** @brief Administrative generation transition (R4). New requests bind to `new_generation`
     *  immediately; already-resident rows keep their captured generation until evicted or explicitly
     *  drained, so old leases stay valid. Never blocks, never allocates, always succeeds -- budget
     *  conflicts between the two live generations are NOT rejected here (this call cannot know the
     *  future access pattern); they surface where R4 actually requires them to: a resolve_into/prefetch
     *  that cannot make room (every candidate victim is pinned or itself Filling) reports
     *  Status::pool_exhausted from that call, exactly as an ordinary single-generation exhaustion would.
     *  This is a deliberate design choice, not an oversight -- see this file's own tests
     *  (test_generation_budget_conflict_is_rejected in tests/row_cache_tests.cpp) for the case it covers.
     */
    void invalidate(std::uint64_t new_generation) noexcept;

    [[nodiscard]] TableStats stats() const noexcept;

    /// Administrative: blocks until no fetch is in flight anywhere in this table.
    [[nodiscard]] Status drain(Deadline deadline = std::nullopt) noexcept;

private:
    friend class RowLease;
    friend class PrefetchTicket;

    static constexpr std::uint32_t NONE = UINT32_MAX;

    enum class SlotState : std::uint8_t { free, filling, ready, failed };

    struct Slot {
        std::uint64_t row_index = 0;
        std::uint64_t row_generation = 0;
        std::uint32_t epoch = 0;   ///< Bumped every time this slot is admitted for a new (row, gen) key.
        std::uint32_t pins = 0;
        SlotState state = SlotState::free;
        Status failure = Status::ok;
        bool referenced = false;  ///< CLOCK second-chance bit; set by resolve_into/try_get hits.
        bool finishing = false;   ///< Some thread already owns driving this fill to a terminal state.
        std::optional<sub0mempage::Claim> claim; ///< Live only while state == filling.
        std::uint32_t scratch_slot = NONE;       ///< Valid only while converting and state == filling.
    };

    struct TicketRecord {
        std::uint32_t generation = 0;
        std::uint32_t count = 0;
        bool held = false;
        bool in_use = false;
    };

    struct WaiterEntry {
        std::uint32_t slot = NONE;
        std::uint32_t epoch = 0;
    };

    // --- row index: open addressing over (row_index, generation), linear probing, no tombstones ------

    [[nodiscard]] std::size_t index_home(std::uint64_t row_index, std::uint64_t generation) const noexcept;
    [[nodiscard]] std::uint32_t index_find(std::uint64_t row_index, std::uint64_t generation) const noexcept;
    void index_insert(std::uint64_t row_index, std::uint64_t generation, std::uint32_t slot) noexcept;
    void index_erase(std::uint64_t row_index, std::uint64_t generation) noexcept;

    [[nodiscard]] std::uint32_t claim_victim() noexcept;
    void make_free(std::uint32_t slot) noexcept;
    void unpin_locked(std::uint32_t slot) noexcept;
    void release_lease(std::uint32_t slot, std::uint32_t epoch) noexcept;
    void discard_ticket(std::uint32_t record, std::uint32_t generation) noexcept;

    /// Admits `slot` for (row_index, current_generation_), transitions it to Filling, submits the
    /// transport fetch (never blocks) and stores the Claim for a later finisher. Called under lock_.
    [[nodiscard]] Status start_fill_locked(std::uint32_t slot, std::uint64_t row_index) noexcept;

    /// Drives a Filling slot to a terminal state: blocks on its Claim, runs the codec if converting,
    /// publishes Ready/Failed. Called by exactly one thread per fill (guarded by Slot::finishing),
    /// WITHOUT lock_ held, then re-locks to publish. May block on I/O (only reached from resolve_into
    /// or wait, both allowed to by R2).
    void finish_fill(std::uint32_t slot) noexcept;

    /// Pins a row (fetching if necessary, becoming finisher inline if nobody else is). Blocking.
    [[nodiscard]] std::expected<RowLease, Status> resolve_one(std::uint64_t row_index) noexcept;

    /// Non-blocking: returns NONE if every scratch slot is in use. Never blocks (called while lock_ is
    /// held by start_fill_locked) -- a blocking acquire here could deadlock against a finisher that
    /// needs lock_ to publish the very release this call would be waiting for.
    [[nodiscard]] std::uint32_t acquire_scratch() noexcept;
    void release_scratch(std::uint32_t index) noexcept;

    [[nodiscard]] std::span<std::byte> output_span(std::uint32_t slot) const noexcept {
        return config_.output_storage.subspan(std::size_t{slot} * config_.output_row_bytes, config_.output_row_bytes);
    }
    [[nodiscard]] std::span<std::byte> scratch_span(std::uint32_t index) const noexcept {
        return config_.scratch_storage.subspan(std::size_t{index} * config_.source_row_bytes, config_.source_row_bytes);
    }

    TableConfig config_;
    sub0mempage::FillBackendRef backend_;
    Bf16ToF32Codec builtin_bf16_codec_;
    Codec* active_codec_ = nullptr; // nullptr iff identity (no codec step)

    std::unique_ptr<sub0mempage::TransferSet> row_transfer_;     // identity: reads straight into output_storage
    std::unique_ptr<sub0mempage::TransferSet> scratch_transfer_; // conversion: reads raw bytes into scratch_storage

    std::vector<Slot> slots_;
    std::vector<std::uint32_t> index_; ///< (row_index, generation) -> slot, open addressing.
    int index_shift_ = 0;

    std::vector<TicketRecord> tickets_;
    std::vector<WaiterEntry> waiters_; ///< max_tickets * max_batch_rows, record-major.
    std::uint32_t clock_hand_ = 0;
    std::uint64_t current_generation_ = 0;
    std::uint32_t pins_total_ = 0;
    std::uint32_t tickets_held_ = 0;
    std::uint32_t in_flight_ = 0;
    std::uint32_t non_free_ = 0;
    TableStats counters_;

    mutable std::mutex lock_;
    std::condition_variable progress_; ///< Signalled whenever any slot leaves Filling.

    std::mutex scratch_lock_;
    std::vector<bool> scratch_used_;
};

/// A named collection of independently registered tables (the "register_table"-shaped entry point --
/// docs/integration-plan.md's delivery table calls this out as T0's deliverable shape). Thin: it owns
/// Table instances and forwards by handle, adding no policy of its own (AGENTS.md sec 2).
class RowCache {
public:
    using TableHandle = std::uint32_t;

    RowCache() = default;
    RowCache(const RowCache&) = delete;
    RowCache& operator=(const RowCache&) = delete;

    [[nodiscard]] std::expected<TableHandle, Status> register_table(const TableConfig& config,
                                                                     sub0mempage::FillBackendRef backend) {
        auto table = Table::create(config, backend);
        if (!table) {
            return std::unexpected(table.error());
        }
        tables_.push_back(std::move(*table));
        return static_cast<TableHandle>(tables_.size() - 1);
    }

    [[nodiscard]] Table* table(TableHandle handle) noexcept {
        return handle < tables_.size() ? tables_[handle].get() : nullptr;
    }
    [[nodiscard]] const Table* table(TableHandle handle) const noexcept {
        return handle < tables_.size() ? tables_[handle].get() : nullptr;
    }

private:
    std::vector<std::unique_ptr<Table>> tables_;
};

// ---------------------------------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------------------------------

inline RowLease& RowLease::operator=(RowLease&& other) noexcept {
    if (this != &other) {
        reset();
        table_ = std::exchange(other.table_, nullptr);
        slot_ = other.slot_;
        epoch_ = other.epoch_;
        bytes_ = std::exchange(other.bytes_, {});
        row_index_ = other.row_index_;
        generation_ = other.generation_;
    }
    return *this;
}

inline void RowLease::reset() noexcept {
    if (table_ != nullptr) {
        std::exchange(table_, nullptr)->release_lease(slot_, epoch_);
        bytes_ = {};
    }
}

inline PrefetchTicket& PrefetchTicket::operator=(PrefetchTicket&& other) noexcept {
    if (this != &other) {
        reset();
        table_ = std::exchange(other.table_, nullptr);
        record_ = other.record_;
        generation_ = other.generation_;
    }
    return *this;
}

inline void PrefetchTicket::reset() noexcept {
    if (table_ != nullptr) {
        std::exchange(table_, nullptr)->discard_ticket(record_, generation_);
    }
}

inline std::expected<std::unique_ptr<Table>, Status> Table::create(const TableConfig& config,
                                                                    sub0mempage::FillBackendRef backend) {
    if (config.row_count == 0 || config.source_row_bytes == 0 || config.output_row_bytes == 0 ||
        config.source_bytes == 0 || config.budget_rows == 0 || config.max_tickets == 0 ||
        config.max_batch_rows == 0 || config.budget_rows >= NONE || config.max_tickets >= NONE ||
        !config.resolve_extent.valid()) {
        return std::unexpected(Status::invalid_argument);
    }
    if (config.max_batch_rows > SIZE_MAX / config.max_tickets ||
        std::size_t{config.max_batch_rows} * config.max_tickets >= NONE) {
        return std::unexpected(Status::invalid_argument);
    }
    // Representation/width compatibility is checked before output_storage's size, deliberately: an
    // unsupported source/output pairing is the more specific, more actionable diagnosis (R6) even when
    // the caller's storage span also happens to be the wrong size for it.
    switch (config.representation) {
    case Representation::identity:
        if (config.output_row_bytes != config.source_row_bytes) {
            return std::unexpected(Status::unsupported_conversion);
        }
        break;
    case Representation::bf16_to_f32:
        // bf16 elements are 2 bytes; widening doubles the byte count exactly (codec.hpp).
        if (config.source_row_bytes % 2 != 0 || config.output_row_bytes != config.source_row_bytes * 2) {
            return std::unexpected(Status::unsupported_conversion);
        }
        break;
    case Representation::custom:
        if (config.codec == nullptr) {
            return std::unexpected(Status::invalid_argument);
        }
        break;
    }
    if (config.output_row_bytes > SIZE_MAX / config.budget_rows ||
        config.output_storage.size() != std::size_t{config.output_row_bytes} * config.budget_rows) {
        return std::unexpected(Status::invalid_argument);
    }
    if (config.representation != Representation::identity) {
        if (config.scratch_rows == 0 ||
            config.source_row_bytes > SIZE_MAX / config.scratch_rows ||
            config.scratch_storage.size() != std::size_t{config.source_row_bytes} * config.scratch_rows) {
            return std::unexpected(Status::invalid_argument);
        }
    }
    return std::make_unique<Table>(Passkey{}, config, backend);
}

inline Table::Table(Passkey, const TableConfig& config, sub0mempage::FillBackendRef backend)
    : config_(config),
      backend_(backend),
      slots_(config.budget_rows),
      index_(std::bit_ceil(std::size_t{config.budget_rows} * 2), NONE),
      tickets_(config.max_tickets),
      waiters_(std::size_t{config.max_tickets} * config.max_batch_rows),
      current_generation_(config.generation),
      scratch_used_(config.scratch_rows, false) {
    index_shift_ = 64 - std::countr_zero(index_.size());

    active_codec_ = config_.representation == Representation::identity   ? nullptr
                   : config_.representation == Representation::bf16_to_f32 ? &builtin_bf16_codec_
                                                                            : config_.codec;

    // Identity rows land straight in the output reservation (no second, lower cache -- docs/transfer-
    // contract.md "Two uses of one transfer scheduler"). Converting tables additionally need a bounded
    // raw-byte staging area, read once per fetch and released the moment the codec finishes with it.
    row_transfer_ = std::move(*sub0mempage::TransferSet::create(
        {.source = config_.source,
         .source_bytes = config_.source_bytes,
         .destination = config_.output_storage,
         .max_claims = config_.budget_rows},
        backend_));
    if (active_codec_ != nullptr) {
        scratch_transfer_ = std::move(*sub0mempage::TransferSet::create(
            {.source = config_.source,
             .source_bytes = config_.source_bytes,
             .destination = config_.scratch_storage,
             .max_claims = config_.scratch_rows},
            backend_));
    }
}

inline Table::~Table() {
    const std::scoped_lock lock(lock_);
    if (in_flight_ != 0 || pins_total_ != 0 || tickets_held_ != 0) {
        // A finisher could still be writing into caller storage, or a lease/ticket would dangle.
        // Teardown never drains implicitly (mirrors sub0mempage's own contract) -- drain() first.
        std::terminate();
    }
}

// --- row index -----------------------------------------------------------------------------------------

inline std::size_t Table::index_home(std::uint64_t row_index, std::uint64_t generation) const noexcept {
    // splitmix64-style combine of two 64-bit keys into one hash; the constants are splitmix64's own
    // (Sebastiano Vigna, public-domain reference implementation), used identically to SlotPool's own
    // single-key hash so both index shapes share the same, already-reasoned-about avalanche property.
    std::uint64_t h = row_index + 0x9E3779B97F4A7C15ull;
    h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ull;
    h ^= generation + 0x9E3779B97F4A7C15ull;
    h = (h ^ (h >> 27)) * 0x94D049BB133111EBull;
    h ^= (h >> 31);
    return static_cast<std::size_t>(h >> index_shift_);
}

inline std::uint32_t Table::index_find(std::uint64_t row_index, std::uint64_t generation) const noexcept {
    const std::size_t mask = index_.size() - 1;
    for (std::size_t pos = index_home(row_index, generation);; pos = (pos + 1) & mask) {
        const std::uint32_t slot = index_[pos];
        if (slot == NONE || (slots_[slot].row_index == row_index && slots_[slot].row_generation == generation)) {
            return slot;
        }
    }
}

inline void Table::index_insert(std::uint64_t row_index, std::uint64_t generation, std::uint32_t slot) noexcept {
    const std::size_t mask = index_.size() - 1;
    std::size_t pos = index_home(row_index, generation);
    while (index_[pos] != NONE) {
        pos = (pos + 1) & mask;
    }
    index_[pos] = slot;
}

inline void Table::index_erase(std::uint64_t row_index, std::uint64_t generation) noexcept {
    const std::size_t mask = index_.size() - 1;
    std::size_t hole = index_home(row_index, generation);
    while (!(slots_[index_[hole]].row_index == row_index && slots_[index_[hole]].row_generation == generation)) {
        hole = (hole + 1) & mask;
    }
    for (std::size_t pos = (hole + 1) & mask; index_[pos] != NONE; pos = (pos + 1) & mask) {
        const std::size_t home = index_home(slots_[index_[pos]].row_index, slots_[index_[pos]].row_generation);
        if (((pos - home) & mask) >= ((pos - hole) & mask)) {
            index_[hole] = index_[pos];
            hole = pos;
        }
    }
    index_[hole] = NONE;
}

// --- slot lifecycle --------------------------------------------------------------------------------------

/// CLOCK (second chance), the same policy and citation as Sub0MemPage's SlotPool (PostgreSQL's buffer
/// manager clock sweep -- docs/prior-art.md via Sub0MemPage's own slot_pool.hpp, reused rather than
/// re-derived per AGENTS.md sec 5's "verify against real prior art" -- the policy choice was already
/// made and cited one layer down; re-picking a different one here for no reason would be the scope
/// violation, not reusing it). Filling slots are skipped unconditionally, matching "never evicts
/// leased/filling rows": a state check, not a pins check, because a Filling slot may have zero pins
/// (a prefetch that nobody has waited on yet) and must still never be reclaimed mid-write.
inline std::uint32_t Table::claim_victim() noexcept {
    const std::uint32_t n = static_cast<std::uint32_t>(slots_.size());
    for (std::uint32_t step = 0; step < 2 * n; ++step) {
        const std::uint32_t slot = clock_hand_;
        clock_hand_ = clock_hand_ + 1 == n ? 0 : clock_hand_ + 1;
        Slot& candidate = slots_[slot];
        if (candidate.state == SlotState::free) {
            return slot;
        }
        if (candidate.state != SlotState::ready || candidate.pins != 0) {
            continue;
        }
        if (candidate.referenced) {
            candidate.referenced = false;
            continue;
        }
        ++counters_.evictions;
        make_free(slot);
        return slot;
    }
    return NONE;
}

inline void Table::make_free(std::uint32_t slot) noexcept {
    Slot& s = slots_[slot];
    if (s.state == SlotState::filling || s.state == SlotState::ready || s.state == SlotState::failed) {
        index_erase(s.row_index, s.row_generation);
    }
    if (s.state != SlotState::free) {
        --non_free_;
    }
    s.state = SlotState::free;
    s.referenced = false;
    s.claim.reset();
    s.scratch_slot = NONE;
}

inline Status Table::start_fill_locked(std::uint32_t slot, std::uint64_t row_index) noexcept {
    Slot& s = slots_[slot];
    s.row_index = row_index;
    s.row_generation = current_generation_;
    ++s.epoch;
    s.state = SlotState::filling;
    s.failure = Status::ok;
    s.finishing = false;
    s.scratch_slot = NONE;
    index_insert(row_index, current_generation_, slot);
    non_free_ = std::min(static_cast<std::uint32_t>(slots_.size()), non_free_ + 1);

    const auto extent = config_.resolve_extent.resolve(row_index);
    if (!extent) {
        s.state = SlotState::failed;
        s.failure = extent.error();
        return extent.error();
    }
    if (extent->length != config_.source_row_bytes) {
        s.state = SlotState::failed;
        s.failure = Status::invalid_argument;
        return Status::invalid_argument;
    }

    if (active_codec_ == nullptr) {
        auto claim = row_transfer_->submit(*extent, std::uint64_t{slot} * config_.output_row_bytes);
        if (!claim) {
            s.state = SlotState::failed;
            s.failure = detail::from_mempage(claim.error());
            return s.failure;
        }
        s.claim = std::move(*claim);
    } else {
        // Non-blocking: the scratch pool is bounded (R12), and a blocking wait here would hold lock_
        // while waiting for a release that itself needs lock_ (see acquire_scratch's own comment).
        const std::uint32_t scratch = acquire_scratch();
        if (scratch == NONE) {
            s.state = SlotState::failed;
            s.failure = Status::pool_exhausted;
            return Status::pool_exhausted;
        }
        auto claim = scratch_transfer_->submit(*extent, std::uint64_t{scratch} * config_.source_row_bytes);
        if (!claim) {
            release_scratch(scratch);
            s.state = SlotState::failed;
            s.failure = detail::from_mempage(claim.error());
            return s.failure;
        }
        s.scratch_slot = scratch;
        s.claim = std::move(*claim);
    }
    ++in_flight_;
    ++counters_.fetches;
    return Status::ok;
}

inline void Table::finish_fill(std::uint32_t slot) noexcept {
    std::unique_lock lock(lock_);
    Slot& s = slots_[slot];
    if (s.state != SlotState::filling) {
        return; // already finished by someone else, or evicted (should not happen while filling)
    }
    sub0mempage::Claim* claim = &*s.claim; // stable address; only the finisher (this thread) touches it
    const bool converting = active_codec_ != nullptr;
    const std::uint32_t scratch = s.scratch_slot;
    lock.unlock();

    const sub0mempage::Status raw_status = claim->wait();
    Status result = detail::from_mempage(raw_status);
    if (result == Status::ok && converting) {
        if (active_codec_->convert(claim->bytes(), output_span(slot))) {
            result = Status::ok;
        } else {
            result = Status::codec_failed;
        }
    }

    lock.lock();
    s.claim.reset(); // releases the identity or scratch reservation now that we are done reading it
    if (converting) {
        release_scratch(scratch);
        s.scratch_slot = NONE;
    }
    --in_flight_;
    if (result == Status::ok) {
        s.state = SlotState::ready;
        if (converting) {
            ++counters_.conversions;
        }
    } else {
        ++counters_.failed;
        if (result == Status::codec_failed) {
            ++counters_.codec_failures;
        }
        s.state = SlotState::failed;
        s.failure = result;
        if (s.pins == 0) {
            make_free(slot);
        }
    }
    s.finishing = false;
    progress_.notify_all();
}

inline void Table::unpin_locked(std::uint32_t slot) noexcept {
    Slot& s = slots_[slot];
    --pins_total_;
    if (--s.pins == 0 && s.state == SlotState::failed) {
        make_free(slot);
    }
}

inline void Table::release_lease(std::uint32_t slot, std::uint32_t epoch) noexcept {
    const std::scoped_lock lock(lock_);
    if (slots_[slot].epoch != epoch || slots_[slot].pins == 0) {
        std::terminate(); // ABA: the slot was reused under us, or double-release -- unrecoverable
    }
    unpin_locked(slot);
}

inline void Table::discard_ticket(std::uint32_t record_index, std::uint32_t generation) noexcept {
    const std::scoped_lock lock(lock_);
    TicketRecord& record = tickets_[record_index];
    if (record.generation != generation || !record.held) {
        std::terminate();
    }
    record.held = false;
    --tickets_held_;
    record.in_use = false;
}

inline std::uint32_t Table::acquire_scratch() noexcept {
    const std::scoped_lock lock(scratch_lock_);
    for (std::uint32_t i = 0; i < scratch_used_.size(); ++i) {
        if (!scratch_used_[i]) {
            scratch_used_[i] = true;
            return i;
        }
    }
    return NONE;
}

inline void Table::release_scratch(std::uint32_t index) noexcept {
    if (index == NONE) {
        return;
    }
    const std::scoped_lock lock(scratch_lock_);
    scratch_used_[index] = false;
}

// --- public operations -----------------------------------------------------------------------------------

inline std::expected<RowLease, Status> Table::resolve_one(std::uint64_t row_index) noexcept {
    if (row_index >= config_.row_count) {
        return std::unexpected(Status::out_of_range);
    }
    std::unique_lock lock(lock_);
    std::uint32_t slot = index_find(row_index, current_generation_);
    bool became_finisher = false;
    const bool found_ready = slot != NONE && slots_[slot].state == SlotState::ready;
    const bool found_filling = slot != NONE && slots_[slot].state == SlotState::filling;
    if (found_filling) {
        ++counters_.coalesced; // joining a fetch someone else already started (R5)
    }
    if (slot == NONE) {
        slot = claim_victim();
        if (slot == NONE) {
            return std::unexpected(Status::pool_exhausted);
        }
        // No unlock between claim_victim() and start_fill_locked(): both only touch this table's own
        // bookkeeping plus non-blocking transport submission (a blocking scratch-pool wait can still
        // happen inside it for a converting table, serialising the table against its own scratch pool
        // -- acceptable for T0's bounded pool sizes; see the file comment's allocation note).
        const Status started = start_fill_locked(slot, row_index);
        became_finisher = started == Status::ok;
        if (started != Status::ok && slots_[slot].pins == 0) {
            make_free(slot);
            return std::unexpected(started);
        }
    }
    Slot& s = slots_[slot];
    ++s.pins;
    ++pins_total_;
    s.referenced = true;
    const std::uint32_t epoch = s.epoch;

    if (became_finisher) {
        s.finishing = true;
        lock.unlock();
        finish_fill(slot);
        lock.lock();
    } else {
        if (s.state == SlotState::filling && !s.finishing) {
            s.finishing = true;
            lock.unlock();
            finish_fill(slot);
            lock.lock();
        } else {
            progress_.wait(lock, [&] { return s.epoch != epoch || s.state != SlotState::filling; });
        }
    }

    if (s.epoch != epoch) {
        // Evicted/reused out from under an unpinned wait window; cannot happen while pinned, so this
        // path is unreachable given the pin taken above, but guarded rather than assumed.
        return std::unexpected(Status::pool_exhausted);
    }
    if (s.state == SlotState::ready) {
        if (found_ready) {
            ++counters_.hits; // already Ready before this call touched it; fetches/coalesced cover the rest
        }
        RowLease lease;
        lease.table_ = this;
        lease.slot_ = slot;
        lease.epoch_ = epoch;
        lease.bytes_ = output_span(slot);
        lease.row_index_ = row_index;
        lease.generation_ = s.row_generation;
        return lease;
    }
    const Status failure = s.failure;
    unpin_locked(slot);
    return std::unexpected(failure);
}

inline std::expected<std::size_t, Status> Table::resolve_into(std::span<const std::uint64_t> rows,
                                                               std::span<RowLease> out) noexcept {
    if (rows.empty()) {
        return std::unexpected(Status::empty_range);
    }
    if (rows.size() > config_.max_batch_rows) {
        return std::unexpected(Status::batch_too_large);
    }
    if (out.size() < rows.size()) {
        return std::unexpected(Status::invalid_argument);
    }
    for (std::size_t i = 0; i < rows.size(); ++i) {
        out[i].reset(); // release whatever the caller's storage held before this call (mirrors SlotPool)
    }
    std::size_t filled = 0;
    Status failure = Status::ok;
    for (std::size_t i = 0; i < rows.size() && failure == Status::ok; ++i) {
        auto lease = resolve_one(rows[i]);
        if (!lease) {
            failure = lease.error();
            break;
        }
        out[filled++] = std::move(*lease);
    }
    if (failure != Status::ok) {
        for (std::size_t i = 0; i < filled; ++i) {
            out[i].reset();
        }
        return std::unexpected(failure);
    }
    return filled;
}

inline std::optional<RowLease> Table::try_get(std::uint64_t row_index) noexcept {
    if (row_index >= config_.row_count) {
        return std::nullopt;
    }
    const std::scoped_lock lock(lock_);
    const std::uint32_t slot = index_find(row_index, current_generation_);
    if (slot == NONE || slots_[slot].state != SlotState::ready) {
        ++counters_.misses;
        return std::nullopt;
    }
    Slot& s = slots_[slot];
    ++s.pins;
    ++pins_total_;
    s.referenced = true;
    ++counters_.hits;
    RowLease lease;
    lease.table_ = this;
    lease.slot_ = slot;
    lease.epoch_ = s.epoch;
    lease.bytes_ = output_span(slot);
    lease.row_index_ = row_index;
    lease.generation_ = s.row_generation;
    return lease;
}

inline std::expected<PrefetchTicket, Status> Table::prefetch(std::span<const std::uint64_t> rows) noexcept {
    if (rows.empty()) {
        return std::unexpected(Status::empty_range);
    }
    if (rows.size() > config_.max_batch_rows) {
        return std::unexpected(Status::batch_too_large);
    }
    for (const std::uint64_t row : rows) {
        if (row >= config_.row_count) {
            return std::unexpected(Status::out_of_range);
        }
    }
    std::unique_lock lock(lock_);
    const auto free_record =
        std::find_if(tickets_.begin(), tickets_.end(), [](const TicketRecord& r) { return !r.in_use; });
    if (free_record == tickets_.end()) {
        return std::unexpected(Status::ticket_exhausted);
    }
    const auto record_index = static_cast<std::uint32_t>(free_record - tickets_.begin());
    TicketRecord& record = *free_record;
    record = {.generation = record.generation + 1, .count = static_cast<std::uint32_t>(rows.size()),
              .held = true, .in_use = true};
    ++tickets_held_;

    for (std::size_t i = 0; i < rows.size(); ++i) {
        const std::uint64_t row_index = rows[i];
        std::uint32_t slot = index_find(row_index, current_generation_);
        if (slot == NONE) {
            slot = claim_victim();
            if (slot == NONE) {
                waiters_[std::size_t{record_index} * config_.max_batch_rows + i] = {NONE, 0};
                continue; // exhaustion recorded implicitly: wait() finds no slot and reports "evicted"
            }
            (void)start_fill_locked(slot, row_index); // failure recorded on the slot itself
        } else if (slots_[slot].state == SlotState::ready) {
            ++counters_.hits;
        } else {
            ++counters_.coalesced;
        }
        waiters_[std::size_t{record_index} * config_.max_batch_rows + i] = {slot, slots_[slot].epoch};
    }
    PrefetchTicket ticket;
    ticket.table_ = this;
    ticket.record_ = record_index;
    ticket.generation_ = record.generation;
    return ticket;
}

inline WaitOutcome Table::wait(const PrefetchTicket& ticket, Deadline deadline) noexcept {
    if (ticket.table_ != this) {
        return {.status = Status::invalid_argument};
    }
    const TicketRecord& record = tickets_[ticket.record_];
    const auto entries = std::span(waiters_).subspan(std::size_t{ticket.record_} * config_.max_batch_rows, record.count);

    WaitOutcome outcome;
    for (const WaiterEntry& entry : entries) {
        if (entry.slot == NONE) {
            ++outcome.evicted;
            continue;
        }
        std::unique_lock lock(lock_);
        Slot& s = slots_[entry.slot];
        if (s.epoch != entry.epoch) {
            ++outcome.evicted;
            continue;
        }
        if (s.state == SlotState::filling && !s.finishing) {
            // Become this row's finisher: drive its fetch/codec to completion ourselves.
            s.finishing = true;
            lock.unlock();
            finish_fill(entry.slot);
            lock.lock();
        } else if (s.state == SlotState::filling) {
            // Someone else is already the finisher; just wait for them to publish a terminal state.
            const auto done = [&] { return s.epoch != entry.epoch || s.state != SlotState::filling; };
            bool reached = true;
            if (deadline) {
                reached = progress_.wait_until(lock, *deadline, done);
            } else {
                progress_.wait(lock, done);
            }
            if (!reached) {
                ++outcome.pending;
                outcome.status = Status::timeout;
                continue;
            }
        }
        if (s.epoch != entry.epoch) {
            ++outcome.evicted;
        } else if (s.state == SlotState::ready) {
            ++outcome.filled;
        } else {
            ++outcome.failed;
            if (outcome.status == Status::ok) {
                outcome.status = s.failure;
            }
        }
    }
    return outcome;
}

inline void Table::invalidate(std::uint64_t new_generation) noexcept {
    const std::scoped_lock lock(lock_);
    current_generation_ = new_generation;
}

inline TableStats Table::stats() const noexcept {
    const std::scoped_lock lock(lock_);
    TableStats snapshot = counters_;
    for (const Slot& s : slots_) {
        snapshot.resident += s.state == SlotState::ready ? 1 : 0;
        snapshot.leased += s.pins != 0 ? 1 : 0;
    }
    return snapshot;
}

inline Status Table::drain(Deadline deadline) noexcept {
    std::unique_lock lock(lock_);
    const auto idle = [&] { return in_flight_ == 0; };
    if (deadline) {
        return progress_.wait_until(lock, *deadline, idle) ? Status::ok : Status::timeout;
    }
    progress_.wait(lock, idle);
    return Status::ok;
}

} // namespace sub0tieredcache
