#pragma once
#include "components/base_component.h"

class IdealSwitch : public BaseComponent {
public:
    IdealSwitch(std::string name, int drain, int source, int gate, int gateRef)
        : name_(std::move(name)), nd_(drain), ns_(source),
          ng_(gate), ngr_(gateRef), state_(OFF) {}

    std::string name() const override { return name_; }
    int maxNode() const override { return std::max({nd_, ns_, ng_, ngr_}); }

    void stamp(MNASolver& solver, double, double, size_t) override {
        double r = (state_ == ON)         ? R_ON      :
                   (state_ == BODY_DIODE) ? BODY_R_ON : R_OFF;
        solver.stampConductance(nd_, ns_, 1.0 / r);
    }

    bool updateState(const Eigen::VectorXd& x, size_t) override {
        double vgs = ((ng_  > 0) ? x(ng_  - 1) : 0.0)
                   - ((ngr_ > 0) ? x(ngr_ - 1) : 0.0);
        double vds = ((nd_ > 0) ? x(nd_ - 1) : 0.0)
                   - ((ns_ > 0) ? x(ns_ - 1) : 0.0);

        State ns = (vgs >= V_GATE_THRESHOLD) ? ON         :
                   (vds  < -V_BD_THRESHOLD)  ? BODY_DIODE : OFF;
        if (ns != state_) { state_ = ns; return true; }
        return false;
    }

    void reset() override { state_ = OFF; }

private:
    enum State { OFF, ON, BODY_DIODE };
    std::string name_;
    int nd_, ns_, ng_, ngr_;
    State state_ = OFF;

    static constexpr double R_ON             = 1e-3;
    static constexpr double BODY_R_ON        = 2e-3;   // body diode forward resistance
    static constexpr double R_OFF            = 1e9;
    static constexpr double V_GATE_THRESHOLD = 0.5;
    static constexpr double V_BD_THRESHOLD   = 1e-3;   // 1 mV hysteresis at Vds = 0
};
