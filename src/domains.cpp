/*! \file domains.cpp
 *
 * \brief Mesh related functions
 */

#include <iostream>
#include <fstream>
#include <string.h>
#include <dolfin.h>
#include "newton.h"
#include "gradient_recovery.h"
#include "poisson_cell_marker.h"
#include "electric_cell_marker.h"
#include "fasp_to_fenics.h"
extern "C"
{
#include "fasp.h"
#include "fasp_const.h"
}

using namespace std;
using namespace dolfin;

/*---------------------------------*/
/*--      Public Functions       --*/
/*---------------------------------*/

/**
 * \fn REAL newton_compute_residual (dvector *residual)
 *
 * \brief Compute the L2 norm of the residual vector
 *
 * \param domain_par  parameters describing mesh
 * \param mesh      mesh to be constructed
 * \param subdomains  mesh subdomains to be constructed
 * \param surfaces    mesh surfaces to be constructed
 */
void domain_build (domain_param *domain_par,
                   std::shared_ptr<dolfin::Mesh>& mesh,
                   dolfin::MeshFunction<size_t> *subdomains,
                   dolfin::MeshFunction<size_t> *surfaces)
{
  // no mesh provided: use length and grid parameters
  if ( strcmp(domain_par->mesh_file,"none")==0 ) {
      fflush(stdout);

        // mesh
      dolfin::Point p0( -domain_par->length_x/2, -domain_par->length_y/2, -domain_par->length_z/2);
      dolfin::Point p1(  domain_par->length_x/2,  domain_par->length_y/2,  domain_par->length_z/2);
      // dolfin::BoxMesh box_mesh(p0, p1, domain_par->grid_x, domain_par->grid_y, domain_par->grid_z);
      auto box_mesh = std::make_shared<dolfin::BoxMesh>(p0, p1, domain_par->grid_x, domain_par->grid_y, domain_par->grid_z);
      (mesh) = (box_mesh);


      // subdomains
      dolfin::CellFunction<std::size_t> subdomains_object(box_mesh);
      subdomains_object.set_all(1);
      *subdomains = subdomains_object;

      // surfaces
      dolfin::FacetFunction<std::size_t> surfaces_object(box_mesh);
      surfaces_object.set_all(1);
      *surfaces = surfaces_object;

    }
    else { // read in mesh from specified files
      printf("### ERROR: Reading in meshes is currently unsupported: %s...\n\n", domain_par->mesh_file);
    }
   /* else { // read in mesh from specified files
      printf(" Reading in the mesh from %s \n", domain_par->mesh_file);
      dolfin::Mesh read_mesh(domain_par->mesh_file);
      *mesh = read_mesh;

      printf(" Reading in the mesh subdomains from %s \n", domain_par->subdomain_file);
      dolfin::MeshFunction<std::size_t> subdomains_object(*mesh, domain_par->subdomain_file);
      *subdomains = subdomains_object;

      printf(" Reading in the mesh surfaces from %s \n", domain_par->surface_file);
      dolfin::MeshFunction<std::size_t>  surfaces_object(*mesh, domain_par->surface_file);
      *surfaces = surfaces_object;
    }*/
}

/**
 * \fn bool check_local_entropy (dolfin::Function *cation,
 *                               dolfin::Function *anion,
 *                               dolfin::Function *voltage,
 *                               dolfin::Mesh *target_mesh,
 *                               double entropy_tol)
 *
 * \brief Check if local entropy is below tolerance and refine
 *    mesh
 *
 * \param cation          cation function
 * \param cation_valency  valency of cation
 * \param anion           anion function
 * \param anion_valency  valency of anion
 * \param voltage         voltage function
 * \param mesh            ptr to refined mesh
 * \param entropy_tol     tolerance for local entropy
 *
 * \return            levels of refinement
 */
unsigned int check_local_entropy (std::shared_ptr<dolfin::Function> cation,
                                  double cation_valency,
                                  std::shared_ptr<dolfin::Function> anion,
                                  double anion_valency,
                                  std::shared_ptr<dolfin::Function> voltage,
                                  std::shared_ptr<dolfin::Mesh>& target_mesh,
                                  double entropy_tol,
                                  int max_cells)
{

  // compute mesh from input voltage function and transfer
  auto mesh = std::make_shared<dolfin::Mesh>(*(voltage->function_space()->mesh()));

  if ((max_cells>0) && (mesh->num_cells()>max_cells)){
    printf("\t\tThe mesh already has too many cells. No adaptivity\n");
    return 0;
  }


  auto gradient_space = std::make_shared<gradient_recovery::FunctionSpace>(mesh);
  gradient_recovery::BilinearForm a(gradient_space,gradient_space);
  gradient_recovery::LinearForm L(gradient_space);
  auto cation_entropy = std::make_shared<Function>(gradient_space);
  auto anion_entropy = std::make_shared<Function>(gradient_space);

  // compute entropic potentials
  auto cation_potential = std::make_shared<Function>( (voltage->function_space()) );
  cation_potential->interpolate(*voltage);
  *(cation_potential->vector()) *= cation_valency;
  *(cation_potential->vector()) += *(cation->vector());
  auto anion_potential = std::make_shared<Function>( (voltage->function_space()) );
  anion_potential->interpolate(*voltage);
  *(anion_potential->vector()) *= anion_valency;
  *(anion_potential->vector()) += *(anion->vector());

  // setup matrix and rhs
  EigenMatrix A;
  assemble(A,a);
  dCSRmat A_fasp;
  EigenMatrix_to_dCSRmat(&A,&A_fasp);
  dBSRmat adaptA_fasp_bsr = fasp_format_dcsr_dbsr(&A_fasp, mesh->topology().dim());
  EigenVector b;
  dvector b_fasp, solu_fasp;

  // Setup FASP solver
  input_param inpar;
  itsolver_param itpar;
  AMG_param amgpar;
  ILU_param ilupar;
  char fasp_param_file[] = "./src/gradient_recovery_bsr.dat";
  fasp_param_input(fasp_param_file, &inpar);
  fasp_param_init(&inpar, &itpar, &amgpar, &ilupar, NULL);
  INT status = FASP_SUCCESS;

  // set form for cation
  L.potential = cation_potential;
  assemble(b,L);
  EigenVector_to_dvector(&b,&b_fasp);
  fasp_dvec_alloc(b.size(), &solu_fasp);

  // solve for cation entropy
  fasp_dvec_set(b_fasp.row, &solu_fasp, 0.0);
  status = fasp_solver_dbsr_krylov_diag(&adaptA_fasp_bsr, &b_fasp, &solu_fasp, &itpar);
  copy_dvector_to_Function(&solu_fasp, cation_entropy.get());

  // set form for anion
  L.potential = anion_potential;
  assemble(b,L);
  EigenVector_to_dvector(&b,&b_fasp);

  // solve for anion entropy
  fasp_dvec_set(b_fasp.row, &solu_fasp, 0.0);
  status = fasp_solver_dbsr_krylov_diag(&adaptA_fasp_bsr, &b_fasp, &solu_fasp, &itpar);
  copy_dvector_to_Function(&solu_fasp, anion_entropy.get());

  // output entropy
  // File entropyFile("./benchmarks/PNP/output/entropy.pvd");
  // entropyFile << cation_entropy;
  // entropyFile << anion_entropy;

  // compute entropic error
  auto DG = std::make_shared<poisson_cell_marker::FunctionSpace>(mesh);
  poisson_cell_marker::LinearForm error_form(DG);
  error_form.cat_entr = cation_entropy;
  error_form.cat_pot  = cation_potential;
  error_form.an_entr = anion_entropy;
  error_form.an_pot  = anion_potential;
  dolfin::EigenVector error_vector;
  assemble(error_vector, error_form);

  // mark for refinement
  MeshFunction<bool> cell_marker(mesh, mesh->topology().dim(), false);
  unsigned int marked_elem_count = 0;
  for ( uint errVecInd = 0; errVecInd < error_vector.size(); errVecInd++) {
    if ( error_vector[errVecInd] > entropy_tol ) {
        marked_elem_count++;
        cell_marker.values()[errVecInd] = true;
    }
  }
  // File marked_elem_file("./benchmarks/PNP/output/marker.pvd");
  // marked_elem_file << cell_marker;

  // check for necessary refiments
  if ( marked_elem_count == 0 ) {
    target_mesh = mesh;
    return 0;
  }
  else {
    // adapt mesh and function space
    std::shared_ptr<const Mesh> mesh_ptr( new const Mesh(refine(*mesh, cell_marker)) );
    // dolfin::FunctionSpace adapt_function_space( adapt(*(voltage->function_space()), mesh_ptr) );
    std::shared_ptr<dolfin::FunctionSpace> adapt_function_space =  adapt(*(voltage->function_space()), mesh_ptr );

    // adapt(*mesh, cell_markers);


    // adapt functions
    auto adapt_cation = std::make_shared<dolfin::Function>(adapt_function_space);
    auto adapt_anion = std::make_shared<dolfin::Function>(adapt_function_space);
    auto adapt_voltage = std::make_shared<dolfin::Function>(adapt_function_space);
    adapt_cation->interpolate(*cation);
    adapt_anion->interpolate(*anion);
    adapt_voltage->interpolate(*voltage);

    unsigned int num_refines = 0;
    num_refines = check_local_entropy(
      adapt_cation,
      cation_valency,
      adapt_anion,
      anion_valency,
      adapt_voltage,
      target_mesh,
      entropy_tol,
      max_cells
    );
    return 1+num_refines;
  }
}

unsigned int check_local_entropy (std::shared_ptr<dolfin::Function> cation,
                                 double cation_valency,
                                 std::shared_ptr<dolfin::Function> anion,
                                 double anion_valency,
                                 std::shared_ptr<dolfin::Function> voltage,
                                 std::shared_ptr<dolfin::Mesh>& target_mesh,
                                 double entropy_tol)
{
  int max_cells = -1;
  unsigned int c = check_local_entropy (cation,
                                        cation_valency,
                                        anion,
                                        anion_valency,
                                        voltage,
                                        target_mesh,entropy_tol,
                                        max_cells);
  return c;
}

/**
 * \fn bool check_electric_field (dolfin::Function *voltage,
 *                               dolfin::Mesh *target_mesh,
 *                               double entropy_tol)
 *
 * \brief Check if the electric field gradient(potential) is below tolerance and refine
 *    mesh
 *
 * \param voltage         voltage function
 * \param mesh            ptr to refined mesh
 * \param entropy_tol     tolerance for local entropy
 *
 * \return            levels of refinement
 */
unsigned int check_electric_field (std::shared_ptr<dolfin::Function> voltage,
                                  std::shared_ptr<dolfin::Mesh> & target_mesh,
                                  double entropy_tol,
                                  int max_cells)
{

  // compute mesh from input voltage function and transfer
  auto mesh = std::make_shared<dolfin::Mesh>(*(voltage->function_space()->mesh()));

  if ((max_cells>0) && (mesh->num_cells()>max_cells)){
    printf("\t\tThe mesh already has too many cells. No adaptivity\n");
    return 0;
  }


  auto gradient_space = std::make_shared<gradient_recovery::FunctionSpace>(mesh);
  gradient_recovery::BilinearForm a(gradient_space,gradient_space);
  gradient_recovery::LinearForm L(gradient_space);
  auto ElecField = std::make_shared<Function>(gradient_space);

  // compute entropic potentials
  auto potential = std::make_shared<Function>((voltage->function_space()));
  potential->interpolate(*voltage);

  // setup matrix and rhs
  EigenMatrix A;
  assemble(A,a);
  dCSRmat A_fasp;
  EigenMatrix_to_dCSRmat(&A,&A_fasp);
  EigenVector b;
  dvector b_fasp, solu_fasp;

  // Setup FASP solver
  input_param inpar;
  itsolver_param itpar;
  AMG_param amgpar;
  ILU_param ilupar;
  char fasp_param_file[] = "./src/gradient_recovery_bsr.dat";
  fasp_param_input(fasp_param_file, &inpar);
  fasp_param_init(&inpar, &itpar, &amgpar, &ilupar, NULL);
  INT status = FASP_SUCCESS;

  // set form for cation
  L.potential = potential;
  assemble(b,L);
  EigenVector_to_dvector(&b,&b_fasp);
  fasp_dvec_alloc(b.size(), &solu_fasp);

  // solve for cation entropy
  fasp_dvec_set(b_fasp.row, &solu_fasp, 0.0);
  status = fasp_solver_dcsr_krylov_diag(&A_fasp, &b_fasp, &solu_fasp, &itpar);
  copy_dvector_to_Function(&solu_fasp, ElecField.get());

  // Free memory
  fasp_dvec_free(&solu_fasp);

  // compute entropic error
  auto DG = std::make_shared<electric_cell_marker::FunctionSpace>(mesh);
  electric_cell_marker::LinearForm error_form(DG);
  error_form.pot = potential;
  error_form.gradpot  = ElecField;
  dolfin::EigenVector error_vector;
  assemble(error_vector, error_form);

  // mark for refinement
  MeshFunction<bool> cell_marker(mesh, mesh->topology().dim(), false);
  unsigned int marked_elem_count = 0;
  for ( uint errVecInd = 0; errVecInd < error_vector.size(); errVecInd++) {
    if ( error_vector[errVecInd] > entropy_tol/2.0 ) {
        marked_elem_count++;
        cell_marker.values()[errVecInd] = true;
    }
  }
  // File marked_elem_file("./benchmarks/PNP/output/marker.pvd");
  // marked_elem_file << cell_marker;

  // check for necessary refiments
  if ( marked_elem_count == 0 ) {
    target_mesh = mesh;
    return 0;
  }
  else {
    // adapt mesh and function space
    std::shared_ptr<const Mesh> mesh_ptr( new const Mesh(refine(*mesh, cell_marker)) );
    // auto adapt_function_space = std::make_shared<dolfin::FunctionSpace> ( adapt(*(voltage->function_space()), mesh_ptr) );
    std::shared_ptr<dolfin::FunctionSpace> adapt_function_space =  adapt(*(voltage->function_space()), mesh_ptr );

    // adapt functions
    auto adapt_voltage = std::make_shared<dolfin::Function>(adapt_function_space);
    adapt_voltage->interpolate(*voltage);

    unsigned int num_refines = 0;
    num_refines =  check_electric_field(
      adapt_voltage,
      target_mesh,
      entropy_tol,
      max_cells
    );
    return 1+num_refines;
  }
}
unsigned int check_electric_field (std::shared_ptr<dolfin::Function> voltage,
                                  std::shared_ptr<dolfin::Mesh>& target_mesh,
                                  double entropy_tol)
{
   int max_cells = -1;
   unsigned int c = check_electric_field(voltage,
                                        target_mesh,
                                        entropy_tol,
                                        max_cells);
   return c;
}

/*---------------------------------*/
/*--        End of File          --*/
/*---------------------------------*/
