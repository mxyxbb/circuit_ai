#include "view_model/scope_model.h"
#include <algorithm>
#include <imgui.h>

// Predefined color palette for signals
static const ImU32 s_colors[] = {
    IM_COL32(255, 100, 100, 255),  // red
    IM_COL32(100, 255, 100, 255),  // green
    IM_COL32(100, 150, 255, 255),  // blue
    IM_COL32(255, 255, 100, 255),  // yellow
    IM_COL32(255, 100, 255, 255),  // magenta
    IM_COL32(100, 255, 255, 255),  // cyan
    IM_COL32(255, 180, 100, 255),  // orange
    IM_COL32(180, 100, 255, 255),  // purple
};
static constexpr int s_colorCount = sizeof(s_colors) / sizeof(s_colors[0]);

int ScopeModel::colorIndex_ = 0;

ScopeModel::ScopeModel() {
    // Start with one empty plot
    plots_.push_back(std::make_unique<PlotArea>("Plot 0"));
}

PlotArea* ScopeModel::getPlot(int index) {
    if (index < 0 || index >= plotCount()) return nullptr;
    return plots_[index].get();
}

const PlotArea* ScopeModel::getPlot(int index) const {
    if (index < 0 || index >= plotCount()) return nullptr;
    return plots_[index].get();
}

std::string ScopeModel::makePlotTitle(int index) {
    return "Plot " + std::to_string(index);
}

int ScopeModel::insertPlot(int insertAfterIdx, const std::string& title) {
    int insertAt = insertAfterIdx + 1;
    if (insertAt < 0) insertAt = 0;
    if (insertAt > plotCount()) insertAt = plotCount();

    std::string t = title.empty() ? makePlotTitle(insertAt) : title;
    auto newPlot = std::make_unique<PlotArea>(t);
    plots_.insert(plots_.begin() + insertAt, std::move(newPlot));

    // Keep selectedPlot_ pointing at the same logical plot after index shift
    if (selectedPlot_ >= insertAt) selectedPlot_++;

    // Re-number titles
    for (int i = 0; i < plotCount(); i++) {
        if (plots_[i]->title.substr(0, 5) == "Plot ")
            plots_[i]->title = makePlotTitle(i);
    }
    return insertAt;
}

void ScopeModel::removePlot(int index) {
    if (plotCount() <= 1) return; // keep at least one
    if (index < 0 || index >= plotCount()) return;

    // Rescue buffer data for signals that exist nowhere else, so they can
    // be re-added to a new plot later and still show historical waveforms.
    PlotArea* dying = plots_[index].get();
    for (const auto& entry : dying->entries) {
        if (entry->buffer.getCount() == 0) continue;
        // Only cache if no other active plot already holds data for this signal
        bool foundElsewhere = false;
        for (int pi = 0; pi < plotCount(); pi++) {
            if (pi == index) continue;
            MuxEntry* e = plots_[pi]->findEntry(entry->signalName);
            if (e && e->buffer.getCount() > 0) { foundElsewhere = true; break; }
        }
        if (!foundElsewhere)
            signalCache_[entry->signalName] = entry->buffer;
    }

    plots_.erase(plots_.begin() + index);
    if (selectedPlot_ >= plotCount()) selectedPlot_ = plotCount() - 1;
}

void ScopeModel::addSignalToPlot(int plotIdx, const std::string& name, ImU32 color) {
    PlotArea* p = getPlot(plotIdx);
    if (!p) return;

    // Find historical buffer data from same signal: first search active plots,
    // then fall back to the rescue cache (populated when plots are deleted).
    const ScrollingBuffer* srcBuf = nullptr;
    for (int pi = 0; pi < plotCount(); pi++) {
        if (pi == plotIdx) continue;
        PlotArea* other = getPlot(pi);
        if (!other) continue;
        MuxEntry* e = other->findEntry(name);
        if (e && e->buffer.getCount() > 0) { srcBuf = &e->buffer; break; }
    }
    if (!srcBuf) {
        auto it = signalCache_.find(name);
        if (it != signalCache_.end() && it->second.getCount() > 0)
            srcBuf = &it->second;
    }

    p->addSignal(name, color, bufferCapacity_);

    // Copy historical data so the new plot shows existing waveform immediately
    if (srcBuf) {
        MuxEntry* newEntry = p->findEntry(name);
        if (newEntry) newEntry->buffer = *srcBuf;
    }
}

void ScopeModel::removeSignalFromPlot(int plotIdx, const std::string& name) {
    PlotArea* p = getPlot(plotIdx);
    if (p) p->removeSignal(name);
}

void ScopeModel::clearAllBuffers() {
    for (auto& plot : plots_)
        for (auto& entry : plot->entries)
            entry->buffer.clear();
    signalCache_.clear();
}

void ScopeModel::resizeAllBuffers(size_t newCapacity) {
    bufferCapacity_ = newCapacity;
    for (auto& plot : plots_)
        for (auto& entry : plot->entries)
            entry->buffer = ScrollingBuffer(newCapacity);  // rebuild with new capacity
}

void ScopeModel::autoFitAll() {
    for (auto& plot : plots_)
        plot->autoFitY = true;
}

std::vector<MuxEntry*> ScopeModel::findAllEntries(const std::string& name) {
    std::vector<MuxEntry*> result;
    for (auto& plot : plots_)
        for (auto& entry : plot->entries)
            if (entry->signalName == name)
                result.push_back(entry.get());
    return result;
}

ImU32 ScopeModel::nextColor() {
    ImU32 c = s_colors[colorIndex_ % s_colorCount];
    colorIndex_++;
    return c;
}

void ScopeModel::resetColorIndex() {
    colorIndex_ = 0;
}
