#include "mpi/communicator.hpp"

#include <mpi4py/mpi4py.h>
#include <pybind11/pybind11.h>
#include <pybind11/pytypes.h>
#include <pybind11/operators.h>
#include <iostream>

namespace sirius {

inline sddk::Communicator make_sirius_comm(pybind11::object py_comm)
{
    PyObject* py_obj = py_comm.ptr();
    MPI_Comm* comm_p  = PyMPIComm_Get(py_obj);
    if(comm_p == NULL) {
        throw std::runtime_error("invalid MPI_Comm object passed");
    }
    int rank = -1;
    MPI_Comm_rank(*comm_p, &rank);

    sddk::Communicator sirius_comm(*comm_p);
    return sirius_comm;
}


inline pybind11::handle make_pycomm(const sddk::Communicator& comm) {
    return pybind11::handle(PyMPIComm_New(comm.mpi_comm()));
}


} // end namespace sirius
