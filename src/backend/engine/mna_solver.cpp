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

const Eigen::VectorXd& MNASolver::solve() {
    x_ = A_.partialPivLu().solve(b_);
    return x_;
}

double MNASolver::getNodeVoltage(int node) const {
    if (node <= 0) return 0.0; // GND
    return x_(node - 1);
}

double MNASolver::getExtraCurrent(size_t absExtra) const {
    return x_(absExtra);
}
