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
* Shrinivas Chimmalgi (TU Delft) 2017.
*/

#define FNFT_ENABLE_SHORT_NAMES

#include <stdio.h>
#include "fnft__nse_fscatter.h"
#include "fnft__misc.h"
#include "fnft__errwarn.h"

static INT nse_fscatter_test_defocusing_2split4A()
{
    UINT i, D = 8, deg;
    INT W;
    REAL scl;
    INT ret_code;
    const REAL eps_t = 0.13;
    COMPLEX q[8];
    COMPLEX result[5*4*8];
    COMPLEX result_exact[132] = {

           1.00511854809713 +  5.49121598887962e-20*I,
                          0 +                     0*I,
          0.020916784133482 -  0.000933221827759738*I,
        -0.0106575270196812 +  0.000933221827759738*I,
         0.0241428351838715 -   0.00210747824126655*I,
        -0.0108099550395217 +  0.000939576813000935*I,
         0.0171938859130026 -   0.00360520732236721*I,
       -0.00646287533397823 +   0.00265918095002184*I,
         0.0143288649999496 -   0.00600018037927714*I,
       -0.00649308001080602 +   0.00266763267952936*I,
         0.0100890193081556 -   0.00650053077615352*I,
       -0.00379028147190939 +   0.00381341521502935*I,
        0.00821279683000393 -   0.00859424301559564*I,
       -0.00375645357975221 +   0.00380696786164919*I,
          0.006819187096043 -   0.00720883568653109*I,
       -0.00323565124310324 +   0.00336420271304428*I,
        0.00702151356003853 -   0.00759146180360678*I,
        -0.0031877273893234 +   0.00333965597859051*I,
        0.00612167782222402 -   0.00499116381443564*I,
       -0.00303803037550326 +   0.00159640841108741*I,
        0.00666753071582355 -    0.0036437392731417*I,
       -0.00298174557431609 +   0.00158062308289114*I,
        0.00476343302174223 -   0.00150102193852257*I,
       -0.00181628800526356 -  0.000129386077279436*I,
        0.00401900299052701 +  0.000226628054994435*I,
       -0.00177752082478855 -  0.000116428868458206*I,
        0.00206255999148961 +  0.000732907229546443*I,
      -0.000287616285609635 -  0.000634987633717622*I,
       0.000638701521973765 +   0.00139832970934004*I,
      -0.000281787316540534 -  0.000615972141257369*I,
       0.000281787316540534 +  0.000615972141257369*I,
                          0 +                     0*I,
                          0 +                     0*I,
                          0 +                     0*I,
         0.0188263526588833 +    0.0128714230163545*I,
       -0.00941317632944167 -   0.00643571150817724*I,
         0.0192246831316494 +     0.013113665498269*I,
      -0.000418201599449768 -  0.000227459470917923*I,
        -0.0134965506486701 +    0.0251295456776579*I,
        0.00656113975582259 -    0.0126312439899999*I,
        -0.0137542552590979 +    0.0255250319822036*I,
      -0.000378059946618653 -  0.000695759755981326*I,
        -0.0341239432910854 +    0.0354971122977432*I,
         0.0169802924195902 -    0.0179045630623187*I,
        -0.0345516118571822 +    0.0359210516183067*I,
       0.000109337456615969 -   0.00122623183043565*I,
        -0.0232128216732282 +    0.0428180969231779*I,
         0.0115542179176526 -    0.0216241783859959*I,
        -0.0231131259495532 +    0.0431117260136925*I,
       0.000189330975734071 -    0.0016060805544665*I,
        0.00957864148961799 +    0.0461748052829475*I,
         -0.004808285785237 -    0.0232923988970671*I,
         0.0099335452769161 +    0.0462221871060843*I,
      -1.32809094433088e-05 -   0.00166601352245547*I,
         0.0335510538907796 +    0.0450808480362089*I,
        -0.0166433654484399 -    0.0226479423503672*I,
         0.0335341087570007 +    0.0448102532257058*I,
       5.77418973129342e-05 -   0.00130254108455319*I,
         0.0261319525231986 +    0.0394872041559111*I,
        -0.0128069984231313 -    0.0196607540083952*I,
         0.0258460785360847 +    0.0389348905546424*I,
       0.000171877206415384 -  0.000563957242627569*I,
        -0.0052050813373691 +    0.0299996613893542*I,
        0.00253490951184551 -    0.0147099453902255*I,
       -0.00506981902369101 +    0.0294198907804509*I,
                          0 +                     0*I,
                          0 +                     0*I,
       -0.00506981902369101 -    0.0294198907804509*I,
        0.00253490951184551 +    0.0147099453902255*I,
        -0.0052050813373691 -    0.0299996613893542*I,
       0.000171877206415384 +  0.000563957242627569*I,
         0.0258460785360848 -    0.0389348905546424*I,
        -0.0128069984231313 +    0.0196607540083952*I,
         0.0261319525231986 -    0.0394872041559111*I,
       5.77418973129342e-05 +   0.00130254108455319*I,
         0.0335341087570007 -    0.0448102532257058*I,
        -0.0166433654484399 +    0.0226479423503672*I,
         0.0335510538907796 -    0.0450808480362089*I,
      -1.32809094433088e-05 +   0.00166601352245547*I,
        0.00993354527691609 -    0.0462221871060843*I,
         -0.004808285785237 +    0.0232923988970671*I,
        0.00957864148961799 -    0.0461748052829475*I,
       0.000189330975734071 +    0.0016060805544665*I,
        -0.0231131259495532 -    0.0431117260136925*I,
         0.0115542179176526 +    0.0216241783859959*I,
        -0.0232128216732282 -    0.0428180969231779*I,
       0.000109337456615969 +   0.00122623183043565*I,
        -0.0345516118571822 -    0.0359210516183067*I,
         0.0169802924195902 +    0.0179045630623187*I,
        -0.0341239432910854 -    0.0354971122977431*I,
      -0.000378059946618653 +  0.000695759755981326*I,
        -0.0137542552590979 -    0.0255250319822036*I,
        0.00656113975582259 +    0.0126312439899999*I,
        -0.0134965506486701 -    0.0251295456776579*I,
      -0.000418201599449768 +  0.000227459470917923*I,
         0.0192246831316494 -     0.013113665498269*I,
       -0.00941317632944167 +   0.00643571150817724*I,
         0.0188263526588833 -    0.0128714230163545*I,
                          0 +                     0*I,
                          0 +                     0*I,
                          0 +                     0*I,
       0.000281787316540534 -  0.000615972141257369*I,
      -0.000281787316540534 +  0.000615972141257369*I,
       0.000638701521973765 -   0.00139832970934004*I,
      -0.000287616285609635 +  0.000634987633717622*I,
        0.00206255999148961 -  0.000732907229546443*I,
       -0.00177752082478855 +  0.000116428868458206*I,
        0.00401900299052701 -  0.000226628054994435*I,
       -0.00181628800526356 +  0.000129386077279436*I,
        0.00476343302174223 +   0.00150102193852257*I,
       -0.00298174557431609 -   0.00158062308289114*I,
        0.00666753071582355 +    0.0036437392731417*I,
       -0.00303803037550326 -   0.00159640841108741*I,
        0.00612167782222403 +   0.00499116381443564*I,
        -0.0031877273893234 -   0.00333965597859052*I,
        0.00702151356003853 +   0.00759146180360678*I,
       -0.00323565124310324 -   0.00336420271304428*I,
          0.006819187096043 +   0.00720883568653109*I,
       -0.00375645357975221 -   0.00380696786164919*I,
        0.00821279683000393 +   0.00859424301559564*I,
       -0.00379028147190939 -   0.00381341521502935*I,
         0.0100890193081556 +   0.00650053077615352*I,
       -0.00649308001080602 -   0.00266763267952936*I,
         0.0143288649999496 +   0.00600018037927714*I,
       -0.00646287533397823 -   0.00265918095002184*I,
         0.0171938859130026 +   0.00360520732236722*I,
        -0.0108099550395217 -  0.000939576813000935*I,
         0.0241428351838715 +   0.00210747824126655*I,
        -0.0106575270196812 -  0.000933221827759738*I,
          0.020916784133482 +  0.000933221827759738*I,
                          0 +                     0*I,
           1.00511854809713 +  5.49121598887962e-20*I,};

    /* Matlab code to generate result_exact:
    e = 0.13;
    kappa = -1; D=8; q = 0.4*cos(1:D)+0.5j*sin(0.3*(1:D));
    r=-kappa*conj(q);
    ms=5;% 4th degree polynomial scheme
    S=zeros(2,2*ms,D);
    for n=1:D
        if q(n) == 0
            B11 = 1; B12 = 0; B13 = 0; B14 = 1;
        else
            B11=cosh(0.5*e*q(n)^(1/2)*r(n)^(1/2));
            B12= (sinh(0.5*e*(q(n)*r(n))^(1/2))*(q(n)*r(n))^(1/2))/r(n);
            B13 = -kappa * conj(B12);
            B14=(B11);
        end
        % Scattering matrix at n
        S(:,:,n)=[[B11^2-(B12*B13)/3,0,(4*B12*B13)/3,0,0],[0,(4*B11*B12)/3 ,-(B12*B14)/3-(B11*B12)/3,(4*B12*B14)/3,0];...
                 [0,(4*B11*B13)/3,-(B13*B14)/3-(B11*B13)/3,(4*B13*B14)/3,0],[0,0,(4*B12*B13)/3,0, B14^2-(B12*B13)/3]];
    end
    % Multiply scattering matrices
    temp=S(:,:,D);
    m=ms;
    for n=D-1:-1:1
        temp=[conv(temp(1,1:m),S(1,1:ms,n))+conv(temp(1,m+1:end),S(2,1:ms,n)),...
            conv(temp(1,1:m),S(1,ms+1:end,n))+conv(temp(1,m+1:end),S(2,ms+1:end,n));...
            conv(temp(2,1:m),S(1,1:ms,n))+conv(temp(2,m+1:end),S(2,1:ms,n)),...
            conv(temp(2,1:m),S(1,ms+1:end,n))+conv(temp(2,m+1:end),S(2,ms+1:end,n))];
         m=length(temp)/2;
    end
    format long g; result_exact = [temp(1,1:m) temp(1,m+1:end) temp(2,1:m) temp(2,m+1:end)].'
    */
 
    for (i=0; i<D; i++)
        q[i] = 0.4*COS(i+1) + 0.5*I*SIN(0.3*(i+1));

    // without normalization
    ret_code = nse_fscatter(D, q, eps_t, -1, result, &deg, NULL, nse_discretization_2SPLIT4A);
    if (ret_code != SUCCESS)
        return E_SUBROUTINE(ret_code);
    if (misc_rel_err(4*(deg+1), result, result_exact) > 100*EPSILON)
        return E_TEST_FAILED;

    // with normalization
    ret_code = nse_fscatter(D, q, eps_t, -1, result, &deg, &W, nse_discretization_2SPLIT4A);
    if (ret_code != SUCCESS)
        return E_SUBROUTINE(ret_code);
    if (W != 0) // it really should be zero in this case!
        return E_TEST_FAILED;
    scl = POW(2.0, W);
    for (i=0; i<4*(deg+1); i++)
        result[i] *= scl;
    if (misc_rel_err(4*(deg+1), result, result_exact) > 100*EPSILON)
        return E_TEST_FAILED;

    return SUCCESS;
}

INT main()
{
    if (nse_fscatter_test_defocusing_2split4A() != SUCCESS)
        return EXIT_FAILURE;

    return EXIT_SUCCESS;
}