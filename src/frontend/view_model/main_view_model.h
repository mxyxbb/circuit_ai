#pragma once
#include "view_model/scope_model.h"
#include "common/sim_types.h"
#include "engine/simulator.h"
#include "circuit/netlist_parser.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

class MainViewModel {
public:
    MainViewModel();
    ~MainViewModel();

    // Commands (called by Views)
    bool loadNetlist(const std::string& filepath);
    void play();
    void pause();
    void reset();

    // Called each frame: consume simulation data, push to Scope
    void update();

    // State queries
    bool isSimRunning() const;
    bool isSimPaused() const;
    double currentTime() const;
    const std::string& netlistPath() const { return netlistPath_; }
    const std::string& statusMessage() const { return statusMsg_; }

    // Data access
    ScopeModel& scope() { return scope_; }
    const ScopeModel& scope() const { return scope_; }
    const std::vector<SignalInfo>& availableSignals() const { return probes_; }

private:
    void dispatchSample(const SimSample& sample);
    void autoPopulateScope();

    std::unique_ptr<Simulator> simulator_;
    ScopeModel scope_;

    // Circuit ownership: lives as long as simulator references it
    std::unique_ptr<Circuit> circuit_;
    SimConfig config_;
    std::vector<SignalInfo> probes_;
    std::string netlistPath_;
    std::string statusMsg_;

    // Signal name → index in SimSample.values
    std::unordered_map<std::string, size_t> signalNameToIdx_;
};
