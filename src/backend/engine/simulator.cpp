#include "engine/simulator.h"
#include "circuit/circuit.h"
#include "components/base_component.h"
#include <chrono>

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
    // Clear ring buffer by draining
    SimSample dummy;
    while (ringBuffer_.pop(dummy)) {}
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
    constexpr int MAX_ITER = 20;
    const double dt = config_.dt;
    double t = t_.load();

    // Iterative solve for nonlinear components
    const Eigen::VectorXd* xp = nullptr;
    for (int iter = 0; iter < MAX_ITER; ++iter) {
        solver_->clear();

        size_t ci = 0;
        for (const auto& comp : circuit_->components()) {
            comp->stamp(*solver_, dt, t, compExtraAbs_[ci]);
            ci++;
        }

        const auto& x = solver_->solve();
        xp = &x;

        // Check convergence for nonlinear components
        bool converged = true;
        ci = 0;
        for (const auto& comp : circuit_->components()) {
            if (comp->updateState(x, compExtraAbs_[ci])) {
                converged = false;
            }
            ci++;
        }
        if (converged) break;
    }

    // xp points to the final solution from the last solve()
    const auto& x = *xp;

    // Commit history for storage components
    size_t ci = 0;
    for (const auto& comp : circuit_->components()) {
        comp->commitHistory(x, compExtraAbs_[ci]);
        ci++;
    }

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
                // Use the component's getBranchCurrent which returns 0 for base class.
                // For resistors, we need to compute (Vnp - Vnn)/R.
                // Since getBranchCurrent returns 0 for resistors, we compute directly.
                size_t ci = pm.absIndex;
                const auto& comps = circuit_->components();
                if (ci < comps.size()) {
                    sample.values[i] = comps[ci]->getBranchCurrent(x, compExtraAbs_[ci]);
                }
                break;
            }
            }
        }
        while (!ringBuffer_.push(sample)) {
            SimSample dummy;
            ringBuffer_.pop(dummy);
        }
    }

    t_ = t + dt;
    stepCount_++;
    return true;
}
