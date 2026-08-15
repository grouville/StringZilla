"""Head-to-head: stringzillas LevenshteinWithinK vs rapidfuzz cdist(score_cutoff=k).

Three corpora x k in {1,2,3}; szs at 1 core (default scope) and N threads, cdist at
workers=1 and workers=-1. Prints pairs/s (millions) and per-core / all-core ratios.
Also cross-checks that both engines agree on the boolean matrix for a subsample.
Run on a QUIET machine - no concurrent builds or fuzzers.
"""

import random
import os
import time

import numpy as np
import stringzilla as sz
import stringzillas as szs
from rapidfuzz import process as rf_process
from rapidfuzz.distance import Levenshtein as rf_levenshtein

SIDE = int(os.environ.get("CDIST_SIDE", "20000"))
REPEATS = int(os.environ.get("CDIST_REPEATS", "3"))
ALPHABET = "abcdefghijklmnopqrstuvwxyz"

random.seed(42)


def make_random(count):
    return ["".join(random.choices(ALPHABET, k=random.randint(3, 10))) for _ in range(count)]


def make_equallen(count):
    return ["".join(random.choices(ALPHABET, k=8)) for _ in range(count)]


def make_accepts(count):
    bases = make_random(count)
    out = []
    for base in bases:
        word = list(base)
        for _ in range(random.randint(0, 3)):
            op = random.randint(0, 2)
            pos = random.randint(0, len(word) - 1) if word else 0
            if op == 0 and word:
                word[pos] = random.choice(ALPHABET)
            elif op == 1:
                word.insert(pos, random.choice(ALPHABET))
            elif word:
                word.pop(pos)
        out.append("".join(word))
    return bases, out


def bench(callable_, repeats=REPEATS):
    best = float("inf")
    for _ in range(repeats):
        start = time.perf_counter()
        callable_()
        best = min(best, time.perf_counter() - start)
    return best


def run_corpus(name, queries, candidates, scope_8t):
    pairs = len(queries) * len(candidates)
    sz_queries, sz_candidates = sz.Strs(queries), sz.Strs(candidates)
    for k in (1, 2, 3):
        engine_1c = szs.LevenshteinWithinK(k)
        t_szs_1c = bench(lambda: engine_1c(sz_queries, sz_candidates))
        t_szs_8t = bench(lambda: engine_1c(sz_queries, sz_candidates, device=scope_8t))
        t_cd_1c = bench(
            lambda: rf_process.cdist(queries, candidates, scorer=rf_levenshtein.distance, score_cutoff=k, workers=1))
        t_cd_8t = bench(
            lambda: rf_process.cdist(queries, candidates, scorer=rf_levenshtein.distance, score_cutoff=k, workers=-1))
        szs_1c, szs_8t = pairs / t_szs_1c / 1e6, pairs / t_szs_8t / 1e6
        cd_1c, cd_8t = pairs / t_cd_1c / 1e6, pairs / t_cd_8t / 1e6
        print(f"{name:9s} k={k}: szs 1c {szs_1c:6.1f}M  8t {szs_8t:6.1f}M | cdist 1c {cd_1c:6.1f}M  8t {cd_8t:6.1f}M | "
              f"per-core {szs_1c / cd_1c:.2f}x  all-core {szs_8t / cd_8t:.2f}x", flush=True)


def check_agreement(queries, candidates, k=2):
    sub_q, sub_c = queries[:500], candidates[:500]
    ours = np.asarray(szs.LevenshteinWithinK(k)(sz.Strs(sub_q), sz.Strs(sub_c)))
    theirs = rf_process.cdist(sub_q, sub_c, scorer=rf_levenshtein.distance, score_cutoff=k, workers=1) <= k
    assert np.array_equal(ours, theirs), f"DISAGREEMENT at k={k}: {np.count_nonzero(ours != theirs)} cells"
    print(f"agreement k={k}: OK ({ours.size} cells)", flush=True)


def main():
    scope_8t = szs.DeviceScope(cpu_cores=8)
    random_words = make_random(2 * SIDE)
    equal_words = make_equallen(2 * SIDE)
    accept_queries, accept_candidates = make_accepts(SIDE)
    for name, queries, candidates in (
        ("random", random_words[:SIDE], random_words[SIDE:]),
        ("equallen", equal_words[:SIDE], equal_words[SIDE:]),
        ("accepts", accept_queries, accept_candidates),
    ):
        check_agreement(queries, candidates)
        run_corpus(name, queries, candidates, scope_8t)


if __name__ == "__main__":
    main()
