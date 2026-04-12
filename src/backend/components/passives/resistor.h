#pragma once
#include "components/base_component.h"

class Resistor : public BaseComponent {
public:
    Resistor(std::string name, int np, int nn, double r)
        : name_(std::move(name)), np_(np), nn_(nn), r_(r) {}

    std::string name() const override { return name_; }
    int maxNode() const override { return std::max(np_, nn_); }

    void stamp(MNASolver& solver, double, double, size_t) override {
        solver.stampConductance(np_, nn_, 1.0 / r_);
    }

    double getBranchCurrent(const Eigen::VectorXd& x, size_t) const override {
        double vnp = (np_ > 0) ? x(np_ - 1) : 0.0;
        double vnn = (nn_ > 0) ? x(nn_ - 1) : 0.0;
        return (vnp - vnn) / r_;
    }

private:
    std::string name_;
    int np_, nn_;
    double r_;
};
