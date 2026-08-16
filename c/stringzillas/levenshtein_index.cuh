/**
 *  @file c/stringzillas/levenshtein_index.cuh
 *  @brief Exact immutable Levenshtein dictionary-index C shim.
 *  @author Guillaume de Rouville
 */
#ifndef STRINGZILLAS_SZS_LEVENSHTEIN_INDEX_CUH_
#define STRINGZILLAS_SZS_LEVENSHTEIN_INDEX_CUH_

#include "stringzillas.cuh"

#include <stringzillas/levenshtein_index.hpp>

using levenshtein_index_t = szs::levenshtein_index<>;
struct levenshtein_index_search_state_t {
    levenshtein_index_t::scratch_t scratch;
    levenshtein_index_t::matches_t matches;
};

template <typename index_type_, typename dictionary_type_, typename index_handle_type_>
sz_status_t szs_levenshtein_index_init_(                                    //
    dictionary_type_ const &dictionary, sz_size_t max_distance,             //
    sz_size_t deletion_max_word_length, sz_memory_allocator_t const *alloc, //
    index_handle_type_ *index_punned, char const **error_message) noexcept {

    sz_unused_(alloc); // Custom allocator support is not implemented yet.
    sz_assert_(index_punned != nullptr && *index_punned == nullptr && "Index must be uninitialized");
    if (max_distance >= std::numeric_limits<sz::u8_t>::max())
        return propagate_error(sz::status_t::unexpected_dimensions_k, error_message,
                               "Maximum Levenshtein distance must fit below 255");

    auto *index = new (std::nothrow) index_type_;
    if (!index)
        return propagate_error(sz::status_t::bad_alloc_k, error_message,
                               "Failed to allocate Levenshtein dictionary index");
    sz::status_t const status =
        index->try_build(dictionary, static_cast<sz::u8_t>(max_distance), deletion_max_word_length);
    if (status != sz::status_t::success_k) {
        delete index;
        return propagate_error(status, error_message, "Failed to build Levenshtein dictionary index");
    }
    *index_punned = reinterpret_cast<index_handle_type_>(index);
    return propagate_error(sz::status_t::success_k, error_message);
}

extern "C" {

SZ_API_RUNTIME sz_status_t szs_levenshtein_index_init(                        //
    sz_sequence_t const *dictionary, sz_size_t max_distance,                  //
    sz_size_t deletion_max_word_length, sz_memory_allocator_t const *alloc,   //
    szs_levenshtein_index_t *index_punned, char const **error_message) {
    sz_assert_(dictionary != nullptr && "Dictionary must not be null");
    return szs_levenshtein_index_init_<levenshtein_index_t>(sz_sequence_as_cpp_container_t {dictionary},
                                                            max_distance, deletion_max_word_length, alloc,
                                                            index_punned, error_message);
}

SZ_API_RUNTIME sz_status_t szs_levenshtein_index_init_u32tape(                 //
    sz_sequence_u32tape_t const *dictionary, sz_size_t max_distance,           //
    sz_size_t deletion_max_word_length, sz_memory_allocator_t const *alloc,    //
    szs_levenshtein_index_t *index_punned, char const **error_message) {
    sz_assert_(dictionary != nullptr && "Dictionary must not be null");
    return szs_levenshtein_index_init_<levenshtein_index_t>(sz_sequence_u32tape_as_cpp_container_t {dictionary},
                                                            max_distance, deletion_max_word_length, alloc,
                                                            index_punned, error_message);
}

SZ_API_RUNTIME sz_status_t szs_levenshtein_index_init_u64tape(                 //
    sz_sequence_u64tape_t const *dictionary, sz_size_t max_distance,           //
    sz_size_t deletion_max_word_length, sz_memory_allocator_t const *alloc,    //
    szs_levenshtein_index_t *index_punned, char const **error_message) {
    sz_assert_(dictionary != nullptr && "Dictionary must not be null");
    return szs_levenshtein_index_init_<levenshtein_index_t>(sz_sequence_u64tape_as_cpp_container_t {dictionary},
                                                            max_distance, deletion_max_word_length, alloc,
                                                            index_punned, error_message);
}

SZ_API_RUNTIME sz_status_t szs_levenshtein_index_search_init( //
    szs_levenshtein_index_t index_punned,                     //
    szs_levenshtein_index_search_t *search_punned, char const **error_message) {
    sz_assert_(index_punned != nullptr && "Index must be initialized");
    sz_assert_(search_punned != nullptr && *search_punned == nullptr && "Search state must be uninitialized");
    auto *search = new (std::nothrow) levenshtein_index_search_state_t;
    if (!search)
        return propagate_error(sz::status_t::bad_alloc_k, error_message,
                               "Failed to allocate Levenshtein index search state");
    *search_punned = reinterpret_cast<szs_levenshtein_index_search_t>(search);
    return propagate_error(sz::status_t::success_k, error_message);
}

SZ_API_RUNTIME sz_status_t szs_levenshtein_index_find(                                      //
    szs_levenshtein_index_t index_punned, szs_levenshtein_index_search_t search_punned,     //
    sz_cptr_t query, sz_size_t query_length, sz_size_t bound,                                //
    szs_levenshtein_index_match_t const **matches, sz_size_t *matches_count,                 //
    char const **error_message) {
    sz_assert_(index_punned != nullptr && "Index must be initialized");
    sz_assert_(search_punned != nullptr && "Search state must be initialized");
    sz_assert_(query != nullptr || query_length == 0);
    sz_assert_(matches != nullptr && matches_count != nullptr && "Result span outputs must not be null");
    *matches = nullptr;
    *matches_count = 0;
    if (bound >= std::numeric_limits<sz::u8_t>::max())
        return propagate_error(sz::status_t::unexpected_dimensions_k, error_message,
                               "Levenshtein search bound must fit below 255");

    auto const *index = reinterpret_cast<levenshtein_index_t const *>(index_punned);
    auto *search = reinterpret_cast<levenshtein_index_search_state_t *>(search_punned);
    sz::status_t const status = index->find({query, query_length}, static_cast<sz::u8_t>(bound), search->scratch,
                                            search->matches);
    if (status != sz::status_t::success_k)
        return propagate_error(status, error_message, "Failed to search Levenshtein dictionary index");
    *matches = search->matches.data();
    *matches_count = search->matches.size();
    return propagate_error(sz::status_t::success_k, error_message);
}

SZ_API_RUNTIME void szs_levenshtein_index_search_free(szs_levenshtein_index_search_t search_punned) {
    sz_assert_(search_punned != nullptr && "Search state must be initialized");
    delete reinterpret_cast<levenshtein_index_search_state_t *>(search_punned);
}

SZ_API_RUNTIME void szs_levenshtein_index_free(szs_levenshtein_index_t index_punned) {
    sz_assert_(index_punned != nullptr && "Index must be initialized");
    delete reinterpret_cast<levenshtein_index_t *>(index_punned);
}

} // extern "C"

#endif // STRINGZILLAS_SZS_LEVENSHTEIN_INDEX_CUH_
