# Requirements

Normative contract for Sub0Firn. Each requirement below is stated as a testable sentence first, then
explained — the sentence is what an implementation is checked against; the explanation is why it says
that and not something weaker. Sourced from [README.md](README.md)'s scope/API-surface work and the
real, code-grounded trace against Sub0Llm's own n-gram embeddings consumer
(`docs/reference-consumer-sub0llm.md`) — nothing here is asserted without a concrete case behind it.

## R1. A caller supplies row indices already computed

"Sub0Firn receives only `(table_id, row_index)` pairs; it does not derive, hash, or interpret how
`row_index` was computed."

Hash formulas, routing decisions, and vocabulary/id derivation are entirely the caller's concern —
Sub0Llm's own n-gram hash, a MoE router's expert-selection logic, or anything else. Baking any of that
into Sub0Firn would tie a generic tiered-cache engine to one caller's domain, defeating the reason it's a
separate project at all (README §2).

## R2. Only two calls may block on I/O

"`resolve_into` and `wait` are the only calls in the API that may block on I/O. `try_get` must never
block. No other call implicitly triggers a synchronous fetch."

This is the load-bearing requirement the whole design exists to satisfy: a caller with its own
no-heap-allocation or bounded-latency hot-path rule (Sub0Llm's `AGENTS.md` §1 is the motivating real
case) needs a hard guarantee about exactly where I/O can happen, not "usually fast." `try_get`'s
never-blocks guarantee is what makes it the one call safe to place anywhere, including inside a tighter
loop than the rest of the API is built around.

## R3. An explicit resolve step exists for a hot-path caller to insert ahead of compute

"A caller can populate a pre-sized destination buffer with a full set of resolved rows via one call to
`resolve_into`, and thereafter read that buffer with zero further calls into Sub0Firn."

This is the shape a no-heap-allocation, no-branch-on-miss hot path actually needs — resolve everything
first, compute unconditionally after. `docs/reference-consumer-sub0llm.md` §1 traces this against
Sub0Llm's real `forward()` call site to confirm the shape actually fits a real caller, not just a
hypothetical one.

## R4. Rows are immutable between registration and an explicit `invalidate`

"A row resolved into any tier is guaranteed correct as of the table's current `version_tag` until that
table's next `invalidate` call. There is no TTL-based expiry and no background staleness."

The tables Sub0Firn serves are either genuinely frozen (an imported checkpoint) or updated only at
well-defined bulk boundaries a caller signals explicitly (R1's non-goal already rules out per-row writes)
— a silent expiry policy would be solving a write-concurrency problem that doesn't exist here, at the
cost of real unpredictability for the read-only case that's the entire point.

## R5. Concurrent requests for the same row coalesce into one fetch

"Multiple threads calling `prefetch`/`resolve_into` concurrently for the same row must trigger exactly
one real fetch, not one per caller."

Sub0Llm's real training path runs up to ~24 concurrent OMP worker threads (`docs/reference-consumer-sub0llm.md`
§1), each processing an independent window that shares real vocabulary with its neighbors — duplicate
concurrent fetches for the same hot row are a real, expected occurrence at that concurrency level, not an
edge case worth deprioritizing.

## R6. Dtype conversion is Sub0Firn's job, performed once

"A caller receives rows already converted to the dtype it registered the table with. Sub0Firn performs
any on-disk-to-requested-dtype conversion itself, cached converted rather than reconverted on every warm
hit, never pushed back onto the caller as a second pass."

The motivating real table (`Qwen/Qwen3.8-Flash-Next`'s n-gram embeddings) is stored `bf16` on disk; the
motivating real caller computes in `float32`. Leaving conversion to the caller would mean every consumer
re-implements the same dequantization logic, which is exactly the kind of thing a shared library exists
to not duplicate.

## R7. Format-agnostic core; no embedded knowledge of any specific external file format

"Sub0Firn's tiered-cache core does not parse safetensors, GGUF, or any other specific external weight
format. A caller supplies an offset-resolution callback (`row_index -> (shard, byte_offset)`); the core
only ever calls it."

Keeps the library genuinely reusable (README §2) rather than accumulating format-specific branches for
each new caller's own source data. Concretely generalizes `gguf.hpp`'s existing offset-computation logic
in Sub0Llm — this callback contract, not a shared parser, is the reuse boundary.

## R8. Identical public semantics on Linux, macOS, and Windows from the first implementation

"No public API behavior differs by platform. A platform-specific default (a cache directory path,
`mmap` vs. `MapViewOfFile`) is an internal implementation detail behind a uniform contract, never a
documented behavioral difference a caller has to branch on."

Sub0Llm itself is Windows-first today; Sub0Firn deliberately is not — the entire reason it's worth
existing as a separate project is that huge sparse lookup tables are a generic problem (README §2), and a
library that only worked on its first caller's platform wouldn't actually be that.

## R9. Row content is opaque

"Sub0Firn never validates, interprets, or assigns meaning to a row's bytes beyond its declared width and
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

## Non-requirements (explicitly out of scope, see README §1b for the full list)

Not repeated here in full — R9 covers the header; README §1b is the normative non-goals list (general
vector search, training-time writes, general-purpose variable-size caching, distributed consensus,
multi-writer conflict resolution). A change proposing to add any of these needs to argue why the
project's scope should change, not just that the feature would be convenient.
