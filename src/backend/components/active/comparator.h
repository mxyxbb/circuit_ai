#pragma once
#include "components/base_component.h"

// Ideal comparator with rail-to-rail 2-state output:
//   CMP<name> IN+ IN- OUT [vhigh=<V>] [vlow=<V>]
//   V(out) = vhigh when V(in+) > V(in-), else vlow (defaults 5 V / 0 V)
// Output is a switched voltage source referenced to GND, carried by one extra
// variable (so I(<name>) probes report the output branch current). Inputs are
// ideal high-impedance sense pins.
//
// The 2-state output is handled through the same updateState / saveState
// iteration protocol as IdealDiode. A small input hysteresis (1 uV) prevents
// state chatter when the differential input sits exactly at zero.
class Comparator : public BaseComponent {
public:
    Comparator(std::string name, int inp, int inn, int out,
               double vhigh, double vlow)
        : name_(std::move(name)), inp_(inp), inn_(inn), out_(out),
          vhigh_(vhigh), vlow_(vlow) {}

    std::string name() const override { return name_; }
    size_t extraVariableCount() const override { return 1; }
    int maxNode() const override {
        return std::max(std::max(inp_, inn_), out_);
    }

    void stamp(MNASolver& solver, double, double, size_t extraOff) override {
        solver.stampB(out_, extraOff, 1.0);
        solver.stampC(extraOff, out_, 1.0);
        solver.stampExtraRhs(extraOff, isHigh_ ? vhigh_ : vlow_);
    }

    bool updateState(const Eigen::VectorXd& x, size_t) override {
        double vp = (inp_ > 0) ? x(inp_ - 1) : 0.0;
        double vn = (inn_ > 0) ? x(inn_ - 1) : 0.0;
        double vd = vp - vn;

        bool next = isHigh_;
        if (vd >  HYS) next = true;
        else if (vd < -HYS) next = false;

        if (next != isHigh_) {
            isHigh_ = next;
            flippedSinceSave_ = true;
            return true;
        }
        return false;
    }

    double getBranchCurrent(const Eigen::VectorXd& x, size_t extraOff) const override {
        return x(extraOff);
    }

    void reset() override { isHigh_ = false; }

    void saveState() override    { savedIsHigh_ = isHigh_; flippedSinceSave_ = false; }
    void restoreState() override { isHigh_ = savedIsHigh_; flippedSinceSave_ = false; }
    bool stateChangedSinceLastSave() const override { return isHigh_ != savedIsHigh_; }
    bool flippedSinceLastSave()      const override { return flippedSinceSave_; }

private:
    std::string name_;
    int inp_, inn_, out_;
    double vhigh_, vlow_;
    bool isHigh_          = false;
    bool savedIsHigh_     = false;
    bool flippedSinceSave_ = false;

    static constexpr double HYS = 1e-6;   // input hysteresis (1 uV)
};
