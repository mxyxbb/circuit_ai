#pragma once
#include "components/base_component.h"
#include <vector>
#include <algorithm>

// Ideal multi-winding transformer.
//
// For N windings (each with turns n_k and nodes np_k/nn_k):
//   - Voltage law (volts-per-turn equality):
//       n_k * V_1 - n_0 * V_k = 0,  for k = 1..N-1
//   - Ampere-turns balance (ideal core, no magnetising current):
//       n_0*I_0 + n_1*I_1 + ... + n_{N-1}*I_{N-1} = 0
//
// Extra variables: I_0..I_{N-1} (one current variable per winding)
//   extraOff+0  -> I_0 (winding 0, primary)
//   extraOff+k  -> I_k (winding k)
//
// MNA rows:
//   Row extraOff+0  : ampere-turns balance
//   Row extraOff+k  : voltage constraint for winding k (k = 1..N-1)
//
// Node stamp (B): I_k enters np_k and leaves nn_k.

class Transformer : public BaseComponent {
public:
    struct Winding {
        int np, nn;
        double turns;
    };

    Transformer(std::string name, std::vector<Winding> windings)
        : name_(std::move(name)), windings_(std::move(windings)) {}

    // Convenience constructor for the common 2-winding case with a turns ratio.
    // ratio = n_primary / n_secondary; stored as turns (ratio, 1).
    Transformer(std::string name, int np1, int nn1, int np2, int nn2, double ratio)
        : name_(std::move(name))
    {
        windings_.push_back({np1, nn1, ratio});
        windings_.push_back({np2, nn2, 1.0});
    }

    std::string name() const override { return name_; }

    size_t extraVariableCount() const override { return windings_.size(); }

    int maxNode() const override {
        int m = 0;
        for (const auto& w : windings_)
            m = std::max({m, w.np, w.nn});
        return m;
    }

    void stamp(MNASolver& solver, double /*dt*/, double /*t*/, size_t extraOff) override {
        const size_t N = windings_.size();

        // B-stamps: winding k current I_k flows into np_k and out of nn_k.
        for (size_t k = 0; k < N; ++k) {
            solver.stampB(windings_[k].np, extraOff + k,  1.0);
            solver.stampB(windings_[k].nn, extraOff + k, -1.0);
        }

        // Row extraOff+0: Ampere-turns balance
        //   n_0*I_0 + n_1*I_1 + ... + n_{N-1}*I_{N-1} = 0
        solver.stampD(extraOff, windings_[0].turns);
        for (size_t k = 1; k < N; ++k)
            solver.stampExtraCross(extraOff, extraOff + k, windings_[k].turns);

        // Rows extraOff+k (k=1..N-1): voltage constraint for winding k
        //   n_k*(V_p0 - V_n0) - n_0*(V_pk - V_nk) = 0
        const double n0 = windings_[0].turns;
        for (size_t k = 1; k < N; ++k) {
            const double nk = windings_[k].turns;
            solver.stampC(extraOff + k, windings_[0].np,  nk);
            solver.stampC(extraOff + k, windings_[0].nn, -nk);
            solver.stampC(extraOff + k, windings_[k].np, -n0);
            solver.stampC(extraOff + k, windings_[k].nn,  n0);
        }
    }

    // Returns the primary winding (winding 0) current.
    double getBranchCurrent(const Eigen::VectorXd& x, size_t extraOff) const override {
        return x(static_cast<Eigen::Index>(extraOff));
    }

    const std::vector<Winding>& windings() const { return windings_; }

private:
    std::string name_;
    std::vector<Winding> windings_;
};
