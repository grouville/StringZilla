/**
 *  @file c/stringzillas/levenshtein_within.cuh
 *  @brief Bounded Levenshtein membership shim (CPU serial & Haswell backends).
 *  @author Ash Vardanian
 */
#ifndef STRINGZILLAS_SZS_LEVENSHTEIN_WITHIN_CUH_
#define STRINGZILLAS_SZS_LEVENSHTEIN_WITHIN_CUH_
#include "stringzillas.cuh"

/**
 *  @brief Allocates a `levenshtein_within_backends_t` holding the `engine_type_` arm built from @p ctor_args,
 *         publishes the opaque handle, and folds the bad-alloc / success status reporting that every capability
 *         arm repeats. Mirrors `emplace_levenshtein_engine`.
 */
template <typename engine_type_, typename... ctor_args_types_>
sz_status_t emplace_levenshtein_within_engine(szs_levenshtein_within_t *engine_punned, char const **error_message,
                                              ctor_args_types_ &&...ctor_args) noexcept {
    auto engine = new (std::nothrow) levenshtein_within_backends_t(
        std::in_place_type_t<engine_type_>(), engine_type_(std::forward<ctor_args_types_>(ctor_args)...));
    if (!engine)
        return propagate_error(sz::status_t::bad_alloc_k, error_message,
                               "Failed to allocate Levenshtein membership engine");
    *engine_punned = reinterpret_cast<szs_levenshtein_within_t>(engine);
    return propagate_error(sz::status_t::success_k, error_message);
}

/**
 *  @brief Cross-product (or symmetric self-similarity) dispatch for the bounded Levenshtein membership shim.
 *
 *  Sibling of `szs_levenshtein_cross_` with a boolean output matrix: builds an `szs::strided_rows<sz_u8_t>` view
 *  over the caller's @p results and invokes the C++ engine's two-set overload when @p candidates_container is
 *  non-null, or its symmetric overload when it is null. The membership engine is CPU-only, so GPU scopes report
 *  `device_code_mismatch_k` through the same visitor the distance shim uses.
 */
template <typename backends_type_, typename queries_type_, typename candidates_type_>
sz_status_t szs_levenshtein_within_cross_(                                                //
    backends_type_ *engine, szs_device_scope_t device_punned,                             //
    queries_type_ const &queries_container, candidates_type_ const *candidates_container, //
    sz_u8_t *results, sz_size_t results_row_stride, char const **error_message) {

    sz_assert_(device_punned != nullptr && "Device must be initialized");
    sz_assert_(results != nullptr && "Results must not be null");

    auto *device = reinterpret_cast<device_scope_t *>(device_punned);
    auto const queries_count = queries_container.size();
    auto const candidates_count = candidates_container != nullptr ? candidates_container->size() : queries_count;
    auto results_matrix = szs::strided_rows<sz_u8_t> {results, queries_count, candidates_count, results_row_stride};

    sz_status_t result = sz_success_k;
    auto variant_logic = [&](auto &engine_variant) {
        // CPU scopes differ only in the executor type they hand out, so one visitor covers both.
        sz::status_t const status = std::visit(
            [&](auto &scope_variant) -> sz::status_t {
                using scope_t = std::decay_t<decltype(scope_variant)>;
                if constexpr (!is_cpu_scope<scope_t>()) return sz::status_t::device_code_mismatch_k;
                else
                    return candidates_container != nullptr
                               ? engine_variant(queries_container, *candidates_container, results_matrix,
                                                get_executor(scope_variant), get_specs(scope_variant))
                               : engine_variant(queries_container, results_matrix, //
                                                get_executor(scope_variant), get_specs(scope_variant));
            },
            device->variants);
        result = propagate_error(status, error_message);
    };

    std::visit(variant_logic, engine->variants);
    return result;
}

extern "C" {

SZ_API_RUNTIME sz_status_t szs_levenshtein_within_init(  //
    sz_size_t bound, sz_memory_allocator_t const *alloc, //
    sz_capability_t capabilities,                        //
    szs_levenshtein_within_t *engine_punned, char const **error_message) {

    sz_unused_(alloc);        // Custom allocator not yet implemented, using default
    sz_unused_(capabilities); // Optional backends may be compiled out
    sz_assert_(engine_punned != nullptr && *engine_punned == nullptr && "Engine must be uninitialized");

#if SZ_USE_HASWELL
    bool const can_use_haswell = (capabilities & sz_cap_haswell_k) == sz_cap_haswell_k;
    if (can_use_haswell)
        return emplace_levenshtein_within_engine<szs::levenshtein_within_haswell_t>(engine_punned, error_message,
                                                                                    bound);
#endif // SZ_USE_HASWELL

    return emplace_levenshtein_within_engine<szs::levenshtein_within_serial_t>(engine_punned, error_message, bound);
}

SZ_API_RUNTIME sz_status_t szs_levenshtein_within(                            //
    szs_levenshtein_within_t engine_punned, szs_device_scope_t device_punned, //
    sz_sequence_t const *queries, sz_sequence_t const *candidates,            //
    sz_u8_t *results, sz_size_t results_row_stride, char const **error_message) {

    sz_assert_(engine_punned != nullptr && "Engine must be initialized");
    sz_assert_(queries != nullptr && "Query texts cannot be null");
    auto *engine = reinterpret_cast<levenshtein_within_backends_t *>(engine_punned);
    auto queries_container = sz_sequence_as_cpp_container_t {queries};
    auto candidates_container = sz_sequence_as_cpp_container_t {candidates};
    return szs_levenshtein_within_cross_(                                                                  //
        engine, device_punned, queries_container, candidates != nullptr ? &candidates_container : nullptr, //
        results, results_row_stride, error_message);
}

SZ_API_RUNTIME sz_status_t szs_levenshtein_within_u32tape(                         //
    szs_levenshtein_within_t engine_punned, szs_device_scope_t device_punned,      //
    sz_sequence_u32tape_t const *queries, sz_sequence_u32tape_t const *candidates, //
    sz_u8_t *results, sz_size_t results_row_stride, char const **error_message) {

    sz_assert_(engine_punned != nullptr && "Engine must be initialized");
    sz_assert_(queries != nullptr && "Query texts cannot be null");
    auto *engine = reinterpret_cast<levenshtein_within_backends_t *>(engine_punned);
    auto queries_container = sz_sequence_u32tape_as_cpp_container_t {queries};
    auto candidates_container = sz_sequence_u32tape_as_cpp_container_t {candidates};
    return szs_levenshtein_within_cross_(                                                                  //
        engine, device_punned, queries_container, candidates != nullptr ? &candidates_container : nullptr, //
        results, results_row_stride, error_message);
}

SZ_API_RUNTIME sz_status_t szs_levenshtein_within_u64tape(                         //
    szs_levenshtein_within_t engine_punned, szs_device_scope_t device_punned,      //
    sz_sequence_u64tape_t const *queries, sz_sequence_u64tape_t const *candidates, //
    sz_u8_t *results, sz_size_t results_row_stride, char const **error_message) {

    sz_assert_(engine_punned != nullptr && "Engine must be initialized");
    sz_assert_(queries != nullptr && "Query texts cannot be null");
    auto *engine = reinterpret_cast<levenshtein_within_backends_t *>(engine_punned);
    auto queries_container = sz_sequence_u64tape_as_cpp_container_t {queries};
    auto candidates_container = sz_sequence_u64tape_as_cpp_container_t {candidates};
    return szs_levenshtein_within_cross_(                                                                  //
        engine, device_punned, queries_container, candidates != nullptr ? &candidates_container : nullptr, //
        results, results_row_stride, error_message);
}

SZ_API_RUNTIME void szs_levenshtein_within_free(szs_levenshtein_within_t engine_punned) {
    sz_assert_(engine_punned != nullptr && "Engine must be initialized");
    auto *engine = reinterpret_cast<levenshtein_within_backends_t *>(engine_punned);
    delete engine;
}
}

#endif // STRINGZILLAS_SZS_LEVENSHTEIN_WITHIN_CUH_
