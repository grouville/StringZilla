
/**
 *  @brief Differential fuzzer for the bounded Levenshtein membership walker.
 *  @file test/fuzz_levenshtein_within.cpp
 *
 *  Validates `szs::levenshtein_distance_within` against two independent oracles:
 *  a plain Wagner-Fischer reference DP and the existing bit-parallel Myers walker.
 *  Not part of the CMake build - compile directly:
 *
 *      g++ -std=c++20 -O2 -Iinclude -Iforkunion/include test/fuzz_levenshtein_within.cpp -lpthread -o fuzz_within
 *      ./fuzz_within [iterations]
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
