#pragma once
#include "components/base_component.h"

class Transformer : public BaseComponent {
public:
    Transformer(std::string name, int np1, int nn1, int np2, int nn2, double ratio)
        : name_(std::move(name)), np1_(np1), nn1_(nn1), np2_(np2), nn2_(nn2), n_(ratio) {}

    std::string name() const override { return name_; }
    size_t extraVariableCount() const override { return 2; }
    int maxNode() const override { return std::max({np1_, nn1_, np2_, nn2_}); }

    // Ideal transformer: V1 = n*V2, I2 = -n*I1
    // extraOff+0 = I1 (primary current), extraOff+1 = I2 (secondary current)
    // Row extraOff+0: Vp1-Vn1 - n*(Vp2-Vn2) = 0
    // Row extraOff+1: n*I1 + I2 = 0
    void stamp(MNASolver& solver, double, double, size_t extraOff) override {
        size_t i1 = extraOff;
        size_t i2 = extraOff + 1;

        // Primary voltage constraint: Vp1-Vn1 - n*(Vp2-Vn2) = 0
        solver.stampB(np1_, i1, 1.0);
        solver.stampB(nn1_, i1, -1.0);
        solver.stampC(i1, np1_, 1.0);
        solver.stampC(i1, nn1_, -1.0);
        // Subtract n*V2 terms from the same row
        solver.stampC(i1, np2_, -n_);
        solver.stampC(i1, nn2_, n_);

        // Current constraint: n*I1 + I2 = 0
        solver.stampD(i2, 1.0);              // I2 coefficient
        solver.stampExtraCross(i2, i1, n_);   // n*I1 coefficient
    }

private:
    std::string name_;
    int np1_, nn1_, np2_, nn2_;
    double n_;
};
