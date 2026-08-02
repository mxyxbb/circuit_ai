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
        // mathematical edge.
        //
        // phaseEps must be RELATIVE, not fixed: the rounding error of both this
        // product and of the edge times returned by nextEventAfter() is a few
        // ULP of `phase`, which grows with the cycle count k. A fixed 1e-12
        // (~1 ULP at phase = 4096) silently stopped absorbing the error once
        // t*freq exceeded ~2^12 cycles: at 1 MHz that is t > ~4 ms, after which
        // edge evaluation at clipped step boundaries randomly returned the
        // PRE-edge value. The missed edge then flips the switches one step
        // late, mid-step, where ZC bisection collapses to its dt/256 floor and
        // the 256x-stiffened inductor companion produces a several-hundred-volt
        // single-sample spike (seen in hsc4816 from t = 7.8 ms onward).
        // 1e-12 relative = ~4.5 ULP at any magnitude; the apparent edge shift
        // is phase*1e-12/freq seconds = t*1e-12 -- always orders of magnitude
        // below dt. The absolute 1e-12 floor covers t near zero.
        double phase = (t - tdelay_) * freq_;
        phase += phase * 1e-12 + 1e-12;
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

    double driveFrequency() const override { return freq_; }


private:
    double freq_, duty_, vhigh_, vlow_, tdelay_;
};
