// Copyright (c) 2017, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-734707. All Rights
// reserved. See files LICENSE and NOTICE for details.
//
// This file is part of CEED, a collection of benchmarks, miniapps, software
// libraries and APIs for efficient high-order finite element and spectral
// element discretizations for exascale applications. For more information and
// source code availability see http://github.com/ceed.
//
// The CEED research is supported by the Exascale Computing Project 17-SC-20-SC,
// a collaborative effort of two U.S. Department of Energy organizations (Office
// of Science and the National Nuclear Security Administration) responsible for
// the planning and preparation of a capable exascale ecosystem, including
// software, applications, hardware, advanced system engineering and early
// testbed platforms, in support of the nation's exascale computing imperative.

#ifndef MFEM_LAGHOS_ASSEMBLY
#define MFEM_LAGHOS_ASSEMBLY

#include "mfem.hpp"


#ifdef MFEM_USE_MPI

#include <memory>
#include <iostream>

namespace mfem
{

namespace hydrodynamics
{

// Container for all data needed at quadrature points.
struct QuadratureData
{
   // TODO: use QuadratureFunctions?

   // Reference to physical Jacobian for the initial mesh. These are computed
   // only at time zero and stored here.
   DenseTensor Jac0inv;

   // Quadrature data used for full/partial assembly of the force operator. At
   // each quadrature point, it combines the stress, inverse Jacobian,
   // determinant of the Jacobian and the integration weight. It must be
   // recomputed in every time step.
   DenseTensor stressJinvT;

   // Quadrature data used for full/partial assembly of the mass matrices. At
   // time zero, we compute and store (rho0 * det(J0) * qp_weight) at each
   // quadrature point. Note the at any other time, we can compute
   // rho = rho0 * det(J0) / det(J), representing the notion of pointwise mass
   // conservation.
   Vector rho0DetJ0w;

   // Initial length scale. This represents a notion of local mesh size. We
   // assume that all initial zones have similar size.
   double h0;

   // Estimate of the minimum time step over all quadrature points. This is
   // recomputed at every time step to achieve adaptive time stepping.
   double dt_est;

   QuadratureData(int dim, int nzones, int quads_per_zone)
      : Jac0inv(dim, dim, nzones * quads_per_zone),
        stressJinvT(nzones * quads_per_zone, dim, dim),
        rho0DetJ0w(nzones * quads_per_zone) { }
};

// Stores values of the one-dimensional shape functions and gradients at all 1D
// quadrature points. All sizes are (dofs1D_cnt x quads1D_cnt).
struct Tensors1D
{
   // H1 shape functions and gradients, L2 shape functions.
   DenseMatrix HQshape1D, HQgrad1D, LQshape1D;

   Tensors1D(int H1order, int L2order, int nqp1D);
};
extern const Tensors1D *tensors1D;

// This class is used only for visualization. It assembles (rho, phi) in each
// zone, which is used by LagrangianHydroOperator::ComputeDensity to do an L2
// projection of the density.
class DensityIntegrator : public LinearFormIntegrator
{
private:
   const QuadratureData &quad_data;

public:
   DensityIntegrator(QuadratureData &quad_data_) : quad_data(quad_data_) { }

   virtual void AssembleRHSElementVect(const FiniteElement &fe,
                                       ElementTransformation &Tr,
                                       Vector &elvect);
};

// Assembles element contributions to the global force matrix. This class is
// used for the full assembly case; it's not used with partial assembly.
class ForceIntegrator : public BilinearFormIntegrator
{
private:
   const QuadratureData &quad_data;

public:
   ForceIntegrator(QuadratureData &quad_data_) : quad_data(quad_data_) { }

   virtual void AssembleElementMatrix2(const FiniteElement &trial_fe,
                                       const FiniteElement &test_fe,
                                       ElementTransformation &Trans,
                                       DenseMatrix &elmat);
};

// Performs partial assembly, which corresponds to (and replaces) the use of the
// LagrangianHydroOperator::Force global matrix.
class ForcePAOperator : public Operator
{
private:
   const int dim, nzones;

   QuadratureData *quad_data;
   ParFiniteElementSpace &H1FESpace, &L2FESpace;

   // Force matrix action on quadrilateral elements in 2D
   void MultQuad(const Vector &vecL2, Vector &vecH1) const;
   // Force matrix action on hexahedral elements in 3D
   void MultHex(const Vector &vecL2, Vector &vecH1) const;

   // Transpose force matrix action on quadrilateral elements in 2D
   void MultTransposeQuad(const Vector &vecH1, Vector &vecL2) const;
   // Transpose force matrix action on hexahedral elements in 3D
   void MultTransposeHex(const Vector &vecH1, Vector &vecL2) const;

public:
   ForcePAOperator(QuadratureData *quad_data_,
                   ParFiniteElementSpace &h1fes, ParFiniteElementSpace &l2fes)
      : dim(h1fes.GetMesh()->Dimension()), nzones(h1fes.GetMesh()->GetNE()),
        quad_data(quad_data_), H1FESpace(h1fes), L2FESpace(l2fes) { }

   virtual void Mult(const Vector &vecL2, Vector &vecH1) const;
   virtual void MultTranspose(const Vector &vecH1, Vector &vecL2) const;

   ~ForcePAOperator() { }
};

// Performs partial assembly for the velocity mass matrix.
class MassPAOperator : public Operator
{
private:
   const int dim, nzones;

   QuadratureData *quad_data;
   ParFiniteElementSpace &FESpace;

   Array<int> *ess_tdofs;

   mutable ParGridFunction x_gf, y_gf;

   // Mass matrix action on quadrilateral elements in 2D
   void MultQuad(const Vector &x, Vector &y) const;
   // Mass matrix action on hexahedral elements in 3D
   void MultHex(const Vector &x, Vector &y) const;

public:
   MassPAOperator(QuadratureData *quad_data_, ParFiniteElementSpace &fes)
      : Operator(fes.TrueVSize()),
        dim(fes.GetMesh()->Dimension()), nzones(fes.GetMesh()->GetNE()),
        quad_data(quad_data_), FESpace(fes), ess_tdofs(NULL),
        x_gf(&fes), y_gf(&fes)
   { }

  void SetEssentialTrueDofs(Array<int> &dofs);

   // Mass matrix action. We work with one velocity component at a time.
   virtual void Mult(const Vector &x, Vector &y) const;

   void EliminateRHS(Vector &b);
};

class OccaMassOperator : public Operator {
private:
  occa::device device;

  int dim, elements;
  QuadratureData *quad_data;
  OccaFiniteElementSpace &fes;

  int ess_tdofs_count;
  occa::memory ess_tdofs;

  // For distributing X
  mutable OccaVector distX;
  mutable ParGridFunction x_gf, y_gf;

   // Force matrix action on quadrilateral elements in 2D
   void MultQuad(const OccaVector &x, OccaVector &y) const;
   // Force matrix action on hexahedral elements in 3D
   void MultHex(const OccaVector &x, OccaVector &y) const;

public:
  OccaMassOperator(QuadratureData *quad_data_, OccaFiniteElementSpace &fes_);
  OccaMassOperator(occa::device device_,
                   QuadratureData *quad_data_, OccaFiniteElementSpace &fes_);

  void Setup(occa::device device_, QuadratureData *quad_data_);

  void SetEssentialTrueDofs(Array<int> &dofs);

  // Can be used for both velocity and specific internal energy. For the case
  // of velocity, we only work with one component at a time.
  virtual void Mult(const OccaVector &x, OccaVector &y) const;

  void EliminateRHS(OccaVector &b);
};

// Performs partial assembly for the energy mass matrix on a single zone.
// Used to perform local CG solves, thus avoiding unnecessary communication.
class OccaForceOperator : public Operator
{
private:
  occa::device device;
  int dim, elements;

  QuadratureData *quad_data;
  OccaFiniteElementSpace &h1fes, &l2fes;

  // Force matrix action on quadrilateral elements in 2D
  void MultQuad(const OccaVector &vecL2, OccaVector &vecH1) const;
  // Force matrix action on hexahedral elements in 3D
  void MultHex(const OccaVector &vecL2, OccaVector &vecH1) const;

  // Transpose force matrix action on quadrilateral elements in 2D
  void MultTransposeQuad(const OccaVector &vecH1, OccaVector &vecL2) const;
  // Transpose force matrix action on hexahedral elements in 3D
  void MultTransposeHex(const OccaVector &vecH1, OccaVector &vecL2) const;

public:
  OccaForceOperator(QuadratureData *quad_data_,
                    OccaFiniteElementSpace &h1fes_,
                    OccaFiniteElementSpace &l2fes_);

  OccaForceOperator(occa::device device_,
                    QuadratureData *quad_data_,
                    OccaFiniteElementSpace &h1fes_,
                    OccaFiniteElementSpace &l2fes_);

  void Setup(occa::device device_,
             QuadratureData *quad_data_);

  virtual void Mult(const OccaVector &vecL2, OccaVector &vecH1) const;
  virtual void MultTranspose(const OccaVector &vecH1, OccaVector &vecL2) const;

  ~OccaForceOperator() { }
};

} // namespace hydrodynamics

} // namespace mfem

#endif // MFEM_USE_MPI

#endif // MFEM_LAGHOS_ASSEMBLY
