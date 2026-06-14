The heat benchmark solves the steady heat equation in a regular grid of NxM
elements using an iterative solver.

The solver is either the Gauss-Seidel or Successive-over-relaxation with a given
relaxation parameter (--relax).

In every iteration the relative error of the solution is computed by using the
infinite norm until the tolerance limit is reached.

