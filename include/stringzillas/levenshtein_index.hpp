/**
 *  @brief Immutable exact Levenshtein dictionary retrieval for small edit bounds.
 *  @file include/stringzillas/levenshtein_index.hpp
 *  @author Ash Vardanian
 *
 *  Builds a lossless deletion-neighborhood filter for byte strings and verifies every candidate independently.
 *  Hash collisions can only add verifier work; they cannot alter the returned matches. Query-local generations,
 *  residual hashes, DP rows, and output are explicit, making one index safe to search concurrently.
 */
#ifndef STRINGZILLAS_LEVENSHTEIN_INDEX_HPP_
#define STRINGZILLAS_LEVENSHTEIN_INDEX_HPP_

#include "stringzillas/types.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <type_traits>

namespace ashvardanian {
namespace stringzillas {

struct levenshtein_index_match_t {
    u32_t id = 0;
    u8_t distance = 0;
};

template <typename allocator_type_ = std::allocator<char>>
class levenshtein_index {
  public:
    using allocator_t = allocator_type_;
    using match_t = levenshtein_index_match_t;

  private:
    static constexpr size_t prefix_bits_k = 20;
    static constexpr size_t suffix_bits_k = 32 - prefix_bits_k;
    static constexpr size_t packed_id_bits_k = 20;
    static constexpr size_t prefix_buckets_k = size_t(1) << prefix_bits_k;
    static constexpr u32_t suffix_mask_k = (u32_t(1) << suffix_bits_k) - 1;
    static constexpr u32_t packed_id_mask_k = (u32_t(1) << packed_id_bits_k) - 1;
    static constexpr u8_t rejected_distance_k = 3;

    struct trie_node_t {
        u32_t first_edge = 0;
        u32_t edges_count = 0;
        u32_t first_terminal = 0;
        u32_t terminals_count = 0;
    };
    struct trie_edge_t {
        u32_t child = 0;
        u8_t symbol = 0;
    };
    struct trie_frame_t {
        u32_t node = 0;
        u32_t next_edge = 0;
        u32_t end_edge = 0;
        u32_t depth = 0;
    };

    template <typename value_type_>
    using rebound_allocator_t =
        typename std::allocator_traits<allocator_t>::template rebind_alloc<value_type_>;
    template <typename value_type_>
    using vector_t = safe_vector<value_type_, rebound_allocator_t<value_type_>>;

    allocator_t alloc_ {};
    vector_t<char> tape_ {alloc_};
    vector_t<u64_t> offsets_ {alloc_};
    vector_t<u32_t> directory_ {alloc_};
    vector_t<u32_t> packed_records_ {alloc_};
    vector_t<u64_t> wide_records_ {alloc_};
    vector_t<trie_node_t> trie_nodes_ {alloc_};
    vector_t<trie_edge_t> trie_edges_ {alloc_};
    vector_t<u32_t> trie_terminals_ {alloc_};
    u8_t max_distance_ = 0;
    size_t max_word_length_ = 0;
    size_t deletion_max_word_length_ = 64;
    size_t fallback_words_count_ = 0;

    static u32_t fold_hash_(u64_t hash) noexcept {
        hash ^= hash >> 33;
        hash *= 0xff51afd7ed558ccdull;
        hash ^= hash >> 33;
        return static_cast<u32_t>(hash ^ (hash >> 32));
    }

    static bool checked_add_(size_t &total, size_t increment) noexcept {
        if (increment > std::numeric_limits<size_t>::max() - total) return false;
        total += increment;
        return true;
    }

    static bool residuals_upper_bound_(size_t length, u8_t distance, size_t &count) noexcept {
        count = 1;
        if (distance >= 1 && !checked_add_(count, length)) return false;
        if (distance >= 2) {
            if (length && length - 1 > std::numeric_limits<size_t>::max() / length) return false;
            if (!checked_add_(count, length * (length - 1) / 2)) return false;
        }
        return true;
    }

    template <typename scratch_type_>
    static status_t generate_residuals_(span<char const> word, u8_t distance, scratch_type_ &scratch) noexcept {
        size_t upper_bound = 0;
        if (!residuals_upper_bound_(word.size(), distance, upper_bound)) return status_t::overflow_risk_k;
        if (status_t status = scratch.residuals.try_resize(0); status != status_t::success_k) return status;
        if (status_t status = scratch.residuals.try_reserve(upper_bound); status != status_t::success_k) return status;
        if (status_t status = scratch.prefixes.try_resize(word.size() + 1); status != status_t::success_k) return status;
        if (status_t status = scratch.powers.try_resize(word.size() + 1); status != status_t::success_k) return status;

        static constexpr u64_t base = 0x9E3779B185EBCA87ull;
        scratch.prefixes[0] = 0;
        scratch.powers[0] = 1;
        for (size_t index = 0; index != word.size(); ++index) {
            scratch.prefixes[index + 1] =
                scratch.prefixes[index] * base + static_cast<u8_t>(word[index]) + 1;
            scratch.powers[index + 1] = scratch.powers[index] * base;
        }
        auto const substring_hash = [&](size_t begin, size_t end) noexcept {
            return scratch.prefixes[end] - scratch.prefixes[begin] * scratch.powers[end - begin];
        };
        if (status_t status = scratch.residuals.try_push_back(fold_hash_(scratch.prefixes[word.size()]));
            status != status_t::success_k)
            return status;
        if (distance >= 1)
            for (size_t first = 0; first != word.size(); ++first) {
                u64_t const without_first = scratch.prefixes[first] * scratch.powers[word.size() - first - 1] +
                                            substring_hash(first + 1, word.size());
                if (status_t status = scratch.residuals.try_push_back(fold_hash_(without_first));
                    status != status_t::success_k)
                    return status;
                if (distance >= 2)
                    for (size_t second = first + 1; second != word.size(); ++second) {
                        size_t const middle_length = second - first - 1;
                        size_t const suffix_length = word.size() - second - 1;
                        u64_t hash = scratch.prefixes[first] * scratch.powers[middle_length] +
                                     substring_hash(first + 1, second);
                        hash = hash * scratch.powers[suffix_length] + substring_hash(second + 1, word.size());
                        if (status_t status = scratch.residuals.try_push_back(fold_hash_(hash));
                            status != status_t::success_k)
                            return status;
                    }
            }
        std::sort(scratch.residuals.begin(), scratch.residuals.end());
        auto const unique_end = std::unique(scratch.residuals.begin(), scratch.residuals.end());
        return scratch.residuals.try_resize(static_cast<size_t>(unique_end - scratch.residuals.begin()));
    }

    span<char const> word_(u32_t id) const noexcept {
        size_t const begin = static_cast<size_t>(offsets_[id]);
        size_t const end = static_cast<size_t>(offsets_[id + 1]);
        return {tape_.data() + begin, end - begin};
    }

    static u8_t distance_within_one_(span<char const> first, span<char const> second) noexcept {
        if (first.size() == second.size() &&
            (first.empty() || std::memcmp(first.data(), second.data(), first.size()) == 0))
            return 0;
        if (first.size() + 1 < second.size() || second.size() + 1 < first.size()) return rejected_distance_k;
        size_t first_index = 0, second_index = 0;
        bool edited = false;
        while (first_index != first.size() && second_index != second.size()) {
            if (first[first_index] == second[second_index]) ++first_index, ++second_index;
            else if (edited) return rejected_distance_k;
            else {
                edited = true;
                if (first.size() > second.size()) ++first_index;
                else if (second.size() > first.size()) ++second_index;
                else ++first_index, ++second_index;
            }
        }
        size_t const tail = first.size() - first_index + second.size() - second_index;
        return static_cast<u8_t>((edited ? 1 : 0) + tail <= 1 ? 1 : rejected_distance_k);
    }

    template <typename scratch_type_>
    static u8_t distance_within_two_(span<char const> first, span<char const> second,
                                     scratch_type_ &scratch) noexcept {
        if (first.size() > second.size()) std::swap(first, second);
        if (second.size() - first.size() > 2) return rejected_distance_k;
        if (first.empty()) return second.size() <= 2 ? static_cast<u8_t>(second.size()) : rejected_distance_k;

        if (first.size() <= 64) {
            u64_t equality[256] = {};
            for (size_t index = 0; index != first.size(); ++index)
                equality[static_cast<u8_t>(first[index])] |= u64_t(1) << index;
            u64_t positive = ~u64_t(0), negative = 0;
            u64_t const top = u64_t(1) << (first.size() - 1);
            size_t score = first.size();
            for (size_t index = 0; index != second.size(); ++index) {
                u64_t const matches = equality[static_cast<u8_t>(second[index])];
                u64_t const carry_in = matches | negative;
                u64_t const differences = (((carry_in & positive) + positive) ^ positive) | carry_in;
                u64_t const horizontal_positive = negative | ~(differences | positive);
                u64_t const horizontal_negative = positive & differences;
                score += (horizontal_positive & top) != 0;
                score -= (horizontal_negative & top) != 0;
                u64_t const shifted_positive = (horizontal_positive << 1) | 1;
                u64_t const shifted_negative = horizontal_negative << 1;
                positive = shifted_negative | ~(differences | shifted_positive);
                negative = shifted_positive & differences;
                if (score > 2 + second.size() - index - 1) return rejected_distance_k;
            }
            return score <= 2 ? static_cast<u8_t>(score) : rejected_distance_k;
        }

        size_t const width = first.size() + 1;
        if (scratch.dp_rows.try_resize(width * 2) != status_t::success_k) return rejected_distance_k;
        u8_t *previous = scratch.dp_rows.data();
        u8_t *current = previous + width;
        std::fill(previous, previous + width, rejected_distance_k);
        for (size_t column = 0; column <= sz_min_of_two(first.size(), size_t(2)); ++column)
            previous[column] = static_cast<u8_t>(column);
        for (size_t row = 1; row <= second.size(); ++row) {
            std::fill(current, current + width, rejected_distance_k);
            if (row <= 2) current[0] = static_cast<u8_t>(row);
            size_t const from = row > 2 ? row - 2 : 1;
            size_t const to = sz_min_of_two(first.size(), row + 2);
            for (size_t column = from; column <= to; ++column) {
                unsigned const value = std::min(
                    {unsigned(previous[column]) + 1, unsigned(current[column - 1]) + 1,
                     unsigned(previous[column - 1]) + (first[column - 1] != second[row - 1])});
                current[column] = static_cast<u8_t>(sz_min_of_two(value, unsigned(rejected_distance_k)));
            }
            std::swap(previous, current);
        }
        return previous[first.size()] <= 2 ? previous[first.size()] : rejected_distance_k;
    }

    template <typename scratch_type_>
    u8_t verify_(u32_t id, span<char const> query, u8_t bound, scratch_type_ &scratch) const noexcept {
        span<char const> const candidate = word_(id);
        if (bound == 0)
            return candidate.size() == query.size() &&
                           (candidate.empty() || std::memcmp(candidate.data(), query.data(), query.size()) == 0)
                       ? 0
                       : rejected_distance_k;
        if (bound == 1) return distance_within_one_(candidate, query);
        return distance_within_two_(candidate, query, scratch);
    }

    status_t build_trie_() noexcept {
        size_t const words_count = size();
        vector_t<u32_t> order {alloc_}, parents {alloc_}, word_nodes {alloc_}, child_cursors {alloc_},
            terminal_cursors {alloc_}, stack {alloc_};
        vector_t<u8_t> symbols {alloc_};
        if (order.try_resize(words_count) != status_t::success_k ||
            word_nodes.try_resize(words_count) != status_t::success_k)
            return status_t::bad_alloc_k;
        for (u32_t id = 0; id != words_count; ++id) order[id] = id;
        std::sort(order.begin(), order.end(), [&](u32_t first_id, u32_t second_id) {
            span<char const> const first = word_(first_id), second = word_(second_id);
            size_t const shared = sz_min_of_two(first.size(), second.size());
            int const compared = shared ? std::memcmp(first.data(), second.data(), shared) : 0;
            if (compared != 0) return compared < 0;
            if (first.size() != second.size()) return first.size() < second.size();
            return first_id < second_id;
        });

        if (parents.try_push_back(0) != status_t::success_k || symbols.try_push_back(0) != status_t::success_k ||
            stack.try_push_back(0) != status_t::success_k)
            return status_t::bad_alloc_k;
        span<char const> previous;
        for (u32_t id : order) {
            span<char const> const word = word_(id);
            size_t common = 0, common_limit = sz_min_of_two(previous.size(), word.size());
            while (common != common_limit && previous[common] == word[common]) ++common;
            if (stack.try_resize(common + 1) != status_t::success_k) return status_t::bad_alloc_k;
            for (size_t position = common; position != word.size(); ++position) {
                if (parents.size() == std::numeric_limits<u32_t>::max()) return status_t::overflow_risk_k;
                u32_t const node = static_cast<u32_t>(parents.size());
                if (parents.try_push_back(stack.back()) != status_t::success_k ||
                    symbols.try_push_back(static_cast<u8_t>(word[position])) != status_t::success_k ||
                    stack.try_push_back(node) != status_t::success_k)
                    return status_t::bad_alloc_k;
            }
            word_nodes[id] = stack.back();
            previous = word;
        }

        size_t const nodes_count = parents.size();
        if (trie_nodes_.try_resize(nodes_count) != status_t::success_k ||
            trie_edges_.try_resize(nodes_count ? nodes_count - 1 : 0) != status_t::success_k ||
            trie_terminals_.try_resize(words_count) != status_t::success_k ||
            child_cursors.try_resize(nodes_count) != status_t::success_k ||
            terminal_cursors.try_resize(nodes_count) != status_t::success_k)
            return status_t::bad_alloc_k;
        for (size_t node = 1; node != nodes_count; ++node) ++trie_nodes_[parents[node]].edges_count;
        for (u32_t id = 0; id != words_count; ++id) ++trie_nodes_[word_nodes[id]].terminals_count;
        u32_t edge_offset = 0, terminal_offset = 0;
        for (size_t node = 0; node != nodes_count; ++node) {
            trie_nodes_[node].first_edge = edge_offset;
            trie_nodes_[node].first_terminal = terminal_offset;
            child_cursors[node] = edge_offset;
            terminal_cursors[node] = terminal_offset;
            edge_offset += trie_nodes_[node].edges_count;
            terminal_offset += trie_nodes_[node].terminals_count;
        }
        for (u32_t node = 1; node != nodes_count; ++node)
            trie_edges_[child_cursors[parents[node]]++] = trie_edge_t {node, symbols[node]};
        for (u32_t id = 0; id != words_count; ++id)
            trie_terminals_[terminal_cursors[word_nodes[id]]++] = id;
        return status_t::success_k;
    }

    template <typename scratch_type_>
    status_t find_trie_(span<char const> query, u8_t bound, bool fallback_only, scratch_type_ &scratch,
                        vector_t<match_t> &matches) const noexcept {
        if (!trie_nodes_.size()) return status_t::success_k;
        size_t const stride = size_t(2) * bound + 3;
        if (max_word_length_ + 1 > std::numeric_limits<size_t>::max() / stride)
            return status_t::overflow_risk_k;
        if (scratch.trie_rows.try_resize((max_word_length_ + 1) * stride) != status_t::success_k ||
            scratch.trie_frames.try_resize(0) != status_t::success_k ||
            scratch.trie_frames.try_reserve(max_word_length_ + 1) != status_t::success_k)
            return status_t::bad_alloc_k;
        u16_t const cap = static_cast<u16_t>(bound) + 1;
        u16_t *root_row = scratch.trie_rows.data();
        size_t const root_to = sz_min_of_two(query.size(), size_t(bound));
        for (size_t column = 0; column <= root_to; ++column) root_row[column] = static_cast<u16_t>(column);

        auto const emit_terminals = [&](u32_t node_id, u16_t distance) noexcept -> status_t {
            trie_node_t const &node = trie_nodes_[node_id];
            for (size_t offset = node.first_terminal; offset != node.first_terminal + node.terminals_count; ++offset) {
                u32_t const id = trie_terminals_[offset];
                if (fallback_only && word_(id).size() <= deletion_max_word_length_) continue;
                if (status_t status = matches.try_push_back(match_t {id, static_cast<u8_t>(distance)});
                    status != status_t::success_k)
                    return status;
            }
            return status_t::success_k;
        };
        if (query.size() <= bound)
            if (status_t status = emit_terminals(0, static_cast<u16_t>(query.size()));
                status != status_t::success_k)
                return status;

        trie_node_t const &root = trie_nodes_[0];
        if (scratch.trie_frames.try_push_back(
                trie_frame_t {0, root.first_edge, root.first_edge + root.edges_count, 0}) != status_t::success_k)
            return status_t::bad_alloc_k;
        while (scratch.trie_frames.size()) {
            trie_frame_t &frame = scratch.trie_frames.back();
            if (frame.next_edge == frame.end_edge) {
                scratch.trie_frames.try_resize(scratch.trie_frames.size() - 1);
                continue;
            }
            trie_edge_t const edge = trie_edges_[frame.next_edge++];
            size_t const previous_depth = frame.depth;
            size_t const current_depth = previous_depth + 1;
            size_t const previous_from = previous_depth > bound ? previous_depth - bound : 0;
            size_t const previous_to = sz_min_of_two(query.size(), previous_depth + bound);
            size_t const current_from = current_depth > bound ? current_depth - bound : 0;
            size_t const current_to = sz_min_of_two(query.size(), current_depth + bound);
            u16_t const *previous_row = scratch.trie_rows.data() + previous_depth * stride;
            u16_t *current_row = scratch.trie_rows.data() + current_depth * stride;
            auto const previous_at = [&](size_t column) noexcept -> u16_t {
                return column >= previous_from && column <= previous_to ? previous_row[column - previous_from] : cap;
            };
            u16_t row_min = cap;
            for (size_t column = current_from; column <= current_to; ++column) {
                u16_t value;
                if (column == 0) value = static_cast<u16_t>(sz_min_of_two(current_depth, size_t(cap)));
                else {
                    unsigned const deletion = unsigned(previous_at(column)) + 1;
                    unsigned const insertion =
                        column > current_from ? unsigned(current_row[column - current_from - 1]) + 1 : cap;
                    unsigned const substitution =
                        unsigned(previous_at(column - 1)) + (query[column - 1] != static_cast<char>(edge.symbol));
                    value = static_cast<u16_t>(sz_min_of_two(std::min({deletion, insertion, substitution}),
                                                             unsigned(cap)));
                }
                current_row[column - current_from] = value;
                row_min = sz_min_of_two(row_min, value);
            }
            if (query.size() >= current_from && query.size() <= current_to) {
                u16_t const distance = current_row[query.size() - current_from];
                if (distance <= bound)
                    if (status_t status = emit_terminals(edge.child, distance); status != status_t::success_k)
                        return status;
            }
            if (row_min <= bound) {
                trie_node_t const &child = trie_nodes_[edge.child];
                if (scratch.trie_frames.try_push_back(trie_frame_t {
                        edge.child, child.first_edge, child.first_edge + child.edges_count,
                        static_cast<u32_t>(current_depth)}) != status_t::success_k)
                    return status_t::bad_alloc_k;
            }
        }
        return status_t::success_k;
    }

    template <typename sequences_type_, typename build_scratch_type_>
    status_t build_(sequences_type_ const &dictionary, u8_t max_distance, size_t deletion_max_word_length,
                    build_scratch_type_ &scratch) noexcept {
        if (max_distance == std::numeric_limits<u8_t>::max()) return status_t::unexpected_dimensions_k;
        if (dictionary.size() > std::numeric_limits<u32_t>::max()) return status_t::overflow_risk_k;
        u8_t const indexed_distance = sz_min_of_two(max_distance, u8_t(2));
        deletion_max_word_length_ = deletion_max_word_length;
        fallback_words_count_ = 0;

        size_t tape_bytes = 0, records_upper_bound = 0;
        max_word_length_ = 0;
        for (size_t id = 0; id != dictionary.size(); ++id) {
            auto const item = dictionary[id];
            size_t const length = item.size();
            if (!checked_add_(tape_bytes, length)) return status_t::overflow_risk_k;
            if (length <= deletion_max_word_length_) {
                size_t word_records = 0;
                if (!residuals_upper_bound_(length, indexed_distance, word_records) ||
                    !checked_add_(records_upper_bound, word_records))
                    return status_t::overflow_risk_k;
            }
            else ++fallback_words_count_;
            max_word_length_ = sz_max_of_two(max_word_length_, length);
        }
        if (records_upper_bound > std::numeric_limits<u32_t>::max()) return status_t::overflow_risk_k;
        if (tape_.try_resize(tape_bytes) != status_t::success_k ||
            offsets_.try_resize(dictionary.size() + 1) != status_t::success_k ||
            wide_records_.try_reserve(records_upper_bound) != status_t::success_k)
            return status_t::bad_alloc_k;

        size_t tape_offset = 0;
        for (u32_t id = 0; id != dictionary.size(); ++id) {
            auto const item = dictionary[id];
            span<char const> const word {item.data(), item.size()};
            offsets_[id] = static_cast<u64_t>(tape_offset);
            if (!word.empty()) std::memcpy(tape_.data() + tape_offset, word.data(), word.size());
            tape_offset += word.size();
            if (word.size() <= deletion_max_word_length_) {
                if (status_t status = generate_residuals_(word, indexed_distance, scratch);
                    status != status_t::success_k)
                    return status;
                for (u32_t hash : scratch.residuals)
                    if (status_t status = wide_records_.try_push_back((u64_t(hash) << 32) | id);
                        status != status_t::success_k)
                        return status;
            }
        }
        offsets_[dictionary.size()] = static_cast<u64_t>(tape_offset);
        std::sort(wide_records_.begin(), wide_records_.end());

        if (directory_.try_resize(prefix_buckets_k + 1) != status_t::success_k) return status_t::bad_alloc_k;
        size_t cursor = 0;
        for (size_t prefix = 0; prefix != prefix_buckets_k; ++prefix) {
            directory_[prefix] = static_cast<u32_t>(cursor);
            while (cursor != wide_records_.size() && (u32_t(wide_records_[cursor] >> 32) >> suffix_bits_k) == prefix)
                ++cursor;
        }
        directory_[prefix_buckets_k] = static_cast<u32_t>(wide_records_.size());

        if (dictionary.size() <= size_t(1) << packed_id_bits_k) {
            if (packed_records_.try_resize(wide_records_.size()) != status_t::success_k)
                return status_t::bad_alloc_k;
            for (size_t index = 0; index != wide_records_.size(); ++index) {
                u32_t const hash = static_cast<u32_t>(wide_records_[index] >> 32);
                u32_t const id = static_cast<u32_t>(wide_records_[index]);
                packed_records_[index] = ((hash & suffix_mask_k) << packed_id_bits_k) | id;
            }
            wide_records_.reset();
        }
        if (max_distance > 2 || fallback_words_count_)
            if (status_t status = build_trie_(); status != status_t::success_k) return status;
        max_distance_ = max_distance;
        return status_t::success_k;
    }

  public:
    class scratch_t {
        friend class levenshtein_index;
        vector_t<u32_t> generations;
        vector_t<u32_t> residuals;
        vector_t<u64_t> prefixes;
        vector_t<u64_t> powers;
        vector_t<u8_t> dp_rows;
        vector_t<u16_t> trie_rows;
        vector_t<trie_frame_t> trie_frames;
        u32_t generation = 0;

      public:
        explicit scratch_t(allocator_t alloc = {}) noexcept
            : generations(alloc), residuals(alloc), prefixes(alloc), powers(alloc), dp_rows(alloc), trie_rows(alloc),
              trie_frames(alloc) {}
    };

    using matches_t = vector_t<match_t>;

    explicit levenshtein_index(allocator_t alloc = {}) noexcept
        : alloc_(alloc), tape_(alloc), offsets_(alloc), directory_(alloc), packed_records_(alloc),
          wide_records_(alloc), trie_nodes_(alloc), trie_edges_(alloc), trie_terminals_(alloc) {}

    template <typename sequences_type_>
    status_t try_build(sequences_type_ const &dictionary, u8_t max_distance,
                       size_t deletion_max_word_length = 64) noexcept {
        levenshtein_index candidate {alloc_};
        scratch_t scratch {alloc_};
        if (status_t status = candidate.build_(dictionary, max_distance, deletion_max_word_length, scratch);
            status != status_t::success_k)
            return status;
        *this = std::move(candidate);
        return status_t::success_k;
    }

    status_t find(span<char const> query, u8_t bound, scratch_t &scratch, matches_t &matches) const noexcept {
        if (bound > max_distance_) return status_t::unexpected_dimensions_k;
        if (status_t status = matches.try_resize(0); status != status_t::success_k) return status;
        if (bound > 2) return find_trie_(query, bound, false, scratch, matches);
        if (scratch.generations.size() != size()) {
            if (status_t status = scratch.generations.try_resize(size()); status != status_t::success_k) return status;
            std::fill(scratch.generations.begin(), scratch.generations.end(), u32_t(0));
            scratch.generation = 0;
        }
        if (++scratch.generation == 0) {
            std::fill(scratch.generations.begin(), scratch.generations.end(), u32_t(0));
            scratch.generation = 1;
        }
        if (status_t status = generate_residuals_(query, bound, scratch); status != status_t::success_k) return status;

        for (u32_t hash : scratch.residuals) {
            size_t const prefix = hash >> suffix_bits_k;
            size_t const begin_offset = directory_[prefix];
            size_t const end_offset = directory_[prefix + 1];
            if (!packed_records_.size()) {
                u64_t const key = u64_t(hash) << 32;
                u64_t const *record = std::lower_bound(wide_records_.begin() + begin_offset,
                                                       wide_records_.begin() + end_offset, key);
                u64_t const *const end = wide_records_.begin() + end_offset;
                for (; record != end && static_cast<u32_t>(*record >> 32) == hash; ++record) {
                    u32_t const id = static_cast<u32_t>(*record);
                    if (scratch.generations[id] == scratch.generation) continue;
                    scratch.generations[id] = scratch.generation;
                    u8_t const distance = verify_(id, query, bound, scratch);
                    if (distance <= bound)
                        if (status_t status = matches.try_push_back(match_t {id, distance});
                            status != status_t::success_k)
                            return status;
                }
            }
            else {
                u32_t const suffix = hash & suffix_mask_k;
                u32_t const key = suffix << packed_id_bits_k;
                u32_t const *record = std::lower_bound(packed_records_.begin() + begin_offset,
                                                       packed_records_.begin() + end_offset, key);
                u32_t const *const end = packed_records_.begin() + end_offset;
                for (; record != end && (*record >> packed_id_bits_k) == suffix; ++record) {
                    u32_t const id = *record & packed_id_mask_k;
                    if (scratch.generations[id] == scratch.generation) continue;
                    scratch.generations[id] = scratch.generation;
                    u8_t const distance = verify_(id, query, bound, scratch);
                    if (distance <= bound)
                        if (status_t status = matches.try_push_back(match_t {id, distance});
                            status != status_t::success_k)
                            return status;
                }
            }
        }
        if (fallback_words_count_)
            return find_trie_(query, bound, true, scratch, matches);
        return status_t::success_k;
    }

    size_t size() const noexcept { return offsets_.size() ? offsets_.size() - 1 : 0; }
    u8_t max_distance() const noexcept { return max_distance_; }
    size_t max_word_length() const noexcept { return max_word_length_; }
    bool uses_packed_records() const noexcept { return packed_records_.size() != 0 || size() == 0; }
    size_t records_count() const noexcept {
        return packed_records_.size() ? packed_records_.size() : wide_records_.size();
    }
    size_t index_bytes() const noexcept {
        return directory_.size() * sizeof(u32_t) + packed_records_.size() * sizeof(u32_t) +
               wide_records_.size() * sizeof(u64_t) + trie_bytes();
    }
    size_t dictionary_bytes() const noexcept {
        return tape_.size() * sizeof(char) + offsets_.size() * sizeof(u64_t);
    }
    size_t trie_bytes() const noexcept {
        return trie_nodes_.size() * sizeof(trie_node_t) + trie_edges_.size() * sizeof(trie_edge_t) +
               trie_terminals_.size() * sizeof(u32_t);
    }
};

} // namespace stringzillas
} // namespace ashvardanian

#endif // STRINGZILLAS_LEVENSHTEIN_INDEX_HPP_
