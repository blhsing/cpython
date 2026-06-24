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
    if (_PyTimeout_CheckNow(tstate) < 0) {
        return NULL;
    }

    Py_RETURN_FALSE;
}


static PyMethodDef timeout_methods[] = {
    {"enter", timeout_enter, METH_O, PyDoc_STR("Enter a timeout block.")},
    {"leave", timeout_leave, METH_NOARGS, PyDoc_STR("Leave a timeout block.")},
    {"check", timeout_check, METH_NOARGS, PyDoc_STR("Check the current timeout.")},
    {NULL, NULL}
};


static struct PyModuleDef timeoutmodule = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_timeout",
    .m_doc = "Prototype synchronous timeout support.",
    .m_size = 0,
    .m_methods = timeout_methods,
};


PyMODINIT_FUNC
PyInit__timeout(void)
{
    PyObject *module = PyModule_Create(&timeoutmodule);
#ifdef Py_GIL_DISABLED
    if (module != NULL) {
        PyUnstable_Module_SetGIL(module, Py_MOD_GIL_NOT_USED);
    }
#endif
    return module;
}
