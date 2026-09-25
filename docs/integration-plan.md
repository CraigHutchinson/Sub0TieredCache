# Tiered cache responsibility and implementation plan

Revision S1, 2026-09-25. Design only. This supersedes the old independent Stage 0–5 ordering in
[tiered-storage-design.md](tiered-storage-design.md). The dependency is Sub0TieredCache -> Sub0MemPage;
Sub0Llm is an optional downstream integration test consumer, never a build dependency of this library.
[Stack acceptance](../../Sub0Llm/docs/STORAGE_STACK_PLAN.md) coordinates the three repositories.

## What this layer owns

- Logical table/row identity, bounds, immutable source generations and version transitions.
- Row-to-source-extent resolution via caller-supplied, prevalidated adapters; no GGUF/safetensors parser.
- Per-tier admission, eviction, deduplication and converted-representation cache keys.
- Source/output widths, representation identity and conversion scheduling. Built-in generic scalar
  conversion can live here; model quantization/layout kernels stay in Sub0Llm and are registered codecs.
  Identity-copy/native-encoded representations are valid. No compulsory float32 expansion.
- Local/remote tier selection, HTTP range validation and versioned local mirror publication. Download
  orchestration belongs here; local-file reads and host/device transfer mechanics belong to Sub0MemPage.
- Owning or borrowing explicitly budgeted cache allocations at startup; registering them below and
  propagating leases/errors. Sub0MemPage never allocates these bulk buffers on the cache's behalf.

Sub0Llm owns row IDs/routing, external-file parsing, model semantics, math, precision choices and the
compute schedule. Sub0MemPage owns transfers and transfer safety, not row replacement decisions.

## Output and cache contract

A registration specifies source representation/width, requested output representation/width, layout,
source generation, target memory domain, budgets and optional codec. Resolve/gather callbacks execute
outside compute hot loops and must use bounded preallocated storage; specify their thread-safety at
registration. A codec produces bytes only into an exclusively reserved output and reports failure or
completion. No codec may retain an unowned source pointer. Unsupported conversion fails at registration.

Key a resident row by table identity, source generation, row index, representation/layout and target
device/context. A CUDA-device row is not a host row hit; matching widths alone do not imply matching
representations. Converted rows occupy different storage from encoded inputs unless an independently
qualified in-place codec explicitly permits aliasing (initial implementation forbids it).

`resolve_into` copies/gathers into a pre-sized caller destination in original request order, including
duplicates. Host completion means readable bytes; device completion means validated data ready in the
specified context, not CPU dereferenceability. All outputs are invalid on batch failure unless an
explicit per-row result marks success; source/output leases remain live until pending writers drain.
First implement the all-or-nothing published-result form using bounded scratch/reservations.

`try_get` returns an optional move-only RowLease over an already-ready representation, not an unowned
row_view. It does no I/O or conversion and fails promptly on miss/contention. `wait` observes a ticket;
it does not pin a row. The lease protects backing storage until release, including any GPU event that
still consumes it. Device access is a typed/domain-tagged handle, not a host span.

For identity transfers, reserve the row destination and use Sub0MemPage's explicit-destination operation.
For conversion, hold a lower raw-byte lease, convert into the cache's distinct output reservation, publish
only on success, then release raw input after the codec's last event. One allocation has one replacement
owner. Lower raw-cache eviction cannot invalidate a published converted row; an identity row borrowing
lower storage retains its lower lease. Advisory eviction notifications are never a validity mechanism.

## Versioning and remote tier

Invalidation starts a new source generation; it does not mutate bytes being read. New requests bind to
the new generation; old leases may finish against their captured generation. Late old completions cannot
publish into the new cache. Old source handles and allocations retire after drain. If budgets cannot
hold both generations, reject/defer the transition explicitly; never exceed the cap. The caller provides
a new immutable source snapshot or quiesces writers; a version string cannot make mutable files safe.

Remote HTTP is optional and tested with a local range server first. Validate range status/length,
source-validator consistency, bounds, retries and truncated responses. Publish versioned local files
atomically only after validation. Cancellation must not expose partially downloaded cache files. Disk
cache identity includes representation/layout and source generation; no path chosen by Sub0Llm leaks
into generic defaults. A remote mirror is not automatically a qualified GPUDirect filesystem.

Training updates to engine-owned tensors stay outside this frozen-row cache. No silent zero row or
missing-signal substitution on miss; blocking/error behavior is explicit. Any approximation policy is
owned and validated by Sub0Llm, not inferred by this cache.

## Delivery packages and consumers

| Package | Depends on | Deliverable / named consumer | Acceptance | Status |
|---|---|---|---|---|
| T0 | MemPage M2 contract | Bounded in-memory row cache + reference fixtures | Duplicate/order/bounds, budgets, RowLease lifetime, versions, codec failure; no engine/GPU/network | **Delivered** |
| T1 | T0 + MemPage M3 | Local-file row adapter using real lower scheduler | Identical output to T0; tiny RAM forces eviction; cross-chunk contiguous gather, concurrent same-row coalescing, no hot allocations | **Delivered** (`tests/local_file_tests.cpp`, `local_file_source.hpp`); sharded sources (`ShardSource`/`RowLocation`) also landed here, ahead of the row above naming it explicitly. A **real external shard fixture** (a genuine multi-shard third-party format, not this repo's own synthetic flat/interleaved test files) is still open. |
| T2 | T1 + MemPage M4 | GPU row representations and codec/event bridge | CPU/native-encoded/CUDA outputs match independent oracles; delayed-event reuse protection; staged path first | Not started |
| T3 | T1 | Optional remote mirror | Local HTTP server failure matrix and restart/version tests; real external shard fixture with provenance | **Standalone mirror delivered** (`include/sub0tieredcache/remote/`, `docs/remote-mirror.md`: versioned local chunk store over a built-in plain-HTTP client) **and integrated** (`remote/mirror_backend.hpp`, `tests/remote_e2e_tests.cpp`: network-then-disk-restart and a validator-mismatch-fails-explicitly case against the local HTTP test server). Open: HTTPS (the built-in client is plain HTTP only; TLS is left to a caller-supplied `RangeTransportRef`), and the same real external shard fixture T1 still needs. |
| T4 | T1/T2 + MemPage M5/M6 | Intel or NVIDIA accelerated paths | Same row contract on each qualified backend; fallback telemetry and identical results; no hardware means not qualified | Not started |

T0 can be authored while M2 stabilizes using a fake transport, but T1 cannot pass with only a fake.
Sub0Llm's S0 fixture/adapter definition happens now; S1 local-row integration follows T1 rather than
waiting for GPU or remote support. Release each usable vertical slice; do not build all backends first.

## Independent test strategy

Default CTest needs C++23 and the pinned Sub0MemPage revision only, never an engine checkout or network.
Use deterministic tiny tables of known bytes and widths around chunk/alignment boundaries, plus a
bounded multithreaded repeated-row workload. Compare outputs to direct row slicing + an independently
implemented conversion oracle; assert counts for fetch/conversion reuse. Verify bit-exact bf16-to-f32
widening (including special-value bit patterns), identity encoded rows and codec-defined error tolerances
where conversion is lossy. Test both small and realistic row/batch sizes without requiring a huge model.

GPU suites are explicitly opt-in and record device/context/backend qualification. Test wrong-device
hits, out-of-order events, partial I/O, budget exhaustion and invalidation with an old lease outstanding.
Windows/Linux/macOS host semantics must pass before baseline portability is claimed; optional GDS is
native-Linux qualification, not a requirement imposed on macOS or Windows users.

Each gate reports lower/upper repository revisions, fixture hashes, test/skip counts, seed, compiler and
backend facts. No shared live file is mutated by two test processes. End-to-end traces feed row size,
working-set/reuse distance and concurrency needs back into T0/T1 fixtures, without importing model code.

### What has actually been verified where (T1/T3 pass)

This container is Linux-only (no macOS, no native Windows), so "Windows/Linux/macOS host semantics must
pass" above is checked as follows, honestly:

- **Linux**: GCC 13 Release `-Wall -Wextra -Wpedantic -Werror`, Clang 18 (via `-stdlib=libc++` --
  system `libstdc++` under this Clang cannot compile `std::expected` at all, a real toolchain gap, not a
  code defect; see this file's own Sub0MemPage contract-feedback note), ASan+UBSan, and TSan (run
  multiple times) are all green for every test executable, including the real-file (`local_file_tests`)
  and real-network (`remote_e2e_tests`) suites.
- **Windows**: not run on real Windows or under an emulator; instead cross-compiled with
  `x86_64-w64-mingw32-g++-posix -std=c++23 -Wall -Wextra -Wpedantic -Werror` and executed under Wine
  (`wine <test>.exe`), one test binary at a time, linked against `ws2_32` for the sockets the remote
  transport/HTTP test server use. This exercises MinGW's libstdc++ (a different standard library build
  than this container's native GCC) and Wine's Win32 socket/thread emulation, but it is not MSVC and not
  real Win32 -- treat it as evidence of portability, not as the Windows CI gate itself (that already runs
  in GitHub Actions per `.github/workflows/ci.yml`'s `build-windows` job, which this container cannot
  reach).
- **macOS**: entirely unverified in this environment. No macOS machine or emulator was available; the
  code avoids any macOS-specific path (the local-file and remote-mirror backends are POSIX-portable, no
  Linux-only syscalls), but that is a design intent, not a substitute for actually running it there.

## Contract feedback to Sub0MemPage (T0)

Found while implementing T0 against the pinned Sub0MemPage revision (README.md records the exact commit).
None of these blocked T0 -- each was worked around locally, as noted -- but they are real gaps in the M2
contract as shipped, not just style preferences, so they are recorded here rather than silently patched
around forever.

- ~~No installed/exported test-support target for the deterministic fake backend.~~ **RESOLVED** (T1,
  Sub0MemPage commit `69227db3cf7c37a6908e46cfbd71dda14c926c20`): Sub0MemPage now exports
  `Sub0MemPage::testing`, an INTERFACE target carrying `<sub0mempage/testing/fake_backend.hpp>`. T0's
  hand-maintained copy in `tests/fake_backend.hpp` has been deleted; every test now links
  `Sub0MemPage::testing` and includes the real header directly.
- **A blocking scratch-pool acquisition inside `TransferSet::submit`'s critical section would deadlock
  a caller's own single global lock.** Not a Sub0MemPage bug -- `TransferSet` itself has no such
  blocking call -- but T0's own conversion path needed a bounded raw-byte staging pool *around*
  `TransferSet`, and the natural first design (a blocking acquire, mirroring how `SlotPool::resolve`
  itself blocks) deadlocks against T0's own single-mutex admission path: a finisher thread can only free
  a scratch slot by re-acquiring the same lock the blocked acquirer is holding. T0's own scratch pool is
  therefore non-blocking (`Table::acquire_scratch` returns `pool_exhausted` immediately rather than
  waiting) -- see `include/sub0tieredcache/row_cache.hpp`'s `acquire_scratch`/`start_fill_locked`
  comments. This is recorded here because a future Sub0MemPage feature that lets a caller register a
  *second*, smaller bounded pool alongside a `TransferSet` (for exactly this "raw stage then transform"
  shape) would let T1 do this without T0 inventing its own parallel pool and mutex.
- **No cross-instance guard yet that one allocation is not registered with both a `SlotPool` and a
  `TransferSet` (R18)**, already flagged as deferred in Sub0MemPage's own `slot_pool.hpp`. T0 itself now
  deliberately relies on the *TransferSet<->TransferSet* variant of the same gap: `invalidate()` (R4)
  builds a brand-new `TransferSet` per binding over the SAME `output_storage`/`scratch_storage` spans as
  the binding it supersedes, so the current and a still-draining "retiring" binding are two live
  `TransferSet` objects registered over overlapping destination memory at once. This is safe only because
  `Table` itself is the sole allocator of which slot (and thus which destination byte range) is live at
  any moment -- a byte range is never resubmitted-into while any earlier claim against it (old or new
  binding) is still outstanding, since a Filling slot is never picked as an eviction victim regardless of
  which binding started its fetch. Sub0MemPage has no way to check this invariant itself today; a future
  cross-instance registration guard (R18) would need to accept "two TransferSets, one destination span,
  never truly concurrent at the byte-range level" as a legitimate pattern, not just reject it outright.
- **Ubuntu's `clang` + system `libstdc++` cannot compile `<expected>` at all.** Confirmed against
  Sub0MemPage's own headers, not something specific to this project's code: Clang 18 reports
  `__cpp_concepts` as the C++20 TS value `201907L` rather than `202002L`, and libstdc++ 13's `<expected>`
  gates itself off entirely below `202002L`. Not a defect to fix in either library (it is a real
  clang/libstdc++ version-interop gap); both projects' CI now builds their `clang` job against `libc++`
  instead (`-stdlib=libc++`, `libc++-18-dev`/`libc++abi-18-dev` installed first) rather than silently
  running a `clang` job that only compiles the parts of the codebase that happen not to touch
  `std::expected`.
