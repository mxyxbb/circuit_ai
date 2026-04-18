#include "engine/simulator.h"
#include "circuit/circuit.h"
#include "components/base_component.h"
#include <chrono>
#include <cstdio>

Simulator::Simulator() : solver_(std::make_unique<MNASolver>()) {}
Simulator::~Simulator() { stop(); }

bool Simulator::setup(const Circuit& circuit, const SimConfig& config,
                      const std::vector<SignalInfo>& probes) {
    stop();
    circuit_ = &circuit;
    config_ = config;
    probes_ = probes;

    n_ = static_cast<size_t>(circuit.nodeCount());
    m_ = 0;
    compExtraAbs_.clear();

    for (const auto& comp : circuit.components()) {
        compExtraAbs_.push_back(n_ + m_);
        m_ += comp->extraVariableCount();
    }

    solver_->init(n_, m_);

    // Build probe map: map each probe to an absolute index in x
    probeMap_.clear();
    for (const auto& probe : probes_) {
        ProbeMapEntry pme;
        pme.kind = ProbeMapEntry::NodeVoltage;
        pme.absIndex = 0;
        pme.nodeP = 0;
        pme.nodeN = 0;
        pme.resistance = 0.0;

        if (probe.type == SignalInfo::NodeVoltage) {
            pme.kind = ProbeMapEntry::NodeVoltage;
            pme.absIndex = static_cast<size_t>(probe.index - 1); // 0-based
        } else {
            // BranchCurrent: find component
            std::string compName = probe.name.substr(2, probe.name.size() - 3);
            const BaseComponent* comp = circuit.findComponent(compName);
            if (!comp) continue;

            if (comp->extraVariableCount() > 0) {
                // Has extra variable: read current directly from x
                pme.kind = ProbeMapEntry::ExtraCurrent;
                size_t ci = 0;
                for (const auto& c : circuit.components()) {
                    if (c.get() == comp) break;
                    ci++;
                }
                if (ci < compExtraAbs_.size()) {
                    pme.absIndex = compExtraAbs_[ci];
                }
            } else {
                // No extra variable (resistor, diode): compute from node voltages
                pme.kind = ProbeMapEntry::ResistorCurrent;
                pme.nodeP = comp->maxNode(); // approximate; need actual terminals
                // We need a way to get np/nn. For now, use getBranchCurrent.
                // Actually, we need to store component index for post-hoc computation.
                // Simplest: store a pointer and call getBranchCurrent during sampling.
                // But that requires the component to know its own nodes.
                // For Resistor specifically, maxNode() gives the highest node.
                // We need both nodes. Let's use a different approach:
                // Store the component pointer for deferred evaluation.
                pme.nodeP = 0;
                pme.nodeN = 0;
                pme.resistance = 0.0;
                pme.absIndex = 0; // will use as component index
                size_t ci = 0;
                for (const auto& c : circuit.components()) {
                    if (c.get() == comp) break;
                    ci++;
                }
                pme.absIndex = ci; // store component index
            }
        }
        probeMap_.push_back(pme);
    }

    t_.store(0.0);
    stepCount_ = 0;
    return true;
}

void Simulator::start() {
    if (running_.load()) return;
    stopRequested_ = false;
    paused_ = false;
    running_ = true;
    thread_ = std::thread(&Simulator::runLoop, this);
}

void Simulator::pause() {
    paused_ = true;
}

void Simulator::resume() {
    paused_ = false;
}

void Simulator::reset() {
    bool wasRunning = running_.load();
    stop();
    t_.store(0.0);
    stepCount_ = 0;
    beStepsRemaining_ = 0;
    // Reset component internal states (inductor iPrev, capacitor vPrev, etc.)
    if (circuit_) {
        for (const auto& comp : circuit_->components()) {
            comp->reset();
        }
    }
    // Clear ring buffers by draining
    SimSample dummy;
    while (ringBuffer_.pop(dummy)) {}
    DiagEvent dummyDiag;
    while (diagRing_.pop(dummyDiag)) {}
    if (wasRunning) start();
}

void Simulator::stop() {
    stopRequested_ = true;
    paused_ = false;
    if (thread_.joinable()) thread_.join();
    running_ = false;
}

void Simulator::runLoop() {
    while (!stopRequested_.load()) {
        if (paused_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (t_.load() >= config_.t_end) {
            paused_ = true;
            continue;
        }
        step();
    }
}

bool Simulator::step() {
    constexpr int MAX_ITER     = 20;
    constexpr int MAX_ZC_BISECT = 8;  // dt / 2^8 = dt/256 crossing accuracy
    const double dt = config_.dt;
    double t = t_.load();

    bool useBE = (beStepsRemaining_ > 0);
    for (const auto& comp : circuit_->components())
        comp->setUseBE(useBE);

    // Save component state (histories + switch positions) at time t.
    // Used by ZC bisection to restore state for each trial step.
    for (const auto& comp : circuit_->components())
        comp->saveState();

    // Inner convergence loop: stamp → solve → updateState until nonlinear
    // components converge or MAX_ITER is reached.
    // Returns true if convergence succeeded (all components settled with no further state change).
    // NOTE: intermediate iterations may flip states; only the FINAL converged state matters.
    auto innerSolve = [&](double stepDt, double atT) -> bool {
        bool converged = true;
        for (int iter = 0; iter < MAX_ITER; ++iter) {
            solver_->clear();
            solver_->applyGmin(1e-12);
            size_t ci = 0;
            for (const auto& comp : circuit_->components())
                comp->stamp(*solver_, stepDt, atT, compExtraAbs_[ci++]);
            const auto& x = solver_->solve();
            converged = true;
            ci = 0;
            for (const auto& comp : circuit_->components()) {
                if (comp->updateState(x, compExtraAbs_[ci++]))
                    converged = false;
            }
            if (converged) break;
        }
        if (!converged) {
            char buf[80];
            snprintf(buf, sizeof(buf),
                     "t=%.3e: convergence failed after %d iterations", atT, MAX_ITER);
            diagRing_.push({DiagEvent::Warning, atT, buf});
        }
        return converged;
    };

    // Helper: did the final state of any nonlinear component differ from saveState()?
    // Uses stateChangedSinceLastSave() so only PERSISTENT state changes trigger ZC.
    auto finalStateChanged = [&]() -> bool {
        for (const auto& comp : circuit_->components())
            if (comp->stateChangedSinceLastSave()) return true;
        return false;
    };

    // Full step to t + dt
    bool converged = innerSolve(dt, t);
    // Only trigger ZC bisection when:
    //   1. The solve converged (no chattering), AND
    //   2. The final state truly differs from the saved initial state.
    // Chattering (MAX_ITER exhausted) is treated as "no reliable crossing" — just proceed.
    bool anyStateChange = converged && finalStateChanged();
    double stepDt = dt;

    if (anyStateChange) {
        // Zero-crossing bisection: narrow [dtLo, dtHi] to locate t_zc to dt/2^MAX_ZC_BISECT.
        // dtLo = largest sub-step with no net state change (pre-crossing)
        // dtHi = smallest sub-step with net state change  (post-crossing ≈ t_zc)
        double dtLo = 0.0, dtHi = dt;
        for (int b = 0; b < MAX_ZC_BISECT; ++b) {
            double dtMid = (dtLo + dtHi) * 0.5;
            for (const auto& comp : circuit_->components()) {
                comp->restoreState();
                comp->setUseBE(true);
            }
            bool midConverged = innerSolve(dtMid, t);
            // If solve didn't converge at this sub-step, treat as "no crossing yet"
            if (midConverged && finalStateChanged()) dtHi = dtMid;
            else                                     dtLo = dtMid;
        }

        // Final solve at dtHi: this is the step that includes the crossing.
        for (const auto& comp : circuit_->components()) {
            comp->restoreState();
            comp->setUseBE(true);
        }
        innerSolve(dtHi, t);
        stepDt = dtHi;

        beStepsRemaining_ = 3;
    } else if (beStepsRemaining_ > 0) {
        --beStepsRemaining_;
    }

    const auto& x = solver_->lastSolution();

    if (!x.allFinite()) {
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "t=%.3e: solver NaN/Inf - check for floating nodes or V-source loops", t);
        diagRing_.push({DiagEvent::Error, t, buf});
    }

    // Commit history for storage components
    size_t ci = 0;
    for (const auto& comp : circuit_->components())
        comp->commitHistory(x, compExtraAbs_[ci++]);

    // Sample data for probes
    if (stepCount_ % static_cast<size_t>(config_.sample_ratio) == 0) {
        SimSample sample;
        sample.time = t;
        sample.values.resize(probeMap_.size());
        for (size_t i = 0; i < probeMap_.size(); ++i) {
            const auto& pm = probeMap_[i];
            switch (pm.kind) {
            case ProbeMapEntry::NodeVoltage:
                sample.values[i] = x(pm.absIndex);
                break;
            case ProbeMapEntry::ExtraCurrent:
                sample.values[i] = x(pm.absIndex);
                break;
            case ProbeMapEntry::ResistorCurrent: {
                size_t idx = pm.absIndex;
                const auto& comps = circuit_->components();
                if (idx < comps.size())
                    sample.values[i] = comps[idx]->getBranchCurrent(x, compExtraAbs_[idx]);
                break;
            }
            }
        }
        ringBuffer_.push(sample);
    }

    t_ = t + stepDt;
    stepCount_++;
    return true;
}
