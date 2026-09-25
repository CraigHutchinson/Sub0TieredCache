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

| Package | Depends on | Deliverable / named consumer | Acceptance |
|---|---|---|---|
| T0 | MemPage M2 contract | Bounded in-memory row cache + reference fixtures | Duplicate/order/bounds, budgets, RowLease lifetime, versions, codec failure; no engine/GPU/network |
| T1 | T0 + MemPage M3 | Local-file row adapter using real lower scheduler | Identical output to T0; tiny RAM forces eviction; cross-chunk contiguous gather, concurrent same-row coalescing, no hot allocations |
| T2 | T1 + MemPage M4 | GPU row representations and codec/event bridge | CPU/native-encoded/CUDA outputs match independent oracles; delayed-event reuse protection; staged path first |
| T3 | T1 | Optional remote mirror | Local HTTP server failure matrix and restart/version tests; real external shard fixture with provenance |
| T4 | T1/T2 + MemPage M5/M6 | Intel or NVIDIA accelerated paths | Same row contract on each qualified backend; fallback telemetry and identical results; no hardware means not qualified |

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
