#include "engine/simulator.h"
#include "circuit/circuit.h"
#include "components/base_component.h"
#include <chrono>
#include <cstdio>
#include <thread>

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
            // BranchCurrent: find component, stripping optional _Wk suffix
            std::string nameArg = probe.name.substr(2, probe.name.size() - 3);
            std::string compName = nameArg;
            size_t wPos = nameArg.rfind("_W");
            if (wPos != std::string::npos && wPos + 2 < nameArg.size()) {
                bool allDigit = true;
                for (size_t i = wPos + 2; i < nameArg.size(); ++i)
                    if (!std::isdigit((unsigned char)nameArg[i])) { allDigit = false; break; }
                if (allDigit) compName = nameArg.substr(0, wPos);
            }
            const BaseComponent* comp = circuit.findComponent(compName);
            if (!comp) continue;

            if (comp->extraVariableCount() > 0) {
                // Has extra variable: read current from x[compExtraAbs + windingOffset]
                pme.kind = ProbeMapEntry::ExtraCurrent;
                size_t ci = 0;
                for (const auto& c : circuit.components()) {
                    if (c.get() == comp) break;
                    ci++;
                }
                if (ci < compExtraAbs_.size()) {
                    pme.absIndex = compExtraAbs_[ci] + static_cast<size_t>(probe.index);
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
    startedAtEventBoundary_ = false;
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
    constexpr int MAX_ITER     = 50;
    constexpr int MAX_ZC_BISECT = 8;  // dt / 2^8 = dt/256 crossing accuracy
    const double dtNominal = config_.dt;
    double t = t_.load();

    // Event-time scheduling: clip the step so its boundary lands exactly on
    // the next scheduled discontinuity advertised by any component (e.g. a
    // PWM gate edge). Smooth components return +inf and don't affect effDt.
    // The +dtTolerance slop is critical: t accumulates FP rounding (~1 ULP per
    // step), so after many cycles the computed dtToEv may be dtNominal+ε for
    // edges that mathematically align with the grid. Without tolerance, those
    // edges fall through to the ZC-bisection path *without* startedAtEventBoundary_
    // set, which leaves TR active for the discontinuity step and kicks the LC
    // filter -- producing the non-periodic burst-mode wobble we'd otherwise see.
    // We extend the step (rather than truncate to dtNominal) so the boundary
    // lands exactly on the edge; an extension of < 1 ULP is numerically harmless.
    double effDt = dtNominal;
    bool clippedToEvent = false;
    // dtTolerance must absorb the late-time FP rounding accumulated in `t` over
    // many step additions. After ~2e5 dt additions the per-ULP slop in `t` can
    // reach 1e-13 to 1e-14 -- a tolerance of 1e-17 (relative 1e-9) was too tight,
    // letting some "natural-grid" edges fall through to ZC bisection and inject
    // dt/256 step offsets that pump the LC filter at specific cycle counts.
    // 1e-3 of dtNominal (= 1e-11) absorbs any plausible accumulation while still
    // being 7 orders of magnitude smaller than dtNominal -- can't cause spurious
    // clipping of events far in the future.
    const double dtTolerance = dtNominal * 1e-3;
    for (const auto& comp : circuit_->components()) {
        double tEv = comp->nextEventAfter(t);
        double dtToEv = tEv - t;
        if (dtToEv > 0.0 && dtToEv <= effDt + dtTolerance) {
            effDt = dtToEv;          // land exactly on edge (may slightly exceed dtNominal by FP slop)
            clippedToEvent = true;
        }
    }

    // Force BE for the step that starts AT a scheduled event boundary. The
    // source value jumps discontinuously at t (the gate just flipped), so
    // trapezoidal would ring on the step input. The pre-existing ZC bisection
    // path achieved this implicitly by leaving useBE_=true after the inner
    // bisection loop; since we now skip that loop on event boundaries, set BE
    // explicitly here. beStepsRemaining_ is still set to 3 below to damp the
    // following steps, mirroring the old behavior.
    bool useBE = (beStepsRemaining_ > 0) || startedAtEventBoundary_;
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
            if (stopRequested_.load(std::memory_order_relaxed)) return false;
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

    // Helper: did ANY component flip during the inner solve, even if it
    // ultimately re-converged to its saved state? Distinguishes transient
    // chatter (e.g. diode flipping OFF then back ON across iterations near
    // i = 0) from pure no-event steps. Used to (a) narrow the ZC bisection
    // when a transient crossing exists inside the sub-step interval, and
    // (b) arm BE damping so the next-step TR doesn't ring on the residual
    // history left by the chatter.
    auto anyComponentFlipped = [&]() -> bool {
        for (const auto& comp : circuit_->components())
            if (comp->flippedSinceLastSave()) return true;
        return false;
    };

    // Full step to t + effDt
    bool converged = innerSolve(effDt, t);
    // Only trigger ZC bisection when:
    //   1. The solve converged (no chattering), AND
    //   2. The final state truly differs from the saved initial state, AND
    //   3. We did NOT just start at a scheduled event boundary. In that case
    //      the change is the source edge landing exactly on this step's start;
    //      bisecting would converge to dtHi -> 0 and waste a solve.
    bool anyStateChange = converged && finalStateChanged();
    bool runBisection   = anyStateChange && !startedAtEventBoundary_;
    double stepDt = effDt;

    if (runBisection) {
        // Zero-crossing bisection: narrow [dtLo, dtHi] to locate t_zc to effDt/2^MAX_ZC_BISECT.
        // dtLo = largest sub-step with no net state change (pre-crossing)
        // dtHi = smallest sub-step with net state change  (post-crossing ≈ t_zc)
        double dtLo = 0.0, dtHi = effDt;
        for (int b = 0; b < MAX_ZC_BISECT; ++b) {
            if (stopRequested_.load(std::memory_order_relaxed)) break;
            double dtMid = (dtLo + dtHi) * 0.5;
            for (const auto& comp : circuit_->components()) {
                comp->restoreState();
                comp->setUseBE(true);
            }
            bool midConverged = innerSolve(dtMid, t);
            // Narrow when EITHER the final state differs (persistent crossing)
            // OR any component flipped during the inner solve (transient chatter
            // hidden inside [t, t+dtMid] -- final state matches saved but a
            // crossing is real and inside this interval). If solve didn't
            // converge, treat as "no crossing yet" and grow dtLo.
            if (midConverged && (finalStateChanged() || anyComponentFlipped()))
                dtHi = dtMid;
            else
                dtLo = dtMid;
        }

        // Final solve at dtHi: this is the step that includes the crossing.
        for (const auto& comp : circuit_->components()) {
            comp->restoreState();
            comp->setUseBE(true);
        }
        innerSolve(dtHi, t);
        stepDt = dtHi;
    }

    // Arm BE damping window when any flip occurred -- persistent (anyStateChange)
    // OR transient chatter that re-converged (e.g. diode flipping OFF then back
    // ON near i_diode = 0 in a DCM transition). The chatter case would otherwise
    // be invisible to the persistent-only check, leaving TR to ring on the small
    // history inconsistencies the chatter deposited on iPrev_/vPrev_.
    bool transientChatter = converged && !finalStateChanged() && anyComponentFlipped();
    if (anyStateChange || transientChatter) {
        // 8 BE steps after any flip event. With 100 kHz PWM and dt = 1e-8
        // (500 dt per half-cycle), that is 1.6% of the half-cycle in BE --
        // accuracy hit is negligible, but the extra damping smooths any kick
        // that would otherwise excite the output LC filter.
        beStepsRemaining_ = 8;
    } else if (beStepsRemaining_ > 0) {
        --beStepsRemaining_;
    }

    // Diagnostic: instrumented to show WHICH components flipped and what their
    // saved-vs-current states are. This is what the user sees in the panel and
    // is the key data for narrowing the burst root cause.
    if (runBisection) {
        char buf[160];
        // Walk components and build a compact list of who changed.
        // We use stateChangedSinceLastSave() on switches/diodes; report by index.
        char who[80] = {0};
        size_t off = 0;
        size_t ci = 0;
        for (const auto& comp : circuit_->components()) {
            if (comp->stateChangedSinceLastSave() && off + 8 < sizeof(who)) {
                int n = snprintf(who + off, sizeof(who) - off, "[%zu]%s ",
                                 ci, comp->name().c_str());
                if (n > 0) off += static_cast<size_t>(n);
            }
            ci++;
        }
        snprintf(buf, sizeof(buf),
                 "t=%.6e: ZC bisect non-evt: %s", t, who);
        diagRing_.push({DiagEvent::Warning, t, buf});
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
        // Block until consumer drains a slot. Without this, a momentarily slow
        // main thread (e.g. opening the scope window) would silently drop
        // samples and leave gaps in the captured waveform.
        while (!ringBuffer_.push(sample)) {
            if (stopRequested_.load(std::memory_order_relaxed)) break;
            std::this_thread::yield();
        }
    }

    t_ = t + stepDt;
    // If we clipped the step to a scheduled event AND we actually advanced to
    // that event (no ZC bisection truncated the step earlier), the next step
    // starts at the discontinuity. Tell the next step to skip ZC bisection
    // for the resulting state change.
    startedAtEventBoundary_ = clippedToEvent && !runBisection;
    stepCount_++;
    return true;
}
