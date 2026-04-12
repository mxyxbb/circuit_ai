#pragma once
#include "components/base_component.h"

class IdealSwitch : public BaseComponent {
public:
    IdealSwitch(std::string name, int drain, int source, int gate, int gateRef)
        : name_(std::move(name)), nd_(drain), ns_(source), ng_(gate), ngr_(gateRef), isOn_(false) {}

    std::string name() const override { return name_; }
    int maxNode() const override { return std::max({nd_, ns_, ng_, ngr_}); }

    void stamp(MNASolver& solver, double, double, size_t) override {
        double r = isOn_ ? R_ON : R_OFF;
        solver.stampConductance(nd_, ns_, 1.0 / r);
    }

    // State driven by gate voltage (no iterative convergence needed)
    bool updateState(const Eigen::VectorXd& x, size_t) override {
        double vg  = (ng_  > 0) ? x(ng_  - 1) : 0.0;
        double vgr = (ngr_ > 0) ? x(ngr_ - 1) : 0.0;
        double vgs = vg - vgr;

        bool newState = (vgs > V_GATE_THRESHOLD);
        if (newState != isOn_) {
            isOn_ = newState;
            return true;
        }
        return false;
    }

private:
    std::string name_;
    int nd_, ns_, ng_, ngr_;
    bool isOn_;
    static constexpr double R_ON  = 1e-3;
    static constexpr double R_OFF = 1e9;
    static constexpr double V_GATE_THRESHOLD = 0.5;
};
