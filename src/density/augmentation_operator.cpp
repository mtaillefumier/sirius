// Copyright (c) 2013-2018 Anton Kozhevnikov, Thomas Schulthess
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

/** \file augmentation_operator.cpp
 *
 *  \brief Contains implementation of sirius::Augmentation_operator class.
 */

#include "augmentation_operator.hpp"

namespace sirius {

#if defined(SIRIUS_GPU)
extern "C" void aug_op_pw_coeffs_gpu(int ngvec__, int const* gvec_shell__, int const* idx__, int idxmax__,
                                     std::complex<double> const* zilm__, int const* l_by_lm__, int lmmax__,
                                     double const* gc__, int ld0__, int ld1__,
                                     double const* gvec_rlm__, int ld2__,
                                     double const* ri_values__, int ld3__, int ld4__,
                                     double* q_pw__, int ld5__, double fourpi_omega__);
extern "C" void aug_op_pw_coeffs_deriv_gpu(int ngvec__, int const* gvec_shell__, double const* gvec_cart__,
                                           int const* idx__, int idxmax__,
                                           double const* gc__, int ld0__, int ld1__,
                                           double const* rlm__, double const* rlm_dg__, int ld2__,
                                           double const* ri_values__, double const* ri_dg_values__, int ld3__, int ld4__,
                                           double* q_pw__, int ld5__, double fourpi__, int nu__, int lmax_q__);

extern "C" void spherical_harmonics_rlm_gpu(int lmax__, int ntp__, double const* tp__, double* rlm__, int ld__);
#endif

void Augmentation_operator::generate_pw_coeffs(Radial_integrals_aug<false> const& radial_integrals__,
    sddk::mdarray<double, 2> const& tp__, sddk::memory_pool& mp__, sddk::memory_pool* mpd__)
{
    if (!atom_type_.augment()) {
        return;
    }
    PROFILE("sirius::Augmentation_operator::generate_pw_coeffs");

    double fourpi_omega = fourpi / gvec_.omega();

    /* maximum l of beta-projectors */
    int lmax_beta = atom_type_.indexr().lmax();
    int lmmax     = utils::lmmax(2 * lmax_beta);

    auto l_by_lm = utils::l_by_lm(2 * lmax_beta);

    sddk::mdarray<std::complex<double>, 1> zilm(lmmax);
    for (int l = 0, lm = 0; l <= 2 * lmax_beta; l++) {
        for (int m = -l; m <= l; m++, lm++) {
            zilm[lm] = std::pow(std::complex<double>(0, 1), l);
        }
    }

    /* Gaunt coefficients of three real spherical harmonics */
    Gaunt_coefficients<double> gaunt_coefs(lmax_beta, 2 * lmax_beta, lmax_beta, SHT::gaunt_rrr);

    /* split G-vectors between ranks */
    int gvec_count  = gvec_.count();

    /* array of real spherical harmonics for each G-vector */
    sddk::mdarray<double, 2> gvec_rlm;
    switch (atom_type_.parameters().processing_unit()) {
        case sddk::device_t::CPU: {
            gvec_rlm = sddk::mdarray<double, 2>(lmmax, gvec_count, mp__);
            #pragma omp parallel for schedule(static)
            for (int igloc = 0; igloc < gvec_count; igloc++) {
                sf::spherical_harmonics(2 * lmax_beta, tp__(igloc, 0), tp__(igloc, 1), &gvec_rlm(0, igloc));
            }
            break;
        }
        case sddk::device_t::GPU: {
            gvec_rlm = sddk::mdarray<double, 2>(lmmax, gvec_count, *mpd__);
#if defined(SIRIUS_GPU)
            spherical_harmonics_rlm_gpu(2 * lmax_beta, gvec_count, tp__.at(sddk::memory_t::device),
                gvec_rlm.at(sddk::memory_t::device), gvec_rlm.ld());
#endif
            break;
        }
    }

    /* number of beta- radial functions */
    int nbrf = atom_type_.mt_radial_basis_size();

    sddk::mdarray<double, 3> ri_values(nbrf * (nbrf + 1) / 2, 2 * lmax_beta + 1, gvec_.num_gvec_shells_local(), mp__);
    #pragma omp parallel for
    for (int j = 0; j < gvec_.num_gvec_shells_local(); j++) {
        auto ri = radial_integrals__.values(atom_type_.id(), gvec_.gvec_shell_len_local(j));
        for (int l = 0; l <= 2 * lmax_beta; l++) {
            for (int i = 0; i < nbrf * (nbrf + 1) / 2; i++) {
                ri_values(i, l, j) = ri(i, l);
            }
        }
    }

    /* number of beta-projectors */
    int nbf = atom_type_.mt_basis_size();

    int idxmax = nbf * (nbf + 1) / 2;

    /* flatten the indices */
    sddk::mdarray<int, 2> idx(3, idxmax, mp__);
    for (int xi2 = 0; xi2 < nbf; xi2++) {
        int lm2    = atom_type_.indexb(xi2).lm;
        int idxrf2 = atom_type_.indexb(xi2).idxrf;

        for (int xi1 = 0; xi1 <= xi2; xi1++) {
            int lm1    = atom_type_.indexb(xi1).lm;
            int idxrf1 = atom_type_.indexb(xi1).idxrf;

            /* packed orbital index */
            int idx12 = utils::packed_index(xi1, xi2);
            /* packed radial-function index */
            int idxrf12 = utils::packed_index(idxrf1, idxrf2);

            idx(0, idx12) = lm1;
            idx(1, idx12) = lm2;
            idx(2, idx12) = idxrf12;
        }
    }

    /* array of plane-wave coefficients */
    q_pw_ = sddk::mdarray<double, 2>(nbf * (nbf + 1) / 2, 2 * gvec_count, mp__, "q_pw_");

    switch (atom_type_.parameters().processing_unit()) {
        case sddk::device_t::CPU: {
            #pragma omp parallel for schedule(static)
            for (int igloc = 0; igloc < gvec_count; igloc++) {
                std::vector<std::complex<double>> v(lmmax);
                for (int idx12 = 0; idx12 < nbf * (nbf + 1) / 2; idx12++) {
                    int lm1     = idx(0, idx12);
                    int lm2     = idx(1, idx12);
                    int idxrf12 = idx(2, idx12);
                    for (int lm3 = 0; lm3 < lmmax; lm3++) {
                        v[lm3] = std::conj(zilm[lm3]) * gvec_rlm(lm3, igloc) *
                            ri_values(idxrf12, l_by_lm[lm3], gvec_.gvec_shell_idx_local(igloc));
                    }
                    std::complex<double> z = fourpi_omega * gaunt_coefs.sum_L3_gaunt(lm2, lm1, &v[0]);
                    q_pw_(idx12, 2 * igloc)     = z.real();
                    q_pw_(idx12, 2 * igloc + 1) = z.imag();
                }
            }
            break;
        }
        case sddk::device_t::GPU: {
            sddk::mdarray<int, 1> gvec_shell(gvec_count, mp__);
            for (int igloc = 0; igloc < gvec_count; igloc++) {
                gvec_shell(igloc) = gvec_.gvec_shell_idx_local(igloc);
            }
            gvec_shell.allocate(*mpd__).copy_to(sddk::memory_t::device);

            idx.allocate(*mpd__).copy_to(sddk::memory_t::device);

            zilm.allocate(*mpd__).copy_to(sddk::memory_t::device);

            sddk::mdarray<int, 1> l_by_lm_d(&l_by_lm[0], lmmax);
            l_by_lm_d.allocate(*mpd__).copy_to(sddk::memory_t::device);

            auto gc = gaunt_coefs.get_full_set_L3();
            gc.allocate(*mpd__).copy_to(sddk::memory_t::device);

            ri_values.allocate(*mpd__).copy_to(sddk::memory_t::device);

            q_pw_.allocate(*mpd__);

#if defined(SIRIUS_GPU)
            int ld0 = static_cast<int>(gc.size(0));
            int ld1 = static_cast<int>(gc.size(1));
            aug_op_pw_coeffs_gpu(gvec_count, gvec_shell.at(sddk::memory_t::device), idx.at(sddk::memory_t::device),
                idxmax, zilm.at(sddk::memory_t::device), l_by_lm_d.at(sddk::memory_t::device), lmmax,
                gc.at(sddk::memory_t::device), ld0, ld1, gvec_rlm.at(sddk::memory_t::device), lmmax,
                ri_values.at(sddk::memory_t::device), static_cast<int>(ri_values.size(0)), static_cast<int>(ri_values.size(1)),
                q_pw_.at(sddk::memory_t::device), static_cast<int>(q_pw_.size(0)), fourpi_omega);
#endif
            q_pw_.copy_to(sddk::memory_t::host);

            q_pw_.deallocate(sddk::memory_t::device);
        }
    }

    sym_weight_ = sddk::mdarray<double, 1>(nbf * (nbf + 1) / 2, mp__, "sym_weight_");
    for (int xi2 = 0; xi2 < nbf; xi2++) {
        for (int xi1 = 0; xi1 <= xi2; xi1++) {
            /* packed orbital index */
            int idx12          = utils::packed_index(xi1, xi2);
            sym_weight_(idx12) = (xi1 == xi2) ? 1 : 2;
        }
    }

    q_mtrx_ = sddk::mdarray<double, 2>(nbf, nbf);
    q_mtrx_.zero();

    if (gvec_.comm().rank() == 0) {
        for (int xi2 = 0; xi2 < nbf; xi2++) {
            for (int xi1 = 0; xi1 <= xi2; xi1++) {
                /* packed orbital index */
                int idx12         = utils::packed_index(xi1, xi2);
                q_mtrx_(xi1, xi2) = q_mtrx_(xi2, xi1) = gvec_.omega() * q_pw_(idx12, 0);
            }
        }
    }
    /* broadcast from rank#0 */
    gvec_.comm().bcast(&q_mtrx_(0, 0), nbf * nbf, 0);

    if (atom_type_.parameters().cfg().control().print_checksum()) {
        auto cs = q_pw_.checksum();
        auto cs1 = q_mtrx_.checksum();
        gvec_.comm().allreduce(&cs, 1);
        if (gvec_.comm().rank() == 0) {
            utils::print_checksum("q_pw", cs, std::cout);
            utils::print_checksum("q_mtrx", cs1, std::cout);
        }
    }
}

Augmentation_operator_gvec_deriv::Augmentation_operator_gvec_deriv(Simulation_parameters const& param__, int lmax__,
    sddk::Gvec const& gvec__, sddk::mdarray<double, 2> const& tp__)
    : gvec_(gvec__)
{
    PROFILE("sirius::Augmentation_operator_gvec_deriv");

    int lmmax = utils::lmmax(2 * lmax__);

    /* Gaunt coefficients of three real spherical harmonics */
    gaunt_coefs_ = std::unique_ptr<Gaunt_coefficients<double>>(
        new Gaunt_coefficients<double>(lmax__, 2 * lmax__, lmax__, SHT::gaunt_rrr));

    /* split G-vectors between ranks */
    int gvec_count = gvec__.count();

    rlm_g_  = sddk::mdarray<double, 2>(lmmax, gvec_count);
    rlm_dg_ = sddk::mdarray<double, 3>(lmmax, 3, gvec_count);

    /* array of real spherical harmonics and derivatives for each G-vector */
    #pragma omp parallel for schedule(static)
    for (int igloc = 0; igloc < gvec_count; igloc++) {
        /* compute Rlm */
        sf::spherical_harmonics(2 * lmax__, tp__(igloc, 0), tp__(igloc, 1), &rlm_g_(0, igloc));
        /* compute dRlm/dG */
        sddk::mdarray<double, 2> tmp(&rlm_dg_(0, 0, igloc), lmmax, 3);
        auto gv = gvec__.gvec_cart<sddk::index_domain_t::local>(igloc);
        sf::dRlm_dr(2 * lmax__, gv, tmp, false);
    }
    switch (param__.processing_unit()) {
        case sddk::device_t::CPU: {
            break;
        }
        case sddk::device_t::GPU: {
            rlm_g_.allocate(get_memory_pool(sddk::memory_t::device)).copy_to(sddk::memory_t::device);
            rlm_dg_.allocate(get_memory_pool(sddk::memory_t::device)).copy_to(sddk::memory_t::device);
        }
    }
}

void Augmentation_operator_gvec_deriv::prepare(Atom_type const& atom_type__,
    Radial_integrals_aug<false> const& ri__, Radial_integrals_aug<true> const& ri_dq__)
{
    PROFILE("sirius::Augmentation_operator_gvec_deriv::prepare");

    int lmax_beta = atom_type__.lmax_beta();

    /* number of beta- radial functions */
    int nbrf = atom_type__.mt_radial_basis_size();

    auto& mp = get_memory_pool(sddk::memory_t::host);

    ri_values_ = sddk::mdarray<double, 3>(2 * lmax_beta + 1, nbrf * (nbrf + 1) / 2, gvec_.num_gvec_shells_local(), mp);
    ri_dg_values_ = sddk::mdarray<double, 3>(2 * lmax_beta + 1, nbrf * (nbrf + 1) / 2, gvec_.num_gvec_shells_local(),
        mp);
    #pragma omp parallel for
    for (int j = 0; j < gvec_.num_gvec_shells_local(); j++) {
        auto ri = ri__.values(atom_type__.id(), gvec_.gvec_shell_len_local(j));
        auto ri_dg = ri_dq__.values(atom_type__.id(), gvec_.gvec_shell_len_local(j));
        for (int l = 0; l <= 2 * lmax_beta; l++) {
            for (int i = 0; i < nbrf * (nbrf + 1) / 2; i++) {
                ri_values_(l, i, j) = ri(i, l);
                ri_dg_values_(l, i, j) = ri_dg(i, l);
            }
        }
    }

    /* number of beta-projectors */
    int nbf = atom_type__.mt_basis_size();

    int idxmax = nbf * (nbf + 1) / 2;

    /* flatten the indices */
    idx_ = sddk::mdarray<int, 2>(3, idxmax, mp);
    for (int xi2 = 0; xi2 < nbf; xi2++) {
        int lm2    = atom_type__.indexb(xi2).lm;
        int idxrf2 = atom_type__.indexb(xi2).idxrf;

        for (int xi1 = 0; xi1 <= xi2; xi1++) {
            int lm1    = atom_type__.indexb(xi1).lm;
            int idxrf1 = atom_type__.indexb(xi1).idxrf;

            /* packed orbital index */
            int idx12 = utils::packed_index(xi1, xi2);
            /* packed radial-function index */
            int idxrf12 = utils::packed_index(idxrf1, idxrf2);

            idx_(0, idx12) = lm1;
            idx_(1, idx12) = lm2;
            idx_(2, idx12) = idxrf12;
        }
    }

    int gvec_count  = gvec_.count();

    gvec_shell_ = sddk::mdarray<int, 1>(gvec_count, get_memory_pool(sddk::memory_t::host));
    gvec_cart_ = sddk::mdarray<double, 2>(3, gvec_count, get_memory_pool(sddk::memory_t::host));
    for (int igloc = 0; igloc < gvec_count; igloc++) {
        auto gvc = gvec_.gvec_cart<sddk::index_domain_t::local>(igloc);
        gvec_shell_(igloc) = gvec_.gvec_shell_idx_local(igloc);
        for (int x: {0, 1, 2}) {
            gvec_cart_(x, igloc) = gvc[x];
        }
    }

    sym_weight_ = sddk::mdarray<double, 1>(nbf * (nbf + 1) / 2, mp, "sym_weight_");
    for (int xi2 = 0; xi2 < nbf; xi2++) {
        for (int xi1 = 0; xi1 <= xi2; xi1++) {
            /* packed orbital index */
            int idx12          = xi2 * (xi2 + 1) / 2 + xi1;
            sym_weight_(idx12) = (xi1 == xi2) ? 1 : 2;
        }
    }

    switch (atom_type__.parameters().processing_unit()) {
        case sddk::device_t::CPU: {
            /* array of plane-wave coefficients */
            q_pw_ = sddk::mdarray<double, 2>(nbf * (nbf + 1) / 2, 2 * gvec_count, mp, "q_pw_dg_");
            break;
        }
        case sddk::device_t::GPU: {
            auto& mpd = get_memory_pool(sddk::memory_t::device);
            ri_values_.allocate(mpd).copy_to(sddk::memory_t::device);
            ri_dg_values_.allocate(mpd).copy_to(sddk::memory_t::device);
            idx_.allocate(mpd).copy_to(sddk::memory_t::device);
            gvec_shell_.allocate(mpd).copy_to(sddk::memory_t::device);
            gvec_cart_.allocate(mpd).copy_to(sddk::memory_t::device);
            /* array of plane-wave coefficients */
            q_pw_ = sddk::mdarray<double, 2>(nbf * (nbf + 1) / 2, 2 * gvec_count, mpd, "q_pw_dg_");
            break;
        }
    }
}

void Augmentation_operator_gvec_deriv::generate_pw_coeffs(Atom_type const& atom_type__, int nu__)
{
    PROFILE("sirius::Augmentation_operator_gvec_deriv::generate_pw_coeffs");

    /* maximum l of beta-projectors */
    int lmax_beta = atom_type__.indexr().lmax();

    int lmax_q = 2 * lmax_beta;

    int lmmax = utils::lmmax(lmax_q);

    auto l_by_lm = utils::l_by_lm(lmax_q);

    sddk::mdarray<std::complex<double>, 1> zilm(lmmax);
    for (int l = 0, lm = 0; l <= lmax_q; l++) {
        for (int m = -l; m <= l; m++, lm++) {
            zilm[lm] = std::pow(std::complex<double>(0, 1), l);
        }
    }

    /* number of beta-projectors */
    int nbf = atom_type__.mt_basis_size();

    /* split G-vectors between ranks */
    int gvec_count  = gvec_.count();

    switch (atom_type__.parameters().processing_unit()) {
        case sddk::device_t::CPU: {
            #pragma omp parallel for schedule(static)
            for (int igloc = 0; igloc < gvec_count; igloc++) {
                /* index of the G-vector shell */
                int igsh = gvec_.gvec_shell_idx_local(igloc);
                std::vector<std::complex<double>> v(lmmax);
                double gvc_nu = gvec_cart_(nu__, igloc);
                for (int idx12 = 0; idx12 < nbf * (nbf + 1) / 2; idx12++) {
                    int lm1     = idx_(0, idx12);
                    int lm2     = idx_(1, idx12);
                    int idxrf12 = idx_(2, idx12);
                    for (int lm3 = 0; lm3 < lmmax; lm3++) {
                        int l = l_by_lm[lm3];
                        v[lm3] = std::conj(zilm[lm3]) *
                            (rlm_dg_(lm3, nu__, igloc) * ri_values_(l, idxrf12, igsh) +
                             rlm_g_(lm3, igloc) * ri_dg_values_(l, idxrf12, igsh) * gvc_nu);
                    }
                    std::complex<double> z = fourpi * gaunt_coefs_->sum_L3_gaunt(lm2, lm1, &v[0]);
                    q_pw_(idx12, 2 * igloc)     = z.real();
                    q_pw_(idx12, 2 * igloc + 1) = z.imag();
                }
            }
            break;
        }
        case sddk::device_t::GPU: {
            auto& mpd = get_memory_pool(sddk::memory_t::device);

            auto gc = gaunt_coefs_->get_full_set_L3();
            gc.allocate(mpd).copy_to(sddk::memory_t::device);

#if defined(SIRIUS_GPU)
            aug_op_pw_coeffs_deriv_gpu(gvec_count, gvec_shell_.at(sddk::memory_t::device), gvec_cart_.at(sddk::memory_t::device),
                idx_.at(sddk::memory_t::device), static_cast<int>(idx_.size(1)),
                gc.at(sddk::memory_t::device), static_cast<int>(gc.size(0)), static_cast<int>(gc.size(1)),
                rlm_g_.at(sddk::memory_t::device), rlm_dg_.at(sddk::memory_t::device), static_cast<int>(rlm_g_.size(0)),
                ri_values_.at(sddk::memory_t::device), ri_dg_values_.at(sddk::memory_t::device), static_cast<int>(ri_values_.size(0)),
                static_cast<int>(ri_values_.size(1)), q_pw_.at(sddk::memory_t::device), static_cast<int>(q_pw_.size(0)),
                fourpi, nu__, lmax_q);
#endif
            break;
        }
    }
}

} // namespace sirius
