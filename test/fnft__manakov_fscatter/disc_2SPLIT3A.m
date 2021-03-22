%% Matlab file to get the polynomial coefficients for the 2SPLIT3A
% also to generate results for the test file
% BO (2nd order base method) with 3rd order splitting (Eq. 19)

%% computing the coefficients in sybmbolic form
syms q1 q2 l kappa h z

A = [-1i*l,0,0;0,1i*l,0;0,0,1i*l];
B = [0,q1,q2;-kappa*conj(q1),0,0;-kappa*conj(q2),0,0];

% Let AE = expm(A*h) and BE = expm(B*h)
% Set z = exp(j lambda h/3) (m=1/3) such that
AE = [1/z^3,0,0;0,z^3,0;0,0,z^3];
BE = sym('BE',[3,3]);
% AE_3 = expm(A*h/3)
AE_3 = [1/z,0,0;0,z,0;0,0,z];
% BE_3 = expm(B*h/3)
BE_3 = sym('BE_3',[3,3]);

% CE=expm((A+B)*h)
% Let CE_approx be approximation of CE after application of splitting
% scheme

CE_approx = 9/8*AE_3*BE_3*BE_3*AE_3*AE_3*BE_3 - ...
            1/8*AE*BE;
CE_approx_pos = expand(CE_approx*z^3);

% We can now look at coefficients of individual polynomials. Here c gives
% the coefficients of the element specified by (i,j) in CE_approx_pos(i,j)
% and t gives which power of z the coefficients belong to
[c11,t11] = coeffs(CE_approx_pos(1,1),z);
[c12,t12] = coeffs(CE_approx_pos(1,2),z);
[c13,t13] = coeffs(CE_approx_pos(1,3),z);

[c21,t21] = coeffs(CE_approx_pos(2,1),z);
[c22,t22] = coeffs(CE_approx_pos(2,2),z);
[c23,t23] = coeffs(CE_approx_pos(2,3),z);

[c31,t31] = coeffs(CE_approx_pos(3,1),z);
[c32,t32] = coeffs(CE_approx_pos(3,2),z);
[c33,t33] = coeffs(CE_approx_pos(3,3),z);

% Organizing all coefficients in corresponding matrices. Don't know if this
% is useful for writing the C code actually, the values cij an tij might be
% enough
% Answer: yes, useful for writing the test file
matz0 = [c11(2),    c12(2),     c13(2);
         0,         0,          0;
         0,         0,          0];
         
    
matz2 = [0,         0,          0;
         c21(2),    c22(2),     c23(2);
         c31(2),    c32(2),     c33(2)];

matz4 = [c11(1),    c12(1),     c13(1);
         0,         0,          0;
         0,         0,          0];

matz6 = [0,         0,          0;
         c21(1),    c22(1),     c23(1);
         c31(1),    c32(1),     c33(1)];

%% Computing values for the test file
eps_t = 0.13;
kappa = 1;
D=8;
q = (0.41*cos(1:2*D)+0.59j*sin(0.28*(1:2*D)))*50;
r = -conj(q);
zlist = exp(1i*[0,pi/4,9*pi/14,4*pi/3,-pi/5]);
result_exact = zeros(45,1);
for i = 1:1:5
    z = zlist(i);
    S = eye(3);
    for n=1:D
        % Eq. 19 in P. J. Prins and S. Wahls, Higher order exponential
        % splittings for the fast non-linear Fourier transform of the KdV
        % equation, to appear in Proc. of IEEE ICASSP 2018, Calgary, April 2018.
        eA_3 = [z^-1 0 0; 0 z 0; 0 0 z];
        B = [0, q(2*n-1), q(2*n); r(2*n-1), 0, 0; r(2*n), 0, 0];
        U = (9/8)*eA_3*expm(B*eps_t*2/3)*eA_3^2*expm(B*eps_t/3) - (1/8)*eA_3^3*expm(B*eps_t);
        S = U*S;
    end
    S=S*z^(D*3);
    result_exact(i) = S(1,1);
    result_exact(5+i) = S(1,2);
    result_exact(10+i) = S(1,3);
    result_exact(15+i) = S(2,1);
    result_exact(20+i) = S(2,2);
    result_exact(25+i) = S(2,3);
    result_exact(30+i) = S(3,1);
    result_exact(35+i) = S(3,2);
    result_exact(40+i) = S(3,3);
end

%%
clear
clc

syms z
% A = [-1i*l,0,0;0,1i*l,0;0,0,1i*l];
% B = [0,q1,q2;-kappa*conj(q1),0,0;-kappa*conj(q2),0,0];

% Let AE = expm(A*h) and BE = expm(B*h)
% Set z = exp(j lambda h/3) (m=1/3) such that
AE = [1/z^3,0,0;0,z^3,0;0,0,z^3];
% BE = sym('BE',[3,3]);
% AE_3 = expm(A*h/3)
AE_3 = [1/z,0,0;0,z,0;0,0,z];
% BE_3 = expm(B*h/3)
% BE_3 = sym('BE_3',[3,3]);

% CE=expm((A+B)*h)
% Let CE_approx be approximation of CE after application of splitting
% scheme
eps_t = 0.13;
kappa = 1;
D=2;
q = (0.41*cos(1:2*D)+0.59j*sin(0.28*(1:2*D)))*50;
r = -kappa*conj(q);
M = eye(3);
for n=1:D
    B = [0, q(2*n-1), q(2*n); r(2*n-1), 0, 0; r(2*n), 0, 0];
    BE = expm(B*eps_t);
    BE_3 = expm(B*eps_t/3);    
    CE_approx = 9/8*AE_3*BE_3*BE_3*AE_3*AE_3*BE_3 - ...
        1/8*AE*BE;
    CE_approx_pos = expand(CE_approx*z^3);
    M = CE_approx_pos*M;
end
[p1e,t] = coeffs(expand(M(2,2)),'all');
p1e = double(p1e).';

%%
c = [0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 5.348614926900e-01+-2.051002316742e-17j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 1.618752968735e-01+-1.387778780781e-17j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, -8.648897251377e-01+1.908195823574e-17j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, -1.124978063774e-01+2.775557561563e-17j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, -6.762134363323e-02+7.319277769553e-02j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 4.127730097208e-01+-4.467820589764e-01j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 2.875836223254e-01+2.116722027857e-01j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, -1.836144730718e-01+-1.351470562342e-01j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, -4.464706556046e-02+8.847305481107e-02j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 2.725338278776e-01+-5.400556562678e-01j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, -2.215000998028e-01+4.068574432528e-01j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 1.414219063721e-01+-2.597676267312e-01j, -4.127730097208e-01+-4.467820589764e-01j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 6.762134363323e-02+7.319277769553e-02j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 1.836144730718e-01+-1.351470562342e-01j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, -2.875836223254e-01+2.116722027857e-01j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 5.786252044610e-01+-3.961135733310e-17j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 2.689064661657e-01+-6.632235359605e-18j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 5.853492173672e-01+4.576097438164e-18j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, -3.223621649981e-01+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, -4.029092809182e-01+1.152042466691e-01j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 2.571224289257e-01+-7.351926880067e-02j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, -7.291006854600e-02+-5.329605393069e-01j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, -5.668251099736e-02+-4.143397782072e-01j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, -2.725338278776e-01+-5.400556562678e-01j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 4.464706556046e-02+8.847305481107e-02j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, -1.414219063721e-01+-2.597676267312e-01j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 2.215000998028e-01+4.068574432528e-01j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, -4.029092809182e-01+-1.152042466691e-01j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 2.571224289257e-01+7.351926880067e-02j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, -7.291006854600e-02+5.329605393069e-01j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, -5.668251099736e-02+4.143397782072e-01j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 5.832500924125e-01+1.730848289989e-17j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 2.659550265243e-01+-1.387778780781e-17j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 3.021529762554e-01+-1.513393378735e-17j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, -5.425275601396e-01+1.908195823574e-17j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j];
% c is the result p after manakov fscatter
n = 7;
p1 = cell(3,3);
p2 = cell(3,3);
% p1,2 are the two matrices with polynomial coefs in each element of the
% cell
idx = 1:n;
for i=1:3
    for j=1:3
        nc = (i-1)*3+j-1;
        p1{i,j} = c(idx);
        p2{i,j} = c(idx+n);
        idx = idx+2*n;
    end
end

len = 15;
res = cell(3,3);
for i=1:3
    for j=1:3
        res{i,j} = ifft(fft(p1{i,1},len).*fft(p2{1,j},len) + fft(p1{i,2},len).*fft(p2{2,j},len) + fft(p1{i,3},len).*fft(p2{3,j},len));
    end
end
% multiplication: res are the polynomial coefs of the result (should
% contain the values of p in poly_eval). These agree, but, res(14), res(15)
% are 0 while in poly_eval p[13], p[14] are the values of the next p. This
% should not matter as long as the degree is correct. (in matlab len = 15,
% should be 13 I think because we multiply 2 6th degree polynomials)
%%
r = [-2.420634573238e-02+6.367861441813e-02j, -2.982001361792e-01+1.055199885831e-02j, 3.561735781980e-01+-1.009099771959e-01j, -2.101547933915e-01+1.455512170868e-01j, -1.253842995204e-01+4.657360835193e-02j, 1.947709819686e-01+-2.066900663200e-01j, -1.094629644470e-01+2.410096783337e-01j, 5.245820180370e-02+-1.074346285630e-01j, 1.247466447706e-01+-1.440376480313e-01j, 1.415944907801e-01+3.302551152075e-01j, -1.366088516963e-01+-1.606787101502e-01j, 7.464627537188e-02+-3.856541120764e-02j, 1.009162721608e-01+1.256209382029e-01j, 1.025215283243e-01+-9.736590768831e-02j, -4.140880350442e-01+-5.953215746252e-02j, 5.701739836395e-01+2.620895648681e-01j, -2.598385732499e-01+-2.413213603469e-01j, -9.106458360558e-02+1.103806042865e-01j, 3.986312245320e-01+-3.211700217543e-01j, -3.505424254562e-01+-8.709313105230e-02j, 2.033870698348e-01+2.702381404006e-02j, -3.113293884014e-02+-1.972700603180e-01j, -1.385838330632e-01+6.198120849431e-01j, 1.560296088494e-01+-2.927927712506e-01j, -1.140407235061e-01+8.900915163078e-02j, 2.445462190372e-01+2.409783623785e-01j, -4.024806059830e-01+-3.041413490312e-01j, 3.619630467516e-01+1.301302051456e-01j, 1.117728909453e-01+-4.718059696588e-02j, -4.743079401970e-01+-2.692502381236e-02j, 4.479508465554e-01+-1.295078836900e-01j, -3.201618505140e-01+1.223014462016e-01j, -3.769400366836e-01+2.620098258938e-02j, 3.343854763355e-01+-3.164267266398e-01j, -1.967225344758e-01+4.571387509961e-01j, 1.296626054740e-01+-1.621007360600e-01j, -1.880327995087e-01+-4.545057051885e-01j, 6.059022802259e-02+4.897777321946e-01j, -1.525698607476e-01+-4.250989685478e-01j, 3.772857432952e-02+-1.831445485142e-02j, 2.778726928797e-01+-6.162523921999e-02j, 1.477749478908e-01+-1.247193564773e-01j, -4.196020571480e-03+-2.642462655215e-01j, -1.147422857994e-01+2.383006586783e-01j, 2.187033833375e-01+-9.320696078205e-02j, 1.890224156256e-01+-1.031231945035e-01j, -6.334805719083e-02+-1.810619499201e-01j, 9.972925441070e-02+1.857652343272e-01j, 1.806038073859e-01+-1.546958706585e-01j, 1.685438904257e-01+-2.012765565498e-01j, -1.481632224358e-01+-6.985004541402e-02j, 2.343049953696e-01+7.118282725994e-02j, 4.620811422076e-01+-1.529411822051e-01j, 9.705915192236e-02+-2.824112631177e-01j, 2.718321885868e-01+-2.728648967159e-03j, 4.007265744783e-01+-9.955947743761e-02j, 2.469622691624e-01+6.232945833306e-02j, 4.337125105449e-01+-6.376350071083e-02j, 3.735746915225e-01+4.807867604102e-02j, 3.520514291115e-01+-1.246960930983e-01j, 2.423600118795e-01+1.001980840879e-01j, 3.151208596198e-01+-5.625763296818e-02j, 3.653194678627e-01+7.561128638181e-02j, 2.236027722441e-01+-4.331115310433e-02j, 3.765967937579e-01+1.039091759764e-01j, 3.006677018340e-01+-2.776617403883e-01j, 4.765459618009e-01+7.877407322334e-02j, 6.873944175164e-02+7.360633682176e-02j, 4.094930463998e-01+-2.663811093815e-01j, -3.778696245028e-01+-9.506749359709e-01j, -2.382874000091e-01+9.362671824106e-02j, 1.172268608162e-01+4.981555306899e-02j, 3.238728366296e-01+-5.419147128166e-01j, -4.783228928580e-01+-5.827528062140e-01j, -1.104675696381e-01+-2.241520566787e-02j, 2.462616541796e-01+-1.211059125055e-01j, 1.653636715924e-01+-7.995386784824e-01j, -4.396096934598e-01+-3.021120488416e-01j, -5.799144421570e-02+-1.626258756621e-02j, -1.152002756942e-01+-2.577660944782e-01j, -3.779890340043e-01+1.721491866655e-01j, 3.276815876770e-02+1.067876768943e-01j, 1.641556196000e-01+-7.487186712246e-02j, -2.715401250079e-01+-1.442061597503e-01j, -2.043896128532e-01+3.621974150719e-01j, 9.911743845669e-02+8.328151746824e-02j, -2.117193167118e-02+-2.239099002084e-02j, -3.179071675901e-01+-9.739529167293e-02j, -2.866639123457e-02+2.936640921075e-01j, 5.094141820374e-02+-4.534812000358e-03j, 1.022312520396e-02+-1.555341955405e-01j, -2.198839065623e-01+-4.536556854466e-02j, -3.611888663001e-02+-3.592819859446e-01j, -5.722952161269e-01+-1.235639166271e-01j, -1.461935630460e-01+1.360381763045e-01j, 2.237097398200e-01+-2.005353840338e-02j, -2.897182082491e-01+-4.115874133544e-01j, -5.836491932456e-01+1.307073435874e-01j, -7.036502753940e-02+1.439353005416e-01j, -6.193942449478e-02+-1.532266169488e-01j, -4.004614775788e-01+-4.033127143080e-01j, -4.337684142103e-01+1.872141193985e-01j, -8.775981689067e-02+2.627049548188e-02j, -6.486765309996e-02+-2.226755307899e-01j, -5.261847270272e-02+3.666402921634e-01j, 6.823849496835e-03+3.359087752829e-01j, -6.463478494120e-02+8.421654693682e-02j, -1.073897186298e-02+3.273949008117e-01j, -9.955544241716e-02+1.985299018333e-01j, 8.504650769074e-02+2.805697699650e-01j, -1.470561597401e-01+1.460535018744e-01j, 5.033560522617e-02+2.679855286177e-01j, -1.077840494518e-01+1.323095254449e-01j, 1.000449021539e-01+1.815162722191e-01j, -1.605207315705e-01+2.453271265570e-01j, 4.416278780639e-02+1.874406143566e-01j, -8.177034272185e-02+3.172008159595e-01j, 3.834111150151e-02+9.899515832194e-02j, -1.010187192087e-01+3.204317222178e-01j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+1.004435457995e-320j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 5.348614926900e-01+-2.051002316742e-17j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 0.000000000000e+00+0.000000000000e+00j, 1.618752968735e-01+-1.387778780781e-17j, 0.000000000000e+00+0.000000000000e+00j];
% not sure what this is. Should be the same as res looking at the print
% statement, but is not the same
n = 15;
re = cell(3,3);
idx = 1:n;
for i=1:3
    for j=1:3
        re{i,j} = r(idx);
        idx = idx+n;
    end
end

%% Evaluating for different z:
zlist = exp(1i*[0,pi/4,9*pi/14,4*pi/3,-pi/5]);
result = zeros(45,1);

for k =1:5
    z=zlist(k);
    for i = 1:3
        for j = 1:3
            result((((i-1)*3+j)-1)*5+k) = polyval(res{i,j}(1:13),z);
        end
    end
end
% These should be the same as result from manakov_fscatter_test_2split3A
% for D=2, and they are. In check_p, the values tmp evaluated using the
% polynomial coefs and horners method are also the same as the
% (intermediate) values from poly_eval. So it looks like the matlab test
% file does not give the right output
            
            