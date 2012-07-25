#include "sirius.h"

extern "C" void FORTRAN(sirius_set_lattice_vectors)(double* a1, double* a2, double* a3)
{
    sirius_global.set_lattice_vectors(a1, a2, a3);
}

extern "C" void FORTRAN(sirius_global_print_info)()
{
    sirius_global.print_info();
}


/*extern "C" void FORTRAN(sirius_load_atom)(char* label_, int4 label_len)
{
    std::string label(label_, label_len);
    sirius_global.load_atom(label);
} 

extern "C" void FORTRAN(sirius_*/