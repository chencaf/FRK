#include <TMB.hpp>
#include <cmath>
#include "FRK-init.h"

template<class Type>
Type objective_function<Type>::operator() ()
{

  // typedef's:
  typedef Eigen::SparseMatrix<Type> SpMat;    // Sparse matrices called 'SpMat'
  typedef Eigen::Triplet<Type> T;             // Triplet lists called 'T'

  // Data
  DATA_VECTOR(Z);             // Vector of observations
  DATA_MATRIX(X);             // Design matrix of fixed effects
  DATA_SPARSE_MATRIX(S);      // Design matrix for basis function random weights
  DATA_IVECTOR(ri);           // Number of basis functions at each resolution
  DATA_SCALAR(sigma2e);       // Measurement error estimate (Gaussian case only)
  DATA_STRING(K_type);        // Indicates the desired model formulation of eta prior (K or Q)
  DATA_STRING(response);      // String specifying the response distribution
  DATA_STRING(link);          // String specifying the link function
  DATA_VECTOR(k_Z);           // "Known-constant" parameter (only relevant for negative-binomial and binomial)
  
  DATA_VECTOR(alpha);         // Tapering parameters (only relevant for block-exponential formulation)
  DATA_IVECTOR(row_indices);
  DATA_IVECTOR(col_indices);
  DATA_VECTOR(x);
  DATA_IVECTOR(nnz);          // Integer vector indicating the number of non-zeros at each resolution of the K_tap or Q

  int m    = Z.size();       // Sample size
  int nres = ri.size();      // Number of resolutions
  int r    = ri.sum();
  int nnz_total = nnz.sum(); // Total number of non-zero elements in Q or K_tap

  // Parameters                   // Transformed Parameters
  PARAMETER_VECTOR(beta);
  PARAMETER(logsigma2xi);         Type sigma2xi       = exp(logsigma2xi);
  PARAMETER(logphi);              Type phi            = exp(logphi);

  
  PARAMETER_VECTOR(logsigma2);    vector<Type> sigma2 = exp(logsigma2);
  PARAMETER_VECTOR(logtau);       vector<Type> tau    = exp(logtau);
  vector<Type> rho    = tau;
  vector<Type> kappa  = sigma2;

  // Latent random effects
  PARAMETER_VECTOR(eta);
  PARAMETER_VECTOR(xi_O);


  // ---- 1. Construct prior covariance matrix K or precision matrix Q  ---- //

  // NOTE: we cannot declare objects inside if() statements if those objects are also 
  // used outside the if() statement. We ARE allowed to declare objects inside if() statements
  // if they are not used outside of this if() statement at any other point in the template.
  
  // Construct Q or K as a sparse matrix: use triplet list (row, column, value)
  std::vector<T> tripletList;       // Create triplet list, called 'tripletList'
  tripletList.reserve(nnz_total);   // Reserve number of non-zeros in the matrix

  int start = 0;
  Type coef = 0;        // Variable to store the current element (coefficient) of the matrix
  
  // ---- Precision
  
  if (K_type == "precision") {
    
    // Add kappa[i] diagonals of block i, multiply block i by rho[i]
    for (int i = 0; i < nres; i++) {                    // For each resolution
      for (int j = start; j < start + nnz[i]; j++){     // For each non-zero entry within resolution i
        
        if (row_indices[j] == col_indices[j]) { // Diagonal terms
          coef = rho[i] * (x[j] + kappa[i]);
        } else {
          coef = rho[i] * x[j];
        }
        tripletList.push_back(T(row_indices[j], col_indices[j], coef));
      }
      start += nnz[i];
    }
    
  }
  
  // Convert triplet list of non-zero entries to a true SparseMatrix.
  SpMat Q(r, r);
  Q.setFromTriplets(tripletList.begin(), tripletList.end());

  // ----- K Matrix 

  if (K_type == "block-exponential") {
    
    for (int i = 0; i < nres; i++){         // For each resolution
      for (int j = start; j < start + nnz[i]; j++){     // For each non-zero entry within resolution i
        
        // Construct the covariance (exponential covariance function WITH spherical taper):
        coef = sigma2[i] * exp( - x[j] / tau[i] ) * pow( 1 - x[j] / alpha[i], 2.0) * ( 1 + x[j] / (2 * alpha[i]));
        
        // Add the current element to the triplet list:
        tripletList.push_back(T(row_indices[j], col_indices[j], coef));
        
      }
      start += nnz[i];
    }
    
  }
  
  
  //  Convert triplet list of non-zero entries to a true SparseMatrix.
  SpMat K(r, r);
  K.setFromTriplets(tripletList.begin(), tripletList.end());
  

  // -------- 2. log-determinant of Q, and the 'u' vector -------- //

  // Compute the Cholesky factorisation of Q (or K):
  Eigen::SimplicialLLT< SpMat, Eigen::Lower, Eigen::NaturalOrdering<int> > llt;
  
  // Quadratic forms required in Gaussian density
  Type quadform_xi = pow(sigma2xi, -1.0) * (xi_O * xi_O).sum();
  Type quadform_eta = 0; 
  
  // Log determinant of the prior covariance matrix of the eta random weights
  Type logdetK = 0;
  
  if (K_type == "precision") {
    llt.compute(Q);
    
    // Cholesky factor M (such that MM' = Q, and M is lower triangular):
    SpMat M = llt.matrixL();
    
    // log-determinant of K (K is the inverse of Q):
    logdetK = - 2.0 * M.diagonal().array().log().sum();
    
    // The vector u such that u = M'eta:
    vector<Type> u(r);
    u   = (M.transpose()) * (eta.matrix());
    
    quadform_eta = (u * u).sum();
  } 
  
  if (K_type == "block-exponential") {
    llt.compute(K);
    
    // Cholesky factor L (such that LL' = K, and L is lower triangular):
    SpMat L = llt.matrixL();
    
    // log-determinant of K:
    logdetK = 2.0 * L.diagonal().array().log().sum();
    
    // The vector v (such that Lv = eta):
    vector<Type> v(r); 
    v   = (L.template triangularView<Eigen::Lower>().solve(eta.matrix())).array();
    
    quadform_eta = (v * v).sum();
  }




  // -------- 3. Construct ln[eta|Q] and ln[xi|sigma2xi].  -------- //

  // ln[eta|K] and ln[xi|sigma2xi]:
  // (These quantities are invariant to the link function/response distribution)
  Type ld_eta =  -0.5 * r * log(2 * M_PI) - 0.5 * logdetK - 0.5 * quadform_eta;
  Type ld_xi  =  -0.5 * m * log(2 * M_PI) - 0.5 * m * log(sigma2xi) - 0.5 * quadform_xi;


  // -------- 4. Construct ln[Z|Y_O]  -------- //

  // 4.1. Construct Y_O, the latent spatial process at observed locations
  vector<Type> Y_O  = X * beta + S * eta + xi_O;

  // 4.2. Link the conditional mean mu_O to the Gaussian scale predictor Y_O

  vector<Type> mu_O(m);
  vector<Type> p_O(m);

  if (link == "identity") {
    mu_O = Y_O;
  } else if (link == "inverse") {
    mu_O = 1 / Y_O;
  } else if (link == "inverse-squared") {
    mu_O = 1 / sqrt(Y_O);
  } else if (link == "log") {
    mu_O = exp(Y_O);
  } else if (link == "square-root"){
    mu_O = Y_O * Y_O;
  } else if (link == "logit") {
    p_O = 1 / (1 + exp(-1 * Y_O));
  } else if (link == "probit") {
    p_O = pnorm(Y_O);
  } else if (link == "cloglog") {
    p_O = 1 - exp(-exp(Y_O));
  } else {
    error("Unknown link function");
  }

  Type epsilon_1 = 10e-8;
  Type epsilon_2 = 2 * (1 - 1/(1 + epsilon_1));

  if (link == "logit" || link == "probit" || link == "cloglog") {
    if (response == "bernoulli") {
      mu_O = p_O;
    } else if (response == "binomial")  {
      mu_O = k_Z * p_O;
    } else if (response == "negative-binomial") {
      mu_O = k_Z * (1 / (p_O + epsilon_1) - 1 + epsilon_2);
    }
  } else if (response == "negative-binomial" && (link == "log" || link == "square-root")) {
    mu_O *= k_Z;
  } 
  
  // 4.3. Create the canonical parameter lambda,
  //      a function of the conditional mean mu_O.
  //      Also create vectors containing a(phi), b(lambda), and c(Z, phi).
  vector<Type> lambda(m);
  vector<Type> blambda(m);
  vector<Type> aphi(m);
  vector<Type> cZphi(m);

  if (response == "gaussian") {
    phi     =   sigma2e;
    lambda  =   mu_O;
    aphi    =   phi;
    blambda =   (lambda * lambda) / 2;
    cZphi   =   -0.5 * (Z * Z / phi + log(2 * M_PI * phi));
  } else if (response == "poisson") {
    phi     =   1;
    lambda  =   log(mu_O);
    blambda =   exp(lambda);
    aphi    =   1;
    // Only need c(Z, phi) here to provide exact density function to the user
    // cZphi   =   -lfactorial(Z);           
    for (int i = 0; i < m; i++) {
      cZphi[i] = -lfactorial(Z[i]);
    }
    // cZphi   = 0;
  } else if (response == "bernoulli") {
    phi     =   1;
    lambda  =   log((mu_O + 1e-10) / (1 - mu_O + 1e-10));
    blambda =   log(1 + exp(lambda));
    aphi    =   1;
    cZphi   =   0;    // c(Z, phi) is indeed equal to zero for bernoulli 
  } else if (response == "gamma") {
    lambda  =   1 / mu_O;
    blambda =   log(lambda);
    aphi    =   -phi;
    cZphi   =   log(Z/phi)/phi - log(Z) - lgamma(1/phi);
  } else if (response == "inverse-gaussian") {
    lambda  =   1 / (mu_O * mu_O);
    blambda =   2 * sqrt(lambda);
    aphi    =   - 2 * phi;
    cZphi   =   - 0.5 / (phi * Z) - 0.5 * log(2 * M_PI * phi * Z * Z * Z);
  } else if (response == "negative-binomial") {
    phi     = 1;
    lambda  = log(mu_O / (mu_O + k_Z));
    blambda = -k_Z * log(1 - exp(lambda));
    aphi    = 1;
    // Only need c(Z, phi) here to provide exact density function to the user
    // cZphi   = lfactorial(Z + k_Z - 1) - lfactorial(Z) - lfactorial(k_Z - 1);  
    for (int i = 0; i < m; i++) {
      cZphi[i] = lfactorial(Z[i] + k_Z[i] - 1) - lfactorial(Z[i]) - lfactorial(k_Z[i] - 1);
    }
    // cZphi   = 0;
  } else if (response == "binomial") {
    phi     = 1;
    lambda  = log((mu_O + 1e-10) / (k_Z - mu_O + 1e-10));
    blambda = k_Z * log(1 + exp(lambda));
    aphi    = 1;
    // Only need c(Z, phi) here to provide exact density function to the user
    // cZphi   = lfactorial(k_Z) - lfactorial(Z) - lfactorial(k_Z - Z);     
    for (int i = 0; i < m; i++) {
      cZphi[i] = lfactorial(k_Z[i]) - lfactorial(Z[i]) - lfactorial(k_Z[i] - Z[i]);  
    }
    // cZphi   = 0;
  } else {
    error("Unknown response distribution");
  }

  // 4.4. Construct ln[Z|Y_O]
  Type ld_Z  =  ((Z*lambda - blambda)/aphi).sum() + cZphi.sum();


  // -------- 5. Define Objective function -------- //

  // ln[Z, eta, xi | ...] =  ln[Z|Y_O] + ln[eta|K] + ln[xi|sigma2xi]
  // Specify the NEGATIVE joint log-likelihood function,
  // as R optimisation routines minimise by default.

  Type nld = -(ld_Z  + ld_xi + ld_eta);

  return nld;
}