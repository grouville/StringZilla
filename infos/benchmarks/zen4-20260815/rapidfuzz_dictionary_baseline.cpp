/** Native RapidFuzz baseline for repeated exact Levenshtein-within-k dictionary retrieval.
 *
 *  Builds RapidFuzz's cached scorer once per query, scans the immutable dictionary, and materializes every matching
 *  dictionary ID. Compile against a pinned rapidfuzz-cpp checkout, for example:
 *
 *    g++ -std=c++20 -O3 -march=native -DNDEBUG -I rapidfuzz-cpp rapidfuzz_dictionary_baseline.cpp -o rf_dict
 */
#include <rapidfuzz/distance/Levenshtein.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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

static void dump_matches(std::vector<std::string> const &dictionary, std::vector<std::string> const &queries,
                         std::size_t bound, std::string const &path) {
    std::ofstream output(path, std::ios::binary);
    if (!output) throw std::runtime_error("failed to create " + path);
    std::uint64_t const queries_count = queries.size();
    output.write(reinterpret_cast<char const *>(&queries_count), sizeof(queries_count));
    for (auto const &query : queries) {
        rapidfuzz::CachedLevenshtein<char> scorer(query);
        std::vector<std::uint32_t> ids;
        for (std::uint32_t id = 0; id != dictionary.size(); ++id)
            if (scorer.distance(dictionary[id], bound) <= bound) ids.push_back(id);
        std::uint64_t const ids_count = ids.size();
        output.write(reinterpret_cast<char const *>(&ids_count), sizeof(ids_count));
        output.write(reinterpret_cast<char const *>(ids.data()), ids.size() * sizeof(ids[0]));
    }
}

int main(int argc, char **argv) {
    if (argc < 3 || argc > 5) {
        std::cerr << "usage: rapidfuzz_dictionary_baseline DICTIONARY QUERIES [QUERY_LIMIT] [OUTPUT_PREFIX]\n";
        return 2;
    }
    std::size_t const query_limit = argc == 4 ? std::stoull(argv[3]) : 0;
    auto const dictionary = load_lines(argv[1]);
    auto const queries = load_lines(argv[2], query_limit);
    int const repeats = std::getenv("RF_REPEATS") ? std::stoi(std::getenv("RF_REPEATS")) : 3;
    std::cout << "dictionary=" << dictionary.size() << " queries=" << queries.size() << '\n';

    for (std::size_t bound : {std::size_t(1), std::size_t(2)}) {
        for (int repeat = 0; repeat != repeats; ++repeat) {
            std::vector<std::uint32_t> match_ids;
            auto const start = std::chrono::steady_clock::now();
            for (auto const &query : queries) {
                rapidfuzz::CachedLevenshtein<char> scorer(query);
                for (std::uint32_t id = 0; id != dictionary.size(); ++id)
                    if (scorer.distance(dictionary[id], bound) <= bound) match_ids.push_back(id);
            }
            double const elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::cout << "k=" << bound << " rapidfuzz-cached " << elapsed << "s matches=" << match_ids.size()
                      << " output_bytes=" << match_ids.size() * sizeof(match_ids[0]) << '\n';
        }
    }
    if (argc == 5)
        for (std::size_t bound : {std::size_t(1), std::size_t(2)})
            dump_matches(dictionary, queries, bound, std::string(argv[4]) + ".k" + std::to_string(bound) + ".bin");
}
