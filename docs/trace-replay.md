# Trace replay: measure the cache without the application

`sub0tieredcache-trace-replay` replays a recorded row-access trace against a `Table` over the real data
file. It measures what a consumer would see: hit rate, bytes read, and how long each miss stalls. It
does this without loading or running the application that produced the trace, so a cache change can be
tried in minutes and judged on these numbers before the full application is benchmarked again.

Build it with `-DSUB0TIEREDCACHE_BUILD_BENCHMARKS=ON`.

## Inputs

Two small binary files, both little-endian, plus the data file they describe. Neither format knows what
a row means: the producer decides that.

**Extents (`S0RX`)**: where each row lives in the data file.

| Field | Type |
|---|---|
| magic | 4 bytes, `S0RX` |
| version | u32, `1` |
| row count | u64 |
| per row: offset, length | u64, u64 |

Rows may differ in length; the table is created with `RowExtent::bounded`, sized to the longest row.

**Trace (`S0RT`)**: the order rows were requested in, as batches. A batch is the set of rows the
consumer needs together, prefetched together and held together.

| Field | Type |
|---|---|
| magic | 4 bytes, `S0RT` |
| version | u32, `1` |
| batch count | u64 |
| per batch: row count `n`, then `n` row ids | u32, then `n` x u64 |

Sub0Llm writes both for its routed-MoE experts: one row per (layer, expert), one batch per layer's
selection. See Sub0Llm `docs/STORAGE_STACK_PLAN.md`.

## What one batch does

1. `prefetch` the whole batch, as a consumer does once it knows its rows.
2. For each row in order: `try_get` (a hit, free), or `resolve_into` (a miss, waited for and timed).
3. Hold the batch's leases through `--compute-us` microseconds of busy work, standing in for the
   consumer's compute.
4. Release the leases before the next batch.

## Options

| Option | Meaning | Default |
|---|---|---|
| `--data FILE` | the file the extents point into | required |
| `--extents FILE`, `--trace FILE` | as above | required |
| `--budget-mib N` | cache size; rounded down to whole rows | 1024 |
| `--readers N` | backend read threads (I/O queue depth) | 8 |
| `--compute-us X` | synthetic compute per batch | 0 |
| `--limit-batches N` | replay only the first N batches | whole trace |
| `--chunk-kib N` | `TableConfig::fill_chunk_bytes`: split each row's fill into reads of N KiB | 0 (whole rows) |

## Report

A readable summary, then one JSON line for scripts:

- `hit_rate`, `misses`, `miss_bytes`: how often a needed row was not yet resident, and what that read.
- `fetches`, `evictions`: the table's own counters.
- `wait_p50_us` ... `wait_max_us`: how long a miss stalled the consumer, from asking for the row to
  holding it. This is the number a miss-path change exists to reduce.
- `stall_s`, `wall_s`: total time stalled on misses, and the whole replay's time.

## Measurement hygiene

- The result depends on whether the data file is in the OS page cache. Evict it before a cold run (Sub0Llm
  `scripts/page_cache.py` verifies the eviction), and say which you measured.
- The cache's storage here is ordinary memory, not pinned. Run on a host that is not under memory
  pressure, or the OS may page the cache out and inflate waits.
- One run is a starting point, not a verdict. Interleave the variants being compared, and repeat.
