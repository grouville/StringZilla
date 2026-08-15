#include <stringzillas/levenshtein_index.hpp>

#include <chrono>
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

int main(int argc, char **argv) {
    if (argc < 3 || argc > 4) {
        std::cerr << "usage: levenshtein_index DICTIONARY QUERIES [QUERY_LIMIT]\n";
        return 2;
    }
    std::size_t const query_limit = argc == 4 ? std::stoull(argv[3]) : 0;
    auto const dictionary = load_lines(argv[1]);
    auto const queries = load_lines(argv[2], query_limit);
    std::cout << "dictionary=" << dictionary.size() << " queries=" << queries.size() << '\n';

    for (std::uint8_t max_distance : {std::uint8_t(1), std::uint8_t(2), std::uint8_t(4)}) {
        szs::levenshtein_index<> index;
        auto const build_start = std::chrono::steady_clock::now();
        if (sz::status_t status = index.try_build(dictionary, max_distance); status != sz::status_t::success_k) {
            std::cerr << "build failed: " << int(status) << '\n';
            return 3;
        }
        double const build_seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - build_start).count();
        std::cout << "k=" << unsigned(max_distance) << " build=" << build_seconds
                  << "s records=" << index.records_count() << " index_bytes=" << index.index_bytes()
                  << " trie_bytes=" << index.trie_bytes() << " dictionary_bytes=" << index.dictionary_bytes()
                  << '\n';

        szs::levenshtein_index<>::scratch_t scratch;
        szs::levenshtein_index<>::matches_t matches;
        std::uint8_t const first_bound = max_distance <= 2 ? max_distance : 3;
        for (std::uint8_t bound = first_bound; bound <= max_distance; ++bound)
            for (int repeat = 0; repeat != 3; ++repeat) {
                std::size_t matches_count = 0;
                auto const start = std::chrono::steady_clock::now();
                for (auto const &query : queries) {
                    if (index.find({query.data(), query.size()}, bound, scratch, matches) !=
                        sz::status_t::success_k)
                        return 4;
                    matches_count += matches.size();
                }
                double const elapsed =
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
                std::cout << "k=" << unsigned(bound) << " query=" << elapsed << "s matches=" << matches_count
                          << " output_element_bytes=" << sizeof(szs::levenshtein_index_match_t) << '\n';
            }
    }
}
