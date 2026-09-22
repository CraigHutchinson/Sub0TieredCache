# Reference consumer: Sub0Llm's n-gram embeddings, checked against the real merged code

[README.md](../README.md)'s API surface and [REQUIREMENTS.md](../REQUIREMENTS.md) are an abstract
contract. This doc exists so it isn't *only* abstract: it traces the API against
[Sub0Llm](https://github.com/CraigHutchinson/Sub0Llm)'s actual, already-merged n-gram embeddings
implementation (`src/backend_cpu.cpp`/`include/sub0/layout.hpp`, `docs/NGRAM_EMBEDDING.md` in that
repo) — real call sites, real shapes, real threading, not a hypothetical usage sketch. Where the trace
surfaces a genuine gap the API doesn't yet resolve, it's flagged as **OPEN**, not quietly designed
around. This is also, concretely, the acceptance test for R1–R6: if a real design decision below can't
be expressed through the API as specified, the API is wrong, not the consumer.

## 1. Training path (`Model::forward()`, one call per training window)

Today's code computes `NGRAM_NUM_EMBEDDERS` small tables (a real worked example from Sub0Llm's
`docs/NGRAM_EMBEDDING.md` §6: `D_MODEL=448, NGRAM_MAX_N=3, K=2` → 4 tables), each looked up `SEQ_LEN`
times (one row per sequence position, up to 512 in a production build) — i.e. every `forward()` call
computes `NGRAM_NUM_EMBEDDERS × SEQ_LEN` row indices (cheaply, in Sub0Llm's own code — Sub0TieredCache never
sees the hash, per R1) and needs that many rows resolved before `op_embed` can run. Mapped directly onto
the API surface:

```
for e in 0..NGRAM_NUM_EMBEDDERS:
    resolve_into(table_handle[e], ngram_ids[e][0..T], ngram_rows_buf[e])   // pre-sized, reused buffer
// only after every resolve_into returns: op_embed reads ngram_rows_buf[e] as if it were tok_emb
```

`resolve_into` (not `prefetch`/`wait`) is the right call here specifically because Sub0Llm's own
`AGENTS.md` §1 (no heap allocation, no unbounded-latency branch inside a hot compute path) means the
*entire* resolve has to be a synchronous barrier before compute starts, not something compute checks
partway through — exactly what R2/R3 already promise.

**Concurrency, with a real number attached**: `train_batch` runs `DEFAULT_THREADS` OMP worker threads in
parallel (`src/backend_cpu.cpp`, `#pragma omp parallel num_threads(DEFAULT_THREADS)`), each processing
its own window independently — so up to `DEFAULT_THREADS` concurrent `forward()` calls, each issuing its
own `NGRAM_NUM_EMBEDDERS` `resolve_into` calls, all in flight together. On the reference development
machine (Sub0Llm's own `[[host-cpu-arrow-lake-hx]]` memory: 8P+16E cores, no SMT) that's up to ~24
concurrent callers. R5 ("coalesce same-row requests") is not a nice-to-have here — with a real training
corpus, adjacent windows share plenty of vocabulary, so concurrent `resolve_into` calls for the *same*
row across different threads are a real, expected occurrence, not an edge case.

**The strong prefetch opportunity, made concrete**: since a training corpus is fixed and known before
the run starts (mirroring how Sub0Llm's own engine already precomputes `corpus.tok` out-of-core — see
[tiered-storage-design.md](tiered-storage-design.md) §2b), the *entire* set of row indices a training run
will ever request, across every window and every table, is computable in one pass ahead of time, the same
shape as the existing `corpus.tok`/tokenizer-vocab precompute. The recommended pattern for this consumer
is therefore: one `prefetch(table_handle[e], ALL row indices this run will ever touch)` + `wait(...)` per
table at startup, sized so essentially every in-run `resolve_into` call is a warm-tier hit — turning the
RAM/disk tiers from a reactive cache into a precomputed working set, which is the strongest case
Bandana's own sizing technique ([prior-art.md](prior-art.md)) has to work with.

## 2. Decode path (`Model::forward_one()`, one call per generated token)

Structurally different, and honestly harder — flagged as such rather than glossed over. Each decode step
looks up exactly one row per table (not `T` rows), from a small rolling history of the last
`NGRAM_MAX_N-1` fed token ids (Sub0Llm's `docs/NGRAM_EMBEDDING.md`'s decode-path mirror of the training
hash). The prompt/generation isn't known in advance the way a training corpus is, so §1's
precomputed-working-set recipe doesn't apply here — this is squarely the **reactive mode** the API
surface already names as first-class.

**OPEN — not resolved by this spec as written**: whether a decode-time miss should block (`resolve_into`
for exactly the current step's single row, accepting whatever latency the coldest tier in play adds —
tolerable if the miss rate is low and every tier at least as fast as local disk, since decode is already
a sequential, per-token-latency-bound loop) or fall back to a defined "no signal" value matching Sub0Llm's
own existing convention for out-of-vocabulary/persistent-slot ids (`id_tok = 0`,
`docs/NGRAM_EMBEDDING.md`'s persistent-slot guard) rather than stall generation waiting on a cold remote
fetch. **Recommended default for a first implementation**: block via `resolve_into` for the single
current-step row (simplest, correct, and the natural training-corpus prefetch from §1 should make this
rare in practice for any vocabulary the training run actually covered) — but this needs an explicit
per-deployment policy knob eventually, not a silently-assumed answer, since a cold miss against the
*remote* tier specifically could add real, user-visible latency to interactive generation. **Also open**:
whether a future speculative-prefetch-during-the-current-step's-compute (predicting likely next-token
n-grams to warm ahead of the next call) is worth building — the honest finding from
[tiered-storage-design.md](tiered-storage-design.md)'s own review is that no such overlap currently
exists to exploit in Sub0Llm today (n-gram injection happens only at the input embedding, so there's no
independent per-step compute today to hide I/O latency behind); this stays a real future improvement, not
a v1 requirement.

## 3. Data type contract

Sub0Llm's engine computes internally in `float32` (`op_embed` reads `float*` rows directly); the
motivating real table (`Qwen/Qwen3.8-Flash-Next`'s n-gram embeddings) is stored in `bf16` on disk. R6
exists specifically because of this real case: `resolve_into`/`try_get` must hand back already-dequantized
`float32` rows, not raw on-disk bytes requiring a second conversion pass in the caller. **A consumer
should never need to know or care what format the source table was stored in.**

## 4. What this trace confirms about the API, and what it doesn't yet

Confirmed workable as specified: the `resolve_into`-before-compute pattern (R2/R3), the
multi-table-per-call shape (one handle per n-gram order/table, matching `ngram_tab[e]`'s real structure),
the many-concurrent-readers requirement (R5, real number attached: ~24 on the reference machine), and the
precomputed-working-set mode's fit to training. **Not yet resolved, carried forward as real open items**:
§2's decode-time miss policy, and (noted but not solved here) exactly how a caller supplies the
offset-resolution callback for a *specific* real external format like the Qwen n-gram table's 128-shard
safetensors layout — R7 names the shape of that contract but a reference implementation of the callback
itself (reusing the extraction logic already proven in Sub0Llm's `docs/QWEN4_PREVIEW_REFERENCE.md`
Stage 1) doesn't exist yet in either repo.
