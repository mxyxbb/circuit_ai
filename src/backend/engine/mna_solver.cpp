#include "engine/mna_solver.h"
#include <cassert>

void MNASolver::init(size_t n, size_t m) {
    n_ = n;
    m_ = m;
    size_t sz = n + m;
    A_.setZero(sz, sz);
    b_.setZero(sz);
    x_.setZero(sz);
}

void MNASolver::clear() {
    A_.setZero();
    b_.setZero();
}

void MNASolver::stampConductance(int i, int j, double g) {
    if (i > 0) A_(i - 1, i - 1) += g;
    if (j > 0) A_(j - 1, j - 1) += g;
    if (i > 0 && j > 0) {
        A_(i - 1, j - 1) -= g;
        A_(j - 1, i - 1) -= g;
    }
}

void MNASolver::stampB(int node, size_t absExtra, double val) {
    if (node <= 0) return; // GND has no matrix row
    A_(node - 1, absExtra) += val;
}

void MNASolver::stampC(size_t absExtra, int node, double val) {
    if (node <= 0) return; // GND has no matrix column
    A_(absExtra, node - 1) += val;
}

void MNASolver::stampD(size_t absExtra, double val) {
    A_(absExtra, absExtra) += val;
}

void MNASolver::stampExtraCross(size_t absRow, size_t absCol, double val) {
    A_(absRow, absCol) += val;
}

void MNASolver::stampRhs(int node, double val) {
    if (node > 0) b_(node - 1) += val;
}

void MNASolver::stampExtraRhs(size_t absExtra, double val) {
    b_(absExtra) += val;
}

void MNASolver::applyGmin(double gmin) {
    for (size_t i = 0; i < n_; ++i)
        A_(i, i) += gmin;
}

const Eigen::VectorXd& MNASolver::solve() {
    // Jacobi (diagonal) preconditioning: transform A*x = b into
    // (D*A*D)*y = D*b and recover x = D*y, where D[i] = 1/sqrt(|A[i,i]|).
    // This maps all diagonal entries to 1.0, reducing the condition number
    // from ~1e15 (caused by capacitor gEq >> switch G_off at small dt) to O(1).
    size_t sz = n_ + m_;
    Eigen::VectorXd scale(sz);
    for (size_t i = 0; i < sz; ++i) {
        double d = std::abs(A_(i, i));
        scale(i) = (d > 1e-20) ? 1.0 / std::sqrt(d) : 1.0;
    }
    // Evaluate in two steps to avoid Eigen expression-template issues.
    Eigen::MatrixXd As = (scale.asDiagonal() * A_).eval() * scale.asDiagonal();
    Eigen::VectorXd bs = scale.cwiseProduct(b_);
    x_ = scale.cwiseProduct(As.partialPivLu().solve(bs));
    return x_;
}

double MNASolver::getNodeVoltage(int node) const {
    if (node <= 0) return 0.0; // GND
    return x_(node - 1);
}

double MNASolver::getExtraCurrent(size_t absExtra) const {
    return x_(absExtra);
}
