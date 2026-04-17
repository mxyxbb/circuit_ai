#pragma once
#include "components/sources/voltage_source.h"
#include <cmath>

class SquareWaveSource : public VoltageSource {
public:
    SquareWaveSource(std::string name, int np, int nn,
                     double freq, double duty, double vhigh, double vlow,
                     double tdelay = 0.0)
        : VoltageSource(std::move(name), np, nn, 0.0),
          freq_(freq), duty_(duty), vhigh_(vhigh), vlow_(vlow), tdelay_(tdelay) {}

protected:
    double voltageAt(double t) const override {
        if (t < tdelay_) return vlow_;
        double phase = (t - tdelay_) * freq_;
        double frac = phase - std::floor(phase);
        return (frac < duty_) ? vhigh_ : vlow_;
    }

private:
    double freq_, duty_, vhigh_, vlow_, tdelay_;
};
