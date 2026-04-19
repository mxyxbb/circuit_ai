#pragma once
#include "view_model/scope_model.h"
#include "view_model/schematic_model.h"
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
    void applySimConfig(double dt, double tEnd);

    // Called each frame: consume simulation data, push to Scope
    void update();

    // State queries
    bool isSimRunning() const;
    bool isSimPaused() const;
    double currentTime() const;
    const std::string& netlistPath() const { return netlistPath_; }
    const std::string& statusMessage() const { return statusMsg_; }

    // Diagnostics log (populated from simulator thread via SPSC ring)
    const std::vector<DiagEvent>& diagLog() const { return diagLog_; }
    void clearDiagLog() { diagLog_.clear(); }

    // Data access
    ScopeModel& scope() { return scope_; }
    const ScopeModel& scope() const { return scope_; }
    const std::vector<SignalInfo>& availableSignals() const { return probes_; }
    const SimConfig& simConfig() const { return config_; }

    // Schematic canvas model + build-from-schematic
    SchematicModel& schematic() { return schematic_; }
    const SchematicModel& schematic() const { return schematic_; }
    void buildFromSchematic();

    // Cross-view signal highlight (scope legend hover → schematic highlight)
    void setHoveredSignal(const std::string& s) { hoveredSignal_ = s; }
    const std::string& hoveredSignal() const { return hoveredSignal_; }

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

    std::vector<DiagEvent> diagLog_;
    static constexpr size_t kMaxDiagLog = 200;

    SchematicModel schematic_;
    std::string hoveredSignal_;
};
