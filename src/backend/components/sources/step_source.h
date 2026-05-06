#pragma once
#include "components/sources/voltage_source.h"
#include <limits>

class StepSource : public VoltageSource {
public:
    StepSource(std::string name, int np, int nn, double v0, double v1, double tdelay)
        : VoltageSource(std::move(name), np, nn, 0.0),
          v0_(v0), v1_(v1), tdelay_(tdelay) {}

    double nextEventAfter(double t) const override {
        return (t < tdelay_) ? tdelay_
                             : std::numeric_limits<double>::infinity();
    }

protected:
    double voltageAt(double t) const override {
        return (t < tdelay_) ? v0_ : v1_;
    }

private:
    double v0_, v1_, tdelay_;
};
