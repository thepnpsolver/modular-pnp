/*! \file lin_pnp.cpp
 *
 *  \brief Setup and solve the linearized PNP equation using FASP
 *
 *  \note Currently initializes the problem based on specification
 */
#include <iostream>
#include <fstream>
#include <iostream>
#include <string>
#include <dolfin.h>
#include "EAFE.h"
#include "funcspace_to_vecspace.h"
#include "fasp_to_fenics.h"
#include "boundary_conditions.h"
#include "pnp.h"
#include "newton.h"
#include "newton_functs.h"
#include "L2Error.h"
extern "C"
{
#include "fasp.h"
#include "fasp_functs.h"
#define FASP_BSR     ON  /** use BSR format in fasp */
}
using namespace dolfin;
// using namespace std;

double lower_cation_val = 0.1;  // 1 / m^3
double upper_cation_val = 1.0;  // 1 / m^3
double lower_anion_val = 1.0;  // 1 / m^3
double upper_anion_val = 0.1;  // 1 / m^3
double lower_potential_val = -1.0;  // V
double upper_potential_val = 1.0;  // V

class analyticCationExpression : public Expression
{
  void eval(Array<double>& values, const Array<double>& x) const
  {
    values[0]  = -9.0/20.0*x[0]+11.0/20.0;
    values[0]  = std::log(values[0]);
  }
};

class analyticAnionExpression : public Expression
{
  void eval(Array<double>& values, const Array<double>& x) const
  {
    values[0]  = 9.0/20.0*x[0]+11.0/20.0;
    values[0]  = std::log(values[0]);
  }
};

class analyticPotentialExpression : public Expression
{
  void eval(Array<double>& values, const Array<double>& x) const
  {
    values[0]  =-x[0];
  }
};


int main()
{

  printf("\n-----------------------------------------------------------    "); fflush(stdout);
  printf("\n Solving the linearized Poisson-Nernst-Planck system           "); fflush(stdout);
  printf("\n of a single cation and anion                                  "); fflush(stdout);
  printf("\n-----------------------------------------------------------\n\n"); fflush(stdout);

  // Need to use Eigen for linear algebra
  parameters["linear_algebra_backend"] = "Eigen";
  parameters["allow_extrapolation"] = true;

  //*************************************************************
  //  Initialization
  //*************************************************************
  printf("Initialize the problem\n"); fflush(stdout);
  // read domain parameters
  printf("\tdomain...\n"); fflush(stdout);
  domain_param domain_par;
  char domain_param_filename[] = "./benchmarks/PNP/domain_params2.dat";
  domain_param_input(domain_param_filename, &domain_par);
  // print_domain_param(&domain_par);

  // build mesh
  printf("\tmesh...\n"); fflush(stdout);
  dolfin::Mesh mesh;
  dolfin::MeshFunction<std::size_t> subdomains;
  dolfin::MeshFunction<std::size_t> surfaces;
  dolfin::File meshOut(domain_par.mesh_output);
  domain_build(&domain_par, &mesh, &subdomains, &surfaces, &meshOut);

  // read coefficients and boundary values
  printf("\tcoefficients...\n"); fflush(stdout);
  coeff_param coeff_par;
  char coeff_param_filename[] = "./benchmarks/PNP/coeff_params2.dat";
  coeff_param_input(coeff_param_filename, &coeff_par);
  // print_coeff_param(&coeff_par);

  // open files for outputting solutions
  File cationFile("./benchmarks/PNP/output/cation.pvd");
  File anionFile("./benchmarks/PNP/output/anion.pvd");
  File potentialFile("./benchmarks/PNP/output/potential.pvd");

  // Initialize variational forms
  printf("\tvariational forms...\n"); fflush(stdout);
  pnp::FunctionSpace V(mesh);
  pnp::BilinearForm a_pnp(V,V);
  pnp::LinearForm L_pnp(V);
  Constant eps(coeff_par.relative_permittivity);
  Constant Dp(coeff_par.cation_diffusivity);
  Constant Dn(coeff_par.anion_diffusivity);
  Constant qp(coeff_par.cation_valency);
  Constant qn(coeff_par.anion_valency);
  Constant zero(0.0);
  a_pnp.eps = eps; L_pnp.eps = eps;
  a_pnp.Dp = Dp; L_pnp.Dp = Dp;
  a_pnp.Dn = Dn; L_pnp.Dn = Dn;
  a_pnp.qp = qp; L_pnp.qp = qp;
  a_pnp.qn = qn; L_pnp.qn = qn;

  //EAFE Formulation
  Constant gamma(0.0);
  EAFE::FunctionSpace V_cat(mesh);
  EAFE::BilinearForm a_cat(V_cat,V_cat);
  a_cat.alpha = Dp;
  a_cat.gamma = gamma;
  EAFE::FunctionSpace V_an(mesh);
  EAFE::BilinearForm a_an(V_an,V_an);
  a_an.alpha = Dn;
  a_an.gamma = gamma;

  // analytic solution
  Function analyticSolutionFunction(V);
  Function analyticCation(analyticSolutionFunction[0]);
  Function analyticAnion(analyticSolutionFunction[1]);
  Function analyticPotential(analyticSolutionFunction[2]);
  analyticCationExpression cationExpression;
  analyticAnionExpression anionExpression;
  analyticPotentialExpression potentialExpression;
  analyticCation.interpolate(cationExpression);
  analyticAnion.interpolate(anionExpression);
  analyticPotential.interpolate(potentialExpression);
  // analyticCation.interpolate(zero);
  // analyticAnion.interpolate(zero);
  // analyticPotential.interpolate(zero);

  File EXcationFile("./benchmarks/PNP/output/Ex_cation.pvd");
  File EXanionFile("./benchmarks/PNP/output/Ex_anion.pvd");
  File EXpotentialFile("./benchmarks/PNP/output/Ex_potential.pvd");
  EXcationFile << analyticCation;
  EXanionFile << analyticAnion;
  EXpotentialFile << analyticPotential;

  // Set Dirichlet boundaries
  printf("\tboundary conditions...\n"); fflush(stdout);
  unsigned int dirichlet_coord = 0;
  Constant zero_vec(0.0, 0.0, 0.0);
  SymmBoundaries boundary(dirichlet_coord, -domain_par.length_x/2.0, domain_par.length_x/2.0);
  dolfin::DirichletBC bc(V, zero_vec, boundary);

  // Initialize analytic expressions
  printf("\tanalytic expressions...\n"); fflush(stdout);
  LogCharge Cation(lower_cation_val, upper_cation_val, -domain_par.length_x/2.0, domain_par.length_x/2.0, dirichlet_coord);
  LogCharge Anion(lower_anion_val, upper_anion_val, -domain_par.length_x/2.0, domain_par.length_x/2.0, dirichlet_coord);
  Voltage Volt(lower_potential_val, upper_potential_val, -domain_par.length_x/2.0, domain_par.length_x/2.0, dirichlet_coord);

  // Interpolate analytic expressions
  Function solutionFunction(V);

  ivector cation_dofs;
  ivector anion_dofs;
  ivector potential_dofs;
  get_dofs(&solutionFunction, &cation_dofs, 0);
  get_dofs(&solutionFunction, &anion_dofs, 1);
  get_dofs(&solutionFunction, &potential_dofs, 2);

  // Solve for consistent voltage : not yet implemented

  Function cationSolution(solutionFunction[0]);
  Function anionSolution(solutionFunction[1]);
  Function potentialSolution(solutionFunction[2]);

  // cationSolution.interpolate(Cation);
  // anionSolution.interpolate(Anion);
  // potentialSolution.interpolate(Volt);

  cationSolution.interpolate(cationExpression);
  anionSolution.interpolate(anionExpression);
  potentialSolution.interpolate(potentialExpression);

  // print to file
  cationFile << cationSolution;
  anionFile << anionSolution;
  potentialFile << potentialSolution;

  // Interpolate analytic expressions for EAFE
  Function CatCatFunction(V_cat);
  Function CatBetaFunction(V_cat);
  Function AnAnFunction(V_an);
  Function AnBetaFunction(V_an);
  CatCatFunction.interpolate(cationSolution);
  CatBetaFunction.interpolate(potentialSolution);
  *(CatBetaFunction.vector())*=coeff_par.cation_mobility;
  *(CatBetaFunction.vector())+=*(CatCatFunction.vector());
  AnAnFunction.interpolate(anionSolution);
  AnBetaFunction.interpolate(potentialSolution);
  *(AnBetaFunction.vector())*=coeff_par.anion_mobility;
  *(AnBetaFunction.vector())+=*(AnAnFunction.vector());


  // initialize linear system
  printf("\tlinear algebraic objects...\n"); fflush(stdout);
  EigenMatrix A_pnp, A_cat, A_an;
  EigenVector b_pnp;
  dCSRmat A_fasp;
  dBSRmat A_fasp_bsr;
  dvector b_fasp, solu_fasp;

  // Setup FASP solver
  printf("\tsetup FASP solver...\n"); fflush(stdout);
  input_param inpar;
  itsolver_param itpar;
  AMG_param amgpar;
  ILU_param ilupar;
  char inputfile[] = "./benchmarks/PNP/bsr.dat";
  fasp_param_input(inputfile, &inpar);
  fasp_param_init(&inpar, &itpar, &amgpar, &ilupar, NULL);
  INT status = FASP_SUCCESS;

  //*************************************************************
  //  Initialize Newton solver
  //*************************************************************
  // Setup newton parameters and compute initial residual
  printf("\tnewton solver setup...\n"); fflush(stdout);
  Function solutionUpdate(V);
  Function dcat(solutionUpdate[0]);
  Function dan(solutionUpdate[1]);
  Function dphi(solutionUpdate[2]);
  Function dcat_cat(V_cat);
  Function dan_an(V_an);
  dcat.interpolate(zero);
  dan.interpolate(zero);
  dphi.interpolate(zero);
  dcat_cat.interpolate(zero);
  dan_an.interpolate(zero);
  unsigned int newton_iteration = 0;

  // compute initial residual and Jacobian
  printf("\tconstruct residual...\n"); fflush(stdout);
  double lambda=1.0;
  L_pnp.CatCat = cationSolution;
  L_pnp.AnAn = anionSolution;
  L_pnp.EsEs = potentialSolution;
  assemble(b_pnp, L_pnp);
  bc.apply(b_pnp);
  double initial_residual = b_pnp.norm("l2");
  double relative_residual = 1.0;
  printf("\tinitial nonlinear residual has l2-norm of %e\n", initial_residual);

  printf("\tinitialized succesfully!\n\n"); fflush(stdout);

  fasp_dvec_alloc(b_pnp.size(), &solu_fasp);

  //*************************************************************
  //  Solve : this will be inside Newton loop
  //*************************************************************
  double tol=1E-6;
  while (relative_residual>tol)
  {
      printf("Solve the system\n"); fflush(stdout);
      newton_iteration++;

      // Construct stiffness matrix
      printf("\tconstruct stiffness matrix...\n"); fflush(stdout);
      a_pnp.CatCat = cationSolution;
      a_pnp.AnAn = anionSolution;
      a_pnp.EsEs = potentialSolution;
      assemble(A_pnp, a_pnp);
      a_cat.eta = CatCatFunction;
      a_cat.beta = CatBetaFunction;
      a_an.eta = AnAnFunction;
      a_an.beta = AnBetaFunction;
      assemble(A_cat, a_cat);
      assemble(A_an, a_an);
      replace_matrix(3,0, &V, &V_cat, &A_pnp, &A_cat);
      replace_matrix(3,1, &V, &V_an , &A_pnp, &A_an );
      bc.apply(A_pnp);

      // Convert to fasp
      printf("\tconvert to FASP and solve...\n"); fflush(stdout);
      EigenVector_to_dvector(&b_pnp,&b_fasp);
      EigenMatrix_to_dCSRmat(&A_pnp,&A_fasp);
      A_fasp_bsr = fasp_format_dcsr_dbsr(&A_fasp, 3);
      fasp_dvec_set(b_fasp.row, &solu_fasp, 0.0);
      status = fasp_solver_dbsr_krylov_amg(&A_fasp_bsr, &b_fasp, &solu_fasp, &itpar, &amgpar);

      // map solu_fasp into solutionUpdate
      printf("\tconvert FASP solution to function...\n"); fflush(stdout);
      copy_dvector_to_vector_function(&solu_fasp, &solutionUpdate, &cation_dofs, &cation_dofs);
      copy_dvector_to_vector_function(&solu_fasp, &solutionUpdate, &anion_dofs, &anion_dofs);
      copy_dvector_to_vector_function(&solu_fasp, &solutionUpdate, &potential_dofs, &potential_dofs);

      // update solution and reset solutionUpdate
      printf("\tupdate solution...\n"); fflush(stdout);
      update_solution(&cationSolution, &solutionUpdate[0],lambda);
      update_solution(&anionSolution, &solutionUpdate[1],lambda);
      update_solution(&potentialSolution, &solutionUpdate[2],lambda);


      CatCatFunction.interpolate(cationSolution);
      CatBetaFunction.interpolate(potentialSolution);
      *(CatBetaFunction.vector())*=coeff_par.cation_mobility;
      *(CatBetaFunction.vector())+=*(CatCatFunction.vector());
      AnAnFunction.interpolate(anionSolution);
      AnBetaFunction.interpolate(potentialSolution);
      *(AnBetaFunction.vector())*=coeff_par.anion_mobility;
      *(AnBetaFunction.vector())+=*(AnAnFunction.vector());


      // compute residual
      L_pnp.CatCat = cationSolution;
      L_pnp.AnAn = anionSolution;
      L_pnp.EsEs = potentialSolution;
      assemble(b_pnp, L_pnp);
      bc.apply(b_pnp);
      relative_residual = b_pnp.norm("l2") / initial_residual;
      if (newton_iteration == 1)
        printf("\trelative nonlinear residual after 1 iteration has l2-norm of %e\n", relative_residual);
      else
        printf("\trelative nonlinear residual after %d iterations has l2-norm of %e\n", newton_iteration, relative_residual);

      // write computed solution to file
      printf("\tsolved successfully!\n"); fflush(stdout);
      cationFile << cationSolution;
      anionFile << anionSolution;
      potentialFile << potentialSolution;

      // compute solution error
      // printf("\nCompute the error\n"); fflush(stdout);
      // Function Error1(analyticCation);
      // Function Error2(analyticAnion);
      // Function Error3(analyticPotential);
      // *(Error1.vector())-=*(cationSolution.vector());
      // *(Error2.vector())-=*(anionSolution.vector());
      // *(Error3.vector())-=*(potentialSolution.vector());
      // double cationError = 0.0;
      // double anionError = 0.0;
      // double potentialError = 0.0;
      // L2Error::Form_M L2error1(mesh,Error1);
      // cationError = assemble(L2error1);
      // L2Error::Form_M L2error2(mesh,Error2);
      // anionError = assemble(L2error2);
      // L2Error::Form_M L2error3(mesh,Error3);
      // potentialError = assemble(L2error3);
      // printf("\tcation l2 error is:     %e\n", cationError);
      // printf("\tanion l2 error is:      %e\n", anionError);
      // printf("\tpotential l2 error is:  %e\n", potentialError);

      // print error
      // cationFile << cationSolution;
      // anionFile << anionSolution;
      // potentialFile << potentialSolution;
  }

  printf("\n-----------------------------------------------------------    "); fflush(stdout);
  printf("\n End                                                           "); fflush(stdout);
  printf("\n-----------------------------------------------------------\n\n"); fflush(stdout);

  return 0;
}
