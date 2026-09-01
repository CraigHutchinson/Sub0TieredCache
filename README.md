# Sub0Firn — a tiered cache for huge frozen sparse-lookup tables

Status: **SPEC / REQUIREMENTS DRAFT.** Written to be handed to whoever bootstraps the standalone
repository — no code exists yet, in either Sub0Llm or a separate repo. This document is the
README-shaped spec for that repository; the design reasoning behind why it should exist as a separate
project, and how it reconciles with Sub0Llm's own engine constraints, lives in the companion doc,
[DESIGN_RATIONALE.md](DESIGN_RATIONALE.md) — read that first for the "why," this
doc is the "what."

**Name**: **Sub0Firn**. *Firn* is the glaciology term for the compacted, intermediate layer of snow
between fresh powder and solid glacial ice — a real, load-bearing metaphor here, not decoration: a
Sub0Firn cache tier is exactly that intermediate, compacted, partially-settled layer sitting between a
cold, deep, expensive-to-reach source (the "ice" — the full frozen table on disk or remote storage) and
the hot, fast, small working surface (the "fresh snow" — VRAM/registers). Keeps the `Sub0` lineage
naming (`sub0::` is this engine's own C++ namespace) while being clearly its own project, not a Sub0Llm
submodule.

- **Repository**: `sub0firn` (matches the crate/package-name convention most build systems expect —
  lowercase, no separator).
- **C++ namespace**: `sub0firn::` (lowercase, unnested from `sub0::` — a sibling project, not a nested
  component of it; Sub0Llm's own consuming code lives in `sub0::` and simply depends on `sub0firn::`
  types the way it might depend on any other external library).
- **Library target name**: `libsub0firn` (the conventional Unix `lib`-prefixed static/shared library
  name; the CMake target itself would be `sub0firn`).

## 1. Scope

Sub0Firn serves **fixed-width rows from huge, sparsely-accessed, effectively-frozen lookup tables**
under a real memory budget — think "one embedding table with billions of rows, a few kilobytes of
working set touched per request, and nowhere near enough RAM or VRAM to hold it all." It answers exactly
one question, well: *given a table and a row index, get me that row's bytes, as fast as this machine's
actual memory hierarchy allows, without ever requiring the whole table to be resident anywhere.*

It exists because this shape of problem is genuinely generic, not specific to language-model n-gram
tables or to Sub0Llm: any application holding one or more huge, mostly-static, sparsely-touched
fixed-row-width tables — embedding tables (recommendation systems, LLM lookup tables), feature stores,
precomputed lookup/cache tables for any domain — hits the identical wall: too big for RAM, accessed too
sparsely and non-locally for the OS page cache alone to reliably help, updated rarely enough that a
write-optimized database is solving a problem that doesn't exist here.

### 1a. In scope

- **Row-granular random-access reads** from a table addressed by `(table_id, row_index)`, where
  `row_index` is an arbitrary, caller-computed integer (a hash bucket, a vocabulary id, a feature key —
  Sub0Firn does not care how it was derived).
- **A tiered cache hierarchy** (in-process hot cache → RAM working-set cache → local disk/NVMe cache →
  a pluggable remote/cold source) that a caller configures per deployment, not per call.
- **Two access shapes**, both first-class: a **precomputed working-set** mode (the caller already knows,
  or can cheaply compute, the exact — or a superset — set of rows a workload will ever touch, and wants
  it warmed once, ahead of time) and a **reactive** mode (rows are only known as they're requested, and
  the cache must adapt via an eviction policy).
- **An explicit, synchronous resolve step** a caller can insert into its own hot path's *setup* phase —
  never something that happens implicitly mid-computation — so a caller with its own no-heap-allocation
  or bounded-latency hot-path requirement can use Sub0Firn without violating it (see §3's concurrency
  model).
- **Pluggable table sources**: a local flat file, a local set of externally-formatted shard files (e.g.
  safetensors) with a caller-supplied offset-resolution callback, or a remote HTTP(S) Range-request
  source — see §3's source-descriptor interface.
- **Portability**: Linux, macOS, and Windows as first-class targets from day one — the *design*, not
  necessarily the first implementation, must not assume a single platform's filesystem/cache-directory
  conventions (contrast with Sub0Llm itself, which is Windows-first today; Sub0Firn is not).

### 1b. Explicitly out of scope (non-goals)

- **Not a general vector database.** No similarity search, no approximate nearest-neighbor index, no
  distance metrics. A row is an opaque fixed-width byte blob to Sub0Firn; if a caller wants ANN search
  over the same data, that's a different (and welcome to be built on top) tool.
- **Not a training framework.** Sub0Firn never computes gradients, never updates a row's value as part
  of an optimization loop. It may support row *replacement* at defined update boundaries (§3), but that
  is a bulk/administrative operation, not a training-time write path.
- **Not a general-purpose caching library.** It is deliberately scoped to fixed-row-width,
  huge-cardinality, sparse-access tables — not a drop-in replacement for `memcached`/`redis`-shaped
  general key-value caching, and not competing with those for variable-size or write-heavy workloads.
- **Not responsible for computing which rows are needed.** Hash formulas, routing decisions, and
  vocabulary/id derivation are entirely the caller's concern (Sub0Llm's own n-gram hash formula, or a
  MoE router's expert-selection logic, or anything else) — Sub0Firn only ever receives already-computed
  row indices.
- **Not a distributed-consensus or multi-writer-conflict-resolution system.** The tables Sub0Firn serves
  are either genuinely frozen (an imported checkpoint) or updated only at well-defined bulk boundaries
  (e.g. a training checkpoint) — never concurrently mutated by multiple writers mid-flight. Single-writer,
  many-reader is the entire concurrency model this project needs to get right (§3); anything requiring
  real multi-writer conflict resolution is out of scope.
- **Not a model file format, and not a model-serving system.** Sub0Firn does not know what a "layer" or
  a "checkpoint" is. It serves rows; what those rows mean is entirely up to the caller.

## 2. What would make this broadly useful beyond Sub0Llm

The problem Sub0Firn solves recurs anywhere a sparse lookup table has outgrown its host's memory —
stated generically because that genericity is the actual argument for spinning this out as its own
project rather than keeping it Sub0Llm-internal:

- **Any large frozen embedding table an inference/distillation pipeline wants to reuse without re-hosting
  it fully resident** — the motivating case here (a real, huge n-gram embedding table from an
  open-weight model release), but structurally identical to reusing any other released model's huge
  embedding/lookup component.
- **Recommendation-system-style embedding tables** — the exact problem shape the prior-art research
  (`DESIGN_RATIONALE.md` §1) is drawn from (HugeCTR, Bandana, DLRM hot/cold splitting, the
  frequency-aware GPU cache). Sub0Firn is not a recommendation-system library, but the underlying
  row-serving problem is the same one those systems solve, minus everything about online training that
  they also need to handle and Sub0Firn deliberately does not.
- **Sparse mixture-of-experts weight serving** — MoE-Infinity/FlashMoe (`DESIGN_RATIONALE.md`
  §1) solve a structurally identical problem (huge sparse table, tiny active subset per request,
  resource-constrained hardware) for expert weights rather than embedding rows; nothing in Sub0Firn's
  design is embedding-specific, so the same library could plausibly serve MoE expert weight blocks too,
  if a caller shaped the request that way.
- **Any precomputed feature/cache table too large to keep resident** in a resource-constrained deployment
  (edge devices, personal workstations, anywhere the "63GB RAM, 8GB VRAM" constraint that motivated this
  project generalizes to "less RAM than the data").

## 3. API surface

Given as an engine-agnostic contract, not C++ syntax — an implementation should render this faithfully
into whatever binding surface it exposes (a C++ header-only client is the first target, per §5, but the
contract itself is language-neutral so a Python/Rust binding later is a faithful port, not a redesign).

```
register_table(table_id, row_width_bytes, row_dtype, source_descriptor, version_tag) -> table_handle
    // Registers a table this process will read from. `source_descriptor` names where the table's rows
    // physically live (see below); Sub0Firn does not eagerly read anything at registration time beyond
    // whatever metadata the source needs (e.g. a safetensors shard index).
    // `version_tag` is an opaque caller-supplied identifier (a content hash, a checkpoint step number)
    // used only for staleness detection (see "Consistency" below) — Sub0Firn never interprets it.

prefetch(table_handle, row_indices[]) -> prefetch_ticket
    // Asynchronous. Kicks off resolution of the named rows into the fast tiers, in the background.
    // Returns immediately. Never blocks the caller's thread.

wait(prefetch_ticket)
    // Blocks the calling thread until every row named by that ticket's prefetch() call is resolved
    // into a tier at least as fast as the RAM working-set tier. This is the caller's explicit
    // "resolve pass" barrier — the ONLY place in this contract a caller should expect to block on I/O.

resolve_into(table_handle, row_indices[], dest_buffer)
    // Synchronous. Caller supplies a pre-sized destination buffer (row_indices.size() * row_width_bytes)
    // and gets every named row copied into it, in the given order, blocking until done. The direct
    // building block for a caller's own "resolve pass before the hot loop" (see
    // DESIGN_RATIONALE.md §2a) — call this once, up front, then read dest_buffer from the
    // hot path with zero further calls into Sub0Firn.

try_get(table_handle, row_index) -> optional<row_view>
    // Zero-copy. Returns a view directly into an already-resident tier's backing memory if this row
    // happens to already be resolved there, or nothing if not (never blocks, never triggers I/O).
    // For callers that can tolerate an occasional fall-through to resolve_into rather than needing a
    // hard guarantee ahead of time.

stats(table_handle) -> { per-tier hit counts, per-tier resident row counts, bytes resident per tier }
    // Observability only. No behavior depends on reading this.

invalidate(table_handle, new_version_tag)
    // Administrative. Marks every currently-cached row for this table as stale (see "Consistency"
    // below) and updates the table's version tag. Does not itself evict anything eagerly — subsequent
    // resolve_into/prefetch calls re-fetch from source on next need. This is the bulk-update path
    // (§1b's "defined update boundaries"), not a per-row write API — there is no per-row write API.
```

**`source_descriptor` variants** (one table registration names exactly one; a caller wanting a
multi-source fallback chain composes it by registering the same logical table under one handle backed by
a layered descriptor — a single flat local file, a local shard set + offset-resolution callback, or a
remote HTTP(S) Range source + local disk cache directory):

- `local_flat_file(path)` — one file, rows at `row_index * row_width_bytes`, `mmap`'d.
- `local_sharded(shard_paths[], offset_resolver_callback)` — a caller-supplied callback maps
  `row_index -> (shard_index, byte_offset)`, so Sub0Firn never needs to understand any particular
  external file format (safetensors, GGUF, or anything else) — that translation is the caller's
  responsibility, matching `DESIGN_RATIONALE.md` §2d's point that this generalizes `gguf.hpp`'s
  existing offset-computation logic rather than duplicating it inside Sub0Firn.
- `remote_http_range(base_url, offset_resolver_callback, local_disk_cache_dir)` — same offset-resolution
  shape, but reads go over HTTP Range requests, with `local_disk_cache_dir` as the persistent disk tier
  in front of the network (`DESIGN_RATIONALE.md` §2d's case (b)).

### 3a. Consistency / staleness guarantees

Rows are **immutable between registration and an explicit `invalidate` call.** Sub0Firn assumes a table
is either genuinely frozen (an imported external checkpoint — the common case) or updated only at
well-defined bulk boundaries the caller signals explicitly (a training checkpoint step) — never mutated
row-by-row while cached copies might be in flight. Concretely:

- Within one process, a row resolved into any tier is guaranteed correct as of the table's current
  `version_tag` until that table's next `invalidate` call — no background staleness, no TTL-based
  silent expiry.
- Across processes sharing a **disk-tier** cache (§5's "system/user-level cache" case): the disk format
  must itself carry the `version_tag` per cached entry (or per cache generation), so a second process
  opening the same cache directory can detect and skip entries written under a stale version rather than
  trusting file mtimes alone. The exact mechanism is an implementation decision for the chosen disk-tier
  backend (`DESIGN_RATIONALE.md` §1g's LMDB-leaning discussion), not fixed by this spec, but
  the guarantee itself (a reader can always tell a stale entry from a fresh one) is a hard requirement.
- Sub0Firn makes **no promise about visibility across processes for a row resolved via `prefetch`/
  `resolve_into` but not yet flushed to a persistent tier** — that is purely an in-process, in-memory
  optimization from Sub0Firn's point of view. Cross-process sharing exists only at the disk-tier
  boundary, following whatever consistency model the disk backend itself provides (an MVCC-style
  embedded store gives "readers see the last-committed generation" for free, which is the recommended
  shape — `DESIGN_RATIONALE.md` §1g).

### 3b. Concurrency model

- **Single writer per table, many concurrent lock-free-or-cheaply-locked readers.** This is not a
  simplification made for convenience — §1b already establishes that Sub0Firn's tables are never
  genuinely multi-writer, so the concurrency model should be built around that fact rather than solving
  a harder problem nothing in scope needs.
  - Cache tier population (a `prefetch`/`resolve_into` call filling the RAM or disk tier) is the "write"
    from Sub0Firn's own internal point of view, even though the caller only ever sees it as a read API —
    multiple threads calling `prefetch`/`resolve_into` concurrently for *different* rows must be safe and
    should scale; concurrent calls that happen to name the *same* row should coalesce into one real
    fetch, not duplicate the I/O.
  - `invalidate` (§3, an actual administrative write to the table's identity) is expected to be rare and
    may take a coarser lock — it is explicitly not a hot-path operation.
- **`try_get` must never block.** It either returns a resident row immediately or returns nothing —
  this is the one call in the contract a caller may safely place anywhere, including inside a tighter
  loop than the "resolve pass" the rest of the API is built around, precisely because it can never
  introduce the unbounded-latency branch `DESIGN_RATIONALE.md` §2a's host engine cannot
  tolerate.
- **`wait` blocks only the calling thread**, never a global lock — other threads' independent
  `prefetch`/`resolve_into`/`try_get` calls must proceed unaffected.

## 4. What Sub0Firn stands on — prior art, as this project's own bootstrapping reference

The full research behind this list — direct quotes, confidence tags, and the reasoning for why each one
matters — lives in `DESIGN_RATIONALE.md` §1; restated here in the compressed form a project
README's own "prior art" section would carry, since that document doubles as this project's research
foundation per the task that produced it:

- **NVIDIA Merlin HugeCTR's Hierarchical Parameter Server** — the closest existing system to Sub0Firn's
  own shape (GPU → CPU → SSD/distributed-DB tiers, RocksDB/Redis as concrete disk/distributed backends,
  LRU + a hit-rate-threshold-gated sync/async insert policy). Deprecated as a standalone module since
  v25.03 — read as a caution about scope creep (a fully general, multi-backend, always-on serving layer
  is expensive to keep alive), not as evidence against tiering itself.
- **Bandana (Meta, MLSys 2019)** — the load-bearing citation. Small-DRAM-cache-in-front-of-NVM,
  co-access-aware physical row placement (hypergraph partitioning), and cache-size-by-simulation rather
  than by guess. Sub0Firn's disk-tier design should eventually adopt both techniques once a real access
  trace exists to drive them (not designed yet — see `DESIGN_RATIONALE.md` §6).
- **TT-Rec (Meta, MLSys 2021)** — an orthogonal axis (compress the table) rather than a competing one
  (tier its serving); relevant mainly if Sub0Firn is ever asked to serve a table its *owner* is willing
  to re-factorize, not for serving an already-dense frozen checkpoint as-is.
- **DeepSpeed ZeRO-Infinity's NVMe offload** — a deliberate negative precedent: built for bulk, sequential
  parameter/optimizer-state streaming, not row-sparse random lookup. Cited to rule out "just offload like
  ZeRO does" as a template, not to reuse.
- **Frequency-aware GPU software caching for DLRM (arXiv:2208.05321) and DLRM hot/cold splitting
  (Mahajan et al., VLDB 2022)** — evidence for a frequency-leaning (not plain-recency) eviction policy as
  the better default for this access shape, and a concrete anchor number (~1.5% GPU residency sufficient
  for "decent" throughput on Zipfian recommendation-embedding access) for sizing a hot tier.
- **MoE-Infinity (arXiv:2401.14361) and FlashMoe (2026)** — the most structurally similar recent systems:
  huge sparse table, tiny per-request active subset, resource-constrained personal hardware, disk-tier
  caching, and a shared finding that structure-aware prediction beats plain LRU for this shape of access.
  Confidence on their specific quantitative claims is lower (see the companion doc's confidence tags) —
  cited for the qualitative direction, not the exact numbers.
- **`mmap` + OS page cache** — the honest null hypothesis worth re-litigating for any specific deployment
  before building a bespoke tier: for large-file random access, the kernel's own LRU-based page cache
  already does real work for free. It stops being sufficient specifically when the caller's row size is
  small relative to the page size (this problem's real row is 320–640 bytes against a 4KB page) *and*
  physically adjacent rows are not access-correlated (true here, since row placement is hash-bucket
  order, not co-access order) — Bandana's own contribution is precisely closing that gap.
- **LMDB vs. RocksDB** as the disk-tier backend shape — LMDB's B+tree-over-`mmap` design (zero-copy reads,
  MVCC, no compaction-induced latency spikes) is the better fit for Sub0Firn's read-dominated, rare-bulk-write
  access pattern than RocksDB's LSM-tree (write-optimized, real compaction stalls) — a genuine,
  reasoned disagreement with HugeCTR's own choice of RocksDB, justified by the fact that HugeCTR's tier
  also serves continuously-online-updating models, a write pattern Sub0Firn's tables (§1b) never have.

## 5. Integration seam: how Sub0Llm (or any consumer) would use this

Three real options, weighed rather than picked by default, per the task brief's explicit ask:

1. **Vendored header-only client, in-process, with a small internal background thread pool for
   prefetch/disk/network I/O.** The simplest option: no new deployment shape, no IPC, works identically
   whether the process is a training run or a `sub0llm-gen` decode session. Matches this engine's own
   existing precedent (`gguf.hpp` is exactly this shape — header-only, engine-free, in-process). **This
   is the recommended starting point** (§7's Stage 3 in the companion doc targets exactly this) — every
   staged exit condition through decode integration is achievable this way, with zero new operational
   complexity.
2. **A linked (static or shared) library**, once the disk/remote tiers need genuinely independent
   background threads/async I/O machinery substantial enough that "header-only" stops being the right
   packaging (large template-heavy header-only libraries have real compile-time costs too) — a natural
   graduation point for Sub0Firn once it has real users beyond Sub0Llm, not a day-one requirement.
3. **An out-of-process service, reached over a local Unix domain socket / named pipe / shared memory
   segment.** Only worth its real complexity once "system-level cache shared across independent
   processes" is an actual goal — e.g. two separate Sub0Llm runs (a training job and a concurrent
   `sub0llm-gen` session) sharing one already-warmed RAM/disk cache without each paying its own
   independent warm-up cost, or a genuinely multi-tenant deployment. **Not recommended before that need
   is real** — it adds a lifecycle-management problem (what starts the service, what happens if it's not
   running, versioning the wire protocol) that the vendored-client option simply doesn't have, and
   nothing in the companion doc's staged plan needs it to reach a working decode-integration end state.

**What stays in the Sub0Llm repo vs. what moves to `sub0firn`**: the thin `sub0::` -namespaced glue that
adapts Sub0Firn's `resolve_into`/`try_get` calls into `op_embed`'s expected input shape (tiny,
Sub0Llm-specific, belongs in Sub0Llm) stays put; the tiered cache engine itself — the tier
implementations, the eviction policies, the disk-format code, the HTTP Range client, the
offset-resolver-callback contract generalized beyond any one file format — is exactly what moves to
`sub0firn`. `DESIGN_RATIONALE.md` §2f already draws the matching line for on-disk *location*
conventions (Sub0Llm's own `out/build/<name>/generated/`-relative paths vs. Sub0Firn's own
platform-appropriate user/system cache directory default) — the same split applies to code, not just
paths: nothing in `sub0firn` should ever need to know what `out/build` or `generated/` mean.

## 6. What "system, user, or project-level cache" means as configuration

Reusing the same three-way split most portable caching tools already converge on, since inventing a
fourth would just be a worse version of an already-solved problem:

- **Project-level**: a cache directory scoped to one specific workload/deployment (Sub0Llm's own
  `out/build/<name>/generated/`-relative path is exactly a project-level cache from Sub0Firn's point of
  view) — explicitly passed in via the `local_disk_cache_dir` source-descriptor field (§3), never
  inferred.
- **User-level**: the platform's per-user cache directory (`$XDG_CACHE_HOME` or `~/.cache` on Linux,
  `%LOCALAPPDATA%` on Windows, `~/Library/Caches` on macOS) — Sub0Firn's own default when a caller does
  not specify a `local_disk_cache_dir` explicitly, namespaced under a `sub0firn/<table identity>`
  subdirectory so multiple unrelated callers on the same machine don't collide.
- **System-level**: a machine-wide, multi-user-shared cache directory (e.g. `/var/cache/sub0firn` on
  Linux, `%ProgramData%` on Windows) — never Sub0Firn's own default (writing there typically needs
  elevated privileges and shared-ownership semantics this spec does not want to assume), but a caller may
  point `local_disk_cache_dir` at one explicitly, in which case §3a's per-entry version-tagging
  requirement is what keeps multiple, possibly-different-versioned, concurrent consumers correct.

None of these three is a distinct code path inside Sub0Firn — they are all just different values for the
same `local_disk_cache_dir` configuration, which is the entire point: "system/user/project-level cache"
is a **deployment configuration choice**, not an architectural axis Sub0Firn's own code needs to branch
on.

## 7. Reference consumer: Sub0Llm's n-gram embeddings, checked against the real merged code

Everything above is an abstract contract. This section exists so it isn't *only* abstract: it traces the
API against Sub0Llm's actual, already-merged n-gram embeddings implementation
(`src/backend_cpu.cpp`/`include/sub0/layout.hpp`, `docs/NGRAM_EMBEDDING.md`) — real call sites, real
shapes, real threading, not a hypothetical usage sketch. Where the trace surfaces a genuine gap the API
above doesn't yet resolve, it's flagged as **OPEN**, not quietly designed around.

### 7a. Training path (`Model::forward()`, one call per training window)

Today's code computes `NGRAM_NUM_EMBEDDERS` small tables (a real worked example from
`docs/NGRAM_EMBEDDING.md` §6: `D_MODEL=448, NGRAM_MAX_N=3, K=2` → 4 tables), each looked up
`SEQ_LEN` times (one row per sequence position, up to 512 in a production build) — i.e. every
`forward()` call computes `NGRAM_NUM_EMBEDDERS × SEQ_LEN` row indices (cheaply, in Sub0Llm's own code —
Sub0Firn never sees the hash) and needs that many rows resolved before `op_embed` can run. Mapped
directly onto §3's contract:

```
for e in 0..NGRAM_NUM_EMBEDDERS:
    resolve_into(table_handle[e], ngram_ids[e][0..T], ngram_rows_buf[e])   // pre-sized, reused buffer
// only after every resolve_into returns: op_embed reads ngram_rows_buf[e] as if it were tok_emb
```

`resolve_into` (not `prefetch`/`wait`) is the right call here specifically because `AGENTS.md` §1 (no
heap allocation, no unbounded-latency branch inside a hot compute path) means the *entire* resolve has to
be a synchronous barrier before compute starts, not something compute checks partway through — exactly
what `resolve_into`'s contract already promises (§3).

**Concurrency, with a real number attached**: `train_batch` runs `DEFAULT_THREADS` OMP worker threads in
parallel (`src/backend_cpu.cpp`, `#pragma omp parallel num_threads(DEFAULT_THREADS)`), each processing
its own window independently — so up to `DEFAULT_THREADS` concurrent `forward()` calls, each issuing its
own `NGRAM_NUM_EMBEDDERS` `resolve_into` calls, all in flight together. On the reference development
machine (`[[host-cpu-arrow-lake-hx]]`: 8P+16E cores, no SMT) that's up to ~24 concurrent callers. §3b's
"many concurrent readers, coalesce same-row requests" requirement is not a nice-to-have here — with a
real training corpus, adjacent windows share plenty of vocabulary, so concurrent `resolve_into` calls for
the *same* row across different threads are a real, expected occurrence, not an edge case.

**The strong prefetch opportunity, made concrete**: since a training corpus is fixed and known before
the run starts (mirroring how this engine already precomputes `corpus.tok` out-of-core — see
`DESIGN_RATIONALE.md` §2b), the *entire* set of row indices a training run will ever request, across
every window and every table, is computable in one pass ahead of time, the same shape as the existing
`corpus.tok`/tokenizer-vocab precompute. The recommended pattern for this consumer is therefore: one
`prefetch(table_handle[e], ALL row indices this run will ever touch)` + `wait(...)` per table at startup,
sized so essentially every in-run `resolve_into` call is a warm-tier hit — turning the RAM/disk tiers
from a reactive cache into a precomputed working set, which is the strongest case Bandana's own sizing
technique (`PRIOR_ART.md`) has to work with.

### 7b. Decode path (`Model::forward_one()`, one call per generated token)

Structurally different, and honestly harder — flagged as such rather than glossed over. Each decode step
looks up exactly one row per table (not `T` rows), from a small rolling history of the last
`NGRAM_MAX_N-1` fed token ids (`docs/NGRAM_EMBEDDING.md`'s decode-path mirror of the training hash). The
prompt/generation isn't known in advance the way a training corpus is, so §7a's precomputed-working-set
recipe doesn't apply here — this is squarely the **reactive mode** §1a already names as first-class.

**OPEN — not resolved by this spec as written**: whether a decode-time miss should block
(`resolve_into` for exactly the current step's single row, accepting whatever latency the coldest tier
in play adds — tolerable if the miss rate is low and every tier at least as fast as local disk, since
decode is already a sequential, per-token-latency-bound loop) or fall back to a defined "no signal" value
matching this engine's own existing convention for out-of-vocabulary/persistent-slot ids (`id_tok = 0`,
`docs/NGRAM_EMBEDDING.md`'s persistent-slot guard) rather than stall generation waiting on a cold remote
fetch. **Recommended default for a first implementation**: block via `resolve_into` for the single
current-step row (simplest, correct, and the natural training-corpus prefetch from §7a should make this
rare in practice for any vocabulary the training run actually covered) — but this needs an explicit
per-deployment policy knob eventually, not a silently-assumed answer, since a cold miss against the
*remote* tier specifically could add real, user-visible latency to interactive generation. **Also open**:
whether a future speculative-prefetch-during-the-current-step's-compute (predicting likely next-token
n-grams to warm ahead of the next call) is worth building — the honest finding from the companion design
doc's own review is that no such overlap currently exists to exploit (n-gram injection happens only at
the input embedding, so there's no independent per-step compute today to hide I/O latency behind); this
stays a real future improvement, not a v1 requirement.

### 7c. Data type contract

Sub0Llm's engine computes internally in `float32` (`op_embed` reads `float*` rows directly); the
motivating real table (`Qwen/Qwen3.8-Flash-Next`'s n-gram embeddings) is stored in `bf16` on disk.
**Requirement, not left implicit**: `register_table`'s `row_dtype` parameter (§3) must let a caller
request `resolve_into`/`try_get` hand back **already-dequantized `float32` rows**, not raw on-disk bytes
requiring a second conversion pass in the caller — i.e. dtype conversion is Sub0Firn's job, performed
once per row (ideally once ever, at whichever tier first pulls a cold row in, cached converted rather
than reconverted on every warm hit), not a burden pushed back onto every consumer. This was implicit in
`row_dtype`'s presence in §3's signature but is worth stating as an explicit, checkable requirement:
**a consumer should never need to know or care what format the source table was stored in.**

### 7d. What this trace confirms about the API, and what it doesn't yet

Confirmed workable as specified: the `resolve_into`-before-compute pattern, the multi-table-per-call
shape (one handle per n-gram order/table, matching `ngram_tab[e]`'s real structure), the
many-concurrent-readers requirement (real number attached: ~24 on the reference machine), and the
precomputed-working-set mode's fit to training. **Not yet resolved, carried forward as real open items**:
§7b's decode-time miss policy, and (noted but not solved here) exactly how a caller supplies the
offset-resolution callback for a *specific* real external format like the Qwen n-gram table's 128-shard
safetensors layout — §3's `local_sharded`/`remote_http_range` descriptors name the shape of that contract
but a reference implementation of the callback itself (reusing the extraction logic already proven in
Sub0Llm's `docs/QWEN4_PREVIEW_REFERENCE.md` Stage 1) doesn't exist yet in either repo.
