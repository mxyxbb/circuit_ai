#pragma once
#include "view_model/scrolling_buffer.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

// A single signal bound to a plot
struct MuxEntry {
    std::string signalName;
    ImU32 color;
    bool visible = true;
    ScrollingBuffer buffer;

    MuxEntry(const std::string& name, ImU32 col, size_t cap = 20000)
        : signalName(name), color(col), buffer(cap) {}
};

// A single coordinate area (one ImPlot)
struct PlotArea {
    std::vector<std::unique_ptr<MuxEntry>> entries;
    std::string title;
    bool autoFitY = true;

    explicit PlotArea(const std::string& t) : title(t) {}

    MuxEntry* findEntry(const std::string& name) {
        for (auto& e : entries)
            if (e->signalName == name) return e.get();
        return nullptr;
    }

    void addSignal(const std::string& name, ImU32 color, size_t capacity = 20000) {
        MuxEntry* e = findEntry(name);
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

    void addSignalToPlot(int plotIdx, const std::string& name, ImU32 color);
    void removeSignalFromPlot(int plotIdx, const std::string& name);

    void clearAllBuffers();
    void autoFitAll();

    // Buffer capacity management (set based on t_end/dt before populating)
    void setBufferCapacity(size_t cap) { bufferCapacity_ = cap; }
    size_t getBufferCapacity() const { return bufferCapacity_; }
    // Rebuild all existing MuxEntry buffers with a new capacity (used after config changes)
    void resizeAllBuffers(size_t newCapacity);

    // Find a MuxEntry across all plots by signal name (for data push)
    std::vector<MuxEntry*> findAllEntries(const std::string& name);

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

    std::string makePlotTitle(int index);
};
