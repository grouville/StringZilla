#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <random>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef FASTSS_SPLIT_THRESHOLD
#define FASTSS_SPLIT_THRESHOLD 10
#endif

#ifndef FASTSS_MAX_WORD_LENGTH
#define FASTSS_MAX_WORD_LENGTH 1000
#endif

#ifndef FASTSS_PREFIX_BITS
#define FASTSS_PREFIX_BITS 18
#endif

#ifndef FASTSS_ID_BITS
#define FASTSS_ID_BITS 20
#endif

static_assert(FASTSS_PREFIX_BITS >= FASTSS_ID_BITS);

static std::vector<std::uint32_t> make_prefix_offsets(std::vector<std::uint64_t> const &entries) {
    static constexpr std::size_t buckets = std::size_t(1) << FASTSS_PREFIX_BITS;
    std::vector<std::uint32_t> offsets(buckets + 1);
    std::size_t cursor = 0;
    for (std::size_t prefix = 0; prefix != buckets; ++prefix) {
        offsets[prefix] = static_cast<std::uint32_t>(cursor);
        std::uint32_t const next_hash = static_cast<std::uint32_t>((prefix + 1) << (32 - FASTSS_PREFIX_BITS));
        while (cursor != entries.size() &&
               (prefix + 1 == buckets || static_cast<std::uint32_t>(entries[cursor] >> 32) < next_hash))
            ++cursor;
    }
    offsets[buckets] = static_cast<std::uint32_t>(entries.size());
    return offsets;
}

static std::vector<std::uint32_t> compact_entries(std::vector<std::uint64_t> &full_entries) {
    static constexpr std::uint32_t suffix_mask =
        (std::uint32_t(1) << (32 - FASTSS_PREFIX_BITS)) - 1;
    std::vector<std::uint32_t> compact;
    compact.reserve(full_entries.size());
    for (std::uint64_t entry : full_entries) {
        std::uint32_t const hash = static_cast<std::uint32_t>(entry >> 32);
        std::uint32_t const id = static_cast<std::uint32_t>(entry);
        compact.push_back(((hash & suffix_mask) << FASTSS_ID_BITS) | id);
    }
    std::vector<std::uint64_t>().swap(full_entries);
    return compact;
}

struct Node {
    std::vector<std::pair<unsigned char, std::uint32_t>> children;
    bool terminal = false;
};

struct FlatNode {
    std::uint32_t first_edge;
    std::uint32_t count_and_terminal;
};

struct FlatEdge {
    std::uint32_t child;
    unsigned char symbol;
};

static std::vector<std::string> parse_csv_row(std::string const &line) {
    std::vector<std::string> fields;
    std::string field;
    bool quoted = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        char c = line[i];
        if (c == '"') {
            if (quoted && i + 1 < line.size() && line[i + 1] == '"') field.push_back('"'), ++i;
            else quoted = !quoted;
        } else if (c == ',' && !quoted) {
            fields.push_back(std::move(field));
            field.clear();
        } else field.push_back(c);
    }
    fields.push_back(std::move(field));
    return fields;
}

static std::vector<std::string> load_names(char const *path) {
    std::ifstream input(path);
    std::string line;
    std::getline(input, line);
    std::unordered_set<std::string> seen;
    std::vector<std::string> names;
    while (std::getline(input, line)) {
        auto fields = parse_csv_row(line);
        if (fields.size() <= 5 || fields[5].empty()) continue;
        bool ascii = std::all_of(fields[5].begin(), fields[5].end(), [](unsigned char c) { return c < 128; });
        if (ascii && seen.insert(fields[5]).second) names.push_back(fields[5]);
    }
    return names;
}

static std::vector<std::string> load_lines(char const *path) {
    std::ifstream input(path);
    std::unordered_set<std::string> seen;
    std::vector<std::string> names;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        bool ascii = !line.empty() && std::all_of(line.begin(), line.end(), [](unsigned char c) { return c < 128; });
        if (ascii && seen.insert(line).second) names.push_back(line);
    }
    return names;
}

static std::vector<std::string> load_queries(char const *path) {
    std::ifstream input(path);
    std::vector<std::string> queries;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        queries.push_back(line);
    }
    return queries;
}

static std::vector<std::string> make_queries(std::vector<std::string> const &names, std::size_t count) {
    static constexpr char alphabet[] = "abcdefghijklmnopqrstuvwxyz";
    std::mt19937_64 rng(42);
    std::vector<std::string> queries;
    queries.reserve(count);
    for (std::size_t index = 0; index != count; ++index) {
        std::string word;
        if ((index & 1) == 0) {
            word = names[rng() % names.size()];
            std::size_t edits = 1 + rng() % 2;
            for (std::size_t edit = 0; edit != edits; ++edit) {
                char op = "sid"[rng() % 3];
                std::size_t position = word.empty() ? 0 : rng() % word.size();
                if (op == 's' && !word.empty()) word[position] = alphabet[rng() % 26];
                else if (op == 'i') word.insert(word.begin() + position, alphabet[rng() % 26]);
                else if (!word.empty()) word.erase(word.begin() + position);
            }
        } else {
            std::size_t length = 4 + rng() % 7;
            for (std::size_t i = 0; i != length; ++i) word.push_back(alphabet[rng() % 26]);
        }
        queries.push_back(std::move(word));
    }
    std::shuffle(queries.begin(), queries.end(), rng);
    return queries;
}

struct Trie {
    std::vector<Node> nodes = std::vector<Node>(1);
    std::vector<FlatNode> flat_nodes;
    std::vector<FlatEdge> flat_edges;
    std::size_t max_depth = 0;

    void insert(std::string const &word) {
        std::uint32_t node = 0;
        for (unsigned char c : word) {
            auto &children = nodes[node].children;
            auto found = std::find_if(children.begin(), children.end(), [&](auto const &edge) { return edge.first == c; });
            if (found == children.end()) {
                std::uint32_t next = static_cast<std::uint32_t>(nodes.size());
                children.emplace_back(c, next);
                nodes.emplace_back();
                node = next;
            } else node = found->second;
        }
        nodes[node].terminal = true;
        max_depth = std::max(max_depth, word.size());
    }

    void compact() {
        flat_nodes.resize(nodes.size());
        flat_edges.reserve(nodes.size() - 1);
        for (std::size_t node_id = 0; node_id != nodes.size(); ++node_id) {
            auto &children = nodes[node_id].children;
            std::sort(children.begin(), children.end());
            std::uint32_t const first = static_cast<std::uint32_t>(flat_edges.size());
            for (auto const &[symbol, child] : children) flat_edges.push_back({child, symbol});
            flat_nodes[node_id] = {first, static_cast<std::uint32_t>(children.size()) |
                                             (nodes[node_id].terminal ? 0x80000000u : 0u)};
        }
        std::vector<Node>().swap(nodes);
    }

    std::size_t count_within(std::string const &query, std::uint8_t bound) const {
        std::size_t const width = query.size() + 1;
        std::uint8_t const cap = static_cast<std::uint8_t>(bound + 1);
        std::vector<std::uint8_t> rows((max_depth + 1) * width, cap);
        for (std::size_t i = 0; i != std::min<std::size_t>(width, bound + 1); ++i)
            rows[i] = static_cast<std::uint8_t>(i);
        std::size_t matches = 0;
        auto walk = [&](auto &&self, std::uint32_t node_id, std::size_t depth) -> void {
            std::uint8_t const *previous = rows.data() + depth * width;
            FlatNode const node = flat_nodes[node_id];
            std::uint32_t const edge_count = node.count_and_terminal & 0x7FFFFFFFu;
            for (std::uint32_t edge_index = node.first_edge; edge_index != node.first_edge + edge_count; ++edge_index) {
                unsigned char const symbol = flat_edges[edge_index].symbol;
                std::uint32_t const child_id = flat_edges[edge_index].child;
                std::uint8_t *current = rows.data() + (depth + 1) * width;
                std::size_t const text_length = depth + 1;
                std::size_t const from = text_length > bound ? text_length - bound : 0;
                std::size_t const to = std::min<std::size_t>(query.size(), text_length + bound);
                std::uint8_t row_min = cap;
                if (from == 0)
                    current[0] = static_cast<std::uint8_t>(std::min<std::size_t>(text_length, cap)), row_min = current[0];
                std::size_t const first_column = std::max<std::size_t>(from, 1);
                if (first_column > 1) current[first_column - 1] = cap;
                for (std::size_t column = first_column; column <= to; ++column) {
                    unsigned value = std::min({unsigned(previous[column]) + 1,
                                               unsigned(current[column - 1]) + 1,
                                               unsigned(previous[column - 1]) + (query[column - 1] != symbol)});
                    current[column] = static_cast<std::uint8_t>(std::min<unsigned>(value, cap));
                    row_min = std::min(row_min, current[column]);
                }
                if (to + 1 < width) current[to + 1] = cap;
                if ((flat_nodes[child_id].count_and_terminal & 0x80000000u) && current[width - 1] <= bound) ++matches;
                if (row_min <= bound) self(self, child_id, depth + 1);
            }
        };
        walk(walk, 0, 0);
        return matches;
    }
};

static std::uint64_t hash_without(std::string const &word, std::size_t skipped) {
    std::uint64_t hash = 1469598103934665603ull;
    for (std::size_t i = 0; i != word.size(); ++i)
        if (i != skipped) hash = (hash ^ static_cast<unsigned char>(word[i])) * 1099511628211ull;
    return hash;
}

static bool within_one(std::string const &first, std::string const &second) {
    if (first.size() + 1 < second.size() || second.size() + 1 < first.size()) return false;
    std::size_t i = 0, j = 0, edits = 0;
    while (i != first.size() && j != second.size()) {
        if (first[i] == second[j]) ++i, ++j;
        else if (++edits > 1) return false;
        else if (first.size() > second.size()) ++i;
        else if (second.size() > first.size()) ++j;
        else ++i, ++j;
    }
    edits += (first.size() - i) + (second.size() - j);
    return edits <= 1;
}

static bool within_two(std::string const &first, std::string const &second) {
    if (first.size() + 2 < second.size() || second.size() + 2 < first.size()) return false;
    if (first.size() <= 64) {
        if (first.empty()) return second.size() <= 2;
        std::uint64_t equality[256] = {};
        for (std::size_t i = 0; i != first.size(); ++i)
            equality[static_cast<unsigned char>(first[i])] |= std::uint64_t(1) << i;
        std::uint64_t positive = ~std::uint64_t(0), negative = 0;
        std::uint64_t const top = std::uint64_t(1) << (first.size() - 1);
        std::size_t score = first.size();
        for (std::size_t i = 0; i != second.size(); ++i) {
            std::uint64_t const matches = equality[static_cast<unsigned char>(second[i])];
            std::uint64_t const x = matches | negative;
            std::uint64_t const differences = (((x & positive) + positive) ^ positive) | x;
            std::uint64_t const horizontal_positive = negative | ~(differences | positive);
            std::uint64_t const horizontal_negative = positive & differences;
            score += (horizontal_positive & top) != 0;
            score -= (horizontal_negative & top) != 0;
            std::uint64_t const shifted_positive = (horizontal_positive << 1) | 1;
            std::uint64_t const shifted_negative = horizontal_negative << 1;
            positive = shifted_negative | ~(differences | shifted_positive);
            negative = shifted_positive & differences;
            if (score > 2 + second.size() - i - 1) return false;
        }
        return score <= 2;
    }
    std::vector<std::uint8_t> previous(first.size() + 1), current(first.size() + 1, 3);
    for (std::size_t i = 0; i <= first.size(); ++i) previous[i] = static_cast<std::uint8_t>(std::min<std::size_t>(i, 3));
    for (std::size_t row = 1; row <= second.size(); ++row) {
        current[0] = static_cast<std::uint8_t>(std::min<std::size_t>(row, 3));
        std::size_t const from = row > 2 ? row - 2 : 1;
        std::size_t const to = std::min<std::size_t>(first.size(), row + 2);
        if (from > 1) current[from - 1] = 3;
        for (std::size_t column = from; column <= to; ++column)
            current[column] = static_cast<std::uint8_t>(std::min<unsigned>(
                {unsigned(previous[column]) + 1, unsigned(current[column - 1]) + 1,
                 unsigned(previous[column - 1]) + (first[column - 1] != second[row - 1]), 3}));
        if (to < first.size()) current[to + 1] = 3;
        previous.swap(current);
    }
    return previous[first.size()] <= 2;
}

struct FastSsOne {
    std::vector<std::uint32_t> entries;
    std::vector<std::uint32_t> prefix_offsets;
    std::vector<std::uint32_t> generations;
    std::uint32_t generation = 0;
    std::vector<std::string> const *words = nullptr;

    void build(std::vector<std::string> const &dictionary) {
        words = &dictionary;
        std::size_t count = dictionary.size();
        for (auto const &word : dictionary) count += word.size();
        std::vector<std::uint64_t> full_entries;
        full_entries.reserve(count);
        generations.resize(dictionary.size());
        for (std::uint32_t id = 0; id != dictionary.size(); ++id) {
            auto const &word = dictionary[id];
            if (word.size() > FASTSS_MAX_WORD_LENGTH) continue;
            std::vector<std::uint32_t> residuals;
            residuals.reserve(word.size() + 1);
            auto hash32 = [](std::uint64_t hash) { return static_cast<std::uint32_t>(hash ^ (hash >> 32)); };
            residuals.push_back(hash32(hash_without(word, word.size())));
            for (std::size_t skipped = 0; skipped != word.size(); ++skipped)
                residuals.push_back(hash32(hash_without(word, skipped)));
            std::sort(residuals.begin(), residuals.end());
            residuals.erase(std::unique(residuals.begin(), residuals.end()), residuals.end());
            for (std::uint32_t hash : residuals) full_entries.push_back((std::uint64_t(hash) << 32) | id);
        }
        std::sort(full_entries.begin(), full_entries.end());
        prefix_offsets = make_prefix_offsets(full_entries);
        entries = compact_entries(full_entries);
    }

    std::size_t count_within(std::string const &query, std::vector<std::uint32_t> *output = nullptr) {
        if (++generation == 0) std::fill(generations.begin(), generations.end(), 0), generation = 1;
        std::vector<std::uint32_t> residuals;
        residuals.reserve(query.size() + 1);
        auto hash32 = [](std::uint64_t hash) { return static_cast<std::uint32_t>(hash ^ (hash >> 32)); };
        residuals.push_back(hash32(hash_without(query, query.size())));
        for (std::size_t skipped = 0; skipped != query.size(); ++skipped)
            residuals.push_back(hash32(hash_without(query, skipped)));
        std::sort(residuals.begin(), residuals.end());
        residuals.erase(std::unique(residuals.begin(), residuals.end()), residuals.end());
        std::size_t matches = 0;
        for (std::uint32_t hash : residuals) {
            std::size_t const prefix = hash >> (32 - FASTSS_PREFIX_BITS);
            std::uint32_t const suffix = hash & ((std::uint32_t(1) << (32 - FASTSS_PREFIX_BITS)) - 1);
            auto begin = entries.begin() + prefix_offsets[prefix];
            auto end = entries.begin() + prefix_offsets[prefix + 1];
            begin = std::lower_bound(begin, end, suffix << FASTSS_ID_BITS);
            for (; begin != end && (*begin >> FASTSS_ID_BITS) == suffix; ++begin) {
                std::uint32_t const id = *begin & ((std::uint32_t(1) << FASTSS_ID_BITS) - 1);
                if (generations[id] == generation) continue;
                generations[id] = generation;
                if (within_one((*words)[id], query)) {
                    ++matches;
                    if (output) output->push_back(id);
                }
            }
        }
        return matches;
    }
};

struct FastSsTwo {
    std::vector<std::uint32_t> entries;
    std::vector<std::uint32_t> prefix_offsets;
    std::size_t unique_hashes = 0;
    std::vector<std::uint32_t> generations;
    std::uint32_t generation = 0;
    std::vector<std::string> const *words = nullptr;

    static std::vector<std::uint32_t> residuals(std::string const &word) {
        static constexpr std::uint64_t base = 0x9E3779B185EBCA87ull;
        std::vector<std::uint64_t> prefixes(word.size() + 1), powers(word.size() + 1, 1);
        for (std::size_t i = 0; i != word.size(); ++i) {
            prefixes[i + 1] = prefixes[i] * base + static_cast<unsigned char>(word[i]) + 1;
            powers[i + 1] = powers[i] * base;
        }
        auto substring = [&](std::size_t begin, std::size_t end) {
            return prefixes[end] - prefixes[begin] * powers[end - begin];
        };
        auto fold = [](std::uint64_t hash) {
            hash ^= hash >> 33;
            hash *= 0xff51afd7ed558ccdull;
            hash ^= hash >> 33;
            return static_cast<std::uint32_t>(hash ^ (hash >> 32));
        };
        std::vector<std::uint32_t> result;
        result.reserve(1 + word.size() + word.size() * (word.size() - 1) / 2);
        result.push_back(fold(prefixes.back()));
        for (std::size_t first = 0; first != word.size(); ++first) {
            std::uint64_t const without_first = prefixes[first] * powers[word.size() - first - 1] +
                                                substring(first + 1, word.size());
            result.push_back(fold(without_first));
            for (std::size_t second = first + 1; second != word.size(); ++second) {
                std::size_t const middle_length = second - first - 1;
                std::size_t const suffix_length = word.size() - second - 1;
                std::uint64_t hash = prefixes[first] * powers[middle_length] + substring(first + 1, second);
                hash = hash * powers[suffix_length] + substring(second + 1, word.size());
                result.push_back(fold(hash));
            }
        }
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    static std::vector<std::uint32_t> residuals_one(std::string_view word, std::uint32_t salt) {
        auto hash_view = [&](std::size_t skipped) {
            std::uint64_t hash = 1469598103934665603ull;
            for (std::size_t i = 0; i != word.size(); ++i)
                if (i != skipped) hash = (hash ^ static_cast<unsigned char>(word[i])) * 1099511628211ull;
            return static_cast<std::uint32_t>(hash ^ (hash >> 32)) ^ salt;
        };
        std::vector<std::uint32_t> result;
        result.reserve(word.size() + 1);
        result.push_back(hash_view(word.size()));
        for (std::size_t skipped = 0; skipped != word.size(); ++skipped) result.push_back(hash_view(skipped));
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    void build(std::vector<std::string> const &dictionary) {
        words = &dictionary;
        std::vector<std::uint64_t> full_entries;
        static constexpr std::size_t split_threshold = FASTSS_SPLIT_THRESHOLD;
        generations.resize(dictionary.size());
        for (std::uint32_t id = 0; id != dictionary.size(); ++id) {
            auto const &word = dictionary[id];
            if (word.size() > FASTSS_MAX_WORD_LENGTH) continue;
            if (word.size() <= split_threshold + 2)
                for (std::uint32_t hash : residuals(word)) full_entries.push_back((std::uint64_t(hash) << 32) | id);
            if (word.size() + 2 >= split_threshold) {
                std::size_t const middle = (word.size() + 1) / 2;
                for (std::uint32_t hash : residuals_one(std::string_view(word).substr(0, middle), 0x9E3779B9u))
                    full_entries.push_back((std::uint64_t(hash) << 32) | id);
                for (std::uint32_t hash : residuals_one(std::string_view(word).substr(middle), 0x85EBCA6Bu))
                    full_entries.push_back((std::uint64_t(hash) << 32) | id);
            }
        }
        std::sort(full_entries.begin(), full_entries.end());
        std::uint32_t previous_hash = 0;
        for (std::size_t i = 0; i != full_entries.size(); ++i) {
            std::uint32_t const hash = static_cast<std::uint32_t>(full_entries[i] >> 32);
            if (i == 0 || hash != previous_hash) ++unique_hashes, previous_hash = hash;
        }
        prefix_offsets = make_prefix_offsets(full_entries);
        entries = compact_entries(full_entries);
    }

    std::size_t count_within(std::string const &query, std::vector<std::uint32_t> *output = nullptr) {
        if (++generation == 0) std::fill(generations.begin(), generations.end(), 0), generation = 1;
        std::size_t matches = 0;
        std::vector<std::uint32_t> query_residuals;
        static constexpr std::size_t split_threshold = FASTSS_SPLIT_THRESHOLD;
        if (query.size() <= split_threshold + 2) query_residuals = residuals(query);
        if (query.size() + 2 >= split_threshold) {
            std::size_t const center = (query.size() + 1) / 2;
            for (std::size_t middle = center > 1 ? center - 1 : 0;
                 middle <= std::min<std::size_t>(query.size(), center + 1); ++middle) {
                auto prefix = residuals_one(std::string_view(query).substr(0, middle), 0x9E3779B9u);
                auto suffix = residuals_one(std::string_view(query).substr(middle), 0x85EBCA6Bu);
                query_residuals.insert(query_residuals.end(), prefix.begin(), prefix.end());
                query_residuals.insert(query_residuals.end(), suffix.begin(), suffix.end());
            }
        }
        std::sort(query_residuals.begin(), query_residuals.end());
        query_residuals.erase(std::unique(query_residuals.begin(), query_residuals.end()), query_residuals.end());
        for (std::uint32_t hash : query_residuals) {
            std::size_t const prefix = hash >> (32 - FASTSS_PREFIX_BITS);
            std::uint32_t const suffix = hash & ((std::uint32_t(1) << (32 - FASTSS_PREFIX_BITS)) - 1);
            auto begin = entries.begin() + prefix_offsets[prefix];
            auto end = entries.begin() + prefix_offsets[prefix + 1];
            begin = std::lower_bound(begin, end, suffix << FASTSS_ID_BITS);
            for (; begin != end && (*begin >> FASTSS_ID_BITS) == suffix; ++begin) {
                std::uint32_t const id = *begin & ((std::uint32_t(1) << FASTSS_ID_BITS) - 1);
                if (generations[id] == generation) continue;
                generations[id] = generation;
                if (within_two((*words)[id], query)) {
                    ++matches;
                    if (output) output->push_back(id);
                }
            }
        }
        return matches;
    }
};

int main(int argc, char **argv) {
    if (argc > 1 && std::string_view(argv[1]) == "selftest") {
        std::vector<std::string> words;
        for (std::size_t length = 0; length <= 8; ++length)
            for (std::size_t bits = 0; bits != (std::size_t(1) << length); ++bits) {
                std::string word(length, '\0');
                for (std::size_t i = 0; i != length; ++i) word[i] = "\0a"[(bits >> i) & 1];
                words.push_back(std::move(word));
            }
        auto distance = [](std::string const &first, std::string const &second) {
            std::vector<std::size_t> previous(first.size() + 1), current(first.size() + 1);
            for (std::size_t i = 0; i <= first.size(); ++i) previous[i] = i;
            for (std::size_t row = 1; row <= second.size(); ++row) {
                current[0] = row;
                for (std::size_t column = 1; column <= first.size(); ++column)
                    current[column] = std::min({previous[column] + 1, current[column - 1] + 1,
                                                previous[column - 1] + (first[column - 1] != second[row - 1])});
                previous.swap(current);
            }
            return previous.back();
        };
        FastSsOne one;
        FastSsTwo two;
        one.build(words);
        two.build(words);
        for (auto const &query : words) {
            std::size_t expected_one = 0, expected_two = 0;
            for (auto const &word : words) {
                std::size_t const d = distance(word, query);
                expected_one += d <= 1;
                expected_two += d <= 2;
            }
            if (one.count_within(query) != expected_one || two.count_within(query) != expected_two) {
                std::cerr << "selftest mismatch at length " << query.size() << "\n";
                return 1;
            }
        }
        std::cout << "selftest passed: " << words.size() * words.size() * 2 << " memberships\n";
        return 0;
    }
    bool const large = argc > 1;
    auto names = large ? load_lines(argv[1]) : load_names("/home/ubuntu/fuzzup-localities.csv");
    std::size_t const queries_count = argc > 2 ? std::stoull(argv[2]) : 10000;
    auto queries = argc > 3 ? load_queries(argv[3]) : make_queries(names, queries_count);
    Trie trie;
    auto build_start = std::chrono::steady_clock::now();
    for (auto const &name : names) trie.insert(name);
    trie.compact();
    auto build_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - build_start).count();
    std::cout << "names=" << names.size() << " nodes=" << trie.flat_nodes.size() << " edges=" << trie.flat_edges.size()
              << " bytes=" << trie.flat_nodes.size() * sizeof(FlatNode) + trie.flat_edges.size() * sizeof(FlatEdge)
              << " build=" << build_seconds << "s\n";
    FastSsOne fastss;
    auto fastss_start = std::chrono::steady_clock::now();
    fastss.build(names);
    std::cout << "fastss entries=" << fastss.entries.size() << " bytes="
              << fastss.entries.size() * sizeof(fastss.entries[0]) << " build="
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - fastss_start).count() << "s\n";
    for (int repeat = 0; repeat != 3; ++repeat) {
        auto start = std::chrono::steady_clock::now();
        std::size_t matches = 0;
        std::vector<std::uint32_t> match_ids;
        for (auto const &query : queries) matches += fastss.count_within(query, &match_ids);
        double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::cout << "k=1 fastss " << elapsed << "s matches=" << matches << " output_bytes="
                  << match_ids.size() * sizeof(match_ids[0]) << "\n";
    }
    FastSsTwo fastss_two;
    auto fastss_two_start = std::chrono::steady_clock::now();
    fastss_two.build(names);
    std::cout << "fastss2split entries=" << fastss_two.entries.size() << " unique_hashes=" << fastss_two.unique_hashes << " bytes="
              << fastss_two.entries.size() * sizeof(fastss_two.entries[0]) << " build="
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - fastss_two_start).count() << "s\n";
    for (int repeat = 0; repeat != 3; ++repeat) {
        auto start = std::chrono::steady_clock::now();
        std::size_t matches = 0;
        std::vector<std::uint32_t> match_ids;
        for (auto const &query : queries) matches += fastss_two.count_within(query, &match_ids);
        double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        std::cout << "k=2 fastss " << elapsed << "s matches=" << matches << " output_bytes="
                  << match_ids.size() * sizeof(match_ids[0]) << "\n";
    }
    if (argc > 4) {
        auto dump = [&](auto &index, int bound) {
            std::string const path = std::string(argv[4]) + ".k" + std::to_string(bound) + ".bin";
            std::ofstream output(path, std::ios::binary);
            if (!output) throw std::runtime_error("failed to create " + path);
            std::uint64_t const queries_count_u64 = queries.size();
            output.write(reinterpret_cast<char const *>(&queries_count_u64), sizeof(queries_count_u64));
            for (auto const &query : queries) {
                std::vector<std::uint32_t> ids;
                index.count_within(query, &ids);
                std::sort(ids.begin(), ids.end());
                std::uint64_t const ids_count = ids.size();
                output.write(reinterpret_cast<char const *>(&ids_count), sizeof(ids_count));
                output.write(reinterpret_cast<char const *>(ids.data()), ids.size() * sizeof(ids[0]));
            }
        };
        dump(fastss, 1);
        dump(fastss_two, 2);
    }
#ifdef FASTSS_ONLY
    return 0;
#endif
    if constexpr (FASTSS_MAX_WORD_LENGTH < 1000) {
        Trie long_trie;
        for (auto const &name : names)
            if (name.size() > FASTSS_MAX_WORD_LENGTH) long_trie.insert(name);
        long_trie.compact();
        std::cout << "hybrid-long nodes=" << long_trie.flat_nodes.size() << " edges=" << long_trie.flat_edges.size()
                  << " bytes=" << long_trie.flat_nodes.size() * sizeof(FlatNode) +
                                     long_trie.flat_edges.size() * sizeof(FlatEdge) << "\n";
        for (int repeat = 0; repeat != 3; ++repeat) {
            auto start = std::chrono::steady_clock::now();
            std::size_t matches = 0;
            for (auto const &query : queries)
                matches += fastss_two.count_within(query) + long_trie.count_within(query, 2);
            double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::cout << "k=2 hybrid " << elapsed << "s matches=" << matches << "\n";
        }
    }
    for (std::uint8_t bound : {std::uint8_t(1), std::uint8_t(2)}) {
        for (int repeat = 0; repeat != 3; ++repeat) {
            auto start = std::chrono::steady_clock::now();
            std::size_t matches = 0;
            for (auto const &query : queries) matches += trie.count_within(query, bound);
            double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::cout << "k=" << unsigned(bound) << " trie-dp " << elapsed << "s matches=" << matches << "\n";
        }
    }
}
