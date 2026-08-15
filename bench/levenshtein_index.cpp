#include <stringzillas/levenshtein_index.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace szs = ashvardanian::stringzillas;
namespace sz = ashvardanian::stringzilla;

static std::vector<std::string> load_lines(char const *path, std::size_t limit = 0) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error(std::string("failed to open ") + path);
    std::vector<std::string> lines;
    std::string line;
    while ((!limit || lines.size() != limit) && std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

template <typename index_type_>
static bool dump_matches(index_type_ const &index, std::vector<std::string> const &queries, std::uint8_t bound,
                         std::string const &path) {
    std::ofstream output(path, std::ios::binary);
    if (!output) return false;
    char const magic[8] = {'S', 'Z', 'L', 'E', 'V', '0', '0', '1'};
    std::uint64_t const dictionary_size = index.size();
    std::uint64_t const queries_size = queries.size();
    output.write(magic, sizeof(magic));
    output.write(reinterpret_cast<char const *>(&dictionary_size), sizeof(dictionary_size));
    output.write(reinterpret_cast<char const *>(&queries_size), sizeof(queries_size));
    output.write(reinterpret_cast<char const *>(&bound), sizeof(bound));

    typename index_type_::scratch_t scratch;
    typename index_type_::matches_t matches;
    for (auto const &query : queries) {
        if (index.find({query.data(), query.size()}, bound, scratch, matches) != sz::status_t::success_k) return false;
        std::sort(matches.begin(), matches.end(), [](auto const &a, auto const &b) {
            return a.id != b.id ? a.id < b.id : a.distance < b.distance;
        });
        std::uint64_t const matches_size = matches.size();
        output.write(reinterpret_cast<char const *>(&matches_size), sizeof(matches_size));
        for (auto const &match : matches) {
            output.write(reinterpret_cast<char const *>(&match.id), sizeof(match.id));
            output.write(reinterpret_cast<char const *>(&match.distance), sizeof(match.distance));
        }
    }
    return output.good();
}

template <typename index_type_>
static int run(std::vector<std::string> const &dictionary, std::vector<std::string> const &queries,
               std::size_t deletion_max_length, std::vector<std::uint8_t> const &max_distances,
               std::string const &dump_prefix) {
    for (std::uint8_t max_distance : max_distances) {
        index_type_ index;
        auto const build_start = std::chrono::steady_clock::now();
        if (sz::status_t status = index.try_build(dictionary, max_distance, deletion_max_length);
            status != sz::status_t::success_k) {
            std::cerr << "build failed: " << int(status) << '\n';
            return 3;
        }
        double const build_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - build_start).count();
        std::cout << "k=" << unsigned(max_distance) << " build=" << build_seconds
                  << "s records=" << index.records_count() << " index_bytes=" << index.index_bytes()
                  << " trie_bytes=" << index.trie_bytes() << " dictionary_bytes=" << index.dictionary_bytes()
                  << " deletion_max_length=" << index.deletion_max_word_length() << '\n';

        typename index_type_::scratch_t scratch;
        typename index_type_::matches_t matches;
        std::uint8_t const first_bound = max_distance <= 2 ? max_distance : 3;
        for (std::uint8_t bound = first_bound; bound <= max_distance; ++bound) {
            for (int repeat = 0; repeat != 3; ++repeat) {
                std::size_t matches_count = 0;
                auto const start = std::chrono::steady_clock::now();
                for (auto const &query : queries) {
                    if (index.find({query.data(), query.size()}, bound, scratch, matches) != sz::status_t::success_k)
                        return 4;
                    matches_count += matches.size();
                }
                double const elapsed =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                std::cout << "k=" << unsigned(bound) << " query=" << elapsed << "s matches=" << matches_count
                          << " output_element_bytes=" << sizeof(szs::levenshtein_index_match_t) << '\n';
            }
            if (!dump_prefix.empty()) {
                std::string const path = dump_prefix + ".k" + std::to_string(bound) + ".bin";
                if (!dump_matches(index, queries, bound, path)) {
                    std::cerr << "dump failed: " << path << '\n';
                    return 5;
                }
            }
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 5) {
        std::cerr << "usage: levenshtein_index DICTIONARY QUERIES [QUERY_LIMIT] [DUMP_PREFIX]\n";
        return 2;
    }
    std::size_t const query_limit = argc >= 4 ? std::stoull(argv[3]) : 0;
    std::string const dump_prefix = argc == 5 ? argv[4] : "";
    auto const dictionary = load_lines(argv[1]);
    auto const queries = load_lines(argv[2], query_limit);
    bool const utf8 = std::getenv("SZ_LEVENSHTEIN_UTF8") != nullptr;
    std::cout << "dictionary=" << dictionary.size() << " queries=" << queries.size()
              << " semantics=" << (utf8 ? "utf8-codepoints" : "bytes") << '\n';
    char const *deletion_max_length_env = std::getenv("SZ_LEVENSHTEIN_DELETION_MAX_LENGTH");
    std::size_t const deletion_max_length =
        deletion_max_length_env ? std::stoull(deletion_max_length_env)
                                : szs::levenshtein_index<>::automatic_deletion_max_word_length_k;

    std::vector<std::uint8_t> max_distances = {1, 2, 4};
    if (char const *requested_max = std::getenv("SZ_LEVENSHTEIN_MAX_DISTANCE")) {
        int const parsed = std::stoi(requested_max);
        if (parsed != 1 && parsed != 2 && parsed != 4) {
            std::cerr << "SZ_LEVENSHTEIN_MAX_DISTANCE must be 1, 2, or 4\n";
            return 2;
        }
        max_distances = {static_cast<std::uint8_t>(parsed)};
    }
    return utf8 ? run<szs::levenshtein_index_utf8<>>(dictionary, queries, deletion_max_length, max_distances,
                                                     dump_prefix)
                : run<szs::levenshtein_index<>>(dictionary, queries, deletion_max_length, max_distances,
                                                dump_prefix);
}
