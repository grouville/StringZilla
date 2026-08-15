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

The byte-string and validated UTF-8/codepoint engines are exposed as separate C++ classes, separate opaque C handles,
and Python `LevenshteinIndex` / `LevenshteinIndexUTF8` classes. The C result is a borrowed sparse span valid until that
search handle's next call; Python copies it into an unordered list of `(ID, distance)` pairs. Both C shims accept
sequence, 32-bit tape, and 64-bit tape dictionaries. The UTF-8 facade rejects malformed or truncated input rather
than replacing it, and its deletion/trie engine indexes decoded UTF-32 codepoints. Custom C allocators, other language
bindings, and batch/parallel query submission remain release work.

On the all-ASCII pinned English corpus, where byte and codepoint answers coincide, the UTF-8 facade's median query
time was 2.064 ms at `k=1` and 57.60 ms at `k=2`, versus 1.921 ms and 41.00 ms for the byte API. The extra validation,
transcoding, and wider dictionary tape therefore cost about 7% and 40% respectively; owned dictionary storage grew
from 6.46 MB to 16.94 MB.

A deterministic non-ASCII audit mapped every lowercase ASCII letter bijectively onto U+0400..U+0419 in both pinned
files. This is not a natural-language corpus, but it preserves every expected edit distance while forcing valid
two-byte UTF-8/codepoint execution. The transcoded dictionary/query SHA-256 values were
`17870ef9f2ad2ed56f94cde8d8239c80f0f9bbb5295b6b425d8d8b5e9bbef0de` and
`e3c344a21bc50cdeb5d9a0cd700d32d2a203a6dbcbf2612c7d6db6613ede3fb3`. Native RapidFuzz
`CachedLevenshtein<char32_t>` took 24.42 s at `k=1` and 37.65 s at `k=2`; StringZilla took 2.141 ms and 57.68 ms,
about 11,406x and 653x faster. RapidFuzz decodes the entire corpus before timing, while StringZilla still validates and
decodes each query inside the timed call, favoring RapidFuzz. More importantly, complete per-query `(ID, distance)`
streams were byte-identical at both bounds and reproduced the topology-preserved hashes above (`10e1ce...38a6` and
`9fa72b...7bb4`).

Rust `fst` 0.4.7 cannot currently serve as the Unicode baseline despite documenting Unicode-scalar semantics. On a
three-key smoke test where `"é"`, `"Ѐ"`, and `"А"` must produce nine matches at distance one, it returned seven,
omitting the two substitutions between multibyte scalars sharing the same leading UTF-8 byte. On the transcoded
corpus it consequently returned only 362/1,064 rather than 9,103/201,190 matches. The checked-in harness now fails its
Unicode contract smoke test before timing, and those incomparable timings are excluded. A natural multilingual corpus
and additional same-contract Unicode index remain claim-gate work.

## Algorithm regions

| Region | Exact engine | Reason |
|---|---|---|
| `k = 0` | exact hash/radix lookup | No fuzzy machinery is needed. |
| `k = 1..2`, short dictionaries | deletion-neighborhood radix index + exact verifier | Measured headline region; avoids scanning the dictionary. |
| `k > 2` | radix trie/FST + banded DP | Avoids the combinatorial deletion-record expansion. |
| long dictionaries at `k = 1..2` | radix trie + lazy banded-DP automaton | Keeps construction memory bounded without false negatives. |

The first compact-trie banded-DP implementation was exact through `k=4`, but only 4.16x / 1.68x faster than native
RapidFuzz at `k=3/4`. A query-local lazy DFA packs a complete clipped row into one u64 for queries through 15 bytes;
for longer queries and bounds through seven, it packs only the active `2k+1` band. It memoizes visited transitions
and stores row-minimum/terminal-distance metadata in each cache entry. On the 370,105-word / 10,000-query persisted
corpus it takes 5.460 s at `k=3` and
16.217 s at `k=4`, versus pinned native RapidFuzz cached scans at 57.038 s and 70.840 s: 10.45x and 4.37x. Both return
3,158,139 and 26,600,296 matches. A 4K-entry direct-mapped cache was 5-9% slower than the 32K linear-probed cache and
is not retained. Do not project the low-bound multi-order-of-magnitude result onto this region.

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
`-march=haswell`, and native AVX-512 on one pinned AMD EPYC 4245P core. The low-bound rows are final CPU-pinned
ten/five-repeat medians after adaptive-directory tuning; the high-bound rows are from the earlier uncontended trie
capture:

| Compile tier | `k=1` | `k=2` | `k=3` | `k=4` |
|:---|---:|---:|---:|---:|
| portable x86-64 | 1.936 ms | 45.615 ms | 5.339 s | 15.920 s |
| AVX2 / Haswell | 1.961 ms | 42.022 ms | 5.385 s | 15.974 s |
| native AVX-512 | 1.921 ms | 41.000 ms | 5.461 s | 16.223 s |

This is not an ISA shootout—the hot index is currently portable scalar code and compiler tuning can change its
layout. It does establish that the result is algorithmic rather than an AVX-512-only effect, and that no AVX-512
performance claim is currently justified. Randomized repeated captures on independent AVX2, Intel AVX-512, and Arm
machines remain part of the release gate.

An independent Rust `fst` 0.4.7 comparison used `fst::Map` and its official Unicode Levenshtein automaton on the same
ASCII corpus, where Unicode-scalar and byte semantics coincide. This comparison is deliberately favorable to `fst`:
it returns dictionary IDs without distances, while StringZilla materializes both. The FST occupied 2,041,719 bytes and
built in 47.9 ms. Its default 10,000-state safety limit refused `k=3/4`; raising the limit to one million states produced
the following one-pass results:

| Bound | StringZilla | Rust `fst` | StringZilla speedup |
|---:|---:|---:|---:|
| 1 | 1.921 ms | 0.866 s | 451x |
| 2 | 41.000 ms | 5.469 s | 133x |
| 3 | 5.461 s | 62.284 s | 11.4x |
| 4 | 16.223 s | 226.019 s | 13.9x |

The raised-limit `fst` process peaked at 451,024 KiB RSS. The corresponding StringZilla process peaked at 254,500 KiB
while sequentially constructing and benchmarking its `k=1`, `k=2`, and `k=4` indexes; persistent `k=4` index plus
owned-dictionary storage was 113,737,588 bytes. Match counts agreed at every bound. Exact set comparison is still
required for this independent implementation; the stronger byte-for-byte ID-and-distance gate above applies to
RapidFuzz. The checked-in Rust harness rejects non-ASCII by default. Its explicit Unicode mode first runs a small
contract test and currently refuses to benchmark because `fst` 0.4.7 misses valid scalar substitutions.

Tantivy 0.26.1 was also benchmarked through its public `FuzzyTermQuery` with transpositions disabled and
`DocSetCollector` materializing every document address. It built its in-memory index in 216.6 ms and peaked at
98,876 KiB RSS. Median query time was 485.1 ms at `k=1` and 4.183 s at `k=2`, making StringZilla 253x and 102x faster
on those bounds. Tantivy returns IDs without distances and currently rejects bounds above two. Its lower observed RSS
is a real advantage; because the StringZilla RSS capture included sequential construction through the larger `k=4`
index, per-bound isolated RSS measurements are required before making a direct memory claim.

Lucene 10.3.1 was tested in two distinct modes on OpenJDK 21. Its public `FuzzyQuery` is not the same exact contract:
at English `k=2` it returned 199,789 rather than 201,190 hits because Lucene additionally requires edit distance to
be less than the shorter term length. That 9.45 s result is therefore excluded from same-contract ratios. The exact
mode constructs Lucene's public `LevenshteinAutomata` for every query and executes it through `AutomatonQuery` with
transpositions disabled and a complete hit-count collector. It returned the validated totals on every corpus:

| Corpus | Bound | StringZilla | Lucene exact automaton | StringZilla speedup |
|:---|---:|---:|---:|---:|
| English | 1 | 1.921 ms | 1.676 s | 872x |
| English | 2 | 41.000 ms | 14.965 s | 365x |
| Wikipedia URLs | 1 | 15.9 ms | 7.796 s | 489x |
| Wikipedia URLs | 2 | 502.6 ms | 45.424 s | 90.4x |
| four-symbol DNA | 1 | 12.9 ms | 963.8 ms | 74.7x |
| four-symbol DNA | 2 | 107.5 ms | 5.644 s | 52.5x |

The pinned harness is checked in. These numbers include query-automaton construction in both systems, and Lucene
returns hit counts rather than distances, which favors Lucene. Java process RSS (roughly 0.8-0.9 GiB with a fixed
heap configuration) is recorded for reproducibility but is not presented as a direct native-index memory comparison.

The official SymSpell-Rust 6.8.3 implementation at commit `df6b21ab` is the closest and strongest indexed baseline.
Its native contract uses optimal-string-alignment Damerau-Levenshtein distance, lowercases terms, cannot retain
duplicate entries, and returns terms/frequencies rather than dictionary IDs. The checked-in harness therefore requires
already-lowercase unique input, requests `Verbosity::All`, filters every suggestion through plain Levenshtein, maps
terms back to IDs, and materializes `(ID, distance)` results. This extra filtering is necessary for contract parity;
complete streams were byte-identical to the RapidFuzz oracle at both bounds.

On one pinned core, ten-repeat medians were 1.921 ms versus 20.931 ms at `k=1` and 41.000 ms versus 455.857 ms at
`k=2`: StringZilla was 10.90x and 11.12x faster. Isolated build times were 0.267/1.402 s for StringZilla and
0.366/1.384 s for SymSpell. StringZilla's speed-oriented persistent index plus owned dictionary occupied
155.8/217.6 MB and process peak was 198,432/379,656 KiB; SymSpell process peak was 317,424/812,100 KiB. Those RSS
figures include different runtime overheads and are not substitutes for serialized index sizes, but they establish
that the latency win was not purchased with a larger process footprint. SymSpell performs internal allocation and
sorting required by its public lookup API while StringZilla reuses caller-owned scratch; both are real API costs and
the distinction must remain visible in claims.

## StringWars relationship

StringWars' existing similarity categories are valid dense matrix workloads; immutable-dictionary retrieval is a
different sparse-output category and should not replace them. The first experimental `within_k` extension on this
work branch reused different random inputs across competitors, mixed byte and text semantics, compared different
matrix sizes and thread counts, and labelled raw cutoff distances as though they were boolean membership. Those were
our extension's errors, not defects in Ash's pre-existing suite. The corrected branch is preserved at
[`grouville/StringWars@1f81925`](https://github.com/grouville/StringWars/commit/1f81925): it uses identical deterministic
inputs, verifies every 512-by-512 matrix against a byte-level RapidFuzz oracle, separates random/sparse/dense
selectivity, matches matrix side and CPU scope, and labels allocation-inclusive, caller-buffer, raw-distance, and
boolean outputs separately.

On the 12-thread AVX-512 server, the final 0.5-second English run verified 2,359,296 cells with zero mismatches. At
one CPU and matched 16-by-16 matrices, StringZilla's allocation-inclusive boolean engine was 1.21x to 2.50x faster
than RapidFuzz `process.cdist` boolean membership across bounds 1/2/4 and random/sparse/dense mixes. The smallest lead
was dense `k=4` (65.0 versus 53.8 million comparisons/s); the largest was reject-heavy `k=1` (138.2 versus 55.2
million comparisons/s). These dense-engine results neither prove nor weaken the much larger immutable-index gains.

The deletion index stores every residual produced by deleting `0..k` symbols, not exactly `k`: the latter cannot
join unequal-length strings by a common residual. A 20-to-25-bit adaptive directory supplies the high hash bits.
Small indexes retain the 4 MiB minimum; large indexes grow until they average at most one record per eight buckets or
reach the 128 MiB cap. Dictionaries below `2^20` entries use packed 32-bit records (the remaining hash suffix plus a
20-bit ID); larger dictionaries require a wide record representation rather than truncation. Hash collisions only
add verifier work and can never change results. On English this speed-oriented policy increased persistent bytes from
19.3 to 149.3 MB at `k=1` and 81.1 to 211.2 MB at `k=2`, while reducing steady query medians from about 3.0/45.5 ms to
1.92/41.0 ms. Callers prioritizing memory can force the compact trie today; a direct memory-budget parameter would be
a useful follow-up.

The default builder now estimates the complete deletion-neighborhood upper bound before allocating it. At no more
than 80 residuals per dictionary word it indexes every word; above that it uses the trie for the complete dictionary.
This deliberately simple first cost model correctly separates the three measured regimes below. Callers can still
provide an explicit maximum indexed word length. A partial index owns a trie containing only fallback words, not a
second copy of the whole dictionary. The 80-record boundary is an empirical policy, not yet a universal optimum; it
must be re-fitted or replaced by a calibrated build/query/memory model before a broad SOTA claim.

## Adversarial corpora

Length and alphabet materially change the winning representation, so the English dictionary is not sufficient
evidence. The latter two deterministic mixed-query corpora below contain equal quarters of exact hits, one-edit
mutations, two-edit mutations, and length-guaranteed rejects:

| Dictionary / queries | Shape | Auto plan | `k=1` | `k=2` | Tantivy `k=1/2` |
|:---|:---|:---|---:|---:|---:|
| English words / 10,000 | 370,105 words, mean 10.44 bytes | deletion / deletion | 1.92 ms | 41.0 ms | 485 ms / 4.183 s |
| Wikipedia URLs / 10,000 | 97,054 strings, mean 47.85 bytes | deletion / trie | 15.9 ms | 502.6 ms | 480 ms / 2.428 s |
| four-symbol DNA / 1,000 | 100,000 strings, exactly 100 bytes | trie / trie | 12.9 ms | 107.5 ms | 52.7 ms / 282.3 ms |

The URL dictionary is Rust `fst` 0.4.7's bundled `wiki-urls-100000` file (97,054 actual lines), SHA-256
`deb1ba1bb5005621de81bbc498922c5e06d3e7a9cb65b52ef854747a93b7ccc2`; its query file is
`a508d155b620fba09ac512377fc544ea9f7f79f0b0b118a3818da9db3a0015c2`. StringZilla and RapidFuzz emitted
byte-identical per-query ID-and-distance streams at both bounds, with SHA-256
`8a5a7d36199b7f5665beb3ea25e20220097ba23176f441b41dcecc4787733abf` (`k=1`) and
`c8262d112c4284bbe1bcfbb2c1fd46b66bc3464acd9a35d1fc4128ac65a13f15` (`k=2`).

The DNA dictionary is StringWars `acgt_100.txt`, SHA-256
`b0df691ddcc7e6db1544db3e472780602b824aff9eafca2efb567e3d10904381`; its query file is
`ddd6318eb864b98cf7006d80b29fe51ad56ab270653152a14826ca9ba0bd249d`. Exact streams also matched RapidFuzz,
with SHA-256 `eed6f71598e6848ce5734df1a37e9100ab69550ab3123dcb93870c9407cc750c` (`k=1`) and
`1e2d9760941311937f2d9360fa14eee8d7cd8c5f12afaf17c260f9885ca5bc4e` (`k=2`). Radix-compressing unary paths
reduced the persistent DNA trie from 194,456,504 to 5,999,728 bytes, build time from about 97 to 19 ms, and isolated
process peak from roughly 334 MiB to 39.5 MiB. It also reduced `k=1/2` latency from 14.9/162.7 ms to 12.9/107.5 ms.
Tantivy's corresponding process peaked near 79 MiB, so the radix representation reverses the original memory loss as
well as extending the latency lead. On URLs, the trie shrank from 27,589,304 to 4,796,744 bytes and build time from
about 18 to 10 ms; median `k=2` query time regressed by roughly 3% in the final exactness run, from 488.5 to 502.6 ms.
On English `k=4`, the trie portion shrank from 26,147,852 to 16,778,260 bytes and median latency improved from about
17.05 to 16.49 s. These are single-machine measurements, not confidence intervals.

## Literature position

This work combines established algorithmic families; performance alone does not make either family novel:

- `k=1..2` is a deletion-neighborhood index in the [FastSS](https://arxiv.org/abs/1008.1191) family. Its production
  contributions are exact residual coverage for unequal lengths, packed ID/hash records, a radix directory, collision
  verification, duplicate-ID semantics, reusable scratch, and the measured boundary where deletion expansion stops.
- `k>2` follows the trie/FST intersection strategy of Schulz and Mihov's
  [Levenshtein automata](https://doi.org/10.1007/s10032-002-0082-8). The current implementation represents a clipped
  DP row in one integer and lazily memoizes only the `(row state, byte)` transitions reached while traversing the
  dictionary trie. Lucene's parametric DFA and Rust `fst` are production representatives that must remain in the
  comparison matrix.
- Approximate nearest-neighbor and streaming edit-distance results solve materially different contracts. They are
  relevant background, but cannot establish or refute SOTA for complete exact immutable-dictionary retrieval.

The defensible research hypothesis is the adaptive exact hybrid and its concrete representations: choose deletion
neighborhoods only where their expansion is cheaper than automaton/trie traversal, then choose a state representation
using query length, bound, alphabet, and trie shape. To claim a literature contribution rather than an engineering
contribution, that policy needs an explicit cost model, independent datasets, ablations of every representation choice,
and evidence that it improves the Pareto frontier of query time, construction time, and memory.

## Claim gate

The headline target is at least 10x lower steady-state query latency than native RapidFuzz cached/process search on
representative repeated-query immutable-dictionary workloads with byte-for-byte identical per-query ID sets. The
English `k=1/2` runs now also clear 10x against the pinned official SymSpell-Rust implementation under an exact-result
filter. Tantivy, Lucene, FastSS, other compact tries/FSTs, and BK-trees remain useful independent indexed baselines. A
broad SOTA claim still requires multiple public/downstream corpora, cold and cache-stress runs, RSS, construction
amortization, natural Unicode, parallel scaling, and adversarial length/hit-rate distributions.
