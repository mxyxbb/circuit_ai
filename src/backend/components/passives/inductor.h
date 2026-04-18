#pragma once
#include "components/base_component.h"

class Inductor : public BaseComponent {
public:
    Inductor(std::string name, int np, int nn, double l)
        : name_(std::move(name)), np_(np), nn_(nn), l_(l) {}

    std::string name() const override { return name_; }
    size_t extraVariableCount() const override { return 1; }
    int maxNode() const override { return std::max(np_, nn_); }
    void setUseBE(bool useBE) override { useBE_ = useBE; }

    void stamp(MNASolver& solver, double dt, double, size_t extraOff) override {
        double rEq, vHist;
        if (useBE_) {
            // Backward Euler: rEq = L/dt, history = rEq * I_prev
            rEq   = l_ / dt;
            vHist = rEq * iPrev_;
        } else {
            // Trapezoidal: rEq = 2L/dt, history = rEq * I_prev + V_prev
            rEq   = 2.0 * l_ / dt;
            vHist = rEq * iPrev_ + vPrev_;
        }
        solver.stampB(np_, extraOff,  1.0);
        solver.stampB(nn_, extraOff, -1.0);
        solver.stampC(extraOff, np_,  1.0);
        solver.stampC(extraOff, nn_, -1.0);
        solver.stampD(extraOff, -rEq);
        solver.stampExtraRhs(extraOff, -vHist);
    }

    void commitHistory(const Eigen::VectorXd& x, size_t extraOff) override {
        double vnp = (np_ > 0) ? x(np_ - 1) : 0.0;
        double vnn = (nn_ > 0) ? x(nn_ - 1) : 0.0;
        vPrev_ = vnp - vnn;
        iPrev_ = x(extraOff);
    }

    double getBranchCurrent(const Eigen::VectorXd& x, size_t extraOff) const override {
        return x(extraOff);
    }

    void reset() override { iPrev_ = 0.0; vPrev_ = 0.0; }

    void saveState() override    { savedIPrev_ = iPrev_; savedVPrev_ = vPrev_; }
    void restoreState() override { iPrev_ = savedIPrev_; vPrev_ = savedVPrev_; }

private:
    std::string name_;
    int np_, nn_;
    double l_;
    bool   useBE_ = true;   // start with BE for stability at t=0
    double iPrev_ = 0.0;
    double vPrev_ = 0.0;
    double savedIPrev_ = 0.0;
    double savedVPrev_ = 0.0;
};
