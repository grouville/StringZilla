
/**
 *  @brief Differential fuzzer for the bounded Levenshtein membership walker.
 *  @file test/fuzz_levenshtein_within.cpp
 *
 *  Validates `szs::levenshtein_distance_within` against two independent oracles:
 *  a plain Wagner-Fischer reference DP and the existing bit-parallel Myers walker.
 *  The `batch` mode differential-tests the SIMD cross-product engines (Haswell, and
 *  Ice Lake when compiled in) against the serial batch engine (cross + symmetric,
 *  sequential + parallel). Not part of the CMake build - compile directly:
 *
 *      g++ -std=c++20 -O2 -Iinclude -Iforkunion/include test/fuzz_levenshtein_within.cpp forkunion/c/forkunion.cpp -lpthread -o fuzz_within
 *      ./fuzz_within [iterations] [exhaustive]
 *      g++ -std=c++20 -O2 -march=haswell -Iinclude -Iforkunion/include test/fuzz_levenshtein_within.cpp forkunion/c/forkunion.cpp -lpthread -o fuzz_within
 *      ./fuzz_within [iterations] batch [long]
 *
 *  The Ice Lake arm needs AVX-512; on hosts without it, run under Intel SDE:
 *
 *      g++ -std=c++20 -O2 -march=icelake-server -Iinclude -Iforkunion/include test/fuzz_levenshtein_within.cpp forkunion/c/forkunion.cpp -lpthread -o fuzz_within_icl
 *      sde64 -icl -- ./fuzz_within_icl [iterations] batch [long]
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include <stringzillas/similarities.hpp>

namespace szs = ashvardanian::stringzillas;
using szs::span;

/** @brief Plain Wagner-Fischer reference distance. Deliberately unoptimized. */
static size_t reference_distance(std::string const &a, std::string const &b) {
    size_t const m = a.size(), n = b.size();
    std::vector<size_t> previous(m + 1), running(m + 1);
    for (size_t i = 0; i <= m; ++i) previous[i] = i;
    for (size_t j = 1; j <= n; ++j) {
        running[0] = j;
        for (size_t i = 1; i <= m; ++i)
            running[i] = std::min(std::min(previous[i - 1] + (a[i - 1] != b[j - 1] ? 1ul : 0ul), previous[i] + 1),
                                  running[i - 1] + 1);
        std::swap(previous, running);
    }
    return previous[m];
}

int main(int argc, char **argv) {
    size_t const iterations = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1000000;
    bool const exhaustive = argc > 2 && std::string(argv[2]) == "exhaustive";
    bool const batch = argc > 2 && std::string(argv[2]) == "batch";
    bool const batch_long = argc > 3 && std::string(argv[3]) == "long";
    std::mt19937_64 rng(0x5eed);

    szs::levenshtein_distance_within<char, sz_cap_serial_k> within;
    szs::levenshtein_distance_myers<char, sz_cap_serial_k> myers;
    szs::cpu_specs_t specs {};
    szs::dummy_executor_t dummy_executor;

    std::vector<std::byte> scratch(1ul << 20);
    span<std::byte> scratch_space {scratch.data(), scratch.size()};

    if (exhaustive) {
        // Every pair of binary strings up to length 7, every bound up to 10 - a deterministic boundary sweep.
        std::vector<std::string> strings {""};
        for (size_t length = 1; length <= 7; ++length)
            for (size_t mask = 0; mask != (1ul << length); ++mask) {
                std::string s;
                for (size_t bit = 0; bit != length; ++bit) s += (char)('a' + ((mask >> bit) & 1));
                strings.push_back(s);
            }
        size_t checks = 0;
        for (std::string const &first : strings)
            for (std::string const &second : strings) {
                span<char const> const first_view {first.data(), first.size()};
                span<char const> const second_view {second.data(), second.size()};
                size_t const reference = reference_distance(first, second);
                for (size_t bound = 0; bound != 11; ++bound) {
                    sz_u8_t got = 0;
                    auto const scorer = szs::levenshtein_distance_within<char, sz_cap_serial_k> {bound};
                    if (scorer(first_view, second_view, got, scratch_space, dummy_executor, specs) !=
                            szs::status_t::success_k ||
                        got != 0 != (reference <= bound)) {
                        std::fprintf(stderr, "exhaustive mismatch: '%s' vs '%s', bound %zu, distance %zu, got %d\n",
                                     first.c_str(), second.c_str(), bound, reference, (int)got);
                        return 1;
                    }
                    ++checks;
                }
            }
        std::printf("OK: exhaustive sweep, %zu checks over %zu strings\n", checks, strings.size());
        return 0;
    }

    if (batch) {
#if SZ_USE_HASWELL || SZ_USE_ICELAKE
        // Differential-test the SIMD batch engines against the serial one: random collections,
        // cross-product + symmetric, sequential + parallel, bounds from 0 past the string lengths.
        szs::forkunion_executor_t pool;
        if (pool.try_spawn(4) != szs::status_t::success_k) {
            std::fprintf(stderr, "failed to spawn the thread pool\n");
            return 1;
        }

        size_t checks = 0, accepted = 0;
        for (size_t iteration = 0; iteration != iterations; ++iteration) {
            // Small alphabets are the adversarial case: dense matches exercise the early exits.
            size_t const alphabet_size = 1 + rng() % 4;
            // Short words fill the lockstep tiles; the `long` config forces the > 64 serial fallback.
            size_t const length_cap = batch_long ? 65 + rng() % 636 : (rng() % 10 < 7 ? 45 : 300);
            auto const random_char = [&]() { return (char)('a' + rng() % alphabet_size); };
            auto const random_collection = [&](size_t count) {
                std::vector<std::string> collection;
                for (size_t i = 0; i != count; ++i) {
                    size_t const length = rng() % (length_cap + 1);
                    std::string s;
                    for (size_t c = 0; c != length; ++c) s += random_char();
                    // Half of the entries are mutations of a sibling, so accept paths fire too.
                    if (i != 0 && rng() % 2) {
                        s = collection[rng() % i];
                        size_t const edits = rng() % 6;
                        for (size_t edit = 0; edit != edits && !s.empty(); ++edit) {
                            size_t const position = rng() % s.size();
                            switch (rng() % 3) {
                            case 0: s[position] = random_char(); break;          // substitution
                            case 1: s.erase(position, 1); break;                 // deletion
                            case 2: s.insert(position, 1, random_char()); break; // insertion
                            }
                        }
                    }
                    collection.push_back(std::move(s));
                }
                return collection;
            };

            std::vector<std::string> const queries = random_collection(1 + rng() % 12);
            std::vector<std::string> const candidates = random_collection(1 + rng() % 12);
            size_t const rows = queries.size(), cols = candidates.size();

            // Bounds span both serial tiers, the 64-bit word edge, and past the string lengths.
            size_t const bounds[] = {0, 1, 2, 3, 4, 5, 9, 63, length_cap + 1, length_cap * 2 + 2};
            for (size_t bound : bounds) {
                szs::levenshtein_within_serial_t serial_engine {bound};

                auto const check_matrices = [&](std::vector<sz_u8_t> &expected, std::vector<sz_u8_t> &got,
                                                size_t const matrix_rows, size_t const matrix_cols,
                                                char const *mode_label) -> bool {
                    for (size_t cell = 0; cell != matrix_rows * matrix_cols; ++cell) {
                        ++checks, accepted += got[cell] == 1;
                        if (got[cell] > 1 || expected[cell] != got[cell]) {
                            size_t const row = cell / matrix_cols, col = cell % matrix_cols;
                            std::fprintf(stderr,
                                         "batch mismatch (%s), iteration %zu, bound %zu, cell (%zu, %zu): %d != %d\n",
                                         mode_label, iteration, bound, row, col, (int)expected[cell], (int)got[cell]);
                            std::fprintf(stderr, "  query     (%zu): %s\n  candidate (%zu): %s\n", queries[row].size(),
                                         queries[row].c_str(), candidates[col].size(), candidates[col].c_str());
                            return false;
                        }
                    }
                    return true;
                };

                // Runs all four driver modes of one accelerated engine against the serial oracle.
                auto const differential = [&](auto &accelerated_engine, char const *engine_label) -> bool {
                    std::vector<sz_u8_t> expected(rows * cols), got(rows * cols);
                    szs::strided_rows<sz_u8_t> const expected_rows {expected.data(), rows, cols, cols};
                    szs::strided_rows<sz_u8_t> const got_rows {got.data(), rows, cols, cols};

                    // Cross-product, sequential then parallel.
                    if (serial_engine(queries, candidates, expected_rows) != szs::status_t::success_k ||
                        accelerated_engine(queries, candidates, got_rows) != szs::status_t::success_k)
                        return false;
                    if (!check_matrices(expected, got, rows, cols,
                                        engine_label[0] == 'h' ? "haswell cross" : "icelake cross"))
                        return false;
                    if (serial_engine(queries, candidates, expected_rows, pool) != szs::status_t::success_k ||
                        accelerated_engine(queries, candidates, got_rows, pool) != szs::status_t::success_k)
                        return false;
                    if (!check_matrices(expected, got, rows, cols,
                                        engine_label[0] == 'h' ? "haswell cross-parallel" : "icelake cross-parallel"))
                        return false;

                    // Symmetric self-similarity over the queries, sequential then parallel.
                    std::vector<sz_u8_t> expected_sym(rows * rows), got_sym(rows * rows);
                    szs::strided_rows<sz_u8_t> const expected_sym_rows {expected_sym.data(), rows, rows, rows};
                    szs::strided_rows<sz_u8_t> const got_sym_rows {got_sym.data(), rows, rows, rows};
                    auto const check_symmetric = [&](char const *mode_label) -> bool {
                        for (size_t cell = 0; cell != rows * rows; ++cell) {
                            ++checks, accepted += got_sym[cell] == 1;
                            if (got_sym[cell] > 1 || expected_sym[cell] != got_sym[cell]) {
                                size_t const row = cell / rows, col = cell % rows;
                                std::fprintf(
                                    stderr,
                                    "batch mismatch (%s), iteration %zu, bound %zu, cell (%zu, %zu): " "%d != %d\n",
                                    mode_label, iteration, bound, row, col, (int)expected_sym[cell],
                                    (int)got_sym[cell]);
                                std::fprintf(stderr, "  first  (%zu): %s\n  second (%zu): %s\n", queries[row].size(),
                                             queries[row].c_str(), queries[col].size(), queries[col].c_str());
                                return false;
                            }
                        }
                        return true;
                    };
                    if (serial_engine(queries, expected_sym_rows) != szs::status_t::success_k ||
                        accelerated_engine(queries, got_sym_rows) != szs::status_t::success_k)
                        return false;
                    if (!check_symmetric(engine_label[0] == 'h' ? "haswell symmetric" : "icelake symmetric"))
                        return false;
                    if (serial_engine(queries, expected_sym_rows, pool) != szs::status_t::success_k ||
                        accelerated_engine(queries, got_sym_rows, pool) != szs::status_t::success_k)
                        return false;
                    if (!check_symmetric(engine_label[0] == 'h' ? "haswell symmetric-parallel"
                                                                : "icelake symmetric-parallel"))
                        return false;
                    return true;
                };

#if SZ_USE_HASWELL
                szs::levenshtein_within_haswell_t haswell_engine {bound};
                if (!differential(haswell_engine, "haswell")) return 1;
#endif
#if SZ_USE_ICELAKE
                szs::levenshtein_within_icelake_t icelake_engine {bound};
                if (!differential(icelake_engine, "icelake")) return 1;
#endif
            }
        }

        std::printf("OK: %zu batch checks (%zu accepted, %.1f%%) over %zu random collections%s\n", checks, accepted,
                    100.0 * accepted / checks, iterations, batch_long ? " [long strings]" : "");
        return 0;
#else
        std::fprintf(stderr,
                     "batch mode needs a SIMD backend - recompile with -march=haswell or -march=icelake-server\n");
        return 1;
#endif
    }

    size_t accepted = 0, checks = 0;
    for (size_t iteration = 0; iteration != iterations; ++iteration) {
        // Small alphabets are the adversarial case for edit-distance filters: matches are dense,
        // so early exits and band boundaries get exercised far more than with uniform bytes.
        size_t const alphabet_size = 1 + rng() % 4;
        // Mix length regimes: short words, medium lines, and long spans crossing the Myers word tiers.
        size_t const length_cap = rng() % 10 < 7 ? 45 : (rng() % 10 < 7 ? 300 : 700);
        size_t const first_length = rng() % (length_cap + 1), second_length = rng() % (length_cap + 1);
        std::string first, second;
        auto const random_char = [&]() { return (char)('a' + rng() % alphabet_size); };
        for (size_t i = 0; i != first_length; ++i) first += random_char();

        // Half of the cases are mutations of the first string, so both accept and reject paths fire.
        if (rng() % 2) {
            second = first;
            size_t const edits = rng() % 6;
            for (size_t edit = 0; edit != edits && !second.empty(); ++edit) {
                size_t const position = rng() % second.size();
                switch (rng() % 3) {
                case 0: second[position] = random_char(); break;          // substitution
                case 1: second.erase(position, 1); break;                 // deletion
                case 2: second.insert(position, 1, random_char()); break; // insertion
                }
            }
            while (second.size() > second_length) second.erase(rng() % second.size(), 1);
            while (second.size() < second_length) second += random_char();
        }
        else {
            for (size_t i = 0; i != second_length; ++i) second += random_char();
        }

        span<char const> const first_view {first.data(), first.size()};
        span<char const> const second_view {second.data(), second.size()};
        size_t distance = 0;
        if (myers(first_view, second_view, distance, scratch_space) != szs::status_t::success_k) {
            std::fprintf(stderr, "myers failed on (%zu, %zu)\n", first.size(), second.size());
            return 1;
        }
        size_t const reference = reference_distance(first, second);
        if (reference != distance) {
            std::fprintf(stderr, "myers oracle disagrees with the reference at iteration %zu: %zu vs %zu\n", iteration,
                         distance, reference);
            return 1;
        }

        // Bounds span both tiers and the boundary between them; scale a few beyond the string lengths.
        size_t const bounds[] = {0, 1, 2, 3, 4, 5, 9, 15, first.size() + 1, first.size() + second.size() + 1};
        for (size_t bound : bounds) {
            sz_u8_t got = 0;
            auto const scorer = szs::levenshtein_distance_within<char, sz_cap_serial_k> {bound};
            szs::status_t const status = scorer(first_view, second_view, got, scratch_space, dummy_executor, specs);
            bool const expected = distance <= bound;
            checks++;
            if (status != szs::status_t::success_k || got != 0 != expected) {
                std::fprintf(stderr, "mismatch at iteration %zu, bound %zu: got %d, distance %zu\n", iteration, bound,
                             (int)got, distance);
                std::fprintf(stderr, "  first  (%zu): %s\n  second (%zu): %s\n", first.size(), first.c_str(),
                             second.size(), second.c_str());
                return 1;
            }
            accepted += got == sz_true_k;
        }
    }

    std::printf("OK: %zu checks (%zu accepted, %.1f%%) over %zu random pairs\n", checks, accepted,
                100.0 * accepted / checks, iterations);
    return 0;
}
