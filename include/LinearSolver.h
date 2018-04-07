/*------------------------------------------------------------------------*/
/*  Copyright 2014 Sandia Corporation.                                    */
/*  This software is released under the license detailed                  */
/*  in the file, LICENSE, which is located in the top-level Nalu          */
/*  directory structure                                                   */
/*------------------------------------------------------------------------*/


#ifndef LinearSolver_h
#define LinearSolver_h

#include <LinearSolverTypes.h>
#include <LinearSolverConfig.h>

#include <LinearSolverTypes.h>

#include <Kokkos_DefaultNode.hpp>
#include <Tpetra_Vector.hpp>
#include <Tpetra_DefaultPlatform.hpp>
#include <Tpetra_CrsMatrix.hpp>
#include <Teuchos_GlobalMPISession.hpp>
#include <Teuchos_oblackholestream.hpp>

#include <Ifpack2_Factory.hpp>

// Header files defining default types for template parameters.
// These headers must be included after other MueLu/Xpetra headers.
typedef double                                                        Scalar;
typedef long                                                          GlobalOrdinal;
typedef int                                                           LocalOrdinal;
typedef Tpetra::DefaultPlatform::DefaultPlatformType                  Platform;
typedef Tpetra::Map<LocalOrdinal, GlobalOrdinal>::node_type           Node;
typedef Teuchos::ScalarTraits<Scalar> STS;

// MueLu main header: include most common header files in one line
#include <MueLu.hpp>

#include <MueLu_TrilinosSmoother.hpp> //TODO: remove
#include <MueLu_TpetraOperator.hpp>

#include <MueLu_UseShortNames.hpp>    // => typedef MueLu::FooClass<Scalar, LocalOrdinal, ...> Foo

namespace sierra{
namespace nalu{

/** Type of solvers available in Nalu simulation **/
  enum PetraType {
    PT_TPETRA,       //!< Nalu Tpetra interface
    PT_HYPRE,        //!< Direct HYPRE interface
    PT_END
  };


class LinearSolvers;
class Simulation;

class CSGraph {
public:
  static const LocalOrdinal INVALID = std::numeric_limits<LocalOrdinal>::max();

  CSGraph(const Kokkos::View<size_t*,sierra::nalu::DeviceSpace>& rowLengths)
  : rowPointers(Teuchos::arcp<size_t>(rowLengths.size()+1)),
    colIndices()
  {
    size_t nnz = compute_row_pointers(rowPointers, rowLengths);
    colIndices = Teuchos::arcp<LocalOrdinal>(nnz);
    for(int i=0, iend=colIndices.size(); i<iend; ++i) { colIndices[i] = INVALID; }
  }

  size_t get_row_length(size_t localRow) const { return rowPointers[localRow+1]-rowPointers[localRow]; }

  void insertIndices(size_t localRow, size_t numInds, const LocalOrdinal* inds, int numDof)
  {
    LocalOrdinal* row = &colIndices[rowPointers[localRow]];
    size_t rowLen = rowPointers[localRow+1] - rowPointers[localRow];
    LocalOrdinal* rowEnd = find_first_invalid(row, rowLen);
    for(size_t i=0; i<numInds; ++i) {
      LocalOrdinal* insertPoint = std::lower_bound(row, rowEnd, inds[i]);
      if (insertPoint <= rowEnd && *insertPoint != inds[i]) {
        insert(inds[i], numDof, insertPoint, rowEnd+numDof);
        rowEnd += numDof;
      }
    }
  }

  static size_t compute_row_pointers(Teuchos::ArrayRCP<size_t>& rowPtrs,
                                   const Kokkos::View<size_t*,sierra::nalu::DeviceSpace>& rowLengths)
  {
    size_t nnz = 0;
    for(unsigned i=0; i<rowLengths.size(); ++i) {
      rowPtrs[i] = nnz;
      nnz += rowLengths(i);
    }
    rowPtrs[rowLengths.size()] = nnz;
    return nnz;
  }

  Teuchos::ArrayRCP<size_t> rowPointers;
  Teuchos::ArrayRCP<LocalOrdinal> colIndices;

private:

  void insert(LocalOrdinal ind, int numDof, LocalOrdinal* insertPoint, LocalOrdinal* rowEnd)
  {
    for(LocalOrdinal* ptr = rowEnd-1; ptr!= insertPoint; --ptr) {
//      if (*(ptr-numDof) != INVALID) {
        *ptr = *(ptr-numDof);
//      }
    }
    for(int i=0; i<numDof; ++i) {
      *insertPoint++ = ind+i;
    }
  }

  LocalOrdinal* find_first_invalid(LocalOrdinal* rowBegin, size_t rowLen)
  {
    for(size_t i=0; i<rowLen; ++i) {
      if (rowBegin[i] == INVALID) {
        return rowBegin+i;
      } 
    }
    return rowBegin+rowLen;
  }
};

/** An abstract representation of a linear solver in Nalu
 *
 *  Defines the basic API supported by the linear solvers for use within Nalu.
 *  See concrete implementations such as sierra::nalu::TpetraLinearSolver for
 *  more details.
 */
class LinearSolver
{
  public:
    LinearSolver(
      std::string name,
      LinearSolvers* linearSolvers,
      LinearSolverConfig* config)
      : name_(name),
        linearSolvers_(linearSolvers),
        config_(config),
        recomputePreconditioner_(config->recomputePreconditioner()),
        reusePreconditioner_(config->reusePreconditioner()),
        timerPrecond_(0.0)
    {}
    virtual ~LinearSolver() {}

  //! User-friendly identifier for this particular solver instance
    std::string name_;

  //! Type of solver instance as defined in sierra::nalu::PetraType
    virtual PetraType getType() = 0;

  /** Utility method to cleanup solvers during simulation
   */
    virtual void destroyLinearSolver() = 0;

    Simulation* root();
    LinearSolvers* parent();
    LinearSolvers* linearSolvers_;

  protected:
  LinearSolverConfig* config_;
  bool recomputePreconditioner_;
  bool reusePreconditioner_;
  double timerPrecond_;
  bool activateMueLu_{false};

  public:
  //! Flag indicating whether the preconditioner is recomputed on each invocation
  bool & recomputePreconditioner() {return recomputePreconditioner_;}
  //! Flag indicating whether the preconditioner is reused on each invocation
  bool & reusePreconditioner() {return reusePreconditioner_;}

  //! Reset the preconditioner timer to 0.0 for future accumulation
  void zero_timer_precond() { timerPrecond_ = 0.0;}

  //! Get the preconditioner timer for the last invocation
  double get_timer_precond() { return timerPrecond_;}

  //! Flag indicating whether the user has activated MueLU
  bool& activeMueLu() { return activateMueLu_; }

  //! Get the solver configuration specified in the input file
  LinearSolverConfig* getConfig() { return config_; }
};

class TpetraLinearSolver : public LinearSolver
{
  public:

  /**
   *  @param[in] solverName The name of the solver
   *  @param[in] config Solver configuration
   */
  TpetraLinearSolver(
    std::string solverName,
    TpetraLinearSolverConfig *config,
    const Teuchos::RCP<Teuchos::ParameterList> params,
    const Teuchos::RCP<Teuchos::ParameterList> paramsPrecond,
    LinearSolvers *linearSolvers);
  virtual ~TpetraLinearSolver() ;
  
    void setSystemObjects(
      Teuchos::RCP<LinSys::Matrix> matrix,
      Teuchos::RCP<LinSys::Vector> rhs);

    void setupLinearSolver(
      Teuchos::RCP<LinSys::Vector> sln,
      Teuchos::RCP<LinSys::Matrix> matrix,
      Teuchos::RCP<LinSys::Vector> rhs,
      Teuchos::RCP<LinSys::MultiVector> coords);

    virtual void destroyLinearSolver() override;

  //! Initialize the MueLU preconditioner before solve
    void setMueLu();

  /** Compute the norm of the non-linear solution vector
   *
   *  @param[in] whichNorm [0, 1, 2] norm to be computed
   *  @param[in] sln The solution vector
   *  @param[out] norm The norm of the solution vector
   */
    int residual_norm(int whichNorm, Teuchos::RCP<LinSys::Vector> sln, double& norm);

  /** Solve the linear system Ax = b
   *
   *  @param[out] sln The solution vector
   *  @param[out] iterationCount The number of linear solver iterations to convergence
   *  @param[out] scaledResidual The final residual norm
   *  @param[in]  isFinalOuterIter Is this the final outer iteration
   */
    int solve(
      Teuchos::RCP<LinSys::Vector> sln,
      int & iterationCount,
      double & scaledResidual,
      bool isFinalOuterIter);

    virtual PetraType getType() override { return PT_TPETRA; }

  private:
  //! The solver parameters
    const Teuchos::RCP<Teuchos::ParameterList> params_;

  //! The preconditioner parameters
    const Teuchos::RCP<Teuchos::ParameterList> paramsPrecond_;
    Teuchos::RCP<LinSys::Matrix> matrix_;
    Teuchos::RCP<LinSys::Vector> rhs_;
    Teuchos::RCP<LinSys::LinearProblem> problem_;
    Teuchos::RCP<LinSys::SolverManager> solver_;
    Teuchos::RCP<LinSys::Preconditioner> preconditioner_;
    Teuchos::RCP<MueLu::TpetraOperator<SC,LO,GO,NO> > mueluPreconditioner_;
    Teuchos::RCP<LinSys::MultiVector> coords_;

    std::string preconditionerType_;
};

} // namespace nalu
} // namespace Sierra

#endif
