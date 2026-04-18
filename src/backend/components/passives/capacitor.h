#pragma once
#include "components/base_component.h"

class Capacitor : public BaseComponent {
public:
    Capacitor(std::string name, int np, int nn, double c)
        : name_(std::move(name)), np_(np), nn_(nn), c_(c) {}

    std::string name() const override { return name_; }
    int maxNode() const override { return std::max(np_, nn_); }
    void setUseBE(bool useBE) override { useBE_ = useBE; }

    void stamp(MNASolver& solver, double dt, double, size_t) override {
        if (useBE_) {
            // Backward Euler: G_eq = C/dt, I_hist = G_eq * V_prev
            gEq_   = c_ / dt;
            iHist_ = gEq_ * vPrev_;
        } else {
            // Trapezoidal: G_eq = 2C/dt, I_hist = G_eq * V_prev + I_prev
            gEq_   = 2.0 * c_ / dt;
            iHist_ = gEq_ * vPrev_ + iPrev_;
        }
        solver.stampConductance(np_, nn_, gEq_);
        solver.stampRhs(np_,  iHist_);
        solver.stampRhs(nn_, -iHist_);
    }

    void commitHistory(const Eigen::VectorXd& x, size_t) override {
        double vnp = (np_ > 0) ? x(np_ - 1) : 0.0;
        double vnn = (nn_ > 0) ? x(nn_ - 1) : 0.0;
        vPrev_ = vnp - vnn;
        iPrev_ = gEq_ * (vnp - vnn) - iHist_; // actual current this step
    }

    // i_C = G_eq * (Vnp - Vnn) - I_hist  (companion model current)
    double getBranchCurrent(const Eigen::VectorXd& x, size_t) const override {
        double vnp = (np_ > 0) ? x(np_ - 1) : 0.0;
        double vnn = (nn_ > 0) ? x(nn_ - 1) : 0.0;
        return gEq_ * (vnp - vnn) - iHist_;
    }

    void reset() override { vPrev_ = 0.0; iPrev_ = 0.0; gEq_ = 0.0; iHist_ = 0.0; }

    void saveState() override    { savedVPrev_ = vPrev_; savedIPrev_ = iPrev_; }
    void restoreState() override { vPrev_ = savedVPrev_; iPrev_ = savedIPrev_; }

private:
    std::string name_;
    int np_, nn_;
    double c_;
    bool   useBE_ = true;  // start with BE for stability at t=0
    double vPrev_ = 0.0;
    double iPrev_ = 0.0;
    double gEq_   = 0.0;
    double iHist_ = 0.0;
    double savedVPrev_ = 0.0;
    double savedIPrev_ = 0.0;
};
