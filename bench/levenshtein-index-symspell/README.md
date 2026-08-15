# SymSpell exact-retrieval baseline

This harness pins the official `symspell_rs` dependency and adapts its public lookup API to StringZilla's exact
Levenshtein retrieval contract. SymSpell uses optimal-string-alignment Damerau-Levenshtein distance, so the harness
requests all suggestions and filters them with plain Unicode-scalar Levenshtein distance before sorting `(ID,
distance)` results.

Inputs must contain unique, already-lowercase UTF-8 strings because SymSpell lowercases dictionary entries and cannot
represent duplicate IDs. Build for the host CPU and run with:

```sh
RUSTFLAGS="-C target-cpu=native" cargo build --release
SYMSPELL_REPEATS=10 SYMSPELL_MIN_DISTANCE=1 SYMSPELL_MAX_DISTANCE=2 SYMSPELL_THREADS=1 \
    taskset -c 2 target/release/stringzilla-levenshtein-index-symspell-bench \
    DICTIONARY QUERIES 10000 DUMP_PREFIX
```

The dump format is the same `SZLEV001` stream used by the StringZilla and RapidFuzz harnesses, enabling a byte-for-byte
`cmp` correctness gate. Report cold and warmed repetitions separately; the first lookup pass is not a steady-state
measurement. For short parallel workloads, set `SYMSPELL_BATCHES_PER_REPEAT` above one to amortize worker startup and
use the same value in the StringZilla harness.
