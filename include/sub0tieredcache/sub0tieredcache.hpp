#pragma once

/** @file sub0tieredcache.hpp
 *  @brief Entry points for the tiered row-cache contract. See REQUIREMENTS.md for the contract
 *         (R1-R14) and README.md sec 3 for the full call signatures this header's growth follows.
 *
 *  STATUS: T0 (docs/integration-plan.md's delivery table) -- a bounded in-memory row cache over
 *  Sub0MemPage's M2 explicit-destination transport. Implemented in row_cache.hpp (RowCache/Table/
 *  RowLease/PrefetchTicket) and codec.hpp (the Codec interface and the built-in bf16->f32 widening).
 *  No GPU, no network, no disk tier and no compute engine yet -- those are T1-T4 (integration-plan.md).
 *
 *  README.md sec 3 contract -> T0 implementation:
 *
 *    register_table          -> RowCache::register_table(TableConfig, FillBackendRef) -> TableHandle
 *    prefetch / wait          -> Table::prefetch -> PrefetchTicket; Table::wait -> WaitOutcome
 *    resolve_into              -> Table::resolve_into(rows, out) -- all-or-nothing, blocking (R2, R3)
 *    try_get                   -> Table::try_get(row_index) -> optional<RowLease>, never blocks (R11)
 *    stats                     -> Table::stats() -> TableStats (R10)
 *    invalidate                -> Table::invalidate(new_generation, new_source, new_source_bytes,
 *                                 new_resolver = {}) -> Status (R4; carries a new immutable source
 *                                 snapshot, not just a number -- see row_cache.hpp's file comment)
 *
 *  Real local-file transport (T1), GPU representations (T2) and a remote mirror (T3) are not
 *  implemented here; T0's transport is whatever sub0mempage::FillBackendRef the registering caller
 *  supplies (a deterministic fake in this project's own tests -- tests/fake_backend.hpp). Each Table
 *  runs one internal completion-worker thread (started in create(), joined in the destructor) so a
 *  dropped prefetch ticket's fill still reaches a terminal state without anyone calling wait().
 */

#include "sub0tieredcache/codec.hpp"
#include "sub0tieredcache/row_cache.hpp"
#include "sub0tieredcache/status.hpp"

namespace sub0tieredcache {

// Umbrella namespace only -- all types are defined in the headers included above.

} // namespace sub0tieredcache
