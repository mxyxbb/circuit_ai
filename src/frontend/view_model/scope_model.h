#pragma once
#include "view_model/scrolling_buffer.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <cstdint>
#include <memory>
#include <unordered_map>

// Render-side decimation cache, owned by MuxEntry but only mutated by ScopeView.
// Holds time-bucketed min/max extremes plus the assembled output points so that
// frame-to-frame rendering only needs to fold in samples that arrived since the
// previous frame. Invalidated by view-window changes, ring-buffer wrap, or a
// regression in source sample count (e.g. buffer cleared / replaced).
struct DecimCache {
    // Cache key — when any of these change vs. the current call, recompute fully.
    double xMin = 0.0;
    double xMax = 0.0;
    int    nBuckets = 0;
    // Source-buffer state captured the last time we processed it.
    int    lastSrcCount      = -1;   // -1 sentinel: "uninitialised, force full pass"
    int    lastSrcOffset     = 0;
    int    lastSrcGeneration = -1;   // detects clear()+refill that didn't shrink count
    // Per-bucket extremes (length nBuckets each). Time-based bucketing keeps
    // boundaries stable as new samples arrive, which is what makes incremental
    // updates correct.
    std::vector<double>  bMinX, bMinY, bMaxX, bMaxY;
    std::vector<uint8_t> bUsed;
    // Edge samples (one just outside each side of the visible window) so the
    // line touches the axes instead of stopping inside the plot.
    bool   hasLeft  = false;
    bool   hasRight = false;
    double leftX = 0.0,  leftY = 0.0;
    double rightX = 0.0, rightY = 0.0;
    // Assembled output passed straight to ImPlot::PlotLine.
    std::vector<double> outX, outY;
    bool                outDirty = true;
};

// A single signal bound to a plot
struct MuxEntry {
    std::string signalName;   // underlying data source (key for dispatchSample)
    std::string label;        // display/legend key; "" = use signalName
    std::string signalNameB;  // second signal for differential: push valA - valB
    double scale    = 1.0;    // multiply raw value before push
    ImU32  color;
    bool   visible  = true;
    int    sourceSchId = -1;  // SchematicDoc::id this signal originated from; -1 = unknown
    ScrollingBuffer buffer;
    DecimCache      decim;    // render-only; safe to copy/reset along with buffer

    MuxEntry(const std::string& name, ImU32 col, size_t cap = 20000)
        : signalName(name), color(col), buffer(cap) {}

    // Unique display key per plot
    const std::string& effectiveLabel() const {
        return label.empty() ? signalName : label;
    }
};

// A single coordinate area (one ImPlot)
struct PlotArea {
    std::vector<std::unique_ptr<MuxEntry>> entries;
    std::string title;
    bool autoFitY = true;

    explicit PlotArea(const std::string& t) : title(t) {}

    // Find by display key (label if set, else signalName)
    MuxEntry* findEntry(const std::string& key) {
        for (auto& e : entries)
            if (e->effectiveLabel() == key) return e.get();
        return nullptr;
    }

    // Find by underlying signalName regardless of label (for data dispatch)
    MuxEntry* findBySignalName(const std::string& name) {
        for (auto& e : entries)
            if (e->signalName == name) return e.get();
        return nullptr;
    }

    void addSignal(const std::string& name, ImU32 color, size_t capacity = 20000) {
        MuxEntry* e = findEntry(name);  // key == name when no label
        if (e) {
            e->visible = true;  // re-show hidden signal (preserves buffer data)
        } else {
            entries.push_back(std::make_unique<MuxEntry>(name, color, capacity));
        }
    }

    void removeSignal(const std::string& name) {
        MuxEntry* e = findEntry(name);
        if (e) e->visible = false;  // hide instead of destroy to preserve data
    }
};

class ScopeModel {
public:
    ScopeModel();

    int plotCount() const { return static_cast<int>(plots_.size()); }
    PlotArea* getPlot(int index);
    const PlotArea* getPlot(int index) const;

    // Insert a new plot; returns its index
    int insertPlot(int insertAfterIdx, const std::string& title = "");
    void removePlot(int index);
    int selectedPlot() const { return selectedPlot_; }
    void setSelectedPlot(int idx) { selectedPlot_ = idx; }

    // Add signal with optional label, scale, and differential second signal.
    // sourceSchId tags the entry with the SchematicDoc::id of the originating schematic
    // (used by scope-ownership routing). -1 = unknown / leave existing tag untouched.
    void addSignalToPlot(int plotIdx, const std::string& sigName, ImU32 color,
                         int sourceSchId = -1);
    void addSignalToPlot(int plotIdx, const std::string& sigName,
                         const std::string& label, ImU32 color,
                         double scale = 1.0,
                         const std::string& sigNameB = "",
                         int sourceSchId = -1);
    void removeSignalFromPlot(int plotIdx, const std::string& name);

    // Owner sch id consensus across all entries in this scope.
    // Returns the unique sourceSchId if every entry agrees on a non-(-1) value,
    // else -1 (mixed sources or no entries).
    int  computeOwnerSchId() const;

    void clearAllBuffers();
    void autoFitAll();

    // Buffer capacity management (set based on t_end/dt before populating)
    void setBufferCapacity(size_t cap) { bufferCapacity_ = cap; }
    size_t getBufferCapacity() const { return bufferCapacity_; }
    // Rebuild all existing MuxEntry buffers with a new capacity (used after config changes)
    void resizeAllBuffers(size_t newCapacity);

    // Find a MuxEntry across all plots by signal name (for data push)
    std::vector<MuxEntry*> findAllEntries(const std::string& name);

    // Find the first available buffer for a signal, searching both active plot
    // entries and the rescue cache (populated when plots are deleted).
    const ScrollingBuffer* findAnySignalBuffer(const std::string& signalName) const;

    // Rename a signal across all MuxEntry signalNames and the rescue cache.
    void renameSignal(const std::string& oldName, const std::string& newName);

    // Clear all signal entries from a single plot (without removing the plot).
    void clearPlotEntries(int plotIdx);

    // Inject an entry into the rescue cache (used by MainViewModel to pre-populate
    // a new scope from the global raw-signal cache so backfill works immediately).
    // Always overwrites: rawCache_ is the freshest source, so a stale stash here
    // (e.g. from an earlier removePlot) would otherwise corrupt later retroComputeSig
    // refills and signal re-add backfill with frozen data.
    void injectSignalCache(const std::string& name, const ScrollingBuffer& buf) {
        signalCache_[name] = buf;
    }

    // Pending load block: raw text of an XSCOPE block that was read from file
    // before the corresponding ScopeView was created. ScopeView reads and clears
    // this on its first render so the state is applied correctly.
    void        setPendingLoadBlock(const std::string& s, int sourceSchId = -1) {
        pendingLoadBlock_ = s;
        pendingLoadSchId_ = sourceSchId;
    }
    bool        hasPendingLoadBlock() const { return !pendingLoadBlock_.empty(); }
    int         pendingLoadSchId() const { return pendingLoadSchId_; }
    std::string takePendingLoadBlock() {
        std::string s = std::move(pendingLoadBlock_);
        pendingLoadBlock_.clear();
        return s;
    }

    static ImU32 nextColor();
    static void resetColorIndex();

private:
    std::vector<std::unique_ptr<PlotArea>> plots_;
    int selectedPlot_ = 0;
    size_t bufferCapacity_ = 20000;
    static int colorIndex_;

    // Rescue cache: preserves buffer data when a plot is deleted so that
    // signals can still be re-added to new plots with historical waveform data.
    std::unordered_map<std::string, ScrollingBuffer> signalCache_;

    std::string pendingLoadBlock_;
    int         pendingLoadSchId_ = -1;
    std::string makePlotTitle(int index);
};
