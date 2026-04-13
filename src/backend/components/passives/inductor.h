#pragma once
#include "components/base_component.h"

class Inductor : public BaseComponent {
public:
    Inductor(std::string name, int np, int nn, double l)
        : name_(std::move(name)), np_(np), nn_(nn), l_(l), iPrev_(0.0) {}

    std::string name() const override { return name_; }
    size_t extraVariableCount() const override { return 1; }
    int maxNode() const override { return std::max(np_, nn_); }

    // Backward Euler: V_L = L/dt * (i_n - i_{n-1})
    // MNA extra row: Vnp - Vnn - (L/dt)*i_n = -(L/dt)*i_{n-1}
    void stamp(MNASolver& solver, double dt, double, size_t extraOff) override {
        double rEq = l_ / dt;
        solver.stampB(np_, extraOff, 1.0);
        solver.stampB(nn_, extraOff, -1.0);
        solver.stampC(extraOff, np_, 1.0);
        solver.stampC(extraOff, nn_, -1.0);
        solver.stampD(extraOff, -rEq);
        solver.stampExtraRhs(extraOff, -rEq * iPrev_);
    }

    void commitHistory(const Eigen::VectorXd& x, size_t extraOff) override {
        iPrev_ = x(extraOff);
    }

    double getBranchCurrent(const Eigen::VectorXd& x, size_t extraOff) const override {
        return x(extraOff);
    }

    void reset() override { iPrev_ = 0.0; }

private:
    std::string name_;
    int np_, nn_;
    double l_;
    double iPrev_;
};
