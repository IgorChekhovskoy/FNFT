/*
* This file is part of FNFT.
*
* FNFT is free software; you can redistribute it and/or
* modify it under the terms of the version 2 of the GNU General
* Public License as published by the Free Software Foundation.
*
* FNFT is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program. If not, see <http://www.gnu.org/licenses/>.
*
* Contributor:
* Lianne de Vries (TU Delft) 2021.
*/

#define FNFT_ENABLE_SHORT_NAMES

#include "fnft__manakov_fscatter.h"
#include "fnft__misc.h"
#include <limits.h>


/**
 * Returns the length of array to be allocated based on the number
 * of samples and discretization.
 */
UINT manakov_fscatter_numel(UINT D, manakov_discretization_t discretization)
{
    const UINT deg = manakov_discretization_degree(discretization);
    if (deg == 0)
        return 0; // unknown discretization
    else
        return poly_fmult3x3_numel(deg, D);
}

/**
 * Returns the scattering matrix for a single step at frequency zero.
 * Exact expressions computed with Maple.
 */
static inline void manakov_fscatter_zero_freq_scatter_matrix(COMPLEX * const M,
                                                const REAL eps_t, const COMPLEX q1, const COMPLEX q2, const INT kappa)
{
    // This function computes the matrix exponential
    //   M = expm([0,q1,q2; -q1*,0,0; -q2*,0,0]*eps_t)
    //   If r=-conj(q), we have M21 = -conj(M12), M31 = -conj(M13), M32 = conj(M23)

    COMPLEX r1 = conj(q1)*-1*kappa; 
    COMPLEX r2 = conj(q2)*-1*kappa;
    COMPLEX x = CSQRT(r1*q1+r2*q2)*eps_t;

    M[0] = CCOS(I*x);                               // M11
    M[1] = q1*misc_CSINC(I*x)*eps_t;                // M12
    M[2] = q2*misc_CSINC(I*x)*eps_t;                // M13
    M[3] = -conj(M[1])*kappa;                             // M21
    M[4] = eps_t*eps_t*(r2*q2 + r1*q1*CCOS(I*x))/(x*x);     // M22
    M[5] = q2*r1*(CCOS(I*x)-1)*eps_t*eps_t/(x*x);   // M23
    M[6] = -conj(M[2])*kappa;                             // M31
    M[7] = conj(M[5]);                              // M32
    M[8] = eps_t*eps_t*(r1*q1 + r2*q2*CCOS(I*x))/(x*x);     // M33
    
}

// returns the matrix exponential exp([0, s1, s2;  t1, 0, 0;  t2, 0, 0])
static inline void manakov_fscatter_get_E(COMPLEX * const M, const REAL eps_t,
                            const COMPLEX s1, const COMPLEX s2, const COMPLEX t1, const COMPLEX t2) 
{
    COMPLEX x = CSQRT(t1*s1+t2*s2);

    M[0] = CCOS(I*x);                               // M11
    M[1] = s1*misc_CSINC(I*x);                // M12
    M[2] = s2*misc_CSINC(I*x);                // M13
    M[3] = t1*misc_CSINC(I*x);                             // M21
    M[4] = (t2*s2 + t1*s1*CCOS(I*x))/(x*x);     // M22
    M[5] = s2*t1*(CCOS(I*x)-1)/(x*x);   // M23
    M[6] = t2*misc_CSINC(I*x);                             // M31
    M[7] = s1*t2*(CCOS(I*x)-1)/(x*x);                              // M32
    M[8] = (t1*s1 + t2*s2*CCOS(I*x))/(x*x);     // M33
}

static inline void manakov_fscatter_mult_E1_TM_E2(COMPLEX * const p11, COMPLEX * const p12, COMPLEX * const p13,
            COMPLEX * const p21, COMPLEX * const p22, COMPLEX * const p23, COMPLEX * const p31, COMPLEX * const p32,
            COMPLEX * const p33, COMPLEX * const E1, COMPLEX * const E2)
{
    // This function multiplies the TM from FTES4 with E1 and E2.
    // The polynmial coefs pij[n] from TM get replaced by the values after E1*TM*E2
        COMPLEX P[9] = { *p11,*p12,*p13, *p21,*p22,*p23, *p31,*p32,*p33 };
        COMPLEX buf1[9];
        COMPLEX buf2[9];
        misc_matrix_mult(3,3,3,E1,&P[0],&buf1[0]);
        misc_matrix_mult(3,3,3,&buf1[0],E2,&buf2[0]);
        *p11 = buf2[0];
        *p12 = buf2[1];
        *p13 = buf2[2];
        *p21 = buf2[3];
        *p22 = buf2[4];
        *p23 = buf2[5];
        *p31 = buf2[6];
        *p32 = buf2[7];
        *p33 = buf2[8];
}


/**
 * Fast computation of polynomial approximation of the combined scattering
 * matrix.
 */
INT manakov_fscatter(const UINT D, COMPLEX const * const q1, COMPLEX const * const q2, const INT kappa, 
                 REAL eps_t, COMPLEX * const result, UINT * const deg_ptr,
                 INT * const W_ptr, manakov_discretization_t const discretization)
                 //TODO: make sure enough memory is allocated for result to allow
                 // r_stride > (2*(deg + 1) - 1) if needed because of next_fast_size
{
    /* D: number of samples
    q1,2: arrays of length D containing the samples: qi=[q1(t0), q1(t0+eps_t), ... q1(T{D-1})]
    eps_t: timestep size
    result: array of size given by manakov_fscatter_numel, containing the final result for the scattering matrix
    deg_ptr: pointer to var. containing the degree of the discretization
    W_ptr: normalization flag. polynomial coefficients are normalized if W_ptr is non-zero (is that the same as not NULL? I think so)
    discretization: type of discretization used. We use the discretizations of fnft__akns_discretization_t, BUT call manakov_fscatter instead of akns_fscatter when dealing with manakov
    kappa: focussing or defocussing

    r is not passed here, as for the manakov equation this is always equal to -kappa *conj(q)
    */

    INT ret_code;
    COMPLEX *p, *p11, *p12, *p13, *p21, *p22, *p23, *p31, *p32, *p33;
    UINT i, n, len, D_eff, j, upsampling_factor;   // TODO: j for print statements, remove when done
    COMPLEX e_Bstorage[63], E_storage[18], scl, scl_den, Q, R; // e_Bstorage length: 9x the max. amount of e_B matrices we need for a discretization
    // These variables are used to store the values of matrix exponentials
    // e_aB = expm([0,q;r,0]*a*eps_t/degree1step)
    COMPLEX *e_B, *e_2_3B, *e_1_3B, *e_1_2B, *e_1_4B, *e_1_6B, *e_7_48B, *e_3_8B, *e_m1_48B;
    COMPLEX *E1, *E2;               // matrices for FTES4 method
    COMPLEX q1n[3], q2n[3];

    // Check inputs
    if (D == 0 || D>INT_MAX)
        return E_INVALID_ARGUMENT(D);
    if (q1 == NULL)
        return E_INVALID_ARGUMENT(q1);
    if (q2 == NULL)
        return E_INVALID_ARGUMENT(q2);
    if (eps_t <= 0.0)
        return E_INVALID_ARGUMENT(eps_t);
    if (result == NULL)
        return E_INVALID_ARGUMENT(result);
    if (deg_ptr == NULL)
        return E_INVALID_ARGUMENT(deg_ptr);
    if (abs(kappa) != 1)
        return E_INVALID_ARGUMENT(kappa);

    //Determine D_eff. Some methods are better implemented "as if" we have 2*D samples
    /* 
    if (discretization == akns_discretization_4SPLIT4A || discretization == akns_discretization_4SPLIT4B
            || akns_discretization_4SPLIT6B){
        D_eff = 2*D;
    }
    else{
        D_eff = D;
    }
    */

    upsampling_factor = manakov_discretization_upsampling_factor(discretization);
    if (upsampling_factor == 0) {
        ret_code = E_INVALID_ARGUMENT(discretization);
        goto release_mem;       // TODO: or maybe define leave_funa and goto leave_fun
    }
    D_eff = D*upsampling_factor;

    // Declaring and defining some variables for 4split4 methods
    COMPLEX *q1_c1, *q1_c2, *q2_c1, *q2_c2, *q1_eff, *q2_eff;
        const REAL a1 = 0.25 + sqrt(3)/6;
        const REAL a2 = 0.25 - sqrt(3)/6;


    // Allocate buffers
    len = manakov_fscatter_numel(D_eff, discretization);
    if (len == 0) { // size D>0, this means unknown discretization
        return E_INVALID_ARGUMENT(discretization);
    }

    p = malloc(len*sizeof(COMPLEX));

    // degree 1 polynomials
    if (p == NULL)
        return E_NOMEM;

    // Set the individual scattering matrices up
     *deg_ptr = manakov_discretization_degree(discretization);


    if (*deg_ptr == 0) {
        ret_code = E_INVALID_ARGUMENT(discretization);
        goto release_mem;
    }
    const UINT deg = *deg_ptr;
    p11 = p;
    p12 = p11 + D_eff*(deg+1);
    p13 = p12 + D_eff*(deg+1);
    p21 = p13 + D_eff*(deg+1);
    p22 = p21 + D_eff*(deg+1);
    p23 = p22 + D_eff*(deg+1);
    p31 = p23 + D_eff*(deg+1);
    p32 = p31 + D_eff*(deg+1);
    p33 = p32 + D_eff*(deg+1);

    switch (discretization) {

        case manakov_discretization_2SPLIT3A:
            e_1_3B = &e_Bstorage[0];    // exp(Bh/3)
            e_B    = &e_Bstorage[18];   // exp(3/3 Bh) = exp(Bh)
            
            for (i=D; i-->0;) {
                
                manakov_fscatter_zero_freq_scatter_matrix(e_1_3B, eps_t/3, q1[i], q2[i], kappa);
                manakov_fscatter_zero_freq_scatter_matrix(e_B, eps_t, q1[i], q2[i], kappa);

                p11[0] = 0.0;   // z^6
                p11[1] = 0.0;   // z^5
                p11[2] = (9*e_1_3B[0]*e_1_3B[1]*e_1_3B[3])/8 + (9*e_1_3B[0]*e_1_3B[2]*e_1_3B[6])/8 + (9*e_1_3B[1]*e_1_3B[3]*e_1_3B[4])/8 + (9*e_1_3B[1]*e_1_3B[5]*e_1_3B[6])/8 + (9*e_1_3B[2]*e_1_3B[3]*e_1_3B[7])/8 + (9*e_1_3B[2]*e_1_3B[6]*e_1_3B[8])/8;    // z^4
                p11[3] = 0.0;   // z^3 
                p11[4] = 0.0;   // z^2
                p11[5] = 0.0;   // z^1
                p11[6] = (9*e_1_3B[0]*e_1_3B[0]*e_1_3B[0])/8 - e_B[0]/8 + (9*e_1_3B[0]*e_1_3B[1]*e_1_3B[3])/8 + (9*e_1_3B[0]*e_1_3B[2]*e_1_3B[6])/8;       // z^0

                p12[0] = 0.0;
                p12[1] = 0.0;
                p12[2] = (9*e_1_3B[1]*e_1_3B[4]*e_1_3B[4])/8 + (9*e_1_3B[0]*e_1_3B[1]*e_1_3B[4])/8 + (9*e_1_3B[0]*e_1_3B[2]*e_1_3B[7])/8 + (9*e_1_3B[1]*e_1_3B[5]*e_1_3B[7])/8 + (9*e_1_3B[2]*e_1_3B[4]*e_1_3B[7])/8 + (9*e_1_3B[2]*e_1_3B[7]*e_1_3B[8])/8;
                p12[3] = 0.0;
                p12[4] = 0.0;
                p12[5] = 0.0;
                p12[6] = (9*e_1_3B[0]*e_1_3B[0]*e_1_3B[1])/8 + (9*e_1_3B[3]*e_1_3B[1]*e_1_3B[1])/8 + (9*e_1_3B[2]*e_1_3B[6]*e_1_3B[1])/8 - e_B[1]/8;

                p13[0] = 0.0;
                p13[1] = 0.0;
                p13[2] = (9*e_1_3B[2]*e_1_3B[8]*e_1_3B[8])/8 + (9*e_1_3B[0]*e_1_3B[1]*e_1_3B[5])/8 + (9*e_1_3B[0]*e_1_3B[2]*e_1_3B[8])/8 + (9*e_1_3B[1]*e_1_3B[4]*e_1_3B[5])/8 + (9*e_1_3B[1]*e_1_3B[5]*e_1_3B[8])/8 + (9*e_1_3B[2]*e_1_3B[5]*e_1_3B[7])/8;
                p13[3] = 0.0;
                p13[4] = 0.0;
                p13[5] = 0.0;
                p13[6] = (9*e_1_3B[0]*e_1_3B[0]*e_1_3B[2])/8 + (9*e_1_3B[6]*e_1_3B[2]*e_1_3B[2])/8 + (9*e_1_3B[1]*e_1_3B[3]*e_1_3B[2])/8 - e_B[2]/8;

                p21[0] = (9*e_1_3B[1]*e_1_3B[3]*e_1_3B[3])/8 - e_B[3]/8 + (9*e_1_3B[3]*e_1_3B[4]*e_1_3B[4])/8 + (9*e_1_3B[2]*e_1_3B[3]*e_1_3B[6])/8 + (9*e_1_3B[3]*e_1_3B[5]*e_1_3B[7])/8 + (9*e_1_3B[4]*e_1_3B[5]*e_1_3B[6])/8 + (9*e_1_3B[5]*e_1_3B[6]*e_1_3B[8])/8;
                p21[1] = 0.0;
                p21[2] = 0.0;
                p21[3] = 0.0;
                p21[4] = (9*e_1_3B[0]*e_1_3B[0]*e_1_3B[3])/8 + (9*e_1_3B[0]*e_1_3B[3]*e_1_3B[4])/8 + (9*e_1_3B[0]*e_1_3B[5]*e_1_3B[6])/8;
                p21[5] = 0.0;
                p21[6] = 0.0;

                p22[0] = (9*e_1_3B[4]*e_1_3B[4]*e_1_3B[4])/8 - e_B[4]/8 + (9*e_1_3B[1]*e_1_3B[3]*e_1_3B[4])/8 + (9*e_1_3B[2]*e_1_3B[3]*e_1_3B[7])/8 + (9*e_1_3B[4]*e_1_3B[5]*e_1_3B[7])/4 + (9*e_1_3B[5]*e_1_3B[7]*e_1_3B[8])/8;
                p22[1] = 0.0;
                p22[2] = 0.0;
                p22[3] = 0.0;
                p22[4] = (9*e_1_3B[0]*e_1_3B[1]*e_1_3B[3])/8 + (9*e_1_3B[1]*e_1_3B[3]*e_1_3B[4])/8 + (9*e_1_3B[1]*e_1_3B[5]*e_1_3B[6])/8;
                p22[5] = 0.0;
                p22[6] = 0.0;

                p23[0] = (9*e_1_3B[4]*e_1_3B[4]*e_1_3B[5])/8 + (9*e_1_3B[4]*e_1_3B[5]*e_1_3B[8])/8 + (9*e_1_3B[7]*e_1_3B[5]*e_1_3B[5])/8 + (9*e_1_3B[5]*e_1_3B[8]*e_1_3B[8])/8 + (9*e_1_3B[1]*e_1_3B[3]*e_1_3B[5])/8 + (9*e_1_3B[2]*e_1_3B[3]*e_1_3B[8])/8 - e_B[5]/8;
                p23[1] = 0.0;
                p23[2] = 0.0;
                p23[3] = 0.0;
                p23[4] = (9*e_1_3B[0]*e_1_3B[2]*e_1_3B[3])/8 + (9*e_1_3B[2]*e_1_3B[3]*e_1_3B[4])/8 + (9*e_1_3B[2]*e_1_3B[5]*e_1_3B[6])/8;
                p23[5] = 0.0;
                p23[6] = 0.0;

                p31[0] = (9*e_1_3B[2]*e_1_3B[6]*e_1_3B[6])/8 - e_B[6]/8 + (9*e_1_3B[6]*e_1_3B[8]*e_1_3B[8])/8 + (9*e_1_3B[1]*e_1_3B[3]*e_1_3B[6])/8 + (9*e_1_3B[3]*e_1_3B[4]*e_1_3B[7])/8 + (9*e_1_3B[3]*e_1_3B[7]*e_1_3B[8])/8 + (9*e_1_3B[5]*e_1_3B[6]*e_1_3B[7])/8;
                p31[1] = 0.0;
                p31[2] = 0.0;
                p31[3] = 0.0;
                p31[4] = (9*e_1_3B[0]*e_1_3B[0]*e_1_3B[6])/8 + (9*e_1_3B[0]*e_1_3B[3]*e_1_3B[7])/8 + (9*e_1_3B[0]*e_1_3B[6]*e_1_3B[8])/8;
                p31[5] = 0.0;
                p31[6] = 0.0;

				p32[0] = (9*e_1_3B[4]*e_1_3B[4]*e_1_3B[7])/8 + (9*e_1_3B[4]*e_1_3B[7]*e_1_3B[8])/8 + (9*e_1_3B[1]*e_1_3B[6]*e_1_3B[4])/8 + (9*e_1_3B[5]*e_1_3B[7]*e_1_3B[7])/8 + (9*e_1_3B[7]*e_1_3B[8]*e_1_3B[8])/8 + (9*e_1_3B[2]*e_1_3B[6]*e_1_3B[7])/8 - e_B[7]/8;
                p32[1] = 0.0;
                p32[2] = 0.0;
                p32[3] = 0.0;
				p32[4] = (9*e_1_3B[0]*e_1_3B[1]*e_1_3B[6])/8 + (9*e_1_3B[1]*e_1_3B[3]*e_1_3B[7])/8 + (9*e_1_3B[1]*e_1_3B[6]*e_1_3B[8])/8;
                p32[5] = 0.0;
                p32[6] = 0.0;

                p33[0] = (9*e_1_3B[8]*e_1_3B[8]*e_1_3B[8])/8 - e_B[8]/8 + (9*e_1_3B[1]*e_1_3B[5]*e_1_3B[6])/8 + (9*e_1_3B[2]*e_1_3B[6]*e_1_3B[8])/8 + (9*e_1_3B[4]*e_1_3B[5]*e_1_3B[7])/8 + (9*e_1_3B[5]*e_1_3B[7]*e_1_3B[8])/4;
                p33[1] = 0.0;
                p33[2] = 0.0;
                p33[3] = 0.0;
                p33[4] = (9*e_1_3B[0]*e_1_3B[2]*e_1_3B[6])/8 + (9*e_1_3B[2]*e_1_3B[3]*e_1_3B[7])/8 + (9*e_1_3B[2]*e_1_3B[6]*e_1_3B[8])/8;
                p33[5] = 0.0;
                p33[6] = 0.0;

                misc_print_buf(7,p11,"p11");

                p11 += 7;
                p12 += 7;
                p13 += 7;
                p21 += 7;
                p22 += 7;
                p23 += 7;
                p31 += 7;
                p32 += 7;
                p33 += 7;
            }
            break;

            case manakov_discretization_2SPLIT3B:
            e_1_3B = &e_Bstorage[0];    // exp(Bh/3)
            e_2_3B = &e_Bstorage[9];    // exp(2/3 Bh)
            e_B    = &e_Bstorage[18];   // exp(3/3 Bh) = exp(Bh)
            
            for (i=D; i-->0;) {
                
                manakov_fscatter_zero_freq_scatter_matrix(e_1_3B, eps_t/3, q1[i], q2[i], kappa);
                manakov_fscatter_zero_freq_scatter_matrix(e_2_3B, eps_t*2/3, q1[i], q2[i], kappa);
                manakov_fscatter_zero_freq_scatter_matrix(e_B, eps_t, q1[i], q2[i], kappa);

                p11[0] = 0.0;   // z^6
                p11[1] = 0.0;   // z^5
                p11[2] = (9*e_1_3B[1]*e_2_3B[3])/8 + (9*e_1_3B[2]*e_2_3B[6])/8;   // z^4
                p11[3] = 0.0;   // z^3 
                p11[4] = 0.0;   // z^2
                p11[5] = 0.0;   // z^1
                p11[6] = (9*e_1_3B[0]*e_2_3B[0])/8 - e_B[0]/8;   // z^0

                p12[0] = (9*e_1_3B[1]*e_2_3B[4])/8 - e_B[1]/8 + (9*e_1_3B[2]*e_2_3B[7])/8;
                p12[1] = 0.0;
                p12[2] = 0.0;
                p12[3] = 0.0;
                p12[4] = (9*e_1_3B[0]*e_2_3B[1])/8;
                p12[5] = 0.0;
                p12[6] = 0.0;

                p13[0] = (9*e_1_3B[1]*e_2_3B[5])/8 - e_B[2]/8 + (9*e_1_3B[2]*e_2_3B[8])/8;
                p13[1] = 0.0;
                p13[2] = 0.0;
                p13[3] = 0.0;
                p13[4] = (9*e_1_3B[0]*e_2_3B[2])/8;
                p13[5] = 0.0;
                p13[6] = 0.0;

                p21[0] = 0.0;
                p21[1] = 0.0;
                p21[2] = (9*e_1_3B[4]*e_2_3B[3])/8 + (9*e_1_3B[5]*e_2_3B[6])/8;
                p21[3] = 0.0;
                p21[4] = 0.0;
                p21[5] = 0.0;
                p21[6] = (9*e_1_3B[3]*e_2_3B[0])/8 - e_B[3]/8;

                p22[0] = (9*e_1_3B[4]*e_2_3B[4])/8 - e_B[4]/8 + (9*e_1_3B[5]*e_2_3B[7])/8;
                p22[1] = 0.0;
                p22[2] = 0.0;
                p22[3] = 0.0;
                p22[4] = (9*e_1_3B[3]*e_2_3B[1])/8;
                p22[5] = 0.0;
                p22[6] = 0.0;

                p23[0] = (9*e_1_3B[4]*e_2_3B[5])/8 - e_B[5]/8 + (9*e_1_3B[5]*e_2_3B[8])/8;
                p23[1] = 0.0;
                p23[2] = 0.0;
                p23[3] = 0.0;
                p23[4] = (9*e_1_3B[3]*e_2_3B[2])/8;
                p23[5] = 0.0;
                p23[6] = 0.0;

                p31[0] = 0.0;
                p31[1] = 0.0;
                p31[2] = (9*e_1_3B[7]*e_2_3B[3])/8 + (9*e_1_3B[8]*e_2_3B[6])/8;
                p31[3] = 0.0;
                p31[4] = 0.0;
                p31[5] = 0.0;
                p31[6] = (9*e_1_3B[6]*e_2_3B[0])/8 - e_B[6]/8;

				p32[0] = (9*e_1_3B[7]*e_2_3B[4])/8 - e_B[7]/8 + (9*e_1_3B[8]*e_2_3B[7])/8;
                p32[1] = 0.0;
                p32[2] = 0.0;
                p32[3] = 0.0;
				p32[4] = (9*e_1_3B[6]*e_2_3B[1])/8;
                p32[5] = 0.0;
                p32[6] = 0.0;

                p33[0] = (9*e_1_3B[7]*e_2_3B[5])/8 - e_B[8]/8 + (9*e_1_3B[8]*e_2_3B[8])/8;
                p33[1] = 0.0;
                p33[2] = 0.0;
                p33[3] = 0.0;
                p33[4] = (9*e_1_3B[6]*e_2_3B[2])/8;
                p33[5] = 0.0;
                p33[6] = 0.0;

                p11 += 7;
                p12 += 7;
                p13 += 7;
                p21 += 7;
                p22 += 7;
                p23 += 7;
                p31 += 7;
                p32 += 7;
                p33 += 7;
            }
            break;

            case manakov_discretization_2SPLIT4A:
            e_1_2B = &e_Bstorage[0];    // exp(Bh/2)
            e_B = &e_Bstorage[9];    // exp(Bh)

            for (i=D_eff; i-->0;) {
                
                manakov_fscatter_zero_freq_scatter_matrix(e_1_2B, eps_t/2, q1[i], q2[i], kappa);
                manakov_fscatter_zero_freq_scatter_matrix(e_B, eps_t, q1[i], q2[i], kappa);

                p11[0] = 0.0;   // z^8
                p11[1] = 0.0;   // z^7
                p11[2] = 0.0;   // z^6
                p11[3] = 0.0;   // z^5 
                p11[4] = (4*e_1_2B[1]*e_1_2B[3])/3 + (4*e_1_2B[2]*e_1_2B[6])/3;     // z^4
                p11[5] = 0.0;   // z^3
                p11[6] = 0.0;   // z^2
                p11[7] = 0.0;   // z^1
                p11[8] = (4*e_1_2B[0]*e_1_2B[0])/3 - e_B[0]/3; // z^0

                p12[0] = 0.0;
                p12[1] = 0.0;
                p12[2] = (4*e_1_2B[1]*e_1_2B[4])/3 + (4*e_1_2B[2]*e_1_2B[7])/3;
                p12[3] = 0.0;
                p12[4] = -e_B[1]/3;
                p12[5] = 0.0;
                p12[6] = (4*e_1_2B[0]*e_1_2B[1])/3;
                p12[7] = 0.0;
                p12[8] = 0.0;

                p13[0] = 0.0;
                p13[1] = 0.0;
                p13[2] = (4*e_1_2B[1]*e_1_2B[5])/3 + (4*e_1_2B[2]*e_1_2B[8])/3;
                p13[3] = 0.0;
                p13[4] = -e_B[2]/3;
                p13[5] = 0.0;
                p13[6] = (4*e_1_2B[0]*e_1_2B[2])/3;
                p13[7] = 0.0;
                p13[8] = 0.0;

                p21[0] = 0.0;
                p21[1] = 0.0;
                p21[2] = (4*e_1_2B[3]*e_1_2B[4])/3 + (4*e_1_2B[5]*e_1_2B[6])/3;
                p21[3] = 0.0;
                p21[4] = -e_B[3]/3;
                p21[5] = 0.0;
                p21[6] = (4*e_1_2B[0]*e_1_2B[3])/3;
                p21[7] = 0.0;
                p21[8] = 0.0;

                p22[0] = (4*e_1_2B[4]*e_1_2B[4])/3 - e_B[4]/3 + (4*e_1_2B[5]*e_1_2B[7])/3;
                p22[1] = 0.0;
                p22[2] = 0.0;
                p22[3] = 0.0;
                p22[4] = (4*e_1_2B[1]*e_1_2B[3])/3;
                p22[5] = 0.0;
                p22[6] = 0.0;
                p22[7] = 0.0;
                p22[8] = 0.0;

                p23[0] = (4*e_1_2B[4]*e_1_2B[5])/3 - e_B[5]/3 + (4*e_1_2B[5]*e_1_2B[8])/3;
                p23[1] = 0.0;
                p23[2] = 0.0;
                p23[3] = 0.0;
                p23[4] = (4*e_1_2B[2]*e_1_2B[3])/3;
                p23[5] = 0.0;
                p23[6] = 0.0;
                p23[7] = 0.0;
                p23[8] = 0.0;

                p31[0] = 0.0;
                p31[1] = 0.0;
                p31[2] = (4*e_1_2B[3]*e_1_2B[7])/3 + (4*e_1_2B[6]*e_1_2B[8])/3;
                p31[3] = 0.0;
                p31[4] = -e_B[6]/3;
                p31[5] = 0.0;
                p31[6] = (4*e_1_2B[0]*e_1_2B[6])/3;
                p31[7] = 0.0;
                p31[8] = 0.0;

                p32[0] = (4*e_1_2B[4]*e_1_2B[7])/3 - e_B[7]/3 + (4*e_1_2B[7]*e_1_2B[8])/3;
                p32[1] = 0.0;
                p32[2] = 0.0;
                p32[3] = 0.0;
                p32[4] = (4*e_1_2B[1]*e_1_2B[6])/3;
                p32[5] = 0.0;
                p32[6] = 0.0;
                p32[7] = 0.0;
                p32[8] = 0.0;

                p33[0] = (4*e_1_2B[8]*e_1_2B[8])/3 - e_B[8]/3 + (4*e_1_2B[5]*e_1_2B[7])/3;
                p33[1] = 0.0;
                p33[2] = 0.0;
                p33[3] = 0.0;
                p33[4] = (4*e_1_2B[2]*e_1_2B[6])/3;
                p33[5] = 0.0;
                p33[6] = 0.0;
                p33[7] = 0.0;
                p33[8] = 0.0;

                p11 += 9;
                p12 += 9;
                p13 += 9;
                p21 += 9;
                p22 += 9;
                p23 += 9;
                p31 += 9;
                p32 += 9;
                p33 += 9;
            }
            break;

            case manakov_discretization_2SPLIT4B:
            e_1_2B = &e_Bstorage[0];    // exp(Bh/2)
            e_1_4B = &e_Bstorage[9];    // exp(Bh/4)

            for (i=D_eff; i-->0;) { 
                
                manakov_fscatter_zero_freq_scatter_matrix(e_1_2B, eps_t/2, q1[i], q2[i], kappa);
                manakov_fscatter_zero_freq_scatter_matrix(e_1_4B, eps_t/4, q1[i], q2[i], kappa);

                p11[0] = (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[3])/3 - (e_1_2B[2]*e_1_2B[6])/3 - (e_1_2B[1]*e_1_2B[3])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[6])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[3])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[6])/3;       // z^4
                p11[1] = 0.0;       // z^3
                p11[2] = (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[3])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[1])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[6])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[2])/3;       // z^2
                p11[3] = 0.0;       // z^1
                p11[4] = - e_1_2B[0]*e_1_2B[0]/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[0])/3;       // z^0

                p12[0] = (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[4])/3 - (e_1_2B[2]*e_1_2B[7])/3 - (e_1_2B[1]*e_1_2B[4])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[7])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[4])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[7])/3;
                p12[1] = 0.0;
                p12[2] = (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[1])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[1])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[4])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[7])/3;
                p12[3] = 0.0;
                p12[4] = (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[1])/3 - (e_1_2B[0]*e_1_2B[1])/3;

                p13[0] = (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[5])/3 - (e_1_2B[2]*e_1_2B[8])/3 - (e_1_2B[1]*e_1_2B[5])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[8])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[5])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[8])/3;
                p13[1] = 0.0;
                p13[2] = (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[2])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[2])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[5])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[8])/3;
                p13[3] = 0.0;
                p13[4] = (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[2])/3 - (e_1_2B[0]*e_1_2B[2])/3;

                p21[0] = (4*e_1_2B[4]*e_1_4B[3]*e_1_4B[4])/3 - (e_1_2B[5]*e_1_2B[6])/3 - (e_1_2B[3]*e_1_2B[4])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[6])/3 + (4*e_1_2B[7]*e_1_4B[3]*e_1_4B[5])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[6])/3;
                p21[1] = 0.0;
                p21[2] = (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[3])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[3])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[4])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[5])/3;
                p21[3] = 0.0;
                p21[4] = (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[3])/3 - (e_1_2B[0]*e_1_2B[3])/3;

                p22[0] = (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[4])/3 - e_1_2B[4]*e_1_2B[4]/3 - (e_1_2B[5]*e_1_2B[7])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[7])/3 + (4*e_1_2B[7]*e_1_4B[4]*e_1_4B[5])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[7])/3;
                p22[1] = 0.0;
                p22[2] = (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[4])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[4])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[7])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[5])/3;
                p22[3] = 0.0;
                p22[4] = (4*e_1_2B[0]*e_1_4B[1]*e_1_4B[3])/3 - (e_1_2B[1]*e_1_2B[3])/3;

                p23[0] = (4*e_1_2B[7]*e_1_4B[5]*e_1_4B[5])/3 - (e_1_2B[4]*e_1_2B[5])/3 - (e_1_2B[5]*e_1_2B[8])/3 + (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[5])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[8])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[8])/3;
                p23[1] = 0.0;
                p23[2] = (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[5])/3 + (4*e_1_2B[3]*e_1_4B[2]*e_1_4B[4])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[8])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[5])/3;
                p23[3] = 0.0;
                p23[4] = (4*e_1_2B[0]*e_1_4B[2]*e_1_4B[3])/3 - (e_1_2B[2]*e_1_2B[3])/3;

                p31[0] = (4*e_1_2B[4]*e_1_4B[3]*e_1_4B[7])/3 - (e_1_2B[6]*e_1_2B[8])/3 - (e_1_2B[3]*e_1_2B[7])/3 + (4*e_1_2B[5]*e_1_4B[6]*e_1_4B[7])/3 + (4*e_1_2B[7]*e_1_4B[3]*e_1_4B[8])/3 + (4*e_1_2B[8]*e_1_4B[6]*e_1_4B[8])/3;
                p31[1] = 0.0;
                p31[2] = (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[6])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[6])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[7])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[8])/3;
                p31[3] = 0.0;
                p31[4] = (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[6])/3 - (e_1_2B[0]*e_1_2B[6])/3;

                p32[0] = (4*e_1_2B[5]*e_1_4B[7]*e_1_4B[7])/3 - (e_1_2B[4]*e_1_2B[7])/3 - (e_1_2B[7]*e_1_2B[8])/3 + (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[7])/3 + (4*e_1_2B[7]*e_1_4B[4]*e_1_4B[8])/3 + (4*e_1_2B[8]*e_1_4B[7]*e_1_4B[8])/3;
                p32[1] = 0.0;
                p32[2] = (4*e_1_2B[1]*e_1_4B[4]*e_1_4B[6])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[7])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[7])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[8])/3;
                p32[3] = 0.0;
                p32[4] = (4*e_1_2B[0]*e_1_4B[1]*e_1_4B[6])/3 - (e_1_2B[1]*e_1_2B[6])/3;

                p33[0] = (4*e_1_2B[8]*e_1_4B[8]*e_1_4B[8])/3 - e_1_2B[8]*e_1_2B[8]/3 - (e_1_2B[5]*e_1_2B[7])/3 + (4*e_1_2B[4]*e_1_4B[5]*e_1_4B[7])/3 + (4*e_1_2B[5]*e_1_4B[7]*e_1_4B[8])/3 + (4*e_1_2B[7]*e_1_4B[5]*e_1_4B[8])/3;
                p33[1] = 0.0;
                p33[2] = (4*e_1_2B[1]*e_1_4B[5]*e_1_4B[6])/3 + (4*e_1_2B[3]*e_1_4B[2]*e_1_4B[7])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[8])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[8])/3;
                p33[3] = 0.0;
                p33[4] = (4*e_1_2B[0]*e_1_4B[2]*e_1_4B[6])/3 - (e_1_2B[2]*e_1_2B[6])/3;

                p11 += 5;
                p12 += 5;
                p13 += 5;
                p21 += 5;
                p22 += 5;
                p23 += 5;
                p31 += 5;
                p32 += 5;
                p33 += 5;
            }
            break;

            case manakov_discretization_2SPLIT6B:
            e_1_2B = &e_Bstorage[0];    // exp(Bh/2)
            e_1_3B = &e_Bstorage[9];    // exp(Bh/2)
            e_1_4B = &e_Bstorage[18];    // exp(Bh/2)
            e_1_6B = &e_Bstorage[27];    // exp(Bh/2)

            for (i = D_eff; i-->0;){
                manakov_fscatter_zero_freq_scatter_matrix(e_1_2B, eps_t/2, q1[i], q2[i], kappa);
                manakov_fscatter_zero_freq_scatter_matrix(e_1_3B, eps_t/3, q1[i], q2[i], kappa);
                manakov_fscatter_zero_freq_scatter_matrix(e_1_4B, eps_t/4, q1[i], q2[i], kappa);
                manakov_fscatter_zero_freq_scatter_matrix(e_1_6B, eps_t/6, q1[i], q2[i], kappa);

                p11[0] = (e_1_2B[1]*e_1_2B[3])/24 + (e_1_2B[2]*e_1_2B[6])/24 - (16*e_1_2B[4]*e_1_4B[1]*e_1_4B[3])/15 - (16*e_1_2B[5]*e_1_4B[1]*e_1_4B[6])/15 - (16*e_1_2B[7]*e_1_4B[2]*e_1_4B[3])/15 - (16*e_1_2B[8]*e_1_4B[2]*e_1_4B[6])/15 + (81*e_1_3B[4]*e_1_3B[4]*e_1_6B[1]*e_1_6B[3])/40 + (81*e_1_3B[8]*e_1_3B[8]*e_1_6B[2]*e_1_6B[6])/40 + (81*e_1_3B[4]*e_1_3B[5]*e_1_6B[1]*e_1_6B[6])/40 + (81*e_1_3B[4]*e_1_3B[7]*e_1_6B[2]*e_1_6B[3])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[1]*e_1_6B[3])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[2]*e_1_6B[6])/40 + (81*e_1_3B[5]*e_1_3B[8]*e_1_6B[1]*e_1_6B[6])/40 + (81*e_1_3B[7]*e_1_3B[8]*e_1_6B[2]*e_1_6B[3])/40;
                p11[1] = 0.0;
                p11[2] = 0.0;
                p11[3] = 0.0;
                p11[4] = (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[1]*e_1_6B[3])/40 + (81*e_1_3B[1]*e_1_3B[4]*e_1_6B[0]*e_1_6B[3])/40 + (81*e_1_3B[3]*e_1_3B[4]*e_1_6B[0]*e_1_6B[1])/40 + (81*e_1_3B[1]*e_1_3B[5]*e_1_6B[0]*e_1_6B[6])/40 + (81*e_1_3B[1]*e_1_3B[6]*e_1_6B[2]*e_1_6B[3])/40 + (81*e_1_3B[2]*e_1_3B[3]*e_1_6B[1]*e_1_6B[6])/40 + (81*e_1_3B[2]*e_1_3B[7]*e_1_6B[0]*e_1_6B[3])/40 + (81*e_1_3B[3]*e_1_3B[7]*e_1_6B[0]*e_1_6B[2])/40 + (81*e_1_3B[5]*e_1_3B[6]*e_1_6B[0]*e_1_6B[1])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[2]*e_1_6B[6])/40 + (81*e_1_3B[2]*e_1_3B[8]*e_1_6B[0]*e_1_6B[6])/40 + (81*e_1_3B[6]*e_1_3B[8]*e_1_6B[0]*e_1_6B[2])/40;
                p11[5] = 0.0;
                p11[6] = - (16*e_1_2B[1]*e_1_4B[0]*e_1_4B[3])/15 - (16*e_1_2B[3]*e_1_4B[0]*e_1_4B[1])/15 - (16*e_1_2B[2]*e_1_4B[0]*e_1_4B[6])/15 - (16*e_1_2B[6]*e_1_4B[0]*e_1_4B[2])/15;
                p11[7] = 0.0;
                p11[8] = (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[0]*e_1_6B[0])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[0]*e_1_6B[0])/40 + (81*e_1_3B[0]*e_1_3B[1]*e_1_6B[0]*e_1_6B[3])/40 + (81*e_1_3B[0]*e_1_3B[3]*e_1_6B[0]*e_1_6B[1])/40 + (81*e_1_3B[0]*e_1_3B[2]*e_1_6B[0]*e_1_6B[6])/40 + (81*e_1_3B[0]*e_1_3B[6]*e_1_6B[0]*e_1_6B[2])/40;
                p11[9] = 0.0;
                p11[10] = 0.0;
                p11[11] = 0.0;
                p11[12] = e_1_2B[0]*e_1_2B[0]/24 - (16*e_1_4B[0]*e_1_4B[0]*e_1_2B[0])/15 + (81*e_1_3B[0]*e_1_3B[0]*e_1_6B[0]*e_1_6B[0])/40;

                p12[0] = (e_1_2B[1]*e_1_2B[4])/24 + (e_1_2B[2]*e_1_2B[7])/24 - (16*e_1_2B[4]*e_1_4B[1]*e_1_4B[4])/15 - (16*e_1_2B[5]*e_1_4B[1]*e_1_4B[7])/15 - (16*e_1_2B[7]*e_1_4B[2]*e_1_4B[4])/15 - (16*e_1_2B[8]*e_1_4B[2]*e_1_4B[7])/15 + (81*e_1_3B[4]*e_1_3B[4]*e_1_6B[1]*e_1_6B[4])/40 + (81*e_1_3B[8]*e_1_3B[8]*e_1_6B[2]*e_1_6B[7])/40 + (81*e_1_3B[4]*e_1_3B[5]*e_1_6B[1]*e_1_6B[7])/40 + (81*e_1_3B[4]*e_1_3B[7]*e_1_6B[2]*e_1_6B[4])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[1]*e_1_6B[4])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[2]*e_1_6B[7])/40 + (81*e_1_3B[5]*e_1_3B[8]*e_1_6B[1]*e_1_6B[7])/40 + (81*e_1_3B[7]*e_1_3B[8]*e_1_6B[2]*e_1_6B[4])/40;
                p12[1] = 0.0;
                p12[2] = 0.0;
                p12[3] = 0.0;
                p12[4] = (81*e_1_3B[3]*e_1_3B[4]*e_1_6B[1]*e_1_6B[1])/40 + (81*e_1_3B[5]*e_1_3B[6]*e_1_6B[1]*e_1_6B[1])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[1]*e_1_6B[4])/40 + (81*e_1_3B[1]*e_1_3B[4]*e_1_6B[0]*e_1_6B[4])/40 + (81*e_1_3B[1]*e_1_3B[5]*e_1_6B[0]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[6]*e_1_6B[2]*e_1_6B[4])/40 + (81*e_1_3B[2]*e_1_3B[3]*e_1_6B[1]*e_1_6B[7])/40 + (81*e_1_3B[2]*e_1_3B[7]*e_1_6B[0]*e_1_6B[4])/40 + (81*e_1_3B[3]*e_1_3B[7]*e_1_6B[1]*e_1_6B[2])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[2]*e_1_6B[7])/40 + (81*e_1_3B[2]*e_1_3B[8]*e_1_6B[0]*e_1_6B[7])/40 + (81*e_1_3B[6]*e_1_3B[8]*e_1_6B[1]*e_1_6B[2])/40;
                p12[5] = 0.0;
                p12[6] = - (16*e_1_2B[3]*e_1_4B[1]*e_1_4B[1])/15 - (16*e_1_2B[1]*e_1_4B[0]*e_1_4B[4])/15 - (16*e_1_2B[2]*e_1_4B[0]*e_1_4B[7])/15 - (16*e_1_2B[6]*e_1_4B[1]*e_1_4B[2])/15;
                p12[7] = 0.0;
                p12[8] = (81*e_1_3B[0]*e_1_3B[3]*e_1_6B[1]*e_1_6B[1])/40 + (81*e_1_3B[0]*e_1_3B[1]*e_1_6B[0]*e_1_6B[4])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[0]*e_1_6B[1])/40 + (81*e_1_3B[0]*e_1_3B[2]*e_1_6B[0]*e_1_6B[7])/40 + (81*e_1_3B[0]*e_1_3B[6]*e_1_6B[1]*e_1_6B[2])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[0]*e_1_6B[1])/40;
                p12[9] = 0.0;
                p12[10] = 0.0;
                p12[11] = 0.0;
                p12[12] = (81*e_1_6B[0]*e_1_6B[1]*e_1_3B[0]*e_1_3B[0])/40 + (e_1_2B[0]*e_1_2B[1])/24 - (16*e_1_2B[0]*e_1_4B[0]*e_1_4B[1])/15;

                p13[0] = (e_1_2B[1]*e_1_2B[5])/24 + (e_1_2B[2]*e_1_2B[8])/24 - (16*e_1_2B[4]*e_1_4B[1]*e_1_4B[5])/15 - (16*e_1_2B[5]*e_1_4B[1]*e_1_4B[8])/15 - (16*e_1_2B[7]*e_1_4B[2]*e_1_4B[5])/15 - (16*e_1_2B[8]*e_1_4B[2]*e_1_4B[8])/15 + (81*e_1_3B[4]*e_1_3B[4]*e_1_6B[1]*e_1_6B[5])/40 + (81*e_1_3B[8]*e_1_3B[8]*e_1_6B[2]*e_1_6B[8])/40 + (81*e_1_3B[4]*e_1_3B[5]*e_1_6B[1]*e_1_6B[8])/40 + (81*e_1_3B[4]*e_1_3B[7]*e_1_6B[2]*e_1_6B[5])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[1]*e_1_6B[5])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[2]*e_1_6B[8])/40 + (81*e_1_3B[5]*e_1_3B[8]*e_1_6B[1]*e_1_6B[8])/40 + (81*e_1_3B[7]*e_1_3B[8]*e_1_6B[2]*e_1_6B[5])/40;
                p13[1] = 0.0;
                p13[2] = 0.0;
                p13[3] = 0.0;
                p13[4] = (81*e_1_3B[3]*e_1_3B[7]*e_1_6B[2]*e_1_6B[2])/40 + (81*e_1_3B[6]*e_1_3B[8]*e_1_6B[2]*e_1_6B[2])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[1]*e_1_6B[5])/40 + (81*e_1_3B[1]*e_1_3B[4]*e_1_6B[0]*e_1_6B[5])/40 + (81*e_1_3B[3]*e_1_3B[4]*e_1_6B[1]*e_1_6B[2])/40 + (81*e_1_3B[1]*e_1_3B[5]*e_1_6B[0]*e_1_6B[8])/40 + (81*e_1_3B[1]*e_1_3B[6]*e_1_6B[2]*e_1_6B[5])/40 + (81*e_1_3B[2]*e_1_3B[3]*e_1_6B[1]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[7]*e_1_6B[0]*e_1_6B[5])/40 + (81*e_1_3B[5]*e_1_3B[6]*e_1_6B[1]*e_1_6B[2])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[2]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[8]*e_1_6B[0]*e_1_6B[8])/40;
                p13[5] = 0.0;
                p13[6] = - (16*e_1_2B[6]*e_1_4B[2]*e_1_4B[2])/15 - (16*e_1_2B[1]*e_1_4B[0]*e_1_4B[5])/15 - (16*e_1_2B[3]*e_1_4B[1]*e_1_4B[2])/15 - (16*e_1_2B[2]*e_1_4B[0]*e_1_4B[8])/15;
                p13[7] = 0.0;
                p13[8] = (81*e_1_3B[0]*e_1_3B[6]*e_1_6B[2]*e_1_6B[2])/40 + (81*e_1_3B[0]*e_1_3B[1]*e_1_6B[0]*e_1_6B[5])/40 + (81*e_1_3B[0]*e_1_3B[3]*e_1_6B[1]*e_1_6B[2])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[0]*e_1_6B[2])/40 + (81*e_1_3B[0]*e_1_3B[2]*e_1_6B[0]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[0]*e_1_6B[2])/40;
                p13[9] = 0.0;
                p13[10] = 0.0;
                p13[11] = 0.0;
                p13[12] = (81*e_1_6B[0]*e_1_6B[2]*e_1_3B[0]*e_1_3B[0])/40 + (e_1_2B[0]*e_1_2B[2])/24 - (16*e_1_2B[0]*e_1_4B[0]*e_1_4B[2])/15;

                p21[0] = (e_1_2B[3]*e_1_2B[4])/24 + (e_1_2B[5]*e_1_2B[6])/24 - (16*e_1_2B[4]*e_1_4B[3]*e_1_4B[4])/15 - (16*e_1_2B[5]*e_1_4B[4]*e_1_4B[6])/15 - (16*e_1_2B[7]*e_1_4B[3]*e_1_4B[5])/15 - (16*e_1_2B[8]*e_1_4B[5]*e_1_4B[6])/15 + (81*e_1_3B[4]*e_1_3B[4]*e_1_6B[3]*e_1_6B[4])/40 + (81*e_1_3B[8]*e_1_3B[8]*e_1_6B[5]*e_1_6B[6])/40 + (81*e_1_3B[4]*e_1_3B[5]*e_1_6B[4]*e_1_6B[6])/40 + (81*e_1_3B[4]*e_1_3B[7]*e_1_6B[3]*e_1_6B[5])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[3]*e_1_6B[4])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[5]*e_1_6B[6])/40 + (81*e_1_3B[5]*e_1_3B[8]*e_1_6B[4]*e_1_6B[6])/40 + (81*e_1_3B[7]*e_1_3B[8]*e_1_6B[3]*e_1_6B[5])/40;
                p21[1] = 0.0;
                p21[2] = 0.0;
                p21[3] = 0.0;
                p21[4] = (81*e_1_3B[1]*e_1_3B[4]*e_1_6B[3]*e_1_6B[3])/40 + (81*e_1_3B[2]*e_1_3B[7]*e_1_6B[3]*e_1_6B[3])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[3]*e_1_6B[4])/40 + (81*e_1_3B[3]*e_1_3B[4]*e_1_6B[0]*e_1_6B[4])/40 + (81*e_1_3B[1]*e_1_3B[5]*e_1_6B[3]*e_1_6B[6])/40 + (81*e_1_3B[1]*e_1_3B[6]*e_1_6B[3]*e_1_6B[5])/40 + (81*e_1_3B[2]*e_1_3B[3]*e_1_6B[4]*e_1_6B[6])/40 + (81*e_1_3B[3]*e_1_3B[7]*e_1_6B[0]*e_1_6B[5])/40 + (81*e_1_3B[5]*e_1_3B[6]*e_1_6B[0]*e_1_6B[4])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[5]*e_1_6B[6])/40 + (81*e_1_3B[2]*e_1_3B[8]*e_1_6B[3]*e_1_6B[6])/40 + (81*e_1_3B[6]*e_1_3B[8]*e_1_6B[0]*e_1_6B[5])/40;
                p21[5] = 0.0;
                p21[6] = - (16*e_1_2B[1]*e_1_4B[3]*e_1_4B[3])/15 - (16*e_1_2B[3]*e_1_4B[0]*e_1_4B[4])/15 - (16*e_1_2B[2]*e_1_4B[3]*e_1_4B[6])/15 - (16*e_1_2B[6]*e_1_4B[0]*e_1_4B[5])/15;
                p21[7] = 0.0;
                p21[8] = (81*e_1_3B[0]*e_1_3B[1]*e_1_6B[3]*e_1_6B[3])/40 + (81*e_1_3B[0]*e_1_3B[3]*e_1_6B[0]*e_1_6B[4])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[0]*e_1_6B[3])/40 + (81*e_1_3B[0]*e_1_3B[2]*e_1_6B[3]*e_1_6B[6])/40 + (81*e_1_3B[0]*e_1_3B[6]*e_1_6B[0]*e_1_6B[5])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[0]*e_1_6B[3])/40;
                p21[9] = 0.0;
                p21[10] = 0.0;
                p21[11] = 0.0;
                p21[12] = (81*e_1_6B[0]*e_1_6B[3]*e_1_3B[0]*e_1_3B[0])/40 + (e_1_2B[0]*e_1_2B[3])/24 - (16*e_1_2B[0]*e_1_4B[0]*e_1_4B[3])/15;

                p22[0] = e_1_2B[4]*e_1_2B[4]/24 - (16*e_1_2B[4]*e_1_4B[4]*e_1_4B[4])/15 + (81*e_1_3B[4]*e_1_3B[4]*e_1_6B[4]*e_1_6B[4])/40 + (e_1_2B[5]*e_1_2B[7])/24 - (16*e_1_2B[5]*e_1_4B[4]*e_1_4B[7])/15 - (16*e_1_2B[7]*e_1_4B[4]*e_1_4B[5])/15 - (16*e_1_2B[8]*e_1_4B[5]*e_1_4B[7])/15 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[4]*e_1_6B[4])/40 + (81*e_1_3B[8]*e_1_3B[8]*e_1_6B[5]*e_1_6B[7])/40 + (81*e_1_3B[4]*e_1_3B[5]*e_1_6B[4]*e_1_6B[7])/40 + (81*e_1_3B[4]*e_1_3B[7]*e_1_6B[4]*e_1_6B[5])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[5]*e_1_6B[7])/40 + (81*e_1_3B[5]*e_1_3B[8]*e_1_6B[4]*e_1_6B[7])/40 + (81*e_1_3B[7]*e_1_3B[8]*e_1_6B[4]*e_1_6B[5])/40;
                p22[1] = 0.0;
                p22[2] = 0.0;
                p22[3] = 0.0;
                p22[4] = (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[4]*e_1_6B[4])/40 + (81*e_1_3B[1]*e_1_3B[4]*e_1_6B[3]*e_1_6B[4])/40 + (81*e_1_3B[3]*e_1_3B[4]*e_1_6B[1]*e_1_6B[4])/40 + (81*e_1_3B[1]*e_1_3B[5]*e_1_6B[3]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[6]*e_1_6B[4]*e_1_6B[5])/40 + (81*e_1_3B[2]*e_1_3B[3]*e_1_6B[4]*e_1_6B[7])/40 + (81*e_1_3B[2]*e_1_3B[7]*e_1_6B[3]*e_1_6B[4])/40 + (81*e_1_3B[3]*e_1_3B[7]*e_1_6B[1]*e_1_6B[5])/40 + (81*e_1_3B[5]*e_1_3B[6]*e_1_6B[1]*e_1_6B[4])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[5]*e_1_6B[7])/40 + (81*e_1_3B[2]*e_1_3B[8]*e_1_6B[3]*e_1_6B[7])/40 + (81*e_1_3B[6]*e_1_3B[8]*e_1_6B[1]*e_1_6B[5])/40;
                p22[5] = 0.0;
                p22[6] = - (16*e_1_2B[1]*e_1_4B[3]*e_1_4B[4])/15 - (16*e_1_2B[3]*e_1_4B[1]*e_1_4B[4])/15 - (16*e_1_2B[2]*e_1_4B[3]*e_1_4B[7])/15 - (16*e_1_2B[6]*e_1_4B[1]*e_1_4B[5])/15;
                p22[7] = 0.0;
                p22[8] = (81*e_1_3B[0]*e_1_3B[1]*e_1_6B[3]*e_1_6B[4])/40 + (81*e_1_3B[0]*e_1_3B[3]*e_1_6B[1]*e_1_6B[4])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[1]*e_1_6B[3])/40 + (81*e_1_3B[0]*e_1_3B[2]*e_1_6B[3]*e_1_6B[7])/40 + (81*e_1_3B[0]*e_1_3B[6]*e_1_6B[1]*e_1_6B[5])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[1]*e_1_6B[3])/40;
                p22[9] = 0.0;
                p22[10] = 0.0;
                p22[11] = 0.0;
                p22[12] = (81*e_1_6B[1]*e_1_6B[3]*e_1_3B[0]*e_1_3B[0])/40 + (e_1_2B[1]*e_1_2B[3])/24 - (16*e_1_2B[0]*e_1_4B[1]*e_1_4B[3])/15;

                p23[0] = (e_1_2B[4]*e_1_2B[5])/24 - (16*e_1_2B[7]*e_1_4B[5]*e_1_4B[5])/15 + (e_1_2B[5]*e_1_2B[8])/24 - (16*e_1_2B[4]*e_1_4B[4]*e_1_4B[5])/15 - (16*e_1_2B[5]*e_1_4B[4]*e_1_4B[8])/15 - (16*e_1_2B[8]*e_1_4B[5]*e_1_4B[8])/15 + (81*e_1_3B[4]*e_1_3B[7]*e_1_6B[5]*e_1_6B[5])/40 + (81*e_1_3B[7]*e_1_3B[8]*e_1_6B[5]*e_1_6B[5])/40 + (81*e_1_3B[4]*e_1_3B[4]*e_1_6B[4]*e_1_6B[5])/40 + (81*e_1_3B[8]*e_1_3B[8]*e_1_6B[5]*e_1_6B[8])/40 + (81*e_1_3B[4]*e_1_3B[5]*e_1_6B[4]*e_1_6B[8])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[4]*e_1_6B[5])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[5]*e_1_6B[8])/40 + (81*e_1_3B[5]*e_1_3B[8]*e_1_6B[4]*e_1_6B[8])/40;
                p23[1] = 0.0;
                p23[2] = 0.0;
                p23[3] = 0.0;
                p23[4] = (81*e_1_3B[1]*e_1_3B[6]*e_1_6B[5]*e_1_6B[5])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[4]*e_1_6B[5])/40 + (81*e_1_3B[1]*e_1_3B[4]*e_1_6B[3]*e_1_6B[5])/40 + (81*e_1_3B[3]*e_1_3B[4]*e_1_6B[2]*e_1_6B[4])/40 + (81*e_1_3B[1]*e_1_3B[5]*e_1_6B[3]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[3]*e_1_6B[4]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[7]*e_1_6B[3]*e_1_6B[5])/40 + (81*e_1_3B[3]*e_1_3B[7]*e_1_6B[2]*e_1_6B[5])/40 + (81*e_1_3B[5]*e_1_3B[6]*e_1_6B[2]*e_1_6B[4])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[5]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[8]*e_1_6B[3]*e_1_6B[8])/40 + (81*e_1_3B[6]*e_1_3B[8]*e_1_6B[2]*e_1_6B[5])/40;
                p23[5] = 0.0;
                p23[6] = - (16*e_1_2B[1]*e_1_4B[3]*e_1_4B[5])/15 - (16*e_1_2B[3]*e_1_4B[2]*e_1_4B[4])/15 - (16*e_1_2B[2]*e_1_4B[3]*e_1_4B[8])/15 - (16*e_1_2B[6]*e_1_4B[2]*e_1_4B[5])/15;
                p23[7] = 0.0;
                p23[8] = (81*e_1_3B[0]*e_1_3B[1]*e_1_6B[3]*e_1_6B[5])/40 + (81*e_1_3B[0]*e_1_3B[3]*e_1_6B[2]*e_1_6B[4])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[2]*e_1_6B[3])/40 + (81*e_1_3B[0]*e_1_3B[2]*e_1_6B[3]*e_1_6B[8])/40 + (81*e_1_3B[0]*e_1_3B[6]*e_1_6B[2]*e_1_6B[5])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[2]*e_1_6B[3])/40;
                p23[9] = 0.0;
                p23[10] = 0.0;
                p23[11] = 0.0;
                p23[12] = (81*e_1_6B[2]*e_1_6B[3]*e_1_3B[0]*e_1_3B[0])/40 + (e_1_2B[2]*e_1_2B[3])/24 - (16*e_1_2B[0]*e_1_4B[2]*e_1_4B[3])/15;

                p31[0] = (e_1_2B[3]*e_1_2B[7])/24 + (e_1_2B[6]*e_1_2B[8])/24 - (16*e_1_2B[4]*e_1_4B[3]*e_1_4B[7])/15 - (16*e_1_2B[5]*e_1_4B[6]*e_1_4B[7])/15 - (16*e_1_2B[7]*e_1_4B[3]*e_1_4B[8])/15 - (16*e_1_2B[8]*e_1_4B[6]*e_1_4B[8])/15 + (81*e_1_3B[4]*e_1_3B[4]*e_1_6B[3]*e_1_6B[7])/40 + (81*e_1_3B[8]*e_1_3B[8]*e_1_6B[6]*e_1_6B[8])/40 + (81*e_1_3B[4]*e_1_3B[5]*e_1_6B[6]*e_1_6B[7])/40 + (81*e_1_3B[4]*e_1_3B[7]*e_1_6B[3]*e_1_6B[8])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[3]*e_1_6B[7])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[6]*e_1_6B[8])/40 + (81*e_1_3B[5]*e_1_3B[8]*e_1_6B[6]*e_1_6B[7])/40 + (81*e_1_3B[7]*e_1_3B[8]*e_1_6B[3]*e_1_6B[8])/40;
                p31[1] = 0.0;
                p31[2] = 0.0;
                p31[3] = 0.0;
                p31[4] = (81*e_1_3B[1]*e_1_3B[5]*e_1_6B[6]*e_1_6B[6])/40 + (81*e_1_3B[2]*e_1_3B[8]*e_1_6B[6]*e_1_6B[6])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[3]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[4]*e_1_6B[3]*e_1_6B[6])/40 + (81*e_1_3B[3]*e_1_3B[4]*e_1_6B[0]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[6]*e_1_6B[3]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[3]*e_1_6B[6]*e_1_6B[7])/40 + (81*e_1_3B[2]*e_1_3B[7]*e_1_6B[3]*e_1_6B[6])/40 + (81*e_1_3B[3]*e_1_3B[7]*e_1_6B[0]*e_1_6B[8])/40 + (81*e_1_3B[5]*e_1_3B[6]*e_1_6B[0]*e_1_6B[7])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[6]*e_1_6B[8])/40 + (81*e_1_3B[6]*e_1_3B[8]*e_1_6B[0]*e_1_6B[8])/40;
                p31[5] = 0.0;
                p31[6] = - (16*e_1_2B[2]*e_1_4B[6]*e_1_4B[6])/15 - (16*e_1_2B[1]*e_1_4B[3]*e_1_4B[6])/15 - (16*e_1_2B[3]*e_1_4B[0]*e_1_4B[7])/15 - (16*e_1_2B[6]*e_1_4B[0]*e_1_4B[8])/15;
                p31[7] = 0.0;
                p31[8] = (81*e_1_3B[0]*e_1_3B[2]*e_1_6B[6]*e_1_6B[6])/40 + (81*e_1_3B[0]*e_1_3B[1]*e_1_6B[3]*e_1_6B[6])/40 + (81*e_1_3B[0]*e_1_3B[3]*e_1_6B[0]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[0]*e_1_6B[6])/40 + (81*e_1_3B[0]*e_1_3B[6]*e_1_6B[0]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[0]*e_1_6B[6])/40;
                p31[9] = 0.0;
                p31[10] = 0.0;
                p31[11] = 0.0;
                p31[12] = (81*e_1_6B[0]*e_1_6B[6]*e_1_3B[0]*e_1_3B[0])/40 + (e_1_2B[0]*e_1_2B[6])/24 - (16*e_1_2B[0]*e_1_4B[0]*e_1_4B[6])/15;

                p32[0] = (e_1_2B[4]*e_1_2B[7])/24 - (16*e_1_2B[5]*e_1_4B[7]*e_1_4B[7])/15 + (e_1_2B[7]*e_1_2B[8])/24 - (16*e_1_2B[4]*e_1_4B[4]*e_1_4B[7])/15 - (16*e_1_2B[7]*e_1_4B[4]*e_1_4B[8])/15 - (16*e_1_2B[8]*e_1_4B[7]*e_1_4B[8])/15 + (81*e_1_3B[4]*e_1_3B[5]*e_1_6B[7]*e_1_6B[7])/40 + (81*e_1_3B[5]*e_1_3B[8]*e_1_6B[7]*e_1_6B[7])/40 + (81*e_1_3B[4]*e_1_3B[4]*e_1_6B[4]*e_1_6B[7])/40 + (81*e_1_3B[8]*e_1_3B[8]*e_1_6B[7]*e_1_6B[8])/40 + (81*e_1_3B[4]*e_1_3B[7]*e_1_6B[4]*e_1_6B[8])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[4]*e_1_6B[7])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[7]*e_1_6B[8])/40 + (81*e_1_3B[7]*e_1_3B[8]*e_1_6B[4]*e_1_6B[8])/40;
                p32[1] = 0.0;
                p32[2] = 0.0;
                p32[3] = 0.0;
                p32[4] = (81*e_1_3B[2]*e_1_3B[3]*e_1_6B[7]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[4]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[4]*e_1_6B[4]*e_1_6B[6])/40 + (81*e_1_3B[3]*e_1_3B[4]*e_1_6B[1]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[5]*e_1_6B[6]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[6]*e_1_6B[4]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[7]*e_1_6B[4]*e_1_6B[6])/40 + (81*e_1_3B[3]*e_1_3B[7]*e_1_6B[1]*e_1_6B[8])/40 + (81*e_1_3B[5]*e_1_3B[6]*e_1_6B[1]*e_1_6B[7])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[7]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[8]*e_1_6B[6]*e_1_6B[7])/40 + (81*e_1_3B[6]*e_1_3B[8]*e_1_6B[1]*e_1_6B[8])/40;
                p32[5] = 0.0;
                p32[6] = - (16*e_1_2B[1]*e_1_4B[4]*e_1_4B[6])/15 - (16*e_1_2B[3]*e_1_4B[1]*e_1_4B[7])/15 - (16*e_1_2B[2]*e_1_4B[6]*e_1_4B[7])/15 - (16*e_1_2B[6]*e_1_4B[1]*e_1_4B[8])/15;
                p32[7] = 0.0;
                p32[8] = (81*e_1_3B[0]*e_1_3B[1]*e_1_6B[4]*e_1_6B[6])/40 + (81*e_1_3B[0]*e_1_3B[3]*e_1_6B[1]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[1]*e_1_6B[6])/40 + (81*e_1_3B[0]*e_1_3B[2]*e_1_6B[6]*e_1_6B[7])/40 + (81*e_1_3B[0]*e_1_3B[6]*e_1_6B[1]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[1]*e_1_6B[6])/40;
                p32[9] = 0.0;
                p32[10] = 0.0;
                p32[11] = 0.0;
                p32[12] = (81*e_1_6B[1]*e_1_6B[6]*e_1_3B[0]*e_1_3B[0])/40 + (e_1_2B[1]*e_1_2B[6])/24 - (16*e_1_2B[0]*e_1_4B[1]*e_1_4B[6])/15;

                p33[0] = e_1_2B[8]*e_1_2B[8]/24 - (16*e_1_2B[8]*e_1_4B[8]*e_1_4B[8])/15 + (81*e_1_3B[8]*e_1_3B[8]*e_1_6B[8]*e_1_6B[8])/40 + (e_1_2B[5]*e_1_2B[7])/24 - (16*e_1_2B[4]*e_1_4B[5]*e_1_4B[7])/15 - (16*e_1_2B[5]*e_1_4B[7]*e_1_4B[8])/15 - (16*e_1_2B[7]*e_1_4B[5]*e_1_4B[8])/15 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[8]*e_1_6B[8])/40 + (81*e_1_3B[4]*e_1_3B[4]*e_1_6B[5]*e_1_6B[7])/40 + (81*e_1_3B[4]*e_1_3B[5]*e_1_6B[7]*e_1_6B[8])/40 + (81*e_1_3B[4]*e_1_3B[7]*e_1_6B[5]*e_1_6B[8])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[5]*e_1_6B[7])/40 + (81*e_1_3B[5]*e_1_3B[8]*e_1_6B[7]*e_1_6B[8])/40 + (81*e_1_3B[7]*e_1_3B[8]*e_1_6B[5]*e_1_6B[8])/40;
                p33[1] = 0.0;
                p33[2] = 0.0;
                p33[3] = 0.0;
                p33[4] = (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[8]*e_1_6B[8])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[5]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[4]*e_1_6B[5]*e_1_6B[6])/40 + (81*e_1_3B[3]*e_1_3B[4]*e_1_6B[2]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[5]*e_1_6B[6]*e_1_6B[8])/40 + (81*e_1_3B[1]*e_1_3B[6]*e_1_6B[5]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[3]*e_1_6B[7]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[7]*e_1_6B[5]*e_1_6B[6])/40 + (81*e_1_3B[3]*e_1_3B[7]*e_1_6B[2]*e_1_6B[8])/40 + (81*e_1_3B[5]*e_1_3B[6]*e_1_6B[2]*e_1_6B[7])/40 + (81*e_1_3B[2]*e_1_3B[8]*e_1_6B[6]*e_1_6B[8])/40 + (81*e_1_3B[6]*e_1_3B[8]*e_1_6B[2]*e_1_6B[8])/40;
                p33[5] = 0.0;
                p33[6] = - (16*e_1_2B[1]*e_1_4B[5]*e_1_4B[6])/15 - (16*e_1_2B[3]*e_1_4B[2]*e_1_4B[7])/15 - (16*e_1_2B[2]*e_1_4B[6]*e_1_4B[8])/15 - (16*e_1_2B[6]*e_1_4B[2]*e_1_4B[8])/15;
                p33[7] = 0.0;
                p33[8] = (81*e_1_3B[0]*e_1_3B[1]*e_1_6B[5]*e_1_6B[6])/40 + (81*e_1_3B[0]*e_1_3B[3]*e_1_6B[2]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[2]*e_1_6B[6])/40 + (81*e_1_3B[0]*e_1_3B[2]*e_1_6B[6]*e_1_6B[8])/40 + (81*e_1_3B[0]*e_1_3B[6]*e_1_6B[2]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[2]*e_1_6B[6])/40;
                p33[9] = 0.0;
                p33[10] = 0.0;
                p33[11] = 0.0;
                p33[12] = (81*e_1_6B[2]*e_1_6B[6]*e_1_3B[0]*e_1_3B[0])/40 + (e_1_2B[2]*e_1_2B[6])/24 - (16*e_1_2B[0]*e_1_4B[2]*e_1_4B[6])/15;

                p11 += 13;
                p12 += 13;
                p13 += 13;
                p21 += 13;
                p22 += 13;
                p23 += 13;
                p31 += 13;
                p32 += 13;
                p33 += 13;

            }
            break;

            case manakov_discretization_4SPLIT4A:
            e_1_2B = &e_Bstorage[0];    // exp(Bh/2)
            e_B = &e_Bstorage[9];    // exp(Bh)
            q1_c1 = malloc(D*sizeof(COMPLEX));
            q2_c1 = malloc(D*sizeof(COMPLEX));
            q1_c2 = malloc(D*sizeof(COMPLEX));
            q2_c2 = malloc(D*sizeof(COMPLEX));
            q1_eff = malloc(D_eff*sizeof(COMPLEX));
            q2_eff = malloc(D_eff*sizeof(COMPLEX));

            
            // Getting non-equidistant samples. qci has sample points at T0+(n+ci)*eps_t,
            // c1 = 0.5-sqrt(3)/6, c2 = 0.5+sqrt(3)/6
            // Getting new samples:
            misc_resample(D, eps_t, q1, (0.5-SQRT(3)/6)*eps_t, q1_c1);
            misc_resample(D, eps_t, q1, (0.5+SQRT(3)/6)*eps_t, q1_c2);
            misc_resample(D, eps_t, q2, (0.5-SQRT(3)/6)*eps_t, q2_c1);
            misc_resample(D, eps_t, q2, (0.5+SQRT(3)/6)*eps_t, q2_c2);

            for (i=0; i<D; i++){
                q1_eff[2*i] = a1*q1_c1[i]+a2*q1_c2[i];
                q2_eff[2*i] = a1*q2_c1[i]+a2*q2_c2[i];
                q1_eff[2*i+1] = a2*q1_c1[i]+a1*q1_c2[i];
                q2_eff[2*i+1] = a2*q2_c1[i]+a1*q2_c2[i];
            }// q1, q2 are passed as constant, so we need to declare new variables q1,2_eff so we can 
                // assign new values

            for (i=D_eff; i-->0;) { 
                
                manakov_fscatter_zero_freq_scatter_matrix(e_1_2B, eps_t/2, q1_eff[i], q2_eff[i], kappa);
                manakov_fscatter_zero_freq_scatter_matrix(e_B, eps_t, q1_eff[i], q2_eff[i], kappa);

                p11[0] = 0.0;   // z^8
                p11[1] = 0.0;   // z^7
                p11[2] = 0.0;   // z^6
                p11[3] = 0.0;   // z^5 
                p11[4] = (4*e_1_2B[1]*e_1_2B[3])/3 + (4*e_1_2B[2]*e_1_2B[6])/3;     // z^4
                p11[5] = 0.0;   // z^3
                p11[6] = 0.0;   // z^2
                p11[7] = 0.0;   // z^1
                p11[8] = (4*e_1_2B[0]*e_1_2B[0])/3 - e_B[0]/3; // z^0

                p12[0] = 0.0;
                p12[1] = 0.0;
                p12[2] = (4*e_1_2B[1]*e_1_2B[4])/3 + (4*e_1_2B[2]*e_1_2B[7])/3;
                p12[3] = 0.0;
                p12[4] = -e_B[1]/3;
                p12[5] = 0.0;
                p12[6] = (4*e_1_2B[0]*e_1_2B[1])/3;
                p12[7] = 0.0;
                p12[8] = 0.0;

                p13[0] = 0.0;
                p13[1] = 0.0;
                p13[2] = (4*e_1_2B[1]*e_1_2B[5])/3 + (4*e_1_2B[2]*e_1_2B[8])/3;
                p13[3] = 0.0;
                p13[4] = -e_B[2]/3;
                p13[5] = 0.0;
                p13[6] = (4*e_1_2B[0]*e_1_2B[2])/3;
                p13[7] = 0.0;
                p13[8] = 0.0;

                p21[0] = 0.0;
                p21[1] = 0.0;
                p21[2] = (4*e_1_2B[3]*e_1_2B[4])/3 + (4*e_1_2B[5]*e_1_2B[6])/3;
                p21[3] = 0.0;
                p21[4] = -e_B[3]/3;
                p21[5] = 0.0;
                p21[6] = (4*e_1_2B[0]*e_1_2B[3])/3;
                p21[7] = 0.0;
                p21[8] = 0.0;

                p22[0] = (4*e_1_2B[4]*e_1_2B[4])/3 - e_B[4]/3 + (4*e_1_2B[5]*e_1_2B[7])/3;
                p22[1] = 0.0;
                p22[2] = 0.0;
                p22[3] = 0.0;
                p22[4] = (4*e_1_2B[1]*e_1_2B[3])/3;
                p22[5] = 0.0;
                p22[6] = 0.0;
                p22[7] = 0.0;
                p22[8] = 0.0;

                p23[0] = (4*e_1_2B[4]*e_1_2B[5])/3 - e_B[5]/3 + (4*e_1_2B[5]*e_1_2B[8])/3;
                p23[1] = 0.0;
                p23[2] = 0.0;
                p23[3] = 0.0;
                p23[4] = (4*e_1_2B[2]*e_1_2B[3])/3;
                p23[5] = 0.0;
                p23[6] = 0.0;
                p23[7] = 0.0;
                p23[8] = 0.0;

                p31[0] = 0.0;
                p31[1] = 0.0;
                p31[2] = (4*e_1_2B[3]*e_1_2B[7])/3 + (4*e_1_2B[6]*e_1_2B[8])/3;
                p31[3] = 0.0;
                p31[4] = -e_B[6]/3;
                p31[5] = 0.0;
                p31[6] = (4*e_1_2B[0]*e_1_2B[6])/3;
                p31[7] = 0.0;
                p31[8] = 0.0;

                p32[0] = (4*e_1_2B[4]*e_1_2B[7])/3 - e_B[7]/3 + (4*e_1_2B[7]*e_1_2B[8])/3;
                p32[1] = 0.0;
                p32[2] = 0.0;
                p32[3] = 0.0;
                p32[4] = (4*e_1_2B[1]*e_1_2B[6])/3;
                p32[5] = 0.0;
                p32[6] = 0.0;
                p32[7] = 0.0;
                p32[8] = 0.0;

                p33[0] = (4*e_1_2B[8]*e_1_2B[8])/3 - e_B[8]/3 + (4*e_1_2B[5]*e_1_2B[7])/3;
                p33[1] = 0.0;
                p33[2] = 0.0;
                p33[3] = 0.0;
                p33[4] = (4*e_1_2B[2]*e_1_2B[6])/3;
                p33[5] = 0.0;
                p33[6] = 0.0;
                p33[7] = 0.0;
                p33[8] = 0.0;

                p11 += 9;
                p12 += 9;
                p13 += 9;
                p21 += 9;
                p22 += 9;
                p23 += 9;
                p31 += 9;
                p32 += 9;
                p33 += 9;
            }
            break;

         case manakov_discretization_4SPLIT4B:
            e_1_2B = &e_Bstorage[0];    // exp(Bh/2)
            e_1_4B = &e_Bstorage[9];    // exp(Bh/4)
            q1_c1 = malloc(D*sizeof(COMPLEX));
            q2_c1 = malloc(D*sizeof(COMPLEX));
            q1_c2 = malloc(D*sizeof(COMPLEX));
            q2_c2 = malloc(D*sizeof(COMPLEX));
            q1_eff = malloc(D_eff*sizeof(COMPLEX));
            q2_eff = malloc(D_eff*sizeof(COMPLEX));

            
            // Getting non-equidistant samples. qci has sample points at T0+(n+ci)*eps_t,
            // c1 = 0.5-sqrt(3)/6, c2 = 0.5+sqrt(3)/6
            // Getting new samples:
            misc_resample(D, eps_t, q1, (0.5-SQRT(3)/6)*eps_t, q1_c1);
            misc_resample(D, eps_t, q1, (0.5+SQRT(3)/6)*eps_t, q1_c2);
            misc_resample(D, eps_t, q2, (0.5-SQRT(3)/6)*eps_t, q2_c1);
            misc_resample(D, eps_t, q2, (0.5+SQRT(3)/6)*eps_t, q2_c2);

            for (i=0; i<D; i++){
                q1_eff[2*i] = a1*q1_c1[i]+a2*q1_c2[i];
                q2_eff[2*i] = a1*q2_c1[i]+a2*q2_c2[i];
                q1_eff[2*i+1] = a2*q1_c1[i]+a1*q1_c2[i];
                q2_eff[2*i+1] = a2*q2_c1[i]+a1*q2_c2[i];
            }// q1, q2 are passed as constant, so we need to declare new variables q1,2_eff so we can 
                // assign new values

            for (i=D_eff; i-->0;) { 
                
                manakov_fscatter_zero_freq_scatter_matrix(e_1_2B, eps_t/2, q1_eff[i], q2_eff[i], kappa);
                manakov_fscatter_zero_freq_scatter_matrix(e_1_4B, eps_t/4, q1_eff[i], q2_eff[i], kappa);

                p11[0] = (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[3])/3 - (e_1_2B[2]*e_1_2B[6])/3 - (e_1_2B[1]*e_1_2B[3])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[6])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[3])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[6])/3;       // z^4
                p11[1] = 0.0;       // z^3
                p11[2] = (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[3])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[1])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[6])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[2])/3;       // z^2
                p11[3] = 0.0;       // z^1
                p11[4] = - e_1_2B[0]*e_1_2B[0]/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[0])/3;       // z^0

                p12[0] = (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[4])/3 - (e_1_2B[2]*e_1_2B[7])/3 - (e_1_2B[1]*e_1_2B[4])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[7])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[4])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[7])/3;
                p12[1] = 0.0;
                p12[2] = (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[1])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[1])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[4])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[7])/3;
                p12[3] = 0.0;
                p12[4] = (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[1])/3 - (e_1_2B[0]*e_1_2B[1])/3;

                p13[0] = (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[5])/3 - (e_1_2B[2]*e_1_2B[8])/3 - (e_1_2B[1]*e_1_2B[5])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[8])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[5])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[8])/3;
                p13[1] = 0.0;
                p13[2] = (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[2])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[2])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[5])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[8])/3;
                p13[3] = 0.0;
                p13[4] = (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[2])/3 - (e_1_2B[0]*e_1_2B[2])/3;

                p21[0] = (4*e_1_2B[4]*e_1_4B[3]*e_1_4B[4])/3 - (e_1_2B[5]*e_1_2B[6])/3 - (e_1_2B[3]*e_1_2B[4])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[6])/3 + (4*e_1_2B[7]*e_1_4B[3]*e_1_4B[5])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[6])/3;
                p21[1] = 0.0;
                p21[2] = (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[3])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[3])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[4])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[5])/3;
                p21[3] = 0.0;
                p21[4] = (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[3])/3 - (e_1_2B[0]*e_1_2B[3])/3;

                p22[0] = (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[4])/3 - e_1_2B[4]*e_1_2B[4]/3 - (e_1_2B[5]*e_1_2B[7])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[7])/3 + (4*e_1_2B[7]*e_1_4B[4]*e_1_4B[5])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[7])/3;
                p22[1] = 0.0;
                p22[2] = (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[4])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[4])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[7])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[5])/3;
                p22[3] = 0.0;
                p22[4] = (4*e_1_2B[0]*e_1_4B[1]*e_1_4B[3])/3 - (e_1_2B[1]*e_1_2B[3])/3;

                p23[0] = (4*e_1_2B[7]*e_1_4B[5]*e_1_4B[5])/3 - (e_1_2B[4]*e_1_2B[5])/3 - (e_1_2B[5]*e_1_2B[8])/3 + (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[5])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[8])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[8])/3;
                p23[1] = 0.0;
                p23[2] = (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[5])/3 + (4*e_1_2B[3]*e_1_4B[2]*e_1_4B[4])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[8])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[5])/3;
                p23[3] = 0.0;
                p23[4] = (4*e_1_2B[0]*e_1_4B[2]*e_1_4B[3])/3 - (e_1_2B[2]*e_1_2B[3])/3;

                p31[0] = (4*e_1_2B[4]*e_1_4B[3]*e_1_4B[7])/3 - (e_1_2B[6]*e_1_2B[8])/3 - (e_1_2B[3]*e_1_2B[7])/3 + (4*e_1_2B[5]*e_1_4B[6]*e_1_4B[7])/3 + (4*e_1_2B[7]*e_1_4B[3]*e_1_4B[8])/3 + (4*e_1_2B[8]*e_1_4B[6]*e_1_4B[8])/3;
                p31[1] = 0.0;
                p31[2] = (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[6])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[6])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[7])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[8])/3;
                p31[3] = 0.0;
                p31[4] = (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[6])/3 - (e_1_2B[0]*e_1_2B[6])/3;

                p32[0] = (4*e_1_2B[5]*e_1_4B[7]*e_1_4B[7])/3 - (e_1_2B[4]*e_1_2B[7])/3 - (e_1_2B[7]*e_1_2B[8])/3 + (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[7])/3 + (4*e_1_2B[7]*e_1_4B[4]*e_1_4B[8])/3 + (4*e_1_2B[8]*e_1_4B[7]*e_1_4B[8])/3;
                p32[1] = 0.0;
                p32[2] = (4*e_1_2B[1]*e_1_4B[4]*e_1_4B[6])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[7])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[7])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[8])/3;
                p32[3] = 0.0;
                p32[4] = (4*e_1_2B[0]*e_1_4B[1]*e_1_4B[6])/3 - (e_1_2B[1]*e_1_2B[6])/3;

                p33[0] = (4*e_1_2B[8]*e_1_4B[8]*e_1_4B[8])/3 - e_1_2B[8]*e_1_2B[8]/3 - (e_1_2B[5]*e_1_2B[7])/3 + (4*e_1_2B[4]*e_1_4B[5]*e_1_4B[7])/3 + (4*e_1_2B[5]*e_1_4B[7]*e_1_4B[8])/3 + (4*e_1_2B[7]*e_1_4B[5]*e_1_4B[8])/3;
                p33[1] = 0.0;
                p33[2] = (4*e_1_2B[1]*e_1_4B[5]*e_1_4B[6])/3 + (4*e_1_2B[3]*e_1_4B[2]*e_1_4B[7])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[8])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[8])/3;
                p33[3] = 0.0;
                p33[4] = (4*e_1_2B[0]*e_1_4B[2]*e_1_4B[6])/3 - (e_1_2B[2]*e_1_2B[6])/3;

                p11 += 5;
                p12 += 5;
                p13 += 5;
                p21 += 5;
                p22 += 5;
                p23 += 5;
                p31 += 5;
                p32 += 5;
                p33 += 5;
            }
            break;

            case manakov_discretization_4SPLIT6B:
            e_1_2B = &e_Bstorage[0];    // exp(Bh/2)
            e_1_3B = &e_Bstorage[9];    // exp(Bh/2)
            e_1_4B = &e_Bstorage[18];    // exp(Bh/4)
            e_1_6B = &e_Bstorage[27];    // exp(Bh/2)
            q1_c1 = malloc(D*sizeof(COMPLEX));
            q2_c1 = malloc(D*sizeof(COMPLEX));
            q1_c2 = malloc(D*sizeof(COMPLEX));
            q2_c2 = malloc(D*sizeof(COMPLEX));
            q1_eff = malloc(D_eff*sizeof(COMPLEX));
            q2_eff = malloc(D_eff*sizeof(COMPLEX));

            
            // Getting non-equidistant samples. qci has sample points at T0+(n+ci)*eps_t,
            // c1 = 0.5-sqrt(3)/6, c2 = 0.5+sqrt(3)/6
            // Getting new samples:
            misc_resample(D, eps_t, q1, (0.5-SQRT(3)/6)*eps_t, q1_c1);
            misc_resample(D, eps_t, q1, (0.5+SQRT(3)/6)*eps_t, q1_c2);
            misc_resample(D, eps_t, q2, (0.5-SQRT(3)/6)*eps_t, q2_c1);
            misc_resample(D, eps_t, q2, (0.5+SQRT(3)/6)*eps_t, q2_c2);

            for (i=0; i<D; i++){
                q1_eff[2*i] = a1*q1_c1[i]+a2*q1_c2[i];
                q2_eff[2*i] = a1*q2_c1[i]+a2*q2_c2[i];
                q1_eff[2*i+1] = a2*q1_c1[i]+a1*q1_c2[i];
                q2_eff[2*i+1] = a2*q2_c1[i]+a1*q2_c2[i];
            }// q1, q2 are passed as constant, so we need to declare new variables q1,2_eff so we can 
                // assign new values

            for (i=D_eff; i-->0;) { 
                manakov_fscatter_zero_freq_scatter_matrix(e_1_2B, eps_t/2, q1_eff[i], q2_eff[i], kappa);
                manakov_fscatter_zero_freq_scatter_matrix(e_1_3B, eps_t/3, q1_eff[i], q2_eff[i], kappa);
                manakov_fscatter_zero_freq_scatter_matrix(e_1_4B, eps_t/4, q1_eff[i], q2_eff[i], kappa);
                manakov_fscatter_zero_freq_scatter_matrix(e_1_6B, eps_t/6, q1_eff[i], q2_eff[i], kappa);

                p11[0] = (e_1_2B[1]*e_1_2B[3])/24 + (e_1_2B[2]*e_1_2B[6])/24 - (16*e_1_2B[4]*e_1_4B[1]*e_1_4B[3])/15 - (16*e_1_2B[5]*e_1_4B[1]*e_1_4B[6])/15 - (16*e_1_2B[7]*e_1_4B[2]*e_1_4B[3])/15 - (16*e_1_2B[8]*e_1_4B[2]*e_1_4B[6])/15 + (81*e_1_3B[4]*e_1_3B[4]*e_1_6B[1]*e_1_6B[3])/40 + (81*e_1_3B[8]*e_1_3B[8]*e_1_6B[2]*e_1_6B[6])/40 + (81*e_1_3B[4]*e_1_3B[5]*e_1_6B[1]*e_1_6B[6])/40 + (81*e_1_3B[4]*e_1_3B[7]*e_1_6B[2]*e_1_6B[3])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[1]*e_1_6B[3])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[2]*e_1_6B[6])/40 + (81*e_1_3B[5]*e_1_3B[8]*e_1_6B[1]*e_1_6B[6])/40 + (81*e_1_3B[7]*e_1_3B[8]*e_1_6B[2]*e_1_6B[3])/40;
                p11[1] = 0.0;
                p11[2] = 0.0;
                p11[3] = 0.0;
                p11[4] = (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[1]*e_1_6B[3])/40 + (81*e_1_3B[1]*e_1_3B[4]*e_1_6B[0]*e_1_6B[3])/40 + (81*e_1_3B[3]*e_1_3B[4]*e_1_6B[0]*e_1_6B[1])/40 + (81*e_1_3B[1]*e_1_3B[5]*e_1_6B[0]*e_1_6B[6])/40 + (81*e_1_3B[1]*e_1_3B[6]*e_1_6B[2]*e_1_6B[3])/40 + (81*e_1_3B[2]*e_1_3B[3]*e_1_6B[1]*e_1_6B[6])/40 + (81*e_1_3B[2]*e_1_3B[7]*e_1_6B[0]*e_1_6B[3])/40 + (81*e_1_3B[3]*e_1_3B[7]*e_1_6B[0]*e_1_6B[2])/40 + (81*e_1_3B[5]*e_1_3B[6]*e_1_6B[0]*e_1_6B[1])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[2]*e_1_6B[6])/40 + (81*e_1_3B[2]*e_1_3B[8]*e_1_6B[0]*e_1_6B[6])/40 + (81*e_1_3B[6]*e_1_3B[8]*e_1_6B[0]*e_1_6B[2])/40;
                p11[5] = 0.0;
                p11[6] = - (16*e_1_2B[1]*e_1_4B[0]*e_1_4B[3])/15 - (16*e_1_2B[3]*e_1_4B[0]*e_1_4B[1])/15 - (16*e_1_2B[2]*e_1_4B[0]*e_1_4B[6])/15 - (16*e_1_2B[6]*e_1_4B[0]*e_1_4B[2])/15;
                p11[7] = 0.0;
                p11[8] = (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[0]*e_1_6B[0])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[0]*e_1_6B[0])/40 + (81*e_1_3B[0]*e_1_3B[1]*e_1_6B[0]*e_1_6B[3])/40 + (81*e_1_3B[0]*e_1_3B[3]*e_1_6B[0]*e_1_6B[1])/40 + (81*e_1_3B[0]*e_1_3B[2]*e_1_6B[0]*e_1_6B[6])/40 + (81*e_1_3B[0]*e_1_3B[6]*e_1_6B[0]*e_1_6B[2])/40;
                p11[9] = 0.0;
                p11[10] = 0.0;
                p11[11] = 0.0;
                p11[12] = e_1_2B[0]*e_1_2B[0]/24 - (16*e_1_4B[0]*e_1_4B[0]*e_1_2B[0])/15 + (81*e_1_3B[0]*e_1_3B[0]*e_1_6B[0]*e_1_6B[0])/40;

                p12[0] = (e_1_2B[1]*e_1_2B[4])/24 + (e_1_2B[2]*e_1_2B[7])/24 - (16*e_1_2B[4]*e_1_4B[1]*e_1_4B[4])/15 - (16*e_1_2B[5]*e_1_4B[1]*e_1_4B[7])/15 - (16*e_1_2B[7]*e_1_4B[2]*e_1_4B[4])/15 - (16*e_1_2B[8]*e_1_4B[2]*e_1_4B[7])/15 + (81*e_1_3B[4]*e_1_3B[4]*e_1_6B[1]*e_1_6B[4])/40 + (81*e_1_3B[8]*e_1_3B[8]*e_1_6B[2]*e_1_6B[7])/40 + (81*e_1_3B[4]*e_1_3B[5]*e_1_6B[1]*e_1_6B[7])/40 + (81*e_1_3B[4]*e_1_3B[7]*e_1_6B[2]*e_1_6B[4])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[1]*e_1_6B[4])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[2]*e_1_6B[7])/40 + (81*e_1_3B[5]*e_1_3B[8]*e_1_6B[1]*e_1_6B[7])/40 + (81*e_1_3B[7]*e_1_3B[8]*e_1_6B[2]*e_1_6B[4])/40;
                p12[1] = 0.0;
                p12[2] = 0.0;
                p12[3] = 0.0;
                p12[4] = (81*e_1_3B[3]*e_1_3B[4]*e_1_6B[1]*e_1_6B[1])/40 + (81*e_1_3B[5]*e_1_3B[6]*e_1_6B[1]*e_1_6B[1])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[1]*e_1_6B[4])/40 + (81*e_1_3B[1]*e_1_3B[4]*e_1_6B[0]*e_1_6B[4])/40 + (81*e_1_3B[1]*e_1_3B[5]*e_1_6B[0]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[6]*e_1_6B[2]*e_1_6B[4])/40 + (81*e_1_3B[2]*e_1_3B[3]*e_1_6B[1]*e_1_6B[7])/40 + (81*e_1_3B[2]*e_1_3B[7]*e_1_6B[0]*e_1_6B[4])/40 + (81*e_1_3B[3]*e_1_3B[7]*e_1_6B[1]*e_1_6B[2])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[2]*e_1_6B[7])/40 + (81*e_1_3B[2]*e_1_3B[8]*e_1_6B[0]*e_1_6B[7])/40 + (81*e_1_3B[6]*e_1_3B[8]*e_1_6B[1]*e_1_6B[2])/40;
                p12[5] = 0.0;
                p12[6] = - (16*e_1_2B[3]*e_1_4B[1]*e_1_4B[1])/15 - (16*e_1_2B[1]*e_1_4B[0]*e_1_4B[4])/15 - (16*e_1_2B[2]*e_1_4B[0]*e_1_4B[7])/15 - (16*e_1_2B[6]*e_1_4B[1]*e_1_4B[2])/15;
                p12[7] = 0.0;
                p12[8] = (81*e_1_3B[0]*e_1_3B[3]*e_1_6B[1]*e_1_6B[1])/40 + (81*e_1_3B[0]*e_1_3B[1]*e_1_6B[0]*e_1_6B[4])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[0]*e_1_6B[1])/40 + (81*e_1_3B[0]*e_1_3B[2]*e_1_6B[0]*e_1_6B[7])/40 + (81*e_1_3B[0]*e_1_3B[6]*e_1_6B[1]*e_1_6B[2])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[0]*e_1_6B[1])/40;
                p12[9] = 0.0;
                p12[10] = 0.0;
                p12[11] = 0.0;
                p12[12] = (81*e_1_6B[0]*e_1_6B[1]*e_1_3B[0]*e_1_3B[0])/40 + (e_1_2B[0]*e_1_2B[1])/24 - (16*e_1_2B[0]*e_1_4B[0]*e_1_4B[1])/15;

                p13[0] = (e_1_2B[1]*e_1_2B[5])/24 + (e_1_2B[2]*e_1_2B[8])/24 - (16*e_1_2B[4]*e_1_4B[1]*e_1_4B[5])/15 - (16*e_1_2B[5]*e_1_4B[1]*e_1_4B[8])/15 - (16*e_1_2B[7]*e_1_4B[2]*e_1_4B[5])/15 - (16*e_1_2B[8]*e_1_4B[2]*e_1_4B[8])/15 + (81*e_1_3B[4]*e_1_3B[4]*e_1_6B[1]*e_1_6B[5])/40 + (81*e_1_3B[8]*e_1_3B[8]*e_1_6B[2]*e_1_6B[8])/40 + (81*e_1_3B[4]*e_1_3B[5]*e_1_6B[1]*e_1_6B[8])/40 + (81*e_1_3B[4]*e_1_3B[7]*e_1_6B[2]*e_1_6B[5])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[1]*e_1_6B[5])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[2]*e_1_6B[8])/40 + (81*e_1_3B[5]*e_1_3B[8]*e_1_6B[1]*e_1_6B[8])/40 + (81*e_1_3B[7]*e_1_3B[8]*e_1_6B[2]*e_1_6B[5])/40;
                p13[1] = 0.0;
                p13[2] = 0.0;
                p13[3] = 0.0;
                p13[4] = (81*e_1_3B[3]*e_1_3B[7]*e_1_6B[2]*e_1_6B[2])/40 + (81*e_1_3B[6]*e_1_3B[8]*e_1_6B[2]*e_1_6B[2])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[1]*e_1_6B[5])/40 + (81*e_1_3B[1]*e_1_3B[4]*e_1_6B[0]*e_1_6B[5])/40 + (81*e_1_3B[3]*e_1_3B[4]*e_1_6B[1]*e_1_6B[2])/40 + (81*e_1_3B[1]*e_1_3B[5]*e_1_6B[0]*e_1_6B[8])/40 + (81*e_1_3B[1]*e_1_3B[6]*e_1_6B[2]*e_1_6B[5])/40 + (81*e_1_3B[2]*e_1_3B[3]*e_1_6B[1]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[7]*e_1_6B[0]*e_1_6B[5])/40 + (81*e_1_3B[5]*e_1_3B[6]*e_1_6B[1]*e_1_6B[2])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[2]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[8]*e_1_6B[0]*e_1_6B[8])/40;
                p13[5] = 0.0;
                p13[6] = - (16*e_1_2B[6]*e_1_4B[2]*e_1_4B[2])/15 - (16*e_1_2B[1]*e_1_4B[0]*e_1_4B[5])/15 - (16*e_1_2B[3]*e_1_4B[1]*e_1_4B[2])/15 - (16*e_1_2B[2]*e_1_4B[0]*e_1_4B[8])/15;
                p13[7] = 0.0;
                p13[8] = (81*e_1_3B[0]*e_1_3B[6]*e_1_6B[2]*e_1_6B[2])/40 + (81*e_1_3B[0]*e_1_3B[1]*e_1_6B[0]*e_1_6B[5])/40 + (81*e_1_3B[0]*e_1_3B[3]*e_1_6B[1]*e_1_6B[2])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[0]*e_1_6B[2])/40 + (81*e_1_3B[0]*e_1_3B[2]*e_1_6B[0]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[0]*e_1_6B[2])/40;
                p13[9] = 0.0;
                p13[10] = 0.0;
                p13[11] = 0.0;
                p13[12] = (81*e_1_6B[0]*e_1_6B[2]*e_1_3B[0]*e_1_3B[0])/40 + (e_1_2B[0]*e_1_2B[2])/24 - (16*e_1_2B[0]*e_1_4B[0]*e_1_4B[2])/15;

                p21[0] = (e_1_2B[3]*e_1_2B[4])/24 + (e_1_2B[5]*e_1_2B[6])/24 - (16*e_1_2B[4]*e_1_4B[3]*e_1_4B[4])/15 - (16*e_1_2B[5]*e_1_4B[4]*e_1_4B[6])/15 - (16*e_1_2B[7]*e_1_4B[3]*e_1_4B[5])/15 - (16*e_1_2B[8]*e_1_4B[5]*e_1_4B[6])/15 + (81*e_1_3B[4]*e_1_3B[4]*e_1_6B[3]*e_1_6B[4])/40 + (81*e_1_3B[8]*e_1_3B[8]*e_1_6B[5]*e_1_6B[6])/40 + (81*e_1_3B[4]*e_1_3B[5]*e_1_6B[4]*e_1_6B[6])/40 + (81*e_1_3B[4]*e_1_3B[7]*e_1_6B[3]*e_1_6B[5])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[3]*e_1_6B[4])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[5]*e_1_6B[6])/40 + (81*e_1_3B[5]*e_1_3B[8]*e_1_6B[4]*e_1_6B[6])/40 + (81*e_1_3B[7]*e_1_3B[8]*e_1_6B[3]*e_1_6B[5])/40;
                p21[1] = 0.0;
                p21[2] = 0.0;
                p21[3] = 0.0;
                p21[4] = (81*e_1_3B[1]*e_1_3B[4]*e_1_6B[3]*e_1_6B[3])/40 + (81*e_1_3B[2]*e_1_3B[7]*e_1_6B[3]*e_1_6B[3])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[3]*e_1_6B[4])/40 + (81*e_1_3B[3]*e_1_3B[4]*e_1_6B[0]*e_1_6B[4])/40 + (81*e_1_3B[1]*e_1_3B[5]*e_1_6B[3]*e_1_6B[6])/40 + (81*e_1_3B[1]*e_1_3B[6]*e_1_6B[3]*e_1_6B[5])/40 + (81*e_1_3B[2]*e_1_3B[3]*e_1_6B[4]*e_1_6B[6])/40 + (81*e_1_3B[3]*e_1_3B[7]*e_1_6B[0]*e_1_6B[5])/40 + (81*e_1_3B[5]*e_1_3B[6]*e_1_6B[0]*e_1_6B[4])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[5]*e_1_6B[6])/40 + (81*e_1_3B[2]*e_1_3B[8]*e_1_6B[3]*e_1_6B[6])/40 + (81*e_1_3B[6]*e_1_3B[8]*e_1_6B[0]*e_1_6B[5])/40;
                p21[5] = 0.0;
                p21[6] = - (16*e_1_2B[1]*e_1_4B[3]*e_1_4B[3])/15 - (16*e_1_2B[3]*e_1_4B[0]*e_1_4B[4])/15 - (16*e_1_2B[2]*e_1_4B[3]*e_1_4B[6])/15 - (16*e_1_2B[6]*e_1_4B[0]*e_1_4B[5])/15;
                p21[7] = 0.0;
                p21[8] = (81*e_1_3B[0]*e_1_3B[1]*e_1_6B[3]*e_1_6B[3])/40 + (81*e_1_3B[0]*e_1_3B[3]*e_1_6B[0]*e_1_6B[4])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[0]*e_1_6B[3])/40 + (81*e_1_3B[0]*e_1_3B[2]*e_1_6B[3]*e_1_6B[6])/40 + (81*e_1_3B[0]*e_1_3B[6]*e_1_6B[0]*e_1_6B[5])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[0]*e_1_6B[3])/40;
                p21[9] = 0.0;
                p21[10] = 0.0;
                p21[11] = 0.0;
                p21[12] = (81*e_1_6B[0]*e_1_6B[3]*e_1_3B[0]*e_1_3B[0])/40 + (e_1_2B[0]*e_1_2B[3])/24 - (16*e_1_2B[0]*e_1_4B[0]*e_1_4B[3])/15;

                p22[0] = e_1_2B[4]*e_1_2B[4]/24 - (16*e_1_2B[4]*e_1_4B[4]*e_1_4B[4])/15 + (81*e_1_3B[4]*e_1_3B[4]*e_1_6B[4]*e_1_6B[4])/40 + (e_1_2B[5]*e_1_2B[7])/24 - (16*e_1_2B[5]*e_1_4B[4]*e_1_4B[7])/15 - (16*e_1_2B[7]*e_1_4B[4]*e_1_4B[5])/15 - (16*e_1_2B[8]*e_1_4B[5]*e_1_4B[7])/15 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[4]*e_1_6B[4])/40 + (81*e_1_3B[8]*e_1_3B[8]*e_1_6B[5]*e_1_6B[7])/40 + (81*e_1_3B[4]*e_1_3B[5]*e_1_6B[4]*e_1_6B[7])/40 + (81*e_1_3B[4]*e_1_3B[7]*e_1_6B[4]*e_1_6B[5])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[5]*e_1_6B[7])/40 + (81*e_1_3B[5]*e_1_3B[8]*e_1_6B[4]*e_1_6B[7])/40 + (81*e_1_3B[7]*e_1_3B[8]*e_1_6B[4]*e_1_6B[5])/40;
                p22[1] = 0.0;
                p22[2] = 0.0;
                p22[3] = 0.0;
                p22[4] = (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[4]*e_1_6B[4])/40 + (81*e_1_3B[1]*e_1_3B[4]*e_1_6B[3]*e_1_6B[4])/40 + (81*e_1_3B[3]*e_1_3B[4]*e_1_6B[1]*e_1_6B[4])/40 + (81*e_1_3B[1]*e_1_3B[5]*e_1_6B[3]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[6]*e_1_6B[4]*e_1_6B[5])/40 + (81*e_1_3B[2]*e_1_3B[3]*e_1_6B[4]*e_1_6B[7])/40 + (81*e_1_3B[2]*e_1_3B[7]*e_1_6B[3]*e_1_6B[4])/40 + (81*e_1_3B[3]*e_1_3B[7]*e_1_6B[1]*e_1_6B[5])/40 + (81*e_1_3B[5]*e_1_3B[6]*e_1_6B[1]*e_1_6B[4])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[5]*e_1_6B[7])/40 + (81*e_1_3B[2]*e_1_3B[8]*e_1_6B[3]*e_1_6B[7])/40 + (81*e_1_3B[6]*e_1_3B[8]*e_1_6B[1]*e_1_6B[5])/40;
                p22[5] = 0.0;
                p22[6] = - (16*e_1_2B[1]*e_1_4B[3]*e_1_4B[4])/15 - (16*e_1_2B[3]*e_1_4B[1]*e_1_4B[4])/15 - (16*e_1_2B[2]*e_1_4B[3]*e_1_4B[7])/15 - (16*e_1_2B[6]*e_1_4B[1]*e_1_4B[5])/15;
                p22[7] = 0.0;
                p22[8] = (81*e_1_3B[0]*e_1_3B[1]*e_1_6B[3]*e_1_6B[4])/40 + (81*e_1_3B[0]*e_1_3B[3]*e_1_6B[1]*e_1_6B[4])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[1]*e_1_6B[3])/40 + (81*e_1_3B[0]*e_1_3B[2]*e_1_6B[3]*e_1_6B[7])/40 + (81*e_1_3B[0]*e_1_3B[6]*e_1_6B[1]*e_1_6B[5])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[1]*e_1_6B[3])/40;
                p22[9] = 0.0;
                p22[10] = 0.0;
                p22[11] = 0.0;
                p22[12] = (81*e_1_6B[1]*e_1_6B[3]*e_1_3B[0]*e_1_3B[0])/40 + (e_1_2B[1]*e_1_2B[3])/24 - (16*e_1_2B[0]*e_1_4B[1]*e_1_4B[3])/15;

                p23[0] = (e_1_2B[4]*e_1_2B[5])/24 - (16*e_1_2B[7]*e_1_4B[5]*e_1_4B[5])/15 + (e_1_2B[5]*e_1_2B[8])/24 - (16*e_1_2B[4]*e_1_4B[4]*e_1_4B[5])/15 - (16*e_1_2B[5]*e_1_4B[4]*e_1_4B[8])/15 - (16*e_1_2B[8]*e_1_4B[5]*e_1_4B[8])/15 + (81*e_1_3B[4]*e_1_3B[7]*e_1_6B[5]*e_1_6B[5])/40 + (81*e_1_3B[7]*e_1_3B[8]*e_1_6B[5]*e_1_6B[5])/40 + (81*e_1_3B[4]*e_1_3B[4]*e_1_6B[4]*e_1_6B[5])/40 + (81*e_1_3B[8]*e_1_3B[8]*e_1_6B[5]*e_1_6B[8])/40 + (81*e_1_3B[4]*e_1_3B[5]*e_1_6B[4]*e_1_6B[8])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[4]*e_1_6B[5])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[5]*e_1_6B[8])/40 + (81*e_1_3B[5]*e_1_3B[8]*e_1_6B[4]*e_1_6B[8])/40;
                p23[1] = 0.0;
                p23[2] = 0.0;
                p23[3] = 0.0;
                p23[4] = (81*e_1_3B[1]*e_1_3B[6]*e_1_6B[5]*e_1_6B[5])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[4]*e_1_6B[5])/40 + (81*e_1_3B[1]*e_1_3B[4]*e_1_6B[3]*e_1_6B[5])/40 + (81*e_1_3B[3]*e_1_3B[4]*e_1_6B[2]*e_1_6B[4])/40 + (81*e_1_3B[1]*e_1_3B[5]*e_1_6B[3]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[3]*e_1_6B[4]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[7]*e_1_6B[3]*e_1_6B[5])/40 + (81*e_1_3B[3]*e_1_3B[7]*e_1_6B[2]*e_1_6B[5])/40 + (81*e_1_3B[5]*e_1_3B[6]*e_1_6B[2]*e_1_6B[4])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[5]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[8]*e_1_6B[3]*e_1_6B[8])/40 + (81*e_1_3B[6]*e_1_3B[8]*e_1_6B[2]*e_1_6B[5])/40;
                p23[5] = 0.0;
                p23[6] = - (16*e_1_2B[1]*e_1_4B[3]*e_1_4B[5])/15 - (16*e_1_2B[3]*e_1_4B[2]*e_1_4B[4])/15 - (16*e_1_2B[2]*e_1_4B[3]*e_1_4B[8])/15 - (16*e_1_2B[6]*e_1_4B[2]*e_1_4B[5])/15;
                p23[7] = 0.0;
                p23[8] = (81*e_1_3B[0]*e_1_3B[1]*e_1_6B[3]*e_1_6B[5])/40 + (81*e_1_3B[0]*e_1_3B[3]*e_1_6B[2]*e_1_6B[4])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[2]*e_1_6B[3])/40 + (81*e_1_3B[0]*e_1_3B[2]*e_1_6B[3]*e_1_6B[8])/40 + (81*e_1_3B[0]*e_1_3B[6]*e_1_6B[2]*e_1_6B[5])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[2]*e_1_6B[3])/40;
                p23[9] = 0.0;
                p23[10] = 0.0;
                p23[11] = 0.0;
                p23[12] = (81*e_1_6B[2]*e_1_6B[3]*e_1_3B[0]*e_1_3B[0])/40 + (e_1_2B[2]*e_1_2B[3])/24 - (16*e_1_2B[0]*e_1_4B[2]*e_1_4B[3])/15;

                p31[0] = (e_1_2B[3]*e_1_2B[7])/24 + (e_1_2B[6]*e_1_2B[8])/24 - (16*e_1_2B[4]*e_1_4B[3]*e_1_4B[7])/15 - (16*e_1_2B[5]*e_1_4B[6]*e_1_4B[7])/15 - (16*e_1_2B[7]*e_1_4B[3]*e_1_4B[8])/15 - (16*e_1_2B[8]*e_1_4B[6]*e_1_4B[8])/15 + (81*e_1_3B[4]*e_1_3B[4]*e_1_6B[3]*e_1_6B[7])/40 + (81*e_1_3B[8]*e_1_3B[8]*e_1_6B[6]*e_1_6B[8])/40 + (81*e_1_3B[4]*e_1_3B[5]*e_1_6B[6]*e_1_6B[7])/40 + (81*e_1_3B[4]*e_1_3B[7]*e_1_6B[3]*e_1_6B[8])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[3]*e_1_6B[7])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[6]*e_1_6B[8])/40 + (81*e_1_3B[5]*e_1_3B[8]*e_1_6B[6]*e_1_6B[7])/40 + (81*e_1_3B[7]*e_1_3B[8]*e_1_6B[3]*e_1_6B[8])/40;
                p31[1] = 0.0;
                p31[2] = 0.0;
                p31[3] = 0.0;
                p31[4] = (81*e_1_3B[1]*e_1_3B[5]*e_1_6B[6]*e_1_6B[6])/40 + (81*e_1_3B[2]*e_1_3B[8]*e_1_6B[6]*e_1_6B[6])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[3]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[4]*e_1_6B[3]*e_1_6B[6])/40 + (81*e_1_3B[3]*e_1_3B[4]*e_1_6B[0]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[6]*e_1_6B[3]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[3]*e_1_6B[6]*e_1_6B[7])/40 + (81*e_1_3B[2]*e_1_3B[7]*e_1_6B[3]*e_1_6B[6])/40 + (81*e_1_3B[3]*e_1_3B[7]*e_1_6B[0]*e_1_6B[8])/40 + (81*e_1_3B[5]*e_1_3B[6]*e_1_6B[0]*e_1_6B[7])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[6]*e_1_6B[8])/40 + (81*e_1_3B[6]*e_1_3B[8]*e_1_6B[0]*e_1_6B[8])/40;
                p31[5] = 0.0;
                p31[6] = - (16*e_1_2B[2]*e_1_4B[6]*e_1_4B[6])/15 - (16*e_1_2B[1]*e_1_4B[3]*e_1_4B[6])/15 - (16*e_1_2B[3]*e_1_4B[0]*e_1_4B[7])/15 - (16*e_1_2B[6]*e_1_4B[0]*e_1_4B[8])/15;
                p31[7] = 0.0;
                p31[8] = (81*e_1_3B[0]*e_1_3B[2]*e_1_6B[6]*e_1_6B[6])/40 + (81*e_1_3B[0]*e_1_3B[1]*e_1_6B[3]*e_1_6B[6])/40 + (81*e_1_3B[0]*e_1_3B[3]*e_1_6B[0]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[0]*e_1_6B[6])/40 + (81*e_1_3B[0]*e_1_3B[6]*e_1_6B[0]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[0]*e_1_6B[6])/40;
                p31[9] = 0.0;
                p31[10] = 0.0;
                p31[11] = 0.0;
                p31[12] = (81*e_1_6B[0]*e_1_6B[6]*e_1_3B[0]*e_1_3B[0])/40 + (e_1_2B[0]*e_1_2B[6])/24 - (16*e_1_2B[0]*e_1_4B[0]*e_1_4B[6])/15;

                p32[0] = (e_1_2B[4]*e_1_2B[7])/24 - (16*e_1_2B[5]*e_1_4B[7]*e_1_4B[7])/15 + (e_1_2B[7]*e_1_2B[8])/24 - (16*e_1_2B[4]*e_1_4B[4]*e_1_4B[7])/15 - (16*e_1_2B[7]*e_1_4B[4]*e_1_4B[8])/15 - (16*e_1_2B[8]*e_1_4B[7]*e_1_4B[8])/15 + (81*e_1_3B[4]*e_1_3B[5]*e_1_6B[7]*e_1_6B[7])/40 + (81*e_1_3B[5]*e_1_3B[8]*e_1_6B[7]*e_1_6B[7])/40 + (81*e_1_3B[4]*e_1_3B[4]*e_1_6B[4]*e_1_6B[7])/40 + (81*e_1_3B[8]*e_1_3B[8]*e_1_6B[7]*e_1_6B[8])/40 + (81*e_1_3B[4]*e_1_3B[7]*e_1_6B[4]*e_1_6B[8])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[4]*e_1_6B[7])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[7]*e_1_6B[8])/40 + (81*e_1_3B[7]*e_1_3B[8]*e_1_6B[4]*e_1_6B[8])/40;
                p32[1] = 0.0;
                p32[2] = 0.0;
                p32[3] = 0.0;
                p32[4] = (81*e_1_3B[2]*e_1_3B[3]*e_1_6B[7]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[4]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[4]*e_1_6B[4]*e_1_6B[6])/40 + (81*e_1_3B[3]*e_1_3B[4]*e_1_6B[1]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[5]*e_1_6B[6]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[6]*e_1_6B[4]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[7]*e_1_6B[4]*e_1_6B[6])/40 + (81*e_1_3B[3]*e_1_3B[7]*e_1_6B[1]*e_1_6B[8])/40 + (81*e_1_3B[5]*e_1_3B[6]*e_1_6B[1]*e_1_6B[7])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[7]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[8]*e_1_6B[6]*e_1_6B[7])/40 + (81*e_1_3B[6]*e_1_3B[8]*e_1_6B[1]*e_1_6B[8])/40;
                p32[5] = 0.0;
                p32[6] = - (16*e_1_2B[1]*e_1_4B[4]*e_1_4B[6])/15 - (16*e_1_2B[3]*e_1_4B[1]*e_1_4B[7])/15 - (16*e_1_2B[2]*e_1_4B[6]*e_1_4B[7])/15 - (16*e_1_2B[6]*e_1_4B[1]*e_1_4B[8])/15;
                p32[7] = 0.0;
                p32[8] = (81*e_1_3B[0]*e_1_3B[1]*e_1_6B[4]*e_1_6B[6])/40 + (81*e_1_3B[0]*e_1_3B[3]*e_1_6B[1]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[1]*e_1_6B[6])/40 + (81*e_1_3B[0]*e_1_3B[2]*e_1_6B[6]*e_1_6B[7])/40 + (81*e_1_3B[0]*e_1_3B[6]*e_1_6B[1]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[1]*e_1_6B[6])/40;
                p32[9] = 0.0;
                p32[10] = 0.0;
                p32[11] = 0.0;
                p32[12] = (81*e_1_6B[1]*e_1_6B[6]*e_1_3B[0]*e_1_3B[0])/40 + (e_1_2B[1]*e_1_2B[6])/24 - (16*e_1_2B[0]*e_1_4B[1]*e_1_4B[6])/15;

                p33[0] = e_1_2B[8]*e_1_2B[8]/24 - (16*e_1_2B[8]*e_1_4B[8]*e_1_4B[8])/15 + (81*e_1_3B[8]*e_1_3B[8]*e_1_6B[8]*e_1_6B[8])/40 + (e_1_2B[5]*e_1_2B[7])/24 - (16*e_1_2B[4]*e_1_4B[5]*e_1_4B[7])/15 - (16*e_1_2B[5]*e_1_4B[7]*e_1_4B[8])/15 - (16*e_1_2B[7]*e_1_4B[5]*e_1_4B[8])/15 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[8]*e_1_6B[8])/40 + (81*e_1_3B[4]*e_1_3B[4]*e_1_6B[5]*e_1_6B[7])/40 + (81*e_1_3B[4]*e_1_3B[5]*e_1_6B[7]*e_1_6B[8])/40 + (81*e_1_3B[4]*e_1_3B[7]*e_1_6B[5]*e_1_6B[8])/40 + (81*e_1_3B[5]*e_1_3B[7]*e_1_6B[5]*e_1_6B[7])/40 + (81*e_1_3B[5]*e_1_3B[8]*e_1_6B[7]*e_1_6B[8])/40 + (81*e_1_3B[7]*e_1_3B[8]*e_1_6B[5]*e_1_6B[8])/40;
                p33[1] = 0.0;
                p33[2] = 0.0;
                p33[3] = 0.0;
                p33[4] = (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[8]*e_1_6B[8])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[5]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[4]*e_1_6B[5]*e_1_6B[6])/40 + (81*e_1_3B[3]*e_1_3B[4]*e_1_6B[2]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[5]*e_1_6B[6]*e_1_6B[8])/40 + (81*e_1_3B[1]*e_1_3B[6]*e_1_6B[5]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[3]*e_1_6B[7]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[7]*e_1_6B[5]*e_1_6B[6])/40 + (81*e_1_3B[3]*e_1_3B[7]*e_1_6B[2]*e_1_6B[8])/40 + (81*e_1_3B[5]*e_1_3B[6]*e_1_6B[2]*e_1_6B[7])/40 + (81*e_1_3B[2]*e_1_3B[8]*e_1_6B[6]*e_1_6B[8])/40 + (81*e_1_3B[6]*e_1_3B[8]*e_1_6B[2]*e_1_6B[8])/40;
                p33[5] = 0.0;
                p33[6] = - (16*e_1_2B[1]*e_1_4B[5]*e_1_4B[6])/15 - (16*e_1_2B[3]*e_1_4B[2]*e_1_4B[7])/15 - (16*e_1_2B[2]*e_1_4B[6]*e_1_4B[8])/15 - (16*e_1_2B[6]*e_1_4B[2]*e_1_4B[8])/15;
                p33[7] = 0.0;
                p33[8] = (81*e_1_3B[0]*e_1_3B[1]*e_1_6B[5]*e_1_6B[6])/40 + (81*e_1_3B[0]*e_1_3B[3]*e_1_6B[2]*e_1_6B[7])/40 + (81*e_1_3B[1]*e_1_3B[3]*e_1_6B[2]*e_1_6B[6])/40 + (81*e_1_3B[0]*e_1_3B[2]*e_1_6B[6]*e_1_6B[8])/40 + (81*e_1_3B[0]*e_1_3B[6]*e_1_6B[2]*e_1_6B[8])/40 + (81*e_1_3B[2]*e_1_3B[6]*e_1_6B[2]*e_1_6B[6])/40;
                p33[9] = 0.0;
                p33[10] = 0.0;
                p33[11] = 0.0;
                p33[12] = (81*e_1_6B[2]*e_1_6B[6]*e_1_3B[0]*e_1_3B[0])/40 + (e_1_2B[2]*e_1_2B[6])/24 - (16*e_1_2B[0]*e_1_4B[2]*e_1_4B[6])/15;

                p11 += 13;
                p12 += 13;
                p13 += 13;
                p21 += 13;
                p22 += 13;
                p23 += 13;
                p31 += 13;
                p32 += 13;
                p33 += 13;
            }
            break;




        case manakov_discretization_FTES4_4A: // Check if there is not yet a type for this.
                                            // _4 denotes the 4th order splitting schem

            e_1_2B = &e_Bstorage[0];    // exp(Bh/2)
            e_B = &e_Bstorage[9];    // exp(Bh)
            E1 = &E_storage[0];
            E2 = &E_storage[9];

            for (i=D_eff; i-->0;){
                manakov_fscatter_zero_freq_scatter_matrix(e_1_2B, eps_t/2, q1[i], q2[i], kappa);
                manakov_fscatter_zero_freq_scatter_matrix(e_B, eps_t, q1[i], q2[i], kappa);
 
                // Setting which elements of q we use
                q1n[1] = q1[i];
                q2n[1] = q2[i];
                
                if (i==0){  //left bnd condition
                q1n[0] = q1[i];      // set q1[n-1] to q[n] on boundary
                q2n[0] = q2[i];
                }
                else {
                q1n[0] = q1[i-1];
                q2n[0] = q2[i-1];
                }

                if (i==D_eff-1){  //right bnd condition
                q1n[2] = q1[i];      // set q1[n+1] to q[n] on boundary
                q2n[2] = q2[i];
                }
                else{
                q1n[2] = q1[i+1];
                q2n[2] = q2[i+1];
                }

                manakov_fscatter_get_E(E1, eps_t, (q1n[2]-q1n[0])*eps_t/24+(q1n[2]-2*q1n[1]+q1n[0])*eps_t/48,
                                (q2n[2]-q2n[0])*eps_t/24+(q2n[2]-2*q2n[1]+q2n[0])*eps_t/48,
                                kappa*CONJ(q1n[0]-q1n[2])*eps_t/24-kappa*CONJ(q1n[2]-2*q1n[1]+q1n[0])*eps_t/48,
                                kappa*CONJ(q2n[0]-q2n[2])*eps_t/24-kappa*CONJ(q2n[2]-2*q2n[1]+q2n[0])*eps_t/48);

                manakov_fscatter_get_E(E2, eps_t, -(q1n[2]-q1n[0])*eps_t/24+(q1n[2]-2*q1n[1]+q1n[0])*eps_t/48,
                                -(q2n[2]-q2n[0])*eps_t/24+(q2n[2]-2*q2n[1]+q2n[0])*eps_t/48,
                                kappa*CONJ(q1n[2]-q1n[0])*eps_t/24-kappa*CONJ(q1n[2]-2*q1n[1]+q1n[0])*eps_t/48,
                                kappa*CONJ(q2n[2]-q2n[0])*eps_t/24-kappa*CONJ(q2n[2]-2*q2n[1]+q2n[0])*eps_t/48);

                //TODO: more descriptive name
                
                p11[0] = (4*e_1_2B[4]*e_1_2B[4]*E1[1]*E2[3])/3 - (e_B[5]*E1[1]*E2[6])/3 - (e_B[7]*E1[2]*E2[3])/3 - (e_B[8]*E1[2]*E2[6])/3 - (e_B[4]*E1[1]*E2[3])/3 + (4*e_1_2B[8]*e_1_2B[8]*E1[2]*E2[6])/3 + (4*e_1_2B[4]*e_1_2B[5]*E1[1]*E2[6])/3 + (4*e_1_2B[4]*e_1_2B[7]*E1[2]*E2[3])/3 + (4*e_1_2B[5]*e_1_2B[7]*E1[1]*E2[3])/3 + (4*e_1_2B[5]*e_1_2B[7]*E1[2]*E2[6])/3 + (4*e_1_2B[5]*e_1_2B[8]*E1[1]*E2[6])/3 + (4*e_1_2B[7]*e_1_2B[8]*E1[2]*E2[3])/3;   // z^8
                p11[1] = 0.0;   // z^7
                p11[2] = (4*e_1_2B[1]*e_1_2B[4]*E1[0]*E2[3])/3 + (4*e_1_2B[3]*e_1_2B[4]*E1[1]*E2[0])/3 + (4*e_1_2B[1]*e_1_2B[5]*E1[0]*E2[6])/3 + (4*e_1_2B[2]*e_1_2B[7]*E1[0]*E2[3])/3 + (4*e_1_2B[3]*e_1_2B[7]*E1[2]*E2[0])/3 + (4*e_1_2B[5]*e_1_2B[6]*E1[1]*E2[0])/3 + (4*e_1_2B[2]*e_1_2B[8]*E1[0]*E2[6])/3 + (4*e_1_2B[6]*e_1_2B[8]*E1[2]*E2[0])/3;   // z^6
                p11[3] = 0.0;   // z^5 
                p11[4] = (4*e_1_2B[1]*e_1_2B[3]*E1[0]*E2[0])/3 - (e_B[3]*E1[1]*E2[0])/3 - (e_B[2]*E1[0]*E2[6])/3 - (e_B[6]*E1[2]*E2[0])/3 - (e_B[1]*E1[0]*E2[3])/3 + (4*e_1_2B[1]*e_1_2B[3]*E1[1]*E2[3])/3 + (4*e_1_2B[2]*e_1_2B[6]*E1[0]*E2[0])/3 + (4*e_1_2B[1]*e_1_2B[6]*E1[2]*E2[3])/3 + (4*e_1_2B[2]*e_1_2B[3]*E1[1]*E2[6])/3 + (4*e_1_2B[2]*e_1_2B[6]*E1[2]*E2[6])/3;     // z^4
                p11[5] = 0.0;   // z^3
                p11[6] = (4*e_1_2B[0]*e_1_2B[1]*E1[0]*E2[3])/3 + (4*e_1_2B[0]*e_1_2B[3]*E1[1]*E2[0])/3 + (4*e_1_2B[0]*e_1_2B[2]*E1[0]*E2[6])/3 + (4*e_1_2B[0]*e_1_2B[6]*E1[2]*E2[0])/3;   // z^2
                p11[7] = 0.0;   // z^1
                p11[8] = (4*E1[0]*E2[0]*e_1_2B[0]*e_1_2B[0])/3 - (e_B[0]*E1[0]*E2[0])/3; // z^0

                p12[0] = (4*e_1_2B[4]*e_1_2B[4]*E1[1]*E2[4])/3 - (e_B[5]*E1[1]*E2[7])/3 - (e_B[7]*E1[2]*E2[4])/3 - (e_B[8]*E1[2]*E2[7])/3 - (e_B[4]*E1[1]*E2[4])/3 + (4*e_1_2B[8]*e_1_2B[8]*E1[2]*E2[7])/3 + (4*e_1_2B[4]*e_1_2B[5]*E1[1]*E2[7])/3 + (4*e_1_2B[4]*e_1_2B[7]*E1[2]*E2[4])/3 + (4*e_1_2B[5]*e_1_2B[7]*E1[1]*E2[4])/3 + (4*e_1_2B[5]*e_1_2B[7]*E1[2]*E2[7])/3 + (4*e_1_2B[5]*e_1_2B[8]*E1[1]*E2[7])/3 + (4*e_1_2B[7]*e_1_2B[8]*E1[2]*E2[4])/3;
                p12[1] = 0.0;
                p12[2] = (4*e_1_2B[1]*e_1_2B[4]*E1[0]*E2[4])/3 + (4*e_1_2B[3]*e_1_2B[4]*E1[1]*E2[1])/3 + (4*e_1_2B[1]*e_1_2B[5]*E1[0]*E2[7])/3 + (4*e_1_2B[2]*e_1_2B[7]*E1[0]*E2[4])/3 + (4*e_1_2B[3]*e_1_2B[7]*E1[2]*E2[1])/3 + (4*e_1_2B[5]*e_1_2B[6]*E1[1]*E2[1])/3 + (4*e_1_2B[2]*e_1_2B[8]*E1[0]*E2[7])/3 + (4*e_1_2B[6]*e_1_2B[8]*E1[2]*E2[1])/3;
                p12[3] = 0.0;
                p12[4] = (4*e_1_2B[1]*e_1_2B[3]*E1[0]*E2[1])/3 - (e_B[3]*E1[1]*E2[1])/3 - (e_B[2]*E1[0]*E2[7])/3 - (e_B[6]*E1[2]*E2[1])/3 - (e_B[1]*E1[0]*E2[4])/3 + (4*e_1_2B[1]*e_1_2B[3]*E1[1]*E2[4])/3 + (4*e_1_2B[2]*e_1_2B[6]*E1[0]*E2[1])/3 + (4*e_1_2B[1]*e_1_2B[6]*E1[2]*E2[4])/3 + (4*e_1_2B[2]*e_1_2B[3]*E1[1]*E2[7])/3 + (4*e_1_2B[2]*e_1_2B[6]*E1[2]*E2[7])/3;
                p12[5] = 0.0;
                p12[6] = (4*e_1_2B[0]*e_1_2B[1]*E1[0]*E2[4])/3 + (4*e_1_2B[0]*e_1_2B[3]*E1[1]*E2[1])/3 + (4*e_1_2B[0]*e_1_2B[2]*E1[0]*E2[7])/3 + (4*e_1_2B[0]*e_1_2B[6]*E1[2]*E2[1])/3;
                p12[7] = 0.0;
                p12[8] = (4*E1[0]*E2[1]*e_1_2B[0]*e_1_2B[0])/3 - (e_B[0]*E1[0]*E2[1])/3;

                p13[0] = (4*e_1_2B[4]*e_1_2B[4]*E1[1]*E2[5])/3 - (e_B[5]*E1[1]*E2[8])/3 - (e_B[7]*E1[2]*E2[5])/3 - (e_B[8]*E1[2]*E2[8])/3 - (e_B[4]*E1[1]*E2[5])/3 + (4*e_1_2B[8]*e_1_2B[8]*E1[2]*E2[8])/3 + (4*e_1_2B[4]*e_1_2B[5]*E1[1]*E2[8])/3 + (4*e_1_2B[4]*e_1_2B[7]*E1[2]*E2[5])/3 + (4*e_1_2B[5]*e_1_2B[7]*E1[1]*E2[5])/3 + (4*e_1_2B[5]*e_1_2B[7]*E1[2]*E2[8])/3 + (4*e_1_2B[5]*e_1_2B[8]*E1[1]*E2[8])/3 + (4*e_1_2B[7]*e_1_2B[8]*E1[2]*E2[5])/3;
                p13[1] = 0.0;
                p13[2] = (4*e_1_2B[1]*e_1_2B[4]*E1[0]*E2[5])/3 + (4*e_1_2B[3]*e_1_2B[4]*E1[1]*E2[2])/3 + (4*e_1_2B[1]*e_1_2B[5]*E1[0]*E2[8])/3 + (4*e_1_2B[2]*e_1_2B[7]*E1[0]*E2[5])/3 + (4*e_1_2B[3]*e_1_2B[7]*E1[2]*E2[2])/3 + (4*e_1_2B[5]*e_1_2B[6]*E1[1]*E2[2])/3 + (4*e_1_2B[2]*e_1_2B[8]*E1[0]*E2[8])/3 + (4*e_1_2B[6]*e_1_2B[8]*E1[2]*E2[2])/3;
                p13[3] = 0.0;
                p13[4] = (4*e_1_2B[1]*e_1_2B[3]*E1[0]*E2[2])/3 - (e_B[3]*E1[1]*E2[2])/3 - (e_B[2]*E1[0]*E2[8])/3 - (e_B[6]*E1[2]*E2[2])/3 - (e_B[1]*E1[0]*E2[5])/3 + (4*e_1_2B[1]*e_1_2B[3]*E1[1]*E2[5])/3 + (4*e_1_2B[2]*e_1_2B[6]*E1[0]*E2[2])/3 + (4*e_1_2B[1]*e_1_2B[6]*E1[2]*E2[5])/3 + (4*e_1_2B[2]*e_1_2B[3]*E1[1]*E2[8])/3 + (4*e_1_2B[2]*e_1_2B[6]*E1[2]*E2[8])/3;
                p13[5] = 0.0;
                p13[6] = (4*e_1_2B[0]*e_1_2B[1]*E1[0]*E2[5])/3 + (4*e_1_2B[0]*e_1_2B[3]*E1[1]*E2[2])/3 + (4*e_1_2B[0]*e_1_2B[2]*E1[0]*E2[8])/3 + (4*e_1_2B[0]*e_1_2B[6]*E1[2]*E2[2])/3;
                p13[7] = 0.0;
                p13[8] = (4*E1[0]*E2[2]*e_1_2B[0]*e_1_2B[0])/3 - (e_B[0]*E1[0]*E2[2])/3;

                p21[0] = (4*e_1_2B[4]*e_1_2B[4]*E1[4]*E2[3])/3 - (e_B[5]*E1[4]*E2[6])/3 - (e_B[7]*E1[5]*E2[3])/3 - (e_B[8]*E1[5]*E2[6])/3 - (e_B[4]*E1[4]*E2[3])/3 + (4*e_1_2B[8]*e_1_2B[8]*E1[5]*E2[6])/3 + (4*e_1_2B[4]*e_1_2B[5]*E1[4]*E2[6])/3 + (4*e_1_2B[4]*e_1_2B[7]*E1[5]*E2[3])/3 + (4*e_1_2B[5]*e_1_2B[7]*E1[4]*E2[3])/3 + (4*e_1_2B[5]*e_1_2B[7]*E1[5]*E2[6])/3 + (4*e_1_2B[5]*e_1_2B[8]*E1[4]*E2[6])/3 + (4*e_1_2B[7]*e_1_2B[8]*E1[5]*E2[3])/3;
                p21[1] = 0.0;
                p21[2] = (4*e_1_2B[1]*e_1_2B[4]*E1[3]*E2[3])/3 + (4*e_1_2B[3]*e_1_2B[4]*E1[4]*E2[0])/3 + (4*e_1_2B[1]*e_1_2B[5]*E1[3]*E2[6])/3 + (4*e_1_2B[2]*e_1_2B[7]*E1[3]*E2[3])/3 + (4*e_1_2B[3]*e_1_2B[7]*E1[5]*E2[0])/3 + (4*e_1_2B[5]*e_1_2B[6]*E1[4]*E2[0])/3 + (4*e_1_2B[2]*e_1_2B[8]*E1[3]*E2[6])/3 + (4*e_1_2B[6]*e_1_2B[8]*E1[5]*E2[0])/3;
                p21[3] = 0.0;
                p21[4] = (4*e_1_2B[1]*e_1_2B[3]*E1[3]*E2[0])/3 - (e_B[3]*E1[4]*E2[0])/3 - (e_B[2]*E1[3]*E2[6])/3 - (e_B[6]*E1[5]*E2[0])/3 - (e_B[1]*E1[3]*E2[3])/3 + (4*e_1_2B[1]*e_1_2B[3]*E1[4]*E2[3])/3 + (4*e_1_2B[2]*e_1_2B[6]*E1[3]*E2[0])/3 + (4*e_1_2B[1]*e_1_2B[6]*E1[5]*E2[3])/3 + (4*e_1_2B[2]*e_1_2B[3]*E1[4]*E2[6])/3 + (4*e_1_2B[2]*e_1_2B[6]*E1[5]*E2[6])/3;
                p21[5] = 0.0;
                p21[6] = (4*e_1_2B[0]*e_1_2B[1]*E1[3]*E2[3])/3 + (4*e_1_2B[0]*e_1_2B[3]*E1[4]*E2[0])/3 + (4*e_1_2B[0]*e_1_2B[2]*E1[3]*E2[6])/3 + (4*e_1_2B[0]*e_1_2B[6]*E1[5]*E2[0])/3;
                p21[7] = 0.0;
                p21[8] = (4*E1[3]*E2[0]*e_1_2B[0]*e_1_2B[0])/3 - (e_B[0]*E1[3]*E2[0])/3;

                p22[0] = (4*e_1_2B[4]*e_1_2B[4]*E1[4]*E2[4])/3 - (e_B[5]*E1[4]*E2[7])/3 - (e_B[7]*E1[5]*E2[4])/3 - (e_B[8]*E1[5]*E2[7])/3 - (e_B[4]*E1[4]*E2[4])/3 + (4*e_1_2B[8]*e_1_2B[8]*E1[5]*E2[7])/3 + (4*e_1_2B[4]*e_1_2B[5]*E1[4]*E2[7])/3 + (4*e_1_2B[4]*e_1_2B[7]*E1[5]*E2[4])/3 + (4*e_1_2B[5]*e_1_2B[7]*E1[4]*E2[4])/3 + (4*e_1_2B[5]*e_1_2B[7]*E1[5]*E2[7])/3 + (4*e_1_2B[5]*e_1_2B[8]*E1[4]*E2[7])/3 + (4*e_1_2B[7]*e_1_2B[8]*E1[5]*E2[4])/3;
                p22[1] = 0.0;
                p22[2] = (4*e_1_2B[1]*e_1_2B[4]*E1[3]*E2[4])/3 + (4*e_1_2B[3]*e_1_2B[4]*E1[4]*E2[1])/3 + (4*e_1_2B[1]*e_1_2B[5]*E1[3]*E2[7])/3 + (4*e_1_2B[2]*e_1_2B[7]*E1[3]*E2[4])/3 + (4*e_1_2B[3]*e_1_2B[7]*E1[5]*E2[1])/3 + (4*e_1_2B[5]*e_1_2B[6]*E1[4]*E2[1])/3 + (4*e_1_2B[2]*e_1_2B[8]*E1[3]*E2[7])/3 + (4*e_1_2B[6]*e_1_2B[8]*E1[5]*E2[1])/3;
                p22[3] = 0.0;
                p22[4] = (4*e_1_2B[1]*e_1_2B[3]*E1[3]*E2[1])/3 - (e_B[3]*E1[4]*E2[1])/3 - (e_B[2]*E1[3]*E2[7])/3 - (e_B[6]*E1[5]*E2[1])/3 - (e_B[1]*E1[3]*E2[4])/3 + (4*e_1_2B[1]*e_1_2B[3]*E1[4]*E2[4])/3 + (4*e_1_2B[2]*e_1_2B[6]*E1[3]*E2[1])/3 + (4*e_1_2B[1]*e_1_2B[6]*E1[5]*E2[4])/3 + (4*e_1_2B[2]*e_1_2B[3]*E1[4]*E2[7])/3 + (4*e_1_2B[2]*e_1_2B[6]*E1[5]*E2[7])/3;
                p22[5] = 0.0;
                p22[6] = (4*e_1_2B[0]*e_1_2B[1]*E1[3]*E2[4])/3 + (4*e_1_2B[0]*e_1_2B[3]*E1[4]*E2[1])/3 + (4*e_1_2B[0]*e_1_2B[2]*E1[3]*E2[7])/3 + (4*e_1_2B[0]*e_1_2B[6]*E1[5]*E2[1])/3;
                p22[7] = 0.0;
                p22[8] = (4*E1[3]*E2[1]*e_1_2B[0]*e_1_2B[0])/3 - (e_B[0]*E1[3]*E2[1])/3;

                p23[0] = (4*e_1_2B[4]*e_1_2B[4]*E1[4]*E2[5])/3 - (e_B[5]*E1[4]*E2[8])/3 - (e_B[7]*E1[5]*E2[5])/3 - (e_B[8]*E1[5]*E2[8])/3 - (e_B[4]*E1[4]*E2[5])/3 + (4*e_1_2B[8]*e_1_2B[8]*E1[5]*E2[8])/3 + (4*e_1_2B[4]*e_1_2B[5]*E1[4]*E2[8])/3 + (4*e_1_2B[4]*e_1_2B[7]*E1[5]*E2[5])/3 + (4*e_1_2B[5]*e_1_2B[7]*E1[4]*E2[5])/3 + (4*e_1_2B[5]*e_1_2B[7]*E1[5]*E2[8])/3 + (4*e_1_2B[5]*e_1_2B[8]*E1[4]*E2[8])/3 + (4*e_1_2B[7]*e_1_2B[8]*E1[5]*E2[5])/3;
                p23[1] = 0.0;
                p23[2] = (4*e_1_2B[1]*e_1_2B[4]*E1[3]*E2[5])/3 + (4*e_1_2B[3]*e_1_2B[4]*E1[4]*E2[2])/3 + (4*e_1_2B[1]*e_1_2B[5]*E1[3]*E2[8])/3 + (4*e_1_2B[2]*e_1_2B[7]*E1[3]*E2[5])/3 + (4*e_1_2B[3]*e_1_2B[7]*E1[5]*E2[2])/3 + (4*e_1_2B[5]*e_1_2B[6]*E1[4]*E2[2])/3 + (4*e_1_2B[2]*e_1_2B[8]*E1[3]*E2[8])/3 + (4*e_1_2B[6]*e_1_2B[8]*E1[5]*E2[2])/3;
                p23[3] = 0.0;
                p23[4] = (4*e_1_2B[1]*e_1_2B[3]*E1[3]*E2[2])/3 - (e_B[3]*E1[4]*E2[2])/3 - (e_B[2]*E1[3]*E2[8])/3 - (e_B[6]*E1[5]*E2[2])/3 - (e_B[1]*E1[3]*E2[5])/3 + (4*e_1_2B[1]*e_1_2B[3]*E1[4]*E2[5])/3 + (4*e_1_2B[2]*e_1_2B[6]*E1[3]*E2[2])/3 + (4*e_1_2B[1]*e_1_2B[6]*E1[5]*E2[5])/3 + (4*e_1_2B[2]*e_1_2B[3]*E1[4]*E2[8])/3 + (4*e_1_2B[2]*e_1_2B[6]*E1[5]*E2[8])/3;
                p23[5] = 0.0;
                p23[6] = (4*e_1_2B[0]*e_1_2B[1]*E1[3]*E2[5])/3 + (4*e_1_2B[0]*e_1_2B[3]*E1[4]*E2[2])/3 + (4*e_1_2B[0]*e_1_2B[2]*E1[3]*E2[8])/3 + (4*e_1_2B[0]*e_1_2B[6]*E1[5]*E2[2])/3;
                p23[7] = 0.0;
                p23[8] = (4*E1[3]*E2[2]*e_1_2B[0]*e_1_2B[0])/3 - (e_B[0]*E1[3]*E2[2])/3;

                p31[0] = (4*e_1_2B[4]*e_1_2B[4]*E1[7]*E2[3])/3 - (e_B[5]*E1[7]*E2[6])/3 - (e_B[7]*E1[8]*E2[3])/3 - (e_B[8]*E1[8]*E2[6])/3 - (e_B[4]*E1[7]*E2[3])/3 + (4*e_1_2B[8]*e_1_2B[8]*E1[8]*E2[6])/3 + (4*e_1_2B[4]*e_1_2B[5]*E1[7]*E2[6])/3 + (4*e_1_2B[4]*e_1_2B[7]*E1[8]*E2[3])/3 + (4*e_1_2B[5]*e_1_2B[7]*E1[7]*E2[3])/3 + (4*e_1_2B[5]*e_1_2B[7]*E1[8]*E2[6])/3 + (4*e_1_2B[5]*e_1_2B[8]*E1[7]*E2[6])/3 + (4*e_1_2B[7]*e_1_2B[8]*E1[8]*E2[3])/3;
                p31[1] = 0.0;
                p31[2] = (4*e_1_2B[1]*e_1_2B[4]*E1[6]*E2[3])/3 + (4*e_1_2B[3]*e_1_2B[4]*E1[7]*E2[0])/3 + (4*e_1_2B[1]*e_1_2B[5]*E1[6]*E2[6])/3 + (4*e_1_2B[2]*e_1_2B[7]*E1[6]*E2[3])/3 + (4*e_1_2B[3]*e_1_2B[7]*E1[8]*E2[0])/3 + (4*e_1_2B[5]*e_1_2B[6]*E1[7]*E2[0])/3 + (4*e_1_2B[2]*e_1_2B[8]*E1[6]*E2[6])/3 + (4*e_1_2B[6]*e_1_2B[8]*E1[8]*E2[0])/3;
                p31[3] = 0.0;
                p31[4] = (4*e_1_2B[1]*e_1_2B[3]*E1[6]*E2[0])/3 - (e_B[3]*E1[7]*E2[0])/3 - (e_B[2]*E1[6]*E2[6])/3 - (e_B[6]*E1[8]*E2[0])/3 - (e_B[1]*E1[6]*E2[3])/3 + (4*e_1_2B[1]*e_1_2B[3]*E1[7]*E2[3])/3 + (4*e_1_2B[2]*e_1_2B[6]*E1[6]*E2[0])/3 + (4*e_1_2B[1]*e_1_2B[6]*E1[8]*E2[3])/3 + (4*e_1_2B[2]*e_1_2B[3]*E1[7]*E2[6])/3 + (4*e_1_2B[2]*e_1_2B[6]*E1[8]*E2[6])/3;
                p31[5] = 0.0;
                p31[6] = (4*e_1_2B[0]*e_1_2B[1]*E1[6]*E2[3])/3 + (4*e_1_2B[0]*e_1_2B[3]*E1[7]*E2[0])/3 + (4*e_1_2B[0]*e_1_2B[2]*E1[6]*E2[6])/3 + (4*e_1_2B[0]*e_1_2B[6]*E1[8]*E2[0])/3;
                p31[7] = 0.0;
                p31[8] = (4*E1[6]*E2[0]*e_1_2B[0]*e_1_2B[0])/3 - (e_B[0]*E1[6]*E2[0])/3;

                p32[0] = (4*e_1_2B[4]*e_1_2B[4]*E1[7]*E2[4])/3 - (e_B[5]*E1[7]*E2[7])/3 - (e_B[7]*E1[8]*E2[4])/3 - (e_B[8]*E1[8]*E2[7])/3 - (e_B[4]*E1[7]*E2[4])/3 + (4*e_1_2B[8]*e_1_2B[8]*E1[8]*E2[7])/3 + (4*e_1_2B[4]*e_1_2B[5]*E1[7]*E2[7])/3 + (4*e_1_2B[4]*e_1_2B[7]*E1[8]*E2[4])/3 + (4*e_1_2B[5]*e_1_2B[7]*E1[7]*E2[4])/3 + (4*e_1_2B[5]*e_1_2B[7]*E1[8]*E2[7])/3 + (4*e_1_2B[5]*e_1_2B[8]*E1[7]*E2[7])/3 + (4*e_1_2B[7]*e_1_2B[8]*E1[8]*E2[4])/3;
                p32[1] = 0.0;
                p32[2] = (4*e_1_2B[1]*e_1_2B[4]*E1[6]*E2[4])/3 + (4*e_1_2B[3]*e_1_2B[4]*E1[7]*E2[1])/3 + (4*e_1_2B[1]*e_1_2B[5]*E1[6]*E2[7])/3 + (4*e_1_2B[2]*e_1_2B[7]*E1[6]*E2[4])/3 + (4*e_1_2B[3]*e_1_2B[7]*E1[8]*E2[1])/3 + (4*e_1_2B[5]*e_1_2B[6]*E1[7]*E2[1])/3 + (4*e_1_2B[2]*e_1_2B[8]*E1[6]*E2[7])/3 + (4*e_1_2B[6]*e_1_2B[8]*E1[8]*E2[1])/3;
                p32[3] = 0.0;
                p32[4] = (4*e_1_2B[1]*e_1_2B[3]*E1[6]*E2[1])/3 - (e_B[3]*E1[7]*E2[1])/3 - (e_B[2]*E1[6]*E2[7])/3 - (e_B[6]*E1[8]*E2[1])/3 - (e_B[1]*E1[6]*E2[4])/3 + (4*e_1_2B[1]*e_1_2B[3]*E1[7]*E2[4])/3 + (4*e_1_2B[2]*e_1_2B[6]*E1[6]*E2[1])/3 + (4*e_1_2B[1]*e_1_2B[6]*E1[8]*E2[4])/3 + (4*e_1_2B[2]*e_1_2B[3]*E1[7]*E2[7])/3 + (4*e_1_2B[2]*e_1_2B[6]*E1[8]*E2[7])/3;
                p32[5] = 0.0;
                p32[6] = (4*e_1_2B[0]*e_1_2B[1]*E1[6]*E2[4])/3 + (4*e_1_2B[0]*e_1_2B[3]*E1[7]*E2[1])/3 + (4*e_1_2B[0]*e_1_2B[2]*E1[6]*E2[7])/3 + (4*e_1_2B[0]*e_1_2B[6]*E1[8]*E2[1])/3;
                p32[7] = 0.0;
                p32[8] = (4*E1[6]*E2[1]*e_1_2B[0]*e_1_2B[0])/3 - (e_B[0]*E1[6]*E2[1])/3;

                p33[0] = (4*e_1_2B[4]*e_1_2B[4]*E1[7]*E2[5])/3 - (e_B[5]*E1[7]*E2[8])/3 - (e_B[7]*E1[8]*E2[5])/3 - (e_B[8]*E1[8]*E2[8])/3 - (e_B[4]*E1[7]*E2[5])/3 + (4*e_1_2B[8]*e_1_2B[8]*E1[8]*E2[8])/3 + (4*e_1_2B[4]*e_1_2B[5]*E1[7]*E2[8])/3 + (4*e_1_2B[4]*e_1_2B[7]*E1[8]*E2[5])/3 + (4*e_1_2B[5]*e_1_2B[7]*E1[7]*E2[5])/3 + (4*e_1_2B[5]*e_1_2B[7]*E1[8]*E2[8])/3 + (4*e_1_2B[5]*e_1_2B[8]*E1[7]*E2[8])/3 + (4*e_1_2B[7]*e_1_2B[8]*E1[8]*E2[5])/3;
                p33[1] = 0.0;
                p33[2] = (4*e_1_2B[1]*e_1_2B[4]*E1[6]*E2[5])/3 + (4*e_1_2B[3]*e_1_2B[4]*E1[7]*E2[2])/3 + (4*e_1_2B[1]*e_1_2B[5]*E1[6]*E2[8])/3 + (4*e_1_2B[2]*e_1_2B[7]*E1[6]*E2[5])/3 + (4*e_1_2B[3]*e_1_2B[7]*E1[8]*E2[2])/3 + (4*e_1_2B[5]*e_1_2B[6]*E1[7]*E2[2])/3 + (4*e_1_2B[2]*e_1_2B[8]*E1[6]*E2[8])/3 + (4*e_1_2B[6]*e_1_2B[8]*E1[8]*E2[2])/3;
                p33[3] = 0.0;
                p33[4] = (4*e_1_2B[1]*e_1_2B[3]*E1[6]*E2[2])/3 - (e_B[3]*E1[7]*E2[2])/3 - (e_B[2]*E1[6]*E2[8])/3 - (e_B[6]*E1[8]*E2[2])/3 - (e_B[1]*E1[6]*E2[5])/3 + (4*e_1_2B[1]*e_1_2B[3]*E1[7]*E2[5])/3 + (4*e_1_2B[2]*e_1_2B[6]*E1[6]*E2[2])/3 + (4*e_1_2B[1]*e_1_2B[6]*E1[8]*E2[5])/3 + (4*e_1_2B[2]*e_1_2B[3]*E1[7]*E2[8])/3 + (4*e_1_2B[2]*e_1_2B[6]*E1[8]*E2[8])/3;
                p33[5] = 0.0;
                p33[6] = (4*e_1_2B[0]*e_1_2B[1]*E1[6]*E2[5])/3 + (4*e_1_2B[0]*e_1_2B[3]*E1[7]*E2[2])/3 + (4*e_1_2B[0]*e_1_2B[2]*E1[6]*E2[8])/3 + (4*e_1_2B[0]*e_1_2B[6]*E1[8]*E2[2])/3;
                p33[7] = 0.0;
                p33[8] = (4*E1[6]*E2[2]*e_1_2B[0]*e_1_2B[0])/3 - (e_B[0]*E1[6]*E2[2])/3;

                p11 += 9;
                p12 += 9;
                p13 += 9;
                p21 += 9;
                p22 += 9;
                p23 += 9;
                p31 += 9;
                p32 += 9;
                p33 += 9;            
            }
            break;

        case manakov_discretization_FTES4_4B: // Check if there is not yet a type for this.
                                            // _4 denotes the 4th order splitting scheme

                                            // TODO: change def. of z so we have 4th order coefficients instead of 8

            e_1_2B = &e_Bstorage[0];    // exp(Bh/2)
            e_1_4B = &e_Bstorage[9];    // exp(Bh/4)
            E1 = &E_storage[0];
            E2 = &E_storage[9];

            for (i=D_eff; i-->0;){
                manakov_fscatter_zero_freq_scatter_matrix(e_1_2B, eps_t/2, q1[i], q2[i], kappa);
                manakov_fscatter_zero_freq_scatter_matrix(e_1_4B, eps_t/4, q1[i], q2[i], kappa);
                
                // Setting which elements of q we use
                q1n[1] = q1[i];
                q2n[1] = q2[i];
                
                if (i==0){  //left bnd condition
                q1n[0] = q1[i];      // set q1[n-1] to q[n] on boundary
                q2n[0] = q2[i];
                }
                else {
                q1n[0] = q1[i-1];
                q2n[0] = q2[i-1];
                }

                if (i==D_eff-1){  //right bnd condition
                q1n[2] = q1[i];      // set q1[n+1] to q[n] on boundary
                q2n[2] = q2[i];
                }
                else{
                q1n[2] = q1[i+1];
                q2n[2] = q2[i+1];
                }

                manakov_fscatter_get_E(E1, eps_t, (q1n[2]-q1n[0])*eps_t/24+(q1n[2]-2*q1n[1]+q1n[0])*eps_t/48,
                                (q2n[2]-q2n[0])*eps_t/24+(q2n[2]-2*q2n[1]+q2n[0])*eps_t/48,
                                kappa*CONJ(q1n[0]-q1n[2])*eps_t/24-kappa*CONJ(q1n[2]-2*q1n[1]+q1n[0])*eps_t/48,
                                kappa*CONJ(q2n[0]-q2n[2])*eps_t/24-kappa*CONJ(q2n[2]-2*q2n[1]+q2n[0])*eps_t/48);

                manakov_fscatter_get_E(E2, eps_t, -(q1n[2]-q1n[0])*eps_t/24+(q1n[2]-2*q1n[1]+q1n[0])*eps_t/48,
                                -(q2n[2]-q2n[0])*eps_t/24+(q2n[2]-2*q2n[1]+q2n[0])*eps_t/48,
                                kappa*CONJ(q1n[2]-q1n[0])*eps_t/24-kappa*CONJ(q1n[2]-2*q1n[1]+q1n[0])*eps_t/48,
                                kappa*CONJ(q2n[2]-q2n[0])*eps_t/24-kappa*CONJ(q2n[2]-2*q2n[1]+q2n[0])*eps_t/48);

                p11[0] = (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[4]*E1[1]*E2[3])/3 - (e_1_2B[8]*e_1_2B[8]*E1[2]*E2[6])/3 - (e_1_2B[1]*e_1_2B[3]*E1[0]*E2[0])/3 - (e_1_2B[1]*e_1_2B[4]*E1[0]*E2[3])/3 - (e_1_2B[2]*e_1_2B[6]*E1[0]*E2[0])/3 - (e_1_2B[3]*e_1_2B[4]*E1[1]*E2[0])/3 - (e_1_2B[1]*e_1_2B[5]*E1[0]*E2[6])/3 - (e_1_2B[2]*e_1_2B[7]*E1[0]*E2[3])/3 - (e_1_2B[3]*e_1_2B[7]*E1[2]*E2[0])/3 - (e_1_2B[5]*e_1_2B[6]*E1[1]*E2[0])/3 - (e_1_2B[2]*e_1_2B[8]*E1[0]*E2[6])/3 - (e_1_2B[4]*e_1_2B[5]*E1[1]*E2[6])/3 - (e_1_2B[4]*e_1_2B[7]*E1[2]*E2[3])/3 - (e_1_2B[5]*e_1_2B[7]*E1[1]*E2[3])/3 - (e_1_2B[6]*e_1_2B[8]*E1[2]*E2[0])/3 - (e_1_2B[5]*e_1_2B[7]*E1[2]*E2[6])/3 - (e_1_2B[5]*e_1_2B[8]*E1[1]*E2[6])/3 - (e_1_2B[7]*e_1_2B[8]*E1[2]*E2[3])/3 - (e_1_2B[4]*e_1_2B[4]*E1[1]*E2[3])/3 + (4*e_1_2B[5]*e_1_4B[7]*e_1_4B[7]*E1[2]*E2[3])/3 + (4*e_1_2B[7]*e_1_4B[5]*e_1_4B[5]*E1[1]*E2[6])/3 + (4*e_1_2B[8]*e_1_4B[8]*e_1_4B[8]*E1[2]*E2[6])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[3]*E1[0]*E2[0])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[4]*E1[0]*E2[3])/3 + (4*e_1_2B[4]*e_1_4B[3]*e_1_4B[4]*E1[1]*E2[0])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[6]*E1[0]*E2[0])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[3]*E1[0]*E2[0])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[5]*E1[0]*E2[6])/3 + (4*e_1_2B[4]*e_1_4B[3]*e_1_4B[7]*E1[2]*E2[0])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[7]*E1[0]*E2[3])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[6]*E1[1]*E2[0])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[4]*E1[0]*E2[3])/3 + (4*e_1_2B[7]*e_1_4B[3]*e_1_4B[5]*E1[1]*E2[0])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[6]*E1[0]*E2[0])/3 + (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[5]*E1[1]*E2[6])/3 + (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[7]*E1[2]*E2[3])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[8]*E1[0]*E2[6])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[7]*E1[1]*E2[3])/3 + (4*e_1_2B[5]*e_1_4B[6]*e_1_4B[7]*E1[2]*E2[0])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[5]*E1[0]*E2[6])/3 + (4*e_1_2B[7]*e_1_4B[3]*e_1_4B[8]*E1[2]*E2[0])/3 + (4*e_1_2B[7]*e_1_4B[4]*e_1_4B[5]*E1[1]*E2[3])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[7]*E1[0]*E2[3])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[6]*E1[1]*E2[0])/3 + (4*e_1_2B[4]*e_1_4B[5]*e_1_4B[7]*E1[2]*E2[6])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[8]*E1[1]*E2[6])/3 + (4*e_1_2B[7]*e_1_4B[4]*e_1_4B[8]*E1[2]*E2[3])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[8]*E1[0]*E2[6])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[7]*E1[1]*E2[3])/3 + (4*e_1_2B[8]*e_1_4B[6]*e_1_4B[8]*E1[2]*E2[0])/3 + (4*e_1_2B[5]*e_1_4B[7]*e_1_4B[8]*E1[2]*E2[6])/3 + (4*e_1_2B[7]*e_1_4B[5]*e_1_4B[8]*E1[2]*E2[6])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[8]*E1[1]*E2[6])/3 + (4*e_1_2B[8]*e_1_4B[7]*e_1_4B[8]*E1[2]*E2[3])/3;   // z^8
                p11[1] = 0.0;   // z^7
                p11[2] = 0.0;   // z^6
                p11[3] = 0.0;   // z^5 
                p11[4] = (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[3]*E1[1]*E2[0])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[1]*E1[0]*E2[3])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[6]*E1[2]*E2[0])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[2]*E1[0]*E2[6])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[3]*E1[0]*E2[0])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[1]*E1[0]*E2[0])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[4]*E1[0]*E2[3])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[6]*E1[0]*E2[0])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[4]*E1[1]*E2[0])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[2]*E1[0]*E2[0])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[5]*E1[0]*E2[6])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[4]*E1[1]*E2[3])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[6]*E1[2]*E2[0])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[7]*E1[0]*E2[3])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[6]*E1[1]*E2[0])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[7]*E1[2]*E2[0])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[2]*E1[0]*E2[6])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[4]*E1[1]*E2[3])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[5]*E1[1]*E2[0])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[2]*E1[0]*E2[3])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[5]*E1[1]*E2[6])/3 + (4*e_1_2B[1]*e_1_4B[4]*e_1_4B[6]*E1[2]*E2[3])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[8]*E1[0]*E2[6])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[7]*E1[1]*E2[3])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[7]*E1[2]*E2[3])/3 + (4*e_1_2B[3]*e_1_4B[2]*e_1_4B[4]*E1[1]*E2[6])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[8]*E1[2]*E2[0])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[5]*E1[1]*E2[3])/3 + (4*e_1_2B[1]*e_1_4B[5]*e_1_4B[6]*E1[2]*E2[6])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[8]*E1[1]*E2[6])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[7]*E1[2]*E2[3])/3 + (4*e_1_2B[3]*e_1_4B[2]*e_1_4B[7]*E1[2]*E2[6])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[8]*E1[2]*E2[3])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[5]*E1[1]*E2[6])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[8]*E1[2]*E2[6])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[8]*E1[2]*E2[6])/3;     // z^4
                p11[5] = 0.0;   // z^3
                p11[6] = 0.0;   // z^2
                p11[7] = 0.0;   // z^1
                p11[8] = (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[0]*E1[0]*E2[0])/3 - (e_1_2B[0]*e_1_2B[1]*E1[0]*E2[3])/3 - (e_1_2B[0]*e_1_2B[3]*E1[1]*E2[0])/3 - (e_1_2B[0]*e_1_2B[2]*E1[0]*E2[6])/3 - (e_1_2B[0]*e_1_2B[6]*E1[2]*E2[0])/3 - (e_1_2B[1]*e_1_2B[3]*E1[1]*E2[3])/3 - (e_1_2B[1]*e_1_2B[6]*E1[2]*E2[3])/3 - (e_1_2B[2]*e_1_2B[3]*E1[1]*E2[6])/3 - (e_1_2B[2]*e_1_2B[6]*E1[2]*E2[6])/3 - (e_1_2B[0]*e_1_2B[0]*E1[0]*E2[0])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[1]*E1[0]*E2[3])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[3]*E1[1]*E2[0])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[2]*E1[0]*E2[6])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[6]*E1[2]*E2[0])/3 + (4*e_1_2B[0]*e_1_4B[1]*e_1_4B[3]*E1[1]*E2[3])/3 + (4*e_1_2B[0]*e_1_4B[1]*e_1_4B[6]*E1[2]*E2[3])/3 + (4*e_1_2B[0]*e_1_4B[2]*e_1_4B[3]*E1[1]*E2[6])/3 + (4*e_1_2B[0]*e_1_4B[2]*e_1_4B[6]*E1[2]*E2[6])/3; // z^0

                p12[0] = (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[4]*E1[1]*E2[4])/3 - (e_1_2B[8]*e_1_2B[8]*E1[2]*E2[7])/3 - (e_1_2B[1]*e_1_2B[3]*E1[0]*E2[1])/3 - (e_1_2B[1]*e_1_2B[4]*E1[0]*E2[4])/3 - (e_1_2B[2]*e_1_2B[6]*E1[0]*E2[1])/3 - (e_1_2B[3]*e_1_2B[4]*E1[1]*E2[1])/3 - (e_1_2B[1]*e_1_2B[5]*E1[0]*E2[7])/3 - (e_1_2B[2]*e_1_2B[7]*E1[0]*E2[4])/3 - (e_1_2B[3]*e_1_2B[7]*E1[2]*E2[1])/3 - (e_1_2B[5]*e_1_2B[6]*E1[1]*E2[1])/3 - (e_1_2B[2]*e_1_2B[8]*E1[0]*E2[7])/3 - (e_1_2B[4]*e_1_2B[5]*E1[1]*E2[7])/3 - (e_1_2B[4]*e_1_2B[7]*E1[2]*E2[4])/3 - (e_1_2B[5]*e_1_2B[7]*E1[1]*E2[4])/3 - (e_1_2B[6]*e_1_2B[8]*E1[2]*E2[1])/3 - (e_1_2B[5]*e_1_2B[7]*E1[2]*E2[7])/3 - (e_1_2B[5]*e_1_2B[8]*E1[1]*E2[7])/3 - (e_1_2B[7]*e_1_2B[8]*E1[2]*E2[4])/3 - (e_1_2B[4]*e_1_2B[4]*E1[1]*E2[4])/3 + (4*e_1_2B[5]*e_1_4B[7]*e_1_4B[7]*E1[2]*E2[4])/3 + (4*e_1_2B[7]*e_1_4B[5]*e_1_4B[5]*E1[1]*E2[7])/3 + (4*e_1_2B[8]*e_1_4B[8]*e_1_4B[8]*E1[2]*E2[7])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[3]*E1[0]*E2[1])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[4]*E1[0]*E2[4])/3 + (4*e_1_2B[4]*e_1_4B[3]*e_1_4B[4]*E1[1]*E2[1])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[6]*E1[0]*E2[1])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[3]*E1[0]*E2[1])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[5]*E1[0]*E2[7])/3 + (4*e_1_2B[4]*e_1_4B[3]*e_1_4B[7]*E1[2]*E2[1])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[7]*E1[0]*E2[4])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[6]*E1[1]*E2[1])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[4]*E1[0]*E2[4])/3 + (4*e_1_2B[7]*e_1_4B[3]*e_1_4B[5]*E1[1]*E2[1])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[6]*E1[0]*E2[1])/3 + (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[5]*E1[1]*E2[7])/3 + (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[7]*E1[2]*E2[4])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[8]*E1[0]*E2[7])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[7]*E1[1]*E2[4])/3 + (4*e_1_2B[5]*e_1_4B[6]*e_1_4B[7]*E1[2]*E2[1])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[5]*E1[0]*E2[7])/3 + (4*e_1_2B[7]*e_1_4B[3]*e_1_4B[8]*E1[2]*E2[1])/3 + (4*e_1_2B[7]*e_1_4B[4]*e_1_4B[5]*E1[1]*E2[4])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[7]*E1[0]*E2[4])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[6]*E1[1]*E2[1])/3 + (4*e_1_2B[4]*e_1_4B[5]*e_1_4B[7]*E1[2]*E2[7])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[8]*E1[1]*E2[7])/3 + (4*e_1_2B[7]*e_1_4B[4]*e_1_4B[8]*E1[2]*E2[4])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[8]*E1[0]*E2[7])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[7]*E1[1]*E2[4])/3 + (4*e_1_2B[8]*e_1_4B[6]*e_1_4B[8]*E1[2]*E2[1])/3 + (4*e_1_2B[5]*e_1_4B[7]*e_1_4B[8]*E1[2]*E2[7])/3 + (4*e_1_2B[7]*e_1_4B[5]*e_1_4B[8]*E1[2]*E2[7])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[8]*E1[1]*E2[7])/3 + (4*e_1_2B[8]*e_1_4B[7]*e_1_4B[8]*E1[2]*E2[4])/3;
                p12[1] = 0.0;
                p12[2] = 0.0;
                p12[3] = 0.0;
                p12[4] = (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[3]*E1[1]*E2[1])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[1]*E1[0]*E2[4])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[6]*E1[2]*E2[1])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[2]*E1[0]*E2[7])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[3]*E1[0]*E2[1])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[1]*E1[0]*E2[1])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[4]*E1[0]*E2[4])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[6]*E1[0]*E2[1])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[4]*E1[1]*E2[1])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[2]*E1[0]*E2[1])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[5]*E1[0]*E2[7])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[4]*E1[1]*E2[4])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[6]*E1[2]*E2[1])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[7]*E1[0]*E2[4])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[6]*E1[1]*E2[1])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[7]*E1[2]*E2[1])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[2]*E1[0]*E2[7])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[4]*E1[1]*E2[4])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[5]*E1[1]*E2[1])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[2]*E1[0]*E2[4])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[5]*E1[1]*E2[7])/3 + (4*e_1_2B[1]*e_1_4B[4]*e_1_4B[6]*E1[2]*E2[4])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[8]*E1[0]*E2[7])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[7]*E1[1]*E2[4])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[7]*E1[2]*E2[4])/3 + (4*e_1_2B[3]*e_1_4B[2]*e_1_4B[4]*E1[1]*E2[7])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[8]*E1[2]*E2[1])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[5]*E1[1]*E2[4])/3 + (4*e_1_2B[1]*e_1_4B[5]*e_1_4B[6]*E1[2]*E2[7])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[8]*E1[1]*E2[7])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[7]*E1[2]*E2[4])/3 + (4*e_1_2B[3]*e_1_4B[2]*e_1_4B[7]*E1[2]*E2[7])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[8]*E1[2]*E2[4])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[5]*E1[1]*E2[7])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[8]*E1[2]*E2[7])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[8]*E1[2]*E2[7])/3;
                p12[5] = 0.0;
                p12[6] = 0.0;
                p12[7] = 0.0;
                p12[8] = (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[0]*E1[0]*E2[1])/3 - (e_1_2B[0]*e_1_2B[1]*E1[0]*E2[4])/3 - (e_1_2B[0]*e_1_2B[3]*E1[1]*E2[1])/3 - (e_1_2B[0]*e_1_2B[2]*E1[0]*E2[7])/3 - (e_1_2B[0]*e_1_2B[6]*E1[2]*E2[1])/3 - (e_1_2B[1]*e_1_2B[3]*E1[1]*E2[4])/3 - (e_1_2B[1]*e_1_2B[6]*E1[2]*E2[4])/3 - (e_1_2B[2]*e_1_2B[3]*E1[1]*E2[7])/3 - (e_1_2B[2]*e_1_2B[6]*E1[2]*E2[7])/3 - (e_1_2B[0]*e_1_2B[0]*E1[0]*E2[1])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[1]*E1[0]*E2[4])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[3]*E1[1]*E2[1])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[2]*E1[0]*E2[7])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[6]*E1[2]*E2[1])/3 + (4*e_1_2B[0]*e_1_4B[1]*e_1_4B[3]*E1[1]*E2[4])/3 + (4*e_1_2B[0]*e_1_4B[1]*e_1_4B[6]*E1[2]*E2[4])/3 + (4*e_1_2B[0]*e_1_4B[2]*e_1_4B[3]*E1[1]*E2[7])/3 + (4*e_1_2B[0]*e_1_4B[2]*e_1_4B[6]*E1[2]*E2[7])/3;

                p13[0] = (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[4]*E1[1]*E2[5])/3 - (e_1_2B[8]*e_1_2B[8]*E1[2]*E2[8])/3 - (e_1_2B[1]*e_1_2B[3]*E1[0]*E2[2])/3 - (e_1_2B[1]*e_1_2B[4]*E1[0]*E2[5])/3 - (e_1_2B[2]*e_1_2B[6]*E1[0]*E2[2])/3 - (e_1_2B[3]*e_1_2B[4]*E1[1]*E2[2])/3 - (e_1_2B[1]*e_1_2B[5]*E1[0]*E2[8])/3 - (e_1_2B[2]*e_1_2B[7]*E1[0]*E2[5])/3 - (e_1_2B[3]*e_1_2B[7]*E1[2]*E2[2])/3 - (e_1_2B[5]*e_1_2B[6]*E1[1]*E2[2])/3 - (e_1_2B[2]*e_1_2B[8]*E1[0]*E2[8])/3 - (e_1_2B[4]*e_1_2B[5]*E1[1]*E2[8])/3 - (e_1_2B[4]*e_1_2B[7]*E1[2]*E2[5])/3 - (e_1_2B[5]*e_1_2B[7]*E1[1]*E2[5])/3 - (e_1_2B[6]*e_1_2B[8]*E1[2]*E2[2])/3 - (e_1_2B[5]*e_1_2B[7]*E1[2]*E2[8])/3 - (e_1_2B[5]*e_1_2B[8]*E1[1]*E2[8])/3 - (e_1_2B[7]*e_1_2B[8]*E1[2]*E2[5])/3 - (e_1_2B[4]*e_1_2B[4]*E1[1]*E2[5])/3 + (4*e_1_2B[5]*e_1_4B[7]*e_1_4B[7]*E1[2]*E2[5])/3 + (4*e_1_2B[7]*e_1_4B[5]*e_1_4B[5]*E1[1]*E2[8])/3 + (4*e_1_2B[8]*e_1_4B[8]*e_1_4B[8]*E1[2]*E2[8])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[3]*E1[0]*E2[2])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[4]*E1[0]*E2[5])/3 + (4*e_1_2B[4]*e_1_4B[3]*e_1_4B[4]*E1[1]*E2[2])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[6]*E1[0]*E2[2])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[3]*E1[0]*E2[2])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[5]*E1[0]*E2[8])/3 + (4*e_1_2B[4]*e_1_4B[3]*e_1_4B[7]*E1[2]*E2[2])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[7]*E1[0]*E2[5])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[6]*E1[1]*E2[2])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[4]*E1[0]*E2[5])/3 + (4*e_1_2B[7]*e_1_4B[3]*e_1_4B[5]*E1[1]*E2[2])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[6]*E1[0]*E2[2])/3 + (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[5]*E1[1]*E2[8])/3 + (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[7]*E1[2]*E2[5])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[8]*E1[0]*E2[8])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[7]*E1[1]*E2[5])/3 + (4*e_1_2B[5]*e_1_4B[6]*e_1_4B[7]*E1[2]*E2[2])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[5]*E1[0]*E2[8])/3 + (4*e_1_2B[7]*e_1_4B[3]*e_1_4B[8]*E1[2]*E2[2])/3 + (4*e_1_2B[7]*e_1_4B[4]*e_1_4B[5]*E1[1]*E2[5])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[7]*E1[0]*E2[5])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[6]*E1[1]*E2[2])/3 + (4*e_1_2B[4]*e_1_4B[5]*e_1_4B[7]*E1[2]*E2[8])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[8]*E1[1]*E2[8])/3 + (4*e_1_2B[7]*e_1_4B[4]*e_1_4B[8]*E1[2]*E2[5])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[8]*E1[0]*E2[8])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[7]*E1[1]*E2[5])/3 + (4*e_1_2B[8]*e_1_4B[6]*e_1_4B[8]*E1[2]*E2[2])/3 + (4*e_1_2B[5]*e_1_4B[7]*e_1_4B[8]*E1[2]*E2[8])/3 + (4*e_1_2B[7]*e_1_4B[5]*e_1_4B[8]*E1[2]*E2[8])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[8]*E1[1]*E2[8])/3 + (4*e_1_2B[8]*e_1_4B[7]*e_1_4B[8]*E1[2]*E2[5])/3;
                p13[1] = 0.0;
                p13[2] = 0.0;
                p13[3] = 0.0;
                p13[4] = (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[3]*E1[1]*E2[2])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[1]*E1[0]*E2[5])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[6]*E1[2]*E2[2])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[2]*E1[0]*E2[8])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[3]*E1[0]*E2[2])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[1]*E1[0]*E2[2])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[4]*E1[0]*E2[5])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[6]*E1[0]*E2[2])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[4]*E1[1]*E2[2])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[2]*E1[0]*E2[2])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[5]*E1[0]*E2[8])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[4]*E1[1]*E2[5])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[6]*E1[2]*E2[2])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[7]*E1[0]*E2[5])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[6]*E1[1]*E2[2])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[7]*E1[2]*E2[2])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[2]*E1[0]*E2[8])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[4]*E1[1]*E2[5])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[5]*E1[1]*E2[2])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[2]*E1[0]*E2[5])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[5]*E1[1]*E2[8])/3 + (4*e_1_2B[1]*e_1_4B[4]*e_1_4B[6]*E1[2]*E2[5])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[8]*E1[0]*E2[8])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[7]*E1[1]*E2[5])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[7]*E1[2]*E2[5])/3 + (4*e_1_2B[3]*e_1_4B[2]*e_1_4B[4]*E1[1]*E2[8])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[8]*E1[2]*E2[2])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[5]*E1[1]*E2[5])/3 + (4*e_1_2B[1]*e_1_4B[5]*e_1_4B[6]*E1[2]*E2[8])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[8]*E1[1]*E2[8])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[7]*E1[2]*E2[5])/3 + (4*e_1_2B[3]*e_1_4B[2]*e_1_4B[7]*E1[2]*E2[8])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[8]*E1[2]*E2[5])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[5]*E1[1]*E2[8])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[8]*E1[2]*E2[8])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[8]*E1[2]*E2[8])/3;
                p13[5] = 0.0;
                p13[6] = 0.0;
                p13[7] = 0.0;
                p13[8] = (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[0]*E1[0]*E2[2])/3 - (e_1_2B[0]*e_1_2B[1]*E1[0]*E2[5])/3 - (e_1_2B[0]*e_1_2B[3]*E1[1]*E2[2])/3 - (e_1_2B[0]*e_1_2B[2]*E1[0]*E2[8])/3 - (e_1_2B[0]*e_1_2B[6]*E1[2]*E2[2])/3 - (e_1_2B[1]*e_1_2B[3]*E1[1]*E2[5])/3 - (e_1_2B[1]*e_1_2B[6]*E1[2]*E2[5])/3 - (e_1_2B[2]*e_1_2B[3]*E1[1]*E2[8])/3 - (e_1_2B[2]*e_1_2B[6]*E1[2]*E2[8])/3 - (e_1_2B[0]*e_1_2B[0]*E1[0]*E2[2])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[1]*E1[0]*E2[5])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[3]*E1[1]*E2[2])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[2]*E1[0]*E2[8])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[6]*E1[2]*E2[2])/3 + (4*e_1_2B[0]*e_1_4B[1]*e_1_4B[3]*E1[1]*E2[5])/3 + (4*e_1_2B[0]*e_1_4B[1]*e_1_4B[6]*E1[2]*E2[5])/3 + (4*e_1_2B[0]*e_1_4B[2]*e_1_4B[3]*E1[1]*E2[8])/3 + (4*e_1_2B[0]*e_1_4B[2]*e_1_4B[6]*E1[2]*E2[8])/3;

                p21[0] = (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[4]*E1[4]*E2[3])/3 - (e_1_2B[8]*e_1_2B[8]*E1[5]*E2[6])/3 - (e_1_2B[1]*e_1_2B[3]*E1[3]*E2[0])/3 - (e_1_2B[1]*e_1_2B[4]*E1[3]*E2[3])/3 - (e_1_2B[2]*e_1_2B[6]*E1[3]*E2[0])/3 - (e_1_2B[3]*e_1_2B[4]*E1[4]*E2[0])/3 - (e_1_2B[1]*e_1_2B[5]*E1[3]*E2[6])/3 - (e_1_2B[2]*e_1_2B[7]*E1[3]*E2[3])/3 - (e_1_2B[3]*e_1_2B[7]*E1[5]*E2[0])/3 - (e_1_2B[5]*e_1_2B[6]*E1[4]*E2[0])/3 - (e_1_2B[2]*e_1_2B[8]*E1[3]*E2[6])/3 - (e_1_2B[4]*e_1_2B[5]*E1[4]*E2[6])/3 - (e_1_2B[4]*e_1_2B[7]*E1[5]*E2[3])/3 - (e_1_2B[5]*e_1_2B[7]*E1[4]*E2[3])/3 - (e_1_2B[6]*e_1_2B[8]*E1[5]*E2[0])/3 - (e_1_2B[5]*e_1_2B[7]*E1[5]*E2[6])/3 - (e_1_2B[5]*e_1_2B[8]*E1[4]*E2[6])/3 - (e_1_2B[7]*e_1_2B[8]*E1[5]*E2[3])/3 - (e_1_2B[4]*e_1_2B[4]*E1[4]*E2[3])/3 + (4*e_1_2B[5]*e_1_4B[7]*e_1_4B[7]*E1[5]*E2[3])/3 + (4*e_1_2B[7]*e_1_4B[5]*e_1_4B[5]*E1[4]*E2[6])/3 + (4*e_1_2B[8]*e_1_4B[8]*e_1_4B[8]*E1[5]*E2[6])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[3]*E1[3]*E2[0])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[4]*E1[3]*E2[3])/3 + (4*e_1_2B[4]*e_1_4B[3]*e_1_4B[4]*E1[4]*E2[0])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[6]*E1[3]*E2[0])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[3]*E1[3]*E2[0])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[5]*E1[3]*E2[6])/3 + (4*e_1_2B[4]*e_1_4B[3]*e_1_4B[7]*E1[5]*E2[0])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[7]*E1[3]*E2[3])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[6]*E1[4]*E2[0])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[4]*E1[3]*E2[3])/3 + (4*e_1_2B[7]*e_1_4B[3]*e_1_4B[5]*E1[4]*E2[0])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[6]*E1[3]*E2[0])/3 + (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[5]*E1[4]*E2[6])/3 + (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[7]*E1[5]*E2[3])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[8]*E1[3]*E2[6])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[7]*E1[4]*E2[3])/3 + (4*e_1_2B[5]*e_1_4B[6]*e_1_4B[7]*E1[5]*E2[0])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[5]*E1[3]*E2[6])/3 + (4*e_1_2B[7]*e_1_4B[3]*e_1_4B[8]*E1[5]*E2[0])/3 + (4*e_1_2B[7]*e_1_4B[4]*e_1_4B[5]*E1[4]*E2[3])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[7]*E1[3]*E2[3])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[6]*E1[4]*E2[0])/3 + (4*e_1_2B[4]*e_1_4B[5]*e_1_4B[7]*E1[5]*E2[6])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[8]*E1[4]*E2[6])/3 + (4*e_1_2B[7]*e_1_4B[4]*e_1_4B[8]*E1[5]*E2[3])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[8]*E1[3]*E2[6])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[7]*E1[4]*E2[3])/3 + (4*e_1_2B[8]*e_1_4B[6]*e_1_4B[8]*E1[5]*E2[0])/3 + (4*e_1_2B[5]*e_1_4B[7]*e_1_4B[8]*E1[5]*E2[6])/3 + (4*e_1_2B[7]*e_1_4B[5]*e_1_4B[8]*E1[5]*E2[6])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[8]*E1[4]*E2[6])/3 + (4*e_1_2B[8]*e_1_4B[7]*e_1_4B[8]*E1[5]*E2[3])/3;
                p21[1] = 0.0;
                p21[2] = 0.0;
                p21[3] = 0.0;
                p21[4] = (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[3]*E1[4]*E2[0])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[1]*E1[3]*E2[3])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[6]*E1[5]*E2[0])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[2]*E1[3]*E2[6])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[3]*E1[3]*E2[0])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[1]*E1[3]*E2[0])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[4]*E1[3]*E2[3])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[6]*E1[3]*E2[0])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[4]*E1[4]*E2[0])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[2]*E1[3]*E2[0])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[5]*E1[3]*E2[6])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[4]*E1[4]*E2[3])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[6]*E1[5]*E2[0])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[7]*E1[3]*E2[3])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[6]*E1[4]*E2[0])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[7]*E1[5]*E2[0])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[2]*E1[3]*E2[6])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[4]*E1[4]*E2[3])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[5]*E1[4]*E2[0])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[2]*E1[3]*E2[3])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[5]*E1[4]*E2[6])/3 + (4*e_1_2B[1]*e_1_4B[4]*e_1_4B[6]*E1[5]*E2[3])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[8]*E1[3]*E2[6])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[7]*E1[4]*E2[3])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[7]*E1[5]*E2[3])/3 + (4*e_1_2B[3]*e_1_4B[2]*e_1_4B[4]*E1[4]*E2[6])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[8]*E1[5]*E2[0])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[5]*E1[4]*E2[3])/3 + (4*e_1_2B[1]*e_1_4B[5]*e_1_4B[6]*E1[5]*E2[6])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[8]*E1[4]*E2[6])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[7]*E1[5]*E2[3])/3 + (4*e_1_2B[3]*e_1_4B[2]*e_1_4B[7]*E1[5]*E2[6])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[8]*E1[5]*E2[3])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[5]*E1[4]*E2[6])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[8]*E1[5]*E2[6])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[8]*E1[5]*E2[6])/3;
                p21[5] = 0.0;
                p21[6] = 0.0;
                p21[7] = 0.0;
                p21[8] = (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[0]*E1[3]*E2[0])/3 - (e_1_2B[0]*e_1_2B[1]*E1[3]*E2[3])/3 - (e_1_2B[0]*e_1_2B[3]*E1[4]*E2[0])/3 - (e_1_2B[0]*e_1_2B[2]*E1[3]*E2[6])/3 - (e_1_2B[0]*e_1_2B[6]*E1[5]*E2[0])/3 - (e_1_2B[1]*e_1_2B[3]*E1[4]*E2[3])/3 - (e_1_2B[1]*e_1_2B[6]*E1[5]*E2[3])/3 - (e_1_2B[2]*e_1_2B[3]*E1[4]*E2[6])/3 - (e_1_2B[2]*e_1_2B[6]*E1[5]*E2[6])/3 - (e_1_2B[0]*e_1_2B[0]*E1[3]*E2[0])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[1]*E1[3]*E2[3])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[3]*E1[4]*E2[0])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[2]*E1[3]*E2[6])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[6]*E1[5]*E2[0])/3 + (4*e_1_2B[0]*e_1_4B[1]*e_1_4B[3]*E1[4]*E2[3])/3 + (4*e_1_2B[0]*e_1_4B[1]*e_1_4B[6]*E1[5]*E2[3])/3 + (4*e_1_2B[0]*e_1_4B[2]*e_1_4B[3]*E1[4]*E2[6])/3 + (4*e_1_2B[0]*e_1_4B[2]*e_1_4B[6]*E1[5]*E2[6])/3;

                p22[0] = (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[4]*E1[4]*E2[4])/3 - (e_1_2B[8]*e_1_2B[8]*E1[5]*E2[7])/3 - (e_1_2B[1]*e_1_2B[3]*E1[3]*E2[1])/3 - (e_1_2B[1]*e_1_2B[4]*E1[3]*E2[4])/3 - (e_1_2B[2]*e_1_2B[6]*E1[3]*E2[1])/3 - (e_1_2B[3]*e_1_2B[4]*E1[4]*E2[1])/3 - (e_1_2B[1]*e_1_2B[5]*E1[3]*E2[7])/3 - (e_1_2B[2]*e_1_2B[7]*E1[3]*E2[4])/3 - (e_1_2B[3]*e_1_2B[7]*E1[5]*E2[1])/3 - (e_1_2B[5]*e_1_2B[6]*E1[4]*E2[1])/3 - (e_1_2B[2]*e_1_2B[8]*E1[3]*E2[7])/3 - (e_1_2B[4]*e_1_2B[5]*E1[4]*E2[7])/3 - (e_1_2B[4]*e_1_2B[7]*E1[5]*E2[4])/3 - (e_1_2B[5]*e_1_2B[7]*E1[4]*E2[4])/3 - (e_1_2B[6]*e_1_2B[8]*E1[5]*E2[1])/3 - (e_1_2B[5]*e_1_2B[7]*E1[5]*E2[7])/3 - (e_1_2B[5]*e_1_2B[8]*E1[4]*E2[7])/3 - (e_1_2B[7]*e_1_2B[8]*E1[5]*E2[4])/3 - (e_1_2B[4]*e_1_2B[4]*E1[4]*E2[4])/3 + (4*e_1_2B[5]*e_1_4B[7]*e_1_4B[7]*E1[5]*E2[4])/3 + (4*e_1_2B[7]*e_1_4B[5]*e_1_4B[5]*E1[4]*E2[7])/3 + (4*e_1_2B[8]*e_1_4B[8]*e_1_4B[8]*E1[5]*E2[7])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[3]*E1[3]*E2[1])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[4]*E1[3]*E2[4])/3 + (4*e_1_2B[4]*e_1_4B[3]*e_1_4B[4]*E1[4]*E2[1])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[6]*E1[3]*E2[1])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[3]*E1[3]*E2[1])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[5]*E1[3]*E2[7])/3 + (4*e_1_2B[4]*e_1_4B[3]*e_1_4B[7]*E1[5]*E2[1])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[7]*E1[3]*E2[4])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[6]*E1[4]*E2[1])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[4]*E1[3]*E2[4])/3 + (4*e_1_2B[7]*e_1_4B[3]*e_1_4B[5]*E1[4]*E2[1])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[6]*E1[3]*E2[1])/3 + (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[5]*E1[4]*E2[7])/3 + (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[7]*E1[5]*E2[4])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[8]*E1[3]*E2[7])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[7]*E1[4]*E2[4])/3 + (4*e_1_2B[5]*e_1_4B[6]*e_1_4B[7]*E1[5]*E2[1])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[5]*E1[3]*E2[7])/3 + (4*e_1_2B[7]*e_1_4B[3]*e_1_4B[8]*E1[5]*E2[1])/3 + (4*e_1_2B[7]*e_1_4B[4]*e_1_4B[5]*E1[4]*E2[4])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[7]*E1[3]*E2[4])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[6]*E1[4]*E2[1])/3 + (4*e_1_2B[4]*e_1_4B[5]*e_1_4B[7]*E1[5]*E2[7])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[8]*E1[4]*E2[7])/3 + (4*e_1_2B[7]*e_1_4B[4]*e_1_4B[8]*E1[5]*E2[4])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[8]*E1[3]*E2[7])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[7]*E1[4]*E2[4])/3 + (4*e_1_2B[8]*e_1_4B[6]*e_1_4B[8]*E1[5]*E2[1])/3 + (4*e_1_2B[5]*e_1_4B[7]*e_1_4B[8]*E1[5]*E2[7])/3 + (4*e_1_2B[7]*e_1_4B[5]*e_1_4B[8]*E1[5]*E2[7])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[8]*E1[4]*E2[7])/3 + (4*e_1_2B[8]*e_1_4B[7]*e_1_4B[8]*E1[5]*E2[4])/3;
                p22[1] = 0.0;
                p22[2] = 0.0;
                p22[3] = 0.0;
                p22[4] = (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[3]*E1[4]*E2[1])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[1]*E1[3]*E2[4])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[6]*E1[5]*E2[1])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[2]*E1[3]*E2[7])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[3]*E1[3]*E2[1])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[1]*E1[3]*E2[1])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[4]*E1[3]*E2[4])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[6]*E1[3]*E2[1])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[4]*E1[4]*E2[1])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[2]*E1[3]*E2[1])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[5]*E1[3]*E2[7])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[4]*E1[4]*E2[4])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[6]*E1[5]*E2[1])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[7]*E1[3]*E2[4])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[6]*E1[4]*E2[1])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[7]*E1[5]*E2[1])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[2]*E1[3]*E2[7])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[4]*E1[4]*E2[4])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[5]*E1[4]*E2[1])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[2]*E1[3]*E2[4])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[5]*E1[4]*E2[7])/3 + (4*e_1_2B[1]*e_1_4B[4]*e_1_4B[6]*E1[5]*E2[4])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[8]*E1[3]*E2[7])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[7]*E1[4]*E2[4])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[7]*E1[5]*E2[4])/3 + (4*e_1_2B[3]*e_1_4B[2]*e_1_4B[4]*E1[4]*E2[7])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[8]*E1[5]*E2[1])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[5]*E1[4]*E2[4])/3 + (4*e_1_2B[1]*e_1_4B[5]*e_1_4B[6]*E1[5]*E2[7])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[8]*E1[4]*E2[7])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[7]*E1[5]*E2[4])/3 + (4*e_1_2B[3]*e_1_4B[2]*e_1_4B[7]*E1[5]*E2[7])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[8]*E1[5]*E2[4])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[5]*E1[4]*E2[7])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[8]*E1[5]*E2[7])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[8]*E1[5]*E2[7])/3;
                p22[5] = 0.0;
                p22[6] = 0.0;
                p22[7] = 0.0;
                p22[8] = (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[0]*E1[3]*E2[1])/3 - (e_1_2B[0]*e_1_2B[1]*E1[3]*E2[4])/3 - (e_1_2B[0]*e_1_2B[3]*E1[4]*E2[1])/3 - (e_1_2B[0]*e_1_2B[2]*E1[3]*E2[7])/3 - (e_1_2B[0]*e_1_2B[6]*E1[5]*E2[1])/3 - (e_1_2B[1]*e_1_2B[3]*E1[4]*E2[4])/3 - (e_1_2B[1]*e_1_2B[6]*E1[5]*E2[4])/3 - (e_1_2B[2]*e_1_2B[3]*E1[4]*E2[7])/3 - (e_1_2B[2]*e_1_2B[6]*E1[5]*E2[7])/3 - (e_1_2B[0]*e_1_2B[0]*E1[3]*E2[1])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[1]*E1[3]*E2[4])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[3]*E1[4]*E2[1])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[2]*E1[3]*E2[7])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[6]*E1[5]*E2[1])/3 + (4*e_1_2B[0]*e_1_4B[1]*e_1_4B[3]*E1[4]*E2[4])/3 + (4*e_1_2B[0]*e_1_4B[1]*e_1_4B[6]*E1[5]*E2[4])/3 + (4*e_1_2B[0]*e_1_4B[2]*e_1_4B[3]*E1[4]*E2[7])/3 + (4*e_1_2B[0]*e_1_4B[2]*e_1_4B[6]*E1[5]*E2[7])/3;

                p23[0] = (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[4]*E1[4]*E2[5])/3 - (e_1_2B[8]*e_1_2B[8]*E1[5]*E2[8])/3 - (e_1_2B[1]*e_1_2B[3]*E1[3]*E2[2])/3 - (e_1_2B[1]*e_1_2B[4]*E1[3]*E2[5])/3 - (e_1_2B[2]*e_1_2B[6]*E1[3]*E2[2])/3 - (e_1_2B[3]*e_1_2B[4]*E1[4]*E2[2])/3 - (e_1_2B[1]*e_1_2B[5]*E1[3]*E2[8])/3 - (e_1_2B[2]*e_1_2B[7]*E1[3]*E2[5])/3 - (e_1_2B[3]*e_1_2B[7]*E1[5]*E2[2])/3 - (e_1_2B[5]*e_1_2B[6]*E1[4]*E2[2])/3 - (e_1_2B[2]*e_1_2B[8]*E1[3]*E2[8])/3 - (e_1_2B[4]*e_1_2B[5]*E1[4]*E2[8])/3 - (e_1_2B[4]*e_1_2B[7]*E1[5]*E2[5])/3 - (e_1_2B[5]*e_1_2B[7]*E1[4]*E2[5])/3 - (e_1_2B[6]*e_1_2B[8]*E1[5]*E2[2])/3 - (e_1_2B[5]*e_1_2B[7]*E1[5]*E2[8])/3 - (e_1_2B[5]*e_1_2B[8]*E1[4]*E2[8])/3 - (e_1_2B[7]*e_1_2B[8]*E1[5]*E2[5])/3 - (e_1_2B[4]*e_1_2B[4]*E1[4]*E2[5])/3 + (4*e_1_2B[5]*e_1_4B[7]*e_1_4B[7]*E1[5]*E2[5])/3 + (4*e_1_2B[7]*e_1_4B[5]*e_1_4B[5]*E1[4]*E2[8])/3 + (4*e_1_2B[8]*e_1_4B[8]*e_1_4B[8]*E1[5]*E2[8])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[3]*E1[3]*E2[2])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[4]*E1[3]*E2[5])/3 + (4*e_1_2B[4]*e_1_4B[3]*e_1_4B[4]*E1[4]*E2[2])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[6]*E1[3]*E2[2])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[3]*E1[3]*E2[2])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[5]*E1[3]*E2[8])/3 + (4*e_1_2B[4]*e_1_4B[3]*e_1_4B[7]*E1[5]*E2[2])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[7]*E1[3]*E2[5])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[6]*E1[4]*E2[2])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[4]*E1[3]*E2[5])/3 + (4*e_1_2B[7]*e_1_4B[3]*e_1_4B[5]*E1[4]*E2[2])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[6]*E1[3]*E2[2])/3 + (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[5]*E1[4]*E2[8])/3 + (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[7]*E1[5]*E2[5])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[8]*E1[3]*E2[8])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[7]*E1[4]*E2[5])/3 + (4*e_1_2B[5]*e_1_4B[6]*e_1_4B[7]*E1[5]*E2[2])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[5]*E1[3]*E2[8])/3 + (4*e_1_2B[7]*e_1_4B[3]*e_1_4B[8]*E1[5]*E2[2])/3 + (4*e_1_2B[7]*e_1_4B[4]*e_1_4B[5]*E1[4]*E2[5])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[7]*E1[3]*E2[5])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[6]*E1[4]*E2[2])/3 + (4*e_1_2B[4]*e_1_4B[5]*e_1_4B[7]*E1[5]*E2[8])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[8]*E1[4]*E2[8])/3 + (4*e_1_2B[7]*e_1_4B[4]*e_1_4B[8]*E1[5]*E2[5])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[8]*E1[3]*E2[8])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[7]*E1[4]*E2[5])/3 + (4*e_1_2B[8]*e_1_4B[6]*e_1_4B[8]*E1[5]*E2[2])/3 + (4*e_1_2B[5]*e_1_4B[7]*e_1_4B[8]*E1[5]*E2[8])/3 + (4*e_1_2B[7]*e_1_4B[5]*e_1_4B[8]*E1[5]*E2[8])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[8]*E1[4]*E2[8])/3 + (4*e_1_2B[8]*e_1_4B[7]*e_1_4B[8]*E1[5]*E2[5])/3;
                p23[1] = 0.0;
                p23[2] = 0.0;
                p23[3] = 0.0;
                p23[4] = (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[3]*E1[4]*E2[2])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[1]*E1[3]*E2[5])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[6]*E1[5]*E2[2])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[2]*E1[3]*E2[8])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[3]*E1[3]*E2[2])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[1]*E1[3]*E2[2])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[4]*E1[3]*E2[5])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[6]*E1[3]*E2[2])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[4]*E1[4]*E2[2])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[2]*E1[3]*E2[2])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[5]*E1[3]*E2[8])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[4]*E1[4]*E2[5])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[6]*E1[5]*E2[2])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[7]*E1[3]*E2[5])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[6]*E1[4]*E2[2])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[7]*E1[5]*E2[2])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[2]*E1[3]*E2[8])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[4]*E1[4]*E2[5])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[5]*E1[4]*E2[2])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[2]*E1[3]*E2[5])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[5]*E1[4]*E2[8])/3 + (4*e_1_2B[1]*e_1_4B[4]*e_1_4B[6]*E1[5]*E2[5])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[8]*E1[3]*E2[8])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[7]*E1[4]*E2[5])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[7]*E1[5]*E2[5])/3 + (4*e_1_2B[3]*e_1_4B[2]*e_1_4B[4]*E1[4]*E2[8])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[8]*E1[5]*E2[2])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[5]*E1[4]*E2[5])/3 + (4*e_1_2B[1]*e_1_4B[5]*e_1_4B[6]*E1[5]*E2[8])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[8]*E1[4]*E2[8])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[7]*E1[5]*E2[5])/3 + (4*e_1_2B[3]*e_1_4B[2]*e_1_4B[7]*E1[5]*E2[8])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[8]*E1[5]*E2[5])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[5]*E1[4]*E2[8])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[8]*E1[5]*E2[8])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[8]*E1[5]*E2[8])/3;
                p23[5] = 0.0;
                p23[6] = 0.0;
                p23[7] = 0.0;
                p23[8] = (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[0]*E1[3]*E2[2])/3 - (e_1_2B[0]*e_1_2B[1]*E1[3]*E2[5])/3 - (e_1_2B[0]*e_1_2B[3]*E1[4]*E2[2])/3 - (e_1_2B[0]*e_1_2B[2]*E1[3]*E2[8])/3 - (e_1_2B[0]*e_1_2B[6]*E1[5]*E2[2])/3 - (e_1_2B[1]*e_1_2B[3]*E1[4]*E2[5])/3 - (e_1_2B[1]*e_1_2B[6]*E1[5]*E2[5])/3 - (e_1_2B[2]*e_1_2B[3]*E1[4]*E2[8])/3 - (e_1_2B[2]*e_1_2B[6]*E1[5]*E2[8])/3 - (e_1_2B[0]*e_1_2B[0]*E1[3]*E2[2])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[1]*E1[3]*E2[5])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[3]*E1[4]*E2[2])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[2]*E1[3]*E2[8])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[6]*E1[5]*E2[2])/3 + (4*e_1_2B[0]*e_1_4B[1]*e_1_4B[3]*E1[4]*E2[5])/3 + (4*e_1_2B[0]*e_1_4B[1]*e_1_4B[6]*E1[5]*E2[5])/3 + (4*e_1_2B[0]*e_1_4B[2]*e_1_4B[3]*E1[4]*E2[8])/3 + (4*e_1_2B[0]*e_1_4B[2]*e_1_4B[6]*E1[5]*E2[8])/3;

                p31[0] = (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[4]*E1[7]*E2[3])/3 - (e_1_2B[8]*e_1_2B[8]*E1[8]*E2[6])/3 - (e_1_2B[1]*e_1_2B[3]*E1[6]*E2[0])/3 - (e_1_2B[1]*e_1_2B[4]*E1[6]*E2[3])/3 - (e_1_2B[2]*e_1_2B[6]*E1[6]*E2[0])/3 - (e_1_2B[3]*e_1_2B[4]*E1[7]*E2[0])/3 - (e_1_2B[1]*e_1_2B[5]*E1[6]*E2[6])/3 - (e_1_2B[2]*e_1_2B[7]*E1[6]*E2[3])/3 - (e_1_2B[3]*e_1_2B[7]*E1[8]*E2[0])/3 - (e_1_2B[5]*e_1_2B[6]*E1[7]*E2[0])/3 - (e_1_2B[2]*e_1_2B[8]*E1[6]*E2[6])/3 - (e_1_2B[4]*e_1_2B[5]*E1[7]*E2[6])/3 - (e_1_2B[4]*e_1_2B[7]*E1[8]*E2[3])/3 - (e_1_2B[5]*e_1_2B[7]*E1[7]*E2[3])/3 - (e_1_2B[6]*e_1_2B[8]*E1[8]*E2[0])/3 - (e_1_2B[5]*e_1_2B[7]*E1[8]*E2[6])/3 - (e_1_2B[5]*e_1_2B[8]*E1[7]*E2[6])/3 - (e_1_2B[7]*e_1_2B[8]*E1[8]*E2[3])/3 - (e_1_2B[4]*e_1_2B[4]*E1[7]*E2[3])/3 + (4*e_1_2B[5]*e_1_4B[7]*e_1_4B[7]*E1[8]*E2[3])/3 + (4*e_1_2B[7]*e_1_4B[5]*e_1_4B[5]*E1[7]*E2[6])/3 + (4*e_1_2B[8]*e_1_4B[8]*e_1_4B[8]*E1[8]*E2[6])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[3]*E1[6]*E2[0])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[4]*E1[6]*E2[3])/3 + (4*e_1_2B[4]*e_1_4B[3]*e_1_4B[4]*E1[7]*E2[0])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[6]*E1[6]*E2[0])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[3]*E1[6]*E2[0])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[5]*E1[6]*E2[6])/3 + (4*e_1_2B[4]*e_1_4B[3]*e_1_4B[7]*E1[8]*E2[0])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[7]*E1[6]*E2[3])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[6]*E1[7]*E2[0])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[4]*E1[6]*E2[3])/3 + (4*e_1_2B[7]*e_1_4B[3]*e_1_4B[5]*E1[7]*E2[0])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[6]*E1[6]*E2[0])/3 + (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[5]*E1[7]*E2[6])/3 + (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[7]*E1[8]*E2[3])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[8]*E1[6]*E2[6])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[7]*E1[7]*E2[3])/3 + (4*e_1_2B[5]*e_1_4B[6]*e_1_4B[7]*E1[8]*E2[0])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[5]*E1[6]*E2[6])/3 + (4*e_1_2B[7]*e_1_4B[3]*e_1_4B[8]*E1[8]*E2[0])/3 + (4*e_1_2B[7]*e_1_4B[4]*e_1_4B[5]*E1[7]*E2[3])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[7]*E1[6]*E2[3])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[6]*E1[7]*E2[0])/3 + (4*e_1_2B[4]*e_1_4B[5]*e_1_4B[7]*E1[8]*E2[6])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[8]*E1[7]*E2[6])/3 + (4*e_1_2B[7]*e_1_4B[4]*e_1_4B[8]*E1[8]*E2[3])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[8]*E1[6]*E2[6])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[7]*E1[7]*E2[3])/3 + (4*e_1_2B[8]*e_1_4B[6]*e_1_4B[8]*E1[8]*E2[0])/3 + (4*e_1_2B[5]*e_1_4B[7]*e_1_4B[8]*E1[8]*E2[6])/3 + (4*e_1_2B[7]*e_1_4B[5]*e_1_4B[8]*E1[8]*E2[6])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[8]*E1[7]*E2[6])/3 + (4*e_1_2B[8]*e_1_4B[7]*e_1_4B[8]*E1[8]*E2[3])/3;
                p31[1] = 0.0;
                p31[2] = 0.0;
                p31[3] = 0.0;
                p31[4] = (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[3]*E1[7]*E2[0])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[1]*E1[6]*E2[3])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[6]*E1[8]*E2[0])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[2]*E1[6]*E2[6])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[3]*E1[6]*E2[0])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[1]*E1[6]*E2[0])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[4]*E1[6]*E2[3])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[6]*E1[6]*E2[0])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[4]*E1[7]*E2[0])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[2]*E1[6]*E2[0])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[5]*E1[6]*E2[6])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[4]*E1[7]*E2[3])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[6]*E1[8]*E2[0])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[7]*E1[6]*E2[3])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[6]*E1[7]*E2[0])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[7]*E1[8]*E2[0])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[2]*E1[6]*E2[6])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[4]*E1[7]*E2[3])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[5]*E1[7]*E2[0])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[2]*E1[6]*E2[3])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[5]*E1[7]*E2[6])/3 + (4*e_1_2B[1]*e_1_4B[4]*e_1_4B[6]*E1[8]*E2[3])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[8]*E1[6]*E2[6])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[7]*E1[7]*E2[3])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[7]*E1[8]*E2[3])/3 + (4*e_1_2B[3]*e_1_4B[2]*e_1_4B[4]*E1[7]*E2[6])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[8]*E1[8]*E2[0])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[5]*E1[7]*E2[3])/3 + (4*e_1_2B[1]*e_1_4B[5]*e_1_4B[6]*E1[8]*E2[6])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[8]*E1[7]*E2[6])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[7]*E1[8]*E2[3])/3 + (4*e_1_2B[3]*e_1_4B[2]*e_1_4B[7]*E1[8]*E2[6])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[8]*E1[8]*E2[3])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[5]*E1[7]*E2[6])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[8]*E1[8]*E2[6])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[8]*E1[8]*E2[6])/3;
                p31[5] = 0.0;
                p31[6] = 0.0;
                p31[7] = 0.0;
                p31[8] = (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[0]*E1[6]*E2[0])/3 - (e_1_2B[0]*e_1_2B[1]*E1[6]*E2[3])/3 - (e_1_2B[0]*e_1_2B[3]*E1[7]*E2[0])/3 - (e_1_2B[0]*e_1_2B[2]*E1[6]*E2[6])/3 - (e_1_2B[0]*e_1_2B[6]*E1[8]*E2[0])/3 - (e_1_2B[1]*e_1_2B[3]*E1[7]*E2[3])/3 - (e_1_2B[1]*e_1_2B[6]*E1[8]*E2[3])/3 - (e_1_2B[2]*e_1_2B[3]*E1[7]*E2[6])/3 - (e_1_2B[2]*e_1_2B[6]*E1[8]*E2[6])/3 - (e_1_2B[0]*e_1_2B[0]*E1[6]*E2[0])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[1]*E1[6]*E2[3])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[3]*E1[7]*E2[0])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[2]*E1[6]*E2[6])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[6]*E1[8]*E2[0])/3 + (4*e_1_2B[0]*e_1_4B[1]*e_1_4B[3]*E1[7]*E2[3])/3 + (4*e_1_2B[0]*e_1_4B[1]*e_1_4B[6]*E1[8]*E2[3])/3 + (4*e_1_2B[0]*e_1_4B[2]*e_1_4B[3]*E1[7]*E2[6])/3 + (4*e_1_2B[0]*e_1_4B[2]*e_1_4B[6]*E1[8]*E2[6])/3;

                p32[0] = (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[4]*E1[7]*E2[4])/3 - (e_1_2B[8]*e_1_2B[8]*E1[8]*E2[7])/3 - (e_1_2B[1]*e_1_2B[3]*E1[6]*E2[1])/3 - (e_1_2B[1]*e_1_2B[4]*E1[6]*E2[4])/3 - (e_1_2B[2]*e_1_2B[6]*E1[6]*E2[1])/3 - (e_1_2B[3]*e_1_2B[4]*E1[7]*E2[1])/3 - (e_1_2B[1]*e_1_2B[5]*E1[6]*E2[7])/3 - (e_1_2B[2]*e_1_2B[7]*E1[6]*E2[4])/3 - (e_1_2B[3]*e_1_2B[7]*E1[8]*E2[1])/3 - (e_1_2B[5]*e_1_2B[6]*E1[7]*E2[1])/3 - (e_1_2B[2]*e_1_2B[8]*E1[6]*E2[7])/3 - (e_1_2B[4]*e_1_2B[5]*E1[7]*E2[7])/3 - (e_1_2B[4]*e_1_2B[7]*E1[8]*E2[4])/3 - (e_1_2B[5]*e_1_2B[7]*E1[7]*E2[4])/3 - (e_1_2B[6]*e_1_2B[8]*E1[8]*E2[1])/3 - (e_1_2B[5]*e_1_2B[7]*E1[8]*E2[7])/3 - (e_1_2B[5]*e_1_2B[8]*E1[7]*E2[7])/3 - (e_1_2B[7]*e_1_2B[8]*E1[8]*E2[4])/3 - (e_1_2B[4]*e_1_2B[4]*E1[7]*E2[4])/3 + (4*e_1_2B[5]*e_1_4B[7]*e_1_4B[7]*E1[8]*E2[4])/3 + (4*e_1_2B[7]*e_1_4B[5]*e_1_4B[5]*E1[7]*E2[7])/3 + (4*e_1_2B[8]*e_1_4B[8]*e_1_4B[8]*E1[8]*E2[7])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[3]*E1[6]*E2[1])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[4]*E1[6]*E2[4])/3 + (4*e_1_2B[4]*e_1_4B[3]*e_1_4B[4]*E1[7]*E2[1])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[6]*E1[6]*E2[1])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[3]*E1[6]*E2[1])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[5]*E1[6]*E2[7])/3 + (4*e_1_2B[4]*e_1_4B[3]*e_1_4B[7]*E1[8]*E2[1])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[7]*E1[6]*E2[4])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[6]*E1[7]*E2[1])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[4]*E1[6]*E2[4])/3 + (4*e_1_2B[7]*e_1_4B[3]*e_1_4B[5]*E1[7]*E2[1])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[6]*E1[6]*E2[1])/3 + (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[5]*E1[7]*E2[7])/3 + (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[7]*E1[8]*E2[4])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[8]*E1[6]*E2[7])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[7]*E1[7]*E2[4])/3 + (4*e_1_2B[5]*e_1_4B[6]*e_1_4B[7]*E1[8]*E2[1])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[5]*E1[6]*E2[7])/3 + (4*e_1_2B[7]*e_1_4B[3]*e_1_4B[8]*E1[8]*E2[1])/3 + (4*e_1_2B[7]*e_1_4B[4]*e_1_4B[5]*E1[7]*E2[4])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[7]*E1[6]*E2[4])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[6]*E1[7]*E2[1])/3 + (4*e_1_2B[4]*e_1_4B[5]*e_1_4B[7]*E1[8]*E2[7])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[8]*E1[7]*E2[7])/3 + (4*e_1_2B[7]*e_1_4B[4]*e_1_4B[8]*E1[8]*E2[4])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[8]*E1[6]*E2[7])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[7]*E1[7]*E2[4])/3 + (4*e_1_2B[8]*e_1_4B[6]*e_1_4B[8]*E1[8]*E2[1])/3 + (4*e_1_2B[5]*e_1_4B[7]*e_1_4B[8]*E1[8]*E2[7])/3 + (4*e_1_2B[7]*e_1_4B[5]*e_1_4B[8]*E1[8]*E2[7])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[8]*E1[7]*E2[7])/3 + (4*e_1_2B[8]*e_1_4B[7]*e_1_4B[8]*E1[8]*E2[4])/3;
                p32[1] = 0.0;
                p32[2] = 0.0;
                p32[3] = 0.0;
                p32[4] = (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[3]*E1[7]*E2[1])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[1]*E1[6]*E2[4])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[6]*E1[8]*E2[1])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[2]*E1[6]*E2[7])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[3]*E1[6]*E2[1])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[1]*E1[6]*E2[1])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[4]*E1[6]*E2[4])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[6]*E1[6]*E2[1])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[4]*E1[7]*E2[1])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[2]*E1[6]*E2[1])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[5]*E1[6]*E2[7])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[4]*E1[7]*E2[4])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[6]*E1[8]*E2[1])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[7]*E1[6]*E2[4])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[6]*E1[7]*E2[1])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[7]*E1[8]*E2[1])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[2]*E1[6]*E2[7])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[4]*E1[7]*E2[4])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[5]*E1[7]*E2[1])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[2]*E1[6]*E2[4])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[5]*E1[7]*E2[7])/3 + (4*e_1_2B[1]*e_1_4B[4]*e_1_4B[6]*E1[8]*E2[4])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[8]*E1[6]*E2[7])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[7]*E1[7]*E2[4])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[7]*E1[8]*E2[4])/3 + (4*e_1_2B[3]*e_1_4B[2]*e_1_4B[4]*E1[7]*E2[7])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[8]*E1[8]*E2[1])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[5]*E1[7]*E2[4])/3 + (4*e_1_2B[1]*e_1_4B[5]*e_1_4B[6]*E1[8]*E2[7])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[8]*E1[7]*E2[7])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[7]*E1[8]*E2[4])/3 + (4*e_1_2B[3]*e_1_4B[2]*e_1_4B[7]*E1[8]*E2[7])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[8]*E1[8]*E2[4])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[5]*E1[7]*E2[7])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[8]*E1[8]*E2[7])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[8]*E1[8]*E2[7])/3;
                p32[5] = 0.0;
                p32[6] = 0.0;
                p32[7] = 0.0;
                p32[8] = (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[0]*E1[6]*E2[1])/3 - (e_1_2B[0]*e_1_2B[1]*E1[6]*E2[4])/3 - (e_1_2B[0]*e_1_2B[3]*E1[7]*E2[1])/3 - (e_1_2B[0]*e_1_2B[2]*E1[6]*E2[7])/3 - (e_1_2B[0]*e_1_2B[6]*E1[8]*E2[1])/3 - (e_1_2B[1]*e_1_2B[3]*E1[7]*E2[4])/3 - (e_1_2B[1]*e_1_2B[6]*E1[8]*E2[4])/3 - (e_1_2B[2]*e_1_2B[3]*E1[7]*E2[7])/3 - (e_1_2B[2]*e_1_2B[6]*E1[8]*E2[7])/3 - (e_1_2B[0]*e_1_2B[0]*E1[6]*E2[1])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[1]*E1[6]*E2[4])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[3]*E1[7]*E2[1])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[2]*E1[6]*E2[7])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[6]*E1[8]*E2[1])/3 + (4*e_1_2B[0]*e_1_4B[1]*e_1_4B[3]*E1[7]*E2[4])/3 + (4*e_1_2B[0]*e_1_4B[1]*e_1_4B[6]*E1[8]*E2[4])/3 + (4*e_1_2B[0]*e_1_4B[2]*e_1_4B[3]*E1[7]*E2[7])/3 + (4*e_1_2B[0]*e_1_4B[2]*e_1_4B[6]*E1[8]*E2[7])/3;

                p33[0] = (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[4]*E1[7]*E2[5])/3 - (e_1_2B[8]*e_1_2B[8]*E1[8]*E2[8])/3 - (e_1_2B[1]*e_1_2B[3]*E1[6]*E2[2])/3 - (e_1_2B[1]*e_1_2B[4]*E1[6]*E2[5])/3 - (e_1_2B[2]*e_1_2B[6]*E1[6]*E2[2])/3 - (e_1_2B[3]*e_1_2B[4]*E1[7]*E2[2])/3 - (e_1_2B[1]*e_1_2B[5]*E1[6]*E2[8])/3 - (e_1_2B[2]*e_1_2B[7]*E1[6]*E2[5])/3 - (e_1_2B[3]*e_1_2B[7]*E1[8]*E2[2])/3 - (e_1_2B[5]*e_1_2B[6]*E1[7]*E2[2])/3 - (e_1_2B[2]*e_1_2B[8]*E1[6]*E2[8])/3 - (e_1_2B[4]*e_1_2B[5]*E1[7]*E2[8])/3 - (e_1_2B[4]*e_1_2B[7]*E1[8]*E2[5])/3 - (e_1_2B[5]*e_1_2B[7]*E1[7]*E2[5])/3 - (e_1_2B[6]*e_1_2B[8]*E1[8]*E2[2])/3 - (e_1_2B[5]*e_1_2B[7]*E1[8]*E2[8])/3 - (e_1_2B[5]*e_1_2B[8]*E1[7]*E2[8])/3 - (e_1_2B[7]*e_1_2B[8]*E1[8]*E2[5])/3 - (e_1_2B[4]*e_1_2B[4]*E1[7]*E2[5])/3 + (4*e_1_2B[5]*e_1_4B[7]*e_1_4B[7]*E1[8]*E2[5])/3 + (4*e_1_2B[7]*e_1_4B[5]*e_1_4B[5]*E1[7]*E2[8])/3 + (4*e_1_2B[8]*e_1_4B[8]*e_1_4B[8]*E1[8]*E2[8])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[3]*E1[6]*E2[2])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[4]*E1[6]*E2[5])/3 + (4*e_1_2B[4]*e_1_4B[3]*e_1_4B[4]*E1[7]*E2[2])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[6]*E1[6]*E2[2])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[3]*E1[6]*E2[2])/3 + (4*e_1_2B[4]*e_1_4B[1]*e_1_4B[5]*E1[6]*E2[8])/3 + (4*e_1_2B[4]*e_1_4B[3]*e_1_4B[7]*E1[8]*E2[2])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[7]*E1[6]*E2[5])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[6]*E1[7]*E2[2])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[4]*E1[6]*E2[5])/3 + (4*e_1_2B[7]*e_1_4B[3]*e_1_4B[5]*E1[7]*E2[2])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[6]*E1[6]*E2[2])/3 + (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[5]*E1[7]*E2[8])/3 + (4*e_1_2B[4]*e_1_4B[4]*e_1_4B[7]*E1[8]*E2[5])/3 + (4*e_1_2B[5]*e_1_4B[1]*e_1_4B[8]*E1[6]*E2[8])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[7]*E1[7]*E2[5])/3 + (4*e_1_2B[5]*e_1_4B[6]*e_1_4B[7]*E1[8]*E2[2])/3 + (4*e_1_2B[7]*e_1_4B[2]*e_1_4B[5]*E1[6]*E2[8])/3 + (4*e_1_2B[7]*e_1_4B[3]*e_1_4B[8]*E1[8]*E2[2])/3 + (4*e_1_2B[7]*e_1_4B[4]*e_1_4B[5]*E1[7]*E2[5])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[7]*E1[6]*E2[5])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[6]*E1[7]*E2[2])/3 + (4*e_1_2B[4]*e_1_4B[5]*e_1_4B[7]*E1[8]*E2[8])/3 + (4*e_1_2B[5]*e_1_4B[4]*e_1_4B[8]*E1[7]*E2[8])/3 + (4*e_1_2B[7]*e_1_4B[4]*e_1_4B[8]*E1[8]*E2[5])/3 + (4*e_1_2B[8]*e_1_4B[2]*e_1_4B[8]*E1[6]*E2[8])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[7]*E1[7]*E2[5])/3 + (4*e_1_2B[8]*e_1_4B[6]*e_1_4B[8]*E1[8]*E2[2])/3 + (4*e_1_2B[5]*e_1_4B[7]*e_1_4B[8]*E1[8]*E2[8])/3 + (4*e_1_2B[7]*e_1_4B[5]*e_1_4B[8]*E1[8]*E2[8])/3 + (4*e_1_2B[8]*e_1_4B[5]*e_1_4B[8]*E1[7]*E2[8])/3 + (4*e_1_2B[8]*e_1_4B[7]*e_1_4B[8]*E1[8]*E2[5])/3;
                p33[1] = 0.0;
                p33[2] = 0.0;
                p33[3] = 0.0;
                p33[4] = (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[3]*E1[7]*E2[2])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[1]*E1[6]*E2[5])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[6]*E1[8]*E2[2])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[2]*E1[6]*E2[8])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[3]*E1[6]*E2[2])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[1]*E1[6]*E2[2])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[4]*E1[6]*E2[5])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[6]*E1[6]*E2[2])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[4]*E1[7]*E2[2])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[2]*E1[6]*E2[2])/3 + (4*e_1_2B[1]*e_1_4B[0]*e_1_4B[5]*E1[6]*E2[8])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[4]*E1[7]*E2[5])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[6]*E1[8]*E2[2])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[7]*E1[6]*E2[5])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[6]*E1[7]*E2[2])/3 + (4*e_1_2B[3]*e_1_4B[0]*e_1_4B[7]*E1[8]*E2[2])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[2]*E1[6]*E2[8])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[4]*E1[7]*E2[5])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[5]*E1[7]*E2[2])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[2]*E1[6]*E2[5])/3 + (4*e_1_2B[1]*e_1_4B[3]*e_1_4B[5]*E1[7]*E2[8])/3 + (4*e_1_2B[1]*e_1_4B[4]*e_1_4B[6]*E1[8]*E2[5])/3 + (4*e_1_2B[2]*e_1_4B[0]*e_1_4B[8]*E1[6]*E2[8])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[7]*E1[7]*E2[5])/3 + (4*e_1_2B[3]*e_1_4B[1]*e_1_4B[7]*E1[8]*E2[5])/3 + (4*e_1_2B[3]*e_1_4B[2]*e_1_4B[4]*E1[7]*E2[8])/3 + (4*e_1_2B[6]*e_1_4B[0]*e_1_4B[8]*E1[8]*E2[2])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[5]*E1[7]*E2[5])/3 + (4*e_1_2B[1]*e_1_4B[5]*e_1_4B[6]*E1[8]*E2[8])/3 + (4*e_1_2B[2]*e_1_4B[3]*e_1_4B[8]*E1[7]*E2[8])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[7]*E1[8]*E2[5])/3 + (4*e_1_2B[3]*e_1_4B[2]*e_1_4B[7]*E1[8]*E2[8])/3 + (4*e_1_2B[6]*e_1_4B[1]*e_1_4B[8]*E1[8]*E2[5])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[5]*E1[7]*E2[8])/3 + (4*e_1_2B[2]*e_1_4B[6]*e_1_4B[8]*E1[8]*E2[8])/3 + (4*e_1_2B[6]*e_1_4B[2]*e_1_4B[8]*E1[8]*E2[8])/3;
                p33[5] = 0.0;
                p33[6] = 0.0;
                p33[7] = 0.0;
                p33[8] = (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[0]*E1[6]*E2[2])/3 - (e_1_2B[0]*e_1_2B[1]*E1[6]*E2[5])/3 - (e_1_2B[0]*e_1_2B[3]*E1[7]*E2[2])/3 - (e_1_2B[0]*e_1_2B[2]*E1[6]*E2[8])/3 - (e_1_2B[0]*e_1_2B[6]*E1[8]*E2[2])/3 - (e_1_2B[1]*e_1_2B[3]*E1[7]*E2[5])/3 - (e_1_2B[1]*e_1_2B[6]*E1[8]*E2[5])/3 - (e_1_2B[2]*e_1_2B[3]*E1[7]*E2[8])/3 - (e_1_2B[2]*e_1_2B[6]*E1[8]*E2[8])/3 - (e_1_2B[0]*e_1_2B[0]*E1[6]*E2[2])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[1]*E1[6]*E2[5])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[3]*E1[7]*E2[2])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[2]*E1[6]*E2[8])/3 + (4*e_1_2B[0]*e_1_4B[0]*e_1_4B[6]*E1[8]*E2[2])/3 + (4*e_1_2B[0]*e_1_4B[1]*e_1_4B[3]*E1[7]*E2[5])/3 + (4*e_1_2B[0]*e_1_4B[1]*e_1_4B[6]*E1[8]*E2[5])/3 + (4*e_1_2B[0]*e_1_4B[2]*e_1_4B[3]*E1[7]*E2[8])/3 + (4*e_1_2B[0]*e_1_4B[2]*e_1_4B[6]*E1[8]*E2[8])/3;

                p11 += 9;
                p12 += 9;
                p13 += 9;
                p21 += 9;
                p22 += 9;
                p23 += 9;
                p31 += 9;
                p32 += 9;
                p33 += 9;
            }
            break;

            case manakov_discretization_FTES4_suzuki:
            
            e_7_48B = &e_Bstorage[0];   // exp(Bh*7/48)
            e_3_8B = &e_Bstorage[9];    // exp(BH*3/8)
            e_m1_48B = &e_Bstorage[18]; // exp(BH*-1/48)
            E1 = &E_storage[0];
            E2 = &E_storage[9];

            for (i=D_eff; i-->0;){
                manakov_fscatter_zero_freq_scatter_matrix(e_7_48B, 7*eps_t/48, q1[i], q2[i], kappa);
                manakov_fscatter_zero_freq_scatter_matrix(e_3_8B, 3*eps_t/8, q1[i], q2[i], kappa);
                manakov_fscatter_zero_freq_scatter_matrix(e_m1_48B, -1*eps_t/48, q1[i], q2[i], kappa);

                // Setting which elements of q we use
                q1n[1] = q1[i];
                q2n[1] = q2[i];
                
                if (i==0){  //left bnd condition
                q1n[0] = q1[i];      // set q1[n-1] to q[n] on boundary
                q2n[0] = q2[i];
                }
                else {
                q1n[0] = q1[i-1];
                q2n[0] = q2[i-1];
                }

                if (i==D_eff-1){  //right bnd condition
                q1n[2] = q1[i];      // set q1[n+1] to q[n] on boundary
                q2n[2] = q2[i];
                }
                else{
                q1n[2] = q1[i+1];
                q2n[2] = q2[i+1];
                }

                manakov_fscatter_get_E(E1, eps_t, (q1n[2]-q1n[0])*eps_t/24+(q1n[2]-2*q1n[1]+q1n[0])*eps_t/48,
                                (q2n[2]-q2n[0])*eps_t/24+(q2n[2]-2*q2n[1]+q2n[0])*eps_t/48,
                                kappa*CONJ(q1n[0]-q1n[2])*eps_t/24-kappa*CONJ(q1n[2]-2*q1n[1]+q1n[0])*eps_t/48,
                                kappa*CONJ(q2n[0]-q2n[2])*eps_t/24-kappa*CONJ(q2n[2]-2*q2n[1]+q2n[0])*eps_t/48);

                manakov_fscatter_get_E(E2, eps_t, -(q1n[2]-q1n[0])*eps_t/24+(q1n[2]-2*q1n[1]+q1n[0])*eps_t/48,
                                -(q2n[2]-q2n[0])*eps_t/24+(q2n[2]-2*q2n[1]+q2n[0])*eps_t/48,
                                kappa*CONJ(q1n[2]-q1n[0])*eps_t/24-kappa*CONJ(q1n[2]-2*q1n[1]+q1n[0])*eps_t/48,
                                kappa*CONJ(q2n[2]-q2n[0])*eps_t/24-kappa*CONJ(q2n[2]-2*q2n[1]+q2n[0])*eps_t/48);

				p11[0] = e_3_8B[1]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[3] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[3] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[3] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[6] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[3] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[6] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[6] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[6];
                p11[1] = 0.0;
                p11[2] = e_3_8B[0]*e_3_8B[1]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[3] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[3] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[6] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[2] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[6] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[2] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[3] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[3] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[3] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[3] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[3] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[6] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[3] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[6] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[3] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[3] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[3] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[3] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[3] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[6] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[6] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[6] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[6] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[3] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[6] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[3] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[6] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[3] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[3] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[6] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[6] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[6] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[6] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[6] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[3] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[6] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[6] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[6];
                p11[3] =  0.0;
                p11[4] = e_3_8B[0]*e_3_8B[0]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[0] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[0] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[3] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[6] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[6] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[3] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[6] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[3] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[6] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[3] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[1] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[3] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[1] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[2] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[3] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[2] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[6] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[3] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[3] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[6] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[6] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[3] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[6] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[6] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[3] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[3] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[6] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[3] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[2] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[6] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[3] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[2] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[6] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[3] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[2] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[6] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[2] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[6] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[3] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[2] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[6] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[2] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[3] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[3] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[6] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[3] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[3] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[3] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[6] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[3] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[3] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[6] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[3] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[6] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[6] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[6] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[6] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[3] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[6] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[6];
                p11[5] = 0.0;
                p11[6] = e_3_8B[0]*e_3_8B[1]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[0] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[0] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[0] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[0] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[0] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[0] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[0] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[0] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[1]*e_7_48B[3] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[3] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[2]*e_7_48B[3] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[1]*e_7_48B[6] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[1] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[6] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[2]*e_7_48B[6] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[2] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[3] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[1] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[6] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[2] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[3] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[3] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[3] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[1] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[1] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[1] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[6] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[3] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[6] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[3] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[6] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[3] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[1] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[2] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[2] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[1] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[1] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[2] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[6] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[6] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[6] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[2] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[2] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[2];
                p11[7] = 0.0;
                p11[8] = e_3_8B[1]*e_3_8B[3]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[0] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[0] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[0] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[0] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[0] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[0] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[0] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[0] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[0]*e_7_48B[3] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[0]*e_7_48B[6] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[0]*e_7_48B[2] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[3] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[1]*e_7_48B[3] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[3] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[3] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[6] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[1]*e_7_48B[6] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[1]*e_7_48B[3] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[2]*e_7_48B[3] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[3] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[6] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[6] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[1]*e_7_48B[6] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[2]*e_7_48B[6] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[2]*e_7_48B[3] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[6] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[2]*e_7_48B[6];
				p11[9] = 0.0;
                p11[10] = e_3_8B[0]*e_3_8B[0]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[0]*e_7_48B[0] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[0]*e_7_48B[1] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[3] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[0]*e_7_48B[2] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[3] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[6] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[3] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[6] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[6] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[0]*e_7_48B[3] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[0]*e_7_48B[6] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[0]*e_7_48B[3] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[2] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[6] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[3] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[0]*e_7_48B[2] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[0]*e_7_48B[6] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[2] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[6] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[3] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[3] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[3] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[3] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[6] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[6] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[3] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[6] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[6] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[6] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[3];
                p11[11] = 0.0;
                p11[12] = e_3_8B[0]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[0] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[0]*e_7_48B[0] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[0] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[0]*e_7_48B[0] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[3] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[1] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[6] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[3] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[3] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[1] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[2] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[1] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[6] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[6] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[3] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[2] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[1] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[2] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[6] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[2];
                p11[13] = 0.0;
                p11[14] = e_3_8B[1]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[0] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[0] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[0] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[0];

				p12[0] = e_3_8B[1]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[4] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[4] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[4] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[7] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[4] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[7] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[7] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[7];
                p12[1] = 0.0;
                p12[2] = e_3_8B[0]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[1] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[1] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[4] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[4] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[7] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[2] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[7] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[2] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[4] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[4] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[4] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[4] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[4] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[7] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[4] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[7] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[4] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[4] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[4] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[4] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[4] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[7] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[7] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[7] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[7] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[4] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[7] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[4] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[7] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[4] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[4] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[7] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[7] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[7] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[7] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[7] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[4] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[7] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[7] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[7];
                p12[3] =  0.0;
                p12[4] = e_3_8B[3]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[1] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[1] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[4] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[7] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[1] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[1] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[1] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[1] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[1] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[1] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[7] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[4] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[7] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[4] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[7] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[4] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[1] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[4] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[2] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[4] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[2] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[7] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[4] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[4] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[7] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[7] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[4] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[7] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[7] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[4] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[4] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[7] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[4] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[2] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[7] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[4] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[2] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[7] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[4] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[2] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[7] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[2] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[7] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[4] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[2] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[7] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[2] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[4] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[4] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[7] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[4] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[4] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[4] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[7] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[4] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[4] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[7] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[4] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[7] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[7] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[7] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[7] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[4] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[7] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[7];
                p12[5] = 0.0;
                p12[6] = e_3_8B[3]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[1] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[1] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[1] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[1] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[1] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[1] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[1] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[1] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[1]*e_7_48B[4] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[4] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[2]*e_7_48B[4] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[1]*e_7_48B[7] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[7] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[2]*e_7_48B[7] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[2] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[4] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[7] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[2] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[1] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[4] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[4] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[4] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[7] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[4] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[7] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[4] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[7] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[4] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[2] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[2] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[2] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[7] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[7] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[7] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[2] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[2] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[2];
                p12[7] = 0.0;
                p12[8] = e_3_8B[0]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[1]*e_7_48B[1] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[0]*e_7_48B[4] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[0]*e_7_48B[7] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[1]*e_7_48B[2] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[1] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[1] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[4] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[1]*e_7_48B[4] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[1] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[4] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[1] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[4] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[1] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[7] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[1]*e_7_48B[7] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[1]*e_7_48B[4] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[2]*e_7_48B[4] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[1] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[4] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[1] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[7] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[1] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[7] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[1]*e_7_48B[7] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[2]*e_7_48B[7] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[2]*e_7_48B[4] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[7] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[2]*e_7_48B[7];
				p12[9] = 0.0;
                p12[10] = e_3_8B[3]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[1]*e_7_48B[1] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[1] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[1] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[1]*e_7_48B[1] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[4] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[1]*e_7_48B[2] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[4] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[7] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[4] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[7] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[7] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[0]*e_7_48B[4] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[0]*e_7_48B[7] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[0]*e_7_48B[4] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[2] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[7] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[4] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[1]*e_7_48B[2] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[0]*e_7_48B[7] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[2] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[7] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[4] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[4] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[4] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[4] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[7] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[7] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[4] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[7] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[7] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[7] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[4];
                p12[11] = 0.0;
                p12[12] =  e_3_8B[3]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[1] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[1] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[1] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[1] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[1] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[0]*e_7_48B[1] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[4] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[7] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[4] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[4] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[2] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[7] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[7] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[4] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[2] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[2] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[7] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[2];
                p12[13] = 0.0;
                p12[14] = e_3_8B[1]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[1] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[1] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[1] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[1];
				
				p13[0] = e_3_8B[1]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[5] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[5] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[5] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[8] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[5] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[8] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[8] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[8];
                p13[1] = 0.0;
                p13[2] = e_3_8B[0]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[2] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[2] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[5] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[2] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[5] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[8] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[2] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[8] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[5] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[5] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[5] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[5] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[5] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[8] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[5] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[8] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[5] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[5] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[5] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[5] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[5] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[8] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[8] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[8] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[8] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[5] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[8] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[5] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[8] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[5] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[5] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[8] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[8] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[8] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[8] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[8] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[5] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[8] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[8] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[8];
                p13[3] =  0.0;
                p13[4] = e_3_8B[6]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[2] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[2] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[5] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[8] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[2] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[2] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[2] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[2] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[2] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[2] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[8] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[5] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[5] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[5] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[2] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[2] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[2] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[5] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[2] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[5] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[8] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[5] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[5] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[8] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[8] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[5] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[8] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[8] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[5] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[2] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[5] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[2] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[8] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[2] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[5] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[8] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[5] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[2] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[8] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[2] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[5] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[8] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[8] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[5] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[2] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[8] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[5] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[5] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[8] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[5] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[5] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[5] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[8] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[5] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[5] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[8] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[5] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[8] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[8] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[5] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[8];
                p13[5] = 0.0;
                p13[6] = e_3_8B[3]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[2] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[2] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[2] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[2] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[2] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[2] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[2] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[2] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[1]*e_7_48B[5] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[5] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[2]*e_7_48B[5] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[1]*e_7_48B[8] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[2] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[8] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[2]*e_7_48B[8] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[5] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[2] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[8] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[2] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[2] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[2] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[2] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[2] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[2] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[2] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[2] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[5] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[5] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[5] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[2] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[2] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[2] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[8] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[5] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[8] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[5] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[8] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[5] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[2] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[2] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[2] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[8] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[8] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[8];
                p13[7] = 0.0;
                p13[8] = e_3_8B[0]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[2]*e_7_48B[2] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[0]*e_7_48B[5] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[1]*e_7_48B[2] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[0]*e_7_48B[8] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[2] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[2] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[5] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[1]*e_7_48B[5] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[2] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[5] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[2] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[5] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[2] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[8] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[1]*e_7_48B[8] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[1]*e_7_48B[5] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[2]*e_7_48B[5] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[2] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[5] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[2] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[8] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[2] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[8] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[1]*e_7_48B[8] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[2]*e_7_48B[8] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[2]*e_7_48B[5] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[8] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[2]*e_7_48B[8];
				p13[9] = 0.0;
                p13[10] = e_3_8B[6]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[2]*e_7_48B[2] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[0]*e_7_48B[2] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[2] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[2]*e_7_48B[2] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[2] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[1]*e_7_48B[2] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[5] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[5] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[8] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[5] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[8] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[8] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[0]*e_7_48B[5] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[2] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[0]*e_7_48B[8] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[2] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[0]*e_7_48B[5] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[8] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[5] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[1]*e_7_48B[2] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[0]*e_7_48B[8] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[8] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[5] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[5] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[5] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[5] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[5] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[8] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[5];
                p13[11] = 0.0;
                p13[12] =  e_3_8B[3]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[2] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[2] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[2] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[2] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[2] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[0]*e_7_48B[2] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[2] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[0]*e_7_48B[2] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[5] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[2] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[8] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[5] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[5] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[2] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[2] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[8] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[8] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[5] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[2] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[8];
                p13[13] = 0.0;
                p13[14] = e_3_8B[1]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[2] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[2] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[2] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[2];
				
				p21[0] = e_3_8B[1]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[4] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[4] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[5] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[6] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[5] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[6] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[6] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[6];
                p21[1] = 0.0;
                p21[2] = e_3_8B[0]*e_3_8B[1]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[3] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[3] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[4] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[6] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[4] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[5] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[6] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[5] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[4] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[4] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[4] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[4] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[5] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[6] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[4] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[6] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[3]*e_7_48B[4] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[5] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[4] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[5] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[5] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[6] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[6] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[5]*e_7_48B[6] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[6] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[4] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[4]*e_7_48B[6] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[5] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[5]*e_7_48B[6] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[3]*e_7_48B[5] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[5] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[6] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[6] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[6] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[6] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[6] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[5] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[5]*e_7_48B[6] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[6] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[6];
                p21[3] =  0.0;
                p21[4] = e_3_8B[1]*e_3_8B[1]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[3] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[3] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[4] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[6] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[3] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[3] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[3]*e_7_48B[3] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[3] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[3] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[3] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[6] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[5] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[5]*e_7_48B[6] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[4] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[6] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[5] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[3] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[3] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[4] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[4] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[5] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[5] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[6] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[4] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[3]*e_7_48B[5] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[6] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[6] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[5] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[6] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[6] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[4] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[4] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[6] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[4] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[5] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[6] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[5] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[4] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[6] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[4] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[5] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[3]*e_7_48B[6] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[5] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[6] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[5] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[4] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[6] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[5] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[4] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[3]*e_7_48B[4] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[6] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[4] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[5] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[4] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[4]*e_7_48B[6] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[5] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[4] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[6] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[5] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[6] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[6] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[6] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[5]*e_7_48B[6] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[5] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[6] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[6];
                p21[5] = 0.0;
                p21[6] = e_3_8B[1]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[3] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[3] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[3] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[3]*e_7_48B[3] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[3] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[3] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[3] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[3] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[3]*e_7_48B[4] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[3]*e_7_48B[5] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[4]*e_7_48B[6] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[4] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[6] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[5]*e_7_48B[6] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[5] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[4] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[6] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[5] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[3] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[3] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[3] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[3] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[3] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[3] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[3] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[3] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[4] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[4] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[4] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[6] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[3]*e_7_48B[6] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[6] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[4] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[5] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[5] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[4] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[4] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[5] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[6] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[6] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[6] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[5] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[5] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[5];
                p21[7] = 0.0;
                p21[8] = e_3_8B[0]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[3]*e_7_48B[3] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[0]*e_7_48B[4] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[3]*e_7_48B[6] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[0]*e_7_48B[5] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[3] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[3] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[4] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[3]*e_7_48B[4] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[3] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[4] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[3] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[5] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[3] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[6] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[4]*e_7_48B[6] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[3]*e_7_48B[4] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[3]*e_7_48B[5] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[3] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[5] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[3] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[6] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[3] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[6] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[4]*e_7_48B[6] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[5]*e_7_48B[6] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[3]*e_7_48B[5] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[6] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[5]*e_7_48B[6];
				p21[9] = 0.0;
                p21[10] = e_3_8B[1]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[3] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[0]*e_7_48B[3] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[3]*e_7_48B[3] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[3]*e_7_48B[3] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[3] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[0]*e_7_48B[4] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[0]*e_7_48B[5] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[4] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[6] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[5] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[6] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[6] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[4] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[3]*e_7_48B[6] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[4] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[5] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[6] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[0]*e_7_48B[5] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[0]*e_7_48B[4] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[3]*e_7_48B[6] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[5] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[6] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[4] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[5] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[4] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[5] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[6] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[6] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[4] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[6] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[6] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[6] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[5];
                p21[11] = 0.0;
                p21[12] = e_3_8B[1]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[3] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[3] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[3] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[3] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[3] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[0]*e_7_48B[3] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[3] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[0]*e_7_48B[3] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[4] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[6] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[4] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[5] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[4] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[6] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[6] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[5] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[4] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[5] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[6] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[5];
                p21[13] = 0.0;
                p21[14] = e_3_8B[1]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[3] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[3] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[3] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[3];
				
				p22[0] = e_3_8B[1]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[4] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[4] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[5] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[7] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[5] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[7] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[7] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[7];
                p22[1] = 0.0;
                p22[2] = e_3_8B[1]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[4] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[4] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[4] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[4] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[4] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[4]*e_7_48B[4] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[4] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[4] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[4] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[4] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[4] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[7] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[4] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[5] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[7] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[5] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[5] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[7] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[7] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[5] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[5] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[5] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[7] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[7] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[5]*e_7_48B[7] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[7] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[4]*e_7_48B[7] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[5] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[5]*e_7_48B[7] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[4]*e_7_48B[5] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[5] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[7] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[7] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[7] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[7] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[7] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[5] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[5]*e_7_48B[7] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[7] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[7];
                p22[3] =  0.0;
                p22[4] = e_3_8B[4]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[4] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[4] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[4] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[7] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[4] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[4]*e_7_48B[4] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[4] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[4] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[4] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[7] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[5] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[5]*e_7_48B[7] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[7] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[5] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[3] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[3] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[4] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[4] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[4] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[5] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[4] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[5] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[7] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[4]*e_7_48B[5] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[7] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[7] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[5] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[7] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[7] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[4] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[4] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[4] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[4] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[7] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[4] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[3]*e_7_48B[4] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[5] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[7] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[4] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[5] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[4] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[7] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[4] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[4] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[5] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[3]*e_7_48B[7] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[5] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[7] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[4] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[5] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[4] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[7] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[5] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[7] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[5] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[4]*e_7_48B[7] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[5] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[7] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[5] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[7] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[7] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[7] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[5]*e_7_48B[7] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[5] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[7] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[7];
                p22[5] = 0.0;
                p22[6] = e_3_8B[1]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[4]*e_7_48B[4] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[4] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[4]*e_7_48B[5] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[4]*e_7_48B[7] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[4] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[7] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[5]*e_7_48B[7] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[5] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[4] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[4] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[7] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[5] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[3] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[3] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[3] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[3] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[3] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[3] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[3] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[3] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[4] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[3]*e_7_48B[4] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[4] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[4] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[4] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[4] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[7] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[4] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[3]*e_7_48B[7] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[4] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[7] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[4] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[4] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[5] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[5] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[4] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[4] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[5] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[7] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[7] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[7] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[5] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[5] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[5];
                p22[7] = 0.0;
                p22[8] = e_3_8B[1]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[4] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[4]*e_7_48B[4] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[4] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[4]*e_7_48B[4] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[3]*e_7_48B[4] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[1]*e_7_48B[4] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[3]*e_7_48B[7] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[1]*e_7_48B[5] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[3] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[3] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[3] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[3] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[5] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[3] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[7] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[4]*e_7_48B[7] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[4]*e_7_48B[5] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[3] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[5] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[3] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[7] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[3] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[7] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[4]*e_7_48B[7] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[5]*e_7_48B[7] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[4]*e_7_48B[5] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[7] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[5]*e_7_48B[7];
				p22[9] = 0.0;
                p22[10] = e_3_8B[4]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[4] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[1]*e_7_48B[3] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[4] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[4] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[4] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[1]*e_7_48B[4] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[4] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[1]*e_7_48B[5] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[7] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[5] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[7] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[7] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[3]*e_7_48B[4] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[4] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[3]*e_7_48B[7] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[4] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[3]*e_7_48B[4] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[5] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[7] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[4] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[1]*e_7_48B[5] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[1]*e_7_48B[4] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[3]*e_7_48B[7] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[5] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[7] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[5] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[5] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[7] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[7] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[7] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[7] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[7] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[5];
                p22[11] = 0.0;
                p22[12] =  e_3_8B[0]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[3] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[1]*e_7_48B[3] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[3] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[1]*e_7_48B[3] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[4] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[4] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[7] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[4] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[4] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[4] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[5] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[4] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[7] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[7] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[4] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[5] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[4] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[5] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[7] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[5];
                p22[13] = 0.0;
                p22[14] = e_3_8B[1]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[3] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[3] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[3] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[3];
				
				p23[0] = e_3_8B[1]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[5] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[5] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[5] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[5] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[8] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[8] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[8] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[8];
                p23[1] = 0.0;
                p23[2] = e_3_8B[1]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[5]*e_7_48B[5] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[5]*e_7_48B[5] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[5] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[5] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[5] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[5]*e_7_48B[5] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[5] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[5] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[5] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[4] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[5] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[8] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[4] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[5] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[8] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[5] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[5] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[5] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[5] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[5] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[8] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[5] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[8] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[4]*e_7_48B[5] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[5] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[8] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[8] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[5]*e_7_48B[8] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[8] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[5] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[4]*e_7_48B[8] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[5]*e_7_48B[8] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[8] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[8] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[8] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[8] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[8] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[5]*e_7_48B[8] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[8] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[8];
                p23[3] =  0.0;
                p23[4] = e_3_8B[4]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[5]*e_7_48B[5] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[5] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[5]*e_7_48B[5] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[5] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[5] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[8] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[5] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[5] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[5] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[5] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[5]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[5] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[8] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[3] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[3] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[4] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[5] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[4] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[5] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[5] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[5] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[8] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[5] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[8] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[8] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[8] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[8] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[5] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[4] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[5] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[4] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[8] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[4] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[3]*e_7_48B[5] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[5] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[8] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[5] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[5] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[4] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[8] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[4] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[5] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[5] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[3]*e_7_48B[8] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[5] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[8] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[5] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[5] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[4] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[8] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[5] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[5] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[4]*e_7_48B[5] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[8] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[5] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[5] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[4]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[5] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[8] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[8] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[5]*e_7_48B[8] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[8];
                p23[5] = 0.0;
                p23[6] = e_3_8B[1]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[5]*e_7_48B[5] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[4]*e_7_48B[5] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[5] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[4]*e_7_48B[8] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[4] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[8] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[5]*e_7_48B[8] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[5] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[5] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[4] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[8] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[5] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[3] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[3] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[3] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[3] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[3] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[3] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[3] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[3] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[5] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[3]*e_7_48B[5] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[5] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[4] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[4] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[4] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[8] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[5] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[3]*e_7_48B[8] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[5] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[8] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[5] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[4] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[5] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[5] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[4] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[4] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[5] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[8] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[8] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[8] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[5] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[5] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[5];
                p23[7] = 0.0;
                p23[8] = e_3_8B[1]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[5] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[5]*e_7_48B[5] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[5] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[5]*e_7_48B[5] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[3]*e_7_48B[5] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[2]*e_7_48B[4] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[3]*e_7_48B[8] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[2]*e_7_48B[5] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[3] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[3] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[5] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[4]*e_7_48B[5] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[3] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[5] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[3] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[3] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[8] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[4]*e_7_48B[8] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[4]*e_7_48B[5] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[3] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[3] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[8] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[3] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[8] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[4]*e_7_48B[8] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[5]*e_7_48B[8] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[8] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[5]*e_7_48B[8];
				p23[9] = 0.0;
                p23[10] = e_3_8B[7]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[5] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[2]*e_7_48B[3] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[5] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[5] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[5] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[2]*e_7_48B[4] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[5] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[2]*e_7_48B[5] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[5] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[8] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[8] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[8] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[3]*e_7_48B[5] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[4] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[3]*e_7_48B[8] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[4] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[3]*e_7_48B[5] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[5] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[8] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[5] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[2]*e_7_48B[5] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[2]*e_7_48B[4] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[3]*e_7_48B[8] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[5] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[8] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[5] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[5] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[5] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[8] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[8];
                p23[11] = 0.0;
                p23[12] =  e_3_8B[0]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[3] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[2]*e_7_48B[3] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[3] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[2]*e_7_48B[3] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[5] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[4] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[8] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[5] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[5] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[4] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[5] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[4] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[8] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[8] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[5] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[5] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[4] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[5] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[8] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[5];
                p23[13] = 0.0;
                p23[14] = e_3_8B[1]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[3] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[3] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[3] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[3];
				
				p31[0] = e_3_8B[1]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[7] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[7] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[8] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[6]*e_7_48B[7] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[8] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[7] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[6]*e_7_48B[8] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[8];
                p31[1] = 0.0;
                p31[2] = e_3_8B[0]*e_3_8B[2]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[6]*e_7_48B[6] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[6] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[6] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[7] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[6] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[7] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[8] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[8] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[7] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[7] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[7] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[7] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[8] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[6]*e_7_48B[7] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[7] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[6]*e_7_48B[7] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[3]*e_7_48B[7] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[8] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[7] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[8] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[8] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[7] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[7] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[6]*e_7_48B[8] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[7] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[7] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[6]*e_7_48B[7] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[8] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[6]*e_7_48B[8] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[3]*e_7_48B[8] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[8] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[7] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[8] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[8] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[7] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[8] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[8] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[6]*e_7_48B[8] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[8] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[8];
                p31[3] =  0.0;
                p31[4] = e_3_8B[2]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[6] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[6] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[7] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[8] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[6]*e_7_48B[6] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[6]*e_7_48B[6] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[6] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[6]*e_7_48B[6] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[6] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[6] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[6]*e_7_48B[7] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[6]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[7] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[7] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[8] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[6] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[6] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[7] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[6] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[7] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[8] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[6] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[8] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[7] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[3]*e_7_48B[8] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[7] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[8] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[7] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[8] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[6] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[7] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[6] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[7] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[7] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[3]*e_7_48B[6] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[8] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[6] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[8] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[7] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[7] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[6] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[8] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[8] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[6] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[8] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[7] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[8] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[7] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[3]*e_7_48B[7] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[7] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[7] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[8] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[7] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[6]*e_7_48B[7] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[7] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[7] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[8] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[8] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[7] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[6]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[8] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[8];
                p31[5] = 0.0;
                p31[6] = e_3_8B[1]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[6]*e_7_48B[6] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[6] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[6] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[6]*e_7_48B[6] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[6] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[6] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[6] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[6] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[3]*e_7_48B[7] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[3]*e_7_48B[6] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[3]*e_7_48B[8] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[6]*e_7_48B[7] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[7] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[6]*e_7_48B[8] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[8] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[6] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[7] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[8] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[6] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[6] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[6] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[6] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[6] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[6] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[6] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[6] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[6] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[3]*e_7_48B[6] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[6] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[7] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[7] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[7] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[6] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[3]*e_7_48B[6] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[3]*e_7_48B[6] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[7] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[8] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[8] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[7] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[7] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[8] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[8] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[8] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[8];
                p31[7] = 0.0;
                p31[8] = e_3_8B[0]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[6]*e_7_48B[6] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[3]*e_7_48B[6] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[0]*e_7_48B[7] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[0]*e_7_48B[8] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[0]*e_7_48B[6] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[6] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[7] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[3]*e_7_48B[7] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[6] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[7] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[0]*e_7_48B[6] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[8] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[6] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[6]*e_7_48B[7] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[6]*e_7_48B[7] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[3]*e_7_48B[7] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[3]*e_7_48B[8] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[6] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[8] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[0]*e_7_48B[6] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[7] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[0]*e_7_48B[6] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[6]*e_7_48B[8] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[6]*e_7_48B[7] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[6]*e_7_48B[8] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[3]*e_7_48B[8] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[8] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[6]*e_7_48B[8];
				p31[9] = 0.0;
                p31[10] = e_3_8B[2]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[6] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[0]*e_7_48B[6] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[6]*e_7_48B[6] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[6]*e_7_48B[6] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[6]*e_7_48B[6] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[0]*e_7_48B[7] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[6] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[0]*e_7_48B[8] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[7] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[8] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[7] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[8] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[3]*e_7_48B[6] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[7] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[7] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[3]*e_7_48B[6] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[8] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[6] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[0]*e_7_48B[8] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[0]*e_7_48B[7] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[8] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[6]*e_7_48B[7] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[7] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[8] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[7] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[8] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[6]*e_7_48B[7] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[6]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[7] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[8] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[7] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[6]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[8];
                p31[11] = 0.0;
                p31[12] =  e_3_8B[1]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[6]*e_7_48B[6] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[6]*e_7_48B[6] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[6] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[6] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[6] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[0]*e_7_48B[6] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[6] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[0]*e_7_48B[6] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[6] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[7] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[3]*e_7_48B[6] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[6] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[7] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[8] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[7] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[3]*e_7_48B[6] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[8] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[7] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[8] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[8];
                p31[13] = 0.0;
                p31[14] =e_3_8B[1]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[6] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[0]*e_7_48B[6] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[6] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[0]*e_7_48B[6];
				
				p32[0] = e_3_8B[2]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[7]*e_7_48B[7] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[7]*e_7_48B[7] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[7] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[7] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[8] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[8] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[7]*e_7_48B[8] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[7]*e_7_48B[8];
                p32[1] = 0.0;
                p32[2] = e_3_8B[2]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[7]*e_7_48B[7] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[7]*e_7_48B[7] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[7]*e_7_48B[7] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[7]*e_7_48B[7] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[7]*e_7_48B[7] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[7]*e_7_48B[7] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[7]*e_7_48B[7] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[7]*e_7_48B[7] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[6] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[7] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[6] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[6]*e_7_48B[7] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[7] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[8] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[7] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[8] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[7] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[7] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[7] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[7] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[8] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[7] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[4]*e_7_48B[7] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[8] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[7] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[8] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[8] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[7]*e_7_48B[8] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[7] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[8] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[7]*e_7_48B[8] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[4]*e_7_48B[8] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[8] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[7]*e_7_48B[8] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[7]*e_7_48B[8] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[7]*e_7_48B[8] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[8] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[7]*e_7_48B[8] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[7]*e_7_48B[8] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[7]*e_7_48B[8];
                p32[3] =  0.0;
                p32[4] = e_3_8B[4]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[7]*e_7_48B[7] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[7]*e_7_48B[7] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[7]*e_7_48B[7] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[7]*e_7_48B[7] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[7] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[7]*e_7_48B[8] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[7]*e_7_48B[7] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[7]*e_7_48B[7] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[7]*e_7_48B[7] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[7]*e_7_48B[7] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[7]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[7] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[8] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[6] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[6] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[7] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[6] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[7] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[8] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[6] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[8] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[7] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[7] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[4]*e_7_48B[8] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[7] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[8] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[7]*e_7_48B[8] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[6] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[7] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[6] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[7] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[6]*e_7_48B[7] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[7] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[4]*e_7_48B[6] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[8] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[6]*e_7_48B[7] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[6] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[8] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[7] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[7] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[7] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[6] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[8] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[6]*e_7_48B[7] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[8] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[7] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[6] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[8] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[7] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[7] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[8] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[7] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[4]*e_7_48B[7] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[7] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[8] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[7] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[7] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[7]*e_7_48B[8] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[7]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[7]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[8] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[7]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[7]*e_7_48B[8];
                p32[5] = 0.0;
                p32[6] = e_3_8B[2]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[7]*e_7_48B[7] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[4]*e_7_48B[7] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[4]*e_7_48B[6] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[4]*e_7_48B[8] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[7] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[6]*e_7_48B[7] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[7]*e_7_48B[8] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[8] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[6] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[7] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[7] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[8] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[6] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[6] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[6] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[6] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[6] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[6] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[6] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[6] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[6] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[4]*e_7_48B[6] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[6] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[7] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[7] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[7] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[7] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[6] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[6]*e_7_48B[7] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[4]*e_7_48B[6] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[7] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[4]*e_7_48B[6] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[7] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[8] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[8] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[7] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[7] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[8] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[7] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[7] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[7] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[8] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[8] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[8];
                p32[7] = 0.0;
                p32[8] = e_3_8B[2]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[7]*e_7_48B[7] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[7]*e_7_48B[7] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[7]*e_7_48B[7] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[7]*e_7_48B[7] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[4]*e_7_48B[6] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[1]*e_7_48B[7] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[6]*e_7_48B[7] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[1]*e_7_48B[8] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[1]*e_7_48B[6] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[6] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[7] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[4]*e_7_48B[7] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[6] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[7] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[1]*e_7_48B[6] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[8] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[6] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[4]*e_7_48B[7] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[4]*e_7_48B[8] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[6] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[8] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[1]*e_7_48B[6] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[1]*e_7_48B[6] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[7]*e_7_48B[8] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[7]*e_7_48B[8] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[4]*e_7_48B[8] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[7]*e_7_48B[8] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[7]*e_7_48B[8];
				p32[9] = 0.0;
                p32[10] = e_3_8B[5]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[7]*e_7_48B[7] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[1]*e_7_48B[6] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[7]*e_7_48B[7] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[7]*e_7_48B[7] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[7]*e_7_48B[7] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[1]*e_7_48B[7] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[6] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[1]*e_7_48B[8] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[7] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[7] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[8] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[7]*e_7_48B[8] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[4]*e_7_48B[6] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[7] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[6]*e_7_48B[7] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[7] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[4]*e_7_48B[6] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[8] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[6]*e_7_48B[7] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[6] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[1]*e_7_48B[8] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[1]*e_7_48B[7] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[6]*e_7_48B[7] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[8] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[7] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[8] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[7] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[7]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[7] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[7]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[7]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[8];
                p32[11] = 0.0;
                p32[12] =  e_3_8B[0]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[6] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[1]*e_7_48B[6] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[6] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[1]*e_7_48B[6] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[6] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[7] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[6]*e_7_48B[7] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[4]*e_7_48B[6] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[6] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[7] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[8] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[7] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[6]*e_7_48B[7] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[7] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[4]*e_7_48B[6] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[8] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[7] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[8] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[7] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[8];
                p32[13] = 0.0;
                p32[14] = e_3_8B[1]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[6] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[1]*e_7_48B[6] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[6] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[1]*e_7_48B[6];
				
				p33[0] = e_3_8B[2]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[8]*e_7_48B[8] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[8]*e_7_48B[8] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[7] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[7] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[8] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[7]*e_7_48B[8] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[8] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[7]*e_7_48B[8];
                p33[1] = 0.0;
                p33[2] = e_3_8B[2]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[8]*e_7_48B[8] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[8]*e_7_48B[8] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[8]*e_7_48B[8] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[8]*e_7_48B[8] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[8]*e_7_48B[8] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[8]*e_7_48B[8] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[8]*e_7_48B[8] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[8]*e_7_48B[8] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[6] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[7] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[6] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[6]*e_7_48B[8] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[7] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[8] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[8] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[8] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[5]*e_7_48B[7] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[5]*e_7_48B[7] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[7] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[7] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[5]*e_7_48B[8] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[7]*e_7_48B[8] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[7] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[7]*e_7_48B[8] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[5]*e_7_48B[7] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[5]*e_7_48B[8] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[7] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[8] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[8] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[7]*e_7_48B[8] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[7]*e_7_48B[8] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[7]*e_7_48B[8] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[7] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[7]*e_7_48B[8] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[8] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[5]*e_7_48B[8] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[8] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[7]*e_7_48B[8] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[7]*e_7_48B[8] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[8];
                p33[3] =  0.0;
                p33[4] = e_3_8B[8]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[8]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[8]*e_7_48B[8] + e_3_8B[8]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[8]*e_7_48B[8] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[5]*e_7_48B[7] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[8]*e_7_48B[8] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[8]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[8]*e_7_48B[8] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[8]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[8]*e_7_48B[8] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[7]*e_7_48B[8] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[5]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[7] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[7]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[8] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[6] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[6] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[7] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[5]*e_7_48B[6] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[7] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[8] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[6] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[8] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[8] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[7] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[5]*e_7_48B[8] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[8] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[7]*e_7_48B[8] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[8] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[7]*e_7_48B[8] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[5]*e_7_48B[6] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[7] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[6] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[7] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[6]*e_7_48B[8] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[7] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[5]*e_7_48B[6] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[8] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[6]*e_7_48B[8] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[6] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[8] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[7] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[8] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[7] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[6] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[8] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[6]*e_7_48B[8] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[8] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[8] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[6] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[8] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[7] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[8] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[8] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[7] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[5]*e_7_48B[7] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[7]*e_7_48B[8] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[7] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[8] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[7] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[7]*e_7_48B[8] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[7] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[7]*e_7_48B[8] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[8] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[7]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[8];
                p33[5] = 0.0;
                p33[6] = e_3_8B[2]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[8]*e_7_48B[8] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[5]*e_7_48B[7] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[5]*e_7_48B[6] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[5]*e_7_48B[8] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[7]*e_7_48B[8] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[7] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[6]*e_7_48B[8] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[8] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[6] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[7] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[8] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[8] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[3]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[6] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[6] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[5]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[6] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[3]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[6] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[2]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[6] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[1]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[6] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[6]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[6] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[6] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[6] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[5]*e_7_48B[6] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[6] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[7] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[7] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[7] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[8] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[6] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[6]*e_7_48B[8] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[5]*e_7_48B[6] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[8] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[5]*e_7_48B[6] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[7] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[8] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[8] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[7] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[7] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[8] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[8] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[6]*e_7_48B[8] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[6]*e_7_48B[8] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[8] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[8] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[8];
                p33[7] = 0.0;
                p33[8] = e_3_8B[2]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[8]*e_7_48B[8] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[8]*e_7_48B[8] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[8]*e_7_48B[8] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[8]*e_7_48B[8] + e_3_8B[0]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[5]*e_7_48B[6] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[2]*e_7_48B[7] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[6]*e_7_48B[8] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[2]*e_7_48B[8] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[4]*e_m1_48B[4]*e_7_48B[2]*e_7_48B[6] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[8]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[6] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[7] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[5]*e_7_48B[7] + e_3_8B[1]*e_3_8B[3]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[6] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[7] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[4]*e_m1_48B[5]*e_7_48B[2]*e_7_48B[6] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[8] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[4]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[6] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[7]*e_7_48B[8] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[7]*e_7_48B[8] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[5]*e_7_48B[7] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[5]*e_7_48B[8] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[6] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[8] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[7]*e_m1_48B[8]*e_7_48B[2]*e_7_48B[6] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[7]*e_7_48B[8] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[5]*e_m1_48B[7]*e_7_48B[2]*e_7_48B[6] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[7]*e_7_48B[8] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[5]*e_7_48B[8];
				p33[9] = 0.0;
                p33[10] = e_3_8B[8]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[8]*e_7_48B[8] + e_3_8B[0]*e_3_8B[0]*e_m1_48B[0]*e_m1_48B[0]*e_7_48B[2]*e_7_48B[6] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[8]*e_7_48B[8] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[8]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[8]*e_7_48B[8] + e_3_8B[3]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[2]*e_7_48B[7] + e_3_8B[1]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[6] + e_3_8B[6]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[2]*e_7_48B[8] + e_3_8B[4]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[7] + e_3_8B[2]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[8] + e_3_8B[7]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[8] + e_3_8B[5]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[7]*e_7_48B[8] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[5]*e_7_48B[6] + e_3_8B[0]*e_3_8B[4]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[7] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[6]*e_7_48B[8] + e_3_8B[0]*e_3_8B[5]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[7] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[5]*e_7_48B[6] + e_3_8B[0]*e_3_8B[7]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[8] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[6]*e_7_48B[8] + e_3_8B[1]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[6] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[2]*e_7_48B[8] + e_3_8B[3]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[2]*e_7_48B[7] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[6]*e_7_48B[8] + e_3_8B[0]*e_3_8B[8]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[8] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[7]*e_7_48B[8] + e_3_8B[4]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[7] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[8] + e_3_8B[4]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[7] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[8] + e_3_8B[4]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[7]*e_7_48B[8] + e_3_8B[5]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[7] + e_3_8B[5]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[7]*e_7_48B[8] + e_3_8B[7]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[8];
                p33[11] = 0.0;
                p33[12] =  e_3_8B[0]*e_3_8B[1]*e_m1_48B[0]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[6] + e_3_8B[0]*e_3_8B[3]*e_m1_48B[0]*e_m1_48B[1]*e_7_48B[2]*e_7_48B[6] + e_3_8B[0]*e_3_8B[2]*e_m1_48B[0]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[6] + e_3_8B[0]*e_3_8B[6]*e_m1_48B[0]*e_m1_48B[2]*e_7_48B[2]*e_7_48B[6] + e_3_8B[1]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[6] + e_3_8B[3]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[7] + e_3_8B[1]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[6]*e_7_48B[8] + e_3_8B[1]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[5]*e_7_48B[6] + e_3_8B[2]*e_3_8B[4]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[6] + e_3_8B[3]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[7] + e_3_8B[3]*e_3_8B[7]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[8] + e_3_8B[4]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[7] + e_3_8B[1]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[6]*e_7_48B[8] + e_3_8B[2]*e_3_8B[5]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[8] + e_3_8B[2]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[5]*e_7_48B[6] + e_3_8B[3]*e_3_8B[8]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[8] + e_3_8B[5]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[7] + e_3_8B[6]*e_3_8B[7]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[8] + e_3_8B[2]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[6]*e_7_48B[8] + e_3_8B[6]*e_3_8B[8]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[8];
                p33[13] = 0.0;
                p33[14] = e_3_8B[1]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[6] + e_3_8B[1]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[3]*e_7_48B[2]*e_7_48B[6] + e_3_8B[2]*e_3_8B[3]*e_m1_48B[1]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[6] + e_3_8B[2]*e_3_8B[6]*e_m1_48B[2]*e_m1_48B[6]*e_7_48B[2]*e_7_48B[6];

                printf("i=%d\n",i);
                misc_print_buf(15,p11,"p11");
                misc_print_buf(15,p12,"p12");
                misc_print_buf(15,p13,"p13");
                misc_print_buf(15,p21,"p21");
                misc_print_buf(15,p22,"p22");
                misc_print_buf(15,p23,"p23");
                misc_print_buf(15,p31,"p31");
                misc_print_buf(15,p32,"p32");
                misc_print_buf(15,p33,"p33");


                // multiply with E1, E2
                // We do this coefficient by coefficient
// Check if multiplication code works
/*if (i==D_eff-200){
            misc_print_buf(15,p11,"p11_before");
            misc_print_buf(15,p12,"p12_before");
            misc_print_buf(15,p13,"p13_before");
            misc_print_buf(15,p21,"p21_before");
            misc_print_buf(15,p22,"p22_before");
            misc_print_buf(15,p23,"p23_before");
            misc_print_buf(15,p31,"p31_before");
            misc_print_buf(15,p32,"p32_before");
            misc_print_buf(15,p33,"p33_before");
            misc_print_buf(9,E1,"E1");
            misc_print_buf(9,E2,"E2");
}*/
//                for (j=0; j<15; j+=2){
//                manakov_fscatter_mult_E1_TM_E2(&p11[j], &p12[j], &p13[j], &p21[j], &p22[j], 
//                        &p23[j], &p31[j], &p32[j], &p33[j], E1, E2);
//                }
/*if (i==D_eff-200){
            misc_print_buf(15,p11,"p11");
            misc_print_buf(15,p12,"p12");
            misc_print_buf(15,p13,"p13");
            misc_print_buf(15,p21,"p21");
            misc_print_buf(15,p22,"p22");
            misc_print_buf(15,p23,"p23");
            misc_print_buf(15,p31,"p31");
            misc_print_buf(15,p32,"p32");
            misc_print_buf(15,p33,"p33");
}*/
                p11 += 15;
                p12 += 15;
                p13 += 15;
                p21 += 15;
                p22 += 15;
                p23 += 15;
                p31 += 15;
                p32 += 15;
                p33 += 15;
            }
            break;

        default: // Unknown discretization
            ret_code = E_INVALID_ARGUMENT(discretization);
            goto release_mem;
    }

    // Multiply the individual scattering matrices
//    misc_print_buf(9,p,"c");
    misc_print_buf(540,p,"p");
    ret_code = poly_fmult3x3(deg_ptr, D_eff, p, result, W_ptr); // replaced D by D_eff because some methods are better 
                                                                // implemented "as if" there are 2*D samples

//    misc_print_buf(540,result,"result");
    CHECK_RETCODE(ret_code, release_mem);

release_mem:
    free(p);
    return ret_code;
}




