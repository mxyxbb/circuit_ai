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
};
