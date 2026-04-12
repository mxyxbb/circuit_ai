#pragma once
#include "components/base_component.h"

class Capacitor : public BaseComponent {
public:
    Capacitor(std::string name, int np, int nn, double c)
        : name_(std::move(name)), np_(np), nn_(nn), c_(c), vPrev_(0.0) {}

    std::string name() const override { return name_; }
    int maxNode() const override { return std::max(np_, nn_); }

    // Backward Euler companion: G_eq = C/dt, I_hist = G_eq * V_prev
    void stamp(MNASolver& solver, double dt, double, size_t) override {
        double gEq = c_ / dt;
        double iHist = gEq * vPrev_;
        solver.stampConductance(np_, nn_, gEq);
        solver.stampRhs(np_, iHist);
        solver.stampRhs(nn_, -iHist);
    }

    void commitHistory(const Eigen::VectorXd& x, size_t) override {
        double vnp = (np_ > 0) ? x(np_ - 1) : 0.0;
        double vnn = (nn_ > 0) ? x(nn_ - 1) : 0.0;
        vPrev_ = vnp - vnn;
    }

private:
    std::string name_;
    int np_, nn_;
    double c_;
    double vPrev_;
};
