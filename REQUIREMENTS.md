# Requirements

Normative contract for Sub0TieredCache. Each requirement below is stated as a testable sentence first, then
explained — the sentence is what an implementation is checked against; the explanation is why it says
that and not something weaker. Sourced from [README.md](README.md)'s scope/API-surface work and the
real, code-grounded trace against Sub0Llm's own n-gram embeddings consumer
(`docs/reference-consumer-sub0llm.md`) — nothing here is asserted without a concrete case behind it.

## R1. A caller supplies row indices already computed

"Sub0TieredCache receives only `(table_id, row_index)` pairs; it does not derive, hash, or interpret how
`row_index` was computed."

Hash formulas, routing decisions, and vocabulary/id derivation are entirely the caller's concern —
Sub0Llm's own n-gram hash, a MoE router's expert-selection logic, or anything else. Baking any of that
into Sub0TieredCache would tie a generic tiered-cache engine to one caller's domain, defeating the reason it's a
separate project at all (README §2).

## R2. Only resolve and wait may block on I/O during steady-state use

"`resolve_into` and `wait` are the only calls in the API that may block on I/O. `try_get` must never
block. No other steady-state call implicitly triggers a synchronous fetch. Registration and explicit
shutdown/drain are administrative and may block; lease release may not hide a drain."

This is the load-bearing requirement the whole design exists to satisfy: a caller with its own
no-heap-allocation or bounded-latency hot-path rule (Sub0Llm's `AGENTS.md` §1 is the motivating real
case) needs a hard guarantee about exactly where I/O can happen, not "usually fast." `try_get`'s
never-blocks guarantee is what makes it the one call safe to place anywhere, including inside a tighter
loop than the rest of the API is built around.

## R3. An explicit resolve step exists for a hot-path caller to insert ahead of compute

"A caller can populate a pre-sized destination buffer with a full set of resolved rows via one call to
`resolve_into`, and thereafter read that buffer with zero further calls into Sub0TieredCache."

This is the shape a no-heap-allocation, no-branch-on-miss hot path actually needs — resolve everything
first, compute after successful completion. The [consumer audit](docs/reference-consumer-sub0llm.md)
distinguishes proposed frozen-table integration from current mutable model parameters.

## R4. Row values belong to immutable source generations

A request captures the current generation. Invalidation publishes a new immutable source generation for
new requests; old leases remain valid for their captured generation until released. Old completions
cannot publish into the new generation. Old source handles and memory drain before destruction. A
version tag does not make concurrent mutation of a source file safe; provide a snapshot or quiesce writers.
If two live generations exceed the budget, report busy/exhaustion rather than silently overcommit.

## R5. Concurrent requests for the same row coalesce into one fetch

"Multiple threads calling `prefetch`/`resolve_into` concurrently for the same row must trigger one live fetch/conversion per
source-generation, row, representation and destination domain. Requests after eviction may fetch again;
distinct device destinations are not the same cache key."

Concurrent frozen-table consumers can request repeated row IDs; standalone tests exercise this
without assuming a particular engine thread count or migrating mutable training parameters.

## R6. The cache owns representation conversion scheduling and reuse

Registration declares source and output widths, dtype/encoding, layout, device/domain and any codec.
A successful resolve returns the requested representation; warm hits do not reconvert. Unsupported
conversion fails explicitly. Generic scalar codecs may be built in; model-specific quantization/layout
kernels are supplied by the consumer, never implemented by the format-agnostic core. Identity/native-
encoded rows are supported so a quantized consumer is not forced through float32. Input/output buffers
are distinct and live through codec completion. The cache owns publication and reuse, not model math.

## R7. Format-agnostic core; no embedded knowledge of any specific external file format

"Sub0TieredCache's tiered-cache core does not parse safetensors, GGUF, or any other specific external weight
format. A caller supplies an offset-resolution callback (`row_index -> (shard, byte_offset)`); the core
only ever calls it."

Keeps the library genuinely reusable (README §2) rather than accumulating format-specific branches for
each new caller's own source data. Concretely generalizes `gguf.hpp`'s existing offset-computation logic
in Sub0Llm — this callback contract, not a shared parser, is the reuse boundary.

## R8. Identical public semantics on Linux, macOS, and Windows from the first implementation

"The baseline host API has identical semantics on Linux, macOS and Windows. Optional accelerator
backends expose capabilities and explicit unsupported results; NVIDIA GDS need not exist on every OS.
A platform-specific default (a cache directory path,
`mmap` vs. `MapViewOfFile`) is an internal implementation detail behind a uniform contract, without weakening the baseline correctness contract. Choosing a supported accelerator
path is explicit; a compatible fallback must preserve output/lifetime semantics and report its path."

Sub0Llm itself is Windows-first today; Sub0TieredCache deliberately is not — the entire reason it's worth
existing as a separate project is that huge sparse lookup tables are a generic problem (README §2), and a
library that only worked on its first caller's platform wouldn't actually be that.

## R9. Row content is opaque

"Sub0TieredCache never validates, interprets, or assigns meaning to a row's bytes beyond its declared width and
dtype. It is not a vector database (no similarity search), not a training framework (no gradients, no
optimizer), and not a model-serving system (no concept of a 'layer' or 'checkpoint')."

Every one of these is a real, separately-solved problem with its own well-established tools; scope creep
into any of them would trade a small, sharply-useful library for a large, weakly-useful one. See README
§1b for the full non-goals list this requirement summarizes.

## R10. The mechanism's own health is observable

"Per-tier hit counts, resident row counts, and resident bytes must be retrievable via `stats`, without a
caller needing to instrument its own call sites to reconstruct that information."

Without this, "is the cache actually working" becomes a guess. Matches the same instinct behind Sub0Log's
own "a drop is never silent" requirement (a sibling project's real, already-written requirement,
independently arriving at the same principle: a mechanism whose own effectiveness can't be observed
can't be trusted or tuned).

## R11. Zero-copy access returns an owning lease

`try_get` returns an optional move-only RowLease, never an unowned row_view. Its storage remains valid
until release, including asynchronous compute. It performs no I/O/conversion and reports miss/contention
promptly. A prefetch ticket or successful wait is not a row pin. Device rows expose domain-aware handles,
not CPU spans; reuse waits for the last consumer event without blocking ordinary release.

## R12. Memory, request and scratch budgets are explicit

Registration fixes per-tier capacity, pinned staging, conversion/gather scratch and request/event limits.
No steady-state request allocates fresh bulk storage or silently expands a pool. Exhaustion is reported;
batch-too-large cannot deadlock on its own pins. Deployment planning counts lower raw pools, output
representations and driver headroom separately. Hard managed limits do not imply a total-RSS bound.

## R13. Local byte transport is delegated to Sub0MemPage

The cache supplies extents and reserved storage to a pinned Sub0MemPage dependency; it does not fork an
OS/CUDA/cuFile/SYCL transfer engine. Row policy has one owner. Raw-cache eviction cannot invalidate a
published row without its backing lease protecting it. Remote HTTP and versioned mirror publication
remain in this project; the lower layer sees immutable local sources. Standalone tests never require Llm.

## R14. Failures and publication are explicit

No partial read/conversion is published as a complete row; stale completions cannot corrupt a new
version. Batched output preserves order/duplicates. The initial batch contract publishes all-or-nothing
success; callers must not read failed output even if some writes occurred. Cancellation/timeout retains
claims until writers drain. No implicit zero-row substitution, retry into live output or hidden fallback.

## Deferred: a table's addressable row range MAY grow (named now, not required for v1)

Raised directly: is R4's "frozen" a hard invariant of the whole design, or an artifact of v1's
simplicity that a later mode could relax? Worked through concretely — the answer is neither, quite: R4
itself (a resolved row's *value* never silently changes) stays a hard requirement forever, because it's
what makes caching correct. What's actually optional is whether a table has exactly one source or more
than one.

**DR1.** "A logical `table_handle` MAY resolve through more than one registered source, layered with a
defined precedence (an `overlay` shadowing a `base`) — the base stays genuinely frozen per R4 unchanged;
the overlay is append-only under a single administrative writer, and new entries may be added to it
without invalidating or re-fetching anything already resolved from the base." Not required for v1 (which
implements single-source tables only), but the row-addressing model must not preclude it later — a
`table_handle` should be free to later resolve through more than one registered source without a
wire-format or on-disk-cache-format change forced by the addition.

**Why this is the right shape, not just a plausible one**: the real motivating case is layering a small,
genuinely local, incrementally-growable vocabulary extension on top of the huge frozen `Qwen/Qwen3.8-Flash-Next`
n-gram table (`docs/reference-consumer-sub0llm.md`) — new project-specific n-grams a deployment wants
without the 320-million-row base ever needing to change, grow, or be re-fetched. Two real, checked
precedents, pointing at the same shape from different directions (`docs/prior-art.md` has the full
citations):

- **HugeCTR's dynamic embedding table** (`embedding_vec_size = -1`) genuinely grows at runtime — a new
  key seen during training gets a freshly-initialized row inserted on the spot. This is the closest real
  precedent for *growth itself*, but it grows via gradient training, which is squarely outside Sub0TieredCache's
  own scope (R9: not a training framework) — cited as evidence the pattern is real and shipped, not as a
  mechanism to reuse directly.
- **HugeCTR's own Hierarchical Parameter Server (the *inference*-time counterpart of the same project)
  does NOT auto-insert a missing key** — it returns a configured default value instead. This is the
  closer analog to Sub0TieredCache's actual role (serving, not training) and is worth taking seriously as a
  reason DR1 should stay opt-in rather than default behavior: an inference-time system silently
  fabricating new "learned" content for an unrecognized key is a correctness trap, not a convenience.
- **Overlay/union-filesystem composition** (the well-established OS pattern — a read-only lower layer
  plus a writable upper layer, composited transparently at lookup time) is the actual architectural fit
  here, not the training-time analog: DR1's `base`/`overlay` split is the same shape, applied to rows
  instead of files.

## Non-requirements (explicitly out of scope, see README §1b for the full list)

Not repeated here in full — R9 covers the header; README §1b is the normative non-goals list (general
vector search, training-time writes, general-purpose variable-size caching, distributed consensus,
multi-writer conflict resolution). A change proposing to add any of these needs to argue why the
project's scope should change, not just that the feature would be convenient.
