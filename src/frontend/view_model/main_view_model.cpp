#include "view_model/main_view_model.h"
#include <algorithm>
#include <cstdio>

// Calculate the ScrollingBuffer capacity needed to store all simulation samples
// at sample_ratio=1.  Capped at maxSamples to bound memory usage.
// (5M doubles × 8 bytes × 2 arrays ≈ 80 MB per signal at the default cap)
static size_t calcBufferCapacity(double tEnd, double dt, size_t maxSamples) {
    if (tEnd <= 0.0 || dt <= 0.0) return std::min<size_t>(10000, maxSamples);
    size_t needed = static_cast<size_t>(tEnd / dt) + 1;
    return std::min(needed, maxSamples);
}

MainViewModel::MainViewModel()
    : simulator_(std::make_unique<Simulator>()),
      circuit_(std::make_unique<Circuit>()) {
    scopes_.push_back(std::make_unique<ScopeModel>());
    // Start with one empty Untitled document so the user always has an active sch.
    newSchDoc();
}

SchematicDoc& MainViewModel::activeSchDoc() {
    if (schDocs_.empty()) newSchDoc();
    if (activeSchIdx_ < 0 || activeSchIdx_ >= (int)schDocs_.size()) activeSchIdx_ = 0;
    return *schDocs_[activeSchIdx_];
}
const SchematicDoc& MainViewModel::activeSchDoc() const {
    // const path: caller is responsible for state being valid (called only when at least one doc exists)
    int idx = (activeSchIdx_ >= 0 && activeSchIdx_ < (int)schDocs_.size()) ? activeSchIdx_ : 0;
    return *schDocs_[idx];
}

void MainViewModel::setActiveSchIdx(int idx) {
    if (idx < 0 || idx >= (int)schDocs_.size()) return;
    activeSchIdx_ = idx;
}

int MainViewModel::newSchDoc() {
    auto doc = std::make_unique<SchematicDoc>();
    doc->id = nextSchDocId_++;
    doc->displayName = "Untitled-" + std::to_string(nextUntitledId_++);
    schDocs_.push_back(std::move(doc));
    activeSchIdx_ = (int)schDocs_.size() - 1;
    return activeSchIdx_;
}

int MainViewModel::findSchDocByPath(const std::string& path) const {
    for (int i = 0; i < (int)schDocs_.size(); i++)
        if (schDocs_[i]->filePath == path) return i;
    return -1;
}
int MainViewModel::findSchDocById(int id) const {
    for (int i = 0; i < (int)schDocs_.size(); i++)
        if (schDocs_[i]->id == id) return i;
    return -1;
}

void MainViewModel::closeSchDoc(int idx) {
    if (idx < 0 || idx >= (int)schDocs_.size()) return;
    int closingId = schDocs_[idx]->id;

    // Remove scopes whose sole owner is this doc (i.e. all signals come from it).
    // Compute owner for each scope before erasing so the index stays consistent.
    for (int si = (int)scopes_.size() - 1; si >= 0; si--) {
        ScopeModel& sc = *scopes_[si];
        // Inline owner computation (avoid coupling to SchematicView)
        int owner = -2;  // -2 sentinel for "no entries seen yet"
        bool mixed = false;
        for (int pi = 0; pi < sc.plotCount() && !mixed; pi++) {
            const PlotArea* p = sc.getPlot(pi);
            if (!p) continue;
            for (const auto& e : p->entries) {
                if (e->sourceSchId == -1) { mixed = true; break; }
                if (owner == -2) owner = e->sourceSchId;
                else if (owner != e->sourceSchId) { mixed = true; break; }
            }
        }
        int finalOwner = mixed ? -1 : (owner == -2 ? -1 : owner);
        if (finalOwner == closingId) {
            scopes_.erase(scopes_.begin() + si);
            if (activeScope_ >= (int)scopes_.size()) activeScope_ = (int)scopes_.size() - 1;
        }
    }

    schDocs_.erase(schDocs_.begin() + idx);
    if (schDocs_.empty()) {
        // Always keep at least one untitled doc so vm.schematic() is valid.
        newSchDoc();
    } else {
        if (activeSchIdx_ >= (int)schDocs_.size()) activeSchIdx_ = (int)schDocs_.size() - 1;
        if (activeSchIdx_ < 0) activeSchIdx_ = 0;
    }
}

ScopeModel& MainViewModel::scope(int idx) {
    if (scopes_.empty()) scopes_.push_back(std::make_unique<ScopeModel>());
    if (idx < 0 || idx >= (int)scopes_.size()) return *scopes_[0];
    return *scopes_[idx];
}
const ScopeModel& MainViewModel::scope(int idx) const {
    static ScopeModel dummy;
    if (scopes_.empty()) return dummy;
    if (idx < 0 || idx >= (int)scopes_.size()) return *scopes_[0];
    return *scopes_[idx];
}

int MainViewModel::addScope() {
    auto sc = std::make_unique<ScopeModel>();
    size_t cap = calcBufferCapacity(config_.t_end, config_.dt, maxStoredSamples_);
    sc->setBufferCapacity(cap > 0 ? cap : 10000);
    // Pre-populate signalCache_ from the active doc's raw cache so that probe-added
    // signals immediately show historical waveform data even if this scope was
    // not open during the simulation run. Only the active doc's data is exposed
    // here — entries added later will be tagged with that doc's id and continue
    // to read from this scope's signalCache_ until they receive live data.
    if (!schDocs_.empty() && activeSchIdx_ >= 0 && activeSchIdx_ < (int)schDocs_.size()) {
        const auto& src = schDocs_[activeSchIdx_]->rawCache;
        for (const auto& [name, buf] : src)
            if (buf.getCount() > 0)
                sc->injectSignalCache(name, buf);
    }
    scopes_.push_back(std::move(sc));
    return (int)scopes_.size() - 1;
}
void MainViewModel::removeScope(int idx) {
    if (idx < 0 || idx >= (int)scopes_.size()) return;
    scopes_.erase(scopes_.begin() + idx);
    if (activeScope_ >= (int)scopes_.size()) activeScope_ = (int)scopes_.size() - 1;
}

std::vector<int> MainViewModel::scopesOwnedByDoc(int docIdx) const {
    std::vector<int> result;
    if (docIdx < 0 || docIdx >= (int)schDocs_.size()) return result;
    int closingId = schDocs_[docIdx]->id;
    for (int si = 0; si < (int)scopes_.size(); si++) {
        if (scopes_[si]->computeOwnerSchId() == closingId)
            result.push_back(si);
    }
    return result;
}

void MainViewModel::stopAndClearSim() {
    stopBuildAndWait();
    simulator_->stop();
    circuit_ = std::make_unique<Circuit>();
    probes_.clear();
    signalNameToIdx_.clear();
    // Per-doc rawCaches are NOT cleared here (each survives in its owning doc
    // until that doc is itself closed via closeSchDoc).
    computedSigs_.clear();
    diagLog_.clear();
    lastBuiltDocId_ = -1;
    statusMsg_ = "Simulation stopped (doc closed)";
}

void MainViewModel::stopBuildAndWait() {
    cancelBuild_.store(true);
    if (buildThread_.joinable()) buildThread_.join();
    buildPending_.store(false);
}

MainViewModel::~MainViewModel() {
    stopBuildAndWait();
    simulator_->stop();
}

void MainViewModel::requestBuild() {
    if (buildPending_.load()) return;
    cancelBuild_.store(false);
    buildPending_.store(true);
    if (buildThread_.joinable()) buildThread_.join();
    buildThread_ = std::thread([this]() {
        buildFromSchematic();
        buildPending_.store(false);
    });
}


void MainViewModel::autoPopulateScope() {
    if (scopes_.empty()) return;
    ScopeModel& sc0 = *scopes_[0];
    size_t bufCap = sc0.getBufferCapacity();
    *scopes_[0] = ScopeModel();
    sc0.setBufferCapacity(bufCap);
    ScopeModel::resetColorIndex();

    bool hasV = false, hasI = false;
    for (const auto& p : probes_) {
        if (p.type == SignalInfo::NodeVoltage) hasV = true;
        else                                   hasI = true;
    }

    if (hasV && hasI) {
        sc0.insertPlot(0);
        PlotArea* pV = sc0.getPlot(0);
        PlotArea* pI = sc0.getPlot(1);
        if (pV) pV->title = "Voltage";
        if (pI) pI->title = "Current";
    }

    for (const auto& probe : probes_) {
        bool isCurrent = (probe.type == SignalInfo::BranchCurrent);
        int plotIdx = (hasV && hasI && isCurrent) ? 1 : 0;
        PlotArea* plot = sc0.getPlot(plotIdx);
        if (plot)
            plot->addSignal(probe.name, ScopeModel::nextColor(), sc0.getBufferCapacity());
    }
}

void MainViewModel::play() {
    if (!circuit_ || circuit_->components().empty()) {
        statusMsg_ = "No circuit loaded";
        return;
    }
    simulator_->reset();  // stops thread, resets component history and time to 0
    // Scope layout is intentionally preserved across runs (user request).
    // Data clearing is handled by buildFromSchematic when a new circuit is built.
    if (!simulator_->isRunning()) {
        simulator_->start();
    }
    statusMsg_ = "Simulation running";
}

void MainViewModel::pause() {
    simulator_->pause();
    statusMsg_ = "Simulation paused";
}

void MainViewModel::stop() {
    simulator_->stop();
    statusMsg_ = "Simulation stopped";
}

void MainViewModel::reset() {
    diagLog_.clear();
    simulator_->reset();
    // Only clear the active doc's rawCache + its owned scope buffers; other docs'
    // data is preserved.
    if (lastBuiltDocId_ >= 0) {
        int idx = findSchDocById(lastBuiltDocId_);
        if (idx >= 0) schDocs_[idx]->rawCache.clear();
    }
    for (auto& sc : scopes_) {
        for (int pi = 0; pi < sc->plotCount(); pi++) {
            PlotArea* p = sc->getPlot(pi);
            if (!p) continue;
            for (auto& e : p->entries)
                if (e->sourceSchId == lastBuiltDocId_ || e->sourceSchId == -1)
                    e->buffer.clear();
        }
    }
    statusMsg_ = "Simulation reset";
}

void MainViewModel::update() {
    // Drain all available samples from the ring buffer
    SimSample sample;
    while (simulator_->consumeSample(sample)) {
        dispatchSample(sample);
    }

    // Drain all diagnostic events emitted by the simulation thread
    DiagEvent dev;
    while (simulator_->consumeDiagEvent(dev)) {
        if (diagLog_.size() >= kMaxDiagLog)
            diagLog_.erase(diagLog_.begin());  // drop oldest to stay within cap
        diagLog_.push_back(std::move(dev));
    }
}

void MainViewModel::dispatchSample(const SimSample& sample) {
    // Resolve the doc whose simulation produced this sample. Samples flow
    // ONLY into that doc's rawCache and into entries tagged with its id.
    // This is what prevents scope0 (sourceSchId=sch0) from filling with sch1
    // data when the user runs sch1 after viewing sch0 in scope0.
    SchematicDoc* runDoc = nullptr;
    if (lastBuiltDocId_ >= 0) {
        int idx = findSchDocById(lastBuiltDocId_);
        if (idx >= 0) runDoc = schDocs_[idx].get();
    }
    if (!runDoc) return;

    // Feed the running doc's per-sch raw cache (used for backfill / retroCompute).
    for (size_t i = 0; i < sample.values.size() && i < probes_.size(); i++) {
        auto [it, inserted] = runDoc->rawCache.try_emplace(probes_[i].name, runDoc->rawCacheCapacity);
        it->second.push(sample.time, sample.values[i]);
    }

    // Raw signals — only dispatched to entries owned by the running doc.
    // sourceSchId == -1 (untagged, e.g. legacy entries) also accepts the live
    // stream as a backward-compat fallback.
    for (auto& sc : scopes_) {
        for (int pi = 0; pi < sc->plotCount(); pi++) {
            PlotArea* plot = sc->getPlot(pi);
            if (!plot) continue;
            for (auto& entry : plot->entries) {
                if (entry->sourceSchId != -1 && entry->sourceSchId != lastBuiltDocId_)
                    continue;
                auto it = signalNameToIdx_.find(entry->signalName);
                if (it == signalNameToIdx_.end() || it->second >= sample.values.size())
                    continue;
                double valA = sample.values[it->second] * entry->scale;
                double valB = 0.0;
                if (!entry->signalNameB.empty()) {
                    auto itB = signalNameToIdx_.find(entry->signalNameB);
                    if (itB != signalNameToIdx_.end() && itB->second < sample.values.size())
                        valB = sample.values[itB->second];
                }
                entry->buffer.push(sample.time, valA - valB);
            }
        }
    }

    // Computed (virtual) signals: val = sigA*kA + sigB*kB
    for (const auto& cs : computedSigs_) {
        auto itA = signalNameToIdx_.find(cs.sigA);
        if (itA == signalNameToIdx_.end() || itA->second >= sample.values.size())
            continue;
        double val = sample.values[itA->second] * cs.kA;
        if (!cs.sigB.empty()) {
            auto itB = signalNameToIdx_.find(cs.sigB);
            if (itB != signalNameToIdx_.end() && itB->second < sample.values.size())
                val += sample.values[itB->second] * cs.kB;
        }
        for (auto& sc : scopes_) {
            for (int pi = 0; pi < sc->plotCount(); pi++) {
                PlotArea* plot = sc->getPlot(pi);
                if (!plot) continue;
                for (auto& entry : plot->entries) {
                    if (entry->signalName != cs.name) continue;
                    if (entry->sourceSchId != -1 && entry->sourceSchId != lastBuiltDocId_)
                        continue;
                    entry->buffer.push(sample.time, val);
                }
            }
        }
    }
}

void MainViewModel::registerComputedSig(const std::string& name,
                                         const std::string& sigA, double kA,
                                         const std::string& sigB, double kB) {
    for (const auto& cs : computedSigs_)
        if (cs.name == name) return;  // already registered
    computedSigs_.push_back({name, sigA, kA, sigB, kB});
}

void MainViewModel::retroComputeSig(const std::string& name) {
    // Find the ComputedSig definition
    const ComputedSig* cs = nullptr;
    for (const auto& c : computedSigs_)
        if (c.name == name) { cs = &c; break; }
    if (!cs) return;

    // Prefer the active doc's per-sch rawCache (the freshest source for the
    // user's current context); fall back to scope-internal buffers.
    const ScrollingBuffer* bufA = nullptr;
    const ScrollingBuffer* bufB = nullptr;
    if (!schDocs_.empty() && activeSchIdx_ >= 0 && activeSchIdx_ < (int)schDocs_.size()) {
        const auto& rc = schDocs_[activeSchIdx_]->rawCache;
        auto ita = rc.find(cs->sigA);
        if (ita != rc.end() && ita->second.getCount() > 0) bufA = &ita->second;
        if (!cs->sigB.empty()) {
            auto itb = rc.find(cs->sigB);
            if (itb != rc.end() && itb->second.getCount() > 0) bufB = &itb->second;
        }
    }
    for (auto& sc : scopes_) {
        if (!bufA) bufA = sc->findAnySignalBuffer(cs->sigA);
        if (!cs->sigB.empty() && !bufB) bufB = sc->findAnySignalBuffer(cs->sigB);
    }
    if (!bufA) return;
    if (!cs->sigB.empty() && !bufB) return;

    int n = bufA->getCount();
    if (bufB) n = std::min(n, bufB->getCount());

    // Fill all MuxEntries that carry this computed signal's name (all scopes)
    for (auto& sc : scopes_) {
        for (int pi = 0; pi < sc->plotCount(); pi++) {
            PlotArea* plot = sc->getPlot(pi);
            if (!plot) continue;
            for (auto& entry : plot->entries) {
                if (entry->signalName != cs->name) continue;
                entry->buffer.clear();
                for (int i = 0; i < n; i++) {
                    double t   = bufA->getXAt(i);
                    double val = bufA->getYAt(i) * cs->kA;
                    if (bufB) val += bufB->getYAt(i) * cs->kB;
                    entry->buffer.push(t, val);
                }
            }
        }
    }
}

void MainViewModel::applySimConfig(double dt, double tEnd) {
    if (dt <= 0.0 || tEnd <= 0.0) return;

    config_.dt    = dt;
    config_.t_end = tEnd;

    // Storage side: always ratio=1; rebuild buffers with new capacity
    config_.sample_ratio = 1;
    size_t cap = calcBufferCapacity(tEnd, dt, maxStoredSamples_);
    for (auto& sc : scopes_) sc->resizeAllBuffers(cap);

    // Re-initialise simulator with updated config (resets to t=0)
    if (circuit_ && !circuit_->components().empty()) {
        simulator_->stop();
        simulator_->setup(*circuit_, config_, probes_);
    }

    statusMsg_ = "Config applied: dt=" + std::to_string(dt)
               + " t_end=" + std::to_string(tEnd);
}

void MainViewModel::renameSignal(const std::string& oldName, const std::string& newName) {
    if (oldName == newName) return;
    // signalNameToIdx_
    auto it = signalNameToIdx_.find(oldName);
    if (it != signalNameToIdx_.end()) {
        size_t idx = it->second;
        signalNameToIdx_.erase(it);
        signalNameToIdx_[newName] = idx;
    }
    // probes list
    for (auto& p : probes_)
        if (p.name == oldName) p.name = newName;
    // scope: MuxEntry signalName, clear stale labels (all scopes)
    for (auto& sc : scopes_) {
        for (int pi = 0; pi < sc->plotCount(); pi++) {
            PlotArea* plot = sc->getPlot(pi);
            if (!plot) continue;
            for (auto& entry : plot->entries) {
                if (entry->signalName == oldName) {
                    entry->signalName = newName;
                    if (entry->label == oldName || entry->label == newName)
                        entry->label = "";
                }
            }
        }
        sc->renameSignal(oldName, newName);
    }
    // computed sigs
    for (auto& cs : computedSigs_) {
        if (cs.name == oldName) cs.name = newName;
        if (cs.sigA == oldName) cs.sigA = newName;
        if (cs.sigB == oldName) cs.sigB = newName;
    }
    // hover tracking
    if (hoveredSignal_ == oldName) hoveredSignal_ = newName;
    // Per-sch raw caches: rename across all docs (a renamed signal could
    // legitimately exist in any open doc that probed the same node).
    for (auto& d : schDocs_) {
        auto rit = d->rawCache.find(oldName);
        if (rit != d->rawCache.end()) {
            d->rawCache[newName] = std::move(rit->second);
            d->rawCache.erase(rit);
        }
    }
}

void MainViewModel::syncRawCacheToScope(int scopeIdx) {
    if (scopeIdx < 0 || scopeIdx >= (int)scopes_.size()) return;
    if (schDocs_.empty() || activeSchIdx_ < 0 || activeSchIdx_ >= (int)schDocs_.size())
        return;
    for (const auto& [name, buf] : schDocs_[activeSchIdx_]->rawCache)
        if (buf.getCount() > 0)
            scopes_[scopeIdx]->injectSignalCache(name, buf);
}

std::string MainViewModel::displayNameForSchId(int id) const {
    for (const auto& d : schDocs_)
        if (d->id == id) return d->displayName;
    return "";
}

double MainViewModel::tEndForSchId(int id) const {
    for (const auto& d : schDocs_)
        if (d->id == id) return d->hasLastConfig ? d->lastConfig.t_end : -1.0;
    return -1.0;
}

bool MainViewModel::isSimRunning() const { return simulator_->isRunning(); }
bool MainViewModel::isSimPaused()  const { return simulator_->isPaused(); }
double MainViewModel::currentTime() const { return simulator_->currentTime(); }

void MainViewModel::buildFromSchematic() {
    SchematicModel& schematic_ = activeSchDoc().model;
    std::string netlist = schematic_.generateNetlist(schematic_.simCfg);
    if (netlist.empty()) {
        statusMsg_ = "Schematic is empty — add components and a GND symbol first";
        return;
    }
    if (cancelBuild_.load()) return;

    simulator_->stop();
    if (cancelBuild_.load()) return;

    circuit_ = std::make_unique<Circuit>();
    signalNameToIdx_.clear();
    // computedSigs_ intentionally kept: user probes survive across re-runs
    diagLog_.clear();

    NetlistParser parser;
    ParseResult result = parser.parseString(netlist);

    if (!result.success) {
        statusMsg_ = "Schematic build error: " + result.error;
        return;
    }
    if (cancelBuild_.load()) return;

    circuit_ = std::make_unique<Circuit>(std::move(result.circuit));
    config_  = result.config;
    probes_  = result.probes;

    if (!simulator_->setup(*circuit_, config_, probes_)) {
        statusMsg_ = "Failed to setup simulator from schematic";
        return;
    }

    for (size_t i = 0; i < probes_.size(); i++)
        signalNameToIdx_[probes_[i].name] = i;

    // Rename numeric voltage probes (V(N)) to user-assigned net names so that
    // scope entries keep receiving data on re-runs without needing re-probe.
    {
        auto netNameToNode = schematic_.computeNetNameToNodeMap();
        // Also collect NETLABEL names
        {
            auto pinNodeMap = schematic_.computePinNodeMap();
            for (const auto& c : schematic_.comps())
                if (c.typeId == "NETLABEL" && !c.paramValues.empty() && !c.paramValues[0].empty()) {
                    auto it = pinNodeMap.find(SchematicModel::pinKey(c.id, 0));
                    if (it != pinNodeMap.end() && it->second != 0)
                        netNameToNode.emplace(c.paramValues[0], it->second);
                }
        }
        // Reverse map: nodeId → user name
        std::unordered_map<int, std::string> nodeToName;
        for (const auto& [nm, nid] : netNameToNode)
            nodeToName[nid] = nm;

        for (size_t i = 0; i < probes_.size(); i++) {
            if (probes_[i].type != SignalInfo::NodeVoltage) continue;
            const std::string& pn = probes_[i].name;          // e.g. "V(2)"
            if (pn.size() < 4 || pn[0] != 'V' || pn[1] != '(') continue;
            std::string inner = pn.substr(2, pn.size() - 3);  // "2"
            bool isNum = !inner.empty();
            for (char ch : inner) if (!std::isdigit((unsigned char)ch)) { isNum = false; break; }
            if (!isNum) continue;
            try {
                int nid = std::stoi(inner);
                auto it = nodeToName.find(nid);
                if (it != nodeToName.end()) {
                    std::string newName = "V(" + it->second + ")";
                    signalNameToIdx_.erase(pn);
                    signalNameToIdx_[newName] = i;
                    probes_[i].name = newName;
                }
            } catch (...) {}
        }
    }

    if (cancelBuild_.load()) return;

    config_.sample_ratio = 1;
    size_t newCap = calcBufferCapacity(config_.t_end, config_.dt, maxStoredSamples_);

    // Reset only the building doc's per-sch rawCache; other docs keep their
    // last-run data so their owning scopes can keep displaying historical waves.
    int activeId = activeSchDoc().id;
    activeSchDoc().rawCacheCapacity = newCap;
    activeSchDoc().rawCache.clear();

    // Check scope 0 for user content; other scopes are user-managed and always preserved
    bool scope0HasContent = false;
    if (!scopes_.empty()) {
        for (int i = 0; i < scopes_[0]->plotCount() && !scope0HasContent; i++) {
            PlotArea* p = scopes_[0]->getPlot(i);
            if (p && !p->entries.empty()) scope0HasContent = true;
        }
    }

    if (scope0HasContent) {
        // Preserve layout AND data for entries owned by other docs. Only entries
        // tagged with this doc's id (or untagged) get cleared and resized.
        // Always update the scope's per-entry default capacity so that probes
        // taken AFTER this build (on the active doc) get correctly-sized buffers.
        for (auto& sc : scopes_) {
            sc->setBufferCapacity(newCap);
            for (int pi = 0; pi < sc->plotCount(); pi++) {
                PlotArea* p = sc->getPlot(pi);
                if (!p) continue;
                for (auto& e : p->entries) {
                    if (e->sourceSchId == -1 || e->sourceSchId == activeId) {
                        e->buffer = ScrollingBuffer(newCap);  // resize + clear
                    }
                }
            }
        }
    } else {
        // Empty scope 0: auto-populate it; set capacity on all others
        if (!scopes_.empty()) scopes_[0]->setBufferCapacity(newCap);
        autoPopulateScope();
        for (size_t i = 1; i < scopes_.size(); i++) {
            scopes_[i]->setBufferCapacity(newCap);
        }
    }

    // Tag every entry in scope 0 with the active doc's id so ownership routing
    // associates auto-populated probes with the doc that just built.
    {
        if (!scopes_.empty()) {
            ScopeModel& sc0 = *scopes_[0];
            for (int pi = 0; pi < sc0.plotCount(); pi++) {
                PlotArea* p = sc0.getPlot(pi);
                if (!p) continue;
                for (auto& entry : p->entries)
                    if (entry->sourceSchId == -1) entry->sourceSchId = activeId;
            }
        }
    }

    if (cancelBuild_.load()) return;

    // Stamp this build's config onto the doc so scopes owned by this doc can
    // lock their X axis to its t_end (independent of unrelated rebuilds of
    // other docs).
    activeSchDoc().lastConfig    = config_;
    activeSchDoc().hasLastConfig = true;

    lastBuiltDocId_ = activeSchDoc().id;

    char buf[128];
    snprintf(buf, sizeof(buf), "Built from schematic: %zu components, %zu probes",
             circuit_->components().size(), probes_.size());
    statusMsg_ = buf;

    play();
}
