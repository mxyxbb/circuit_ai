#pragma once
#include "components/base_component.h"

// Voltage-controlled voltage source (SPICE 'E'):
//   E<name> N+ N- NC+ NC- <gain>
//   V(N+,N-) = gain * V(NC+,NC-)
// One extra variable carries the output branch current (same convention as
// VoltageSource: positive current flows N+ -> N- through the source), so
// I(<name>) probes work directly.
class VCVS : public BaseComponent {
public:
    VCVS(std::string name, int np, int nn, int ncp, int ncn, double gain)
        : name_(std::move(name)), np_(np), nn_(nn), ncp_(ncp), ncn_(ncn), gain_(gain) {}

    std::string name() const override { return name_; }
    size_t extraVariableCount() const override { return 1; }
    int maxNode() const override {
        return std::max(std::max(np_, nn_), std::max(ncp_, ncn_));
    }

    void stamp(MNASolver& solver, double, double, size_t extraOff) override {
        solver.stampB(np_, extraOff, 1.0);
        solver.stampB(nn_, extraOff, -1.0);
        // Constraint row: V(np) - V(nn) - gain*(V(ncp) - V(ncn)) = 0
        solver.stampC(extraOff, np_, 1.0);
        solver.stampC(extraOff, nn_, -1.0);
        solver.stampC(extraOff, ncp_, -gain_);
        solver.stampC(extraOff, ncn_, gain_);
    }

    double getBranchCurrent(const Eigen::VectorXd& x, size_t extraOff) const override {
        return x(extraOff);
    }

private:
    std::string name_;
    int np_, nn_, ncp_, ncn_;
    double gain_;
};
