#pragma once
#include "common/sim_types.h"
#include "common/ring_buffer.h"
#include "engine/mna_solver.h"
#include <atomic>
#include <memory>
#include <thread>
#include <vector>

class BaseComponent;
class Circuit;

class Simulator {
public:
    Simulator();
    ~Simulator();

    // Circuit is NOT owned by Simulator; caller must keep it alive.
    // Builds internal solver and computes extra variable offsets.
    bool setup(const Circuit& circuit, const SimConfig& config,
               const std::vector<SignalInfo>& probes);

    void start();
    void pause();
    void resume();
    void reset();
    void stop();

    bool isRunning() const { return running_.load(); }
    bool isPaused()  const { return paused_.load(); }
    double currentTime() const { return t_.load(); }

    bool consumeSample(SimSample& sample) { return ringBuffer_.pop(sample); }

    size_t nodeCount() const { return n_; }
    const std::vector<SignalInfo>& probes() const { return probes_; }

    struct ProbeMapEntry {
        enum Kind { NodeVoltage, ExtraCurrent, ResistorCurrent } kind;
        size_t absIndex;      // for NodeVoltage: node-1; for ExtraCurrent: extra var index
        int nodeP, nodeN;     // for ResistorCurrent: compute (V_P - V_N) / resistance
        double resistance;    // for ResistorCurrent
    };
    const std::vector<ProbeMapEntry>& probeMap() const { return probeMap_; }

private:
    void runLoop();
    bool step();

    std::unique_ptr<MNASolver> solver_;
    const Circuit* circuit_ = nullptr; // non-owning

    SimConfig config_;
    std::vector<SignalInfo> probes_;
    std::vector<ProbeMapEntry> probeMap_;

    // Absolute matrix index for each component's first extra variable
    std::vector<size_t> compExtraAbs_;

    size_t n_ = 0;
    size_t m_ = 0;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    std::atomic<bool> stopRequested_{false};
    std::atomic<double> t_{0.0};

    SPSCRingBuffer<SimSample, 65536> ringBuffer_;
    size_t stepCount_ = 0;
};
