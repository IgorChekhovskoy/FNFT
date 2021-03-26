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
 * Contributors:
 * Sander Wahls (TU Delft) 2017-2018.
 * Peter J Prins (TU Delft) 2020.
 * Lianne de Vries (TU Delft) 2021.
 */

/**
 * @file fnft__poly_fmult.h
 * @brief Fast multiplication of n polynomials.
 * @ingroup poly
 */

#ifndef FNFT__POLY_FMULT_H
#define FNFT__POLY_FMULT_H

#include "fnft__fft_wrapper_plan_t.h"

/**
 * @brief Length of the FFTs used by \link fnft__poly_fmult_two_polys \endlink.
 *
 * @param deg The degree of the polynomials to be multiplied.
 * @return The length of the (inverse) FFT (in number of complex elements)
 *   required by \link fnft__poly_fmult_two_polys \endlink.
 * @ingroup poly
 */
FNFT_UINT fnft__poly_fmult_two_polys_len(const FNFT_UINT deg);

/**
 * @brief Multiplies two polynomials.
 *
 * @ingroup poly
 * Fast multiplication of two polynomials using the FFT.
 * @param deg Degree of the polynomials.
 * @param [in] p1 Array of coefficients for the first polynomial. If p1 is NULL,
 *   then buf1 is expected to contain the FFT of the zero-padded coefficient
 *   vector of the first polynomial instead. The purpose of this feature is that
 *   the buf2 returned by another run of this function can be reused, thus
 *   saving an FFT.
 * @param [in] p2 Array of coefficients for the second polynomial. If p2 is
 *   NULL, then buf2 is expected to contain the FFT of the zero-padded
 *   coefficient vector of the second polynomial instead.
 * @param [out] result Array in which the coefficients of the result are
 *   stored.
 * @param plan_fwd Plan generated by \link fnft__fft_wrapper_create_plan
 *   \endlink for a forward FFT of the length returned by
 *   \link fnft__poly_fmult_two_polys_len \endlink.
 * @param plan_inv Plan generated by \link fnft__fft_wrapper_create_plan
 *   \endlink for an inverse FFT of the length returned by
 *   \link fnft__poly_fmult_two_polys_len \endlink.
 * @param [in,out] buf0 Buffer of the same length as the FFTs. Must be allocated
 *   allocated and free by the user using \link fnft__fft_wrapper_malloc
 *   \endlink and \link fnft__fft_wrapper_free \endlink, respectively.
 * @param [in,out] buf1 See buf0.
 * @param [in,out] buf2 See buf0. Upon exit, buf2 contains the FFT of the
 *   zero-padded coefficient vector of the second polynomial.
 * @param [in] mode If mode==0, then the product of the two polynomials
 *   is stored in result. This is the standard mode. The following modes are
 *   useful to speed-up the multiplication of polynomial matrices. If mode==1,
 *   then the product is added to the values currently stored in result. If
 *   mode==3, then the FFT of the product of the (zero-padded) polynomials
 *   is stored in result. If mode==4, then the FFT of the two (zero-padded)
 *   polynomials is added to the values currently stored in result, an inverse
 *   FFT is applied to these values, and the outcome is finally stored in
 *   result. IMPORTANT: The modes 3 and 4 require that the array result is large
 *   enough to store an FFT of the size given above.
 * @return \link FNFT_SUCCESS \endlink or one of the FNFT_EC_... error codes
 *  defined in \link fnft_errwarn.h \endlink.
 */
FNFT_INT fnft__poly_fmult_two_polys(
    const FNFT_UINT deg,
    FNFT_COMPLEX const * const p1,
    FNFT_COMPLEX const * const p2,
    FNFT_COMPLEX * const result,
    fnft__fft_wrapper_plan_t plan_fwd,
    fnft__fft_wrapper_plan_t plan_inv,
    FNFT_COMPLEX * const buf0,
    FNFT_COMPLEX * const buf1,
    FNFT_COMPLEX * const buf2,
    const FNFT_UINT mode);

/**
 * @brief Multiplies two 2x2 matrices of polynomials.
 *
 * @ingroup poly
 * Fast multiplication of two 2x2 matrices of polynomials using the FFT.
 * @param deg Degree of the polynomials.
 * @param [in] p1_11 Array of deg+1 coefficients for the upper left polynomial
 *   p1_11(z) of the first matrix p1(z).
 * @param [in] p1_stride The deg+1 coefficients of the other polynomials
 *   p1_12(z), p1_21(z) and p1_22(z) in the first matrix are expected to be
 *   stored at p1_11+p1_stride, p1_11+2*p1_stride and p1+3*p1_stride,
 *   respectively.
 * @param [in] p2_11 Array of deg+1 coefficients for the upper left polynomial
 *   p2_11(z) of the second matrix p2(z).
 * @param [in] p2_stride The deg+1 coefficients of the other polynomials
 *   p2_12(z), p2_21(z) and p2_22(z) in the first matrix are expected to be
 *   stored at p2_11+p2_stride, p2_11+2*p2_stride and p2+3*p2_stride,
 *   respectively.
 * @param [out] result_11 Array in which the 2*deg+1 coefficients of the upper
 *   left polynomial result_11(z) of the product result(z)=p1(z)*p2(z) will be
 *   stored.
 * @param [in] result_stride The 2*deg+1 coefficients of the other polynomials
 *   result_12(z), result_21(z) and result_22(z) in the resulting matrix are
 *   will be stored at result_11+result_stride, result_11+2*result_stride and
 *   result+3*result_stride, respectively.
 * @param plan_fwd Plan generated by \link fnft__fft_wrapper_create_plan
 *   \endlink for a forward FFT of the length returned by
 *   \link fnft__poly_fmult_two_polys_len \endlink.
 * @param plan_inv Plan generated by \link fnft__fft_wrapper_create_plan
 *   \endlink for an inverse FFT of the length returned by
 *   \link fnft__poly_fmult_two_polys_len \endlink.
 * @param [in,out] buf0 Buffer of the same length as the FFTs. Must be allocated
 *   allocated and free by the user using \link fnft__fft_wrapper_malloc
 *   \endlink and \link fnft__fft_wrapper_free \endlink, respectively.
 * @param [in,out] buf1 See buf0.
 * @param [in,out] buf2 See buf0.
 * @param [in] mode_offset Set to 2 if the arrays result_11+n*result_stride,
 *   where n=0,1,2,3, are long enough to store the FFT's. This allows improve
 *   performance by storing some intermediate values there. Otherwise, set to 0.
 * @return \link FNFT_SUCCESS \endlink or one of the FNFT_EC_... error codes
 *   defined in \link fnft_errwarn.h \endlink.
 */
FNFT_INT fnft__poly_fmult_two_polys2x2(const FNFT_UINT deg,
    FNFT_COMPLEX const * const p1_11,
    const FNFT_UINT p1_stride,
    FNFT_COMPLEX const * const p2_11,
    const FNFT_UINT p2_stride,
    FNFT_COMPLEX * const result_11,
    const FNFT_UINT result_stride,
    fnft__fft_wrapper_plan_t plan_fwd,
    fnft__fft_wrapper_plan_t plan_inv,
    FNFT_COMPLEX * const buf0,
    FNFT_COMPLEX * const buf1,
    FNFT_COMPLEX * const buf2,
    const FNFT_UINT mode_offset);

FNFT_INT fnft__poly_fmult_two_polys3x3(const FNFT_UINT deg,
    FNFT_COMPLEX const * const p1_11,
    const FNFT_UINT p1_stride,
    FNFT_COMPLEX const * const p2_11,
    const FNFT_UINT p2_stride,
    FNFT_COMPLEX * const result_11,
    const FNFT_UINT result_stride,
    fnft__fft_wrapper_plan_t plan_fwd,
    fnft__fft_wrapper_plan_t plan_inv,
    FNFT_COMPLEX * const buf0,
    FNFT_COMPLEX * const buf1,
    FNFT_COMPLEX * const buf2,
    const FNFT_UINT mode_offset);

/**
 * @brief Number of elements that the input p to
 * \link fnft__poly_fmult \endlink should have.
 *
 * @ingroup poly
 * Specifies how much memory (in number of elements) the user needs to allocate
 * for the input p of the routine \link fnft__poly_fmult \endlink.
 * @param [in] deg Degree of the polynomials
 * @param [in] n Number of polynomials
 * @return A number m. The input p to \link fnft__poly_fmult \endlink should be
 * a array with m entries.
 */
FNFT_UINT fnft__poly_fmult_numel(const FNFT_UINT deg, const FNFT_UINT n);

/**
 * @brief Fast multiplication of multiple polynomials of same degree.
 *
 * @ingroup poly
 * Fast multiplication of n polynomials of degree d. Their coefficients are
 * stored in the array p and will be overwritten. If W_ptr != NULL, the
 * result has been normalized by a factor 2^W. Upon exit, W has been stored
 * in *W_ptr.
 * @param[in,out] d Upon entry, degree of the input polynomials. Upon exit,
 *  degree of their product.
 * @param[in,out] n Number of polynomials (=1 upon exit).
 * @param[in,out] p Complex valued array with m entries, where m is determined
 *  using \link fnft__poly_fmult_numel \endlink. Upon entry, the first
 *  (*d+1)*n elements of this array contain the coefficients of the
 *  polynomials. Upon exit, the first *d+1 elements contain the result.
 * @param[out] W_ptr Pointer to normalization flag.
 * @return \link FNFT_SUCCESS \endlink or one of the FNFT_EC_... error codes
 *  defined in \link fnft_errwarn.h \endlink.
 */
FNFT_INT fnft__poly_fmult(FNFT_UINT * const d, FNFT_UINT n, FNFT_COMPLEX * const p,
    FNFT_INT * const W_ptr);

/**
 * @brief Number of elements that the inputs p and result to
 * \link fnft__poly_fmult2x2 \endlink should have.
 *
 * @ingroup poly
 * Specifies how much memory (in number of elements) the user needs to allocate
 * for the inputs p and result of the routine
 * \link fnft__poly_fmult2x2 \endlink.
 * @param [in] deg Degree of the polynomials
 * @param [in] n Number of polynomials
 * @return A number m. The inputs p and result to
 * \link fnft__poly_fmult2x2 \endlink should be a array with m entries.
 */
FNFT_UINT fnft__poly_fmult2x2_numel(const FNFT_UINT deg, const FNFT_UINT n);

/* 
 * Specifies how much memory (in number of elements) the user needs to allocate
 * for the inputs p and result of the routine where the dimension of the AKNS system
 * is NxN
 */
FNFT_UINT fnft__poly_fmult3x3_numel(const FNFT_UINT deg, const FNFT_UINT n);

/**
 * @brief Fast multiplication of multiple 2x2 matrix-valued polynomials of the
 *   same degree.
 *
 * @ingroup poly
 * Fast multiplication of n 2x2 matrix-valued polynomials of degree d. Their
 * coefficients are stored in the array p and will be overwritten. If
 * W_ptr != NULL, the result has been normalized by a factor 2^W. Upon exit,
 * W has been stored in *W_ptr.
 * @param[in] d Pointer to a \link FNFT_UINT \endlink containing the degree of
 * the polynomials.
 * @param[in] n Number of 2x2 matrix-valued polynomials.
 * @param[in,out] p Complex valued array which holds the coefficients of
 * the polynomials being multiplied. Should be of length m*(*d+1), where
 * m is obtained using \link fnft__poly_fmult2x2_numel \endlink.
 * WARNING: p is overwritten.
 * @param[out] result Complex valued array that holds the result of the
 * multiplication. Should be of the same size as p.
 * @param[out] W_ptr Pointer to normalization flag.
 * @return \link FNFT_SUCCESS \endlink or one of the FNFT_EC_... error codes
 *  defined in \link fnft_errwarn.h \endlink.
 */
FNFT_INT fnft__poly_fmult2x2(FNFT_UINT *d, FNFT_UINT n, FNFT_COMPLEX * const p,
    FNFT_COMPLEX * const result, FNFT_INT * const W_ptr);

// My code, 3x3 case
FNFT_INT fnft__poly_fmult3x3(FNFT_UINT* d, FNFT_UINT n, FNFT_COMPLEX* const p,
	FNFT_COMPLEX* const result, FNFT_INT* const W_ptr);

#ifdef FNFT_ENABLE_SHORT_NAMES
#define poly_fmult_two_polys_len(...) fnft__poly_fmult_two_polys_len(__VA_ARGS__)
#define poly_fmult_two_polys_lenmen(...) fnft__poly_fmult_two_polys_lenmen(__VA_ARGS__)
#define poly_fmult_two_polys(...) fnft__poly_fmult_two_polys(__VA_ARGS__)
#define poly_fmult_two_polys2x2(...) fnft__poly_fmult_two_polys2x2(__VA_ARGS__)
#define poly_fmult_two_polys3x3(...) fnft__poly_fmult_two_polys3x3(__VA_ARGS__)
#define poly_fmult_numel(...) fnft__poly_fmult_numel(__VA_ARGS__)
#define poly_fmult2x2_numel(...) fnft__poly_fmult2x2_numel(__VA_ARGS__)
#define poly_fmult3x3_numel(...) fnft__poly_fmult3x3_numel(__VA_ARGS__)
#define poly_fmult(...) fnft__poly_fmult(__VA_ARGS__)
#define poly_fmult2x2(...) fnft__poly_fmult2x2(__VA_ARGS__)
#define poly_fmult3x3(...) fnft__poly_fmult3x3(__VA_ARGS__)
#endif

#endif
