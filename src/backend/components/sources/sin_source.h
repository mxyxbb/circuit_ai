#pragma once
#include "components/sources/voltage_source.h"
#include <cmath>

class SinSource : public VoltageSource {
public:
    SinSource(std::string name, int np, int nn,
              double voff, double vampl, double freq, double phase)
        : VoltageSource(std::move(name), np, nn, 0.0),
          voff_(voff), vampl_(vampl), freq_(freq), phase_(phase) {}

protected:
    double voltageAt(double t) const override {
        return voff_ + vampl_ * std::sin(6.283185307179586 * freq_ * t + phase_);
    }

private:
    double voff_;    // DC offset (V)
    double vampl_;   // Peak amplitude (V)
    double freq_;    // Frequency (Hz)
    double phase_;   // Initial phase (rad)
};
