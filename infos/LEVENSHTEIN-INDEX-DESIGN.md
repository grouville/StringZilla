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

The first compact-trie banded-DP implementation is exact through at least `k=4`, but its measured higher-bound wins
are much smaller than the deletion index. On the 370,105-word / 10,000-query persisted corpus it takes 13.703 s at
`k=3` and 42.29 s at `k=4`, versus pinned native RapidFuzz cached scans at 57.038 s and 70.85 s: 4.16x and 1.68x.
Both return 3,158,139 and 26,600,296 matches. The next justified higher-bound optimization is a query-specific
Levenshtein DFA/parametric automaton over the compact trie, caching transitions instead of recomputing the band for
every edge. Do not project the low-bound 760-6,376x result onto this region.

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
