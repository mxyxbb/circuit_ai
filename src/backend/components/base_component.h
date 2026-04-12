#pragma once
#include <string>
#include <cstddef>
#include <algorithm>
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
};
