/**
 *  @brief Deterministic query generator for immutable-dictionary Levenshtein benchmarks.
 *
 *  Generates exact, one-edit, two-edit, guaranteed-beyond-four, or mixed queries by sampling lines from an existing
 *  dictionary. All mutations avoid newlines and preserve arbitrary UTF-8 byte sequences without interpreting them.
 */
#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

static std::vector<std::string> load_lines(char const *path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error(std::string("failed to open ") + path);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

static std::uint64_t splitmix64(std::uint64_t &state) {
    std::uint64_t value = (state += 0x9E3779B97F4A7C15ull);
    value = (value ^ (value >> 30)) * 0xBF58476D1CE4E5B9ull;
    value = (value ^ (value >> 27)) * 0x94D049BB133111EBull;
    return value ^ (value >> 31);
}

static std::size_t substitute(std::string &query, std::uint64_t random) {
    if (query.empty()) {
        query.push_back('~');
        return 0;
    }
    std::size_t const position = random % query.size();
    query[position] = query[position] == '~' ? '^' : '~';
    return position;
}

int main(int argc, char **argv) {
    if (argc != 6) {
        std::cerr << "usage: levenshtein_index_queries DICTIONARY OUTPUT COUNT exact|edit1|edit2|reject|mixed SEED\n";
        return 2;
    }
    auto const dictionary = load_lines(argv[1]);
    if (dictionary.empty()) {
        std::cerr << "dictionary is empty\n";
        return 3;
    }
    std::ofstream output(argv[2]);
    if (!output) {
        std::cerr << "failed to create " << argv[2] << '\n';
        return 4;
    }
    std::size_t const count = std::stoull(argv[3]);
    std::string_view const requested_mode = argv[4];
    std::uint64_t random_state = std::stoull(argv[5]);
    for (std::size_t query_index = 0; query_index != count; ++query_index) {
        std::uint64_t const sample_random = splitmix64(random_state);
        std::string query = dictionary[sample_random % dictionary.size()];
        std::string_view mode = requested_mode;
        if (mode == "mixed") {
            static constexpr std::string_view modes[] = {"exact", "edit1", "edit2", "reject"};
            mode = modes[query_index % 4];
        }
        if (mode == "edit1" || mode == "edit2") {
            std::size_t const first_position = substitute(query, splitmix64(random_state));
            if (mode == "edit2") {
                if (query.size() > 1) {
                    std::size_t second_position = splitmix64(random_state) % (query.size() - 1);
                    if (second_position >= first_position) ++second_position;
                    query[second_position] = query[second_position] == '~' ? '^' : '~';
                }
                else
                    query.push_back('~');
            }
        }
        if (mode == "reject") query.insert(0, "~~~~~"); // Length difference proves distance > 4.
        if (mode != "exact" && mode != "edit1" && mode != "edit2" && mode != "reject") {
            std::cerr << "unknown mode: " << mode << '\n';
            return 5;
        }
        output << query << '\n';
    }
    return output.good() ? 0 : 6;
}
