/// Main file for solving the linearized PNP problem
#include <boost/filesystem.hpp>
#include <fstream>
#include <iostream>
#include <string>
#include <time.h>
#include <stdlib.h>
#include <dolfin.h>
#include "pde.h"
#include "newton_status.h"
#include "mesh_refiner.h"
#include "domain.h"
#include "dirichlet.h"
#include "error.h"
extern "C" {
  #include "fasp.h"
  #include "fasp_functs.h"
}

#include "vector_linear_pnp_forms.h"
#include "linear_pnp.h"

using namespace std;

const double pi = 3.14159265359;
const double permittivity_const = 1.0;
const double electric_strength = 1.0;
const double ref_concentration = 1.0;

std::vector<double> exact_solution (double x) {
  return {
    x * electric_strength + std::sin(pi * x),
    std::log(ref_concentration) - x * x * electric_strength,
    std::log(ref_concentration) + x * x * electric_strength
  };
};

std::vector<double> exact_derivative (double x) {
  return {
    electric_strength + pi * std::cos(pi * x),
    -2.0 * x * electric_strength,
    2.0 * x * electric_strength
  };
};

std::vector<double> exact_second (double x) {
  return {
    -(pi * pi) * std::sin(pi * x),
    -2.0 * electric_strength,
    2.0 * electric_strength
  };
};

std::vector<double> diffusivities (double x) {
  return {
    0.0,
    1.0, // x > 0.0 ? 1.0 : 0.1,
    1.0 // x > 0.0 ? 2.0 : 0.1
  };
};

double fixed (double x) {
  std::vector<double> dds(exact_second(x));
  std::vector<double> s(exact_solution(x));

  return -permittivity_const * dds[0] - (std::exp(s[1]) - std::exp(s[2]));
};

std::vector<double> reactions (double x) {
  std::vector<double> diff(diffusivities(x));
  std::vector<double> dds(exact_second(x));
  std::vector<double> ds(exact_derivative(x));
  std::vector<double> s(exact_solution(x));
  return {
    0.0,
    -diff[1] * std::exp(s[1]) * (ds[1] * (ds[1] + ds[0]) + (dds[1] + dds[0])),
    -diff[2] * std::exp(s[2]) * (ds[2] * (ds[2] - ds[0]) + (dds[2] - dds[0]))
  };
};


class Exact_Solution : public dolfin::Expression {
  public:
    Exact_Solution() : dolfin::Expression(3) {}
    void eval(dolfin::Array<double>& values, const dolfin::Array<double>& x) const {
      std::vector<double> soln(exact_solution(x[0]));
      values[0] = soln[0];
      values[1] = soln[1];
      values[2] = soln[2];
    }
};

class Permittivity_Expression : public dolfin::Expression {
  public:
    void eval(dolfin::Array<double>& values, const dolfin::Array<double>& x) const {
      values[0] = permittivity_const;
    }
};

class Fixed_Charged_Expression : public dolfin::Expression {
  public:
    void eval(dolfin::Array<double>& values, const dolfin::Array<double>& x) const {
      values[0] = fixed(x[0]);
    }
};

class Diffusivity_Expression : public dolfin::Expression {
  public:
    Diffusivity_Expression() : dolfin::Expression(3) {}
    void eval(dolfin::Array<double>& values, const dolfin::Array<double>& x) const {
      std::vector<double> diff(diffusivities(x[0]));
      values[0] = diff[0];
      values[1] = diff[1];
      values[2] = diff[2];
    }
};

class Reaction_Expression : public dolfin::Expression {
  public:
    Reaction_Expression() : dolfin::Expression(3) {}
    void eval(dolfin::Array<double>& values, const dolfin::Array<double>& x) const {
      std::vector<double> reac(reactions(x[0]));
      values[0] = reac[0];
      values[1] = reac[1];
      values[2] = reac[2];
    }
};

class Valency_Expression : public dolfin::Expression {
  public:
    Valency_Expression() : dolfin::Expression(3) {}
    void eval(dolfin::Array<double>& values, const dolfin::Array<double>& x) const {
      values[0] =  0.0; // potential valency is not used
      values[1] =  1.0;
      values[2] = -1.0;
    }
};

dolfin::Function solve_pnp (
  std::size_t iteration,
  std::shared_ptr<const dolfin::Mesh> mesh,
  bool use_eafe_approximation,
  std::map<std::string, std::vector<double>> pnp_coefficients,
  std::map<std::string, std::vector<double>> pnp_sources,
  itsolver_param itsolver,
  AMG_param amg
);

void print_error (
  dolfin::Function computed_solution
);

int main (int argc, char** argv) {
  printf("\n");
  printf("----------------------------------------------------\n");
  printf(" Setting up the linearized PNP problem\n");
  printf("----------------------------------------------------\n\n");
  fflush(stdout);

  // Need to use Eigen for linear algebra
  dolfin::parameters["linear_algebra_backend"] = "Eigen";
  dolfin::parameters["allow_extrapolation"] = true;

  // Deleting the folders:
  boost::filesystem::remove_all("./benchmarks/pnp_refine_exact/output");

  // read in parameters
  printf("Reading parameters from files...\n");
  char domain_param_filename[] = "./benchmarks/pnp_refine_exact/domain.dat";
  printf("\tdomain... %s\n", domain_param_filename);
  domain_param domain;
  domain_param_input(domain_param_filename, &domain);
  std::shared_ptr<dolfin::Mesh> initial_mesh;
  initial_mesh.reset(new dolfin::Mesh(domain_build(domain)));
  // print_domain_param(&domain);

  // set parameters for FASP solver
  char fasp_params[] = "./benchmarks/pnp_refine_exact/bsr.dat";
  printf("\tFASP parameters... %s\n", fasp_params);
  input_param input;
  itsolver_param itsolver;
  AMG_param amg;
  ILU_param ilu;
  fasp_param_input(fasp_params, &input);
  fasp_param_init(
    &input,
    &itsolver,
    &amg,
    &ilu,
    NULL
  );

  // set initializer for PDE coefficients
  printf("Initialize coefficients\n");
  std::map<std::string, std::vector<double>> pnp_coefficients = {
    {"permittivity", {1.0}},
    {"diffusivity", {0.0, 2.0, 2.0}},
    {"valency", {0.0, 1.0, -1.0}}
  };
  std::map<std::string, std::vector<double>> pnp_sources = {
    {"fixed_charge", {1.0}},
    {"reaction", {0.0, 0.0, 0.0}}
  };

  //-------------------------
  // Mesh Adaptivity Loop
  //-------------------------
  std::size_t max_elements = 5000;
  std::size_t max_refine_depth = 3;
  Mesh_Refiner mesh_adapt(initial_mesh, max_elements, max_refine_depth);
  bool use_eafe_approximation = true;

  while (mesh_adapt.needs_to_solve) {
    auto mesh = mesh_adapt.get_mesh();
    dolfin::Function computed_solution = solve_pnp(
      mesh_adapt.iteration++,
      mesh,
      use_eafe_approximation,
      pnp_coefficients,
      pnp_sources,
      itsolver,
      amg
    );

    // print error of computed solution
    print_error(computed_solution);

    // compute entropy terms
    auto entropy = make_shared<dolfin::Function>(computed_solution[0]);
    mesh_adapt.mark_for_refinement(entropy);

    if (mesh_adapt.needs_refinement) {
      printf("Adaptivity loop needs to run again\n");
      mesh_adapt.refine_mesh();
    }
  }

  printf("\nCompleted adaptivity loop\n\n");
  return 0;
}


/// compute solution to PNP equation
/// using a Newton solver on the given mesh
dolfin::Function solve_pnp (
  std::size_t adaptivity_iteration,
  std::shared_ptr<const dolfin::Mesh> mesh,
  bool use_eafe_approximation,
  std::map<std::string, std::vector<double>> pnp_coefficients,
  std::map<std::string, std::vector<double>> pnp_sources,
  itsolver_param itsolver,
  AMG_param amg
) {
  // setup function spaces and forms
  printf("\nConstruct vector PNP problem\n");
  std::shared_ptr<dolfin::FunctionSpace> function_space;
  std::shared_ptr<dolfin::Form> bilinear_form;
  std::shared_ptr<dolfin::Form> linear_form;
  function_space.reset(
    new vector_linear_pnp_forms::FunctionSpace(mesh)
  );
  bilinear_form.reset(
    new vector_linear_pnp_forms::Form_a(function_space, function_space)
  );
  linear_form.reset(
    new vector_linear_pnp_forms::Form_L(function_space)
  );

  // build problem
  Linear_PNP pnp_problem(
    mesh,
    function_space,
    bilinear_form,
    linear_form,
    pnp_coefficients,
    pnp_sources,
    itsolver,
    amg
  );

  // set eafe flag
  if (use_eafe_approximation) {
  printf("Setting solver to use EAFE approximation\n");
    pnp_problem.use_eafe();
  }

  printf("Define PNP coefficients from expressions\n");
  dolfin::Function permittivity(pnp_problem.fixed_charge_space);
  Permittivity_Expression permittivity_expr;
  permittivity.interpolate(permittivity_expr);

  dolfin::Function charges(pnp_problem.fixed_charge_space);
  Fixed_Charged_Expression fc_expr;
  charges.interpolate(fc_expr);

  dolfin::Function diffusivity(pnp_problem.diffusivity_space);
  Diffusivity_Expression diff_expr;
  diffusivity.interpolate(diff_expr);

  dolfin::Function reaction(pnp_problem.reaction_space);
  Reaction_Expression reac_expr;
  reaction.interpolate(reac_expr);

  dolfin::Function valency(pnp_problem.valency_space);
  Valency_Expression valency_expr;
  valency.interpolate(valency_expr);

  std::map<std::string, dolfin::Function> pnp_coefficient_fns = {
    {"permittivity", permittivity},
    {"diffusivity", diffusivity},
    {"valency", valency}
  };
  std::map<std::string, dolfin::Function> pnp_source_fns = {
    {"fixed_charge", charges},
    {"reaction", reaction}
  };

  pnp_problem.set_coefficients(
    pnp_coefficient_fns,
    pnp_source_fns
  );


  //-------------------------
  // Print various solutions
  //-------------------------
  std::string path("./benchmarks/pnp_refine_exact/output/adapt_");
  path += std::to_string(adaptivity_iteration);
  dolfin::File solution_file0(path + "_1solution.pvd");
  dolfin::File solution_file1(path + "_2solution.pvd");
  dolfin::File solution_file2(path + "_3solution.pvd");
  dolfin::File total_charge_file(path + "_total_charge.pvd");

  // initial guess for prescibed Dirichlet
  printf("Record interpolant for given Dirichlet BCs (initial guess for solution)\n");
  std::vector<std::size_t> components = {0, 0, 0};
  std::vector<std::vector<double>> bcs;

  std::vector<double> left(exact_solution(-1.0));
  std::vector<double> right(exact_solution(+1.0));
  bcs.push_back({left[0], right[0]});
  bcs.push_back({left[1], right[1]});
  bcs.push_back({left[2], right[2]});

  pnp_problem.set_DirichletBC(components, bcs);
  dolfin::Function solutionFn = pnp_problem.get_solution();
  solution_file0 << solutionFn[0];
  solution_file1 << solutionFn[1];
  solution_file2 << solutionFn[2];
  total_charge_file << pnp_problem.get_total_charge();
  printf("\n");


  //------------------------
  // Start nonlinear solver
  //------------------------
  printf("Initializing nonlinear solver\n");

  // set nonlinear solver parameters
  const std::size_t max_newton = 15;
  const double max_residual_tol = 1.0e-10;
  const double relative_residual_tol = 1.0e-8;
  const double initial_residual = pnp_problem.compute_residual("l2");
  const double initial_max_residual = pnp_problem.compute_residual("max");
  Newton_Status newton(
    max_newton,
    initial_residual,
    relative_residual_tol,
    max_residual_tol
  );

  printf("\tinitial residual :     %10.5e\n", newton.initial_residual);
  printf("\tinitial max residual : %10.5e\n", initial_max_residual);
  printf("\n");

  newton.update_max_residual(initial_max_residual);
  while (newton.needs_to_iterate()) {
    // solve
    printf("Solving for Newton iterate %lu \n", newton.iteration);
    solutionFn = pnp_problem.fasp_solve();

    // update newton measurements
    printf("Newton measurements for iteration :\n");
    double residual = pnp_problem.compute_residual("l2");
    double max_residual = pnp_problem.compute_residual("max");
    newton.update_residuals(residual, max_residual);
    newton.update_iteration();

    // output
    printf("\tmaximum residual :  %10.5e\n", newton.max_residual);
    printf("\trelative residual : %10.5e\n", newton.relative_residual);
    printf("\toutput solution to file...\n");
    solution_file0 << solutionFn[0];
    solution_file1 << solutionFn[1];
    solution_file2 << solutionFn[2];
    total_charge_file << pnp_problem.get_total_charge();
    printf("\n");
  }


  // check status of nonlinear solve
  if (newton.converged()) {
    printf("Solver succeeded!\n");
  } else {
    newton.print_status();
  }
  printf("\nSolver exiting\n"); fflush(stdout);

  // plot coefficients if requested
  bool plot_coefficients = false;
  if (plot_coefficients) {
    printf("\toutput coefficients to file\n");
    dolfin::File permittivity_file("./benchmarks/pnp_refine_exact/output/permittivity.pvd");
    dolfin::File charges_file("./benchmarks/pnp_refine_exact/output/charges.pvd");
    dolfin::File diffusivity_file("./benchmarks/pnp_refine_exact/output/diffusivity.pvd");
    dolfin::File reaction_file("./benchmarks/pnp_refine_exact/output/reaction.pvd");
    dolfin::File valency_file("./benchmarks/pnp_refine_exact/output/valency.pvd");
    permittivity_file << permittivity;
    charges_file << charges;
    diffusivity_file << diffusivity[1];
    diffusivity_file << diffusivity[2];
    reaction_file << reaction[1];
    reaction_file << reaction[2];
    valency_file << valency[1];
    valency_file << valency[2];
  }

  return pnp_problem.get_solution();
}

void print_error (
  dolfin::Function computed_solution
) {
  printf("Measuring error of computed solution wrt interpolant\n"); fflush(stdout);
  auto computed_solution_ptr = make_shared<dolfin::Function>(computed_solution);
  computed_solution_ptr.reset(new dolfin::Function(computed_solution.function_space()));
  *computed_solution_ptr = computed_solution;

  Exact_Solution exact_expr;
  std::shared_ptr<dolfin::Function> exact_solution_ptr;
  exact_solution_ptr.reset(new dolfin::Function(computed_solution.function_space()));
  exact_solution_ptr->interpolate(exact_expr);

  Error error(exact_solution_ptr);
  dolfin::File error_file("./benchmarks/pnp_refine_exact/output/error.pvd");
  dolfin::Function error_function(computed_solution.function_space());
  error_function = error.compute_error(computed_solution_ptr);
  error_file << error_function;

  double l2_error = error.compute_l2_error(computed_solution_ptr);
  double h1_error = error.compute_h1_error(computed_solution_ptr);
  printf("\tL2 error: %e\n", l2_error);
  printf("\tH1 error: %e\n\n", h1_error);
}