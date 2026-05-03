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
        // Only cache plain (unscaled, single-ended) entries to avoid mixing scaled data
        if (entry->scale != 1.0 || !entry->signalNameB.empty()) continue;
        // Only cache if no other active plot already holds data for this signal
        bool foundElsewhere = false;
        for (int pi = 0; pi < plotCount(); pi++) {
            if (pi == index) continue;
            MuxEntry* e = plots_[pi]->findBySignalName(entry->signalName);
            if (e && e->buffer.getCount() > 0) { foundElsewhere = true; break; }
        }
        if (!foundElsewhere)
            signalCache_[entry->signalName] = entry->buffer;
    }

    plots_.erase(plots_.begin() + index);
    if (selectedPlot_ >= plotCount()) selectedPlot_ = plotCount() - 1;
}

void ScopeModel::addSignalToPlot(int plotIdx, const std::string& name, ImU32 color,
                                  int sourceSchId) {
    PlotArea* p = getPlot(plotIdx);
    if (!p) return;
    if (color == 0) color = nextColor();

    // If the signal is already present in this plot (visible or hidden), just
    // re-show it. DO NOT overwrite the existing buffer with a snapshot from
    // another plot or signalCache_ — the live entry's buffer carries the most
    // recent samples; copying a stale snapshot over it produces a time gap in
    // the waveform after the user re-adds a probe during simulation.
    if (MuxEntry* existing = p->findEntry(name)) {
        existing->visible = true;
        if (sourceSchId != -1) existing->sourceSchId = sourceSchId;
        return;
    }

    // New entry: backfill historical data from another plot or the rescue cache
    // so the freshly-added line shows the waveform that existed before the add.
    const ScrollingBuffer* srcBuf = nullptr;
    for (int pi = 0; pi < plotCount(); pi++) {
        if (pi == plotIdx) continue;
        PlotArea* other = getPlot(pi);
        if (!other) continue;
        MuxEntry* e = other->findBySignalName(name);
        if (e && e->scale == 1.0 && e->signalNameB.empty() && e->buffer.getCount() > 0)
            { srcBuf = &e->buffer; break; }
    }
    if (!srcBuf) {
        auto it = signalCache_.find(name);
        if (it != signalCache_.end() && it->second.getCount() > 0)
            srcBuf = &it->second;
    }

    p->addSignal(name, color, bufferCapacity_);

    if (srcBuf) {
        MuxEntry* newEntry = p->findEntry(name);
        if (newEntry) newEntry->buffer = *srcBuf;
    }
    if (sourceSchId != -1) {
        MuxEntry* newEntry = p->findEntry(name);
        if (newEntry) newEntry->sourceSchId = sourceSchId;
    }
}

void ScopeModel::addSignalToPlot(int plotIdx, const std::string& sigName,
                                  const std::string& label, ImU32 color,
                                  double scale, const std::string& sigNameB,
                                  int sourceSchId) {
    PlotArea* p = getPlot(plotIdx);
    if (!p) return;
    if (color == 0) color = nextColor();

    const std::string& key = label.empty() ? sigName : label;

    // Find existing entry by display key
    MuxEntry* existing = p->findEntry(key);
    if (existing) {
        existing->visible    = true;
        existing->scale      = scale;
        existing->signalNameB = sigNameB;
        if (sourceSchId != -1) existing->sourceSchId = sourceSchId;
        return;
    }

    // Historical copy only for unscaled, single-ended entries (safe to copy verbatim)
    const ScrollingBuffer* srcBuf = nullptr;
    if (scale == 1.0 && sigNameB.empty()) {
        for (int pi = 0; pi < plotCount(); pi++) {
            if (pi == plotIdx) continue;
            MuxEntry* e = getPlot(pi)->findBySignalName(sigName);
            if (e && e->scale == 1.0 && e->signalNameB.empty() && e->buffer.getCount() > 0)
                { srcBuf = &e->buffer; break; }
        }
        if (!srcBuf) {
            auto it = signalCache_.find(sigName);
            if (it != signalCache_.end() && it->second.getCount() > 0)
                srcBuf = &it->second;
        }
    }

    auto m = std::make_unique<MuxEntry>(sigName, color, bufferCapacity_);
    m->label       = label;
    m->scale       = scale;
    m->signalNameB = sigNameB;
    m->sourceSchId = sourceSchId;
    p->entries.push_back(std::move(m));

    if (srcBuf) {
        MuxEntry* newEntry = p->findEntry(key);
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

const ScrollingBuffer* ScopeModel::findAnySignalBuffer(const std::string& signalName) const {
    // Search active plot entries first (includes soft-deleted/hidden entries)
    for (const auto& plot : plots_)
        for (const auto& entry : plot->entries)
            if (entry->signalName == signalName && entry->buffer.getCount() > 0)
                return &entry->buffer;
    // Fall back to rescue cache (populated when a plot is deleted)
    auto it = signalCache_.find(signalName);
    if (it != signalCache_.end() && it->second.getCount() > 0)
        return &it->second;
    return nullptr;
}

void ScopeModel::renameSignal(const std::string& oldName, const std::string& newName) {
    for (auto& plot : plots_)
        for (auto& entry : plot->entries)
            if (entry->signalName == oldName)
                entry->signalName = newName;
    auto it = signalCache_.find(oldName);
    if (it != signalCache_.end()) {
        signalCache_[newName] = std::move(it->second);
        signalCache_.erase(it);
    }
}

void ScopeModel::clearPlotEntries(int plotIdx) {
    PlotArea* p = getPlot(plotIdx);
    if (!p) return;
    // Rescue buffer data into cache before clearing (same logic as removePlot),
    // so signals can be re-added with their historical waveform data intact.
    for (const auto& entry : p->entries) {
        if (entry->buffer.getCount() == 0) continue;
        if (entry->scale != 1.0 || !entry->signalNameB.empty()) continue;
        auto it = signalCache_.find(entry->signalName);
        if (it == signalCache_.end() || it->second.getCount() == 0)
            signalCache_[entry->signalName] = entry->buffer;
    }
    p->entries.clear();
}

ImU32 ScopeModel::nextColor() {
    ImU32 c = s_colors[colorIndex_ % s_colorCount];
    colorIndex_++;
    return c;
}

void ScopeModel::resetColorIndex() {
    colorIndex_ = 0;
}

int ScopeModel::computeOwnerSchId() const {
    int owner = -2;  // sentinel: no entries seen yet
    for (const auto& plot : plots_) {
        for (const auto& entry : plot->entries) {
            if (entry->sourceSchId == -1) return -1;  // any unknown → mixed/shared
            if (owner == -2) owner = entry->sourceSchId;
            else if (owner != entry->sourceSchId) return -1;  // mixed
        }
    }
    return (owner == -2) ? -1 : owner;
}
