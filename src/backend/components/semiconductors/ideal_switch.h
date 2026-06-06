#pragma once
#include "components/base_component.h"

class IdealSwitch : public BaseComponent {
public:
    IdealSwitch(std::string name, int drain, int source, int gate, int gateRef,
                double rOn = 1e-3)
        : name_(std::move(name)), nd_(drain), ns_(source),
          ng_(gate), ngr_(gateRef), state_(OFF), rOn_(rOn) {}

    std::string name() const override { return name_; }
    int maxNode() const override { return std::max({nd_, ns_, ng_, ngr_}); }

    void stamp(MNASolver& solver, double, double, size_t) override {
        double r = (state_ == ON)         ? rOn_      :
                   (state_ == BODY_DIODE) ? BODY_R_ON : R_OFF;
        solver.stampConductance(nd_, ns_, 1.0 / r);
    }

    bool updateState(const Eigen::VectorXd& x, size_t) override {
        double vgs = ((ng_  > 0) ? x(ng_  - 1) : 0.0)
                   - ((ngr_ > 0) ? x(ngr_ - 1) : 0.0);
        double vds = ((nd_ > 0) ? x(nd_ - 1) : 0.0)
                   - ((ns_ > 0) ? x(ns_ - 1) : 0.0);

        State desired;
        if (vgs >= V_GATE_THRESHOLD) {
            // Gate high: channel ON.  Also clear body-diode anti-chatter flag.
            desired             = ON;
            justFlippedOff_     = false;
            justExitedBodyDiode_ = false;
        } else {
            // Gate low: channel off.
            // Body diode activates when Vds < -threshold, BUT:
            //   (a) not immediately after the channel just turned OFF (justFlippedOff_)
            //   (b) not if the body diode just exited in this same innerSolve (justExitedBodyDiode_)
            // Both flags are reset at saveState / restoreState so each innerSolve starts fresh.
            if (vds < -V_BD_THRESHOLD && !justFlippedOff_ && !justExitedBodyDiode_)
                desired = BODY_DIODE;
            else
                desired = OFF;
        }

        if (desired == state_) return false;

        bool changed = true;

        // Track body-diode exit so it can't chatter back ON in the same innerSolve pass.
        if (state_ == BODY_DIODE && desired == OFF)
            justExitedBodyDiode_ = true;

        // Track channel turn-off so body diode can't fire in the same innerSolve pass.
        if (state_ == ON && desired == OFF)
            justFlippedOff_ = true;

        state_ = desired;
        flippedSinceSave_ = true;
        return changed;
    }

    double getBranchCurrent(const Eigen::VectorXd& x, size_t) const override {
        double vd = (nd_ > 0) ? x(nd_ - 1) : 0.0;
        double vs = (ns_ > 0) ? x(ns_ - 1) : 0.0;
        double vds = vd - vs;
        switch (state_) {
        case ON:
            // Channel: current flows drain→source (positive for normal operation)
            return vds / rOn_;
        case BODY_DIODE:
            // Body diode: current flows source→drain (negative Ids direction).
            // Clamp to negative-only: body diode cannot carry positive (forward) current.
            {
                double i = vds / BODY_R_ON;
                return (i < 0.0) ? i : 0.0;
            }
        default: // OFF
            return 0.0;  // leakage negligible; report 0 for clean scope traces
        }
    }

    void reset() override {
        state_               = OFF;
        justFlippedOff_      = false;
        justExitedBodyDiode_ = false;
        flippedSinceSave_    = false;
    }

    void saveState() override {
        savedState_          = state_;
        justFlippedOff_      = false;   // fresh start for new timestep
        justExitedBodyDiode_ = false;
        flippedSinceSave_    = false;
    }
    void restoreState() override {
        state_               = savedState_;
        justFlippedOff_      = false;   // fresh start for ZC sub-step
        justExitedBodyDiode_ = false;
        flippedSinceSave_    = false;
    }

    bool stateChangedSinceLastSave() const override { return state_ != savedState_; }
    bool flippedSinceLastSave()      const override { return flippedSinceSave_; }

private:
    enum State { OFF, ON, BODY_DIODE };
    std::string name_;
    int nd_, ns_, ng_, ngr_;
    State state_      = OFF;
    State savedState_ = OFF;

    // Anti-chatter flags (per-innerSolve, reset by saveState/restoreState):
    bool justFlippedOff_      = false;  // ON→OFF: block immediate body-diode activation
    bool justExitedBodyDiode_ = false;  // BODY_DIODE→OFF: block immediate re-entry
    bool flippedSinceSave_    = false;  // any state change since saveState (transient or persistent)

    double rOn_;                                       // user-settable channel ON resistance (Ω)
    static constexpr double BODY_R_ON        = 2e-3;
    static constexpr double R_OFF            = 1e9;
    static constexpr double V_GATE_THRESHOLD = 0.5;
    static constexpr double V_BD_THRESHOLD   = 1e-3;   // 1 mV body-diode activation
};
