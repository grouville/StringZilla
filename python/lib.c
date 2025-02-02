/**
 *  @file       lib.c
 *  @brief      Very light-weight CPython wrapper for StringZilla, with support for memory-mapping,
 *              native Python strings, Apache Arrow collections, and more.
 *  @author     Ash Vardanian
 *  @date       July 10, 2023
 *  @copyright  Copyright (c) 2023
 *
 *  - Doesn't use PyBind11, NanoBind, Boost.Python, or any other high-level libs, only CPython API.
 *  - To minimize latency this implementation avoids `PyArg_ParseTupleAndKeywords` calls.
 */
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>    // `O_RDNLY`
#include <sys/mman.h> // `mmap`
#include <sys/stat.h> // `stat`
#include <sys/types.h>
#endif

#ifdef _MSC_VER
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#else
#include <limits.h> // `SSIZE_MAX`
#include <unistd.h> // `ssize_t`
#endif

// It seems like some Python versions forget to include a header, so we should:
// https://github.com/ashvardanian/StringZilla/actions/runs/7706636733/job/21002535521
#ifndef SSIZE_MAX
#define SSIZE_MAX (SIZE_MAX / 2)
#endif

#include <Python.h> // Core CPython interfaces

#include <stdio.h>  // `fopen`
#include <string.h> // `memset`, `memcpy`

#include <stringzilla/stringzilla.h>

#pragma region Forward Declarations

static PyTypeObject FileType;
static PyTypeObject StrType;
static PyTypeObject StrsType;

static sz_string_view_t temporary_memory = {NULL, 0};

/**
 *  @brief  Describes an on-disk file mapped into RAM, which is different from Python's
 *          native `mmap` module, as it exposes the address of the mapping in memory.
 */
typedef struct {
    PyObject_HEAD
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
        HANDLE file_handle;
    HANDLE mapping_handle;
#else
        int file_descriptor;
#endif
    sz_cptr_t start;
    sz_size_t length;
} File;

/**
 *  @brief  Type-punned StringZilla-string, that points to a slice of an existing Python `str`
 *          or a `File`.
 *
 *  When a slice is constructed, the `parent` object's reference count is being incremented to preserve lifetime.
 *  It usage in Python would look like:
 *
 *      - Str() # Empty string
 *      - Str("some-string") # Full-range slice of a Python `str`
 *      - Str(File("some-path.txt")) # Full-range view of a persisted file
 *      - Str(File("some-path.txt"), from=0, to=sys.maxint)
 */
typedef struct {
    PyObject_HEAD //
        PyObject *parent;
    sz_cptr_t start;
    sz_size_t length;
} Str;

/**
 *  @brief  Variable length Python object similar to `Tuple[Union[Str, str]]`,
 *          for faster sorting, shuffling, joins, and lookups.
 */
typedef struct {
    PyObject_HEAD

        enum {
            STRS_CONSECUTIVE_32,
            STRS_CONSECUTIVE_64,
            STRS_REORDERED,
            STRS_MULTI_SOURCE,
        } type;

    union {
        /**
         *  Simple structure resembling Apache Arrow arrays of variable length strings.
         *  When you split a `Str`, that is under 4 GB in size, this is used for space-efficiency.
         *  The `end_offsets` contains `count`-many integers marking the end offset of part at a given
         *  index. The length of consecutive elements can be determined as the difference in consecutive
         *  offsets. The starting offset of the first element is zero bytes after the `start`.
         *  Every chunk will include a separator of length `separator_length` at the end, except for the
         *  last one.
         */
        struct consecutive_slices_32bit_t {
            size_t count;
            size_t separator_length;
            PyObject *parent;
            char const *start;
            uint32_t *end_offsets;
        } consecutive_32bit;

        /**
         *  Simple structure resembling Apache Arrow arrays of variable length strings.
         *  When you split a `Str`, over 4 GB long, this structure is used to indicate chunk offsets.
         *  The `end_offsets` contains `count`-many integers marking the end offset of part at a given
         *  index. The length of consecutive elements can be determined as the difference in consecutive
         *  offsets. The starting offset of the first element is zero bytes after the `start`.
         *  Every chunk will include a separator of length `separator_length` at the end, except for the
         *  last one.
         */
        struct consecutive_slices_64bit_t {
            size_t count;
            size_t separator_length;
            PyObject *parent;
            char const *start;
            uint64_t *end_offsets;
        } consecutive_64bit;

        /**
         *  Once you sort, shuffle, or reorganize slices making up a larger string, this structure
         *  cn be used for space-efficient lookups.
         */
        struct reordered_slices_t {
            size_t count;
            PyObject *parent;
            sz_string_view_t *parts;
        } reordered;

    } data;

} Strs;

#pragma endregion

#pragma region Helpers

static sz_ptr_t temporary_memory_allocate(sz_size_t size, sz_string_view_t *existing) {
    if (existing->length < size) {
        sz_cptr_t new_start = realloc(existing->start, size);
        if (!new_start) {
            PyErr_Format(PyExc_MemoryError, "Unable to allocate memory for the Levenshtein matrix");
            return NULL;
        }
        existing->start = new_start;
        existing->length = size;
    }
    return existing->start;
}

static void temporary_memory_free(sz_ptr_t start, sz_size_t size, sz_string_view_t *existing) {}

static sz_cptr_t parts_get_start(sz_sequence_t *seq, sz_size_t i) {
    return ((sz_string_view_t const *)seq->handle)[i].start;
}

static sz_size_t parts_get_length(sz_sequence_t *seq, sz_size_t i) {
    return ((sz_string_view_t const *)seq->handle)[i].length;
}

void reverse_offsets(sz_sorted_idx_t *array, size_t length) {
    size_t i, j;
    // Swap array[i] and array[j]
    for (i = 0, j = length - 1; i < j; i++, j--) {
        sz_sorted_idx_t temp = array[i];
        array[i] = array[j];
        array[j] = temp;
    }
}

void reverse_haystacks(sz_string_view_t *array, size_t length) {
    size_t i, j;
    // Swap array[i] and array[j]
    for (i = 0, j = length - 1; i < j; i++, j--) {
        sz_string_view_t temp = array[i];
        array[i] = array[j];
        array[j] = temp;
    }
}

void apply_order(sz_string_view_t *array, sz_sorted_idx_t *order, size_t length) {
    for (size_t i = 0; i < length; ++i) {
        if (i == order[i]) continue;
        sz_string_view_t temp = array[i];
        size_t k = i, j;
        while (i != (j = (size_t)order[k])) {
            array[k] = array[j];
            order[k] = k;
            k = j;
        }
        array[k] = temp;
        order[k] = k;
    }
}

sz_bool_t export_string_like(PyObject *object, sz_cptr_t **start, sz_size_t *length) {
    if (PyUnicode_Check(object)) {
        // Handle Python str
        Py_ssize_t signed_length;
        *start = PyUnicode_AsUTF8AndSize(object, &signed_length);
        *length = (size_t)signed_length;
        return 1;
    }
    else if (PyBytes_Check(object)) {
        // Handle Python str
        Py_ssize_t signed_length;
        if (PyBytes_AsStringAndSize(object, (char **)start, &signed_length) == -1) {
            PyErr_SetString(PyExc_TypeError, "Mapping bytes failed");
            return 0;
        }
        *length = (size_t)signed_length;
        return 1;
    }
    else if (PyObject_TypeCheck(object, &StrType)) {
        Str *str = (Str *)object;
        *start = str->start;
        *length = str->length;
        return 1;
    }
    else if (PyObject_TypeCheck(object, &FileType)) {
        File *file = (File *)object;
        *start = file->start;
        *length = file->length;
        return 1;
    }
    return 0;
}

typedef void (*get_string_at_offset_t)(Strs *, Py_ssize_t, Py_ssize_t, PyObject **, char const **, size_t *);

void str_at_offset_consecutive_32bit(Strs *strs, Py_ssize_t i, Py_ssize_t count, PyObject **parent, char const **start,
                                     size_t *length) {
    uint32_t start_offset = (i == 0) ? 0 : strs->data.consecutive_32bit.end_offsets[i - 1];
    uint32_t end_offset = strs->data.consecutive_32bit.end_offsets[i];
    *start = strs->data.consecutive_32bit.start + start_offset;
    *length = end_offset - start_offset - strs->data.consecutive_32bit.separator_length * (i + 1 != count);
    *parent = strs->data.consecutive_32bit.parent;
}

void str_at_offset_consecutive_64bit(Strs *strs, Py_ssize_t i, Py_ssize_t count, PyObject **parent, char const **start,
                                     size_t *length) {
    uint64_t start_offset = (i == 0) ? 0 : strs->data.consecutive_64bit.end_offsets[i - 1];
    uint64_t end_offset = strs->data.consecutive_64bit.end_offsets[i];
    *start = strs->data.consecutive_64bit.start + start_offset;
    *length = end_offset - start_offset - strs->data.consecutive_64bit.separator_length * (i + 1 != count);
    *parent = strs->data.consecutive_64bit.parent;
}

void str_at_offset_reordered(Strs *strs, Py_ssize_t i, Py_ssize_t count, PyObject **parent, char const **start,
                             size_t *length) {
    *start = strs->data.reordered.parts[i].start;
    *length = strs->data.reordered.parts[i].length;
    *parent = strs->data.reordered.parent;
}

get_string_at_offset_t str_at_offset_getter(Strs *strs) {
    switch (strs->type) {
    case STRS_CONSECUTIVE_32: return str_at_offset_consecutive_32bit;
    case STRS_CONSECUTIVE_64: return str_at_offset_consecutive_64bit;
    case STRS_REORDERED: return str_at_offset_reordered;
    default:
        // Unsupported type
        PyErr_SetString(PyExc_TypeError, "Unsupported type for conversion");
        return NULL;
    }
}

sz_bool_t prepare_strings_for_reordering(Strs *strs) {

    // Allocate memory for reordered slices
    size_t count = 0;
    void *old_buffer = NULL;
    get_string_at_offset_t getter = NULL;
    PyObject *parent = NULL;
    switch (strs->type) {
    case STRS_CONSECUTIVE_32:
        count = strs->data.consecutive_32bit.count;
        old_buffer = strs->data.consecutive_32bit.end_offsets;
        parent = strs->data.consecutive_32bit.parent;
        getter = str_at_offset_consecutive_32bit;
        break;
    case STRS_CONSECUTIVE_64:
        count = strs->data.consecutive_64bit.count;
        old_buffer = strs->data.consecutive_64bit.end_offsets;
        parent = strs->data.consecutive_64bit.parent;
        getter = str_at_offset_consecutive_64bit;
        break;
    // Already in reordered form
    case STRS_REORDERED: return 1;
    case STRS_MULTI_SOURCE: return 1;
    default:
        // Unsupported type
        PyErr_SetString(PyExc_TypeError, "Unsupported type for conversion");
        return 0;
    }

    sz_string_view_t *new_parts = (sz_string_view_t *)malloc(count * sizeof(sz_string_view_t));
    if (new_parts == NULL) {
        PyErr_SetString(PyExc_MemoryError, "Unable to allocate memory for reordered slices");
        return 0;
    }

    // Populate the new reordered array using get_string_at_offset
    for (size_t i = 0; i < count; ++i) {
        PyObject *parent;
        char const *start;
        size_t length;
        getter(strs, (Py_ssize_t)i, count, &parent, &start, &length);
        new_parts[i].start = start;
        new_parts[i].length = length;
    }

    // Release previous used memory.
    if (old_buffer) free(old_buffer);

    // Update the Strs object
    strs->type = STRS_REORDERED;
    strs->data.reordered.count = count;
    strs->data.reordered.parts = new_parts;
    strs->data.reordered.parent = parent;
    return 1;
}

sz_bool_t prepare_strings_for_extension(Strs *strs, size_t new_parents, size_t new_parts) { return 1; }

#pragma endregion

#pragma region MemoryMappingFile

static void File_dealloc(File *self) {
#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
    if (self->start) {
        UnmapViewOfFile(self->start);
        self->start = NULL;
    }
    if (self->mapping_handle) {
        CloseHandle(self->mapping_handle);
        self->mapping_handle = NULL;
    }
    if (self->file_handle) {
        CloseHandle(self->file_handle);
        self->file_handle = NULL;
    }
#else
    if (self->start) {
        munmap(self->start, self->length);
        self->start = NULL;
        self->length = 0;
    }
    if (self->file_descriptor != 0) {
        close(self->file_descriptor);
        self->file_descriptor = 0;
    }
#endif
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *File_new(PyTypeObject *type, PyObject *positional_args, PyObject *named_args) {
    File *self;
    self = (File *)type->tp_alloc(type, 0);
    if (self == NULL) {
        PyErr_SetString(PyExc_RuntimeError, "Couldn't allocate the file handle!");
        return NULL;
    }

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
    self->file_handle = NULL;
    self->mapping_handle = NULL;
#else
    self->file_descriptor = 0;
#endif
    self->start = NULL;
    self->length = 0;
    return (PyObject *)self;
}

static int File_init(File *self, PyObject *positional_args, PyObject *named_args) {
    const char *path;
    if (!PyArg_ParseTuple(positional_args, "s", &path)) return -1;

#if defined(WIN32) || defined(_WIN32) || defined(__WIN32__) || defined(__NT__)
    self->file_handle = CreateFile(path, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
    if (self->file_handle == INVALID_HANDLE_VALUE) {
        PyErr_SetString(PyExc_RuntimeError, "Couldn't map the file!");
        return -1;
    }

    self->mapping_handle = CreateFileMapping(self->file_handle, 0, PAGE_READONLY, 0, 0, 0);
    if (self->mapping_handle == 0) {
        CloseHandle(self->file_handle);
        self->file_handle = NULL;
        PyErr_SetString(PyExc_RuntimeError, "Couldn't map the file!");
        return -1;
    }

    char *file = (char *)MapViewOfFile(self->mapping_handle, FILE_MAP_READ, 0, 0, 0);
    if (file == 0) {
        CloseHandle(self->mapping_handle);
        self->mapping_handle = NULL;
        CloseHandle(self->file_handle);
        self->file_handle = NULL;
        PyErr_SetString(PyExc_RuntimeError, "Couldn't map the file!");
        return -1;
    }
    self->start = file;
    self->length = GetFileSize(self->file_handle, 0);
#else
    struct stat sb;
    self->file_descriptor = open(path, O_RDONLY);
    if (fstat(self->file_descriptor, &sb) != 0) {
        close(self->file_descriptor);
        self->file_descriptor = 0;
        PyErr_SetString(PyExc_RuntimeError, "Can't retrieve file size!");
        return -1;
    }
    size_t file_size = sb.st_size;
    void *map = mmap(NULL, sb.st_size, PROT_READ, MAP_SHARED, self->file_descriptor, 0);
    if (map == MAP_FAILED) {
        close(self->file_descriptor);
        self->file_descriptor = 0;
        PyErr_SetString(PyExc_RuntimeError, "Couldn't map the file!");
        return -1;
    }
    self->start = map;
    self->length = file_size;
#endif

    return 0;
}

static PyMethodDef File_methods[] = { //
    {NULL, NULL, 0, NULL}};

static PyTypeObject FileType = {
    PyObject_HEAD_INIT(NULL).tp_name = "stringzilla.File",
    .tp_doc = "Memory mapped file class, that exposes the memory range for low-level access",
    .tp_basicsize = sizeof(File),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_methods = File_methods,
    .tp_new = (newfunc)File_new,
    .tp_init = (initproc)File_init,
    .tp_dealloc = (destructor)File_dealloc,
};

#pragma endregion

#pragma region Str

static int Str_init(Str *self, PyObject *args, PyObject *kwargs) {

    // Parse all arguments into PyObjects first
    Py_ssize_t nargs = PyTuple_Size(args);
    if (nargs > 3) {
        PyErr_SetString(PyExc_TypeError, "Invalid number of arguments");
        return -1;
    }
    PyObject *parent_obj = nargs >= 1 ? PyTuple_GET_ITEM(args, 0) : NULL;
    PyObject *from_obj = nargs >= 2 ? PyTuple_GET_ITEM(args, 1) : NULL;
    PyObject *to_obj = nargs >= 3 ? PyTuple_GET_ITEM(args, 2) : NULL;

    // Parse keyword arguments, if provided, and ensure no duplicates
    if (kwargs) {
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(kwargs, &pos, &key, &value)) {
            if (PyUnicode_CompareWithASCIIString(key, "parent") == 0) {
                if (parent_obj) {
                    PyErr_SetString(PyExc_TypeError, "Received `parent` both as positional and keyword argument");
                    return -1;
                }
                parent_obj = value;
            }
            else if (PyUnicode_CompareWithASCIIString(key, "from") == 0) {
                if (from_obj) {
                    PyErr_SetString(PyExc_TypeError, "Received `from` both as positional and keyword argument");
                    return -1;
                }
                from_obj = value;
            }
            else if (PyUnicode_CompareWithASCIIString(key, "to") == 0) {
                if (to_obj) {
                    PyErr_SetString(PyExc_TypeError, "Received `to` both as positional and keyword argument");
                    return -1;
                }
                to_obj = value;
            }
            else {
                PyErr_SetString(PyExc_TypeError, "Invalid keyword argument");
                return -1;
            }
        }
    }

    // Now, type-check and cast each argument
    Py_ssize_t from = 0, to = PY_SSIZE_T_MAX;
    if (from_obj) {
        from = PyLong_AsSsize_t(from_obj);
        if (from == -1 && PyErr_Occurred()) {
            PyErr_SetString(PyExc_TypeError, "The `from` argument must be an integer");
            return -1;
        }
    }
    if (to_obj) {
        to = PyLong_AsSsize_t(to_obj);
        if (to == -1 && PyErr_Occurred()) {
            PyErr_SetString(PyExc_TypeError, "The `to` argument must be an integer");
            return -1;
        }
    }

    // Handle empty string
    if (parent_obj == NULL) {
        self->start = NULL;
        self->length = 0;
    }
    // Increment the reference count of the parent
    else if (export_string_like(parent_obj, &self->start, &self->length)) {
        self->parent = parent_obj;
        Py_INCREF(parent_obj);
    }
    else {
        PyErr_SetString(PyExc_TypeError, "Unsupported parent type");
        return -1;
    }

    // Apply slicing
    size_t normalized_offset, normalized_length;
    sz_ssize_clamp_interval(self->length, from, to, &normalized_offset, &normalized_length);
    self->start = ((char *)self->start) + normalized_offset;
    self->length = normalized_length;
    return 0;
}

static PyObject *Str_new(PyTypeObject *type, PyObject *args, PyObject *kwds) {
    Str *self;
    self = (Str *)type->tp_alloc(type, 0);
    if (!self) {
        PyErr_SetString(PyExc_RuntimeError, "Couldn't allocate a Str handle!");
        return NULL;
    }

    self->parent = NULL;
    self->start = NULL;
    self->length = 0;
    return (PyObject *)self;
}

static void Str_dealloc(Str *self) {
    if (self->parent) { Py_XDECREF(self->parent); }
    else if (self->start) { free(self->start); }
    self->parent = NULL;
    Py_TYPE(self)->tp_free((PyObject *)self);
}

static PyObject *Str_str(Str *self) { return PyUnicode_FromStringAndSize(self->start, self->length); }

static Py_hash_t Str_hash(Str *self) { return (Py_hash_t)sz_hash(self->start, self->length); }

static PyObject *Str_like_hash(PyObject *self, PyObject *args, PyObject *kwargs) {
    // Check minimum arguments
    int is_member = self != NULL && PyObject_TypeCheck(self, &StrType);
    Py_ssize_t nargs = PyTuple_Size(args);
    if (nargs < !is_member || nargs > !is_member + 1 || kwargs) {
        PyErr_SetString(PyExc_TypeError, "hash() expects exactly one positional argument");
        return NULL;
    }

    PyObject *text_obj = is_member ? self : PyTuple_GET_ITEM(args, 0);
    sz_string_view_t text;

    // Validate and convert `text`
    if (!export_string_like(text_obj, &text.start, &text.length)) {
        PyErr_SetString(PyExc_TypeError, "The text argument must be string-like");
        return NULL;
    }

    sz_u64_t result = sz_hash(text.start, text.length);
    return PyLong_FromSize_t((size_t)result);
}

static Py_ssize_t Str_len(Str *self) { return self->length; }

static PyObject *Str_getitem(Str *self, Py_ssize_t i) {

    // Negative indexing
    if (i < 0) i += self->length;

    if (i < 0 || (size_t)i >= self->length) {
        PyErr_SetString(PyExc_IndexError, "Index out of range");
        return NULL;
    }

    // Assuming the underlying data is UTF-8 encoded
    return PyUnicode_FromStringAndSize(self->start + i, 1);
}

static PyObject *Str_subscript(Str *self, PyObject *key) {
    if (PySlice_Check(key)) {
        // Sanity checks
        Py_ssize_t start, stop, step;
        if (PySlice_Unpack(key, &start, &stop, &step) < 0) return NULL;
        if (PySlice_AdjustIndices(self->length, &start, &stop, step) < 0) return NULL;
        if (step != 1) {
            PyErr_SetString(PyExc_IndexError, "Efficient step is not supported");
            return NULL;
        }

        // Create a new `Str` object
        Str *self_slice = (Str *)StrType.tp_alloc(&StrType, 0);
        if (self_slice == NULL && PyErr_NoMemory()) return NULL;

        // Set its properties based on the slice
        self_slice->start = self->start + start;
        self_slice->length = stop - start;
        self_slice->parent = (PyObject *)self; // Set parent to keep it alive

        // Increment the reference count of the parent
        Py_INCREF(self);
        return (PyObject *)self_slice;
    }
    else if (PyLong_Check(key)) { return Str_getitem(self, PyLong_AsSsize_t(key)); }
    else {
        PyErr_SetString(PyExc_TypeError, "Str indices must be integers or slices");
        return NULL;
    }
}

static int Str_getbuffer(Str *self, Py_buffer *view, int flags) {
    if (view == NULL) {
        PyErr_SetString(PyExc_ValueError, "NULL view in getbuffer");
        return -1;
    }

    static Py_ssize_t itemsize[1] = {1};
    view->obj = (PyObject *)self;
    view->buf = self->start;
    view->len = self->length;
    view->readonly = 1;
    view->itemsize = sizeof(char);
    view->format = "c"; // https://docs.python.org/3/library/struct.html#format-characters
    view->ndim = 1;
    view->shape = (Py_ssize_t *)&self->length; // 1-D array, so shape is just a pointer to the length
    view->strides = itemsize;                  // strides in a 1-D array is just the item size
    view->suboffsets = NULL;
    view->internal = NULL;

    Py_INCREF(self);
    return 0;
}

static void Str_releasebuffer(PyObject *_, Py_buffer *view) {
    // This function MUST NOT decrement view->obj, since that is done automatically
    // in PyBuffer_Release() (this scheme is useful for breaking reference cycles).
    // https://docs.python.org/3/c-api/typeobj.html#c.PyBufferProcs.bf_releasebuffer
}

static int Str_in(Str *self, PyObject *arg) {

    sz_string_view_t needle_struct;
    if (!export_string_like(arg, &needle_struct.start, &needle_struct.length)) {
        PyErr_SetString(PyExc_TypeError, "Unsupported argument type");
        return -1;
    }

    return sz_find(self->start, self->length, needle_struct.start, needle_struct.length) != NULL;
}

static Py_ssize_t Strs_len(Strs *self) {
    switch (self->type) {
    case STRS_CONSECUTIVE_32: return self->data.consecutive_32bit.count;
    case STRS_CONSECUTIVE_64: return self->data.consecutive_64bit.count;
    case STRS_REORDERED: return self->data.reordered.count;
    default: return 0;
    }
}

static PyObject *Strs_getitem(Strs *self, Py_ssize_t i) {
    // Check for negative index and convert to positive
    Py_ssize_t count = Strs_len(self);
    if (i < 0) i += count;
    if (i < 0 || i >= count) {
        PyErr_SetString(PyExc_IndexError, "Index out of range");
        return NULL;
    }

    PyObject *parent = NULL;
    char const *start = NULL;
    size_t length = 0;
    get_string_at_offset_t getter = str_at_offset_getter(self);
    if (!getter) {
        PyErr_SetString(PyExc_TypeError, "Unknown Strs kind");
        return NULL;
    }
    else
        getter(self, i, count, &parent, &start, &length);

    // Create a new `Str` object
    Str *view_copy = (Str *)StrType.tp_alloc(&StrType, 0);
    if (view_copy == NULL && PyErr_NoMemory()) return NULL;

    view_copy->start = start;
    view_copy->length = length;
    view_copy->parent = parent;
    Py_INCREF(parent);
    return view_copy;
}

static PyObject *Strs_subscript(Strs *self, PyObject *key) {
    if (PySlice_Check(key)) {
        // Sanity checks
        Py_ssize_t count = Strs_len(self);
        Py_ssize_t start, stop, step;
        if (PySlice_Unpack(key, &start, &stop, &step) < 0) return NULL;
        if (PySlice_AdjustIndices(count, &start, &stop, step) < 0) return NULL;
        if (step != 1) {
            PyErr_SetString(PyExc_IndexError, "Efficient step is not supported");
            return NULL;
        }

        // Create a new `Strs` object
        Strs *self_slice = (Strs *)StrsType.tp_alloc(&StrsType, 0);
        if (self_slice == NULL && PyErr_NoMemory()) return NULL;

        // Depending on the layout, the procedure will be different.
        self_slice->type = self->type;
        switch (self->type) {

/* Usable as consecutive_logic(64bit), e.g. */
#define consecutive_logic(type)                                                                               \
    typedef index_##type##_t index_t;                                                                         \
    typedef struct consecutive_slices_##type##_t slice_t;                                                     \
    slice_t *from = &self->data.consecutive_##type;                                                           \
    slice_t *to = &self_slice->data.consecutive_##type;                                                       \
    to->count = stop - start;                                                                                 \
    to->separator_length = from->separator_length;                                                            \
    to->parent = from->parent;                                                                                \
    size_t first_length;                                                                                      \
    str_at_offset_consecutive_##type(self, start, count, &to->parent, &to->start, &first_length);             \
    index_t first_offset = to->start - from->start;                                                           \
    to->end_offsets = malloc(sizeof(index_t) * to->count);                                                    \
    if (to->end_offsets == NULL && PyErr_NoMemory()) {                                                        \
        Py_XDECREF(self_slice);                                                                               \
        return NULL;                                                                                          \
    }                                                                                                         \
    for (size_t i = 0; i != to->count; ++i) to->end_offsets[i] = from->end_offsets[i + start] - first_offset; \
    Py_INCREF(to->parent);
        case STRS_CONSECUTIVE_32: {
            typedef uint32_t index_32bit_t;
            consecutive_logic(32bit);
            break;
        }
        case STRS_CONSECUTIVE_64: {
            typedef uint64_t index_64bit_t;
            consecutive_logic(64bit);
            break;
        }
#undef consecutive_logic
        case STRS_REORDERED: {
            struct reordered_slices_t *from = &self->data.reordered;
            struct reordered_slices_t *to = &self_slice->data.reordered;
            to->count = stop - start;
            to->parent = from->parent;

            to->parts = malloc(sizeof(sz_string_view_t) * to->count);
            if (to->parts == NULL && PyErr_NoMemory()) {
                Py_XDECREF(self_slice);
                return NULL;
            }
            memcpy(to->parts, from->parts + start, sizeof(sz_string_view_t) * to->count);
            Py_INCREF(to->parent);
            break;
        }
        default:
            // Unsupported type
            PyErr_SetString(PyExc_TypeError, "Unsupported type for conversion");
            return NULL;
        }

        return (PyObject *)self_slice;
    }
    else if (PyLong_Check(key)) { return Strs_getitem(self, PyLong_AsSsize_t(key)); }
    else {
        PyErr_SetString(PyExc_TypeError, "Strs indices must be integers or slices");
        return NULL;
    }
}

// Will be called by the `PySequence_Contains`
static int Strs_contains(Str *self, PyObject *arg) { return 0; }

static PyObject *Str_richcompare(PyObject *self, PyObject *other, int op) {

    sz_cptr_t a_start = NULL, b_start = NULL;
    sz_size_t a_length = 0, b_length = 0;
    if (!export_string_like(self, &a_start, &a_length) || !export_string_like(other, &b_start, &b_length))
        Py_RETURN_NOTIMPLEMENTED;

    // Perform byte-wise comparison up to the minimum length
    sz_size_t min_length = a_length < b_length ? a_length : b_length;
    int order = memcmp(a_start, b_start, min_length);

    // If the strings are equal up to `min_length`, then the shorter string is smaller
    if (order == 0) order = (a_length > b_length) - (a_length < b_length);

    switch (op) {
    case Py_LT: return PyBool_FromLong(order < 0);
    case Py_LE: return PyBool_FromLong(order <= 0);
    case Py_EQ: return PyBool_FromLong(order == 0);
    case Py_NE: return PyBool_FromLong(order != 0);
    case Py_GT: return PyBool_FromLong(order > 0);
    case Py_GE: return PyBool_FromLong(order >= 0);
    default: Py_RETURN_NOTIMPLEMENTED;
    }
}

/**
 *  @brief  Saves a StringZilla string to disk.
 */
static PyObject *Str_write_to(PyObject *self, PyObject *args, PyObject *kwargs) {

    int is_member = self != NULL && PyObject_TypeCheck(self, &StrType);
    Py_ssize_t nargs = PyTuple_Size(args);
    if (nargs != !is_member + 1) {
        PyErr_SetString(PyExc_TypeError, "Invalid number of arguments");
        return NULL;
    }

    PyObject *text_obj = is_member ? self : PyTuple_GET_ITEM(args, 0);
    PyObject *path_obj = PyTuple_GET_ITEM(args, !is_member + 0);

    // Parse keyword arguments
    if (kwargs) {
        PyErr_Format(PyExc_TypeError, "Got an unexpected keyword argument");
        return NULL;
    }

    sz_string_view_t text;
    sz_string_view_t path;

    // Validate and convert `text` and `path`
    if (!export_string_like(text_obj, &text.start, &text.length) ||
        !export_string_like(path_obj, &path.start, &path.length)) {
        PyErr_SetString(PyExc_TypeError, "Text and path must be string-like");
        return NULL;
    }

    // There is a chance, the path isn't NULL-terminated, so copy it to a new buffer.
    // Many OSes have fairly low limit for the maximum path length.
    // On Windows its 260, but up to __around__ 32,767 characters are supported in extended API.
    // But it's better to be safe than sorry and use malloc :)
    //
    // https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation?tabs=registry
    // https://doc.owncloud.com/server/next/admin_manual/troubleshooting/path_filename_length.html
    char *path_buffer = (char *)malloc(path.length + 1);
    if (path_buffer == NULL) {
        PyErr_SetString(PyExc_MemoryError, "Unable to allocate memory for the path");
        return NULL;
    }
    memcpy(path_buffer, path.start, path.length);

    FILE *file_pointer = fopen(path_buffer, "wb");
    if (file_pointer == NULL) {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, path_buffer);
        free(path_buffer);
        return NULL;
    }

    setbuf(file_pointer, NULL); // Set the stream to unbuffered
    int status = fwrite(text.start, 1, text.length, file_pointer);
    if (status != text.length) {
        PyErr_SetFromErrnoWithFilename(PyExc_OSError, path_buffer);
        free(path_buffer);
        fclose(file_pointer);
        return NULL;
    }

    free(path_buffer);
    fclose(file_pointer);
    Py_RETURN_NONE;
}

/**
 *  @brief  Given a native StringZilla string, suggests it's offset within another native StringZilla string.
 *          Very practical when dealing with large files.
 *  @return Unsigned integer on success.
 */
static PyObject *Str_offset_within(PyObject *self, PyObject *args, PyObject *kwargs) {

    int is_member = self != NULL && PyObject_TypeCheck(self, &StrType);
    Py_ssize_t nargs = PyTuple_Size(args);
    if (nargs != !is_member + 1) {
        PyErr_SetString(PyExc_TypeError, "Invalid number of arguments");
        return NULL;
    }

    PyObject *slice_obj = is_member ? self : PyTuple_GET_ITEM(args, 0);
    PyObject *text_obj = PyTuple_GET_ITEM(args, !is_member + 0);

    // Parse keyword arguments
    if (kwargs) {
        PyErr_Format(PyExc_TypeError, "Got an unexpected keyword argument");
        return NULL;
    }

    sz_string_view_t text;
    sz_string_view_t slice;

    // Validate and convert `text` and `slice`
    if (!export_string_like(text_obj, &text.start, &text.length) ||
        !export_string_like(slice_obj, &slice.start, &slice.length)) {
        PyErr_SetString(PyExc_TypeError, "Text and slice must be string-like");
        return NULL;
    }

    if (slice.start < text.start || slice.start + slice.length > text.start + text.length) {
        PyErr_SetString(PyExc_ValueError, "The slice is not within the text bounds");
        return NULL;
    }

    return PyLong_FromSize_t((size_t)(slice.start - text.start));
}

/**
 *  @brief  Implementation function for all search-like operations, parameterized by a function callback.
 *  @return 1 on success, 0 on failure.
 */
static int _Str_find_implementation_( //
    PyObject *self, PyObject *args, PyObject *kwargs, sz_find_t finder, Py_ssize_t *offset_out,
    sz_string_view_t *haystack_out, sz_string_view_t *needle_out) {

    int is_member = self != NULL && PyObject_TypeCheck(self, &StrType);
    Py_ssize_t nargs = PyTuple_Size(args);
    if (nargs < !is_member + 1 || nargs > !is_member + 3) {
        PyErr_SetString(PyExc_TypeError, "Invalid number of arguments");
        return 0;
    }

    PyObject *haystack_obj = is_member ? self : PyTuple_GET_ITEM(args, 0);
    PyObject *needle_obj = PyTuple_GET_ITEM(args, !is_member + 0);
    PyObject *start_obj = nargs > !is_member + 1 ? PyTuple_GET_ITEM(args, !is_member + 1) : NULL;
    PyObject *end_obj = nargs > !is_member + 2 ? PyTuple_GET_ITEM(args, !is_member + 2) : NULL;

    // Parse keyword arguments
    if (kwargs) {
        Py_ssize_t pos = 0;
        PyObject *key, *value;
        while (PyDict_Next(kwargs, &pos, &key, &value)) {
            if (PyUnicode_CompareWithASCIIString(key, "start") == 0) { start_obj = value; }
            else if (PyUnicode_CompareWithASCIIString(key, "end") == 0) { end_obj = value; }
            else {
                PyErr_Format(PyExc_TypeError, "Got an unexpected keyword argument '%U'", key);
                return 0;
            }
        }
    }

    sz_string_view_t haystack;
    sz_string_view_t needle;
    Py_ssize_t start, end;

    // Validate and convert `haystack` and `needle`
    if (!export_string_like(haystack_obj, &haystack.start, &haystack.length) ||
        !export_string_like(needle_obj, &needle.start, &needle.length)) {
        PyErr_SetString(PyExc_TypeError, "Haystack and needle must be string-like");
        return 0;
    }

    // Validate and convert `start`
    if (start_obj) {
        start = PyLong_AsSsize_t(start_obj);
        if (start == -1 && PyErr_Occurred()) {
            PyErr_SetString(PyExc_TypeError, "The start argument must be an integer");
            return 0;
        }
    }
    else { start = 0; }

    // Validate and convert `end`
    if (end_obj) {
        end = PyLong_AsSsize_t(end_obj);
        if (end == -1 && PyErr_Occurred()) {
            PyErr_SetString(PyExc_TypeError, "The end argument must be an integer");
            return 0;
        }
    }
    else { end = PY_SSIZE_T_MAX; }

    // Limit the `haystack` range
    size_t normalized_offset, normalized_length;
    sz_ssize_clamp_interval(haystack.length, start, end, &normalized_offset, &normalized_length);
    haystack.start += normalized_offset;
    haystack.length = normalized_length;

    // Perform contains operation
    sz_cptr_t match = finder(haystack.start, haystack.length, needle.start, needle.length);
    if (match == NULL) { *offset_out = -1; }
    else { *offset_out = (Py_ssize_t)(match - haystack.start + normalized_offset); }

    *haystack_out = haystack;
    *needle_out = needle;
    return 1;
}

static PyObject *Str_contains(PyObject *self, PyObject *args, PyObject *kwargs) {
    Py_ssize_t signed_offset;
    sz_string_view_t text;
    sz_string_view_t separator;
    if (!_Str_find_implementation_(self, args, kwargs, &sz_find, &signed_offset, &text, &separator)) return NULL;
    if (signed_offset == -1) { Py_RETURN_FALSE; }
    else { Py_RETURN_TRUE; }
}

static PyObject *Str_find(PyObject *self, PyObject *args, PyObject *kwargs) {
    Py_ssize_t signed_offset;
    sz_string_view_t text;
    sz_string_view_t separator;
    if (!_Str_find_implementation_(self, args, kwargs, &sz_find, &signed_offset, &text, &separator)) return NULL;
    return PyLong_FromSsize_t(signed_offset);
}

static PyObject *Str_index(PyObject *self, PyObject *args, PyObject *kwargs) {
    Py_ssize_t signed_offset;
    sz_string_view_t text;
    sz_string_view_t separator;
    if (!_Str_find_implementation_(self, args, kwargs, &sz_find, &signed_offset, &text, &separator)) return NULL;
    if (signed_offset == -1) {
        PyErr_SetString(PyExc_ValueError, "substring not found");
        return NULL;
    }
    return PyLong_FromSsize_t(signed_offset);
}

static PyObject *Str_rfind(PyObject *self, PyObject *args, PyObject *kwargs) {
    Py_ssize_t signed_offset;
    sz_string_view_t text;
    sz_string_view_t separator;
    if (!_Str_find_implementation_(self, args, kwargs, &sz_rfind, &signed_offset, &text, &separator)) return NULL;
    return PyLong_FromSsize_t(signed_offset);
}

static PyObject *Str_rindex(PyObject *self, PyObject *args, PyObject *kwargs) {
    Py_ssize_t signed_offset;
    sz_string_view_t text;
    sz_string_view_t separator;
    if (!_Str_find_implementation_(self, args, kwargs, &sz_rfind, &signed_offset, &text, &separator)) return NULL;
    if (signed_offset == -1) {
        PyErr_SetString(PyExc_ValueError, "substring not found");
        return NULL;
    }
    return PyLong_FromSsize_t(signed_offset);
}

static PyObject *_Str_partition_implementation(PyObject *self, PyObject *args, PyObject *kwargs, sz_find_t finder) {
    Py_ssize_t separator_index;
    sz_string_view_t text;
    sz_string_view_t separator;
    PyObject *result_tuple;

    // Use _Str_find_implementation_ to get the index of the separator
    if (!_Str_find_implementation_(self, args, kwargs, finder, &separator_index, &text, &separator)) return NULL;

    // If separator is not found, return a tuple (self, "", "")
    if (separator_index == -1) {
        PyObject *empty_str1 = Str_new(&StrType, Py_None, Py_None);
        PyObject *empty_str2 = Str_new(&StrType, Py_None, Py_None);

        result_tuple = PyTuple_New(3);
        Py_INCREF(self);
        PyTuple_SET_ITEM(result_tuple, 0, self);
        PyTuple_SET_ITEM(result_tuple, 1, empty_str1);
        PyTuple_SET_ITEM(result_tuple, 2, empty_str2);
        return result_tuple;
    }

    // Create the three parts manually
    Str *before = Str_new(&StrType, NULL, NULL);
    Str *middle = Str_new(&StrType, NULL, NULL);
    Str *after = Str_new(&StrType, NULL, NULL);

    before->parent = self, before->start = text.start, before->length = separator_index;
    middle->parent = self, middle->start = text.start + separator_index, middle->length = separator.length;
    after->parent = self, after->start = text.start + separator_index + separator.length,
    after->length = text.length - separator_index - separator.length;

    // All parts reference the same parent
    Py_INCREF(self);
    Py_INCREF(self);
    Py_INCREF(self);

    // Build the result tuple
    result_tuple = PyTuple_New(3);
    PyTuple_SET_ITEM(result_tuple, 0, before);
    PyTuple_SET_ITEM(result_tuple, 1, middle);
    PyTuple_SET_ITEM(result_tuple, 2, after);

    return result_tuple;
}

static PyObject *Str_partition(PyObject *self, PyObject *args, PyObject *kwargs) {
    return _Str_partition_implementation(self, args, kwargs, &sz_find);
}

static PyObject *Str_rpartition(PyObject *self, PyObject *args, PyObject *kwargs) {
    return _Str_partition_implementation(self, args, kwargs, &sz_rfind);
}

static PyObject *Str_count(PyObject *self, PyObject *args, PyObject *kwargs) {
    int is_member = self != NULL && PyObject_TypeCheck(self, &StrType);
    Py_ssize_t nargs = PyTuple_Size(args);
    if (nargs < !is_member + 1 || nargs > !is_member + 4) {
        PyErr_Format(PyExc_TypeError, "Invalid number of arguments");
        return NULL;
    }

    PyObject *haystack_obj = is_member ? self : PyTuple_GET_ITEM(args, 0);
    PyObject *needle_obj = PyTuple_GET_ITEM(args, !is_member + 0);
    PyObject *start_obj = nargs > !is_member + 1 ? PyTuple_GET_ITEM(args, !is_member + 1) : NULL;
    PyObject *end_obj = nargs > !is_member + 2 ? PyTuple_GET_ITEM(args, !is_member + 2) : NULL;
    PyObject *allowoverlap_obj = nargs > !is_member + 3 ? PyTuple_GET_ITEM(args, !is_member + 3) : NULL;

    if (kwargs) {
        Py_ssize_t pos = 0;
        PyObject *key, *value;
        while (PyDict_Next(kwargs, &pos, &key, &value))
            if (PyUnicode_CompareWithASCIIString(key, "start") == 0) { start_obj = value; }
            else if (PyUnicode_CompareWithASCIIString(key, "end") == 0) { end_obj = value; }
            else if (PyUnicode_CompareWithASCIIString(key, "allowoverlap") == 0) { allowoverlap_obj = value; }
            else if (PyErr_Format(PyExc_TypeError, "Got an unexpected keyword argument '%U'", key))
                return NULL;
    }

    sz_string_view_t haystack;
    sz_string_view_t needle;
    Py_ssize_t start = start_obj ? PyLong_AsSsize_t(start_obj) : 0;
    Py_ssize_t end = end_obj ? PyLong_AsSsize_t(end_obj) : PY_SSIZE_T_MAX;
    int allowoverlap = allowoverlap_obj ? PyObject_IsTrue(allowoverlap_obj) : 0;

    if (!export_string_like(haystack_obj, &haystack.start, &haystack.length) ||
        !export_string_like(needle_obj, &needle.start, &needle.length))
        return PyErr_Format(PyExc_TypeError, "Haystack and needle must be string-like"), NULL;

    if ((start == -1 || end == -1 || allowoverlap == -1) && PyErr_Occurred()) return NULL;

    size_t normalized_offset, normalized_length;
    sz_ssize_clamp_interval(haystack.length, start, end, &normalized_offset, &normalized_length);
    haystack.start += normalized_offset;
    haystack.length = normalized_length;

    size_t count = 0;
    if (needle.length == 0 || haystack.length == 0 || haystack.length < needle.length) { count = 0; }
    else if (allowoverlap) {
        while (haystack.length) {
            sz_cptr_t ptr = sz_find(haystack.start, haystack.length, needle.start, needle.length);
            sz_bool_t found = ptr != NULL;
            sz_size_t offset = found ? ptr - haystack.start : haystack.length;
            count += found;
            haystack.start += offset + found;
            haystack.length -= offset + found;
        }
    }
    else {
        while (haystack.length) {
            sz_cptr_t ptr = sz_find(haystack.start, haystack.length, needle.start, needle.length);
            sz_bool_t found = ptr != NULL;
            sz_size_t offset = found ? ptr - haystack.start : haystack.length;
            count += found;
            haystack.start += offset + needle.length;
            haystack.length -= offset + needle.length * found;
        }
    }

    return PyLong_FromSize_t(count);
}

static PyObject *_Str_edit_distance(PyObject *self, PyObject *args, PyObject *kwargs, sz_edit_distance_t function) {
    int is_member = self != NULL && PyObject_TypeCheck(self, &StrType);
    Py_ssize_t nargs = PyTuple_Size(args);
    if (nargs < !is_member + 1 || nargs > !is_member + 2) {
        PyErr_Format(PyExc_TypeError, "Invalid number of arguments");
        return NULL;
    }

    PyObject *str1_obj = is_member ? self : PyTuple_GET_ITEM(args, 0);
    PyObject *str2_obj = PyTuple_GET_ITEM(args, !is_member + 0);
    PyObject *bound_obj = nargs > !is_member + 1 ? PyTuple_GET_ITEM(args, !is_member + 1) : NULL;

    if (kwargs) {
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(kwargs, &pos, &key, &value))
            if (PyUnicode_CompareWithASCIIString(key, "bound") == 0) {
                if (bound_obj) {
                    PyErr_Format(PyExc_TypeError, "Received bound both as positional and keyword argument");
                    return NULL;
                }
                bound_obj = value;
            }
    }

    Py_ssize_t bound = 0; // Default value for bound
    if (bound_obj && ((bound = PyLong_AsSsize_t(bound_obj)) < 0)) {
        PyErr_Format(PyExc_ValueError, "Bound must be a non-negative integer");
        return NULL;
    }

    sz_string_view_t str1, str2;
    if (!export_string_like(str1_obj, &str1.start, &str1.length) ||
        !export_string_like(str2_obj, &str2.start, &str2.length)) {
        PyErr_Format(PyExc_TypeError, "Both arguments must be string-like");
        return NULL;
    }

    // Allocate memory for the Levenshtein matrix
    sz_memory_allocator_t reusing_allocator;
    reusing_allocator.allocate = &temporary_memory_allocate;
    reusing_allocator.free = &temporary_memory_free;
    reusing_allocator.handle = &temporary_memory;

    sz_size_t distance =
        function(str1.start, str1.length, str2.start, str2.length, (sz_size_t)bound, &reusing_allocator);

    // Check for memory allocation issues
    if (distance == SZ_SIZE_MAX) {
        PyErr_NoMemory();
        return NULL;
    }

    return PyLong_FromSize_t(distance);
}

static PyObject *Str_edit_distance(PyObject *self, PyObject *args, PyObject *kwargs) {
    return _Str_edit_distance(self, args, kwargs, &sz_edit_distance);
}

static PyObject *Str_edit_distance_unicode(PyObject *self, PyObject *args, PyObject *kwargs) {
    return _Str_edit_distance(self, args, kwargs, &sz_edit_distance_utf8);
}

static PyObject *_Str_hamming_distance(PyObject *self, PyObject *args, PyObject *kwargs,
                                       sz_hamming_distance_t function) {
    int is_member = self != NULL && PyObject_TypeCheck(self, &StrType);
    Py_ssize_t nargs = PyTuple_Size(args);
    if (nargs < !is_member + 1 || nargs > !is_member + 2) {
        PyErr_Format(PyExc_TypeError, "Invalid number of arguments");
        return NULL;
    }

    PyObject *str1_obj = is_member ? self : PyTuple_GET_ITEM(args, 0);
    PyObject *str2_obj = PyTuple_GET_ITEM(args, !is_member + 0);
    PyObject *bound_obj = nargs > !is_member + 1 ? PyTuple_GET_ITEM(args, !is_member + 1) : NULL;

    if (kwargs) {
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(kwargs, &pos, &key, &value))
            if (PyUnicode_CompareWithASCIIString(key, "bound") == 0) {
                if (bound_obj) {
                    PyErr_Format(PyExc_TypeError, "Received bound both as positional and keyword argument");
                    return NULL;
                }
                bound_obj = value;
            }
    }

    Py_ssize_t bound = 0; // Default value for bound
    if (bound_obj && ((bound = PyLong_AsSsize_t(bound_obj)) < 0)) {
        PyErr_Format(PyExc_ValueError, "Bound must be a non-negative integer");
        return NULL;
    }

    sz_string_view_t str1, str2;
    if (!export_string_like(str1_obj, &str1.start, &str1.length) ||
        !export_string_like(str2_obj, &str2.start, &str2.length)) {
        PyErr_Format(PyExc_TypeError, "Both arguments must be string-like");
        return NULL;
    }

    sz_size_t distance = function(str1.start, str1.length, str2.start, str2.length, (sz_size_t)bound);

    // Check for memory allocation issues
    if (distance == SZ_SIZE_MAX) {
        PyErr_NoMemory();
        return NULL;
    }

    return PyLong_FromSize_t(distance);
}

static PyObject *Str_hamming_distance(PyObject *self, PyObject *args, PyObject *kwargs) {
    return _Str_hamming_distance(self, args, kwargs, &sz_hamming_distance);
}

static PyObject *Str_hamming_distance_unicode(PyObject *self, PyObject *args, PyObject *kwargs) {
    return _Str_hamming_distance(self, args, kwargs, &sz_hamming_distance_utf8);
}

static PyObject *Str_alignment_score(PyObject *self, PyObject *args, PyObject *kwargs) {
    int is_member = self != NULL && PyObject_TypeCheck(self, &StrType);
    Py_ssize_t nargs = PyTuple_Size(args);
    if (nargs < !is_member + 1 || nargs > !is_member + 2) {
        PyErr_Format(PyExc_TypeError, "Invalid number of arguments");
        return NULL;
    }

    PyObject *str1_obj = is_member ? self : PyTuple_GET_ITEM(args, 0);
    PyObject *str2_obj = PyTuple_GET_ITEM(args, !is_member + 0);
    PyObject *substitutions_obj = nargs > !is_member + 1 ? PyTuple_GET_ITEM(args, !is_member + 1) : NULL;
    PyObject *gap_obj = nargs > !is_member + 2 ? PyTuple_GET_ITEM(args, !is_member + 2) : NULL;

    if (kwargs) {
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(kwargs, &pos, &key, &value))
            if (PyUnicode_CompareWithASCIIString(key, "gap_score") == 0) {
                if (gap_obj) {
                    PyErr_Format(PyExc_TypeError, "Received the `gap_score` both as positional and keyword argument");
                    return NULL;
                }
                gap_obj = value;
            }
            else if (PyUnicode_CompareWithASCIIString(key, "substitution_matrix") == 0) {
                if (substitutions_obj) {
                    PyErr_Format(PyExc_TypeError,
                                 "Received the `substitution_matrix` both as positional and keyword argument");
                    return NULL;
                }
                substitutions_obj = value;
            }
    }

    Py_ssize_t gap = 1; // Default value for gap costs
    if (gap_obj && (gap = PyLong_AsSsize_t(gap_obj)) && (gap >= 128 || gap <= -128)) {
        PyErr_Format(PyExc_ValueError, "The `gap_score` must fit into an 8-bit signed integer");
        return NULL;
    }

    // Now extract the substitution matrix from the `substitutions_obj`.
    // It must conform to the buffer protocol, and contain a continuous 256x256 matrix of 8-bit signed integers.
    sz_error_cost_t const *substitutions;

    // Ensure the substitution matrix object is provided
    if (!substitutions_obj) {
        PyErr_Format(PyExc_TypeError, "No substitution matrix provided");
        return NULL;
    }

    // Request a buffer view
    Py_buffer substitutions_view;
    if (PyObject_GetBuffer(substitutions_obj, &substitutions_view, PyBUF_FULL)) {
        PyErr_Format(PyExc_TypeError, "Failed to get buffer from substitution matrix");
        return NULL;
    }

    // Validate the buffer
    if (substitutions_view.ndim != 2 || substitutions_view.shape[0] != 256 || substitutions_view.shape[1] != 256 ||
        substitutions_view.itemsize != sizeof(sz_error_cost_t)) {
        PyErr_Format(PyExc_ValueError, "Substitution matrix must be a 256x256 matrix of 8-bit signed integers");
        PyBuffer_Release(&substitutions_view);
        return NULL;
    }

    sz_string_view_t str1, str2;
    if (!export_string_like(str1_obj, &str1.start, &str1.length) ||
        !export_string_like(str2_obj, &str2.start, &str2.length)) {
        PyErr_Format(PyExc_TypeError, "Both arguments must be string-like");
        return NULL;
    }

    // Assign the buffer's data to substitutions
    substitutions = (sz_error_cost_t const *)substitutions_view.buf;

    // Allocate memory for the Levenshtein matrix
    sz_memory_allocator_t reusing_allocator;
    reusing_allocator.allocate = &temporary_memory_allocate;
    reusing_allocator.free = &temporary_memory_free;
    reusing_allocator.handle = &temporary_memory;

    sz_ssize_t score = sz_alignment_score(str1.start, str1.length, str2.start, str2.length, substitutions,
                                          (sz_error_cost_t)gap, &reusing_allocator);

    // Don't forget to release the buffer view
    PyBuffer_Release(&substitutions_view);

    // Check for memory allocation issues
    if (score == SZ_SSIZE_MAX) {
        PyErr_NoMemory();
        return NULL;
    }

    return PyLong_FromSsize_t(score);
}

static PyObject *Str_startswith(PyObject *self, PyObject *args, PyObject *kwargs) {
    int is_member = self != NULL && PyObject_TypeCheck(self, &StrType);
    Py_ssize_t nargs = PyTuple_Size(args);
    if (nargs < !is_member + 1 || nargs > !is_member + 3) {
        PyErr_Format(PyExc_TypeError, "Invalid number of arguments");
        return NULL;
    }

    PyObject *str_obj = is_member ? self : PyTuple_GET_ITEM(args, 0);
    PyObject *prefix_obj = PyTuple_GET_ITEM(args, !is_member);
    PyObject *start_obj = nargs > !is_member + 1 ? PyTuple_GET_ITEM(args, !is_member + 1) : NULL;
    PyObject *end_obj = nargs > !is_member + 2 ? PyTuple_GET_ITEM(args, !is_member + 2) : NULL;

    // Optional start and end arguments
    Py_ssize_t start = 0, end = PY_SSIZE_T_MAX;

    if (start_obj && ((start = PyLong_AsSsize_t(start_obj)) == -1 && PyErr_Occurred())) {
        PyErr_SetString(PyExc_TypeError, "start must be an integer");
        return NULL;
    }

    if (end_obj && ((end = PyLong_AsSsize_t(end_obj)) == -1 && PyErr_Occurred())) {
        PyErr_SetString(PyExc_TypeError, "end must be an integer");
        return NULL;
    }

    sz_string_view_t str, prefix;
    if (!export_string_like(str_obj, &str.start, &str.length) ||
        !export_string_like(prefix_obj, &prefix.start, &prefix.length)) {
        PyErr_SetString(PyExc_TypeError, "Both arguments must be string-like");
        return NULL;
    }

    // Apply start and end arguments
    str.start += start;
    str.length -= start;
    if (end != PY_SSIZE_T_MAX && end - start < str.length) { str.length = end - start; }

    if (str.length < prefix.length) { Py_RETURN_FALSE; }
    else if (strncmp(str.start, prefix.start, prefix.length) == 0) { Py_RETURN_TRUE; }
    else { Py_RETURN_FALSE; }
}

static PyObject *Str_endswith(PyObject *self, PyObject *args, PyObject *kwargs) {
    int is_member = self != NULL && PyObject_TypeCheck(self, &StrType);
    Py_ssize_t nargs = PyTuple_Size(args);
    if (nargs < !is_member + 1 || nargs > !is_member + 3) {
        PyErr_Format(PyExc_TypeError, "Invalid number of arguments");
        return NULL;
    }

    PyObject *str_obj = is_member ? self : PyTuple_GET_ITEM(args, 0);
    PyObject *suffix_obj = PyTuple_GET_ITEM(args, !is_member);
    PyObject *start_obj = nargs > !is_member + 1 ? PyTuple_GET_ITEM(args, !is_member + 1) : NULL;
    PyObject *end_obj = nargs > !is_member + 2 ? PyTuple_GET_ITEM(args, !is_member + 2) : NULL;

    // Optional start and end arguments
    Py_ssize_t start = 0, end = PY_SSIZE_T_MAX;

    if (start_obj && ((start = PyLong_AsSsize_t(start_obj)) == -1 && PyErr_Occurred())) {
        PyErr_SetString(PyExc_TypeError, "start must be an integer");
        return NULL;
    }

    if (end_obj && ((end = PyLong_AsSsize_t(end_obj)) == -1 && PyErr_Occurred())) {
        PyErr_SetString(PyExc_TypeError, "end must be an integer");
        return NULL;
    }

    sz_string_view_t str, suffix;
    if (!export_string_like(str_obj, &str.start, &str.length) ||
        !export_string_like(suffix_obj, &suffix.start, &suffix.length)) {
        PyErr_SetString(PyExc_TypeError, "Both arguments must be string-like");
        return NULL;
    }

    // Apply start and end arguments
    str.start += start;
    str.length -= start;
    if (end != PY_SSIZE_T_MAX && end - start < str.length) { str.length = end - start; }

    if (str.length < suffix.length) { Py_RETURN_FALSE; }
    else if (strncmp(str.start + (str.length - suffix.length), suffix.start, suffix.length) == 0) { Py_RETURN_TRUE; }
    else { Py_RETURN_FALSE; }
}

static PyObject *Str_find_first_of(PyObject *self, PyObject *args, PyObject *kwargs) {
    Py_ssize_t signed_offset;
    sz_string_view_t text;
    sz_string_view_t separator;
    if (!_Str_find_implementation_(self, args, kwargs, &sz_find_char_from, &signed_offset, &text, &separator))
        return NULL;
    return PyLong_FromSsize_t(signed_offset);
}

static PyObject *Str_find_first_not_of(PyObject *self, PyObject *args, PyObject *kwargs) {
    Py_ssize_t signed_offset;
    sz_string_view_t text;
    sz_string_view_t separator;
    if (!_Str_find_implementation_(self, args, kwargs, &sz_find_char_not_from, &signed_offset, &text, &separator))
        return NULL;
    return PyLong_FromSsize_t(signed_offset);
}

static PyObject *Str_find_last_of(PyObject *self, PyObject *args, PyObject *kwargs) {
    Py_ssize_t signed_offset;
    sz_string_view_t text;
    sz_string_view_t separator;
    if (!_Str_find_implementation_(self, args, kwargs, &sz_rfind_char_from, &signed_offset, &text, &separator))
        return NULL;
    return PyLong_FromSsize_t(signed_offset);
}

static PyObject *Str_find_last_not_of(PyObject *self, PyObject *args, PyObject *kwargs) {
    Py_ssize_t signed_offset;
    sz_string_view_t text;
    sz_string_view_t separator;
    if (!_Str_find_implementation_(self, args, kwargs, &sz_rfind_char_not_from, &signed_offset, &text, &separator))
        return NULL;
    return PyLong_FromSsize_t(signed_offset);
}

static Strs *Str_split_(PyObject *parent, sz_string_view_t text, sz_string_view_t separator, int keepseparator,
                        Py_ssize_t maxsplit) {
    // Create Strs object
    Strs *result = (Strs *)PyObject_New(Strs, &StrsType);
    if (!result) return NULL;

    // Initialize Strs object based on the splitting logic
    void *offsets_endings = NULL;
    size_t offsets_capacity = 0;
    size_t offsets_count = 0;
    size_t bytes_per_offset;
    if (text.length >= UINT32_MAX) {
        bytes_per_offset = 8;
        result->type = STRS_CONSECUTIVE_64;
        result->data.consecutive_64bit.start = text.start;
        result->data.consecutive_64bit.parent = parent;
        result->data.consecutive_64bit.separator_length = !keepseparator * separator.length;
    }
    else {
        bytes_per_offset = 4;
        result->type = STRS_CONSECUTIVE_32;
        result->data.consecutive_32bit.start = text.start;
        result->data.consecutive_32bit.parent = parent;
        result->data.consecutive_32bit.separator_length = !keepseparator * separator.length;
    }

    // Iterate through string, keeping track of the
    sz_size_t last_start = 0;
    while (last_start <= text.length && offsets_count < maxsplit) {
        sz_cptr_t match = sz_find(text.start + last_start, text.length - last_start, separator.start, separator.length);
        sz_size_t offset_in_remaining = match ? match - text.start - last_start : text.length - last_start;

        // Reallocate offsets array if needed
        if (offsets_count >= offsets_capacity) {
            offsets_capacity = (offsets_capacity + 1) * 2;
            void *new_offsets = realloc(offsets_endings, offsets_capacity * bytes_per_offset);
            if (!new_offsets) {
                if (offsets_endings) free(offsets_endings);
            }
            offsets_endings = new_offsets;
        }

        // If the memory allocation has failed - discard the response
        if (!offsets_endings) {
            Py_XDECREF(result);
            PyErr_NoMemory();
            return NULL;
        }

        // Export the offset
        size_t will_continue = match != NULL;
        size_t next_offset = last_start + offset_in_remaining + separator.length * will_continue;
        if (text.length >= UINT32_MAX) { ((uint64_t *)offsets_endings)[offsets_count++] = (uint64_t)next_offset; }
        else { ((uint32_t *)offsets_endings)[offsets_count++] = (uint32_t)next_offset; }

        // Next time we want to start
        last_start = last_start + offset_in_remaining + separator.length;
    }

    // Populate the Strs object with the offsets
    if (text.length >= UINT32_MAX) {
        result->data.consecutive_64bit.end_offsets = offsets_endings;
        result->data.consecutive_64bit.count = offsets_count;
    }
    else {
        result->data.consecutive_32bit.end_offsets = offsets_endings;
        result->data.consecutive_32bit.count = offsets_count;
    }

    Py_INCREF(parent);
    return result;
}

static PyObject *Str_split(PyObject *self, PyObject *args, PyObject *kwargs) {
    // Check minimum arguments
    int is_member = self != NULL && PyObject_TypeCheck(self, &StrType);
    Py_ssize_t nargs = PyTuple_Size(args);
    if (nargs < !is_member || nargs > !is_member + 3) {
        PyErr_SetString(PyExc_TypeError, "sz.split() received unsupported number of arguments");
        return NULL;
    }

    PyObject *text_obj = is_member ? self : PyTuple_GET_ITEM(args, 0);
    PyObject *separator_obj = nargs > !is_member + 0 ? PyTuple_GET_ITEM(args, !is_member + 0) : NULL;
    PyObject *maxsplit_obj = nargs > !is_member + 1 ? PyTuple_GET_ITEM(args, !is_member + 1) : NULL;
    PyObject *keepseparator_obj = nargs > !is_member + 2 ? PyTuple_GET_ITEM(args, !is_member + 2) : NULL;

    if (kwargs) {
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(kwargs, &pos, &key, &value)) {
            if (PyUnicode_CompareWithASCIIString(key, "separator") == 0) { separator_obj = value; }
            else if (PyUnicode_CompareWithASCIIString(key, "maxsplit") == 0) { maxsplit_obj = value; }
            else if (PyUnicode_CompareWithASCIIString(key, "keepseparator") == 0) { keepseparator_obj = value; }
            else if (PyErr_Format(PyExc_TypeError, "Got an unexpected keyword argument '%U'", key))
                return NULL;
        }
    }

    sz_string_view_t text;
    sz_string_view_t separator;
    int keepseparator;
    Py_ssize_t maxsplit;

    // Validate and convert `text`
    if (!export_string_like(text_obj, &text.start, &text.length)) {
        PyErr_SetString(PyExc_TypeError, "The text argument must be string-like");
        return NULL;
    }

    // Validate and convert `separator`
    if (separator_obj) {
        if (!export_string_like(separator_obj, &separator.start, &separator.length)) {
            PyErr_SetString(PyExc_TypeError, "The separator argument must be string-like");
            return NULL;
        }
    }
    else {
        separator.start = " ";
        separator.length = 1;
    }

    // Validate and convert `keepseparator`
    if (keepseparator_obj) {
        keepseparator = PyObject_IsTrue(keepseparator_obj);
        if (keepseparator == -1) {
            PyErr_SetString(PyExc_TypeError, "The keepseparator argument must be a boolean");
            return NULL;
        }
    }
    else { keepseparator = 0; }

    // Validate and convert `maxsplit`
    if (maxsplit_obj) {
        maxsplit = PyLong_AsSsize_t(maxsplit_obj);
        if (maxsplit == -1 && PyErr_Occurred()) {
            PyErr_SetString(PyExc_TypeError, "The maxsplit argument must be an integer");
            return NULL;
        }
    }
    else { maxsplit = PY_SSIZE_T_MAX; }

    return Str_split_(text_obj, text, separator, keepseparator, maxsplit);
}

static PyObject *Str_splitlines(PyObject *self, PyObject *args, PyObject *kwargs) {
    // Check minimum arguments
    int is_member = self != NULL && PyObject_TypeCheck(self, &StrType);
    Py_ssize_t nargs = PyTuple_Size(args);
    if (nargs < !is_member || nargs > !is_member + 2) {
        PyErr_SetString(PyExc_TypeError, "splitlines() requires at least 1 argument");
        return NULL;
    }

    PyObject *text_obj = is_member ? self : PyTuple_GET_ITEM(args, 0);
    PyObject *keeplinebreaks_obj = nargs > !is_member ? PyTuple_GET_ITEM(args, !is_member) : NULL;
    PyObject *maxsplit_obj = nargs > !is_member + 1 ? PyTuple_GET_ITEM(args, !is_member + 1) : NULL;

    if (kwargs) {
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(kwargs, &pos, &key, &value)) {
            if (PyUnicode_CompareWithASCIIString(key, "keeplinebreaks") == 0) { keeplinebreaks_obj = value; }
            else if (PyUnicode_CompareWithASCIIString(key, "maxsplit") == 0) { maxsplit_obj = value; }
            else if (PyErr_Format(PyExc_TypeError, "Got an unexpected keyword argument '%U'", key)) { return NULL; }
        }
    }

    sz_string_view_t text;
    int keeplinebreaks;
    Py_ssize_t maxsplit = PY_SSIZE_T_MAX; // Default value for maxsplit

    // Validate and convert `text`
    if (!export_string_like(text_obj, &text.start, &text.length)) {
        PyErr_SetString(PyExc_TypeError, "The text argument must be string-like");
        return NULL;
    }

    // Validate and convert `keeplinebreaks`
    if (keeplinebreaks_obj) {
        keeplinebreaks = PyObject_IsTrue(keeplinebreaks_obj);
        if (keeplinebreaks == -1) {
            PyErr_SetString(PyExc_TypeError, "The keeplinebreaks argument must be a boolean");
            return NULL;
        }
    }
    else { keeplinebreaks = 0; }

    // Validate and convert `maxsplit`
    if (maxsplit_obj) {
        maxsplit = PyLong_AsSsize_t(maxsplit_obj);
        if (maxsplit == -1 && PyErr_Occurred()) {
            PyErr_SetString(PyExc_TypeError, "The maxsplit argument must be an integer");
            return NULL;
        }
    }

    // TODO: Support arbitrary newline characters:
    // https://docs.python.org/3/library/stdtypes.html#str.splitlines
    // \n, \r, \r\n, \v or \x0b, \f or \x0c, \x1c, \x1d, \x1e, \x85, \u2028, \u2029
    // https://github.com/ashvardanian/StringZilla/issues/29
    sz_string_view_t separator;
    separator.start = "\n";
    separator.length = 1;
    return Str_split_(text_obj, text, separator, keeplinebreaks, maxsplit);
}

static PyObject *Str_concat(PyObject *self, PyObject *other) {
    struct sz_string_view_t self_str, other_str;

    // Validate and convert `self`
    if (!export_string_like(self, &self_str.start, &self_str.length)) {
        PyErr_SetString(PyExc_TypeError, "The self object must be string-like");
        return NULL;
    }

    // Validate and convert `other`
    if (!export_string_like(other, &other_str.start, &other_str.length)) {
        PyErr_SetString(PyExc_TypeError, "The other object must be string-like");
        return NULL;
    }

    // Allocate a new Str instance
    Str *result_str = PyObject_New(Str, &StrType);
    if (result_str == NULL) { return NULL; }

    // Calculate the total length of the new string
    result_str->parent = NULL;
    result_str->length = self_str.length + other_str.length;

    // Allocate memory for the new string
    result_str->start = malloc(result_str->length);
    if (result_str->start == NULL) {
        PyErr_SetString(PyExc_MemoryError, "Unable to allocate memory for string concatenation");
        return NULL;
    }

    // Perform the string concatenation
    memcpy(result_str->start, self_str.start, self_str.length);
    memcpy(result_str->start + self_str.length, other_str.start, other_str.length);

    return (PyObject *)result_str;
}

static PySequenceMethods Str_as_sequence = {
    .sq_length = Str_len,   //
    .sq_item = Str_getitem, //
    .sq_contains = Str_in,  //
};

static PyMappingMethods Str_as_mapping = {
    .mp_length = Str_len,          //
    .mp_subscript = Str_subscript, // Is used to implement slices in Python
};

static PyBufferProcs Str_as_buffer = {
    .bf_getbuffer = Str_getbuffer,
    .bf_releasebuffer = Str_releasebuffer,
};

static PyNumberMethods Str_as_number = {
    .nb_add = Str_concat,
};

#define SZ_METHOD_FLAGS METH_VARARGS | METH_KEYWORDS

static PyMethodDef Str_methods[] = {
    // Basic `str`-like functionality
    {"contains", Str_contains, SZ_METHOD_FLAGS, "Check if a string contains a substring."},
    {"count", Str_count, SZ_METHOD_FLAGS, "Count the occurrences of a substring."},
    {"splitlines", Str_splitlines, SZ_METHOD_FLAGS, "Split a string by line breaks."},
    {"startswith", Str_startswith, SZ_METHOD_FLAGS, "Check if a string starts with a given prefix."},
    {"endswith", Str_endswith, SZ_METHOD_FLAGS, "Check if a string ends with a given suffix."},
    {"split", Str_split, SZ_METHOD_FLAGS, "Split a string by a separator."},

    // Bidirectional operations
    {"find", Str_find, SZ_METHOD_FLAGS, "Find the first occurrence of a substring."},
    {"index", Str_index, SZ_METHOD_FLAGS, "Find the first occurrence of a substring or raise error if missing."},
    {"partition", Str_partition, SZ_METHOD_FLAGS, "Splits string into 3-tuple: before, first match, after."},
    {"rfind", Str_rfind, SZ_METHOD_FLAGS, "Find the last occurrence of a substring."},
    {"rindex", Str_rindex, SZ_METHOD_FLAGS, "Find the last occurrence of a substring or raise error if missing."},
    {"rpartition", Str_rpartition, SZ_METHOD_FLAGS, "Splits string into 3-tuple: before, last match, after."},

    // Edit distance extensions
    {"hamming_distance", Str_hamming_distance, SZ_METHOD_FLAGS,
     "Hamming distance between two strings, as the number of replaced bytes, and difference in length."},
    {"hamming_distance_unicode", Str_hamming_distance_unicode, SZ_METHOD_FLAGS,
     "Hamming distance between two strings, as the number of replaced unicode characters, and difference in length."},
    {"edit_distance", Str_edit_distance, SZ_METHOD_FLAGS,
     "Levenshtein distance between two strings, as the number of inserted, deleted, and replaced bytes."},
    {"edit_distance_unicode", Str_edit_distance_unicode, SZ_METHOD_FLAGS,
     "Levenshtein distance between two strings, as the number of inserted, deleted, and replaced unicode characters."},
    {"alignment_score", Str_alignment_score, SZ_METHOD_FLAGS,
     "Needleman-Wunsch alignment score given a substitution cost matrix."},

    // Character search extensions
    {"find_first_of", Str_find_first_of, SZ_METHOD_FLAGS,
     "Finds the first occurrence of a character from another string."},
    {"find_last_of", Str_find_last_of, SZ_METHOD_FLAGS,
     "Finds the last occurrence of a character from another string."},
    {"find_first_not_of", Str_find_first_not_of, SZ_METHOD_FLAGS,
     "Finds the first occurrence of a character not present in another string."},
    {"find_last_not_of", Str_find_last_not_of, SZ_METHOD_FLAGS,
     "Finds the last occurrence of a character not present in another string."},

    // Dealing with larger-than-memory datasets
    {"offset_within", Str_offset_within, SZ_METHOD_FLAGS,
     "Return the raw byte offset of one binary string within another."},
    {"write_to", Str_write_to, SZ_METHOD_FLAGS, "Return the raw byte offset of one binary string within another."},

    {NULL, NULL, 0, NULL}};

static PyTypeObject StrType = {
    PyObject_HEAD_INIT(NULL).tp_name = "stringzilla.Str",
    .tp_doc = "Immutable string/slice class with SIMD and SWAR-accelerated operations",
    .tp_basicsize = sizeof(Str),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = Str_new,
    .tp_init = Str_init,
    .tp_dealloc = Str_dealloc,
    .tp_hash = Str_hash,
    .tp_richcompare = Str_richcompare,
    .tp_str = Str_str,
    .tp_methods = Str_methods,
    .tp_as_sequence = &Str_as_sequence,
    .tp_as_mapping = &Str_as_mapping,
    .tp_as_buffer = &Str_as_buffer,
    .tp_as_number = &Str_as_number,
};

#pragma endregion

#pragma regions Strs

static PyObject *Strs_shuffle(Strs *self, PyObject *args, PyObject *kwargs) {
    unsigned int seed = time(NULL); // Default seed

    // Check for positional arguments
    Py_ssize_t nargs = PyTuple_Size(args);
    if (nargs > 1) {
        PyErr_SetString(PyExc_TypeError, "shuffle() takes at most 1 positional argument");
        return NULL;
    }
    else if (nargs == 1) {
        PyObject *seed_obj = PyTuple_GET_ITEM(args, 0);
        if (!PyLong_Check(seed_obj)) {
            PyErr_SetString(PyExc_TypeError, "The seed must be an integer");
            return NULL;
        }
        seed = PyLong_AsUnsignedLong(seed_obj);
    }

    // Check for keyword arguments
    if (kwargs) {
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(kwargs, &pos, &key, &value)) {
            if (PyUnicode_CompareWithASCIIString(key, "seed") == 0) {
                if (nargs == 1) {
                    PyErr_SetString(PyExc_TypeError, "Received seed both as positional and keyword argument");
                    return NULL;
                }
                if (!PyLong_Check(value)) {
                    PyErr_SetString(PyExc_TypeError, "The seed must be an integer");
                    return NULL;
                }
                seed = PyLong_AsUnsignedLong(value);
            }
            else {
                PyErr_Format(PyExc_TypeError, "Received an unexpected keyword argument '%U'", key);
                return NULL;
            }
        }
    }

    // Change the layout
    if (!prepare_strings_for_reordering(self)) {
        PyErr_Format(PyExc_TypeError, "Failed to prepare the sequence for shuffling");
        return NULL;
    }

    // Get the parts and their count
    struct reordered_slices_t *reordered = &self->data.reordered;
    sz_string_view_t *parts = reordered->parts;
    size_t count = reordered->count;

    // Fisher-Yates Shuffle Algorithm
    srand(seed);
    for (size_t i = count - 1; i > 0; --i) {
        size_t j = rand() % (i + 1);
        // Swap parts[i] and parts[j]
        sz_string_view_t temp = parts[i];
        parts[i] = parts[j];
        parts[j] = temp;
    }

    Py_RETURN_NONE;
}

static sz_bool_t Strs_sort_(Strs *self, sz_string_view_t **parts_output, sz_sorted_idx_t **order_output,
                            sz_size_t *count_output) {
    // Change the layout
    if (!prepare_strings_for_reordering(self)) {
        PyErr_Format(PyExc_TypeError, "Failed to prepare the sequence for sorting");
        return 0;
    }

    // Get the parts and their count
    // The only possible `self->type` by now is the `STRS_REORDERED`
    sz_string_view_t *parts = self->data.reordered.parts;
    size_t count = self->data.reordered.count;

    // Allocate temporary memory to store the ordering offsets
    size_t memory_needed = sizeof(sz_sorted_idx_t) * count;
    if (temporary_memory.length < memory_needed) {
        temporary_memory.start = realloc(temporary_memory.start, memory_needed);
        temporary_memory.length = memory_needed;
    }
    if (!temporary_memory.start) {
        PyErr_Format(PyExc_MemoryError, "Unable to allocate memory for the Levenshtein matrix");
        return 0;
    }

    // Call our sorting algorithm
    sz_sequence_t sequence;
    memset(&sequence, 0, sizeof(sequence));
    sequence.order = (sz_sorted_idx_t *)temporary_memory.start;
    sequence.count = count;
    sequence.handle = parts;
    sequence.get_start = parts_get_start;
    sequence.get_length = parts_get_length;
    for (sz_sorted_idx_t i = 0; i != sequence.count; ++i) sequence.order[i] = i;
    sz_sort(&sequence);

    // Export results
    *parts_output = parts;
    *order_output = sequence.order;
    *count_output = sequence.count;
    return 1;
}

static PyObject *Strs_sort(Strs *self, PyObject *args, PyObject *kwargs) {
    PyObject *reverse_obj = NULL; // Default is not reversed

    // Check for positional arguments
    Py_ssize_t nargs = PyTuple_Size(args);
    if (nargs > 1) {
        PyErr_SetString(PyExc_TypeError, "sort() takes at most 1 positional argument");
        return NULL;
    }
    else if (nargs == 1) { reverse_obj = PyTuple_GET_ITEM(args, 0); }

    // Check for keyword arguments
    if (kwargs) {
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(kwargs, &pos, &key, &value)) {
            if (PyUnicode_CompareWithASCIIString(key, "reverse") == 0) {
                if (reverse_obj) {
                    PyErr_SetString(PyExc_TypeError, "Received reverse both as positional and keyword argument");
                    return NULL;
                }
                reverse_obj = value;
            }
            else {
                PyErr_Format(PyExc_TypeError, "Received an unexpected keyword argument '%U'", key);
                return NULL;
            }
        }
    }

    sz_bool_t reverse = 0; // Default is False
    if (reverse_obj) {
        if (!PyBool_Check(reverse_obj)) {
            PyErr_SetString(PyExc_TypeError, "The reverse must be a boolean");
            return NULL;
        }
        reverse = PyObject_IsTrue(reverse_obj);
    }

    sz_string_view_t *parts = NULL;
    sz_size_t *order = NULL;
    sz_size_t count = 0;
    if (!Strs_sort_(self, &parts, &order, &count)) return NULL;

    // Apply the sorting algorithm here, considering the `reverse` value
    if (reverse) reverse_offsets(order, count);

    // Apply the new order.
    apply_order(parts, order, count);

    Py_RETURN_NONE;
}

static PyObject *Strs_order(Strs *self, PyObject *args, PyObject *kwargs) {
    PyObject *reverse_obj = NULL; // Default is not reversed

    // Check for positional arguments
    Py_ssize_t nargs = PyTuple_Size(args);
    if (nargs > 1) {
        PyErr_SetString(PyExc_TypeError, "order() takes at most 1 positional argument");
        return NULL;
    }
    else if (nargs == 1) { reverse_obj = PyTuple_GET_ITEM(args, 0); }

    // Check for keyword arguments
    if (kwargs) {
        PyObject *key, *value;
        Py_ssize_t pos = 0;
        while (PyDict_Next(kwargs, &pos, &key, &value)) {
            if (PyUnicode_CompareWithASCIIString(key, "reverse") == 0) {
                if (reverse_obj) {
                    PyErr_SetString(PyExc_TypeError, "Received reverse both as positional and keyword argument");
                    return NULL;
                }
                reverse_obj = value;
            }
            else {
                PyErr_Format(PyExc_TypeError, "Received an unexpected keyword argument '%U'", key);
                return NULL;
            }
        }
    }

    sz_bool_t reverse = 0; // Default is False
    if (reverse_obj) {
        if (!PyBool_Check(reverse_obj)) {
            PyErr_SetString(PyExc_TypeError, "The reverse must be a boolean");
            return NULL;
        }
        reverse = PyObject_IsTrue(reverse_obj);
    }

    sz_string_view_t *parts = NULL;
    sz_sorted_idx_t *order = NULL;
    sz_size_t count = 0;
    if (!Strs_sort_(self, &parts, &order, &count)) return NULL;

    // Apply the sorting algorithm here, considering the `reverse` value
    if (reverse) reverse_offsets(order, count);

    // Here, instead of applying the order, we want to return the copy of the
    // order as a NumPy array of 64-bit unsigned integers.
    //
    //      npy_intp numpy_size = count;
    //      PyObject *array = PyArray_SimpleNew(1, &numpy_size, NPY_UINT64);
    //      if (!array) {
    //          PyErr_SetString(PyExc_RuntimeError, "Failed to create a NumPy array");
    //          return NULL;
    //      }
    //      sz_sorted_idx_t *numpy_data_ptr = (sz_sorted_idx_t *)PyArray_DATA((PyArrayObject *)array);
    //      memcpy(numpy_data_ptr, order, count * sizeof(sz_sorted_idx_t));
    //
    // There are compilation issues with NumPy.
    // Here is an example for `cp312-musllinux_s390x`: https://x.com/ashvardanian/status/1757880762278531447?s=20
    // So instead of NumPy, let's produce a tuple of integers.
    PyObject *tuple = PyTuple_New(count);
    if (!tuple) {
        PyErr_SetString(PyExc_RuntimeError, "Failed to create a tuple");
        return NULL;
    }
    for (sz_size_t i = 0; i < count; ++i) {
        PyObject *index = PyLong_FromUnsignedLong(order[i]);
        if (!index) {
            PyErr_SetString(PyExc_RuntimeError, "Failed to create a tuple element");
            Py_DECREF(tuple);
            return NULL;
        }
        PyTuple_SET_ITEM(tuple, i, index);
    }
    return tuple;
}

static PySequenceMethods Strs_as_sequence = {
    .sq_length = Strs_len,        //
    .sq_item = Strs_getitem,      //
    .sq_contains = Strs_contains, //
};

static PyMappingMethods Strs_as_mapping = {
    .mp_length = Strs_len,          //
    .mp_subscript = Strs_subscript, // Is used to implement slices in Python
};

static PyMethodDef Strs_methods[] = {
    {"shuffle", Strs_shuffle, SZ_METHOD_FLAGS, "Shuffle the elements of the Strs object."},  //
    {"sort", Strs_sort, SZ_METHOD_FLAGS, "Sort the elements of the Strs object."},           //
    {"order", Strs_order, SZ_METHOD_FLAGS, "Provides the indexes to achieve sorted order."}, //
    {NULL, NULL, 0, NULL}};

static PyTypeObject StrsType = {
    PyVarObject_HEAD_INIT(NULL, 0).tp_name = "stringzilla.Strs",
    .tp_doc = "Space-efficient container for large collections of strings and their slices",
    .tp_basicsize = sizeof(Strs),
    .tp_itemsize = 0,
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_new = PyType_GenericNew,
    .tp_methods = Strs_methods,
    .tp_as_sequence = &Strs_as_sequence,
    .tp_as_mapping = &Strs_as_mapping,
};

#pragma endregion

static void stringzilla_cleanup(PyObject *m) {
    if (temporary_memory.start) free(temporary_memory.start);
    temporary_memory.start = NULL;
    temporary_memory.length = 0;
}

static PyMethodDef stringzilla_methods[] = {
    // Basic `str`-like functionality
    {"contains", Str_contains, SZ_METHOD_FLAGS, "Check if a string contains a substring."},
    {"count", Str_count, SZ_METHOD_FLAGS, "Count the occurrences of a substring."},
    {"splitlines", Str_splitlines, SZ_METHOD_FLAGS, "Split a string by line breaks."},
    {"startswith", Str_startswith, SZ_METHOD_FLAGS, "Check if a string starts with a given prefix."},
    {"endswith", Str_endswith, SZ_METHOD_FLAGS, "Check if a string ends with a given suffix."},
    {"split", Str_split, SZ_METHOD_FLAGS, "Split a string by a separator."},

    // Bidirectional operations
    {"find", Str_find, SZ_METHOD_FLAGS, "Find the first occurrence of a substring."},
    {"index", Str_index, SZ_METHOD_FLAGS, "Find the first occurrence of a substring or raise error if missing."},
    {"partition", Str_partition, SZ_METHOD_FLAGS, "Splits string into 3-tuple: before, first match, after."},
    {"rfind", Str_rfind, SZ_METHOD_FLAGS, "Find the last occurrence of a substring."},
    {"rindex", Str_rindex, SZ_METHOD_FLAGS, "Find the last occurrence of a substring or raise error if missing."},
    {"rpartition", Str_rpartition, SZ_METHOD_FLAGS, "Splits string into 3-tuple: before, last match, after."},

    // Edit distance extensions
    {"hamming_distance", Str_hamming_distance, SZ_METHOD_FLAGS,
     "Hamming distance between two strings, as the number of replaced bytes, and difference in length."},
    {"hamming_distance_unicode", Str_hamming_distance_unicode, SZ_METHOD_FLAGS,
     "Hamming distance between two strings, as the number of replaced unicode characters, and difference in length."},
    {"edit_distance", Str_edit_distance, SZ_METHOD_FLAGS,
     "Levenshtein distance between two strings, as the number of inserted, deleted, and replaced bytes."},
    {"edit_distance_unicode", Str_edit_distance_unicode, SZ_METHOD_FLAGS,
     "Levenshtein distance between two strings, as the number of inserted, deleted, and replaced unicode characters."},
    {"alignment_score", Str_alignment_score, SZ_METHOD_FLAGS,
     "Needleman-Wunsch alignment score given a substitution cost matrix."},

    // Character search extensions
    {"find_first_of", Str_find_first_of, SZ_METHOD_FLAGS,
     "Finds the first occurrence of a character from another string."},
    {"find_last_of", Str_find_last_of, SZ_METHOD_FLAGS,
     "Finds the last occurrence of a character from another string."},
    {"find_first_not_of", Str_find_first_not_of, SZ_METHOD_FLAGS,
     "Finds the first occurrence of a character not present in another string."},
    {"find_last_not_of", Str_find_last_not_of, SZ_METHOD_FLAGS,
     "Finds the last occurrence of a character not present in another string."},

    // Global unary extensions
    {"hash", Str_like_hash, SZ_METHOD_FLAGS, "Hash a string or a byte-array."},

    {NULL, NULL, 0, NULL}};

static PyModuleDef stringzilla_module = {
    PyModuleDef_HEAD_INIT,
    "stringzilla",
    "SIMD-accelerated string search, sort, hashes, fingerprints, & edit distances",
    -1,
    stringzilla_methods,
    NULL,
    NULL,
    NULL,
    stringzilla_cleanup,
};

PyMODINIT_FUNC PyInit_stringzilla(void) {
    PyObject *m;

    if (PyType_Ready(&StrType) < 0) return NULL;
    if (PyType_Ready(&FileType) < 0) return NULL;
    if (PyType_Ready(&StrsType) < 0) return NULL;

    m = PyModule_Create(&stringzilla_module);
    if (m == NULL) return NULL;

    // Add version metadata
    {
        char version_str[50];
        sprintf(version_str, "%d.%d.%d", STRINGZILLA_VERSION_MAJOR, STRINGZILLA_VERSION_MINOR,
                STRINGZILLA_VERSION_PATCH);
        PyModule_AddStringConstant(m, "__version__", version_str);
    }

    // Define SIMD capabilities
    {
        sz_capability_t caps = sz_capabilities();
        char caps_str[512];
        char const *serial = (caps & sz_cap_serial_k) ? "serial," : "";
        char const *neon = (caps & sz_cap_arm_neon_k) ? "neon," : "";
        char const *sve = (caps & sz_cap_arm_sve_k) ? "sve," : "";
        char const *avx2 = (caps & sz_cap_x86_avx2_k) ? "avx2," : "";
        char const *avx512f = (caps & sz_cap_x86_avx512f_k) ? "avx512f," : "";
        char const *avx512vl = (caps & sz_cap_x86_avx512vl_k) ? "avx512vl," : "";
        char const *avx512bw = (caps & sz_cap_x86_avx512bw_k) ? "avx512bw," : "";
        char const *avx512vbmi = (caps & sz_cap_x86_avx512vbmi_k) ? "avx512vbmi," : "";
        char const *gfni = (caps & sz_cap_x86_gfni_k) ? "gfni," : "";
        sprintf(caps_str, "%s%s%s%s%s%s%s%s%s", serial, neon, sve, avx2, avx512f, avx512vl, avx512bw, avx512vbmi, gfni);
        PyModule_AddStringConstant(m, "__capabilities__", caps_str);
    }

    Py_INCREF(&StrType);
    if (PyModule_AddObject(m, "Str", (PyObject *)&StrType) < 0) {
        Py_XDECREF(&StrType);
        Py_XDECREF(m);
        return NULL;
    }

    Py_INCREF(&FileType);
    if (PyModule_AddObject(m, "File", (PyObject *)&FileType) < 0) {
        Py_XDECREF(&FileType);
        Py_XDECREF(&StrType);
        Py_XDECREF(m);
        return NULL;
    }

    Py_INCREF(&StrsType);
    if (PyModule_AddObject(m, "Strs", (PyObject *)&StrsType) < 0) {
        Py_XDECREF(&StrsType);
        Py_XDECREF(&FileType);
        Py_XDECREF(&StrType);
        Py_XDECREF(m);
        return NULL;
    }

    // Initialize temporary_memory, if needed
    temporary_memory.start = malloc(4096);
    temporary_memory.length = 4096 * (temporary_memory.start != NULL);
    return m;
}
