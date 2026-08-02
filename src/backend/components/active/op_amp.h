#pragma once
#include "components/base_component.h"

// Op-amp with finite open-loop gain and output saturation (rail clamping):
//   OP<name> IN+ IN- OUT [gain=<A>] [vmax=<V>] [vmin=<V>]
//   Linear region : V(out) = A * (V(in+) - V(in-)),  A default 1e5
//   Saturated     : V(out) = vmax (or vmin),         defaults ±15 V
// Output is a controlled voltage source referenced to GND, carried by one
// extra variable (so I(<name>) probes report the output branch current).
// Inputs are ideal high-impedance sense pins (no current flows).
//
// The rail clamp is a 3-state PWL nonlinearity handled through the same
// updateState / saveState iteration protocol as IdealDiode.
class OpAmp : public BaseComponent {
public:
    OpAmp(std::string name, int inp, int inn, int out,
          double gain, double vmax, double vmin)
        : name_(std::move(name)), inp_(inp), inn_(inn), out_(out),
          gain_(gain), vmax_(vmax), vmin_(vmin) {}

    std::string name() const override { return name_; }
    size_t extraVariableCount() const override { return 1; }
    int maxNode() const override {
        return std::max(std::max(inp_, inn_), out_);
    }

    void stamp(MNASolver& solver, double, double, size_t extraOff) override {
        solver.stampB(out_, extraOff, 1.0);
        solver.stampC(extraOff, out_, 1.0);
        if (state_ == Linear) {
            // V(out) - A*(V(in+) - V(in-)) = 0
            solver.stampC(extraOff, inp_, -gain_);
            solver.stampC(extraOff, inn_,  gain_);
        } else {
            // V(out) = rail
            solver.stampExtraRhs(extraOff, state_ == SatHi ? vmax_ : vmin_);
        }
    }

    bool updateState(const Eigen::VectorXd& x, size_t) override {
        double vp = (inp_ > 0) ? x(inp_ - 1) : 0.0;
        double vn = (inn_ > 0) ? x(inn_ - 1) : 0.0;
        double vLin = gain_ * (vp - vn);   // unclamped output the gain demands

        State next = state_;
        if (vLin > vmax_)      next = SatHi;
        else if (vLin < vmin_) next = SatLo;
        else                   next = Linear;

        if (next != state_) {
            state_ = next;
            flippedSinceSave_ = true;
            return true;
        }
        return false;
    }

    double getBranchCurrent(const Eigen::VectorXd& x, size_t extraOff) const override {
        return x(extraOff);
    }

    void reset() override { state_ = Linear; }

    void saveState() override    { savedState_ = state_; flippedSinceSave_ = false; }
    void restoreState() override { state_ = savedState_; flippedSinceSave_ = false; }
    bool stateChangedSinceLastSave() const override { return state_ != savedState_; }
    bool flippedSinceLastSave()      const override { return flippedSinceSave_; }

private:
    enum State { Linear, SatHi, SatLo };

    std::string name_;
    int inp_, inn_, out_;
    double gain_, vmax_, vmin_;
    State state_      = Linear;
    State savedState_ = Linear;
    bool  flippedSinceSave_ = false;
};
