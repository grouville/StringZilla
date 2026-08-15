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
    u8_t max_distance_ = 0;
    size_t max_word_length_ = 0;

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

    template <typename sequences_type_, typename build_scratch_type_>
    status_t build_(sequences_type_ const &dictionary, u8_t max_distance, build_scratch_type_ &scratch) noexcept {
        if (max_distance > 2) return status_t::unexpected_dimensions_k;
        if (dictionary.size() > std::numeric_limits<u32_t>::max()) return status_t::overflow_risk_k;

        size_t tape_bytes = 0, records_upper_bound = 0;
        max_word_length_ = 0;
        for (size_t id = 0; id != dictionary.size(); ++id) {
            auto const item = dictionary[id];
            size_t const length = item.size();
            if (!checked_add_(tape_bytes, length)) return status_t::overflow_risk_k;
            size_t word_records = 0;
            if (!residuals_upper_bound_(length, max_distance, word_records) ||
                !checked_add_(records_upper_bound, word_records))
                return status_t::overflow_risk_k;
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
            if (status_t status = generate_residuals_(word, max_distance, scratch); status != status_t::success_k)
                return status;
            for (u32_t hash : scratch.residuals)
                if (status_t status = wide_records_.try_push_back((u64_t(hash) << 32) | id);
                    status != status_t::success_k)
                    return status;
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
        u32_t generation = 0;

      public:
        explicit scratch_t(allocator_t alloc = {}) noexcept
            : generations(alloc), residuals(alloc), prefixes(alloc), powers(alloc), dp_rows(alloc) {}
    };

    using matches_t = vector_t<match_t>;

    explicit levenshtein_index(allocator_t alloc = {}) noexcept
        : alloc_(alloc), tape_(alloc), offsets_(alloc), directory_(alloc), packed_records_(alloc),
          wide_records_(alloc) {}

    template <typename sequences_type_>
    status_t try_build(sequences_type_ const &dictionary, u8_t max_distance) noexcept {
        levenshtein_index candidate {alloc_};
        scratch_t scratch {alloc_};
        if (status_t status = candidate.build_(dictionary, max_distance, scratch); status != status_t::success_k)
            return status;
        *this = std::move(candidate);
        return status_t::success_k;
    }

    status_t find(span<char const> query, u8_t bound, scratch_t &scratch, matches_t &matches) const noexcept {
        if (bound > max_distance_) return status_t::unexpected_dimensions_k;
        if (status_t status = matches.try_resize(0); status != status_t::success_k) return status;
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
               wide_records_.size() * sizeof(u64_t);
    }
    size_t dictionary_bytes() const noexcept {
        return tape_.size() * sizeof(char) + offsets_.size() * sizeof(u64_t);
    }
};

} // namespace stringzillas
} // namespace ashvardanian

#endif // STRINGZILLAS_LEVENSHTEIN_INDEX_HPP_
