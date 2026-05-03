#pragma once
#include "view_model/scope_model.h"
#include "view_model/scrolling_buffer.h"
#include "view_model/schematic_model.h"
#include "common/sim_types.h"
#include "engine/simulator.h"
#include "circuit/netlist_parser.h"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <deque>

// Virtual/derived signal: val = sigA*kA + sigB*kB, dispatched by name
struct ComputedSig {
    std::string name;   // virtual signal name, e.g. "I(L2-N)", "V(1-2)"
    std::string sigA;   // first underlying signal
    double      kA;
    std::string sigB;   // second underlying signal (empty if single)
    double      kB = 0.0;
};

// One open schematic document — multi-doc tab UI is built around this.
struct SchematicDoc {
    SchematicModel model;
    std::string    filePath;     // full path on disk; empty for unsaved
    std::string    displayName;  // tab label (filename or "Untitled-N")
    int            id = 0;       // permanent unique id (stable across reorders)

    // Per-doc undo/redo (managed by SchematicView)
    std::deque<SchematicModel> undoStack;
    std::deque<SchematicModel> redoStack;

    // Per-doc raw signal cache. Holds the data from the last simulation run
    // of THIS doc, used for backfill when the user adds a probe to a scope
    // and for retroComputeSig. Keeping it per-doc prevents cross-contamination:
    // running sch1 leaves sch0's last-run rawCache untouched, so scope0
    // (whose entries are tagged sourceSchId=sch0.id) can still be queried.
    std::unordered_map<std::string, ScrollingBuffer> rawCache;
    size_t rawCacheCapacity = 10000;

    // Sim config captured at the last successful build of this doc. Scopes
    // owned by this doc lock their X-axis to lastConfig.t_end so a different
    // doc rerun (with a different t_end) doesn't clobber their time range.
    SimConfig lastConfig;
    bool      hasLastConfig = false;  // true after first build of this doc
};

class MainViewModel {
public:
    MainViewModel();
    ~MainViewModel();

    // Commands (called by Views)
    void play();
    void pause();
    void stop();
    void reset();
    void applySimConfig(double dt, double tEnd);

    // Async build: spawns a background thread so UI stays responsive.
    void requestBuild();
    void cancelBuild() { cancelBuild_.store(true); }
    bool isBuildPending() const { return buildPending_.load(); }
    void stopBuildAndWait();

    // Called each frame: consume simulation data, push to Scope
    void update();

    // State queries
    bool isSimRunning() const;
    bool isSimPaused() const;
    double currentTime() const;
    const std::string& statusMessage() const { return statusMsg_; }

    // Diagnostics log
    const std::vector<DiagEvent>& diagLog() const { return diagLog_; }
    void clearDiagLog() { diagLog_.clear(); }

    // Multi-scope access
    ScopeModel& scope(int idx = 0);
    const ScopeModel& scope(int idx = 0) const;
    int  scopeCount() const { return (int)scopes_.size(); }
    int  addScope();
    void removeScope(int idx);
    const std::vector<SignalInfo>& availableSignals() const { return probes_; }
    const SimConfig& simConfig() const { return config_; }

    // ── Schematic documents (multi-tab) ──────────────────────────────────────
    SchematicModel&       schematic()       { return activeSchDoc().model; }
    const SchematicModel& schematic() const { return activeSchDoc().model; }
    SchematicDoc&         activeSchDoc();
    const SchematicDoc&   activeSchDoc() const;
    int                   activeSchIdx() const { return activeSchIdx_; }
    void                  setActiveSchIdx(int idx);
    int                   schDocCount() const { return (int)schDocs_.size(); }
    SchematicDoc&         schDoc(int idx)       { return *schDocs_[idx]; }
    const SchematicDoc&   schDoc(int idx) const { return *schDocs_[idx]; }
    int                   newSchDoc();             // creates a fresh untitled doc; returns its index
    void                  closeSchDoc(int idx);    // closes doc; also removes scopes solely owned by it
    int                   findSchDocByPath(const std::string& path) const;
    int                   findSchDocById(int id) const;

    // Build-from-active-schematic (active doc)
    void buildFromSchematic();

    // Identifies scopes whose entries are solely owned by the given doc.
    // Used by MainView to tear down those scopes (and their ScopeViews) when
    // the user closes the doc tab.
    std::vector<int> scopesOwnedByDoc(int docIdx) const;

    // Returns the doc id that drove the most recent buildFromSchematic.
    // -1 if no build has run, or after stopAndClearSim was called.
    int  lastBuiltDocId() const { return lastBuiltDocId_; }

    // Stops the simulator and discards everything tied to the current build:
    // circuit, probe map, signal index, raw cache, computed sigs, diag log.
    // Scope layouts/entries are NOT touched (caller is responsible for that
    // if the scopes belong to a closing doc).
    void stopAndClearSim();

    // Active scope index
    int  activeScope() const { return activeScope_; }
    void setActiveScope(int idx) {
        if (idx >= 0 && idx < (int)scopes_.size()) activeScope_ = idx;
    }

    // Cross-view signal highlight
    void setHoveredSignal(const std::string& s) { hoveredSignal_ = s; }
    const std::string& hoveredSignal() const { return hoveredSignal_; }

    // Probe active state
    void setProbeActive(bool b) { probeActive_ = b; }
    bool isProbeActive() const  { return probeActive_; }

    // Computed (virtual) signals
    void registerComputedSig(const std::string& name,
                              const std::string& sigA, double kA,
                              const std::string& sigB = "", double kB = 0.0);
    void clearComputedSigs() { computedSigs_.clear(); }
    void retroComputeSig(const std::string& name);
    const std::vector<ComputedSig>& computedSigs() const { return computedSigs_; }

    // Rename a signal everywhere
    void renameSignal(const std::string& oldName, const std::string& newName);

    // Sync raw cache to a scope (uses the active doc's per-sch rawCache)
    void syncRawCacheToScope(int scopeIdx);

    // Look up a doc's display name from its id; "" if id not found.
    std::string displayNameForSchId(int id) const;

    // Look up the t_end captured at the last build of the doc with this id.
    // Returns -1.0 if the id is unknown or that doc has never been built.
    double tEndForSchId(int id) const;

    // Max samples per ScrollingBuffer (per signal). Caps calcBufferCapacity so
    // multi-doc memory cost stays bounded. Default 5,000,000 (≈80 MB per signal,
    // matching the old hard-coded ceiling). Surfaced in SettingsView.
    size_t maxStoredSamples() const { return maxStoredSamples_; }
    void   setMaxStoredSamples(size_t n) {
        if (n < 1000) n = 1000;
        maxStoredSamples_ = n;
    }

private:
    void dispatchSample(const SimSample& sample);
    void autoPopulateScope();

    std::unique_ptr<Simulator> simulator_;
    std::vector<std::unique_ptr<ScopeModel>> scopes_;
    int activeScope_ = 0;

    std::unique_ptr<Circuit> circuit_;
    SimConfig config_;
    std::vector<SignalInfo> probes_;
    std::string statusMsg_;

    std::unordered_map<std::string, size_t> signalNameToIdx_;

    std::vector<DiagEvent> diagLog_;
    static constexpr size_t kMaxDiagLog = 200;

    // User-tunable cap for per-signal buffer length (samples). 5e6 × 16 B ≈ 80 MB
    // per signal. Multiplied by total signals × open schematics for total memory.
    size_t maxStoredSamples_ = 5'000'000;

    // Multi-doc schematic state
    std::vector<std::unique_ptr<SchematicDoc>> schDocs_;
    int activeSchIdx_   = 0;
    int nextSchDocId_   = 1;
    int nextUntitledId_ = 1;

    std::string hoveredSignal_;
    bool probeActive_ = false;
    std::vector<ComputedSig> computedSigs_;

    std::atomic<bool> buildPending_{false};
    std::atomic<bool> cancelBuild_{false};
    std::thread       buildThread_;

    int lastBuiltDocId_ = -1;
};
