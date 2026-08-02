#pragma once
#include <string>
#include <cstddef>
#include <algorithm>
#include <limits>
#include <Eigen/Dense>
#include "engine/mna_solver.h"

class BaseComponent {
public:
    virtual ~BaseComponent() = default;

    virtual std::string name() const = 0;
    virtual size_t extraVariableCount() const { return 0; }
    virtual int maxNode() const = 0;

    // extraOff = absolute matrix row index of this component's first extra variable.
    // Simulator computes: absOff = nodeCount + relativeOffset
    virtual void stamp(MNASolver& solver, double dt, double t, size_t extraOff) = 0;
    virtual bool updateState(const Eigen::VectorXd& x, size_t extraOff) {
        (void)x; (void)extraOff; return false;
    }
    virtual void commitHistory(const Eigen::VectorXd& x, size_t extraOff) {
        (void)x; (void)extraOff;
    }
    virtual double getBranchCurrent(const Eigen::VectorXd& x, size_t extraOff) const {
        (void)x; (void)extraOff; return 0.0;
    }
    virtual void reset() {}
    virtual void setUseBE(bool) {}

    // ZC bisection support: save/restore full component state (history + switch position).
    // Called by Simulator before and during zero-crossing detection.
    virtual void saveState() {}
    virtual void restoreState() {}

    // Returns true if the current (post-solve) state differs from the last saveState().
    // Only nonlinear components (switches, diodes) need to override this.
    virtual bool stateChangedSinceLastSave() const { return false; }

    // Returns true if updateState() reported any flip since the last saveState(),
    // even if the FINAL state matches savedState. Lets the simulator detect
    // transient chatter (diode flips OFF then back ON within one innerSolve)
    // that stateChangedSinceLastSave() would miss because it compares only
    // current vs saved.
    virtual bool flippedSinceLastSave() const { return false; }

    // Smallest scheduled discontinuity time strictly greater than t, or +inf.
    // Time-dependent sources with discrete edges (square wave, step) override
    // this so the simulator can clip its next step to land exactly on the edge.
    // Smooth sources (sin, DC) leave the default and behave as before.
    virtual double nextEventAfter(double t) const {
        (void)t;
        return std::numeric_limits<double>::infinity();
    }

    // ── POP / fundamental-frequency detection ────────────────────────────────
    // Periodic sources (square wave, sine) report their drive frequency in Hz;
    // everything else returns 0. Used to identify the FFT fundamental directly
    // from the user-set frequency property rather than inferring it from data.
    virtual double driveFrequency() const { return 0.0; }
    // The node this component actively drives (its positive terminal); -1 if the
    // component is not a source. Lets the simulator trace a switch gate node back
    // to the source that drives it.
    virtual int    drivenNode()    const { return -1; }
    // Switching devices (MOSFET / ideal switch) report their gate node; -1 for
    // every other component. Nodes are >= 0 (0 = GND), so >= 0 == "is a switch".
    virtual int    gateNode()      const { return -1; }
};
