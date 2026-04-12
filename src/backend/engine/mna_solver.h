#pragma once
#include <Eigen/Dense>
#include <cstddef>

// MNA Solver: builds and solves [A][x] = [b]
// Matrix layout:
//   Rows/cols 0..n-1   : node voltages (node 1 = index 0, node n = index n-1)
//   Rows/cols n..n+m-1 : extra variables (voltage source currents, inductor currents, etc.)
//
// Convention: extraOff in stamp methods is an ABSOLUTE row/col index.
// The Simulator computes absOff = nodeCount + relativeOffset before calling stamp.

class MNASolver {
public:
    void init(size_t n, size_t m);
    void clear();

    // Conductance between nodes i, j (1-based; 0 = GND, no stamp)
    void stampConductance(int i, int j, double g);

    // B block: A[node-1][absExtra] += val
    void stampB(int node, size_t absExtra, double val);
    // C block: A[absExtra][node-1] += val
    void stampC(size_t absExtra, int node, double val);
    // D block (diagonal): A[absExtra][absExtra] += val
    void stampD(size_t absExtra, double val);
    // D block (off-diagonal): A[absRow][absCol] += val
    void stampExtraCross(size_t absRow, size_t absCol, double val);

    // RHS: b[node-1] += val (node 1-based; 0 = no stamp)
    void stampRhs(int node, double val);
    // RHS for extra variable: b[absExtra] += val
    void stampExtraRhs(size_t absExtra, double val);

    const Eigen::VectorXd& solve();

    double getNodeVoltage(int node) const;     // 1-based
    double getExtraCurrent(size_t absExtra) const; // absolute index
    size_t matrixSize() const { return n_ + m_; }
    size_t nodeCount() const { return n_; }
    const Eigen::MatrixXd& getA() const { return A_; }
    const Eigen::VectorXd& getB() const { return b_; }

private:
    size_t n_ = 0, m_ = 0;
    Eigen::MatrixXd A_;
    Eigen::VectorXd b_;
    Eigen::VectorXd x_;
};
