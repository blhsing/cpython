#include "Python.h"
#include "pycore_pystate.h"         // _PyTimeout_Push()
#include "pycore_time.h"            // _PyTime_FromSecondsObject()


static PyObject *
timeout_enter(PyObject *Py_UNUSED(module), PyObject *timeout_obj)
{
    PyTime_t timeout;
    if (_PyTime_FromSecondsObject(&timeout, timeout_obj,
                                  _PyTime_ROUND_TIMEOUT) < 0)
    {
        return NULL;
    }

    PyThreadState *tstate = _PyThreadState_GET();
    if (_PyTimeout_Push(tstate, timeout) < 0) {
        return NULL;
    }

    Py_RETURN_NONE;
}


static PyObject *
timeout_leave(PyObject *Py_UNUSED(module), PyObject *Py_UNUSED(ignored))
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (_PyTimeout_Pop(tstate) < 0) {
        return NULL;
    }

    Py_RETURN_NONE;
}


static PyObject *
timeout_check(PyObject *Py_UNUSED(module), PyObject *Py_UNUSED(ignored))
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (_PyThreadState_CheckCancellation(tstate) < 0) {
        return NULL;
    }

    Py_RETURN_FALSE;
}


static PyObject *
timeout_cancel(PyObject *Py_UNUSED(module), PyObject *args)
{
    PyThreadState *tstate = _PyThreadState_GET();
    if (PyTuple_GET_SIZE(args) == 0) {
        _PyThreadState_RequestCancel(tstate, _PY_CANCEL_GENERIC);
        Py_RETURN_NONE;
    }

    unsigned long id;
    if (!PyArg_ParseTuple(args, "k:cancel", &id)) {
        return NULL;
    }

    int requested = _PyThreadState_RequestCancelByThreadId(
        tstate->interp, id, _PY_CANCEL_GENERIC);
    return PyLong_FromLong(requested);
}


static PyMethodDef timeout_methods[] = {
    {"enter", timeout_enter, METH_O, PyDoc_STR("Enter a timeout block.")},
    {"leave", timeout_leave, METH_NOARGS, PyDoc_STR("Leave a timeout block.")},
    {"check", timeout_check, METH_NOARGS, PyDoc_STR("Check for cancellation.")},
    {"cancel", timeout_cancel, METH_VARARGS,
        PyDoc_STR("Request cooperative cancellation.")},
    {NULL, NULL}
};


static PyModuleDef_Slot timeout_slots[] = {
    _Py_ABI_SLOT,
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {Py_mod_gil, Py_MOD_GIL_NOT_USED},
    {0, NULL}
};


static struct PyModuleDef timeoutmodule = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_timeout",
    .m_doc = "Prototype synchronous timeout support.",
    .m_size = 0,
    .m_methods = timeout_methods,
    .m_slots = timeout_slots,
};


PyMODINIT_FUNC
PyInit__timeout(void)
{
    return PyModuleDef_Init(&timeoutmodule);
}
