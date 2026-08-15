#include <stringzillas/levenshtein_index.hpp>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace szs = ashvardanian::stringzillas;
namespace sz = ashvardanian::stringzilla;

static std::size_t distance(std::string const &first, std::string const &second) {
    std::vector<std::size_t> previous(first.size() + 1), current(first.size() + 1);
    for (std::size_t column = 0; column <= first.size(); ++column) previous[column] = column;
    for (std::size_t row = 1; row <= second.size(); ++row) {
        current[0] = row;
        for (std::size_t column = 1; column <= first.size(); ++column)
            current[column] = std::min({previous[column] + 1, current[column - 1] + 1,
                                        previous[column - 1] + (first[column - 1] != second[row - 1])});
        previous.swap(current);
    }
    return previous.back();
}

int main() {
    std::vector<std::string> dictionary;
    for (std::size_t length = 0; length <= 8; ++length)
        for (std::size_t bits = 0; bits != (std::size_t(1) << length); ++bits) {
            std::string word(length, '\0');
            for (std::size_t index = 0; index != length; ++index) word[index] = "\0a"[(bits >> index) & 1];
            dictionary.push_back(std::move(word));
        }
    dictionary.push_back(dictionary[42]); // Duplicate values must retain distinct IDs.
    dictionary.push_back(std::string(70, 'x'));
    dictionary.push_back(std::string(70, 'x'));
    dictionary.back()[17] = 'y';
    dictionary.back()[53] = 'z';
    dictionary.push_back(std::string(68, 'x'));

    szs::levenshtein_index<> index;
    if (index.try_build(dictionary, 4, 128) != sz::status_t::success_k) return 1;
    szs::levenshtein_index<>::scratch_t scratch;
    szs::levenshtein_index<>::matches_t matches;
    std::size_t checks = 0;
    for (auto const &query : dictionary)
        for (std::uint8_t bound = 0; bound <= 4; ++bound) {
            if (index.find({query.data(), query.size()}, bound, scratch, matches) != sz::status_t::success_k) return 2;
            std::vector<std::pair<std::uint32_t, std::uint8_t>> actual, expected;
            for (auto const &match : matches) actual.emplace_back(match.id, match.distance);
            for (std::uint32_t id = 0; id != dictionary.size(); ++id) {
                std::size_t const score = distance(dictionary[id], query);
                if (score <= bound) expected.emplace_back(id, static_cast<std::uint8_t>(score));
                ++checks;
            }
            std::sort(actual.begin(), actual.end());
            if (actual != expected) {
                std::cerr << "mismatch query_length=" << query.size() << " bound=" << unsigned(bound)
                          << " actual=" << actual.size() << " expected=" << expected.size() << '\n';
                return 3;
            }
        }

    // One immutable index must be safely searchable from independent worker scratch spaces.
    bool concurrent_ok[2] = {false, false};
    std::thread workers[2];
    for (std::size_t worker = 0; worker != 2; ++worker)
        workers[worker] = std::thread([&, worker] {
            szs::levenshtein_index<>::scratch_t worker_scratch;
            szs::levenshtein_index<>::matches_t worker_matches;
            auto const &query = dictionary[dictionary.size() - 1 - worker];
            concurrent_ok[worker] = index.find({query.data(), query.size()}, 4, worker_scratch, worker_matches) ==
                                        sz::status_t::success_k &&
                                    worker_matches.size() != 0;
            // Every query is present in the dictionary, so at least one exact match is required.
        });
    for (auto &worker : workers) worker.join();
    if (!concurrent_ok[0] || !concurrent_ok[1]) return 4;

    // An explicit deletion cutoff routes unusually long words through the same exact trie even for k<=2.
    szs::levenshtein_index<> fallback_index;
    if (fallback_index.try_build(dictionary, 2, 64) != sz::status_t::success_k) return 5;
    auto const &long_query = dictionary[dictionary.size() - 2];
    if (fallback_index.find({long_query.data(), long_query.size()}, 2, scratch, matches) !=
        sz::status_t::success_k)
        return 6;
    std::vector<std::pair<std::uint32_t, std::uint8_t>> actual_fallback, expected_fallback;
    for (auto const &match : matches) actual_fallback.emplace_back(match.id, match.distance);
    for (std::uint32_t id = 0; id != dictionary.size(); ++id) {
        std::size_t const score = distance(dictionary[id], long_query);
        if (score <= 2) expected_fallback.emplace_back(id, static_cast<std::uint8_t>(score));
    }
    std::sort(actual_fallback.begin(), actual_fallback.end());
    if (actual_fallback != expected_fallback) return 7;

    // Automatic planning avoids quadratic deletion neighborhoods for uniformly long dictionaries.
    std::vector<std::string> long_dictionary = {std::string(100, 'a'), std::string(100, 'b')};
    szs::levenshtein_index<> automatic_index;
    if (automatic_index.try_build(long_dictionary, 2) != sz::status_t::success_k ||
        automatic_index.records_count() != 0)
        return 8;
    if (automatic_index.find({long_dictionary[0].data(), long_dictionary[0].size()}, 2, scratch, matches) !=
            sz::status_t::success_k ||
        matches.size() != 1 || matches[0].id != 0 || matches[0].distance != 0)
        return 9;
    std::cout << "OK: " << checks << " exhaustive memberships, records=" << index.records_count()
              << " index_bytes=" << index.index_bytes() << '\n';
}
