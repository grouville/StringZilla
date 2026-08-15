# Plan: Bounded Levenshtein `within(k)` for StringZilla (issue #243)

Working notes, not part of the PR. Branch: `levenshtein-within-k-simd` on the fork
(`github.com/grouville/StringZilla`, remote `fork`). One single PR, stacked commits.
Last updated: 2026-08-15.

## TAKEOVER AUDIT (2026-08-15) — supersedes the strategy and claims below

### Product intent clarification (user decision, 2026-08-15)

Preserve the fuzzy-search use case, not any particular automaton implementation. The dense bounded `within(k)`
engine remains an important exact verifier and batch primitive, but the linked literature's repeated-query use case
requires a focused immutable Levenshtein dictionary index with sparse results. This must not expand into a generic
search framework. Retain NFA/Myers/packed/split tiers only where corrected benchmarks prove a dispatch-region win;
replace or delete losing tiers. Aim for state-of-the-art dense performance versus RapidFuzz and indexed performance
versus Tantivy/Lucene automata, SymSpell/FastSS-style deletion indexes, compact tries, and BK-trees. Prefer stacked
review units (bounded primitive, then focused index) if that keeps correctness and API review tractable.

The headline challenge is **at least 10x lower query latency than RapidFuzz for repeated exact
Levenshtein-within-k retrieval over an immutable dictionary**, returning the same complete sparse set of IDs. The
primary RapidFuzz baseline is its cached/process one-query-many-choice search, not a Python loop. Dense `cdist`
remains a separate kernel baseline and Tantivy/Lucene/SymSpell/FastSS/trie/BK-tree implementations remain indexed
system baselines. Report index construction time, steady-state and cold/cache-stress query latency, peak and
persistent memory, dictionary/query distribution, k, hit rate, byte versus Unicode semantics, output
materialization, threads, ISA, and exact agreement. A 10x claim on one friendly corpus is a lead, not SOTA; the
claim survives only if representative public and downstream corpora retain it without semantic shortcuts.

The earlier plan is preserved as history, but its central scope and performance conclusions are false.

- Issue #243 asks about Levenshtein-automaton **dictionary retrieval**: preprocess a dictionary and return the
  matching words for a query. The branch implements an exhaustive dense query×candidate Boolean matrix. That is
  a useful verifier primitive, but it does not implement the issue's indexed-search proposal.
- The statement that the branch “wins big on strings >= ~110” is disproven. On the paid Zen4 AVX-512 host, a
  corrected identical-corpus benchmark measured fixed 128-byte random pairs at 4.25M/s (k=1), 3.79M/s (k=4),
  and 3.38M/s (k=8) per core, versus RapidFuzz Boolean output at 69.15M/s, 34.04M/s, and 23.41M/s. The primary
  reason is structural: the bounded SIMD engine stops at a 64-byte shorter side and falls back to a serial
  pattern-table rebuild for every matrix cell.
- RapidFuzz `cdist` does **not** simply use scalar mbleven as claimed below. Its batched scorer uses
  `experimental::MultiLevenshtein<MaxLen>` and packs 64×u8 / 32×u16 / 16×u32 / 8×u64 patterns on AVX-512
  (half those lane counts on AVX2). StringZilla always uses 8×u64 / 4×u64, including for 8-byte words.
- The StringWars patch is not a fair benchmark: engines receive different mutated corpora from one sequential
  mutable RNG; matrix dimensions differ; allocation policies differ; and the “accept-heavy” diagonal mutation
  is overwhelmingly reject-heavy over the full cross-product. Use
  `infos/benchmarks/zen4-20260815/within_k_audit.log` for the corrected baseline.
- The new bounded engine is barely differentiated from StringZilla's existing exact matrix engine: at length 8,
  k=1 they both deliver about 70M pairs/s per core; at k=4 the bounded engine is about 12% slower. At lengths
  32/64 it is only about 10% faster; at 128/256 it is ~1.6× faster but still far behind RapidFuzz.
- A first experimental AVX-512 packed-byte tier (`within_64x8_shared_query_`, currently an uncommitted working
  change) is fuzz-clean and raises fixed-8 throughput to ~100–104M pairs/s per core, about 1.6× over the branch,
  but remains about 2× behind RapidFuzz. The next dense redesign must pack **patterns** and reuse their masks while
  streaming candidates, rather than repeatedly transposing query-major candidate tiles.
- The fuzzup integration is not semantically valid as written. `LevenshteinWithinK` counts bytes, while
  RapidFuzz counts Unicode characters; 3,218/8,266 fuzzup locality names are non-ASCII. On a 1,000,000-cell
  Unicode sample, byte membership disagreed with RapidFuzz in 24 cells at k=1 and 82 at k=2. A production index
  needs separate byte and UTF-8/codepoint semantics, like StringZilla's existing exact APIs.

### Corrected real-workload result and product direction

On the ASCII-semantics-controlled part of fuzzup (5,048 unique dictionary names, 10,000 mixed typo/random
queries, 50.48M dense pairs), the paid Zen4 host measured:

| k | StringZilla dense 1c | StringZilla dense 12 threads | RapidFuzz Boolean 1c | Tantivy indexed 1c |
|---|---:|---:|---:|---:|
| 1 | 0.467 s | 0.055 s | 0.306 s | 0.15–0.27 s |
| 2 | 0.717 s | 0.097 s | 0.309 s | 0.66–0.72 s |

A deliberately simple C++ compact trie with pruned DP, before production engineering, answered the same-shaped
10,000-query workload in ~0.050 s at k=1 and ~0.281 s at k=2; diagonal banding improved that to ~0.022 s and
~0.190 s respectively. It builds 23,274 nodes from the 5,048 words in ~0.7 ms. This already beats Tantivy on one
core and, at k=1, beats the current 12-thread dense scan without evaluating/materializing 50.48M cells.

The stronger low-k result is a lossless FastSS deletion-neighborhood filter with independent bounded-distance
verification. Packing a 32-bit residual hash and 32-bit dictionary ID into one sorted 64-bit entry gives:

| Dictionary / queries | k | Query total | Time/query | Index entries | Index bytes |
|---|---:|---:|---:|---:|---:|
| fuzzup ASCII 5,048 / 10,000 | 1 | 0.0095 s | 0.95 us | 48,069 | 0.38 MB |
| fuzzup ASCII 5,048 / 10,000 | 2 | 0.0521 s | 5.21 us | 231,427 | 1.85 MB |
| English 370,105 / 2,000 | 1 | 0.0035 s | 1.75 us | 3,774,912 | 30.2 MB |
| English 370,105 / 2,000 | 2 | 0.0393 s | 19.6 us | 19,234,971 | 153.9 MB |

All FastSS match totals equal both the independent trie-DP result and Tantivy on the exact same persisted query
corpus: 2,061 matches at k=1 and 47,001 at k=2. Median query totals were 3.44 ms / 39.6 ms for FastSS, 27.8 ms /
373 ms for trie DP, and 122.7 ms / 1.042 s for Tantivy: 35.6x and 26.3x faster than Tantivy. Hash collisions cannot
create false negatives; they only add candidates, and every candidate is verified. The k=2 cost is the expected
memory expansion, so the paper's split-word generalization and compressed postings are the next comparisons.

The next optimization pass showed that full-array binary search, not hashing or verification, dominated FastSS.
A 20-bit radix directory (4 MiB) narrows each residual lookup to about 18 adjacent records and replaces the second
binary search with a short posting scan. On the same saved 2,000-query corpus this reduced warm medians to 0.60 ms
at k=1 and 8.72 ms at k=2: about 205x and 119x faster than the corresponding Tantivy totals. A 100,000-query
cache-stress run measured 0.0391 s / 0.5079 s (0.39 us / 5.08 us per query), returning 108,314 / 2,448,031 total
matches. A 23-bit directory costs 32 MiB and improved the large k=2 run by only ~3%, so 20 bits is the current knee.
An exhaustive binary-alphabet sweep through length 8, including empty strings and embedded NULs, passed 522,242
independently computed membership checks.

The radix directory also makes its high hash bits redundant in each record. For dictionaries below 2^20 entries,
packing the remaining 12 hash bits and a 20-bit dictionary ID into one u32 halves record storage without weakening
the full 32-bit hash comparison (the directory supplies the high 20 bits). On 370,105 words the persistent k=2
index is now 76.94 MB records + 4.19 MB directory, and the 100,000-query run materializes 2,448,031 sparse IDs in
0.489 s. On the original public 213,557-word English dataset used by later approximate-dictionary work, the same
10,000 persisted queries produced exact Tantivy-agreeing totals and measured 2.58 ms vs 450.5 ms at k=1 (174.6x)
and 31.22 ms vs 3.868 s at k=2 (123.9x). See `levenshtein_index_audit.log`; these query timings materialize IDs,
not just counts.

The 2010 paper's split-word claim did not reproduce with our lossless implementation: thresholds 10, 12, and 15
reduced or increased memory by roughly -13%, -2%, and +2%, while making k=2 queries about 4x, 2x, and 1.2x slower.
A simpler length-cutoff hybrid gives a clean Pareto curve on the 370,105-word corpus: full FastSS is 153.9 MB /
39.4 ms before the radix optimization; FastSS through lengths 12, 10, and 8 plus a trie for longer words use about
111.9, 78.9, and 44.8 MB and answer in 118, 159, and 213 ms respectively. All return exactly 47,001 matches.

One paper-level correctness caveat needs explicit resolution: Karch/Luxen/Sanders define `N_d(w)` using exactly
`d` deletions, but equal deletion counts cannot produce a common residual for unequal-length strings. The prototype
uses every deletion count 0..d, and its oracle sweep covers unequal lengths. Do not adopt the paper's smaller
`C(length,d)` representation unless the missing unequal-length mechanism can be found and proved.

The revised product split is therefore:

1. An immutable compact trie/FST dictionary index with sparse match IDs/values, codepoint-correct UTF-8 and byte
   variants, initially optimized for k<=2 (the production limit in both Lucene and Tantivy).
2. A separately optimized dense/verifier API using u8/u16/u32/u64 packed-pattern SIMD tiers and multiword SIMD
   beyond 64 bytes. It must justify its API surface independently against RapidFuzz.
3. Larger k should use a documented fallback (pruned trie DP, deletion index, or dense verifier), not inflate the
   low-k DFA/state tables silently.

The paid host `ubuntu@51.159.203.6` is reachable and has native Zen4 AVX-512. It is currently worth retaining for
packed-kernel profiling, but the decisive indexed prototype is not AVX-512-dependent.

### Native RapidFuzz application baseline (2026-08-15)

The packed index also clears the headline target against RapidFuzz itself, not only Tantivy. Using rapidfuzz-cpp
commit `b5830af53bd1b3c7460a8de1e9f7095df99b3470`, a native `CachedLevenshtein<char>` was constructed once per
query, scanned all 370,105 dictionary words with the same distance cutoff, and materialized every matching u32
dictionary ID. On 10,000 persisted queries, three warm repetitions produced:

| k | RapidFuzz cached scan median | Packed index median | Speedup | Matching IDs | Index build |
|---|---:|---:|---:|---:|---:|
| 1 | 20.6121 s | 0.00323256 s | 6,376x | 9,103 | 0.226587 s |
| 2 | 35.2618 s | 0.0464070 s | 760x | 201,190 | 1.33063 s |

The build amortizes after roughly 110 queries at k=1 and 378 queries at k=2 versus this scan. A separate untimed
verification pass serialized the sorted IDs for every query; RapidFuzz and the index outputs compare byte-for-byte
equal. Their SHA-256 digests are `ef604a66ce03122f4466c0171577f52f5ab8a451bf52fe9f1928f0e998aef7bb`
at k=1 and `310a502728eb268d771c4eabbd509f5d4772d2735ebd5e39c0fe935c23f2eae6` at k=2. See
`rapidfuzz_dictionary_10k.log`, `stringzilla_index_10k.log`, and the pinned native baseline source
`rapidfuzz_dictionary_baseline.cpp`.

The new AVX-512 pattern-major dense experiment did not become the headline result. It passed 1.159 billion random
batch checks, but fixed/equal eight-byte input remained about 0.60-0.62x RapidFuzz per core and mixed k=2-3 input
about 0.39-0.48x. An L1-local 64x64 output tile did not materially improve it, falsifying output cache-line scatter
as the principal bottleneck. Preserve this experiment and its measurements, but do not add complexity or claim a
dense win without a new profile-supported design.

## 0. The one-sentence answer to "is the automaton winning?"

The automaton (our bounded Myers/NFA SIMD kernel) is **kept, not removed** — it wins on
long strings — but it **loses to rapidfuzz on short words at every k**, so we are adding
two cheaper tiers for short strings (mbleven for k≤3, banded Myers for k≥4) on top of it.
Nothing already merged gets deleted.

## 1. Goal

Ship `szs_levenshtein_distance_within` / cross-product `within(k)` membership that is
**faster than rapidfuzz `cdist(scorer=Levenshtein.distance, score_cutoff=k)` at every k
and every string length**, on both AVX2 (Haswell) and AVX-512 (Ice Lake). Ash will not
take a PR that wins only for k=1,2 and loses for k≥3, and he is not moved by
microbenchmarks — the evidence must be StringWars-style head-to-heads plus real-world
integrator benchmarks (fuzzup).

## 2. What already exists (done, fuzz-validated, pushed)

Commits through `39745be3` on the fork branch:

- **Serial tiers** (`include/stringzillas/similarities/serial.hpp`): NFA walker +
  bounded Myers, any length, any k. Beats StringZilla's own serial baseline everywhere.
- **SIMD batched automaton** (`.../haswell.hpp`, `.../icelake.hpp`): 4-lane (AVX2) /
  8-lane (AVX-512) bounded single-word Myers (`within_4x64_` / `within_8x64_`) with
  per-lane early-exit freeze, Eq-precompute instead of gathers, reject-pairs zeroed
  inline without breaking lane groups.
- **Validated**: exhaustive sweep 715k checks + 1.9B random batch (AVX2 native) +
  3.86B batch (AVX-512 native on Zen4 box); 68/68 `pytest test/similarities.py -k within`;
  500×500 matrices agree with rapidfuzz oracle.
- **StringWars** (local branch `levenshtein-within-k-bench`, commit `deea711`, NOT pushed):
  within_k category with per-bound oracle verification (20/20 clean) + fair cdist
  baselines at 4096² (1-cpu and all-cpu variants). Patch at `/tmp/within_k_bench.patch`
  and on the box at `~/stringzilla-bench/within_k_bench.patch`.

## 3. Measured state of play (why the automaton alone is not shippable)

Head-to-head vs rapidfuzz `cdist(score_cutoff=k)`, short-word corpora (5–20 chars),
pairs/s per core:

| Corpus | AVX2 laptop | AVX-512 Zen4 box |
|---|---|---|
| identical words | 0.57x | ~0.5x |
| random words | 0.25x | ~0.22x |
| reject-heavy (prefilter floor) | **1.10x** | — |

- We still **win big on strings ≥ ~110 chars** (automaton tier is the right design there).
- Diagnosis (confirmed against rapidfuzz source/docs): for k≤3 they use **mbleven**
  (O(n) model checking, no DP tables, ~18–38 cycles/pair); for k≥4 a **banded cutoff
  Myers with column-min early exit**. Our batched launch+scan costs ~125 cycles/pair.
  The gathers were eliminated and it changed nothing — per-pair launch overhead and the
  full-width scan are the bottleneck, not loads.
- Known small negative to triage: within-k9 on 7k-char single pairs is 2–6% slower than
  plain serial Myers (fixed per-call overhead in `bounded_myers_`, serial.hpp:2775).

## 4. Target architecture: three tiers, one feature

```
pair arrives (shorter length m, longer n, bound k)
├── length-diff reject (n - m > k)          → 0, free
├── k ≤ 3 && n ≤ 8    → mbleven lane kernel (4/8 pairs per launch, no tables, no scan)
├── k any && n short  → banded-Myers lane kernel (k≥4: diagonal band, column-min exit)
│                        [mbleven covers k≤3; existing within_4x64_ covers mid sizes]
└── n large           → existing batched bounded Myers automaton (within_4x64_/8x64_)
                         + serial NFA/Myers walker for single pairs / huge strings
```

Dispatch stays inside `score_range_` group formation: group key gains a "low-k class"
bit so mbleven-eligible pairs (k≤3, n≤8) batch together at full lane width.

### 4a. mbleven lane kernel (IN PROGRESS — haswell first)

Design fully worked out; do not re-derive:

- New method `within_lowk_4x8_` on `levenshtein_distance_within<char, haswell>` in
  `include/stringzillas/similarities/haswell.hpp` (after `within_4x64_`, ~line 5688).
- Per launch: zero 4 u64 slots per side, `memcpy` per lane (safe partial loads),
  build `__m256i` S and T.
- For σ ∈ [-k, k]: shift T by 8σ (`_mm256_srl_epi64`/`_mm256_sll_epi64`),
  `unequal = ~cmpeq_epi8(S, Tσ)`. **Critical**: OR in a "one-side-valid" mask
  (`s_valid XOR shifted t_valid`, precomputed per lane as u64 valid-byte masks) so
  phantom positions past string end always read as mismatch — otherwise real `'\0'`
  bytes compare equal to zero-padding and silently corrupt results. (Fuzz alphabet
  {0,1} covers NUL.)
- movemask per σ → `u32 mismatch_rows[7]`; per lane extract 7 bytes into a u64 word.
- Per-lane scalar greedy model walk over the canonical mbleven model table
  (ops d=0/i=1/r=2 packed 2 bits LSB-first):
  k=1: {r},{i}; k=2: {rr,di,id},{ri,ir},{ii};
  k=3: {rrr,dir,dri,idr,ird,rdi,rid},{rri,rir,irr,dii,idi,iid},{rii,iri,iir},{iii}.
  Counts `{{0},{1,1},{3,2,1},{7,6,3,1}}`. Walk: `rest = mm[σ+3] >> i`; `!rest` → accept;
  else `p = i + sz_u32_ctz(rest)`; d: `i=p+1, σ--`; i: `i=p, σ++`; r: `i=p+1`.
  End: accept iff `(mm[σ+3] >> i) == 0`. Any model success ⇒ explicit ≤k edit script
  (sound by construction); completeness = canonical mbleven table + exhaustive fuzz.
- Dispatch in `score_range_` (haswell.hpp ~6645): `lowk = (1 <= bound_ <= 3 && longer <= 8)`,
  break groups on class change, call the new kernel (no scratch needed).
- Correctness invariants already proven by hand: phantom bits force mismatch on
  exactly-one-side regions; negative-side phantom bits always behind walk position;
  8 bits cover all real pairs since m ≤ n ≤ 8.

### 4b. Banded-Myers lane kernel for k≥4 short strings (NOT designed yet)

Diagonal-band (width 2k+1) single-word Myers per lane with column-min early exit —
the rapidfuzz mid-k strategy, batched 4/8-wide. Design after mbleven lands and measures.

## 5. Verification loop (the "agentic loop")

1. Build fuzzer:
   `g++ -std=c++20 -O3 -Iinclude -Itest -Iforkunion/include -march=native test/fuzz_levenshtein_within.cpp forkunion/c/forkunion.cpp -lpthread -o /tmp/fuzz_within_new`
2. `/tmp/fuzz_within_new 0 exhaustive` (covers the new tier end-to-end: binary strings
   ≤7 × bounds ≤10; bump max length to 8 to fully cover n≤8) then
   `/tmp/fuzz_within_new 1000000 batch long` in background (10–15 min, untimed).
3. `pytest test/similarities.py -k within` (68 tests; **not** doctests.py).
4. Head-to-head: `python /tmp/cdist_compare.py` (side=20000, k=1,2,3, agreement-checked,
   writes `/tmp/cdist_new.log`) and `/tmp/cdist_k4568.py` for k=4,6,8. Quiet machine only.
5. StringWars on the Zen4 box with `/tmp/within_k_bench.patch` (fair cdist + oracle).
6. Real-world: fuzzup bench (`/tmp/fuzzup/bench_whitelist.py`, swap fix already in
   `fuzzup/whitelists.py`; their distance mode is broken upstream, our swap fixes
   semantics; byte-metric caveat verified).

Targets: k=1 ≥ ~200M pairs/s/core (cdist: 89M laptop / 220M Zen4), k=2,3 at least parity,
k≥4 banded tier at least parity, no regressions elsewhere.

## 6. Hardware / infra

- **Zen4 box** (Scaleway EM-B130E-NVMe-64G, EPYC 4245P, AVX-512 native, ~€0.315/h):
  `ssh ubuntu@51.159.203.6` — **currently DOWN** (`Permission denied (publickey)` since
  mid-session; user to check Scaleway console / reinstall). All critical data retrieved
  except final fair-cdist StringWars logs (`~/stringzilla-bench/stringwars_*.log`).
  Fallback for AVX-512 fuzzing: Intel SDE at
  `/home/dagger/tools/sde-external-10.8.0-2026-03-15-lin/sde64 -icl -- <bin>` (10–30x
  slower; ~300k batch iterations). → **Destroy the box when done.**
- **DigitalOcean droplet** (Premium Intel, 2 vCPU, $0.048/h): `ssh root@146.190.166.138` —
  status unverified lately; Ice Lake-class AVX-512 if premium Intel is ICL/SPR.
  Destroy droplet when done.
- Laptop: Haswell AVX2, native.

## 7. Remaining tasks (TODO order)

1. Implement `within_lowk_4x8_` (haswell) + dispatch — design in §4a.
2. Fuzz + pytest + cdist re-measure on laptop.
3. Port to icelake (8 lanes, `_mm512_*`); fuzz natively on box or via SDE.
4. Banded-Myers lane tier for k≥4 short strings (§4b), haswell + icelake, same loop.
5. Triage the within-k9 long-pairwise −2..−6% regression (serial.hpp:2775 overhead).
6. fuzzup bench re-run with fixed engine.
7. Assemble PR results section from all logs (laptop AVX2 + Zen4 AVX-512 + StringWars
   + fuzzup); adversarial re-check of every claimed number.
8. Ask user to `gh repo fork ashvardanian/StringWars` to share the bench patch.

## 8. Rules in force (user standing instructions)

- One PR only, stacked commits on `levenshtein-within-k-simd`; push to `fork`, NEVER to
  `origin` (ashvardanian upstream). Commit with `git -c commit.gpgsign=false commit`,
  `Add:`/`Improve:` prefixes, `Signed-off-by: Guillaume de Rouville <guillaume.derouville@gmail.com>`.
- Adversarial, ethical methodology: verify against oracles, fair baselines (the tiny-matrix
  cdist number was an artifact *in our favor* — fixed), quiet-machine-only measurements,
  never claim unverified results.
- Ash cares about StringWars-style and integrator (fuzzup) results, not microbenches.
