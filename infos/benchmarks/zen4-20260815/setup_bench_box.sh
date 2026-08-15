#!/usr/bin/env bash
# StringZilla levenshtein-within-k bench-box setup + run script.
# Tested targets: Scaleway EM-B130E-NVMe (Zen4), DigitalOcean Premium Intel (Ice Lake).
# Usage: bash setup_bench_box.sh [step]
#   step = all (default) | setup | fuzz | sweep | stringwars
set -euo pipefail

STEP="${1:-all}"
REPO_URL="https://github.com/grouville/StringZilla.git"
BRANCH="levenshtein-within-k-simd"
WORK="$HOME/stringzilla-bench"

step_setup() {
    echo "=== CPU flags ==="
    grep -o 'avx512vbmi\|avx512f\|avx2' /proc/cpuinfo | sort -u

    sudo apt-get update -qq
    sudo apt-get install -y -qq build-essential git python3-venv python3-pip tmux

    mkdir -p "$WORK"
    cd "$WORK"
    if [ ! -d StringZilla ]; then
        git clone -b "$BRANCH" "$REPO_URL" StringZilla
    fi
    cd StringZilla
    git submodule update --init forkunion 2>/dev/null || true

    echo "=== Building C++ bench + fuzzer (native) ==="
    g++ -std=c++20 -O3 -Iinclude -Itest -Ibench -Iforkunion/include -march=native \
        bench/similarities.cpp forkunion/c/forkunion.cpp -lpthread -o bench_similarities
    g++ -std=c++20 -O3 -Iinclude -Itest -Iforkunion/include -march=icelake-server \
        test/fuzz_levenshtein_within.cpp forkunion/c/forkunion.cpp -lpthread -o fuzz_within_icelake
    g++ -std=c++20 -O3 -Iinclude -Itest -Iforkunion/include -march=native \
        test/fuzz_levenshtein_within.cpp forkunion/c/forkunion.cpp -lpthread -o fuzz_within_native

    echo "=== Building dataset (3M random word tokens) ==="
    if [ ! -f /tmp/dataset.txt ]; then
        python3 - <<'EOF'
import random
rng = random.Random(42)
with open("/tmp/dataset.txt", "w") as f:
    words = []
    for _ in range(3_000_000):
        n = rng.randint(3, 12)
        words.append("".join(rng.choice("abcdefghijklmnopqrstuvwxyz") for _ in range(n)))
    f.write(" ".join(words))
EOF
    fi
    wc -c /tmp/dataset.txt
}

step_fuzz() {
    cd "$WORK/StringZilla"
    echo "=== Native icelake fuzzer: exhaustive + random + batch ==="
    ./fuzz_within_icelake 0 exhaustive
    ./fuzz_within_icelake 300000
    ./fuzz_within_icelake 100000 batch
    ./fuzz_within_icelake 5000 batch long
    echo "=== Native (auto-dispatch) fuzzer smoke ==="
    ./fuzz_within_native 100000
    ./fuzz_within_native 20000 batch
}

step_sweep() {
    cd "$WORK/StringZilla"
    echo "=== Perf sweep: within-k vs full-distance baseline ==="
    STRINGWARS_DATASET=/tmp/dataset.txt STRINGWARS_TOKENS=words \
    STRINGWARS_BATCH_PER_CORE=65536 STRINGWARS_DURATION=2 \
    STRINGWARS_FILTER="levenshtein_within|levenshtein_serial_unit" \
    ./bench_similarities 2>&1 | tee "$WORK/sweep_$(uname -m)_$(date +%H%M).log"
}

step_stringwars() {
    cd "$WORK/StringZilla"
    echo "=== StringWars within_k comparison (vs polyleven / rapidfuzz cutoff) ==="
    if [ ! -d "$WORK/StringWars" ]; then
        git clone -q https://github.com/ashvardanian/StringWars.git "$WORK/StringWars"
    fi
    cd "$WORK/StringWars"
    # Apply the within_k bench commit (carried as a patch; lives on grouville's local branch).
    # Idempotent: skip when the working tree already carries the patch.
    if git diff --quiet -- similarities/bench.py; then
        git apply "$WORK/within_k_bench.patch"
    fi
    if [ ! -d "$WORK/venv" ]; then
        python3 -m venv "$WORK/venv"
        "$WORK/venv/bin/pip" install -q --upgrade pip
        SZ_TARGET=stringzillas-cpus "$WORK/venv/bin/pip" install -q -e "$WORK/StringZilla" --no-build-isolation
        "$WORK/venv/bin/pip" install -q stringzilla rapidfuzz polyleven numpy
    fi
    PYTHONPATH=$PWD STRINGWARS_DATASET=/tmp/dataset.txt STRINGWARS_TOKENS=words \
    STRINGWARS_TIME=5 STRINGWARS_WARMUP=1 STRINGWARS_WITHIN_CANDIDATES=both \
    "$WORK/venv/bin/python" similarities/bench.py -k "within_k" 2>&1 | tee "$WORK/stringwars_$(date +%H%M).log"
}

case "$STEP" in
    setup) step_setup ;;
    fuzz) step_fuzz ;;
    sweep) step_sweep ;;
    stringwars) step_stringwars ;;
    all) step_setup; step_fuzz; step_sweep; step_stringwars ;;
    *) echo "unknown step: $STEP" >&2; exit 1 ;;
esac
echo "=== DONE ($STEP) ==="
