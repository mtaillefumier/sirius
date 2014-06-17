// Copyright (c) 2013-2014 Anton Kozhevnikov, Thomas Schulthess
// All rights reserved.
// 
// Redistribution and use in source and binary forms, with or without modification, are permitted provided that 
// the following conditions are met:
// 
// 1. Redistributions of source code must retain the above copyright notice, this list of conditions and the 
//    following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions 
//    and the following disclaimer in the documentation and/or other materials provided with the distribution.
// 
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED 
// WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A 
// PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR 
// ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, 
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER 
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR 
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

/** \file density.cpp
 *   
 *  \brief Contains remaining implementation of sirius::Density class.
 */

#include <thread>
#include <mutex>
#include "density.h"

namespace sirius {

Density::Density(Global& parameters__) : parameters_(parameters__), gaunt_coefs_(NULL)
{
    fft_ = parameters_.reciprocal_lattice()->fft();

    rho_ = new Periodic_function<double>(parameters_, parameters_.lmmax_rho(), parameters_.reciprocal_lattice()->num_gvec());

    /* core density of the pseudopotential method */
    if (parameters_.esm_type() == ultrasoft_pseudopotential)
    {
        rho_pseudo_core_ = new Periodic_function<double>(parameters_, 0);
        rho_pseudo_core_->allocate(false, true);
        rho_pseudo_core_->zero();

        generate_pseudo_core_charge_density();
    }

    for (int i = 0; i < parameters_.num_mag_dims(); i++)
    {
        magnetization_[i] = new Periodic_function<double>(parameters_, parameters_.lmmax_rho());
    }

    dmat_spins_.clear();
    dmat_spins_.push_back(std::pair<int, int>(0, 0));
    dmat_spins_.push_back(std::pair<int, int>(1, 1));
    dmat_spins_.push_back(std::pair<int, int>(0, 1));
    
    switch (parameters_.esm_type())
    {
        case full_potential_lapwlo:
        {
            gaunt_coefs_ = new Gaunt_coefficients<double_complex>(parameters_.lmax_apw(), parameters_.lmax_rho(), 
                                                                  parameters_.lmax_apw());
            break;
        }
        case full_potential_pwlo:
        {
            gaunt_coefs_ = new Gaunt_coefficients<double_complex>(parameters_.lmax_pw(), parameters_.lmax_rho(), 
                                                                  parameters_.lmax_pw());
            break;
        }
        case ultrasoft_pseudopotential:
        {
            break;
        }
    }

    l_by_lm_ = Utils::l_by_lm(parameters_.lmax_rho());
}

Density::~Density()
{
    delete rho_;
    for (int j = 0; j < parameters_.num_mag_dims(); j++) delete magnetization_[j];
    if (parameters_.esm_type() == ultrasoft_pseudopotential) delete rho_pseudo_core_;
    if (gaunt_coefs_) delete gaunt_coefs_;
}

void Density::set_charge_density_ptr(double* rhomt, double* rhoir)
{
    rho_->set_mt_ptr(rhomt);
    rho_->set_it_ptr(rhoir);
}

void Density::set_magnetization_ptr(double* magmt, double* magir)
{
    if (parameters_.num_mag_dims() == 0) return;
    assert(parameters_.num_spins() == 2);

    // set temporary array wrapper
    mdarray<double, 4> magmt_tmp(magmt, parameters_.lmmax_rho(), parameters_.unit_cell()->max_num_mt_points(), 
                                 parameters_.unit_cell()->num_atoms(), parameters_.num_mag_dims());
    mdarray<double, 2> magir_tmp(magir, fft_->size(), parameters_.num_mag_dims());
    
    if (parameters_.num_mag_dims() == 1)
    {
        // z component is the first and only one
        magnetization_[0]->set_mt_ptr(&magmt_tmp(0, 0, 0, 0));
        magnetization_[0]->set_it_ptr(&magir_tmp(0, 0));
    }

    if (parameters_.num_mag_dims() == 3)
    {
        // z component is the first
        magnetization_[0]->set_mt_ptr(&magmt_tmp(0, 0, 0, 2));
        magnetization_[0]->set_it_ptr(&magir_tmp(0, 2));
        // x component is the second
        magnetization_[1]->set_mt_ptr(&magmt_tmp(0, 0, 0, 0));
        magnetization_[1]->set_it_ptr(&magir_tmp(0, 0));
        // y component is the third
        magnetization_[2]->set_mt_ptr(&magmt_tmp(0, 0, 0, 1));
        magnetization_[2]->set_it_ptr(&magir_tmp(0, 1));
    }
}
    
void Density::zero()
{
    rho_->zero();
    for (int i = 0; i < parameters_.num_mag_dims(); i++) magnetization_[i]->zero();
}

mdarray<double, 2> Density::generate_rho_radial_integrals(int type__)
{
    Timer t("sirius::Density::generate_rho_radial_integrals");

    auto rl = parameters_.reciprocal_lattice();
    auto uc = parameters_.unit_cell();

    mdarray<double, 2> rho_radial_integrals(uc->num_atom_types(), rl->num_gvec_shells_inner());

    /* split G-shells between MPI ranks */
    splindex<block> spl_gshells(rl->num_gvec_shells_inner(), Platform::num_mpi_ranks(), Platform::mpi_rank());

    #pragma omp parallel
    {
        /* splines for all atom types */
        std::vector< Spline<double> > sa(uc->num_atom_types());
        
        for (int iat = 0; iat < uc->num_atom_types(); iat++) 
        {
            /* full potential radial integrals requre a free atom grid */
            if (type__ == 0) 
            {
                sa[iat] = Spline<double>(uc->atom_type(iat)->free_atom_radial_grid());
            }
            else
            {
                sa[iat] = Spline<double>(uc->atom_type(iat)->radial_grid());
            }
        }
        
        /* spherical Bessl functions */
        sbessel_pw<double> jl(uc, 0);

        #pragma omp for
        for (int igsloc = 0; igsloc < (int)spl_gshells.local_size(); igsloc++)
        {
            int igs = (int)spl_gshells[igsloc];

            /* for pseudopotential valence or core charge density */
            if (type__ == 1 || type__ == 2) jl.load(rl->gvec_shell_len(igs));

            for (int iat = 0; iat < uc->num_atom_types(); iat++)
            {
                auto atom_type = uc->atom_type(iat);

                if (type__ == 0)
                {
                    if (igs == 0)
                    {
                        for (int ir = 0; ir < sa[iat].num_points(); ir++) sa[iat][ir] = atom_type->free_atom_density(ir);
                        rho_radial_integrals(iat, igs) = sa[iat].interpolate().integrate(2);
                    }
                    else
                    {
                        double G = rl->gvec_shell_len(igs);
                        for (int ir = 0; ir < sa[iat].num_points(); ir++) 
                        {
                            sa[iat][ir] = atom_type->free_atom_density(ir) *
                                          sin(G * atom_type->free_atom_radial_grid(ir)) / G;
                        }
                        rho_radial_integrals(iat, igs) = sa[iat].interpolate().integrate(1);
                    }
                }

                if (type__ == 1)
                {
                    for (int ir = 0; ir < sa[iat].num_points(); ir++) 
                        sa[iat][ir] = jl(ir, 0, iat) * atom_type->uspp().total_charge_density[ir];
                    rho_radial_integrals(iat, igs) = sa[iat].interpolate().integrate(0) / fourpi;
                }

                if (type__ == 2)
                {
                    for (int ir = 0; ir < sa[iat].num_points(); ir++) 
                        sa[iat][ir] = jl(ir, 0, iat) * atom_type->uspp().core_charge_density[ir];
                    rho_radial_integrals(iat, igs) = sa[iat].interpolate().integrate(2);
                }
            }
        }
    }

    int ld = uc->num_atom_types();
    Platform::allgather(rho_radial_integrals.ptr(), static_cast<int>(ld * spl_gshells.global_offset()), 
                        static_cast<int>(ld * spl_gshells.local_size()));

    return rho_radial_integrals;
}

void Density::initial_density()
{
    Timer t("sirius::Density::initial_density");

    zero();
    
    auto rl = parameters_.reciprocal_lattice();
    auto uc = parameters_.unit_cell();

    if (uc->full_potential())
    {
        /* initialize smooth density of free atoms */
        for (int iat = 0; iat < uc->num_atom_types(); iat++) uc->atom_type(iat)->init_free_atom(true);

        /* compute radial integrals */
        //== mdarray<double, 2> rho_radial_integrals(uc->num_atom_types(), rl->num_gvec_shells_inner());
        //== rho_radial_integrals.zero();

        //== Timer t1("sirius::Density::initial_density:rad_int");
        //== splindex<block> spl_gshells(rl->num_gvec_shells_inner(), Platform::num_mpi_ranks(), Platform::mpi_rank());

        //== #pragma omp parallel for
        //== for (int igsloc = 0; igsloc < (int)spl_gshells.local_size(); igsloc++)
        //== {
        //==     int igs = (int)spl_gshells[igsloc];
        //==     for (int iat = 0; iat < uc->num_atom_types(); iat++)
        //==     {
        //==         auto atom_type = uc->atom_type(iat);
        //==         Spline<double> s(atom_type->free_atom_radial_grid());
        //==         
        //==         if (igs == 0)
        //==         {
        //==             for (int ir = 0; ir < s.num_points(); ir++) s[ir] = atom_type->free_atom_density(ir);
        //==             rho_radial_integrals(iat, igs) = s.interpolate().integrate(2);
        //==         }
        //==         else
        //==         {
        //==             double G = rl->gvec_shell_len(igs);
        //==             for (int ir = 0; ir < s.num_points(); ir++) 
        //==             {
        //==                 s[ir] = atom_type->free_atom_density(ir) *
        //==                         sin(G * atom_type->free_atom_radial_grid(ir)) / G;
        //==             }
        //==             rho_radial_integrals(iat, igs) = s.interpolate().integrate(1);
        //==         }
        //==     }
        //== }
        //== Platform::allreduce(rho_radial_integrals.ptr(), (int)rho_radial_integrals.size());
        //== t1.stop();

        auto rho_radial_integrals = generate_rho_radial_integrals(0);

        /* compute contribution from free atoms to the interstitial density */
        std::vector<double_complex> v = rl->make_periodic_function(rho_radial_integrals, rl->num_gvec());

        /* set plane-wave coefficients of the charge density */
        memcpy(&rho_->f_pw(0), &v[0], rl->num_gvec() * sizeof(double_complex));

        /* convert charge deisnty to real space mesh */
        fft_->input(rl->num_gvec(), rl->fft_index(), &rho_->f_pw(0));
        fft_->transform(1);
        fft_->output(&rho_->f_it<global>(0));

        /* remove possible negative noise */
        for (int ir = 0; ir < fft_->size(); ir++)
        {
            if (rho_->f_it<global>(ir) < 0) rho_->f_it<global>(ir) = 0;
        }
        
        /* mapping between G-shell and a list of G-vectors */
        std::map<int, std::vector<int> > gsh_map;

        for (int igloc = 0; igloc < (int)rl->spl_num_gvec().local_size(); igloc++)
        {
            /* global index of the G-vector */
            int ig = (int)rl->spl_num_gvec(igloc);
            /* index of the G-vector shell */
            int igsh = rl->gvec_shell(ig);
            if (gsh_map.count(igsh) == 0) gsh_map[igsh] = std::vector<int>();
            gsh_map[igsh].push_back(igloc);
        }

        /* list of G-shells for the curent MPI rank */
        std::vector<std::pair<int, std::vector<int> > > gsh_list;
        for (auto& i: gsh_map) gsh_list.push_back(std::pair<int, std::vector<int> >(i.first, i.second));

        Timer t2("sirius::Density::initial_density:rho_mt");
        int lmax = 2; //parameters_.lmax_rho();
        int lmmax = Utils::lmmax(lmax);
        auto l_by_lm = Utils::l_by_lm(lmax);

        SHT sht(lmax);
        
        /* cache 4 * pi * complex_i^l values */ 
        std::vector<double_complex> cil4p(lmax + 1);
        for (int l = 0; l <= lmax; l++) cil4p[l] = std::pow(complex_i, l) * fourpi;
        
        /* loop over atom types */
        for (int iat = 0; iat < uc->num_atom_types(); iat++)
        {
            /* number of muffin-tin points */
            int nmtp = uc->atom_type(iat)->num_mt_points();
            
            /* number of atoms for the current atom type */
            int num_atoms_t = uc->atom_type(iat)->num_atoms();

            /* charge density for all atoms of the current type */
            mdarray<double_complex, 3> rho_ylm(nmtp, lmmax, num_atoms_t);
            rho_ylm.zero();
            
            /* spherical Bessel functions */
            mdarray<double, 2> sbessel(nmtp, lmax + 1);
            
            /* loop over G-vector shells */
            for (int i = 0; i < (int)gsh_list.size(); i++)
            {
                int igsh = gsh_list[i].first;
                auto& gv = gsh_list[i].second;

                /* lengths of the G-vectors in the current shell */
                double G = rl->gvec_shell_len(igsh);
                    
                #pragma omp parallel
                {
                    /* get spherical Bessel function j_l(Gr) */
                    std::vector<double> jl(lmax + 1);
                    #pragma omp for
                    for (int ir = 0; ir < nmtp; ir++)
                    {
                        double x = uc->atom_type(iat)->radial_grid(ir) * G;
                        gsl_sf_bessel_jl_array(lmax, x, &jl[0]);
                        for (int l = 0; l <= lmax; l++) sbessel(ir, l) = jl[l];
                    }
                    
                    /* r-independent coefficients */
                    mdarray<double_complex, 1> c(lmmax);

                    /* loop over all atoms of the current type */
                    #pragma omp for
                    for (int j = 0; j < num_atoms_t; j++)
                    {
                        int ia = uc->atom_type(iat)->atom_id(j);
                        
                        /* add contribution from the plane-waves of the given G-shell */
                        c.zero();
                        for (int igloc: gv)
                        {
                            int ig = rl->spl_num_gvec(igloc);

                            for (int l = 0; l <= lmax; l++)
                            {
                                double_complex z1 = rho_->f_pw(ig) * rl->gvec_phase_factor<local>(igloc, ia) * cil4p[l];

                                for (int lm = Utils::lm_by_l_m(l, -l); lm <= Utils::lm_by_l_m(l, l); lm++) 
                                    c(lm) += z1 * conj(rl->gvec_ylm(lm, igloc));
                            }
                        }

                        /* update muffin-tin functions */
                        for (int lm = 0; lm < lmmax; lm++)
                        {
                            int l = l_by_lm[lm];
                            for (int ir = 0; ir < nmtp; ir++) rho_ylm(ir, lm, j) += c(lm) * sbessel(ir, l);
                        }
                    }
                }
            }

            Spheric_function<spectral, double_complex> rho_ylm_tmp(lmmax, uc->atom_type(iat)->radial_grid());
            for (int j = 0; j < num_atoms_t; j++)
            {
                int ia = uc->atom_type(iat)->atom_id(j);

                /* get the total sum over all plane-waves */
                Platform::allreduce(&rho_ylm(0, 0, j), lmmax * nmtp);

                /* remove smooth density of the free atom (we need contribution only from tails) */
                for (int ir = 0; ir < nmtp; ir++)
                {
                    double x = uc->atom_type(iat)->radial_grid(ir);
                    rho_ylm(ir, 0, j) -= uc->atom_type(iat)->free_atom_density(x) / y00;
                }
            
                /* transpose indices */
                for (int lm = 0; lm < lmmax; lm++)
                {
                    for (int ir = 0; ir < nmtp; ir++) rho_ylm_tmp(lm, ir) = rho_ylm(ir, lm, j);
                }
            
                /* get local offset and rank corresponding to the global index of atom */
                auto p = uc->spl_num_atoms().location(ia);
                
                if (p.second == Platform::mpi_rank())
                {
                    /* convert from Ylm to Rlm expansion */
                    sht.convert(rho_ylm_tmp, rho_->f_mt((int)p.first));
                }
            }
        }
        t2.stop();

        /* initialize density of free atoms (not smoothed) */
        for (int iat = 0; iat < uc->num_atom_types(); iat++) uc->atom_type(iat)->init_free_atom(false);

        for (int ia = 0; ia < uc->num_atoms(); ia++)
        {
            auto p = uc->spl_num_atoms().location(ia);
            
            if (p.second == Platform::mpi_rank())
            {
                /* add density of the free atom */
                for (int ir = 0; ir < uc->atom(ia)->num_mt_points(); ir++)
                {
                    double x = uc->atom(ia)->type()->radial_grid(ir);
                    rho_->f_mt<local>(0, ir, (int)p.first) += uc->atom(ia)->type()->free_atom_density(x) / y00;
                }
            }
        }

        /* initialize the magnetization */
        if (parameters_.num_mag_dims())
        {
            for (int ialoc = 0; ialoc < (int)uc->spl_num_atoms().local_size(); ialoc++)
            {
                int ia = (int)uc->spl_num_atoms(ialoc);
                vector3d<double> v = uc->atom(ia)->vector_field();
                double len = v.length();

                int nmtp = uc->atom(ia)->type()->num_mt_points();
                Spline<double> rho(uc->atom(ia)->type()->radial_grid());
                double R = uc->atom(ia)->type()->mt_radius();
                for (int ir = 0; ir < nmtp; ir++)
                {
                    double x = uc->atom(ia)->type()->radial_grid(ir);
                    rho[ir] = rho_->f_mt<local>(0, ir, ialoc) * y00 * (1 - 3 * std::pow(x / R, 2) + 2 * std::pow(x / R, 3));
                }

                /* maximum magnetization which can be achieved if we smooth density towards MT boundary */
                double q = fourpi * rho.interpolate().integrate(2);
                
                /* if very strong initial magnetization is given */
                if (q < len)
                {
                    /* renormalize starting magnetization */
                    for (int x = 0; x < 3; x++) v[x] *= (q / len);

                    len = q;
                }

                if (len > 1e-8)
                {
                    for (int ir = 0; ir < nmtp; ir++)
                        magnetization_[0]->f_mt<local>(0, ir, ialoc) = rho[ir] * v[2] / q / y00;

                    if (parameters_.num_mag_dims() == 3)
                    {
                        for (int ir = 0; ir < nmtp; ir++)
                        {
                            magnetization_[1]->f_mt<local>(0, ir, ialoc) = rho[ir] * v[0] / q / y00;
                            magnetization_[2]->f_mt<local>(0, ir, ialoc) = rho[ir] * v[1] / q / y00;
                        }
                    }
                }
            }
        }
    }

    if (parameters_.esm_type() == ultrasoft_pseudopotential)
    {
        //== mdarray<double, 2> rho_radial_integrals(uc->num_atom_types(), rl->num_gvec_shells_inner());
        //== 
        //== splindex<block> spl_gsh(rl->num_gvec_shells_inner(), Platform::num_mpi_ranks(), Platform::mpi_rank());

        //== // TODO: unify with  generate_pseudo_core_charge_density() 
        //== #pragma omp parallel
        //== {
        //==     /* splines for all atom types */
        //==     std::vector< Spline<double> > sa(uc->num_atom_types());
        //==     for (int iat = 0; iat < uc->num_atom_types(); iat++) sa[iat] = Spline<double>(uc->atom_type(iat)->radial_grid());

        //==     /* spherical Bessl functions */
        //==     sbessel_pw<double> jl(uc, 0);

        //==     #pragma omp for
        //==     for (int igsloc = 0; igsloc < (int)spl_gsh.local_size(); igsloc++)
        //==     {
        //==         int igs = (int)spl_gsh[igsloc];

        //==         jl.load(rl->gvec_shell_len(igs));

        //==         for (int iat = 0; iat < uc->num_atom_types(); iat++)
        //==         {
        //==             auto atom_type = uc->atom_type(iat);
        //==             for (int ir = 0; ir < atom_type->num_mt_points(); ir++) 
        //==                 sa[iat][ir] = jl(ir, 0, iat) * atom_type->uspp().total_charge_density[ir];
        //==             rho_radial_integrals(iat, igs) = sa[iat].interpolate().integrate(0) / fourpi;
        //==         }
        //==     }
        //== }
        //== int ld = uc->num_atom_types();
        //== Platform::allgather(rho_radial_integrals.ptr(), static_cast<int>(ld * spl_gsh.global_offset()), 
        //==                     static_cast<int>(ld * spl_gsh.local_size()));
        
        auto rho_radial_integrals = generate_rho_radial_integrals(1);

        std::vector<double_complex> v = rl->make_periodic_function(rho_radial_integrals, rl->num_gvec());

        memcpy(&rho_->f_pw(0), &v[0], rl->num_gvec() * sizeof(double_complex));

        if (fabs(rho_->f_pw(0) * uc->omega() - uc->num_valence_electrons()) > 1e-6)
        {
            std::stringstream s;
            s << "wrong initial charge density" << std::endl
              << "  integral of the density : " << real(rho_->f_pw(0) * uc->omega()) << std::endl
              << "  target number of electrons : " << uc->num_valence_electrons();
            warning_global(__FILE__, __LINE__, s);
        }

        fft_->input(rl->num_gvec(), rl->fft_index(), &rho_->f_pw(0));
        fft_->transform(1);
        fft_->output(&rho_->f_it<global>(0));
        
        // remove possible negative noise
        for (int ir = 0; ir < fft_->size(); ir++)
        {
            if (rho_->f_it<global>(ir) < 0) rho_->f_it<global>(ir) = 0;
        }
        
        fft_->input(&rho_->f_it<global>(0));
        fft_->transform(-1);
        fft_->output(rl->num_gvec(), rl->fft_index(), &rho_->f_pw(0));
    }

    rho_->sync(true, true);
    for (int i = 0; i < parameters_.num_mag_dims(); i++) magnetization_[i]->sync(true, true);

    if (uc->full_potential())
    {
        /* check initial charge */
        std::vector<double> nel_mt;
        double nel_it;
        double nel = rho_->integrate(nel_mt, nel_it);
        if (uc->num_electrons() > 1e-8 && std::abs(nel - uc->num_electrons()) / uc->num_electrons()  > 1e-3)
        {
            std::stringstream s;
            s << "wrong initial charge density" << std::endl
              << "  integral of the density : " << nel << std::endl
              << "  target number of electrons : " << uc->num_electrons();
            warning_global(__FILE__, __LINE__, s);
        }
    }
}

void Density::add_kpoint_contribution_mt(K_point* kp, std::vector< std::pair<int, double> >& occupied_bands, 
                                         mdarray<double_complex, 4>& mt_complex_density_matrix)
{
    Timer t("sirius::Density::add_kpoint_contribution_mt");
    
    if (occupied_bands.size() == 0) return;
   
    mdarray<double_complex, 3> wf1(parameters_.unit_cell()->max_mt_basis_size(), (int)occupied_bands.size(), parameters_.num_spins());
    mdarray<double_complex, 3> wf2(parameters_.unit_cell()->max_mt_basis_size(), (int)occupied_bands.size(), parameters_.num_spins());

    for (int ia = 0; ia < parameters_.unit_cell()->num_atoms(); ia++)
    {
        int offset_wf = parameters_.unit_cell()->atom(ia)->offset_wf();
        int mt_basis_size = parameters_.unit_cell()->atom(ia)->type()->mt_basis_size();
        
        for (int i = 0; i < (int)occupied_bands.size(); i++)
        {
            for (int ispn = 0; ispn < parameters_.num_spins(); ispn++)
            {
                for (int j = 0; j < mt_basis_size; j++)
                {
                    wf1(j, i, ispn) = conj(kp->spinor_wave_function(offset_wf + j, occupied_bands[i].first, ispn));
                    wf2(j, i, ispn) = kp->spinor_wave_function(offset_wf + j, occupied_bands[i].first, ispn) * occupied_bands[i].second;
                }
            }
        }

        for (int j = 0; j < (int)mt_complex_density_matrix.size(2); j++)
        {
            blas<cpu>::gemm(0, 1, mt_basis_size, mt_basis_size, (int)occupied_bands.size(), complex_one, 
                            &wf1(0, 0, dmat_spins_[j].first), wf1.ld(), 
                            &wf2(0, 0, dmat_spins_[j].second), wf2.ld(), complex_one, 
                            &mt_complex_density_matrix(0, 0, j, ia), mt_complex_density_matrix.ld());
        }
    }
}

template <int num_mag_dims> 
void Density::reduce_zdens(Atom_type* atom_type, int ialoc, mdarray<double_complex, 4>& zdens, mdarray<double, 3>& mt_density_matrix)
{
    mt_density_matrix.zero();
    
    #pragma omp parallel for default(shared)
    for (int idxrf2 = 0; idxrf2 < atom_type->mt_radial_basis_size(); idxrf2++)
    {
        int l2 = atom_type->indexr(idxrf2).l;
        for (int idxrf1 = 0; idxrf1 <= idxrf2; idxrf1++)
        {
            int offs = idxrf2 * (idxrf2 + 1) / 2 + idxrf1;
            int l1 = atom_type->indexr(idxrf1).l;

            int xi2 = atom_type->indexb().index_by_idxrf(idxrf2);
            for (int lm2 = Utils::lm_by_l_m(l2, -l2); lm2 <= Utils::lm_by_l_m(l2, l2); lm2++, xi2++)
            {
                int xi1 = atom_type->indexb().index_by_idxrf(idxrf1);
                for (int lm1 = Utils::lm_by_l_m(l1, -l1); lm1 <= Utils::lm_by_l_m(l1, l1); lm1++, xi1++)
                {
                    for (int k = 0; k < gaunt_coefs_->num_gaunt(lm1, lm2); k++)
                    {
                        int lm3 = gaunt_coefs_->gaunt(lm1, lm2, k).lm3;
                        double_complex gc = gaunt_coefs_->gaunt(lm1, lm2, k).coef;
                        switch (num_mag_dims)
                        {
                            case 3:
                            {
                                mt_density_matrix(lm3, offs, 2) += 2.0 * real(zdens(xi1, xi2, 2, ialoc) * gc); 
                                mt_density_matrix(lm3, offs, 3) -= 2.0 * imag(zdens(xi1, xi2, 2, ialoc) * gc);
                            }
                            case 1:
                            {
                                mt_density_matrix(lm3, offs, 1) += real(zdens(xi1, xi2, 1, ialoc) * gc);
                            }
                            case 0:
                            {
                                mt_density_matrix(lm3, offs, 0) += real(zdens(xi1, xi2, 0, ialoc) * gc);
                            }
                        }
                    }
                }
            }
        }
    }
}

std::vector< std::pair<int, double> > Density::get_occupied_bands_list(Band* band, K_point* kp)
{
    std::vector< std::pair<int, double> > bands;
    for (int jsub = 0; jsub < parameters_.num_sub_bands(); jsub++)
    {
        int j = parameters_.idxbandglob(jsub);
        double wo = kp->band_occupancy(j) * kp->weight();
        if (wo > 1e-14) bands.push_back(std::pair<int, double>(jsub, wo));
    }
    return bands;
}

//== // memory-conservative implementation
//== void Density::add_kpoint_contribution_pp(K_point* kp, std::vector< std::pair<int, double> >& occupied_bands, 
//==                                          mdarray<double_complex, 4>& pp_complex_density_matrix)
//== {
//==     Timer t("sirius::Density::add_kpoint_contribution_pp");
//== 
//==     if (occupied_bands.size() == 0) return;
//== 
//==     // take only occupied wave-functions
//==     mdarray<double_complex, 2> wfs(kp->num_gkvec(), (int)occupied_bands.size());
//==     for (int i = 0; i < (int)occupied_bands.size(); i++)
//==     {
//==         memcpy(&wfs(0, i), &kp->spinor_wave_function(0, 0, occupied_bands[i].first), kp->num_gkvec() * sizeof(double_complex));
//==     }
//== 
//==     mdarray<double_complex, 2> beta_pw(kp->num_gkvec(), parameters_.unit_cell()->max_mt_basis_size());
//== 
//==     mdarray<double_complex, 2> beta_psi(parameters_.unit_cell()->max_mt_basis_size(), (int)occupied_bands.size());
//== 
//==     // auxiliary arrays
//==     mdarray<double_complex, 2> bp1(parameters_.unit_cell()->max_mt_basis_size(), (int)occupied_bands.size());
//==     mdarray<double_complex, 2> bp2(parameters_.unit_cell()->max_mt_basis_size(), (int)occupied_bands.size());
//== 
//==     for (int ia = 0; ia < parameters_.unit_cell()->num_atoms(); ia++)
//==     {   
//==         // number of beta functions for a given atom
//==         int nbf = parameters_.unit_cell()->atom(ia)->type()->mt_basis_size();
//== 
//==         kp->generate_beta_pw(&beta_pw(0, 0), ia);
//==         
//==         // compute <beta|Psi>
//==         blas<cpu>::gemm(2, 0, nbf, (int)occupied_bands.size(), kp->num_gkvec(), &beta_pw(0, 0), beta_pw.ld(), 
//==                         &wfs(0, 0), wfs.ld(), &beta_psi(0, 0), beta_psi.ld());
//==         
//==         for (int i = 0; i < (int)occupied_bands.size(); i++)
//==         {
//==             for (int xi = 0; xi < nbf; xi++)
//==             {
//==                 bp1(xi, i) = beta_psi(xi, i);
//==                 bp2(xi, i) = conj(beta_psi(xi, i)) * occupied_bands[i].second;
//==             }
//==         }
//== 
//==         blas<cpu>::gemm(0, 1, nbf, nbf, (int)occupied_bands.size(), double_complex(1, 0), &bp1(0, 0), bp1.ld(),
//==                         &bp2(0, 0), bp2.ld(), double_complex(1, 0), &pp_complex_density_matrix(0, 0, 0, ia), pp_complex_density_matrix.ld());
//==     }
//== }

// memory-greedy implementation
void Density::add_kpoint_contribution_pp(K_point* kp, std::vector< std::pair<int, double> >& occupied_bands, 
                                         mdarray<double_complex, 4>& pp_complex_density_matrix)
{
    Timer t("sirius::Density::add_kpoint_contribution_pp");

    if (occupied_bands.size() == 0) return;

    // take only occupied wave-functions
    mdarray<double_complex, 2> wfs(kp->num_gkvec(), occupied_bands.size());
    for (int i = 0; i < (int)occupied_bands.size(); i++)
    {
        memcpy(&wfs(0, i), &kp->spinor_wave_function(0, occupied_bands[i].first, 0), kp->num_gkvec() * sizeof(double_complex));
    }

    // <\beta_{\xi}^{\alpha}|\Psi_j>
    mdarray<double_complex, 2> beta_psi(parameters_.unit_cell()->mt_lo_basis_size(), occupied_bands.size());

    // compute <beta|Psi>
    blas<cpu>::gemm(2, 0, parameters_.unit_cell()->mt_lo_basis_size(), (int)occupied_bands.size(), kp->num_gkvec(), 
                    &kp->beta_pw(0, 0), kp->num_gkvec(), &wfs(0, 0), wfs.ld(), &beta_psi(0, 0), beta_psi.ld());
    
    #pragma omp parallel
    {
        // auxiliary arrays
        mdarray<double_complex, 2> bp1(parameters_.unit_cell()->max_mt_basis_size(), (int)occupied_bands.size());
        mdarray<double_complex, 2> bp2(parameters_.unit_cell()->max_mt_basis_size(), (int)occupied_bands.size());
        #pragma omp for
        for (int ia = 0; ia < parameters_.unit_cell()->num_atoms(); ia++)
        {   
            // number of beta functions for a given atom
            int nbf = parameters_.unit_cell()->atom(ia)->type()->mt_basis_size();

            for (int i = 0; i < (int)occupied_bands.size(); i++)
            {
                for (int xi = 0; xi < nbf; xi++)
                {
                    bp1(xi, i) = beta_psi(parameters_.unit_cell()->atom(ia)->offset_lo() + xi, i);
                    bp2(xi, i) = conj(bp1(xi, i)) * occupied_bands[i].second;
                }
            }

            blas<cpu>::gemm(0, 1, nbf, nbf, (int)occupied_bands.size(), complex_one, &bp1(0, 0), bp1.ld(),
                            &bp2(0, 0), bp2.ld(), complex_one, &pp_complex_density_matrix(0, 0, 0, ia), 
                            pp_complex_density_matrix.ld());
        }
    }
}

#ifdef _GPU_
extern "C" void copy_beta_psi_gpu(int num_beta_atot, 
                                  int num_bands, 
                                  int ld,
                                  int num_beta,
                                  int offset,
                                  void* beta_psi,
                                  double* wo,
                                  void* bp1,
                                  void* bp2,
                                  int stream_id);

void Density::add_kpoint_contribution_pp_gpu(K_point* kp, std::vector< std::pair<int, double> >& occupied_bands, 
                                             mdarray<double_complex, 4>& pp_complex_density_matrix)
{
    Timer t("sirius::Density::add_kpoint_contribution_pp_gpu");

    if (occupied_bands.size() == 0) return;

    // take only occupied wave-functions
    mdarray<double_complex, 2> wfs(kp->num_gkvec(), (int)occupied_bands.size());
    for (int i = 0; i < (int)occupied_bands.size(); i++)
    {
        memcpy(&wfs(0, i), &kp->spinor_wave_function(0, occupied_bands[i].first, 0), kp->num_gkvec() * sizeof(double_complex));
    }
    wfs.allocate_on_device();
    wfs.copy_to_device();

    // <G+k|\beta_{\xi}^{\alpha}>
    mdarray<double_complex, 2> beta_pw(NULL, kp->num_gkvec(), parameters_.unit_cell()->mt_lo_basis_size());
    beta_pw.allocate_on_device();
    
    kp->beta_pw_t().allocate_on_device();
    kp->beta_pw_t().copy_to_device();

    kp->gkvec().allocate_on_device(); 
    kp->gkvec().copy_to_device();

    parameters_.unit_cell()->atom_pos().allocate_on_device(); 
    parameters_.unit_cell()->atom_pos().copy_to_device();

    parameters_.unit_cell()->beta_t_idx().allocate_on_device(); 
    parameters_.unit_cell()->beta_t_idx().copy_to_device();

    // create <G+k|beta>
    create_beta_pw_gpu(kp->num_gkvec(), 
                       parameters_.unit_cell()->mt_lo_basis_size(), 
                       parameters_.unit_cell()->beta_t_idx().ptr_device(),
                       kp->beta_pw_t().ptr_device(),
                       kp->gkvec().ptr_device(),
                       parameters_.unit_cell()->atom_pos().ptr_device(),
                       beta_pw.ptr_device());

    parameters_.unit_cell()->beta_t_idx().deallocate_on_device();
    parameters_.unit_cell()->atom_pos().deallocate_on_device();
    kp->gkvec().deallocate_on_device();
    kp->beta_pw_t().deallocate_on_device();

    // <\beta_{\xi}^{\alpha}|\Psi_j>
    mdarray<double_complex, 2> beta_psi(NULL, parameters_.unit_cell()->mt_lo_basis_size(), (int)occupied_bands.size());
    beta_psi.allocate_on_device();

    // compute <beta|Psi>
    blas<gpu>::gemm(2, 0, parameters_.unit_cell()->mt_lo_basis_size(), (int)occupied_bands.size(), kp->num_gkvec(), 
                    beta_pw.ptr_device(), beta_pw.ld(), wfs.ptr_device(), wfs.ld(), 
                    beta_psi.ptr_device(), beta_psi.ld());
    
    wfs.deallocate_on_device();
    beta_pw.deallocate_on_device();

    mdarray<double, 1> wo((int)occupied_bands.size());
    for (int i = 0; i < (int)occupied_bands.size(); i++) wo(i) = occupied_bands[i].second;
    wo.allocate_on_device();
    wo.copy_to_device();
    
    double_complex zone(1, 0);
    #pragma omp parallel
    {
        int thread_id = Platform::thread_id();
        // auxiliary arrays
        mdarray<double_complex, 2> bp1(parameters_.unit_cell()->max_mt_basis_size(), (int)occupied_bands.size());
        mdarray<double_complex, 2> bp2(parameters_.unit_cell()->max_mt_basis_size(), (int)occupied_bands.size());
        bp1.allocate_on_device();
        bp2.allocate_on_device();
        #pragma omp for
        for (int ia = 0; ia < parameters_.unit_cell()->num_atoms(); ia++)
        {   
            // number of beta functions for a given atom
            int nbf = parameters_.unit_cell()->atom(ia)->type()->mt_basis_size();
            int ofs = parameters_.unit_cell()->atom(ia)->offset_lo();

            copy_beta_psi_gpu(parameters_.unit_cell()->mt_lo_basis_size(), 
                              (int)occupied_bands.size(), 
                              parameters_.unit_cell()->max_mt_basis_size(),
                              nbf,
                              ofs,
                              beta_psi.ptr_device(),
                              wo.ptr_device(),
                              bp1.ptr_device(),
                              bp2.ptr_device(),
                              thread_id);
            
            #pragma omp critical
            {
                cublas_set_stream(thread_id);

                blas<gpu>::gemm(0, 1, nbf, nbf, (int)occupied_bands.size(), &zone, bp1.ptr_device(), bp1.ld(),
                                bp2.ptr_device(), bp2.ld(), &zone, pp_complex_density_matrix.ptr_device(0, 0, 0, ia), 
                                pp_complex_density_matrix.ld());
            }

            cuda_stream_synchronize(thread_id);
        }
    }
    cuda_device_synchronize();
    cublas_set_stream(-1);
    wo.deallocate_on_device();
    beta_psi.deallocate_on_device();
}
#endif

void add_kpoint_contribution_cpu(int thread_id, K_point* kp, Global& parameters, 
                                 std::vector< std::pair<int, double> >& occupied_bands, 
                                 mdarray<double, 3>& it_density_matrix,
                                 int* idx_band, std::mutex* idx_band_mutex)
{
    bool done = false;

    auto fft = parameters.reciprocal_lattice()->fft();

    int wf_pw_offset = kp->wf_pw_offset();
    
    mdarray<double_complex, 2> psi_it(fft->size(), parameters.num_spins());

    while (!done)
    {
        // increment the band index
        idx_band_mutex->lock();
        int i = *idx_band;
        if ((*idx_band) + 1 > (int)occupied_bands.size()) 
        {
            done = true;
        }
        else
        {
            (*idx_band)++;
        }
        idx_band_mutex->unlock();

        if (!done)
        {
            for (int ispn = 0; ispn < parameters.num_spins(); ispn++)
            {
                fft->input(kp->num_gkvec(), kp->fft_index(), 
                           &kp->spinor_wave_function(wf_pw_offset, occupied_bands[i].first, ispn), thread_id);
                fft->transform(1, thread_id);
                fft->output(&psi_it(0, ispn), thread_id);
            }
            double w = occupied_bands[i].second / parameters.unit_cell()->omega();
           
            switch (parameters.num_mag_dims())
            {
                case 3:
                {
                    for (int ir = 0; ir < fft->size(); ir++)
                    {
                        double_complex z = psi_it(ir, 0) * conj(psi_it(ir, 1)) * w;
                        it_density_matrix(ir, 2, thread_id) += 2.0 * real(z);
                        it_density_matrix(ir, 3, thread_id) -= 2.0 * imag(z);
                    }
                }
                case 1:
                {
                    for (int ir = 0; ir < fft->size(); ir++)
                        it_density_matrix(ir, 1, thread_id) += real(psi_it(ir, 1) * conj(psi_it(ir, 1))) * w;
                }
                case 0:
                {
                    for (int ir = 0; ir < fft->size(); ir++)
                        it_density_matrix(ir, 0, thread_id) += real(psi_it(ir, 0) * conj(psi_it(ir, 0))) * w;
                }
            }
        }
    }

}

#ifdef _GPU_
extern "C" void update_it_density_matrix_gpu(int fft_size, 
                                             int nfft_max, 
                                             int num_spins, 
                                             int num_mag_dims, 
                                             void* psi_it, 
                                             double* wt, 
                                             void* it_density_matrix);

void add_kpoint_contribution_gpu(int thread_id, K_point* kp, Global& parameters, 
                                 std::vector< std::pair<int, double> >& occupied_bands, 
                                 mdarray<double, 2>& it_density_matrix_gpu,
                                 int* idx_band, std::mutex* idx_band_mutex)
{
    FFT3D<gpu> fft(parameters.reciprocal_lattice()->fft()->grid_size());

    int wf_pw_offset = kp->wf_pw_offset();
    
    // move fft index to GPU
    mdarray<int, 1> fft_index(kp->fft_index(), kp->num_gkvec());
    fft_index.allocate_on_device();
    fft_index.copy_to_device();

    // maximum available memory of the device
    size_t max_free_mem = cuda_get_free_mem();
    
    // size of a single FFT: space for plane-wave coefficients + space for components of spinor wave-function
    size_t single_fft_size = (fft.size() * parameters.num_spins() + kp->num_gkvec()) * sizeof(double_complex);
    
    /* find maximum number of FFTs that can fit into device;
     * take into account nfft_max double weights
     */
    int nfft_max = 0;
    while (fft.num_fft_max(max_free_mem - nfft_max * single_fft_size - nfft_max * sizeof(double)) > nfft_max) nfft_max++;
    
    /* sum at maximum half of the bands on the GPU;
     * the second half is done by CPU
     */
    nfft_max = std::min(nfft_max, (int)occupied_bands.size() / 2);
 
    if (nfft_max <= 0)
    {
        fft_index.deallocate_on_device();
        return;
    }

    #ifdef _WRITE_GPU_INFO_
    INFO << "max_free_mem (Mb) = " << (max_free_mem >> 20) << std::endl;
    INFO << "nfft_max = " << nfft_max << std::endl;
    INFO << "work_size (Mb) = " << (fft.work_area_size(nfft_max) >> 20) << std::endl;
    INFO << "size of wf arrays (Mb) = " << ((nfft_max * single_fft_size) >> 20) << std::endl;
    #endif
    
    // allocate work area array
    mdarray<char, 1> work_area(NULL, fft.work_area_size(nfft_max));
    work_area.allocate_on_device();
    
    // allocate space for plane-wave expansion coefficients
    mdarray<double_complex, 2> psi_pw_gpu(NULL, kp->num_gkvec(), nfft_max); 
    psi_pw_gpu.allocate_on_device();
    
    // allocate space for spinor components
    mdarray<double_complex, 3> psi_it_gpu(NULL, fft.size(), nfft_max, parameters.num_spins());
    psi_it_gpu.allocate_on_device();
    
    // allocate space for weights
    mdarray<double, 1> w(nfft_max);
    w.allocate_on_device();

    // initialize cuFFT transform
    fft.initialize(nfft_max, work_area.ptr_device());

    bool done = false;

    while (!done)
    {
        idx_band_mutex->lock();
        int i = *idx_band;
        if ((*idx_band) + nfft_max > (int)occupied_bands.size()) 
        {
            done = true;
        }
        else
        {
            (*idx_band) += nfft_max;
        }
        idx_band_mutex->unlock();

        if (!done)
        {
            for (int ispn = 0; ispn < parameters.num_spins(); ispn++)
            {
                for (int j = 0; j < nfft_max; j++)
                {
                    w(j) = occupied_bands[i + j].second / parameters.unit_cell()->omega();

                    cublas_set_vector(kp->num_gkvec(), sizeof(double_complex), 
                                      &kp->spinor_wave_function(wf_pw_offset, occupied_bands[i + j].first, ispn), 1, 
                                      psi_pw_gpu.ptr_device(0, j), 1);
                }
                w.copy_to_device();

                fft.batch_load(kp->num_gkvec(), fft_index.ptr_device(), psi_pw_gpu.ptr_device(), 
                               psi_it_gpu.ptr_device(0, 0, ispn));

                #ifdef _WRITE_GPU_INFO_
                INFO << "about to execute cuFFT" << std::endl;
                #endif
                fft.transform(1, psi_it_gpu.ptr_device(0, 0, ispn));
            }

            update_it_density_matrix_gpu(fft.size(), nfft_max, parameters.num_spins(), parameters.num_mag_dims(), 
                                         psi_it_gpu.ptr_device(), w.ptr_device(), it_density_matrix_gpu.ptr_device());
        }
    }

    fft.finalize();
}
#endif

void Density::add_kpoint_contribution_it(K_point* kp, std::vector< std::pair<int, double> >& occupied_bands)
{
    Timer t("sirius::Density::add_kpoint_contribution_it");
    
    if (occupied_bands.size() == 0) return;
    
    // index of the occupied bands
    int idx_band = 0;
    std::mutex idx_band_mutex;

    int num_fft_threads = -1;
    switch (parameters_.processing_unit())
    {
        case cpu:
        {
            num_fft_threads = Platform::num_fft_threads();
            break;
        }
        case gpu:
        {
            num_fft_threads = std::min(Platform::num_fft_threads() + 1, Platform::max_num_threads());
            break;
        }
    }

    mdarray<double, 3> it_density_matrix(fft_->size(), parameters_.num_mag_dims() + 1, num_fft_threads);
    it_density_matrix.zero();
    
    #ifdef _GPU_
    mdarray<double, 2> it_density_matrix_gpu(NULL, fft_->size(), parameters_.num_mag_dims() + 1);
    // last thread is doing cuFFT 
    if (parameters_.processing_unit() == gpu && num_fft_threads > 1)
    {
        it_density_matrix_gpu.set_ptr(&it_density_matrix(0, 0, num_fft_threads - 1));
        it_density_matrix_gpu.allocate_on_device();
        it_density_matrix_gpu.zero_on_device();
    }
    #endif

    std::vector<std::thread> fft_threads;

    for (int i = 0; i < num_fft_threads; i++)
    {
        if (i == (num_fft_threads - 1) && num_fft_threads > 1 && parameters_.processing_unit() == gpu)
        {
            #ifdef _GPU_
            fft_threads.push_back(std::thread(add_kpoint_contribution_gpu, i, kp, std::ref(parameters_), 
                                              std::ref(occupied_bands), std::ref(it_density_matrix_gpu), 
                                              &idx_band, &idx_band_mutex));
            #else
            TERMINATE_NO_GPU
            #endif
        }
        else
        {
            fft_threads.push_back(std::thread(add_kpoint_contribution_cpu, i, kp, std::ref(parameters_), 
                                              std::ref(occupied_bands), std::ref(it_density_matrix), 
                                              &idx_band, &idx_band_mutex));
        }
    }

    for (auto& thread: fft_threads) thread.join();

    if (idx_band != (int)occupied_bands.size()) 
    {
        std::stringstream s;
        s << "not all FFTs are executed" << std::endl
          << " number of wave-functions : " << occupied_bands.size() << ", number of executed FFTs : " << idx_band;
        error_local(__FILE__, __LINE__, s);
    }

    #ifdef _GPU_
    if (parameters_.processing_unit() == gpu && num_fft_threads > 1)
    {
        it_density_matrix_gpu.copy_to_host();
        it_density_matrix_gpu.deallocate_on_device();
    }
    #endif

    // switch from real density matrix to density and magnetization
    switch (parameters_.num_mag_dims())
    {
        case 3:
        {
            for (int i = 0; i < num_fft_threads; i++)
            {
                for (int ir = 0; ir < fft_->size(); ir++)
                {
                    magnetization_[1]->f_it<global>(ir) += it_density_matrix(ir, 2, i);
                    magnetization_[2]->f_it<global>(ir) += it_density_matrix(ir, 3, i);
                }
            }
        }
        case 1:
        {
            for (int i = 0; i < num_fft_threads; i++)
            {
                for (int ir = 0; ir < fft_->size(); ir++)
                {
                    rho_->f_it<global>(ir) += (it_density_matrix(ir, 0, i) + it_density_matrix(ir, 1, i));
                    magnetization_[0]->f_it<global>(ir) += (it_density_matrix(ir, 0, i) - it_density_matrix(ir, 1, i));
                }
            }
            break;
        }
        case 0:
        {
            for (int i = 0; i < num_fft_threads; i++)
            {
                for (int ir = 0; ir < fft_->size(); ir++) rho_->f_it<global>(ir) += it_density_matrix(ir, 0, i);
            }
        }
    }
}

void Density::add_q_contribution_to_valence_density(K_set& ks)
{
    Timer t("sirius::Density::add_q_contribution_to_valence_density");

    //========================================================================================
    // if we have ud and du spin blocks, don't compute one of them (du in this implementation)
    // because density matrix is symmetric
    //========================================================================================
    int num_zdmat = (parameters_.num_mag_dims() == 3) ? 3 : (parameters_.num_mag_dims() + 1);

    // complex density matrix
    mdarray<double_complex, 4> pp_complex_density_matrix(parameters_.unit_cell()->max_mt_basis_size(), 
                                                         parameters_.unit_cell()->max_mt_basis_size(),
                                                         num_zdmat, parameters_.unit_cell()->num_atoms());
    pp_complex_density_matrix.zero();
    
    //=========================
    // add k-point contribution
    //=========================
    for (int ikloc = 0; ikloc < (int)ks.spl_num_kpoints().local_size(); ikloc++)
    {
        int ik = (int)ks.spl_num_kpoints(ikloc);
        auto occupied_bands = get_occupied_bands_list(ks.band(), ks[ik]);

        add_kpoint_contribution_pp(ks[ik], occupied_bands, pp_complex_density_matrix);
    }
    Platform::allreduce(pp_complex_density_matrix.ptr(), (int)pp_complex_density_matrix.size());

    auto rl = parameters_.reciprocal_lattice();

    std::vector<double_complex> f_pw(rl->num_gvec(), double_complex(0, 0));

    int max_num_atoms = 0;
    for (int iat = 0; iat < parameters_.unit_cell()->num_atom_types(); iat++)
        max_num_atoms = std::max(max_num_atoms, parameters_.unit_cell()->atom_type(iat)->num_atoms());

    mdarray<double_complex, 2> phase_factors(rl->spl_num_gvec().local_size(), max_num_atoms);

    mdarray<double_complex, 2> d_mtrx_pw(rl->spl_num_gvec().local_size(), 
                                         parameters_.unit_cell()->max_mt_basis_size() * 
                                         (parameters_.unit_cell()->max_mt_basis_size() + 1) / 2);
    
    for (int iat = 0; iat < parameters_.unit_cell()->num_atom_types(); iat++)
    {
        auto atom_type = parameters_.unit_cell()->atom_type(iat);
        int nbf = atom_type->mt_basis_size();

        mdarray<double_complex, 2> d_mtrx_packed(atom_type->num_atoms(), nbf * (nbf + 1) / 2);
        for (int i = 0; i < atom_type->num_atoms(); i++)
        {
            int ia = atom_type->atom_id(i);

            for (int xi2 = 0; xi2 < nbf; xi2++)
            {
                for (int xi1 = 0; xi1 <= xi2; xi1++)
                {
                    d_mtrx_packed(i, xi2 * (xi2 + 1) / 2 + xi1) = pp_complex_density_matrix(xi2, xi1, 0, ia);
                }
            }
            for (int igloc = 0; igloc < (int)rl->spl_num_gvec().local_size(); igloc++)
                phase_factors(igloc, i) = conj(rl->gvec_phase_factor<local>(igloc, ia));

        }
        blas<cpu>::gemm(0, 0, (int)rl->spl_num_gvec().local_size(), nbf * (nbf + 1) / 2, atom_type->num_atoms(),
                        &phase_factors(0, 0), phase_factors.ld(), &d_mtrx_packed(0, 0), d_mtrx_packed.ld(), 
                        &d_mtrx_pw(0, 0), d_mtrx_pw.ld());
        
        #pragma omp parallel
        for (int xi2 = 0; xi2 < nbf; xi2++)
        {
            int idx12 = xi2 * (xi2 + 1) / 2;

            /* add diagonal term */
            #pragma omp for
            for (int igloc = 0; igloc < (int)rl->spl_num_gvec().local_size(); igloc++)
            {
                /* D_{xi2,xi2} * Q(G)_{xi2, xi2} */
                f_pw[rl->spl_num_gvec(igloc)] += d_mtrx_pw(igloc, idx12 + xi2) * 
                                                 atom_type->uspp().q_pw(igloc, idx12 + xi2);

            }
            /* add non-diagonal terms */
            for (int xi1 = 0; xi1 < xi2; xi1++, idx12++)
            {
                #pragma omp for
                for (int igloc = 0; igloc < (int)rl->spl_num_gvec().local_size(); igloc++)
                {
                    /* D_{xi2,xi1} * Q(G)_{xi1, xi2} */
                    f_pw[rl->spl_num_gvec(igloc)] += 2 * real(d_mtrx_pw(igloc, idx12) * 
                                                              atom_type->uspp().q_pw(igloc, idx12));
                }
            }
        }
    }
    
    Platform::allgather(&f_pw[0], (int)rl->spl_num_gvec().global_offset(), (int)rl->spl_num_gvec().local_size());

    fft_->input(rl->num_gvec(), rl->fft_index(), &f_pw[0]);
    fft_->transform(1);
    for (int ir = 0; ir < fft_->size(); ir++) rho_->f_it<global>(ir) += real(fft_->buffer(ir));
}

#ifdef _GPU_

extern "C" void sum_q_pw_d_mtrx_pw_gpu(int num_gvec_loc,
                                       int num_beta,
                                       void* q_pw_t,
                                       void* dm_g,
                                       void* rho_pw);

extern "C" void generate_d_mtrx_pw_gpu(int num_atoms,
                                       int num_gvec_loc,
                                       int num_beta,
                                       double* atom_pos,
                                       int* gvec,
                                       void* d_mtrx_packed,
                                       void* d_mtrx_pw);

void Density::add_q_contribution_to_valence_density_gpu(K_set& ks)
{
    Timer t("sirius::Density::add_q_contribution_to_valence_density_gpu");

    //========================================================================================
    // if we have ud and du spin blocks, don't compute one of them (du in this implementation)
    // because density matrix is symmetric
    //========================================================================================
    int num_zdmat = (parameters_.num_mag_dims() == 3) ? 3 : (parameters_.num_mag_dims() + 1);

    // complex density matrix
    mdarray<double_complex, 4> pp_complex_density_matrix(parameters_.unit_cell()->max_mt_basis_size(), 
                                                    parameters_.unit_cell()->max_mt_basis_size(),
                                                    num_zdmat, parameters_.unit_cell()->num_atoms());
    pp_complex_density_matrix.allocate_on_device();
    pp_complex_density_matrix.zero_on_device();
    
    //=========================
    // add k-point contribution
    //=========================
    for (int ikloc = 0; ikloc < (int)ks.spl_num_kpoints().local_size(); ikloc++)
    {
        int ik = ks.spl_num_kpoints(ikloc);
        std::vector< std::pair<int, double> > occupied_bands = get_occupied_bands_list(ks.band(), ks[ik]);

        add_kpoint_contribution_pp_gpu(ks[ik], occupied_bands, pp_complex_density_matrix);
    }
    pp_complex_density_matrix.copy_to_host();
    pp_complex_density_matrix.deallocate_on_device();

    Platform::allreduce(pp_complex_density_matrix.ptr(), (int)pp_complex_density_matrix.size());

    auto rl = parameters_.reciprocal_lattice();

    for (int iat = 0; iat < parameters_.unit_cell()->num_atom_types(); iat++)
    {
         auto type = parameters_.unit_cell()->atom_type(iat);
         type->uspp().q_pw.allocate_on_device();
         type->uspp().q_pw.copy_to_device();
    }

    mdarray<int, 2> gvec(3, rl->spl_num_gvec().local_size());
    for (int igloc = 0; igloc < (int)rl->spl_num_gvec().local_size(); igloc++)
    {
        for (int x = 0; x < 3; x++) gvec(x, igloc) = rl->gvec(rl->spl_num_gvec(igloc))[x];
    }
    gvec.allocate_on_device();
    gvec.copy_to_device();

    std::vector<double_complex> rho_pw(rl->num_gvec(), double_complex(0, 0));
    mdarray<double_complex, 1> rho_pw_gpu(&rho_pw[rl->spl_num_gvec().global_offset()], rl->spl_num_gvec().local_size());
    rho_pw_gpu.allocate_on_device();
    rho_pw_gpu.zero_on_device();

    for (int iat = 0; iat < parameters_.unit_cell()->num_atom_types(); iat++)
    {
        auto type = parameters_.unit_cell()->atom_type(iat);
        int nbf = type->mt_basis_size();

        mdarray<double_complex, 2> d_mtrx_packed(type->num_atoms(), nbf * (nbf + 1) / 2);
        mdarray<double, 2> atom_pos(type->num_atoms(), 3);
        for (int i = 0; i < type->num_atoms(); i++)
        {
            int ia = type->atom_id(i);

            for (int xi2 = 0; xi2 < nbf; xi2++)
            {
                for (int xi1 = 0; xi1 <= xi2; xi1++)
                {
                    d_mtrx_packed(i, xi2 * (xi2 + 1) / 2 + xi1) = pp_complex_density_matrix(xi2, xi1, 0, ia);
                }
            }
            for (int x = 0; x < 3; x++) atom_pos(i, x) = parameters_.unit_cell()->atom(ia)->position(x);
        }
        d_mtrx_packed.allocate_on_device();
        d_mtrx_packed.copy_to_device();
        atom_pos.allocate_on_device();
        atom_pos.copy_to_device();

        mdarray<double_complex, 2> d_mtrx_pw(NULL, rl->spl_num_gvec().local_size(), nbf * (nbf + 1) / 2);
        d_mtrx_pw.allocate_on_device();
        d_mtrx_pw.zero_on_device();

        generate_d_mtrx_pw_gpu(type->num_atoms(),
                               (int)rl->spl_num_gvec().local_size(),
                               nbf,
                               atom_pos.ptr_device(),
                               gvec.ptr_device(),
                               d_mtrx_packed.ptr_device(),
                               d_mtrx_pw.ptr_device());

        sum_q_pw_d_mtrx_pw_gpu((int)rl->spl_num_gvec().local_size(), 
                               nbf,
                               type->uspp().q_pw.ptr_device(),
                               d_mtrx_pw.ptr_device(),
                               rho_pw_gpu.ptr_device());
    }

    rho_pw_gpu.copy_to_host();

    Platform::allgather(&rho_pw[0], (int)rl->spl_num_gvec().global_offset(), (int)rl->spl_num_gvec().local_size());
    
    fft_->input(rl->num_gvec(), rl->fft_index(), &rho_pw[0]);
    fft_->transform(1);
    for (int ir = 0; ir < fft_->size(); ir++) rho_->f_it<global>(ir) += real(fft_->buffer(ir));
    
    for (int iat = 0; iat < parameters_.unit_cell()->num_atom_types(); iat++)
         parameters_.unit_cell()->atom_type(iat)->uspp().q_pw.deallocate_on_device();
}
#endif

void Density::generate_valence_density_mt(K_set& ks)
{
    Timer t("sirius::Density::generate_valence_density_mt");

    //========================================================================================
    // if we have ud and du spin blocks, don't compute one of them (du in this implementation)
    // because density matrix is symmetric
    //========================================================================================
    int num_zdmat = (parameters_.num_mag_dims() == 3) ? 3 : (parameters_.num_mag_dims() + 1);

    // complex density matrix
    mdarray<double_complex, 4> mt_complex_density_matrix(parameters_.unit_cell()->max_mt_basis_size(), 
                                                    parameters_.unit_cell()->max_mt_basis_size(),
                                                    num_zdmat, parameters_.unit_cell()->num_atoms());
    mt_complex_density_matrix.zero();
    
    //=========================
    // add k-point contribution
    //=========================
    for (int ikloc = 0; ikloc < (int)ks.spl_num_kpoints().local_size(); ikloc++)
    {
        int ik = ks.spl_num_kpoints(ikloc);
        std::vector< std::pair<int, double> > occupied_bands = get_occupied_bands_list(ks.band(), ks[ik]);
        add_kpoint_contribution_mt(ks[ik], occupied_bands, mt_complex_density_matrix);
    }
    
    mdarray<double_complex, 4> mt_complex_density_matrix_loc(parameters_.unit_cell()->max_mt_basis_size(), 
                                                        parameters_.unit_cell()->max_mt_basis_size(),
                                                        num_zdmat, parameters_.unit_cell()->spl_num_atoms().local_size(0));
   
    for (int j = 0; j < num_zdmat; j++)
    {
        for (int ia = 0; ia < parameters_.unit_cell()->num_atoms(); ia++)
        {
            int ialoc = (int)parameters_.unit_cell()->spl_num_atoms().local_index(ia);
            int rank = parameters_.unit_cell()->spl_num_atoms().local_rank(ia);

            Platform::reduce(&mt_complex_density_matrix(0, 0, j, ia), &mt_complex_density_matrix_loc(0, 0, j, ialoc),
                             parameters_.unit_cell()->max_mt_basis_size() * parameters_.unit_cell()->max_mt_basis_size(),
                             parameters_.mpi_grid().communicator(), rank);
        }
    }
   
    // compute occupation matrix
    if (parameters_.uj_correction())
    {
        Timer* t3 = new Timer("sirius::Density::generate:om");
        
        mdarray<double_complex, 4> occupation_matrix(16, 16, 2, 2); 
        
        for (int ialoc = 0; ialoc < (int)parameters_.unit_cell()->spl_num_atoms().local_size(); ialoc++)
        {
            int ia = parameters_.unit_cell()->spl_num_atoms(ialoc);
            Atom_type* type = parameters_.unit_cell()->atom(ia)->type();
            
            occupation_matrix.zero();
            for (int l = 0; l <= 3; l++)
            {
                int num_rf = type->indexr().num_rf(l);

                for (int j = 0; j < num_zdmat; j++)
                {
                    for (int order2 = 0; order2 < num_rf; order2++)
                    {
                    for (int lm2 = Utils::lm_by_l_m(l, -l); lm2 <= Utils::lm_by_l_m(l, l); lm2++)
                    {
                        for (int order1 = 0; order1 < num_rf; order1++)
                        {
                        for (int lm1 = Utils::lm_by_l_m(l, -l); lm1 <= Utils::lm_by_l_m(l, l); lm1++)
                        {
                            occupation_matrix(lm1, lm2, dmat_spins_[j].first, dmat_spins_[j].second) +=
                                mt_complex_density_matrix_loc(type->indexb_by_lm_order(lm1, order1),
                                                              type->indexb_by_lm_order(lm2, order2), j, ialoc) *
                                parameters_.unit_cell()->atom(ia)->symmetry_class()->o_radial_integral(l, order1, order2);
                        }
                        }
                    }
                    }
                }
            }
        
            // restore the du block
            for (int lm1 = 0; lm1 < 16; lm1++)
            {
                for (int lm2 = 0; lm2 < 16; lm2++)
                    occupation_matrix(lm2, lm1, 1, 0) = conj(occupation_matrix(lm1, lm2, 0, 1));
            }

            parameters_.unit_cell()->atom(ia)->set_occupation_matrix(&occupation_matrix(0, 0, 0, 0));
        }

        for (int ia = 0; ia < parameters_.unit_cell()->num_atoms(); ia++)
        {
            int rank = parameters_.unit_cell()->spl_num_atoms().local_rank(ia);
            parameters_.unit_cell()->atom(ia)->sync_occupation_matrix(rank);
        }

        delete t3;
    }

    int max_num_rf_pairs = parameters_.unit_cell()->max_mt_radial_basis_size() * 
                           (parameters_.unit_cell()->max_mt_radial_basis_size() + 1) / 2;
    
    // real density matrix
    mdarray<double, 3> mt_density_matrix(parameters_.lmmax_rho(), max_num_rf_pairs, parameters_.num_mag_dims() + 1);
    
    mdarray<double, 2> rf_pairs(parameters_.unit_cell()->max_num_mt_points(), max_num_rf_pairs);
    mdarray<double, 3> dlm(parameters_.lmmax_rho(), parameters_.unit_cell()->max_num_mt_points(), 
                           parameters_.num_mag_dims() + 1);
    for (int ialoc = 0; ialoc < (int)parameters_.unit_cell()->spl_num_atoms().local_size(); ialoc++)
    {
        int ia = (int)parameters_.unit_cell()->spl_num_atoms(ialoc);
        Atom_type* atom_type = parameters_.unit_cell()->atom(ia)->type();

        int nmtp = atom_type->num_mt_points();
        int num_rf_pairs = atom_type->mt_radial_basis_size() * (atom_type->mt_radial_basis_size() + 1) / 2;
        
        Timer t1("sirius::Density::generate|sum_zdens");
        switch (parameters_.num_mag_dims())
        {
            case 3:
            {
                reduce_zdens<3>(atom_type, ialoc, mt_complex_density_matrix_loc, mt_density_matrix);
                break;
            }
            case 1:
            {
                reduce_zdens<1>(atom_type, ialoc, mt_complex_density_matrix_loc, mt_density_matrix);
                break;
            }
            case 0:
            {
                reduce_zdens<0>(atom_type, ialoc, mt_complex_density_matrix_loc, mt_density_matrix);
                break;
            }
        }
        t1.stop();
        
        Timer t2("sirius::Density::generate|expand_lm");
        // collect radial functions
        for (int idxrf2 = 0; idxrf2 < atom_type->mt_radial_basis_size(); idxrf2++)
        {
            int offs = idxrf2 * (idxrf2 + 1) / 2;
            for (int idxrf1 = 0; idxrf1 <= idxrf2; idxrf1++)
            {
                // off-diagonal pairs are taken two times: d_{12}*f_1*f_2 + d_{21}*f_2*f_1 = d_{12}*2*f_1*f_2
                int n = (idxrf1 == idxrf2) ? 1 : 2; 
                for (int ir = 0; ir < parameters_.unit_cell()->atom(ia)->type()->num_mt_points(); ir++)
                {
                    rf_pairs(ir, offs + idxrf1) = n * parameters_.unit_cell()->atom(ia)->symmetry_class()->radial_function(ir, idxrf1) * 
                                                      parameters_.unit_cell()->atom(ia)->symmetry_class()->radial_function(ir, idxrf2); 
                }
            }
        }
        for (int j = 0; j < parameters_.num_mag_dims() + 1; j++)
        {
            blas<cpu>::gemm(0, 1, parameters_.lmmax_rho(), nmtp, num_rf_pairs, 
                            &mt_density_matrix(0, 0, j), mt_density_matrix.ld(), 
                            &rf_pairs(0, 0), rf_pairs.ld(), &dlm(0, 0, j), dlm.ld());
        }

        int sz = parameters_.lmmax_rho() * nmtp * (int)sizeof(double);
        switch (parameters_.num_mag_dims())
        {
            case 3:
            {
                memcpy(&magnetization_[1]->f_mt<local>(0, 0, ialoc), &dlm(0, 0, 2), sz); 
                memcpy(&magnetization_[2]->f_mt<local>(0, 0, ialoc), &dlm(0, 0, 3), sz);
            }
            case 1:
            {
                for (int ir = 0; ir < nmtp; ir++)
                {
                    for (int lm = 0; lm < parameters_.lmmax_rho(); lm++)
                    {
                        rho_->f_mt<local>(lm, ir, ialoc) = dlm(lm, ir, 0) + dlm(lm, ir, 1);
                        magnetization_[0]->f_mt<local>(lm, ir, ialoc) = dlm(lm, ir, 0) - dlm(lm, ir, 1);
                    }
                }
                break;
            }
            case 0:
            {
                memcpy(&rho_->f_mt<local>(0, 0, ialoc), &dlm(0, 0, 0), sz);
            }
        }
        t2.stop();
    }
}

void Density::generate_valence_density_it(K_set& ks)
{
    Timer t("sirius::Density::generate_valence_density_it");

    //=========================
    // add k-point contribution
    //=========================
    for (int ikloc = 0; ikloc < (int)ks.spl_num_kpoints().local_size(); ikloc++)
    {
        int ik = ks.spl_num_kpoints(ikloc);
        auto occupied_bands = get_occupied_bands_list(ks.band(), ks[ik]);
        add_kpoint_contribution_it(ks[ik], occupied_bands);
    }
    
    //==========================================================================
    // reduce arrays; assume that each rank did it's own fraction of the density
    //==========================================================================
    Platform::allreduce(&rho_->f_it<global>(0), fft_->size()); 
    for (int j = 0; j < parameters_.num_mag_dims(); j++)
        Platform::allreduce(&magnetization_[j]->f_it<global>(0), fft_->size()); 
}

double Density::core_leakage()
{
    double sum = 0.0;
    for (int ic = 0; ic < parameters_.unit_cell()->num_atom_symmetry_classes(); ic++)
    {
        sum += parameters_.unit_cell()->atom_symmetry_class(ic)->core_leakage() * 
               parameters_.unit_cell()->atom_symmetry_class(ic)->num_atoms();
    }
    return sum;
}

double Density::core_leakage(int ic)
{
    return parameters_.unit_cell()->atom_symmetry_class(ic)->core_leakage();
}

void Density::generate_core_charge_density()
{
    Timer t("sirius::Density::generate_core_charge_density");

    for (int icloc = 0; icloc < (int)parameters_.unit_cell()->spl_num_atom_symmetry_classes().local_size(); icloc++)
    {
        int ic = parameters_.unit_cell()->spl_num_atom_symmetry_classes(icloc);
        parameters_.unit_cell()->atom_symmetry_class(ic)->generate_core_charge_density();
    }

    for (int ic = 0; ic < parameters_.unit_cell()->num_atom_symmetry_classes(); ic++)
    {
        int rank = parameters_.unit_cell()->spl_num_atom_symmetry_classes().local_rank(ic);
        parameters_.unit_cell()->atom_symmetry_class(ic)->sync_core_charge_density(rank);
    }
}

void Density::generate_pseudo_core_charge_density()
{
    Timer t("sirius::Density::generate_pseudo_core_charge_density");

    auto rl = parameters_.reciprocal_lattice();
    auto rho_core_radial_integrals = generate_rho_radial_integrals(2);

    //==auto uc = parameters_.unit_cell();

    //==mdarray<double, 2> rho_core_radial_integrals(uc->num_atom_types(), rl->num_gvec_shells_inner());

    //==splindex<block> spl_gsh(rl->num_gvec_shells_inner(), Platform::num_mpi_ranks(), Platform::mpi_rank());

    //==#pragma omp parallel
    //=={
    //==    /* splines for all atom types */
    //==    std::vector< Spline<double> > sa(uc->num_atom_types());
    //==    for (int iat = 0; iat < uc->num_atom_types(); iat++) sa[iat] = Spline<double>(uc->atom_type(iat)->radial_grid());

    //==    /* spherical Bessl functions */
    //==    sbessel_pw<double> jl(uc, 0);

    //==    #pragma omp for
    //==    for (int igsloc = 0; igsloc < (int)spl_gsh.local_size(); igsloc++)
    //==    {
    //==        int igs = (int)spl_gsh[igsloc];

    //==        jl.load(rl->gvec_shell_len(igs));

    //==        for (int iat = 0; iat < uc->num_atom_types(); iat++)
    //==        {
    //==            auto atom_type = uc->atom_type(iat);
    //==            for (int ir = 0; ir < atom_type->num_mt_points(); ir++) 
    //==                sa[iat][ir] = jl(ir, 0, iat) * atom_type->uspp().core_charge_density[ir];
    //==            rho_core_radial_integrals(iat, igs) = sa[iat].interpolate().integrate(2);
    //==        }
    //==    }
    //==}
    //==int ld = uc->num_atom_types();
    //==Platform::allgather(rho_core_radial_integrals.ptr(), static_cast<int>(ld * spl_gsh.global_offset()), 
    //==                    static_cast<int>(ld * spl_gsh.local_size()));

    std::vector<double_complex> v = rl->make_periodic_function(rho_core_radial_integrals, rl->num_gvec());
    
    fft_->input(rl->num_gvec(), rl->fft_index(), &v[0]);
    fft_->transform(1);
    fft_->output(&rho_pseudo_core_->f_it<global>(0));
}

void Density::generate(K_set& ks)
{
    Timer t("sirius::Density::generate");
    
    double wt = 0.0;
    double ot = 0.0;
    for (int ik = 0; ik < ks.num_kpoints(); ik++)
    {
        wt += ks[ik]->weight();
        for (int j = 0; j < parameters_.num_bands(); j++) ot += ks[ik]->weight() * ks[ik]->band_occupancy(j);
    }

    if (fabs(wt - 1.0) > 1e-12) error_local(__FILE__, __LINE__, "K_point weights don't sum to one");

    if (fabs(ot - parameters_.unit_cell()->num_valence_electrons()) > 1e-8)
    {
        std::stringstream s;
        s << "wrong occupancies" << std::endl
          << "  computed : " << ot << std::endl
          << "  required : " << parameters_.unit_cell()->num_valence_electrons() << std::endl
          << "  difference : " << fabs(ot - parameters_.unit_cell()->num_valence_electrons());
        warning_local(__FILE__, __LINE__, s);
    }

    // zero density and magnetization
    zero();

    // interstitial part is independent of basis type
    generate_valence_density_it(ks);
   
    // for muffin-tin part
    switch (parameters_.esm_type())
    {
        case full_potential_lapwlo:
        {
            generate_valence_density_mt(ks);
            break;
        }
        case full_potential_pwlo:
        {
            switch (parameters_.processing_unit())
            {
                stop_here
                case cpu:
                {
                    break;
                }
                #ifdef _GPU_
                case gpu:
                {
                    break;
                }
                #endif
                default:
                {
                    error_local(__FILE__, __LINE__, "wrong processing unit");
                }
            }
            break;
        }
        case ultrasoft_pseudopotential:
        {
            switch (parameters_.processing_unit())
            {
                case cpu:
                {
                    add_q_contribution_to_valence_density(ks);
                    break;
                }
                #ifdef _GPU_
                case gpu:
                {
                    add_q_contribution_to_valence_density_gpu(ks);
                    break;
                }
                #endif
                default:
                {
                    error_local(__FILE__, __LINE__, "wrong processing unit");
                }
            }
            break;
        }
    }
    
    if (parameters_.unit_cell()->full_potential())
    {
        generate_core_charge_density();

        // add core contribution
        for (int ialoc = 0; ialoc < (int)parameters_.unit_cell()->spl_num_atoms().local_size(); ialoc++)
        {
            int ia = parameters_.unit_cell()->spl_num_atoms(ialoc);
            for (int ir = 0; ir < parameters_.unit_cell()->atom(ia)->num_mt_points(); ir++)
                rho_->f_mt<local>(0, ir, ialoc) += parameters_.unit_cell()->atom(ia)->symmetry_class()->core_charge_density(ir) / y00;
        }

        // synctronize muffin-tin part (interstitial is already syncronized with allreduce)
        rho_->sync(true, false);
        for (int j = 0; j < parameters_.num_mag_dims(); j++) magnetization_[j]->sync(true, false);
    }

    std::vector<double> nel_mt;
    double nel_it;
    double nel = rho_->integrate(nel_mt, nel_it);
    
    //if (Platform::mpi_rank() == 0)
    //{
    //    printf("\n");
    //    printf("Charges before symmetrization\n");
    //    for (int ia = 0; ia < parameters_.num_atoms(); ia++)
    //    {
    //        printf("ia : %i  q : %f\n", ia, nel_mt[ia]);
    //    }
    //    printf("interstitial : %f\n", nel_it);
    //}
    
    if (fabs(nel - parameters_.unit_cell()->num_electrons()) > 1e-5)
    {
        std::stringstream s;
        s << "wrong charge density after k-point summation" << std::endl
          << "obtained value : " << nel << std::endl 
          << "target value : " << parameters_.unit_cell()->num_electrons() << std::endl
          << "difference : " << fabs(nel - parameters_.unit_cell()->num_electrons()) << std::endl;
        if (parameters_.unit_cell()->full_potential())
        {
            s << "total core leakage : " << core_leakage();
            for (int ic = 0; ic < parameters_.unit_cell()->num_atom_symmetry_classes(); ic++) 
                s << std::endl << "  atom class : " << ic << ", core leakage : " << core_leakage(ic);
        }
        warning_global(__FILE__, __LINE__, s);
    }

    //if (debug_level > 1) check_density_continuity_at_mt();
}

//void Density::check_density_continuity_at_mt()
//{
//    // generate plane-wave coefficients of the potential in the interstitial region
//    parameters_.fft().input(&rho_->f_it<global>(0));
//    parameters_.fft().transform(-1);
//    parameters_.fft().output(parameters_.num_gvec(), parameters_.fft_index(), &rho_->f_pw(0));
//    
//    SHT sht(parameters_.lmax_rho());
//
//    double diff = 0.0;
//    for (int ia = 0; ia < parameters_.num_atoms(); ia++)
//    {
//        for (int itp = 0; itp < sht.num_points(); itp++)
//        {
//            double vc[3];
//            for (int x = 0; x < 3; x++) vc[x] = sht.coord(x, itp) * parameters_.atom(ia)->mt_radius();
//
//            double val_it = 0.0;
//            for (int ig = 0; ig < parameters_.num_gvec(); ig++) 
//            {
//                double vgc[3];
//                parameters_.get_coordinates<cartesian, reciprocal>(parameters_.gvec(ig), vgc);
//                val_it += real(rho_->f_pw(ig) * exp(double_complex(0.0, Utils::scalar_product(vc, vgc))));
//            }
//
//            double val_mt = 0.0;
//            for (int lm = 0; lm < parameters_.lmmax_rho(); lm++)
//                val_mt += rho_->f_rlm(lm, parameters_.atom(ia)->num_mt_points() - 1, ia) * sht.rlm_backward(lm, itp);
//
//            diff += fabs(val_it - val_mt);
//        }
//    }
//    printf("Total and average charge difference at MT boundary : %.12f %.12f\n", diff, diff / parameters_.num_atoms() / sht.num_points());
//}


void Density::save()
{
    if (Platform::mpi_rank() == 0)
    {
        HDF5_tree fout(storage_file_name, false);
        rho_->hdf5_write(fout["density"]);
        for (int j = 0; j < parameters_.num_mag_dims(); j++)
            magnetization_[j]->hdf5_write(fout["magnetization"].create_node(j));
    }
}

void Density::load()
{
    HDF5_tree fout(storage_file_name, false);
    rho_->hdf5_read(fout["density"]);
    for (int j = 0; j < parameters_.num_mag_dims(); j++)
        magnetization_[j]->hdf5_read(fout["magnetization"][j]);
}

void Density::generate_pw_coefs()
{
    fft_->input(&rho_->f_it<global>(0));
    fft_->transform(-1);

    auto rl = parameters_.reciprocal_lattice();
    fft_->output(rl->num_gvec(), rl->fft_index(), &rho_->f_pw(0));
}

}
