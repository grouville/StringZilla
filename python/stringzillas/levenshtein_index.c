/**
 *  @brief Immutable exact Levenshtein dictionary retrieval for Python.
 *  @file python/stringzillas/levenshtein_index.c
 *  @author Ash Vardanian
 */
#include "stringzillas.h"

typedef struct {
    PyObject ob_base;
    szs_levenshtein_index_t handle;
    szs_levenshtein_index_search_t search;
    sz_size_t max_distance;
    SZS_LOCK_FIELD_
} LevenshteinIndex;

static void LevenshteinIndex_dealloc(LevenshteinIndex *self) {
    if (self->search) {
        szs_levenshtein_index_search_free(self->search);
        self->search = NULL;
    }
    if (self->handle) {
        szs_levenshtein_index_free(self->handle);
        self->handle = NULL;
    }
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *LevenshteinIndex_new(PyTypeObject *type, PyObject *args, PyObject *kwargs) {
    LevenshteinIndex *self = (LevenshteinIndex *)type->tp_alloc(type, 0);
    if (self) {
        self->handle = NULL;
        self->search = NULL;
        self->max_distance = 0;
    }
    return (PyObject *)self;
}

static int LevenshteinIndex_init(LevenshteinIndex *self, PyObject *args, PyObject *kwargs) {
    PyObject *dictionary_obj = NULL, *max_distance_obj = NULL, *deletion_max_length_obj = NULL;
    static char *kwlist[] = {"dictionary", "max_distance", "deletion_max_word_length", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|OO", kwlist, &dictionary_obj, &max_distance_obj,
                                     &deletion_max_length_obj))
        return -1;
    if (self->handle) {
        PyErr_SetString(PyExc_RuntimeError, "LevenshteinIndex is already initialized");
        return -1;
    }

    sz_size_t max_distance = 2;
    if (max_distance_obj && max_distance_obj != Py_None) {
        max_distance = PyLong_AsSize_t(max_distance_obj);
        if (PyErr_Occurred()) return -1;
    }
    sz_size_t deletion_max_length = SZ_SIZE_MAX;
    if (deletion_max_length_obj && deletion_max_length_obj != Py_None) {
        deletion_max_length = PyLong_AsSize_t(deletion_max_length_obj);
        if (PyErr_Occurred()) return -1;
    }

    char const *error_detail = NULL;
    sz_status_t status = sz_status_unknown_k;
    sz_sequence_u32tape_t dictionary_u32tape;
    sz_sequence_u64tape_t dictionary_u64tape;
    sz_sequence_t dictionary_sequence;
    if (sz_py_export_strings_as_u32tape(dictionary_obj, &dictionary_u32tape.data, &dictionary_u32tape.offsets,
                                        &dictionary_u32tape.count))
        status = szs_levenshtein_index_init_u32tape(&dictionary_u32tape, max_distance, deletion_max_length, NULL,
                                                    &self->handle, &error_detail);
    else if (sz_py_export_strings_as_u64tape(dictionary_obj, &dictionary_u64tape.data, &dictionary_u64tape.offsets,
                                             &dictionary_u64tape.count))
        status = szs_levenshtein_index_init_u64tape(&dictionary_u64tape, max_distance, deletion_max_length, NULL,
                                                    &self->handle, &error_detail);
    else if (sz_py_export_strings_as_sequence(dictionary_obj, &dictionary_sequence))
        status = szs_levenshtein_index_init(&dictionary_sequence, max_distance, deletion_max_length, NULL,
                                            &self->handle, &error_detail);
    else {
        PyObject *items = PySequence_Fast(dictionary_obj, "dictionary must be an iterable of string-like objects");
        if (!items) return -1;
        Py_ssize_t const count = PySequence_Fast_GET_SIZE(items);
        sz_string_view_t *views = (sz_string_view_t *)PyMem_Malloc((size_t)count * sizeof(sz_string_view_t));
        if (!views && count) {
            Py_DECREF(items);
            return PyErr_NoMemory(), -1;
        }
        for (Py_ssize_t index = 0; index != count; ++index)
            if (!sz_py_export_string_like(PySequence_Fast_GET_ITEM(items, index), &views[index].start,
                                          &views[index].length)) {
                PyMem_Free(views);
                Py_DECREF(items);
                PyErr_Format(PyExc_TypeError, "dictionary item %zd is not string-like", index);
                return -1;
            }
        sz_sequence_from_string_views(views, (sz_size_t)count, &dictionary_sequence);
        status = szs_levenshtein_index_init(&dictionary_sequence, max_distance, deletion_max_length, NULL,
                                            &self->handle, &error_detail);
        PyMem_Free(views);
        Py_DECREF(items);
    }
    if (status != sz_success_k) {
        set_stringzilla_error(status, error_detail, "LevenshteinIndex construction");
        return -1;
    }
    status = szs_levenshtein_index_search_init(self->handle, &self->search, &error_detail);
    if (status != sz_success_k) {
        szs_levenshtein_index_free(self->handle);
        self->handle = NULL;
        set_stringzilla_error(status, error_detail, "LevenshteinIndex search-state construction");
        return -1;
    }
    self->max_distance = max_distance;
    return 0;
}

static PyObject *LevenshteinIndex_repr(LevenshteinIndex *self) {
    return PyUnicode_FromFormat("LevenshteinIndex(max_distance=%zu)", self->max_distance);
}

static PyObject *LevenshteinIndex_call(LevenshteinIndex *self, PyObject *args, PyObject *kwargs) {
    PyObject *query_obj = NULL, *bound_obj = NULL;
    static char *kwlist[] = {"query", "bound", NULL};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O|O", kwlist, &query_obj, &bound_obj)) return NULL;
    sz_size_t bound = self->max_distance;
    if (bound_obj && bound_obj != Py_None) {
        bound = PyLong_AsSize_t(bound_obj);
        if (PyErr_Occurred()) return NULL;
    }
    sz_cptr_t query = NULL;
    sz_size_t query_length = 0;
    if (!sz_py_export_string_like(query_obj, &query, &query_length)) {
        PyErr_SetString(PyExc_TypeError, "query must be a string-like object");
        return NULL;
    }

    szs_levenshtein_index_match_t const *matches = NULL;
    sz_size_t matches_count = 0;
    char const *error_detail = NULL;
    sz_status_t status;
    SZS_LOCK_(&self->lock);
    Py_BEGIN_ALLOW_THREADS
    status = szs_levenshtein_index_find(self->handle, self->search, query, query_length, bound, &matches,
                                        &matches_count, &error_detail);
    Py_END_ALLOW_THREADS
    if (status != sz_success_k) {
        SZS_UNLOCK_(&self->lock);
        set_stringzilla_error(status, error_detail, "LevenshteinIndex search");
        return NULL;
    }

    PyObject *result = PyList_New((Py_ssize_t)matches_count);
    if (!result) {
        SZS_UNLOCK_(&self->lock);
        return NULL;
    }
    for (sz_size_t index = 0; index != matches_count; ++index) {
        PyObject *pair = PyTuple_New(2);
        if (!pair) {
            Py_DECREF(result);
            SZS_UNLOCK_(&self->lock);
            return NULL;
        }
        PyObject *id = PyLong_FromUnsignedLong(matches[index].id);
        PyObject *distance = PyLong_FromUnsignedLong(matches[index].distance);
        if (!id || !distance) {
            Py_XDECREF(id);
            Py_XDECREF(distance);
            Py_DECREF(pair);
            Py_DECREF(result);
            SZS_UNLOCK_(&self->lock);
            return NULL;
        }
        PyTuple_SET_ITEM(pair, 0, id);
        PyTuple_SET_ITEM(pair, 1, distance);
        PyList_SET_ITEM(result, (Py_ssize_t)index, pair);
    }
    SZS_UNLOCK_(&self->lock);
    return result;
}

static char const doc_LevenshteinIndex[] =
    "LevenshteinIndex(dictionary, max_distance=2, deletion_max_word_length=None)\n\n"
    "Build an immutable exact byte-level dictionary index. Calling the object with ``(query, bound=None)`` returns "
    "an unordered list of ``(dictionary_id, distance)`` pairs. Duplicate dictionary values retain distinct IDs.";

PyTypeObject LevenshteinIndexType = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "stringzillas.LevenshteinIndex",
    .tp_doc = doc_LevenshteinIndex,
    .tp_basicsize = sizeof(LevenshteinIndex),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = LevenshteinIndex_new,
    .tp_init = (initproc)LevenshteinIndex_init,
    .tp_dealloc = (destructor)LevenshteinIndex_dealloc,
    .tp_call = (ternaryfunc)LevenshteinIndex_call,
    .tp_repr = (reprfunc)LevenshteinIndex_repr,
};
