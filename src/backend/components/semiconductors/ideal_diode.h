#pragma once
#include "components/base_component.h"

class IdealDiode : public BaseComponent {
public:
    IdealDiode(std::string name, int anode, int cathode)
        : name_(std::move(name)), na_(anode), nk_(cathode), isOn_(false) {}

    std::string name() const override { return name_; }
    int maxNode() const override { return std::max(na_, nk_); }

    void stamp(MNASolver& solver, double, double, size_t) override {
        double r = isOn_ ? R_ON : R_OFF;
        solver.stampConductance(na_, nk_, 1.0 / r);
    }

    double getBranchCurrent(const Eigen::VectorXd& x, size_t) const override {
        double va = (na_ > 0) ? x(na_ - 1) : 0.0;
        double vk = (nk_ > 0) ? x(nk_ - 1) : 0.0;
        double vak = va - vk;
        if (isOn_) {
            // ON: current = Vak / R_ON. Clamp to 0 to report no reverse current
            // (transient solver may leave Vak slightly negative before next iteration flips state)
            double i = vak / R_ON;
            return (i > 0.0) ? i : 0.0;
        }
        // OFF: only tiny leakage; clamp to 0 for clean reporting
        return 0.0;
    }

    bool updateState(const Eigen::VectorXd& x, size_t) override {
        double va = (na_ > 0) ? x(na_ - 1) : 0.0;
        double vk = (nk_ > 0) ? x(nk_ - 1) : 0.0;
        double vak = va - vk;

        bool changed = false;
        if (isOn_) {
            // Turn OFF as soon as Vak < 0: ideal diode carries no reverse current.
            // (A larger hysteresis like Vak < -1 mV would let the diode keep R_ON
            // active for reverse current up to V_OFF/R_ON = 1 A — that's a model
            // error, not a hysteresis margin. Anti-chatter is provided instead by
            // justFlippedOff_ within an innerSolve and by `flippedSinceLastSave`
            // tracking + BE arming across innerSolves.)
            if (vak < 0.0) {
                isOn_         = false;
                changed       = true;
                justFlippedOff_ = true;  // block re-turn-on this innerSolve (anti-chatter)
            }
        } else if (!justFlippedOff_) {
            // Turn ON only when clearly forward biased AND we haven't just flipped OFF
            // in this same inner-solve pass (prevents ON↔OFF chattering at zero crossing).
            if (vak > V_ON) {
                isOn_   = true;
                changed = true;
            }
        }
        if (changed) flippedSinceSave_ = true;
        return changed;
    }

    // saveState / restoreState called by the simulator around each innerSolve call.
    // Both reset justFlippedOff_ so every new solve pass starts fresh.
    void saveState() override {
        savedIsOn_        = isOn_;
        justFlippedOff_   = false;
        flippedSinceSave_ = false;
    }
    void restoreState() override {
        isOn_             = savedIsOn_;
        justFlippedOff_   = false;
        flippedSinceSave_ = false;
    }

    bool stateChangedSinceLastSave() const override { return isOn_ != savedIsOn_; }
    bool flippedSinceLastSave()      const override { return flippedSinceSave_; }

private:
    std::string name_;
    int  na_, nk_;
    bool isOn_             = false;
    bool savedIsOn_        = false;
    bool justFlippedOff_   = false;   // per-innerSolve anti-chatter flag
    bool flippedSinceSave_ = false;   // true if updateState reported any flip since saveState

    static constexpr double R_ON  = 1e-3;   // on-state resistance (1 mΩ)
    static constexpr double R_OFF = 1e9;    // off-state resistance (1 GΩ)
    static constexpr double V_ON  = 1e-3;   // turn-on threshold: Vak > 1 mV
};
