/*
 * _dataclasses: fixed C implementations of the dataclass-generated
 * __init__ / __repr__ / __eq__ / __hash__ methods.
 *
 * These do NOT generate or compile any per-class code.  Each is a single
 * shared C function that reads per-class metadata at call time.
 *
 * __init__ reads a *C-native cached spec* (dc_cspec) built once at class
 * creation by make_cspec() and stored on the type as a capsule
 * (__dataclass_c_spec__).  That removes the per-call attribute lookups and
 * per-field PyLong conversions the Python _InitSpec object would require:
 * one capsule fetch then a tight loop over C arrays.
 *
 * repr/eq/hash read the tuple of field names stashed on the type:
 *   type.__dataclass_repr_fields__ / _eq_fields / _hash_fields
 *
 * The exported callables are custom method descriptors with vectorcall.
 * Special-method slot dispatch calls them unbound with self already in
 * args[0], avoiding instancemethod's bound-method allocation and argument
 * re-tupling.
 */
#define PY_SSIZE_T_CLEAN
#ifndef Py_BUILD_CORE_MODULE
#  define Py_BUILD_CORE_MODULE
#endif
#include <Python.h>
#include "structmember.h"
#include "internal/pycore_dict.h"    // _PyDictValues_AddToInsertionOrder()
#include "internal/pycore_object.h"  // _PyObject_GetManagedDict()

static PyObject *S_c_spec;
static PyObject *S_repr_fields;
static PyObject *S_eq_fields;
static PyObject *S_hash_fields;
static PyObject *S_qualname;
static PyObject *S_post_init;
static PyObject *S_pos;
static PyObject *S_kwonly;
static PyObject *S_assignments;
static PyObject *S_initvar_names;
static PyObject *S_has_post_init;
static PyObject *S_frozen;
static PyObject *S_fast;

static PyObject *
get_type_attr(PyObject *self, PyObject *name)
{
    return PyObject_GetAttr((PyObject *)Py_TYPE(self), name);
}

/* ----------------------- C-native cached spec ----------------------- */

typedef struct {
    PyObject *name;   /* interned str, borrowed (kept alive by owner_spec) */
    int code;         /* param kind (0/1/2) or assignment mode (0/1/2) */
    PyObject *extra;  /* default/factory/const, borrowed */
    int store_kind;   /* assignment cache kind; only used for assignments */
    unsigned int type_version;
    Py_ssize_t inline_offset;
} dc_slot;

typedef struct {
    Py_ssize_t npos;        dc_slot *pos;
    Py_ssize_t nkw;         dc_slot *kwonly;
    Py_ssize_t nassign;     dc_slot *assignments;
    Py_ssize_t ninitvar;    PyObject **initvar_names;  /* borrowed */
    int has_post_init;
    int frozen;
    int fast;
    int inline_ready;
    unsigned int inline_type_version;
    PyObject *param_index;
    PyObject *owner_spec;   /* strong ref: keeps the borrowed PyObjects alive */
    PyTypeObject *owner_type;  /* borrowed: the type owns the capsule */
} dc_cspec;

#define DC_STORE_UNKNOWN 0
#define DC_STORE_GENERIC 1
#define DC_STORE_MANAGED 2
#define DC_STORE_INLINE  3
#define DC_STORE_SLOT    4

#define DC_LOAD_UNKNOWN 0
#define DC_LOAD_GENERIC 1
#define DC_LOAD_INLINE  2
#define DC_LOAD_SLOT    3

typedef struct {
    PyObject *name;   /* borrowed from owner_tuple */
    int load_kind;
    unsigned int type_version;
    Py_ssize_t inline_offset;
} dc_read_slot;

typedef struct {
    Py_ssize_t n;
    dc_read_slot *fields;
    PyObject *owner_tuple;      /* strong ref: keeps field names alive */
    PyTypeObject *owner_type;   /* borrowed: descriptor is owned by type */
    int inline_ready;
    unsigned int inline_type_version;
} dc_field_spec;

static void
cspec_destructor(PyObject *capsule)
{
    dc_cspec *cs = (dc_cspec *)PyCapsule_GetPointer(capsule, NULL);
    if (cs == NULL) {
        PyErr_Clear();
        return;
    }
    PyMem_Free(cs->pos);
    PyMem_Free(cs->kwonly);
    PyMem_Free(cs->assignments);
    PyMem_Free(cs->initvar_names);
    Py_XDECREF(cs->param_index);
    Py_XDECREF(cs->owner_spec);
    PyMem_Free(cs);
}

/* Fill an array of dc_slot from a tuple of (name, code, extra) triples. */
static int
fill_slots(PyObject *tup, dc_slot **out, Py_ssize_t *nout)
{
    Py_ssize_t n = PyTuple_GET_SIZE(tup);
    dc_slot *arr = NULL;
    if (n > 0) {
        arr = (dc_slot *)PyMem_Calloc(n, sizeof(dc_slot));
        if (arr == NULL) {
            PyErr_NoMemory();
            return -1;
        }
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *t = PyTuple_GET_ITEM(tup, i);
            arr[i].name = PyTuple_GET_ITEM(t, 0);
            long code = PyLong_AsLong(PyTuple_GET_ITEM(t, 1));
            if (code == -1 && PyErr_Occurred()) {
                PyMem_Free(arr);
                return -1;
            }
            arr[i].code = (int)code;
            arr[i].extra = PyTuple_GET_ITEM(t, 2);
            arr[i].store_kind = DC_STORE_UNKNOWN;
            arr[i].type_version = 0;
            arr[i].inline_offset = -1;
        }
    }
    *out = arr;
    *nout = n;
    return 0;
}

static PyObject *
build_param_index(dc_cspec *cs)
{
    PyObject *map = PyDict_New();
    if (map == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < cs->npos; i++) {
        PyObject *idx = PyLong_FromSsize_t(i);
        if (idx == NULL) {
            Py_DECREF(map);
            return NULL;
        }
        int res = PyDict_SetItem(map, cs->pos[i].name, idx);
        Py_DECREF(idx);
        if (res < 0) {
            Py_DECREF(map);
            return NULL;
        }
    }
    for (Py_ssize_t i = 0; i < cs->nkw; i++) {
        PyObject *idx = PyLong_FromSsize_t(cs->npos + i);
        if (idx == NULL) {
            Py_DECREF(map);
            return NULL;
        }
        int res = PyDict_SetItem(map, cs->kwonly[i].name, idx);
        Py_DECREF(idx);
        if (res < 0) {
            Py_DECREF(map);
            return NULL;
        }
    }
    return map;
}

/* make_cspec(spec, cls) -> capsule wrapping dc_cspec*, called from dataclasses.py. */
static PyObject *
make_cspec(PyObject *Py_UNUSED(mod), PyObject *const *args, Py_ssize_t nargs)
{
    if (nargs != 2) {
        PyErr_Format(PyExc_TypeError,
                     "make_cspec() takes exactly 2 arguments (%zd given)",
                     nargs);
        return NULL;
    }
    PyObject *spec = args[0];
    PyObject *cls = args[1];
    if (!PyType_Check(cls)) {
        PyErr_SetString(PyExc_TypeError,
                        "make_cspec() argument 2 must be a type");
        return NULL;
    }

    dc_cspec *cs = (dc_cspec *)PyMem_Calloc(1, sizeof(dc_cspec));
    if (cs == NULL)
        return PyErr_NoMemory();

    PyObject *pos = PyObject_GetAttr(spec, S_pos);
    PyObject *kwonly = PyObject_GetAttr(spec, S_kwonly);
    PyObject *assignments = PyObject_GetAttr(spec, S_assignments);
    PyObject *initvar = PyObject_GetAttr(spec, S_initvar_names);
    PyObject *hpi = PyObject_GetAttr(spec, S_has_post_init);
    PyObject *frz = PyObject_GetAttr(spec, S_frozen);
    PyObject *fst = PyObject_GetAttr(spec, S_fast);
    if (!pos || !kwonly || !assignments || !initvar || !hpi || !frz || !fst)
        goto error;

    if (fill_slots(pos, &cs->pos, &cs->npos) < 0)
        goto error;
    if (fill_slots(kwonly, &cs->kwonly, &cs->nkw) < 0)
        goto error;
    if (fill_slots(assignments, &cs->assignments, &cs->nassign) < 0)
        goto error;
    cs->param_index = build_param_index(cs);
    if (cs->param_index == NULL)
        goto error;

    cs->ninitvar = PyTuple_GET_SIZE(initvar);
    if (cs->ninitvar > 0) {
        cs->initvar_names =
            (PyObject **)PyMem_Calloc(cs->ninitvar, sizeof(PyObject *));
        if (cs->initvar_names == NULL) {
            PyErr_NoMemory();
            goto error;
        }
        for (Py_ssize_t i = 0; i < cs->ninitvar; i++)
            cs->initvar_names[i] = PyTuple_GET_ITEM(initvar, i);
    }

    cs->has_post_init = PyObject_IsTrue(hpi);
    cs->frozen = PyObject_IsTrue(frz);
    cs->fast = PyObject_IsTrue(fst);
    cs->owner_spec = Py_NewRef(spec);
    cs->owner_type = (PyTypeObject *)cls;

    Py_DECREF(pos);
    Py_DECREF(kwonly);
    Py_DECREF(assignments);
    Py_DECREF(initvar);
    Py_DECREF(hpi);
    Py_DECREF(frz);
    Py_DECREF(fst);

    PyObject *cap = PyCapsule_New(cs, NULL, cspec_destructor);
    if (cap == NULL) {
        /* destructor won't run; free manually */
        PyMem_Free(cs->pos);
        PyMem_Free(cs->kwonly);
        PyMem_Free(cs->assignments);
        PyMem_Free(cs->initvar_names);
        Py_XDECREF(cs->param_index);
        Py_DECREF(cs->owner_spec);
        PyMem_Free(cs);
        return NULL;
    }
    return cap;

error:
    Py_XDECREF(pos);
    Py_XDECREF(kwonly);
    Py_XDECREF(assignments);
    Py_XDECREF(initvar);
    Py_XDECREF(hpi);
    Py_XDECREF(frz);
    Py_XDECREF(fst);
    PyMem_Free(cs->pos);
    PyMem_Free(cs->kwonly);
    PyMem_Free(cs->assignments);
    PyMem_Free(cs->initvar_names);
    Py_XDECREF(cs->param_index);
    PyMem_Free(cs);
    return NULL;
}

static void
free_field_spec(dc_field_spec *fs)
{
    if (fs == NULL) {
        return;
    }
    PyMem_Free(fs->fields);
    Py_XDECREF(fs->owner_tuple);
    PyMem_Free(fs);
}

static dc_field_spec *
make_field_spec(PyObject *fields, PyObject *cls)
{
    if (!PyTuple_Check(fields)) {
        PyErr_SetString(PyExc_TypeError,
                        "field spec argument 1 must be a tuple");
        return NULL;
    }
    if (!PyType_Check(cls)) {
        PyErr_SetString(PyExc_TypeError,
                        "field spec argument 2 must be a type");
        return NULL;
    }
    dc_field_spec *fs = (dc_field_spec *)PyMem_Calloc(1, sizeof(dc_field_spec));
    if (fs == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    fs->n = PyTuple_GET_SIZE(fields);
    fs->owner_tuple = Py_NewRef(fields);
    fs->owner_type = (PyTypeObject *)cls;
    if (fs->n > 0) {
        fs->fields = (dc_read_slot *)PyMem_Calloc(fs->n, sizeof(dc_read_slot));
        if (fs->fields == NULL) {
            PyErr_NoMemory();
            free_field_spec(fs);
            return NULL;
        }
        for (Py_ssize_t i = 0; i < fs->n; i++) {
            fs->fields[i].name = PyTuple_GET_ITEM(fields, i);
            fs->fields[i].load_kind = DC_LOAD_UNKNOWN;
            fs->fields[i].type_version = 0;
            fs->fields[i].inline_offset = -1;
        }
    }
    return fs;
}

/* ----------------------------- __init__ ----------------------------- */

static void
raise_missing(PyObject *self, const char *kind, PyObject *missing)
{
    PyObject *qual = get_type_attr(self, S_qualname);
    Py_ssize_t n = PyList_GET_SIZE(missing);
    PyObject *joined;
    if (n == 1) {
        joined = PyObject_Repr(PyList_GET_ITEM(missing, 0));
    }
    else {
        PyObject *parts = PyList_New(0);
        for (Py_ssize_t i = 0; i < n; i++) {
            PyObject *r = PyObject_Repr(PyList_GET_ITEM(missing, i));
            PyList_Append(parts, r);
            Py_DECREF(r);
        }
        if (n == 2) {
            joined = PyUnicode_FromFormat("%U and %U",
                                          PyList_GET_ITEM(parts, 0),
                                          PyList_GET_ITEM(parts, 1));
        }
        else {
            PyObject *sep = PyUnicode_FromString(", ");
            PyObject *slice = PyList_GetSlice(parts, 0, n - 1);
            PyObject *head = PyUnicode_Join(sep, slice);
            joined = PyUnicode_FromFormat("%U, and %U", head,
                                          PyList_GET_ITEM(parts, n - 1));
            Py_DECREF(sep);
            Py_DECREF(slice);
            Py_DECREF(head);
        }
        Py_DECREF(parts);
    }
    PyErr_Format(PyExc_TypeError,
                 "%U.__init__() missing %zd required %s argument%s: %U",
                 qual, n, kind, (n == 1 ? "" : "s"), joined);
    Py_XDECREF(qual);
    Py_XDECREF(joined);
}

static void
raise_too_many(PyObject *self, Py_ssize_t npos, Py_ssize_t nargs)
{
    PyObject *qual = get_type_attr(self, S_qualname);
    PyErr_Format(PyExc_TypeError,
        "%U.__init__() takes %zd positional argument%s but %zd were given",
        qual, npos + 1, (npos + 1 == 1 ? "" : "s"), nargs + 1);
    Py_XDECREF(qual);
}

/* Resolve param p into bound[name]; returns 1 bound, 0 missing, -1 error. */
static int
bind_slot(PyObject *bound, PyObject *kw, dc_slot *p)
{
    if (kw) {
        PyObject *v = PyDict_GetItemWithError(kw, p->name);
        if (v) {
            if (PyDict_SetItem(bound, p->name, v) < 0 ||
                PyDict_DelItem(kw, p->name) < 0)
                return -1;
            return 1;
        }
        if (PyErr_Occurred())
            return -1;
    }
    if (p->code == 1)
        return PyDict_SetItem(bound, p->name, p->extra) < 0 ? -1 : 1;
    if (p->code == 2) {
        PyObject *v = PyObject_CallNoArgs(p->extra);
        if (!v)
            return -1;
        int r = PyDict_SetItem(bound, p->name, v);
        Py_DECREF(v);
        return r < 0 ? -1 : 1;
    }
    return 0;
}

static Py_ssize_t
kw_count(PyObject *kwnames)
{
    return kwnames == NULL ? 0 : PyTuple_GET_SIZE(kwnames);
}

static Py_ssize_t
keyword_param_index(dc_cspec *cs, PyObject *key)
{
    PyObject *idx = PyDict_GetItemWithError(cs->param_index, key);
    if (idx == NULL) {
        return PyErr_Occurred() ? -2 : -1;
    }
    return PyLong_AsSsize_t(idx);
}

static PyObject **
bind_fast_keywords(PyObject *self, dc_cspec *cs, PyObject *const *kwvalues,
                   PyObject *kwnames, Py_ssize_t nargs)
{
    Py_ssize_t nkw = kw_count(kwnames);
    if (nkw == 0) {
        return NULL;
    }
    PyObject **bound = (PyObject **)PyMem_Calloc(cs->npos, sizeof(PyObject *));
    if (bound == NULL) {
        PyErr_NoMemory();
        return NULL;
    }
    for (Py_ssize_t i = 0; i < nkw; i++) {
        PyObject *key = PyTuple_GET_ITEM(kwnames, i);
        Py_ssize_t idx = keyword_param_index(cs, key);
        if (idx == -2) {
            PyMem_Free(bound);
            return NULL;
        }
        if (idx < 0 || idx >= cs->npos) {
            PyObject *qual = get_type_attr(self, S_qualname);
            PyErr_Format(PyExc_TypeError,
                "%U.__init__() got an unexpected keyword argument %R",
                qual, key);
            Py_XDECREF(qual);
            PyMem_Free(bound);
            return NULL;
        }
        if (idx < nargs || bound[idx] != NULL) {
            PyObject *qual = get_type_attr(self, S_qualname);
            PyErr_Format(PyExc_TypeError,
                "%U.__init__() got multiple values for argument %R",
                qual, key);
            Py_XDECREF(qual);
            PyMem_Free(bound);
            return NULL;
        }
        bound[idx] = kwvalues[i];
    }
    return bound;
}

static PyObject *
make_kwargs_dict(PyObject *const *kwvalues, PyObject *kwnames)
{
    Py_ssize_t nkw = kw_count(kwnames);
    if (nkw == 0) {
        return NULL;
    }
    PyObject *kw = PyDict_New();
    if (kw == NULL) {
        return NULL;
    }
    for (Py_ssize_t i = 0; i < nkw; i++) {
        if (PyDict_SetItem(kw, PyTuple_GET_ITEM(kwnames, i), kwvalues[i]) < 0) {
            Py_DECREF(kw);
            return NULL;
        }
    }
    return kw;
}

static void
update_inline_ready(PyObject *self, dc_cspec *cs)
{
#ifndef Py_GIL_DISABLED
    PyTypeObject *tp = Py_TYPE(self);
    cs->inline_ready = 0;
    cs->inline_type_version = 0;
    if (tp != cs->owner_type)
    {
        return;
    }
    for (Py_ssize_t i = 0; i < cs->nassign; i++) {
        dc_slot *a = &cs->assignments[i];
        if (a->type_version != tp->tp_version_tag || a->inline_offset < 0) {
            return;
        }
        if (a->store_kind == DC_STORE_INLINE) {
            if (!(tp->tp_flags & Py_TPFLAGS_INLINE_VALUES) ||
                _PyObject_GetManagedDict(self) != NULL)
            {
                return;
            }
            PyDictValues *values = _PyObject_InlineValues(self);
            if (!values->valid) {
                return;
            }
        }
        else if (a->store_kind != DC_STORE_SLOT) {
            return;
        }
    }
    cs->inline_ready = 1;
    cs->inline_type_version = tp->tp_version_tag;
#endif
}

static int
member_object_slot_offset(PyObject *self, PyObject *descr,
                          Py_ssize_t *offset)
{
    if (!Py_IS_TYPE(descr, &PyMemberDescr_Type)) {
        return 0;
    }
    PyMemberDescrObject *member = (PyMemberDescrObject *)descr;
    PyMemberDef *dmem = member->d_member;
    if (!PyObject_TypeCheck(self, member->d_common.d_type) ||
        (dmem->flags & Py_READONLY) ||
        (dmem->flags & _Py_AFTER_ITEMS) ||
        !(dmem->type == Py_T_OBJECT_EX || dmem->type == _Py_T_OBJECT) ||
        dmem->offset <= 0)
    {
        return 0;
    }
    *offset = dmem->offset;
    return 1;
}

static void
cache_inline_offset(PyObject *self, dc_cspec *cs, dc_slot *a)
{
#ifndef Py_GIL_DISABLED
    PyTypeObject *tp = Py_TYPE(self);
    if (!(tp->tp_flags & Py_TPFLAGS_INLINE_VALUES) ||
        _PyObject_GetManagedDict(self) != NULL)
    {
        return;
    }
    PyDictValues *values = _PyObject_InlineValues(self);
    if (!values->valid) {
        return;
    }
    PyDictKeysObject *keys = ((PyHeapTypeObject *)tp)->ht_cached_keys;
    Py_ssize_t ix = _PyDictKeys_StringLookupSplit(keys, a->name);
    if (ix < 0) {
        return;
    }
    PyObject **value_ptr = &values->values[ix];
    a->inline_offset = (char *)value_ptr - (char *)self;
    a->store_kind = DC_STORE_INLINE;
    update_inline_ready(self, cs);
#endif
}

static void
refresh_store_cache(dc_cspec *cs, dc_slot *a, PyObject *self)
{
    a->store_kind = DC_STORE_GENERIC;
    a->type_version = 0;
    a->inline_offset = -1;
    cs->inline_ready = 0;
    cs->inline_type_version = 0;

    PyTypeObject *tp = Py_TYPE(self);
    if (tp != cs->owner_type) {
        return;
    }

    unsigned int version = 0;
    PyObject *descr = _PyType_LookupRefAndVersion(tp, a->name, &version);
    if (version == 0) {
        Py_XDECREF(descr);
        return;
    }
    Py_ssize_t slot_offset = -1;
    if (descr != NULL &&
        member_object_slot_offset(self, descr, &slot_offset))
    {
        Py_DECREF(descr);
        a->store_kind = DC_STORE_SLOT;
        a->type_version = version;
        a->inline_offset = slot_offset;
        update_inline_ready(self, cs);
        return;
    }
    if (cs->frozen ||
        tp->tp_setattro != PyObject_GenericSetAttr ||
        !(tp->tp_flags & Py_TPFLAGS_INLINE_VALUES))
    {
        Py_XDECREF(descr);
        return;
    }
    if (descr != NULL && Py_TYPE(descr)->tp_descr_set != NULL) {
        Py_DECREF(descr);
        a->type_version = version;
        return;
    }
    Py_XDECREF(descr);

    a->store_kind = DC_STORE_MANAGED;
    a->type_version = version;
    cache_inline_offset(self, cs, a);
}

static int
store_inline_value(PyObject *self, dc_slot *a, PyObject *value)
{
#ifdef Py_GIL_DISABLED
    return -1;
#else
    PyTypeObject *tp = Py_TYPE(self);
    if (!(tp->tp_flags & Py_TPFLAGS_INLINE_VALUES) ||
        _PyObject_GetManagedDict(self) != NULL ||
        a->inline_offset < 0)
    {
        return -1;
    }
    PyDictValues *values = _PyObject_InlineValues(self);
    if (!values->valid) {
        return -1;
    }
    PyObject **value_ptr = (PyObject **)((char *)self + a->inline_offset);
    PyObject *old_value = *value_ptr;
    *value_ptr = Py_NewRef(value);
    if (old_value == NULL) {
        Py_ssize_t ix = value_ptr - values->values;
        _PyDictValues_AddToInsertionOrder(values, ix);
    }
    else {
        Py_DECREF(old_value);
    }
    return 0;
#endif
}

static int
store_slot_value(PyObject *self, dc_slot *a, PyObject *value)
{
    if (a->inline_offset <= 0) {
        return -1;
    }
    PyObject **value_ptr = (PyObject **)((char *)self + a->inline_offset);
    PyObject *old_value = *value_ptr;
    *value_ptr = Py_NewRef(value);
    Py_XDECREF(old_value);
    return 0;
}

static int
inline_cache_ready(PyObject *self, dc_cspec *cs)
{
#ifdef Py_GIL_DISABLED
    return 0;
#else
    PyTypeObject *tp = Py_TYPE(self);
    if (tp != cs->owner_type ||
        cs->frozen)
    {
        return 0;
    }
    if (!cs->inline_ready ||
        cs->inline_type_version != tp->tp_version_tag)
    {
        cs->inline_ready = 0;
        return 0;
    }
    for (Py_ssize_t i = 0; i < cs->nassign; i++) {
        if (cs->assignments[i].store_kind == DC_STORE_INLINE) {
            if (!(tp->tp_flags & Py_TPFLAGS_INLINE_VALUES) ||
                _PyObject_GetManagedDict(self) != NULL)
            {
                return 0;
            }
            PyDictValues *values = _PyObject_InlineValues(self);
            return values->valid;
        }
    }
    return 1;
#endif
}

static void
store_direct_known(PyObject *self, dc_slot *a, PyObject *value,
                   PyDictValues *values)
{
    if (a->store_kind == DC_STORE_SLOT) {
        (void)store_slot_value(self, a, value);
        return;
    }
    PyObject **value_ptr = (PyObject **)((char *)self + a->inline_offset);
    PyObject *old_value = *value_ptr;
    *value_ptr = Py_NewRef(value);
    if (old_value == NULL) {
        Py_ssize_t ix = value_ptr - values->values;
        _PyDictValues_AddToInsertionOrder(values, ix);
    }
    else {
        Py_DECREF(old_value);
    }
}

static int
try_no_kw_inline_init(PyObject *self, dc_cspec *cs,
                      PyObject *const *args, Py_ssize_t nargs,
                      PyObject *kwnames)
{
#ifdef Py_GIL_DISABLED
    return 0;
#else
    if (kw_count(kwnames) != 0 || nargs > cs->npos ||
        cs->nassign != cs->npos || !inline_cache_ready(self, cs))
    {
        return 0;
    }

    for (Py_ssize_t i = nargs; i < cs->npos; i++) {
        if (cs->pos[i].code == 0) {
            /* Let the regular binder produce the exact TypeError text. */
            return 0;
        }
    }

    PyDictValues *values = (Py_TYPE(self)->tp_flags & Py_TPFLAGS_INLINE_VALUES)
        ? _PyObject_InlineValues(self) : NULL;
    for (Py_ssize_t i = 0; i < cs->npos; i++) {
        dc_slot *p = &cs->pos[i];
        PyObject *value;
        int owned = 0;
        if (i < nargs) {
            value = args[i];
        }
        else if (p->code == 1) {
            value = p->extra;
        }
        else {
            value = PyObject_CallNoArgs(p->extra);
            if (value == NULL) {
                return -1;
            }
            owned = 1;
        }
        store_direct_known(self, &cs->assignments[i], value, values);
        if (owned) {
            Py_DECREF(value);
        }
    }
    return 1;
#endif
}

static int
store_attr(PyObject *self, dc_cspec *cs, dc_slot *a, PyObject *value)
{
    if (!cs->frozen) {
        PyTypeObject *tp = Py_TYPE(self);
        if (tp == cs->owner_type) {
            if (a->store_kind == DC_STORE_UNKNOWN ||
                a->type_version != tp->tp_version_tag)
            {
                refresh_store_cache(cs, a, self);
            }
            if (a->type_version == tp->tp_version_tag) {
                if (a->store_kind == DC_STORE_INLINE &&
                    store_inline_value(self, a, value) == 0)
                {
                    return 0;
                }
                if (a->store_kind == DC_STORE_SLOT &&
                    store_slot_value(self, a, value) == 0)
                {
                    return 0;
                }
                if (a->store_kind == DC_STORE_MANAGED) {
                    int res = PyObject_SetAttr(self, a->name, value);
                    if (res == 0) {
                        cache_inline_offset(self, cs, a);
                    }
                    return res;
                }
            }
        }
        return PyObject_SetAttr(self, a->name, value);
    }
    return PyObject_GenericSetAttr(self, a->name, value);
}

static void
update_load_inline_ready(PyObject *self, dc_field_spec *fs)
{
#ifndef Py_GIL_DISABLED
    PyTypeObject *tp = Py_TYPE(self);
    fs->inline_ready = 0;
    fs->inline_type_version = 0;
    if (tp != fs->owner_type) {
        return;
    }
    for (Py_ssize_t i = 0; i < fs->n; i++) {
        dc_read_slot *r = &fs->fields[i];
        if (r->type_version != tp->tp_version_tag || r->inline_offset < 0) {
            return;
        }
        if (r->load_kind == DC_LOAD_INLINE) {
            if (!(tp->tp_flags & Py_TPFLAGS_INLINE_VALUES) ||
                _PyObject_GetManagedDict(self) != NULL)
            {
                return;
            }
            PyDictValues *values = _PyObject_InlineValues(self);
            if (!values->valid) {
                return;
            }
        }
        else if (r->load_kind != DC_LOAD_SLOT) {
            return;
        }
    }
    fs->inline_ready = 1;
    fs->inline_type_version = tp->tp_version_tag;
#endif
}

static void
cache_read_inline_offset(PyObject *self, dc_field_spec *fs, dc_read_slot *r)
{
#ifndef Py_GIL_DISABLED
    PyTypeObject *tp = Py_TYPE(self);
    if (tp != fs->owner_type ||
        !(tp->tp_flags & Py_TPFLAGS_INLINE_VALUES) ||
        _PyObject_GetManagedDict(self) != NULL)
    {
        return;
    }
    PyDictValues *values = _PyObject_InlineValues(self);
    if (!values->valid) {
        return;
    }
    PyDictKeysObject *keys = ((PyHeapTypeObject *)tp)->ht_cached_keys;
    Py_ssize_t ix = _PyDictKeys_StringLookupSplit(keys, r->name);
    if (ix < 0 || values->values[ix] == NULL) {
        return;
    }
    PyObject **value_ptr = &values->values[ix];
    r->inline_offset = (char *)value_ptr - (char *)self;
    r->load_kind = DC_LOAD_INLINE;
    update_load_inline_ready(self, fs);
#endif
}

static void
refresh_load_cache(PyObject *self, dc_field_spec *fs, dc_read_slot *r)
{
    r->load_kind = DC_LOAD_GENERIC;
    r->type_version = 0;
    r->inline_offset = -1;
    fs->inline_ready = 0;
    fs->inline_type_version = 0;

    PyTypeObject *tp = Py_TYPE(self);
    if (tp != fs->owner_type) {
        return;
    }
    unsigned int version = 0;
    PyObject *descr = _PyType_LookupRefAndVersion(tp, r->name, &version);
    if (version == 0) {
        Py_XDECREF(descr);
        return;
    }
    Py_ssize_t slot_offset = -1;
    if (descr != NULL &&
        member_object_slot_offset(self, descr, &slot_offset))
    {
        Py_DECREF(descr);
        r->load_kind = DC_LOAD_SLOT;
        r->type_version = version;
        r->inline_offset = slot_offset;
        update_load_inline_ready(self, fs);
        return;
    }
    if (!(tp->tp_flags & Py_TPFLAGS_INLINE_VALUES)) {
        Py_XDECREF(descr);
        return;
    }
    if (descr != NULL && Py_TYPE(descr)->tp_descr_set != NULL) {
        Py_DECREF(descr);
        r->type_version = version;
        return;
    }
    Py_XDECREF(descr);

    r->type_version = version;
    cache_read_inline_offset(self, fs, r);
}

static int
field_spec_inline_ready(PyObject *self, dc_field_spec *fs)
{
#ifdef Py_GIL_DISABLED
    return 0;
#else
    PyTypeObject *tp = Py_TYPE(self);
    if (tp != fs->owner_type) {
        return 0;
    }
    if (!fs->inline_ready ||
        fs->inline_type_version != tp->tp_version_tag)
    {
        fs->inline_ready = 0;
        return 0;
    }
    for (Py_ssize_t i = 0; i < fs->n; i++) {
        if (fs->fields[i].load_kind == DC_LOAD_INLINE) {
            if (!(tp->tp_flags & Py_TPFLAGS_INLINE_VALUES) ||
                _PyObject_GetManagedDict(self) != NULL)
            {
                return 0;
            }
            PyDictValues *values = _PyObject_InlineValues(self);
            if (!values->valid) {
                return 0;
            }
            break;
        }
    }
    return 1;
#endif
}

static PyObject *
load_inline_known(PyObject *self, dc_read_slot *r)
{
#ifdef Py_GIL_DISABLED
    return NULL;
#else
    if (r->inline_offset < 0) {
        return NULL;
    }
    PyObject **value_ptr = (PyObject **)((char *)self + r->inline_offset);
    return Py_XNewRef(*value_ptr);
#endif
}

static PyObject *
load_field(PyObject *self, dc_field_spec *fs, dc_read_slot *r, int fast)
{
    if (fast) {
        PyObject *value = load_inline_known(self, r);
        if (value != NULL) {
            return value;
        }
    }
    else if (fs != NULL && Py_TYPE(self) == fs->owner_type) {
        PyTypeObject *tp = Py_TYPE(self);
        if (r->load_kind == DC_LOAD_UNKNOWN ||
            r->type_version != tp->tp_version_tag)
        {
            refresh_load_cache(self, fs, r);
        }
        if (r->type_version == tp->tp_version_tag &&
            r->load_kind == DC_LOAD_INLINE)
        {
            PyObject *value = load_inline_known(self, r);
            if (value != NULL) {
                return value;
            }
        }
    }
    return PyObject_GetAttr(self, r->name);
}

static PyObject *
dc_init_impl(PyObject *self, dc_cspec *cs, PyObject *const *args,
             Py_ssize_t nargs, PyObject *kwnames)
{
    if (cs == NULL) {
        /* Shared descriptor fallback: fetch the capsule.  The common case
           finds it in the type's own dict (borrowed, no descriptor
           machinery).  A non-dataclass subclass that inherited the method
           falls back to a full MRO lookup. */
        PyTypeObject *tp = Py_TYPE(self);
        PyObject *cap = tp->tp_dict
            ? PyDict_GetItemWithError(tp->tp_dict, S_c_spec) : NULL;
        int cap_owned = 0;
        if (!cap) {
            if (PyErr_Occurred())
                return NULL;
            cap = PyObject_GetAttr((PyObject *)tp, S_c_spec);
            if (!cap)
                return NULL;
            cap_owned = 1;
        }
        cs = (dc_cspec *)PyCapsule_GetPointer(cap, NULL);
        if (cap_owned)
            Py_DECREF(cap);
        if (!cs)
            return NULL;
    }

    if (cs->fast) {
        int done = try_no_kw_inline_init(self, cs, args, nargs, kwnames);
        if (done > 0) {
            Py_RETURN_NONE;
        }
        if (done < 0) {
            return NULL;
        }
    }

    /* ---------------- fast path: bind + assign in one pass --------------- */
    if (cs->fast) {
        if (nargs > cs->npos) {
            raise_too_many(self, cs->npos, nargs);
            return NULL;
        }
        Py_ssize_t nkw = kw_count(kwnames);
        PyObject *const *kwvalues = args + nargs;
        PyObject **kwbound = NULL;
        if (nkw) {
            kwbound = bind_fast_keywords(self, cs, kwvalues, kwnames, nargs);
            if (kwbound == NULL) {
                return NULL;
            }
        }
        PyObject *missing = NULL;
        for (Py_ssize_t i = 0; i < cs->npos; i++) {
            dc_slot *p = &cs->pos[i];
            dc_slot *a = &cs->assignments[i];
            PyObject *value = NULL;
            int owned = 0;
            if (i < nargs) {
                value = args[i];
            }
            else {
                PyObject *kwv = kwbound ? kwbound[i] : NULL;
                if (kwv) {
                    value = kwv;
                }
                else if (p->code == 1) {
                    value = p->extra;
                }
                else if (p->code == 2) {
                    value = PyObject_CallNoArgs(p->extra);
                    if (!value) goto fast_err;
                    owned = 1;
                }
                else {
                    if (!missing) {
                        missing = PyList_New(0);
                        if (!missing) goto fast_err;
                    }
                    PyList_Append(missing, p->name);
                    continue;
                }
            }
            int r = store_attr(self, cs, a, value);
            if (owned) Py_DECREF(value);
            if (r < 0) goto fast_err;
        }
        if (missing) {
            raise_missing(self, "positional", missing);
            goto fast_err;
        }
        PyMem_Free(kwbound);
        Py_RETURN_NONE;
    fast_err:
        Py_XDECREF(missing);
        PyMem_Free(kwbound);
        return NULL;
    }

    /* ------------------------- general path ------------------------- */
    Py_ssize_t nkw = kw_count(kwnames);
    PyObject *kw = make_kwargs_dict(args + nargs, kwnames);
    if (nkw && kw == NULL) {
        return NULL;
    }
    PyObject *bound = PyDict_New();
    if (!bound) {
        Py_XDECREF(kw);
        return NULL;
    }

    if (nargs > cs->npos) {
        raise_too_many(self, cs->npos, nargs);
        goto gerror;
    }
    for (Py_ssize_t i = 0; i < nargs; i++) {
        dc_slot *p = &cs->pos[i];
        if (kw) {
            int c = PyDict_Contains(kw, p->name);
            if (c < 0) goto gerror;
            if (c) {
                PyObject *qual = get_type_attr(self, S_qualname);
                PyErr_Format(PyExc_TypeError,
                    "%U.__init__() got multiple values for argument %R",
                    qual, p->name);
                Py_XDECREF(qual);
                goto gerror;
            }
        }
        if (PyDict_SetItem(bound, p->name, args[i]) < 0)
            goto gerror;
    }

    PyObject *missing_pos = PyList_New(0);
    if (!missing_pos) goto gerror;
    for (Py_ssize_t i = nargs; i < cs->npos; i++) {
        int r = bind_slot(bound, kw, &cs->pos[i]);
        if (r < 0) { Py_DECREF(missing_pos); goto gerror; }
        if (r == 0) PyList_Append(missing_pos, cs->pos[i].name);
    }
    if (PyList_GET_SIZE(missing_pos) > 0) {
        raise_missing(self, "positional", missing_pos);
        Py_DECREF(missing_pos);
        goto gerror;
    }
    Py_DECREF(missing_pos);

    PyObject *missing_kw = PyList_New(0);
    if (!missing_kw) goto gerror;
    for (Py_ssize_t i = 0; i < cs->nkw; i++) {
        int r = bind_slot(bound, kw, &cs->kwonly[i]);
        if (r < 0) { Py_DECREF(missing_kw); goto gerror; }
        if (r == 0) PyList_Append(missing_kw, cs->kwonly[i].name);
    }
    if (PyList_GET_SIZE(missing_kw) > 0) {
        raise_missing(self, "keyword-only", missing_kw);
        Py_DECREF(missing_kw);
        goto gerror;
    }
    Py_DECREF(missing_kw);

    if (kw && PyDict_GET_SIZE(kw) > 0) {
        PyObject *k, *v;
        Py_ssize_t pp = 0;
        PyDict_Next(kw, &pp, &k, &v);
        PyObject *qual = get_type_attr(self, S_qualname);
        PyErr_Format(PyExc_TypeError,
            "%U.__init__() got an unexpected keyword argument %R", qual, k);
        Py_XDECREF(qual);
        goto gerror;
    }

    for (Py_ssize_t i = 0; i < cs->nassign; i++) {
        dc_slot *a = &cs->assignments[i];
        PyObject *value;
        int owned = 0;
        if (a->code == 0) {
            value = PyDict_GetItemWithError(bound, a->name);
            if (!value) {
                if (!PyErr_Occurred())
                    PyErr_SetObject(PyExc_KeyError, a->name);
                goto gerror;
            }
        }
        else if (a->code == 1) {
            value = PyObject_CallNoArgs(a->extra);
            if (!value) goto gerror;
            owned = 1;
        }
        else {
            value = a->extra;
        }
        int r = store_attr(self, cs, a, value);
        if (owned) Py_DECREF(value);
        if (r < 0) goto gerror;
    }

    if (cs->has_post_init) {
        PyObject *callargs = PyTuple_New(cs->ninitvar);
        if (!callargs) goto gerror;
        for (Py_ssize_t i = 0; i < cs->ninitvar; i++) {
            PyObject *v = PyDict_GetItemWithError(bound, cs->initvar_names[i]);
            if (!v) {
                if (!PyErr_Occurred())
                    PyErr_SetObject(PyExc_KeyError, cs->initvar_names[i]);
                Py_DECREF(callargs);
                goto gerror;
            }
            PyTuple_SET_ITEM(callargs, i, Py_NewRef(v));
        }
        PyObject *meth = PyObject_GetAttr(self, S_post_init);
        if (!meth) { Py_DECREF(callargs); goto gerror; }
        PyObject *res = PyObject_Call(meth, callargs, NULL);
        Py_DECREF(meth);
        Py_DECREF(callargs);
        if (!res) goto gerror;
        Py_DECREF(res);
    }

    Py_DECREF(bound);
    Py_XDECREF(kw);
    Py_RETURN_NONE;

gerror:
    Py_DECREF(bound);
    Py_XDECREF(kw);
    return NULL;
}

static int
dc_tp_init(PyObject *self, PyObject *args_tuple, PyObject *kwargs)
{
    if (!PyTuple_Check(args_tuple)) {
        PyErr_SetString(PyExc_TypeError, "tp_init args must be a tuple");
        return -1;
    }

    PyTypeObject *tp = Py_TYPE(self);
    PyObject *cap = tp->tp_dict
        ? PyDict_GetItemWithError(tp->tp_dict, S_c_spec) : NULL;
    int cap_owned = 0;
    if (!cap) {
        if (PyErr_Occurred()) {
            return -1;
        }
        cap = PyObject_GetAttr((PyObject *)tp, S_c_spec);
        if (!cap) {
            return -1;
        }
        cap_owned = 1;
    }
    dc_cspec *cs = (dc_cspec *)PyCapsule_GetPointer(cap, NULL);
    if (cap_owned) {
        Py_DECREF(cap);
    }
    if (cs == NULL) {
        return -1;
    }

    Py_ssize_t nargs = PyTuple_GET_SIZE(args_tuple);
    Py_ssize_t nkw = kwargs ? PyDict_GET_SIZE(kwargs) : 0;
    PyObject **stack = NULL;
    PyObject *kwnames = NULL;
    Py_ssize_t total = nargs + nkw;
    if (total > 0) {
        stack = (PyObject **)PyMem_Malloc(total * sizeof(PyObject *));
        if (stack == NULL) {
            PyErr_NoMemory();
            return -1;
        }
    }
    for (Py_ssize_t i = 0; i < nargs; i++) {
        stack[i] = PyTuple_GET_ITEM(args_tuple, i);
    }
    if (nkw) {
        kwnames = PyTuple_New(nkw);
        if (kwnames == NULL) {
            PyMem_Free(stack);
            return -1;
        }
        Py_ssize_t pos = 0;
        PyObject *key, *value;
        Py_ssize_t pp = 0;
        while (PyDict_Next(kwargs, &pp, &key, &value)) {
            PyTuple_SET_ITEM(kwnames, pos, Py_NewRef(key));
            stack[nargs + pos] = value;
            pos++;
        }
    }

    PyObject *empty_stack[1];
    PyObject *res = dc_init_impl(self, cs, total ? stack : empty_stack,
                                 nargs, kwnames);
    Py_XDECREF(kwnames);
    PyMem_Free(stack);
    if (res == NULL) {
        return -1;
    }
    Py_DECREF(res);
    return 0;
}

/* ----------------------------- __repr__ ----------------------------- */

static PyObject *
dc_repr_impl(PyObject *self, dc_field_spec *fs)
{
    int status = Py_ReprEnter(self);
    if (status != 0)
        return status > 0 ? PyUnicode_FromString("...") : NULL;

    PyObject *result = NULL;
    PyObject *qual = get_type_attr(self, S_qualname);
    PyObject *names = fs ? NULL : get_type_attr(self, S_repr_fields);
    if (!qual || (!fs && !names))
        goto done;

    Py_ssize_t n = fs ? fs->n : PyTuple_GET_SIZE(names);
    int fast = fs ? field_spec_inline_ready(self, fs) : 0;
    PyObject *parts = PyList_New(n);
    if (!parts)
        goto done;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *name = fs ? fs->fields[i].name : PyTuple_GET_ITEM(names, i);
        PyObject *val = fs ? load_field(self, fs, &fs->fields[i], fast)
                           : PyObject_GetAttr(self, name);
        if (!val) { Py_DECREF(parts); goto done; }
        PyObject *r = PyObject_Repr(val);
        Py_DECREF(val);
        if (!r) { Py_DECREF(parts); goto done; }
        PyObject *part = PyUnicode_FromFormat("%U=%U", name, r);
        Py_DECREF(r);
        if (!part) { Py_DECREF(parts); goto done; }
        PyList_SET_ITEM(parts, i, part);
    }
    PyObject *sep = PyUnicode_FromString(", ");
    PyObject *inner = PyUnicode_Join(sep, parts);
    Py_DECREF(sep);
    Py_DECREF(parts);
    if (!inner)
        goto done;
    result = PyUnicode_FromFormat("%U(%U)", qual, inner);
    Py_DECREF(inner);

done:
    Py_XDECREF(qual);
    Py_XDECREF(names);
    Py_ReprLeave(self);
    return result;
}

/* ----------------------------- __eq__ ------------------------------- */

static PyObject *
dc_eq_impl(PyObject *self, PyObject *other, dc_field_spec *fs)
{
    if (self == other)
        Py_RETURN_TRUE;
    if (Py_TYPE(self) != Py_TYPE(other))
        Py_RETURN_NOTIMPLEMENTED;

    PyObject *names = fs ? NULL : get_type_attr(self, S_eq_fields);
    if (!fs && !names)
        return NULL;
    Py_ssize_t n = fs ? fs->n : PyTuple_GET_SIZE(names);
    int fast_self = fs ? field_spec_inline_ready(self, fs) : 0;
    int fast_other = fs ? field_spec_inline_ready(other, fs) : 0;
    PyObject *result = Py_True;
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *name = fs ? fs->fields[i].name : PyTuple_GET_ITEM(names, i);
        PyObject *a = fs ? load_field(self, fs, &fs->fields[i], fast_self)
                         : PyObject_GetAttr(self, name);
        if (!a) { Py_XDECREF(names); return NULL; }
        PyObject *b = fs ? load_field(other, fs, &fs->fields[i], fast_other)
                         : PyObject_GetAttr(other, name);
        if (!b) { Py_DECREF(a); Py_XDECREF(names); return NULL; }
        /* RichCompare (not RichCompareBool): the latter short-circuits on
           identity, which would make a NaN field equal to itself. */
        PyObject *r = PyObject_RichCompare(a, b, Py_EQ);
        Py_DECREF(a);
        Py_DECREF(b);
        if (!r) { Py_XDECREF(names); return NULL; }
        int cmp = PyObject_IsTrue(r);
        Py_DECREF(r);
        if (cmp < 0) { Py_XDECREF(names); return NULL; }
        if (cmp == 0) { result = Py_False; break; }
    }
    Py_XDECREF(names);
    return Py_NewRef(result);
}

/* ----------------------------- __hash__ ----------------------------- */

static PyObject *
dc_hash_impl(PyObject *self, dc_field_spec *fs)
{
    PyObject *names = fs ? NULL : get_type_attr(self, S_hash_fields);
    if (!fs && !names)
        return NULL;
    Py_ssize_t n = fs ? fs->n : PyTuple_GET_SIZE(names);
    int fast = fs ? field_spec_inline_ready(self, fs) : 0;
    PyObject *t = PyTuple_New(n);
    if (!t) { Py_XDECREF(names); return NULL; }
    for (Py_ssize_t i = 0; i < n; i++) {
        PyObject *v = fs ? load_field(self, fs, &fs->fields[i], fast)
                         : PyObject_GetAttr(self, PyTuple_GET_ITEM(names, i));
        if (!v) { Py_DECREF(t); Py_XDECREF(names); return NULL; }
        PyTuple_SET_ITEM(t, i, v);
    }
    Py_XDECREF(names);
    Py_hash_t h = PyObject_Hash(t);
    Py_DECREF(t);
    if (h == -1)
        return NULL;
    return PyLong_FromSsize_t(h);
}

/* ----------------------------- module ------------------------------- */

typedef enum {
    DC_METHOD_INIT,
    DC_METHOD_REPR,
    DC_METHOD_EQ,
    DC_METHOD_HASH
} dc_method_kind;

typedef struct {
    PyObject_HEAD
    vectorcallfunc vectorcall;
    dc_method_kind kind;
    PyObject *capsule;
    dc_cspec *cs;
    dc_field_spec *field_spec;
    PyObject *name;
    PyObject *qualname;
    PyObject *annotate;
    PyObject *wrapped;
} dc_method_descr;

static PyTypeObject DCMethodDescr_Type;

static PyObject *
dc_method_arg_error(dc_method_descr *descr, Py_ssize_t nargs,
                    Py_ssize_t expected)
{
    PyErr_Format(PyExc_TypeError,
                 "%U() takes exactly %zd argument%s (%zd given)",
                 descr->name, expected, expected == 1 ? "" : "s", nargs);
    return NULL;
}

static int
dc_method_no_keywords(dc_method_descr *descr, PyObject *kwnames)
{
    if (kw_count(kwnames) == 0) {
        return 0;
    }
    PyErr_Format(PyExc_TypeError,
                 "%U() takes no keyword arguments", descr->name);
    return -1;
}

static PyObject *
dc_method_vectorcall(PyObject *callable, PyObject *const *args,
                     size_t nargsf, PyObject *kwnames)
{
    dc_method_descr *descr = (dc_method_descr *)callable;
    Py_ssize_t nargs = PyVectorcall_NARGS(nargsf);
    if (nargs < 1) {
        PyErr_Format(PyExc_TypeError,
                     "unbound method %U() needs an argument", descr->name);
        return NULL;
    }
    PyObject *self = args[0];

    switch (descr->kind) {
        case DC_METHOD_INIT:
            return dc_init_impl(self, descr->cs, args + 1, nargs - 1,
                                kwnames);
        case DC_METHOD_REPR:
            if (dc_method_no_keywords(descr, kwnames) < 0) {
                return NULL;
            }
            if (nargs != 1) {
                return dc_method_arg_error(descr, nargs, 1);
            }
            return dc_repr_impl(self, descr->field_spec);
        case DC_METHOD_EQ:
            if (dc_method_no_keywords(descr, kwnames) < 0) {
                return NULL;
            }
            if (nargs != 2) {
                return dc_method_arg_error(descr, nargs, 2);
            }
            return dc_eq_impl(self, args[1], descr->field_spec);
        case DC_METHOD_HASH:
            if (dc_method_no_keywords(descr, kwnames) < 0) {
                return NULL;
            }
            if (nargs != 1) {
                return dc_method_arg_error(descr, nargs, 1);
            }
            return dc_hash_impl(self, descr->field_spec);
    }
    Py_UNREACHABLE();
}

static PyObject *
dc_method_descr_get(PyObject *descr, PyObject *obj, PyObject *Py_UNUSED(type))
{
    if (obj == NULL || obj == Py_None) {
        return Py_NewRef(descr);
    }
    return PyMethod_New(descr, obj);
}

static int
dc_method_traverse(PyObject *self, visitproc visit, void *arg)
{
    dc_method_descr *descr = (dc_method_descr *)self;
    Py_VISIT(descr->capsule);
    if (descr->field_spec != NULL) {
        Py_VISIT(descr->field_spec->owner_tuple);
    }
    Py_VISIT(descr->name);
    Py_VISIT(descr->qualname);
    Py_VISIT(descr->annotate);
    Py_VISIT(descr->wrapped);
    return 0;
}

static int
dc_method_clear(PyObject *self)
{
    dc_method_descr *descr = (dc_method_descr *)self;
    Py_CLEAR(descr->capsule);
    free_field_spec(descr->field_spec);
    descr->field_spec = NULL;
    descr->cs = NULL;
    Py_CLEAR(descr->name);
    Py_CLEAR(descr->qualname);
    Py_CLEAR(descr->annotate);
    Py_CLEAR(descr->wrapped);
    return 0;
}

static void
dc_method_dealloc(PyObject *self)
{
    PyObject_GC_UnTrack(self);
    (void)dc_method_clear(self);
    PyObject_GC_Del(self);
}

static PyObject *
dc_method_repr(PyObject *self)
{
    dc_method_descr *descr = (dc_method_descr *)self;
    return PyUnicode_FromFormat("<dataclass method %U>", descr->name);
}

#define DC_MD_OFF(x) offsetof(dc_method_descr, x)

static PyMemberDef dc_method_members[] = {
    {"__name__", Py_T_OBJECT_EX, DC_MD_OFF(name), Py_READONLY, NULL},
    {"__qualname__", Py_T_OBJECT_EX, DC_MD_OFF(qualname), Py_READONLY, NULL},
    {"__annotate__", Py_T_OBJECT_EX, DC_MD_OFF(annotate), Py_READONLY, NULL},
    {"__wrapped__", Py_T_OBJECT_EX, DC_MD_OFF(wrapped), Py_READONLY, NULL},
    {NULL}
};

static PyTypeObject DCMethodDescr_Type = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "_dataclasses.method_descriptor",
    .tp_basicsize = sizeof(dc_method_descr),
    .tp_dealloc = dc_method_dealloc,
    .tp_traverse = dc_method_traverse,
    .tp_clear = dc_method_clear,
    .tp_repr = dc_method_repr,
    .tp_call = PyVectorcall_Call,
    .tp_vectorcall_offset = offsetof(dc_method_descr, vectorcall),
    .tp_flags = Py_TPFLAGS_DEFAULT |
                Py_TPFLAGS_HAVE_GC |
                Py_TPFLAGS_METHOD_DESCRIPTOR |
                Py_TPFLAGS_HAVE_VECTORCALL,
    .tp_descr_get = dc_method_descr_get,
    .tp_members = dc_method_members,
    .tp_free = PyObject_GC_Del,
};

static PyObject *
method_qualname(PyObject *cls, const char *name)
{
    if (cls == NULL) {
        return PyUnicode_InternFromString(name);
    }
    PyObject *cls_qualname = PyObject_GetAttr(cls, S_qualname);
    if (cls_qualname == NULL) {
        return NULL;
    }
    PyObject *qualname = PyUnicode_FromFormat("%U.%s", cls_qualname, name);
    Py_DECREF(cls_qualname);
    return qualname;
}

static PyObject *
make_method_descr(const char *name, dc_method_kind kind,
                  PyObject *capsule, dc_field_spec *field_spec,
                  PyObject *cls, PyObject *annotate, PyObject *wrapped)
{
    dc_method_descr *descr = PyObject_GC_New(dc_method_descr,
                                             &DCMethodDescr_Type);
    if (descr == NULL) {
        return NULL;
    }
    descr->vectorcall = dc_method_vectorcall;
    descr->kind = kind;
    descr->name = NULL;
    descr->qualname = NULL;
    descr->capsule = Py_XNewRef(capsule);
    descr->cs = NULL;
    descr->field_spec = field_spec;
    descr->annotate = Py_XNewRef(annotate);
    descr->wrapped = Py_XNewRef(wrapped);
    PyObject_GC_Track(descr);
    if (capsule != NULL) {
        descr->cs = (dc_cspec *)PyCapsule_GetPointer(capsule, NULL);
        if (descr->cs == NULL) {
            Py_DECREF(descr);
            return NULL;
        }
    }
    descr->name = PyUnicode_InternFromString(name);
    if (descr->name == NULL) {
        Py_DECREF(descr);
        return NULL;
    }
    descr->qualname = method_qualname(cls, name);
    if (descr->qualname == NULL) {
        Py_DECREF(descr);
        return NULL;
    }
    return (PyObject *)descr;
}

static int
add_method_descr(PyObject *m, const char *attr,
                 const char *method_name, dc_method_kind kind)
{
    PyObject *o = make_method_descr(method_name, kind, NULL, NULL,
                                    NULL, NULL, NULL);
    if (!o)
        return -1;
    int r = PyModule_AddObjectRef(m, attr, o);
    Py_DECREF(o);
    return r;
}

static PyObject *
make_init_descr(PyObject *Py_UNUSED(mod), PyObject *const *args,
                Py_ssize_t nargs)
{
    if (nargs != 1 && nargs != 4) {
        PyErr_Format(PyExc_TypeError,
                     "make_init() takes 1 or 4 arguments (%zd given)",
                     nargs);
        return NULL;
    }
    PyObject *cls = NULL;
    PyObject *annotate = NULL;
    PyObject *wrapped = NULL;
    if (nargs == 4) {
        cls = args[1];
        if (!PyType_Check(cls)) {
            PyErr_SetString(PyExc_TypeError,
                            "make_init() argument 2 must be a type");
            return NULL;
        }
        annotate = args[2];
        wrapped = args[3];
    }
    return make_method_descr("__init__", DC_METHOD_INIT, args[0], NULL,
                             cls, annotate, wrapped);
}

static PyObject *
make_field_method_descr(PyObject *const *args, Py_ssize_t nargs,
                        const char *name, dc_method_kind kind)
{
    if (nargs != 2) {
        PyErr_Format(PyExc_TypeError,
                     "%s factory takes exactly 2 arguments (%zd given)",
                     name, nargs);
        return NULL;
    }
    dc_field_spec *fs = make_field_spec(args[0], args[1]);
    if (fs == NULL) {
        return NULL;
    }
    return make_method_descr(name, kind, NULL, fs, args[1], NULL, NULL);
}

static PyObject *
make_repr_descr(PyObject *Py_UNUSED(mod), PyObject *const *args,
                Py_ssize_t nargs)
{
    return make_field_method_descr(args, nargs, "__repr__", DC_METHOD_REPR);
}

static PyObject *
make_eq_descr(PyObject *Py_UNUSED(mod), PyObject *const *args,
              Py_ssize_t nargs)
{
    return make_field_method_descr(args, nargs, "__eq__", DC_METHOD_EQ);
}

static PyObject *
make_hash_descr(PyObject *Py_UNUSED(mod), PyObject *const *args,
                Py_ssize_t nargs)
{
    return make_field_method_descr(args, nargs, "__hash__", DC_METHOD_HASH);
}

static PyObject *
install_tp_init(PyObject *Py_UNUSED(mod), PyObject *cls)
{
    if (!PyType_Check(cls)) {
        PyErr_SetString(PyExc_TypeError,
                        "install_tp_init() argument must be a type");
        return NULL;
    }
    PyTypeObject *tp = (PyTypeObject *)cls;
    tp->tp_init = dc_tp_init;
    PyType_Modified(tp);
    Py_RETURN_NONE;
}

static PyMethodDef dc_module_methods[] = {
    {"make_cspec", _PyCFunction_CAST(make_cspec), METH_FASTCALL,
     "Build a C-native cached spec capsule from an _InitSpec and owner type."},
    {"make_init", _PyCFunction_CAST(make_init_descr), METH_FASTCALL,
     "Build a per-class __init__ descriptor from a C spec capsule."},
    {"make_repr", _PyCFunction_CAST(make_repr_descr), METH_FASTCALL,
     "Build a per-class __repr__ descriptor from field names."},
    {"make_eq", _PyCFunction_CAST(make_eq_descr), METH_FASTCALL,
     "Build a per-class __eq__ descriptor from field names."},
    {"make_hash", _PyCFunction_CAST(make_hash_descr), METH_FASTCALL,
     "Build a per-class __hash__ descriptor from field names."},
    {"install_tp_init", install_tp_init, METH_O,
     "Install the direct dataclass tp_init fast path on a type."},
    {NULL, NULL, 0, NULL}
};

static int
dc_exec(PyObject *m)
{
#define INTERN(var, str) \
    do { var = PyUnicode_InternFromString(str); if (!var) return -1; } while (0)
    INTERN(S_c_spec, "__dataclass_c_spec__");
    INTERN(S_repr_fields, "__dataclass_repr_fields__");
    INTERN(S_eq_fields, "__dataclass_eq_fields__");
    INTERN(S_hash_fields, "__dataclass_hash_fields__");
    INTERN(S_qualname, "__qualname__");
    INTERN(S_post_init, "__post_init__");
    INTERN(S_pos, "pos");
    INTERN(S_kwonly, "kwonly");
    INTERN(S_assignments, "assignments");
    INTERN(S_initvar_names, "initvar_names");
    INTERN(S_has_post_init, "has_post_init");
    INTERN(S_frozen, "frozen");
    INTERN(S_fast, "fast");
#undef INTERN

    if (PyType_Ready(&DCMethodDescr_Type) < 0) return -1;
    if (add_method_descr(m, "init", "__init__", DC_METHOD_INIT) < 0) return -1;
    if (add_method_descr(m, "repr", "__repr__", DC_METHOD_REPR) < 0) return -1;
    if (add_method_descr(m, "eq", "__eq__", DC_METHOD_EQ) < 0) return -1;
    if (add_method_descr(m, "hash", "__hash__", DC_METHOD_HASH) < 0) return -1;
    return 0;
}

static PyModuleDef_Slot dc_slots[] = {
    {Py_mod_exec, dc_exec},
    {Py_mod_multiple_interpreters, Py_MOD_MULTIPLE_INTERPRETERS_SUPPORTED},
    {0, NULL}
};

static struct PyModuleDef dc_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_dataclasses",
    .m_doc = "C accelerators for dataclass __init__/__repr__/__eq__/__hash__.",
    .m_size = 0,
    .m_methods = dc_module_methods,
    .m_slots = dc_slots,
};

PyMODINIT_FUNC
PyInit__dataclasses(void)
{
    return PyModuleDef_Init(&dc_module);
}
