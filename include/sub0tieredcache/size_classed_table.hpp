#pragma once

/** @file size_classed_table.hpp
 *  @brief A row cache whose rows come in a few exact sizes, each class held in slots of exactly its
 *         own width, so no budget is spent on padding.
 *
 *  A single Table sizes every slot to its widest row (RowExtent::bounded). When rows come in a few
 *  distinct sizes -- a mixture-of-experts sidecar whose expert size differs between layers, say -- the
 *  narrower rows waste the difference in every slot. SizeClassedTable discovers the distinct row
 *  lengths once, at registration, and keeps one exact-width Table per length over its share of the
 *  caller's storage. Lookups route by row to the right class; the interface mirrors Table's.
 *
 *  Registration walks every row through the resolver: O(row_count), administrative. A table whose
 *  rows are all one width gains nothing from this and should use Table directly.
 *
 *  The storage is split so that every class keeps the same fraction of its own rows resident (each
 *  class's share is proportional to its total bytes). That is the right split when each class is read
 *  in proportion to its size, as when every row group is read equally often.
 */

#include "row_cache.hpp"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <numeric>
#include <span>
#include <vector>

namespace sub0tieredcache {

/// Registration for a SizeClassedTable: a TableConfig without the per-table width, budget and
/// representation fields, which are derived per class.
struct SizeClassedTableConfig {
    std::uint64_t row_count = 0;              ///< Addressable row_index range is [0, row_count).
    std::span<const ShardSource> sources;     ///< As TableConfig::sources.
    std::uint64_t generation = 0;
    RowExtentResolverRef resolve_extent;      ///< row_index -> RowLocation; lengths define the classes.
    std::span<std::byte> output_storage;      ///< Caller-owned; split between classes. Must outlive the table.
    std::uint32_t max_tickets = 0;            ///< Per class, as TableConfig::max_tickets.
    std::uint32_t max_batch_rows = 0;         ///< Per call, as TableConfig::max_batch_rows.
    std::uint64_t fill_chunk_bytes = 0;       ///< As TableConfig::fill_chunk_bytes, for every class.
    std::uint32_t max_classes = 8;            ///< Refuse registration with more distinct row lengths.
};

/** @brief Identity row cache over a few exact row sizes; see the file comment.
 *
 *  Blocking follows Table: try_get never blocks; prefetch never blocks on I/O; resolve_into and drain
 *  may. try_get, resolve_into, stats and drain are thread-safe, as Table's. prefetch splits the batch
 *  into per-instance scratch, so it takes one caller at a time. Not movable: the class tables hold
 *  references to its resolvers.
 */
class SizeClassedTable {
    struct Passkey {};

public:
    /// One row-length class and its share of the storage.
    struct Class {
        std::uint64_t width = 0;      ///< Exact length of every row in this class.
        std::uint64_t rows = 0;       ///< Rows of the table in this class.
        std::uint32_t budget_rows = 0;///< Resident capacity of this class.
    };

    /** Discovers the row-length classes, splits the storage and creates one Table per class.
     *  @return invalid_argument for an empty table, storage too small to give every class two batches
     *          of rows, or more than max_classes distinct lengths; otherwise as Table::create.
     */
    [[nodiscard]] static std::expected<std::unique_ptr<SizeClassedTable>, Status>
    create(const SizeClassedTableConfig& config, sub0mempage::FillBackendRef backend);

    SizeClassedTable(Passkey, const SizeClassedTableConfig& config) : config_(config) {}
    SizeClassedTable(const SizeClassedTable&) = delete;
    SizeClassedTable& operator=(const SizeClassedTable&) = delete;

    /// Hint: start fetches for every listed row not already resident, class by class. Never blocks on
    /// I/O. One caller at a time (per-instance scratch).
    [[nodiscard]] Status prefetch(std::span<const std::uint64_t> rows) noexcept;
    /** Pins every row, fetching misses, and blocks until all are resident. All-or-nothing, as Table:
     *  on failure no lease is left held. Rows resolve one at a time, so a batch of misses waits for
     *  each in turn: call prefetch first to put every fill in flight. @return the number of leases written.
     */
    [[nodiscard]] std::expected<std::size_t, Status> resolve_into(std::span<const std::uint64_t> rows,
                                                                  std::span<RowLease> out) noexcept;
    /// Non-blocking, no I/O: a lease only if the row is already resident.
    [[nodiscard]] std::optional<RowLease> try_get(std::uint64_t row_index) noexcept;
    /// Sum of every class's counters.
    [[nodiscard]] TableStats stats() const noexcept;
    /// Administrative: blocks until no class has a fill in flight.
    [[nodiscard]] Status drain(Deadline deadline = std::nullopt) noexcept;
    /// The discovered classes, in increasing width.
    [[nodiscard]] std::span<const Class> classes() const noexcept { return classes_; }

private:
    /// Resolves a class-local row index through the caller's resolver.
    struct ClassResolver {
        const SizeClassedTable* owner = nullptr; // non-owning
        std::uint32_t cls = 0;
        [[nodiscard]] std::expected<RowLocation, Status> resolve_extent(std::uint64_t local) const noexcept {
            return owner->config_.resolve_extent.resolve(owner->global_of_[cls][static_cast<std::size_t>(local)]);
        }
    };

    SizeClassedTableConfig config_;
    std::vector<Class> classes_;
    std::vector<std::uint32_t> class_of_;               ///< global row -> class
    std::vector<std::uint32_t> local_of_;               ///< global row -> index within its class
    std::vector<std::vector<std::uint64_t>> global_of_; ///< class, local -> global row
    std::vector<std::unique_ptr<ClassResolver>> resolvers_; // stable addresses: tables hold refs to them
    std::vector<std::unique_ptr<Table>> tables_;
    std::vector<std::vector<std::uint64_t>> scratch_;   ///< per class: one call's local rows (max_batch_rows)
};

// ---------------------------------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------------------------------

inline std::expected<std::unique_ptr<SizeClassedTable>, Status>
SizeClassedTable::create(const SizeClassedTableConfig& config, sub0mempage::FillBackendRef backend) {
    if (config.row_count == 0 || config.row_count >= UINT32_MAX || !config.resolve_extent.valid() ||
        config.max_batch_rows == 0 || config.max_classes == 0) {
        return std::unexpected(Status::invalid_argument);
    }
    auto t = std::make_unique<SizeClassedTable>(Passkey{}, config);
    // Discover the classes: one pass over every row's extent (administrative, may allocate).
    t->class_of_.resize(static_cast<std::size_t>(config.row_count));
    t->local_of_.resize(static_cast<std::size_t>(config.row_count));
    std::vector<std::uint64_t> widths;
    for (std::uint64_t row = 0; row < config.row_count; ++row) {
        const auto location = config.resolve_extent.resolve(row);
        if (!location) return std::unexpected(location.error());
        const std::uint64_t width = location->range.length;
        if (width == 0) return std::unexpected(Status::invalid_argument);
        auto it = std::find(widths.begin(), widths.end(), width);
        if (it == widths.end()) {
            if (widths.size() == config.max_classes) return std::unexpected(Status::invalid_argument);
            widths.push_back(width);
            it = widths.end() - 1;
        }
        t->class_of_[static_cast<std::size_t>(row)] = static_cast<std::uint32_t>(it - widths.begin());
    }
    // Renumber classes by increasing width, and give every row its class-local index.
    std::vector<std::uint32_t> order(widths.size());
    std::iota(order.begin(), order.end(), 0u);
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return widths[a] < widths[b]; });
    std::vector<std::uint32_t> rank(widths.size());
    for (std::uint32_t r = 0; r < order.size(); ++r) rank[order[r]] = r;
    t->classes_.resize(widths.size());
    t->global_of_.resize(widths.size());
    for (std::uint32_t c = 0; c < widths.size(); ++c) t->classes_[rank[c]].width = widths[c];
    for (std::uint64_t row = 0; row < config.row_count; ++row) {
        auto& cls = t->class_of_[static_cast<std::size_t>(row)];
        cls = rank[cls];
        t->local_of_[static_cast<std::size_t>(row)] = static_cast<std::uint32_t>(t->global_of_[cls].size());
        t->global_of_[cls].push_back(row);
    }
    // Split the storage in proportion to each class's total bytes, in whole rows of each class's width.
    double total_bytes = 0;
    for (std::uint32_t c = 0; c < t->classes_.size(); ++c) {
        t->classes_[c].rows = t->global_of_[c].size();
        total_bytes += static_cast<double>(t->classes_[c].rows) * static_cast<double>(t->classes_[c].width);
    }
    std::uint64_t used = 0;
    for (auto& cls : t->classes_) {
        const double share = static_cast<double>(cls.rows) * static_cast<double>(cls.width) / total_bytes;
        const auto bytes = static_cast<std::uint64_t>(share * static_cast<double>(config.output_storage.size()));
        const std::uint64_t rows = std::min<std::uint64_t>(bytes / cls.width, cls.rows);
        if (rows < 2ull * config.max_batch_rows || rows >= UINT32_MAX) return std::unexpected(Status::invalid_argument);
        cls.budget_rows = static_cast<std::uint32_t>(rows);
        used += rows * cls.width;
    }
    if (used > config.output_storage.size()) return std::unexpected(Status::invalid_argument);
    // One exact-width Table per class over its own slice of the storage.
    std::uint64_t at = 0;
    for (std::uint32_t c = 0; c < t->classes_.size(); ++c) {
        const Class& cls = t->classes_[c];
        t->resolvers_.push_back(std::make_unique<ClassResolver>(ClassResolver{t.get(), c}));
        TableConfig tc{};
        tc.row_count = cls.rows;
        tc.source_row_bytes = cls.width;
        tc.output_row_bytes = cls.width;
        tc.row_extent = RowExtent::exact;
        tc.representation = Representation::identity;
        tc.sources = config.sources;
        tc.generation = config.generation;
        tc.resolve_extent = RowExtentResolverRef(*t->resolvers_.back());
        tc.output_storage = config.output_storage.subspan(static_cast<std::size_t>(at),
                                                          static_cast<std::size_t>(cls.budget_rows * cls.width));
        tc.budget_rows = cls.budget_rows;
        tc.max_tickets = config.max_tickets;
        tc.max_batch_rows = config.max_batch_rows;
        tc.fill_chunk_bytes = config.fill_chunk_bytes;
        auto table = Table::create(tc, backend);
        if (!table) return std::unexpected(table.error());
        t->tables_.push_back(std::move(*table));
        t->scratch_.emplace_back(config.max_batch_rows);
        at += cls.budget_rows * cls.width;
    }
    return t;
}

inline Status SizeClassedTable::prefetch(std::span<const std::uint64_t> rows) noexcept {
    if (rows.size() > config_.max_batch_rows) return Status::batch_too_large;
    for (std::uint32_t c = 0; c < tables_.size(); ++c) {
        std::size_t n = 0;
        for (const std::uint64_t row : rows) {
            if (row >= config_.row_count) return Status::out_of_range;
            if (class_of_[static_cast<std::size_t>(row)] == c) scratch_[c][n++] = local_of_[static_cast<std::size_t>(row)];
        }
        if (n == 0) continue;
        // A dropped ticket relinquishes only its record; the fills continue (Table::prefetch).
        if (const auto ticket = tables_[c]->prefetch(std::span(scratch_[c]).first(n)); !ticket) return ticket.error();
    }
    return Status::ok;
}

inline std::expected<std::size_t, Status> SizeClassedTable::resolve_into(std::span<const std::uint64_t> rows,
                                                                         std::span<RowLease> out) noexcept {
    if (rows.empty()) return std::unexpected(Status::empty_range);
    if (out.size() < rows.size()) return std::unexpected(Status::invalid_argument);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const std::uint64_t row = rows[i];
        if (row >= config_.row_count) {
            for (std::size_t j = 0; j < i; ++j) out[j].reset(); // all-or-nothing, as Table
            return std::unexpected(Status::out_of_range);
        }
        const std::uint64_t local = local_of_[static_cast<std::size_t>(row)];
        const auto resolved = tables_[class_of_[static_cast<std::size_t>(row)]]->resolve_into(
            std::span<const std::uint64_t>(&local, 1), out.subspan(i, 1));
        if (!resolved) {
            for (std::size_t j = 0; j < i; ++j) out[j].reset();
            return std::unexpected(resolved.error());
        }
    }
    return rows.size();
}

inline std::optional<RowLease> SizeClassedTable::try_get(std::uint64_t row_index) noexcept {
    if (row_index >= config_.row_count) return std::nullopt;
    return tables_[class_of_[static_cast<std::size_t>(row_index)]]->try_get(local_of_[static_cast<std::size_t>(row_index)]);
}

inline TableStats SizeClassedTable::stats() const noexcept {
    TableStats sum{};
    for (const auto& table : tables_) {
        const TableStats s = table->stats();
        sum.hits += s.hits;
        sum.misses += s.misses;
        sum.fetches += s.fetches;
        sum.coalesced += s.coalesced;
        sum.conversions += s.conversions;
        sum.codec_failures += s.codec_failures;
        sum.evictions += s.evictions;
        sum.resident += s.resident;
        sum.leased += s.leased;
        sum.failed += s.failed;
    }
    return sum;
}

inline Status SizeClassedTable::drain(Deadline deadline) noexcept {
    for (const auto& table : tables_)
        if (const Status s = table->drain(deadline); s != Status::ok) return s;
    return Status::ok;
}

} // namespace sub0tieredcache
