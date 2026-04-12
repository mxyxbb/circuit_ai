#pragma once
#include "components/base_component.h"

class VoltageSource : public BaseComponent {
public:
    VoltageSource(std::string name, int np, int nn, double voltage)
        : name_(std::move(name)), np_(np), nn_(nn), v_(voltage) {}

    std::string name() const override { return name_; }
    size_t extraVariableCount() const override { return 1; }
    int maxNode() const override { return std::max(np_, nn_); }

    void stamp(MNASolver& solver, double, double t, size_t extraOff) override {
        double v = voltageAt(t);
        solver.stampB(np_, extraOff, 1.0);
        solver.stampB(nn_, extraOff, -1.0);
        solver.stampC(extraOff, np_, 1.0);
        solver.stampC(extraOff, nn_, -1.0);
        solver.stampExtraRhs(extraOff, v);
    }

    double getBranchCurrent(const Eigen::VectorXd& x, size_t extraOff) const override {
        return x(extraOff);
    }

protected:
    virtual double voltageAt(double t) const { (void)t; return v_; }

    std::string name_;
    int np_, nn_;
    double v_;
};
