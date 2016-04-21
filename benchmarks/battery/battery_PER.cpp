/*! \file lin_pnp.cpp
 *
 *  \brief Setup and solve the linearized PNP equation using FASP
 *
 *  \note Currently initializes the problem based on specification
 */
#include <boost/filesystem.hpp>
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
#include "chargeSurface.h"
#include "chargeVolume.h"
#include "L2Error.h"
#include "energy.h"
#include "newton.h"
#include "newton_functs.h"
#include "spheres.h"
#include <ctime>
extern "C"
{
  #include "fasp.h"
  #include "fasp_functs.h"
  #define FASP_BSR ON /** use BSR format in fasp */
}
using namespace dolfin;
// using namespace std;

bool eafe_switch = false;
double max_mesh_growth = 1.5;

double lower_cation_val = 1.0;  // 1 / m^3
double upper_cation_val = 1.0;  // 1 / m^3
double lower_anion_val = 1.0;  // 1 / m^3
double upper_anion_val = 1.0;  // 1 / m^3
double lower_potential_val = +1.0e-0;  // V
double upper_potential_val = -1.0e-0;  // V
double Lx = 12.0;
double Ly = 12.0;
double Lz = 12.0;

unsigned int dirichlet_coord = 0;

double time_step_size = 1.0;
double final_time = 50.0;

double get_initial_residual (
  pnp::LinearForm* L,
  int index,
  dolfin::Function* cation,
  dolfin::Function* anion,
  dolfin::Function* potential
);

double update_solution_pnp (
  dolfin::Function* iterate0,
  dolfin::Function* iterate1,
  dolfin::Function* iterate2,
  dolfin::Function* update0,
  dolfin::Function* update1,
  dolfin::Function* update2,
  double relative_residual,
  double initial_residual,
  pnp::LinearForm* L,
  int index,
  newton_param* params
);

// Sub domain for Periodic boundary condition
class PeriodicBoundary : public SubDomain
{
  // Left boundary is "target domain" G
  bool inside(const Array<double>& x, bool on_boundary) const
  {
    return on_boundary && (
      std::abs(x[0]) < Lx / 2.0 + 5.0 * DOLFIN_EPS
      || std::abs(x[1]) < Ly / 2.0 + 5.0 * DOLFIN_EPS
      || std::abs(x[2]) < Lz / 2.0 + 5.0 * DOLFIN_EPS
    );
  }

  // Map right boundary (H) to left boundary (G)
  void map(const Array<double>& x, Array<double>& y) const
  {
    y[0] = x[0];
    y[1] = x[1];
    y[2] = x[2];
  }
};

// Sub domain for Periodic boundary condition
class PeriodicBoundary1D : public SubDomain
{
  // Left boundary is "target domain" G
  bool inside(const Array<double>& x, bool on_boundary) const
  {
    return on_boundary && (
      std::abs(x[0]) < Lx / 2.0 + 5.0 * DOLFIN_EPS
      || std::abs(x[1]) < Ly / 2.0 + 5.0 * DOLFIN_EPS
      || std::abs(x[2]) < Lz / 2.0 + 5.0 * DOLFIN_EPS
    );
  }

  // Map right boundary (H) to left boundary (G)
  void map(const Array<double>& x, Array<double>& y) const
  {
    y[0] = x[0];
  }
};


int main(int argc, char** argv)
{
  if (argc > 1)
    if (std::string(argv[1])=="EAFE")
      eafe_switch = true;

  printf("\n-----------------------------------------------------------    ");
  printf("\n Solving the linearized Poisson-Nernst-Planck system           ");
  printf("\n of a single cation and anion ");
  if (eafe_switch)
    printf("using EAFE approximations \n to the Jacobians");
  printf("\n-----------------------------------------------------------\n\n");
  fflush(stdout);

  // Need to use Eigen for linear algebra
  parameters["linear_algebra_backend"] = "Eigen";
  parameters["allow_extrapolation"] = true;
  parameters["refinement_algorithm"] = "plaza_with_parent_facets";

  //*************************************************************
  //  Initialization
  //*************************************************************
  printf("Initialize the problem\n"); fflush(stdout);

  // Deleting the folders:
  boost::filesystem::remove_all("./benchmarks/battery/output");
  boost::filesystem::remove_all("./benchmarks/battery/meshOut");

  // build mesh
  printf("mesh...\n"); fflush(stdout);
  dolfin::Mesh mesh_adapt("./benchmarks/battery/mesh.xml.gz");
  // MeshFunction<std::size_t> sub_domains_adapt(mesh_adapt, "./benchmarks/battery/boundary_parts.xml.gz");
  // dolfin::MeshFunction<std::size_t> subdomains_init;
  // dolfin::MeshFunction<std::size_t> surfaces_init;
  dolfin::File meshOut("./benchmarks/battery/meshOut/mesh.pvd");
  // domain_build(&domain_par, &mesh_adapt, &subdomains_init, &surfaces_init);


  // dolfin::File BoundaryFile("./benchmarks/battery/meshOut/boundary.pvd");
  // BoundaryFile << sub_domains_adapt;
  meshOut << mesh_adapt;
  // return 0;

  // read coefficients and boundary values
  printf("coefficients...\n"); fflush(stdout);
  coeff_param coeff_par;
  char coeff_param_filename[] = "./benchmarks/battery/coeff_params.dat";
  coeff_param_input(coeff_param_filename, &coeff_par);
  print_coeff_param(&coeff_par);

  // initialize Newton solver parameters
  printf("Newton solver parameters...\n"); fflush(stdout);
  newton_param newtparam;
  char newton_param_file[] = "./benchmarks/battery/newton_param.dat";
  newton_param_input (newton_param_file, &newtparam);
  print_newton_param(&newtparam);
  double initial_residual, relative_residual = 1.0;

  // Setup FASP solver
  printf("FASP solver parameters...\n\n"); fflush(stdout);
  input_param inpar;
  itsolver_param itpar;
  AMG_param amgpar;
  ILU_param ilupar;
  char fasp_params[] = "./benchmarks/battery/bsr.dat";
  fasp_param_input(fasp_params, &inpar);
  fasp_param_init(&inpar, &itpar, &amgpar, &ilupar, NULL);
  INT status = FASP_SUCCESS;

  // File
  std::ofstream ofs;
  ofs.open ("./benchmarks/battery/data.txt", std::ofstream::out);
  ofs << "starting mesh size =" << mesh_adapt.num_cells() << "\n";
  ofs << "t" << "\t" << "NewtonIteration" << "\t" << "RelativeResidual" << "\t" << "Cation" << "\t" << "Anion" << "\t" << "Potential" << "\t" << "Energy" << "\t"<< "TimeElaspsed" << "\t" << "MeshSize" << "\n";
  ofs.close();

  // open files for outputting solutions
  File cationFile("./benchmarks/battery/output/cation.pvd");
  File anionFile("./benchmarks/battery/output/anion.pvd");
  File potentialFile("./benchmarks/battery/output/potential.pvd");

  File dcationFile("./benchmarks/battery/output/dcation.pvd");
  File danionFile("./benchmarks/battery/output/danion.pvd");
  File dpotentialFile("./benchmarks/battery/output/dpotential.pvd");

  File cationFileBefore("./benchmarks/battery/output/cationBefore.pvd");
  File anionFileBefore("./benchmarks/battery/output/anionefore.pvd");
  File potentialFileBefore("./benchmarks/battery/output/potentialefore.pvd");

  File cationFileAfter("./benchmarks/battery/output/cationAfter.pvd");
  File anionFileAfter("./benchmarks/battery/output/anionAfter.pvd");
  File potentialFileAfter("./benchmarks/battery/output/potentialAfter.pvd");

  PeriodicBoundary periodic_boundary;
  PeriodicBoundary1D periodic_boundary1D;

  // PREVIOUS ITERATE
  pnp::FunctionSpace V_init(mesh_adapt,periodic_boundary);
  dolfin::Function initial_soln(V_init);
  dolfin::Function initial_cation(initial_soln[0]);
  dolfin::Function initial_anion(initial_soln[1]);
  dolfin::Function initial_potential(initial_soln[2]);

  LogCharge_SPH Cation(
  // LogCharge Cation(
    lower_cation_val,
    upper_cation_val,
    -Lx / 2.0,
    Lx / 2.0,
    dirichlet_coord
  );

  LogCharge_SPH Anion(
  // LogCharge Anion(
    lower_anion_val,
    upper_anion_val,
    -Lx / 2.0,
    Lx / 2.0,
    dirichlet_coord
  );

  Potential_SPH Volt(
  // Voltage Volt(
    lower_potential_val,
    upper_potential_val,
    -Lx / 2.0,
    Lx / 2.0,
    dirichlet_coord
  );

  initial_cation.interpolate(Constant(0.69314718056));
  initial_anion.interpolate(Constant(0.740978168975));
  initial_potential.interpolate(Volt);

  // output solution after solved for timestep
  cationFile << initial_cation;
  anionFile << initial_anion;
  potentialFile << initial_potential;

  // initialize error
  double cationError = 0.0;
  double anionError = 0.0;
  double potentialError = 0.0;
  double energy = 0.0;

  // Time
  std::clock_t begin = std::clock();
  std::clock_t end;
  double timeElaspsed;

  // Fasp matrices and vectors
  dCSRmat A_fasp;
  dBSRmat A_fasp_bsr;
  dvector b_fasp, solu_fasp;

  // Constants
  Constant eps(coeff_par.relative_permittivity);
  Constant Dp(coeff_par.cation_diffusivity);
  Constant Dn(coeff_par.anion_diffusivity);
  Constant qp(coeff_par.cation_valency);
  Constant qn(coeff_par.anion_valency);
  Constant C_dt(time_step_size);
  Constant cat_alpha(coeff_par.cation_diffusivity*time_step_size);
  Constant an_alpha(coeff_par.anion_diffusivity*time_step_size);
  Constant one(1.0);
  // Constant C_g(1.0);//1.0*coeff_par.relative_permittivity);
  Constant zero(0.0);




  // Check charge sanity
  printf("Checking for sane charges\n");
  chargeVolume::Functional volumeCharge(mesh_adapt);
  volumeCharge.charge = initial_cation;
  double cationNetCharge = assemble(volumeCharge);

  volumeCharge.charge = initial_anion;
  double anionNetCharge = assemble(volumeCharge);

  SpheresSubDomain SPS;
  FacetFunction<std::size_t> surf_boundaries(mesh_adapt);
  surf_boundaries.set_all(0);
  SPS.mark(surf_boundaries, 1);
  meshOut << surf_boundaries;
  chargeSurface::Functional surfaceCharge(mesh_adapt);
  surfaceCharge.charge = one;
  surfaceCharge.ds = surf_boundaries;
  double surfaceNetCharge = assemble(surfaceCharge);

  double correctedSurfaceCharge = -(cationNetCharge - anionNetCharge) / surfaceNetCharge;
  Constant C_g( correctedSurfaceCharge );
  surfaceCharge.charge = C_g;
  surfaceNetCharge = assemble(surfaceCharge);
  printf("\tcorrected Surface Charge %e\n", correctedSurfaceCharge);
  printf("\tcorrected Surface Net charge is %e\n", surfaceNetCharge);
  printf("\ttotal charge is %e\n\n", cationNetCharge - anionNetCharge + surfaceNetCharge);



  for (double t = time_step_size; t < final_time; t += time_step_size) {
    // printf("\nSet voltage to %e...\n", volt); fflush(stdout);

    //*************************************************************
    //  Mesh adaptivity
    //*************************************************************


    // set adaptivity parameters
    dolfin::Mesh mesh(mesh_adapt);
    // dolfin::MeshFunction<std::size_t> sub_domains(sub_domains_adapt);
    // dolfin::MeshFunction<std::size_t> sub_domains = adapt(sub_domains_adapt, mesh);
    double entropy_tol = newtparam.adapt_tol;
    unsigned int num_adapts = 0, max_adapts = 5;
    bool adaptive_convergence = false;

    // initialize storage functions for adaptivity
    printf("store previous solution and initialize solution functions\n"); fflush(stdout);
    pnp::FunctionSpace V_adapt(mesh_adapt ,periodic_boundary);
    dolfin::Function prev_soln_adapt(V_adapt);
    dolfin::Function prev_cation_adapt(prev_soln_adapt[0]);
    dolfin::Function prev_anion_adapt(prev_soln_adapt[1]);
    dolfin::Function prev_potential_adapt(prev_soln_adapt[2]);
    prev_cation_adapt.interpolate(initial_cation);
    prev_anion_adapt.interpolate(initial_anion);
    prev_potential_adapt.interpolate(initial_potential);

    dolfin::Function soln_adapt(V_adapt);
    dolfin::Function cation_adapt(soln_adapt[0]);
    dolfin::Function anion_adapt(soln_adapt[1]);
    dolfin::Function potential_adapt(soln_adapt[2]);
    cation_adapt.interpolate(initial_cation);
    anion_adapt.interpolate(initial_anion);
    potential_adapt.interpolate(initial_potential);

    // adaptivity loop
    printf("Adaptivity loop\n"); fflush(stdout);
    while (!adaptive_convergence)
    {
      // mark and output mesh
      FacetFunction<std::size_t> boundaries(mesh);
      boundaries.set_all(0);
      SPS.mark(boundaries, 1);
      meshOut << boundaries;

      printf("\tComputing fix point..."); fflush(stdout);
      pnp::FunctionSpace V_index(mesh);
      Constant one_vec(1.0,1.0,1.0);
      DirichletBC bc_index(V_index, one_vec, DomainBoundary());
      Function u_index(V_index);
      u_index.interpolate(Constant(0.0,0.0,0.0));
      bc_index.apply(*(u_index.vector()));
      Function u_index0(u_index[0]);
      int index = 0;
      bool flag_index=false;
      while (flag_index)
      {
        if (u_index0.vector()->getitem(index)==1){
          flag_index=true;
        }
        else
          index += 1;
      }
      index = 3 * index + 2;
      printf("index = %d\n", index); fflush(stdout);
      // int index = 3* ( (int) mesh.num_vertices()/4.0 ) + 2 +3*25;

      // Initialize variational forms
      printf("\tvariational forms...\n"); fflush(stdout);
      pnp::FunctionSpace V(mesh ,periodic_boundary);
      pnp::BilinearForm a_pnp(V,V);
      pnp::LinearForm L_pnp(V);
      a_pnp.eps = eps; L_pnp.eps = eps;
      a_pnp.Dp = Dp; L_pnp.Dp = Dp;
      a_pnp.Dn = Dn; L_pnp.Dn = Dn;
      a_pnp.qp = qp; L_pnp.qp = qp;
      a_pnp.qn = qn; L_pnp.qn = qn;
      a_pnp.dt = C_dt; L_pnp.dt = C_dt;
      Constant g(0.01);
      L_pnp.g = C_g;//C_g; // one;
      L_pnp.ds = boundaries;

      // Interpolate previous solutions analytic expressions
      printf("\tinterpolate previous step solution onto new mesh...\n"); fflush(stdout);
      dolfin::Function prev_soln(V);
      dolfin::Function previous_cation(prev_soln[0]);
      previous_cation.interpolate(prev_cation_adapt);
      dolfin::Function previous_anion(prev_soln[1]);
      previous_anion.interpolate(prev_anion_adapt);
      dolfin::Function previous_potential(prev_soln[2]);
      previous_potential.interpolate(prev_potential_adapt);

      printf("\tinterpolate solution onto new mesh...\n"); fflush(stdout);
      dolfin::Function solutionFunction(V);
      dolfin::Function cationSolution(solutionFunction[0]);
      cationSolution.interpolate(cation_adapt);
      dolfin::Function anionSolution(solutionFunction[1]);
      anionSolution.interpolate(anion_adapt);
      dolfin::Function potentialSolution(solutionFunction[2]);
      potentialSolution.interpolate(potential_adapt);

      // Set Dirichlet boundaries
      printf("\tboundary conditions...\n"); fflush(stdout);
      Constant zero_vec(0.0, 0.0, 0.0);
      SymmBoundaries boundary(dirichlet_coord, -Lx / 2.0, Lx / 2.0);
      dolfin::DirichletBC bc(V, zero_vec, boundary);
      printf("\t\tdone\n"); fflush(stdout);
      // map dofs
      ivector cation_dofs;
      ivector anion_dofs;
      ivector potential_dofs;
      get_dofs(&solutionFunction, &cation_dofs, 0);
      get_dofs(&solutionFunction, &anion_dofs, 1);
      get_dofs(&solutionFunction, &potential_dofs, 2);

      //EAFE Formulation
      if (eafe_switch)
        printf("\tEAFE initialization...\n");
      EAFE::FunctionSpace V_cat(mesh ,periodic_boundary1D);
      EAFE::BilinearForm a_cat(V_cat,V_cat);
      a_cat.alpha = an_alpha;
      a_cat.gamma = one;
      EAFE::FunctionSpace V_an(mesh ,periodic_boundary1D);
      EAFE::BilinearForm a_an(V_an,V_an);
      a_an.alpha = cat_alpha;
      a_an.gamma = one;
      dolfin::Function CatCatFunction(V_cat);
      dolfin::Function CatBetaFunction(V_cat);
      dolfin::Function AnAnFunction(V_an);
      dolfin::Function AnBetaFunction(V_an);

      // initialize linear system
      printf("\tlinear algebraic objects...\n"); fflush(stdout);
      EigenMatrix A_pnp, A_cat, A_an;
      EigenVector b_pnp;

      //*************************************************************
      //  Initialize Newton solver
      //*************************************************************
      // Setup newton parameters and compute initial residual
      printf("\tNewton solver initialization...\n"); fflush(stdout);
      dolfin::Function solutionUpdate(V);
      unsigned int newton_iteration = 0;

      // set initial residual
      printf("\tupdate initial residual...\n"); fflush(stdout);
      initial_residual = get_initial_residual(
        &L_pnp,
        index,
        &previous_cation,
        &previous_anion,
        &previous_potential
      );

      printf("\tcompute relative residual...\n"); fflush(stdout);
      L_pnp.CatCat = cationSolution;
      L_pnp.AnAn = anionSolution;
      L_pnp.EsEs = potentialSolution;
      L_pnp.CatCat_t0 = previous_cation;
      L_pnp.AnAn_t0 = previous_anion;
      assemble(b_pnp, L_pnp);
      b_pnp[index] = 0.0; //bc.apply(b_pnp);
      relative_residual = b_pnp.norm("l2") / initial_residual;

      if (num_adapts == 0)
        printf("\tinitial nonlinear residual has l2-norm of %e\n", initial_residual);
      else
        printf("\tadapted relative nonlinear residual is %e\n", relative_residual);

      fasp_dvec_alloc(b_pnp.size(), &solu_fasp);
      printf("\tinitialized succesfully...\n\n"); fflush(stdout);

      //*************************************************************
      //  Newton solver
      //*************************************************************
      printf("Solve the nonlinear system\n"); fflush(stdout);

      double nonlinear_tol = newtparam.tol;
      unsigned int max_newton_iters = newtparam.max_it;
      while (relative_residual > nonlinear_tol && newton_iteration < max_newton_iters)
      {
        printf("\nNewton iteration: %d at t=%f\n", ++newton_iteration,t); fflush(stdout);

        // Construct stiffness matrix
        printf("\tconstruct stiffness matrix...\n"); fflush(stdout);
        a_pnp.CatCat = cationSolution;
        a_pnp.AnAn = anionSolution;
        a_pnp.EsEs = potentialSolution;
        assemble(A_pnp, a_pnp);

        // EAFE expressions
        if (eafe_switch) {
          printf("\tcompute EAFE expressions...\n");
          CatCatFunction.interpolate(cationSolution);
          CatBetaFunction.interpolate(potentialSolution);
          *(CatBetaFunction.vector()) *= coeff_par.cation_valency;
          *(CatBetaFunction.vector()) += *(CatCatFunction.vector());
          AnAnFunction.interpolate(anionSolution);
          AnBetaFunction.interpolate(potentialSolution);
          *(AnBetaFunction.vector()) *= coeff_par.anion_valency;
          *(AnBetaFunction.vector()) += *(AnAnFunction.vector());

          // Construct EAFE approximations to Jacobian
          printf("\tconstruct EAFE modifications...\n"); fflush(stdout);
          a_cat.eta = CatCatFunction;
          a_cat.beta = CatBetaFunction;
          a_an.eta = AnAnFunction;
          a_an.beta = AnBetaFunction;
          assemble(A_cat, a_cat);
          assemble(A_an, a_an);

          // Modify Jacobian
          printf("\treplace Jacobian with EAFE approximations...\n"); fflush(stdout);
          replace_matrix(3,0, &V, &V_cat, &A_pnp, &A_cat);
          replace_matrix(3,1, &V, &V_an , &A_pnp, &A_an );
        }
        // bc.apply(A_pnp);
        replace_row(index, &A_pnp);

        // Convert to fasp
        printf("\tconvert to FASP and solve...\n"); fflush(stdout);
        EigenVector_to_dvector(&b_pnp,&b_fasp);
        EigenMatrix_to_dCSRmat(&A_pnp,&A_fasp);
        A_fasp_bsr = fasp_format_dcsr_dbsr(&A_fasp, 3);
        fasp_dvec_set(b_fasp.row, &solu_fasp, 0.0);
        // BSR SOLVER
        status = fasp_solver_dbsr_krylov_amg(&A_fasp_bsr, &b_fasp, &solu_fasp, &itpar, &amgpar);
        // CSR SOLVER
        //status = fasp_solver_dcsr_krylov(&A_fasp, &b_fasp, &solu_fasp, &itpar);
        if (status < 0)
          printf("\n### WARNING: Solver failed! Exit status = %d.\n\n", status);
        else
          printf("\tsolved linear system successfully...\n");

        // map solu_fasp into solutionUpdate
        printf("\tconvert FASP solution to function...\n"); fflush(stdout);
        copy_dvector_to_vector_function(&solu_fasp, &solutionUpdate, &cation_dofs, &cation_dofs);
        copy_dvector_to_vector_function(&solu_fasp, &solutionUpdate, &anion_dofs, &anion_dofs);
        copy_dvector_to_vector_function(&solu_fasp, &solutionUpdate, &potential_dofs, &potential_dofs);

        // dcationFile << solutionUpdate[0];
        // danionFile << solutionUpdate[1];
        // dpotentialFile << solutionUpdate[2];

        // update solution and reset solutionUpdate
        printf("\tupdate solution...\n"); fflush(stdout);
        relative_residual = update_solution_pnp (
          &cationSolution,
          &anionSolution,
          &potentialSolution,
          &(solutionUpdate[0]),
          &(solutionUpdate[1]),
          &(solutionUpdate[2]),
          relative_residual,
          initial_residual,
          &L_pnp,
          index,
          &newtparam
        );
        if (relative_residual < 0.0) {
          printf("Newton backtracking failed!\n");
          printf("\tresidual has not decreased after damping %d times\n", newtparam.damp_it);
          printf("\tthe relative residual is %e\n", relative_residual);
          relative_residual *= -1.0;
        }

        cationFile << cationSolution;
        anionFile << anionSolution;
        potentialFile << potentialSolution;

        // update nonlinear residual
        L_pnp.CatCat = cationSolution;
        L_pnp.AnAn = anionSolution;
        L_pnp.EsEs = potentialSolution;
        L_pnp.CatCat_t0 = previous_cation;
        L_pnp.AnAn_t0 = previous_anion;
        assemble(b_pnp, L_pnp);
        b_pnp[index] = 0.0; // bc.apply(b_pnp);


        fasp_dbsr_free(&A_fasp_bsr);

      }

      if (relative_residual < nonlinear_tol)
        printf("\nSuccessfully solved the system below desired residual in %d steps!\n\n", newton_iteration);
      else {
        printf("\nDid not converge in %d Newton iterations at t=%e...\n", max_newton_iters,t);
        printf("\tcurrent relative residual is %e > %e\n\n", relative_residual, nonlinear_tol);
      }

      // compute local entropy and refine mesh
      printf("Computing electric field for refinement\n");
      unsigned int num_refines;
      double max_mesh_size_double = max_mesh_growth * ((double) mesh_adapt.size(3));
      int max_mesh_size = (int) std::floor(max_mesh_size_double);
      num_refines = check_electric_field(
        &potentialSolution,
        &mesh_adapt,
        entropy_tol,
        max_mesh_size_double,
        // newtparam.max_cells,
        3
      );
      printf("\tneed %d levels of refinement\n", num_refines);

      // free fasp solution
      fasp_dvec_free(&solu_fasp);

      if ( (num_refines == 0) || ( ++num_adapts > max_adapts ) ){
        // successful solve
          if (num_refines == 0)
            printf("\tsuccessfully distributed electric field below desired electric field in %d adapts!\n\n", num_adapts);
          else
            printf("\nDid not adapt mesh to electric field in %d adapts...\n", max_adapts);

          adaptive_convergence = true;
          dolfin::Function Er_cat(previous_cation);
          dolfin::Function Er_an(previous_anion);
          dolfin::Function Er_es(previous_potential);
          *(Er_cat.vector()) -= *(cationSolution.vector());
          *(Er_an.vector()) -= *(anionSolution.vector());
          *(Er_es.vector()) -= *(potentialSolution.vector());
          *(Er_cat.vector()) /= time_step_size;
          *(Er_an.vector()) /= time_step_size;
          *(Er_es.vector()) /= time_step_size;
          L2Error::Form_M L2error1(mesh,Er_cat);
          cationError = assemble(L2error1);
          L2Error::Form_M L2error2(mesh,Er_an);
          anionError = assemble(L2error2);
          L2Error::Form_M L2error3(mesh,Er_es);
          potentialError = assemble(L2error3);
          energy::Form_M EN(mesh,cationSolution,anionSolution,potentialSolution,eps);
          energy = assemble(EN);

          printf("***********************************************\n");
          printf("***********************************************\n");
          printf("Difference at t=%e...\n",t);
          printf("\tcation l2 error is:     %e\n", cationError);
          printf("\tanion l2 error is:      %e\n", anionError);
          printf("\tpotential l2 error is:  %e\n", potentialError);
          printf("\tEnergy is:  %e\n", energy);
          printf("***********************************************\n");
          printf("***********************************************\n\n");
          end = clock();

          ofs.open("./benchmarks/battery/data.txt", std::ofstream::out | std::ofstream::app);
          timeElaspsed = double(end - begin) / CLOCKS_PER_SEC;
          ofs << t << "\t" << newton_iteration << "\t" << relative_residual << "\t" << cationError << "\t" << anionError << "\t" << potentialError << "\t" << energy << "\t"<< timeElaspsed << "\t" << mesh.num_cells() << "\n";
          ofs.close();

          // store solution as solution from previous step
          std::shared_ptr<const Mesh> mesh_ptr( new const Mesh(mesh_adapt) );
          initial_cation = adapt(cationSolution, mesh_ptr);
          initial_anion = adapt(anionSolution, mesh_ptr);
          initial_potential = adapt(potentialSolution, mesh_ptr);
          // sub_domains_adapt= adapt(sub_domains, mesh_ptr);

          // to ensure the building_box_tree is correctly indexed
          mesh = mesh_adapt;
          // sub_domains = sub_domains_adapt;
          mesh.bounding_box_tree()->build(mesh);
          mesh_adapt.bounding_box_tree()->build(mesh_adapt);

          // output solution after solved for timestep
          cationFile << initial_cation;
          anionFile << initial_anion;
          potentialFile << initial_potential;

        break;
      }

      // adapt solutions to refined mesh
      if (num_refines == 1)
        printf("\tadapting the mesh using one level of local refinement...\n");
      else
        printf("\tadapting the mesh using %d levels of local refinement...\n", num_refines);


      cationFileBefore << cationSolution;
      anionFileBefore << anionSolution;
      potentialFileBefore << potentialSolution;

      std::shared_ptr<const Mesh> mesh_ptr( new const Mesh(mesh_adapt) );
      cation_adapt = adapt(cationSolution, mesh_ptr);
      anion_adapt = adapt(anionSolution, mesh_ptr);
      potential_adapt = adapt(potentialSolution, mesh_ptr);

      prev_cation_adapt = adapt(previous_cation, mesh_ptr);
      prev_anion_adapt = adapt(previous_anion, mesh_ptr);
      prev_potential_adapt = adapt(previous_potential, mesh_ptr);
      mesh = mesh_adapt;
      mesh.bounding_box_tree()->build(mesh);  // to ensure the building_box_tree is correctly indexed

      cationFileAfter << cation_adapt;
      anionFileAfter << anion_adapt;
      potentialFileAfter << potential_adapt;


    }
  }
  printf("\n-----------------------------------------------------------    "); fflush(stdout);
  printf("\n End                                                           "); fflush(stdout);
  printf("\n-----------------------------------------------------------\n\n"); fflush(stdout);

  return 0;
}

double update_solution_pnp (
  dolfin::Function* iterate0,
  dolfin::Function* iterate1,
  dolfin::Function* iterate2,
  dolfin::Function* update0,
  dolfin::Function* update1,
  dolfin::Function* update2,
  double relative_residual,
  double initial_residual,
  pnp::LinearForm* L,
  int index,
  newton_param* params )
{
  // compute residual
  dolfin::Function _iterate0(*iterate0);
  dolfin::Function _iterate1(*iterate1);
  dolfin::Function _iterate2(*iterate2);
  dolfin::Function _update0(*update0);
  dolfin::Function _update1(*update1);
  dolfin::Function _update2(*update2);
  update_solution(&_iterate0, &_update0);
  update_solution(&_iterate1, &_update1);
  update_solution(&_iterate2, &_update2);
  dolfin::Constant C_dt(time_step_size);
  L->CatCat = _iterate0;
  L->AnAn = _iterate1;
  L->EsEs = _iterate2;
  EigenVector b;
  assemble(b, *L);
  b[index] = 0.0; // bc->apply(b);
  double new_relative_residual = b.norm("l2") / initial_residual;


  // backtrack loop
  unsigned int damp_iters = 0;
  printf("\t\trelative residual after damping %d times: %e\n", damp_iters, new_relative_residual);

  while (
    new_relative_residual > relative_residual && damp_iters < params->damp_it )
  {
    damp_iters++;
    *(_iterate0.vector()) = *(iterate0->vector());
    *(_iterate1.vector()) = *(iterate1->vector());
    *(_iterate2.vector()) = *(iterate2->vector());
    *(_update0.vector()) *= params->damp_factor;
    *(_update1.vector()) *= params->damp_factor;
    *(_update2.vector()) *= params->damp_factor;
    update_solution(&_iterate0, &_update0);
    update_solution(&_iterate1, &_update1);
    update_solution(&_iterate2, &_update2);
    L->CatCat = _iterate0;
    L->AnAn = _iterate1;
    L->EsEs = _iterate2;
    assemble(b, *L);
    b[index] = 0.0; // bc->apply(b);
    new_relative_residual = b.norm("l2") / initial_residual;

    printf("\t\trel_res after damping %d times: %e\n", damp_iters, new_relative_residual);
  }

  // check for decrease
  if ( new_relative_residual > relative_residual )
    return -new_relative_residual;

  // update iterates
  *(iterate0->vector()) = *(_iterate0.vector());
  *(iterate1->vector()) = *(_iterate1.vector());
  *(iterate2->vector()) = *(_iterate2.vector());
  return new_relative_residual;
}

double get_initial_residual (
  pnp::LinearForm* L,
  int index,
  dolfin::Function* cation,
  dolfin::Function* anion,
  dolfin::Function* potential)
{
  pnp::FunctionSpace V( *(cation->function_space()->mesh()) );
  dolfin::Function adapt_func(V);
  dolfin::Function adapt_cation(adapt_func[0]);
  dolfin::Function adapt_anion(adapt_func[1]);
  dolfin::Function adapt_potential(adapt_func[2]);
  adapt_cation.interpolate(*cation);
  adapt_anion.interpolate(*anion);
  adapt_potential.interpolate(*potential);
  L->CatCat = adapt_cation;
  L->AnAn = adapt_anion;
  L->EsEs = adapt_potential;
  L->CatCat_t0 = adapt_cation;
  L->AnAn_t0 = adapt_anion;
  EigenVector b;
  assemble(b, *L);
  b[index] = 0.0; // bc->apply(b);
  return b.norm("l2");

}
