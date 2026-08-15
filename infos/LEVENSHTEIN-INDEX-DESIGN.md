# Immutable Levenshtein dictionary index

Production design for issue #243. Experimental evidence remains on branch
`levenshtein-within-k-simd` through commit `ab6b6020`; this branch starts from the validated bounded verifier at
`39745be3` and does not depend on the later AVX-512 experiment.

## Contract

- Own an immutable dictionary and preserve its original zero-based IDs, including duplicate strings.
- Return the complete sparse set of `(dictionary_id, distance)` matches for an inclusive maximum distance.
- Provide distinct byte and valid-UTF-8/codepoint indexes, matching StringZilla's existing semantic split.
- Allow concurrent searches. Immutable index storage is shared; deduplication, DP rows, and output live in explicit
  reusable per-query/per-worker scratch.
- Make result order unspecified in the hot API. Offer sorting as an explicit caller operation; do not hide it in the
  benchmark.
- Report build time, owned dictionary bytes, persistent index bytes, scratch, output, and maximum supported sizes.

## Algorithm regions

| Region | Exact engine | Reason |
|---|---|---|
| `k = 0` | exact hash/radix lookup | No fuzzy machinery is needed. |
| `k = 1..2`, indexed lengths | deletion-neighborhood radix index + exact verifier | Measured headline region; avoids scanning the dictionary. |
| `k > 2` | compact trie/FST + banded DP | Avoids the combinatorial deletion-record expansion. |
| unusually long strings | compact trie/FST or dense bounded verifier | Keeps construction memory bounded without false negatives. |

The first compact-trie banded-DP implementation was exact through `k=4`, but only 4.16x / 1.68x faster than native
RapidFuzz at `k=3/4`. A query-local lazy DFA now packs each clipped DP row into one u64 for queries through 15 bytes
and bounds through 14, memoizes only visited `(state, byte)` transitions, and stores row-minimum/terminal-distance
metadata in each cache entry. On the 370,105-word / 10,000-query persisted corpus it takes 5.460 s at `k=3` and
16.217 s at `k=4`, versus pinned native RapidFuzz cached scans at 57.038 s and 70.840 s: 10.45x and 4.37x. Both return
3,158,139 and 26,600,296 matches. A 4K-entry direct-mapped cache was 5-9% slower than the 32K linear-probed cache and
is not retained. Do not project the low-bound ~776-6,300x result onto this region.

The full correctness gate passes for `k=1..4`. Both implementations emitted a versioned binary stream containing
the dictionary/query cardinalities and, for every query, the sorted `(u32 dictionary_id, u8 distance)` matches. `cmp`
reported byte equality at every bound:

| Bound | Exact result-stream SHA-256 |
|---:|:---|
| 1 | `10e1ce405d7ad90140f3bdc56982d888b4349ea55a7ef9325e13cc6f421a38a6` |
| 2 | `9fa72bff43487869292a50f8feb76ae3ec17c24b56cf75214cce8a99b4cc7bb4` |
| 3 | `d1f52cabfebda60766dd1eaa87ab2f13405d079a8b0257125dcca5749833895c` |
| 4 | `9752fa896ca66a33651f2c9d09c44d65c5466c8e26bc8d60ca03d912a009c488` |

The dictionary was `english-words/words_alpha.txt` at SHA-256
`3ed0c94610d8bcf7c11bbb49c56aa49c7234d32b66824df91f554169e572da48`; the persisted query file was
`paper_queries.txt` at SHA-256 `e5a10ba0393bea2e8ff43bf326a2e43b647de9ea588192f18e09dce1dfc06fe2`.
RapidFuzz was pinned to `b5830af53bd1b3c7460a8de1e9f7095df99b3470`. The checked-in native harnesses reproduce the
stream format, so the large 127 MiB `k=4` artifacts need not be committed.

The same source was also compiled with GCC 13.3 as portable `-march=x86-64 -mtune=generic`, AVX2
`-march=haswell`, and native AVX-512 on one pinned AMD EPYC 4245P core. In the first uncontended capture, median
query times were:

| Compile tier | `k=1` | `k=2` | `k=3` | `k=4` |
|:---|---:|---:|---:|---:|
| portable x86-64 | 3.210 ms | 50.046 ms | 5.339 s | 15.920 s |
| AVX2 / Haswell | 3.053 ms | 45.260 ms | 5.385 s | 15.974 s |
| native AVX-512 | 3.078 ms | 45.483 ms | 5.461 s | 16.223 s |

This is not an ISA shootout—the hot index is currently portable scalar code and compiler tuning can change its
layout. It does establish that the result is algorithmic rather than an AVX-512-only effect, and that no AVX-512
performance claim is currently justified. Randomized repeated captures on independent AVX2, Intel AVX-512, and Arm
machines remain part of the release gate.

The deletion index stores every residual produced by deleting `0..k` symbols, not exactly `k`: the latter cannot
join unequal-length strings by a common residual. A 20-bit directory supplies the high hash bits. Dictionaries below
`2^20` entries use packed 32-bit records (`12-bit hash suffix + 20-bit ID`); larger dictionaries require a wide
record representation rather than truncation. Hash collisions only add verifier work and can never change results.

## Claim gate

The headline target is at least 10x lower steady-state query latency than native RapidFuzz cached/process search on
representative repeated-query immutable-dictionary workloads with byte-for-byte identical per-query ID sets. Tantivy,
Lucene, SymSpell, FastSS, compact tries/FSTs, and BK-trees are independent indexed baselines. A broad SOTA claim also
requires multiple public/downstream corpora, cold and cache-stress runs, RSS, construction amortization, Unicode,
parallel scaling, and adversarial length/hit-rate distributions.
