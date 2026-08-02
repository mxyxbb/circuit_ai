#pragma once
#include "components/base_component.h"

// Voltage-controlled current source (SPICE 'G'):
//   G<name> N+ N- NC+ NC- <gm>
// Injects I = gm * V(NC+,NC-) into node N+ (drawn out of N-), matching the
// arrow convention of the independent CurrentSource. The branch current is
// carried by one extra variable so I(<name>) probes work directly; this also
// avoids needing an asymmetric node-node stamp in MNASolver.
class VCCS : public BaseComponent {
public:
    VCCS(std::string name, int np, int nn, int ncp, int ncn, double gm)
        : name_(std::move(name)), np_(np), nn_(nn), ncp_(ncp), ncn_(ncn), gm_(gm) {}

    std::string name() const override { return name_; }
    size_t extraVariableCount() const override { return 1; }
    int maxNode() const override {
        return std::max(std::max(np_, nn_), std::max(ncp_, ncn_));
    }

    void stamp(MNASolver& solver, double, double, size_t extraOff) override {
        // KCL: current i enters node N+ and leaves node N-
        solver.stampB(np_, extraOff, -1.0);
        solver.stampB(nn_, extraOff, 1.0);
        // Branch equation: i - gm*(V(ncp) - V(ncn)) = 0
        solver.stampD(extraOff, 1.0);
        solver.stampC(extraOff, ncp_, -gm_);
        solver.stampC(extraOff, ncn_, gm_);
    }

    double getBranchCurrent(const Eigen::VectorXd& x, size_t extraOff) const override {
        return x(extraOff);
    }

private:
    std::string name_;
    int np_, nn_, ncp_, ncn_;
    double gm_;
};
