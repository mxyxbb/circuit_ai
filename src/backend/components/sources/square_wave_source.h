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
        // FP-tolerant edge handling: when nextEventAfter() returns tEv at exactly
        // an edge time and clip lands the step there, FP rounding of (1/freq)*freq
        // would otherwise put the computed phase slightly below an integer (k -
        // ~5e-17). That breaks both edge types simultaneously:
        //   - HIGH->LOW (frac approaches duty=0.5): FP frac comes out as 0.4999...,
        //     `frac < 0.5` is true, returns vhigh (PRE-edge) when post-edge vlow
        //     was expected at tEv.
        //   - LOW->HIGH (frac wraps from 1.0 to 0): FP frac stays at 0.999... and
        //     floor(phase) returns k-1 instead of k, so frac never reaches 0.
        // Adding phaseEps to phase BEFORE floor lifts both cases above the FP
        // rounding margin, so the floor and frac come out matching the
        // mathematical edge. The shift moves the apparent transition by
        // phaseEps/freq = 1e-17 s for freq=100 kHz -- far below dt and physically
        // irrelevant, but enough to absorb the ~5e-17 s rounding error.
        constexpr double phaseEps = 1e-12;
        double phase = (t - tdelay_) * freq_ + phaseEps;
        double frac = phase - std::floor(phase);
        return (frac < duty_) ? vhigh_ : vlow_;
    }

public:
    double nextEventAfter(double t) const override {
        if (t < tdelay_) return tdelay_;                 // first rising edge
        const double T  = 1.0 / freq_;
        const double k  = std::floor((t - tdelay_) / T);
        const double cs = tdelay_ + k * T;               // current cycle start
        const double dp = cs + duty_ * T;                // HIGH->LOW edge in cycle
        const double ns = cs + T;                        // next LOW->HIGH edge
        if (dp > t) return dp;
        if (ns > t) return ns;
        return ns + duty_ * T;                           // floating-point edge case
    }


private:
    double freq_, duty_, vhigh_, vlow_, tdelay_;
};
