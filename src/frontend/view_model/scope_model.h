#pragma once
#include "view_model/scrolling_buffer.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <memory>

// A single signal bound to a plot
struct MuxEntry {
    std::string signalName;
    ImU32 color;
    bool visible = true;
    ScrollingBuffer buffer;

    MuxEntry(const std::string& name, ImU32 col)
        : signalName(name), color(col), buffer(20000) {}
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

    void addSignal(const std::string& name, ImU32 color) {
        if (!findEntry(name))
            entries.push_back(std::make_unique<MuxEntry>(name, color));
    }

    void removeSignal(const std::string& name) {
        entries.erase(std::remove_if(entries.begin(), entries.end(),
            [&](const std::unique_ptr<MuxEntry>& e) { return e->signalName == name; }),
            entries.end());
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

    // Find a MuxEntry across all plots by signal name (for data push)
    std::vector<MuxEntry*> findAllEntries(const std::string& name);

    static ImU32 nextColor();
    static void resetColorIndex();

private:
    std::vector<std::unique_ptr<PlotArea>> plots_;
    int selectedPlot_ = 0;
    static int colorIndex_;

    std::string makePlotTitle(int index);
};
