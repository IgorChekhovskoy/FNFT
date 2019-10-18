% MEX_FNFT_NSEP Fast nonlinear Fourier transform for the nonlinear
% Schroedinger equation with periodic boundaries.
%
%   [main_spec, aux_spec] = MEX_FNFT_NSEP(q, T, kappa)
%   [main_spec, aux_spec] = MEX_FNFT_NSEP(q, T, kappa, OPTIONAL INPUTS)
%
% DESCRIPTION
%   Provides an interface to the C routine fnft_nsep.
%
% INPUTS
%   q               Complex row vector of length D>2
%   T               Real 1x2 vector
%   kappa           +1.0 or -1.0
%
% OPTIONAL INPUTS
%   It is possible to provide additional inputs. These come either in the
%   form of a single string or of a string followed by a value.
%
%    'loc_subsample_and_refine' Localize spectra by applpying fast eigenmethod
%                   to a subsampled version of the signal, then refine using
%                   the complete signal
%    'loc_gridsearch'  Localize real spectra by using a FFT-based grid search
%    'loc_mixed'    Localize real spectra using grid search and non-real using
%                   subsample and refine, respectively (default)
%    'filt_none'    Do not filter spectra.
%    'filt_manual'  Remove spectra outside a given bounding box. This argument
%                   must be followed by a real 1x4 vector [min_real max_real
%                   min_imag max_imag] that specifies the box.
%   'quiet'         Turns off messages generated by then FNFT C library.
%                   (To turn off the messages generated by the mex
%                   interface functions, use Matlab's warning and error
%                   commands instead.)
%
% OUTPUTS
%   main_spec       Complex row vector
%   aux_spec        Complex row vector

% This file is part of FNFT.  
%                                                                  
% FNFT is free software; you can redistribute it and/or
% modify it under the terms of the version 2 of the GNU General
% Public License as published by the Free Software Foundation.
%
% FNFT is distributed in the hope that it will be useful,
% but WITHOUT ANY WARRANTY; without even the implied warranty of
% MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
% GNU General Public License for more details.
%                                                                      
% You should have received a copy of the GNU General Public License
% along with this program. If not, see <http://www.gnu.org/licenses/>.
%
% Contributors:
% Sander Wahls (TU Delft) 2018.
