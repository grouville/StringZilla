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
    return 0;
}
