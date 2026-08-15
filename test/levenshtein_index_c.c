#include <stringzillas/stringzillas.h>

#include <stddef.h>

int main(void) {
    char const dictionary_data[] = "bookbackbookboon";
    sz_u32_t const dictionary_offsets[] = {0, 4, 8, 12, 16};
    sz_sequence_u32tape_t dictionary = {dictionary_data, dictionary_offsets, 4};
    szs_levenshtein_index_t index = NULL;
    char const *error_message = NULL;
    if (szs_levenshtein_index_init_u32tape(&dictionary, 2, SZ_SIZE_MAX, NULL, &index, &error_message) !=
        sz_success_k)
        return 1;

    szs_levenshtein_index_search_t search = NULL;
    if (szs_levenshtein_index_search_init(index, &search, &error_message) != sz_success_k) return 2;
    szs_levenshtein_index_match_t const *matches = NULL;
    sz_size_t matches_count = 0;
    if (szs_levenshtein_index_find(index, search, "cook", 4, 1, &matches, &matches_count, &error_message) !=
        sz_success_k)
        return 3;
    if (matches_count != 2 || matches[0].distance != 1 || matches[1].distance != 1 ||
        !((matches[0].id == 0 && matches[1].id == 2) || (matches[0].id == 2 && matches[1].id == 0)))
        return 4;

    if (szs_levenshtein_index_find(index, search, "book", 4, 0, &matches, &matches_count, &error_message) !=
        sz_success_k)
        return 5;
    if (matches_count != 2 || matches[0].distance != 0 || matches[1].distance != 0 ||
        !((matches[0].id == 0 && matches[1].id == 2) || (matches[0].id == 2 && matches[1].id == 0)))
        return 6;

    szs_levenshtein_index_search_free(search);
    szs_levenshtein_index_free(index);

    char const utf8_dictionary_data[] = "caf\xC3\xA9" "cafe" "\xE5\x92\x96\xE5\x95\xA1"
                                        "\xE5\x92\x96\xE9\x9D\x9E";
    sz_u32_t const utf8_dictionary_offsets[] = {0, 5, 9, 15, 21};
    sz_sequence_u32tape_t utf8_dictionary = {utf8_dictionary_data, utf8_dictionary_offsets, 4};
    szs_levenshtein_index_utf8_t utf8_index = NULL;
    if (szs_levenshtein_index_utf8_init_u32tape(&utf8_dictionary, 2, SZ_SIZE_MAX, NULL, &utf8_index,
                                                &error_message) != sz_success_k)
        return 7;
    szs_levenshtein_index_utf8_search_t utf8_search = NULL;
    if (szs_levenshtein_index_utf8_search_init(utf8_index, &utf8_search, &error_message) != sz_success_k)
        return 8;
    char const coffee[] = "\xE5\x92\x96\xE5\x95\xA1";
    if (szs_levenshtein_index_utf8_find(utf8_index, utf8_search, coffee, 6, 1, &matches, &matches_count,
                                        &error_message) != sz_success_k)
        return 9;
    if (matches_count != 2 || matches[0].id != 2 || matches[0].distance != 0 || matches[1].id != 3 ||
        matches[1].distance != 1)
        return 10;
    if (szs_levenshtein_index_utf8_find(utf8_index, utf8_search, "\xF0\x9F", 2, 1, &matches, &matches_count,
                                        &error_message) != sz_invalid_utf8_k)
        return 11;
    if (matches != NULL || matches_count != 0) return 12;
    szs_levenshtein_index_utf8_search_free(utf8_search);
    szs_levenshtein_index_utf8_free(utf8_index);
    return 0;
}
