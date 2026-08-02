#include "views/scope_view.h"
#include "view_model/main_view_model.h"
#include "view_model/scope_model.h"
#include "view_model/fft.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#endif

ScopeView::ScopeView(int scopeIdx)
    : BaseView(scopeIdx == 0 ? "Scope" : "Scope " + std::to_string(scopeIdx)),
      scopeIdx_(scopeIdx) {
    // Scopes default closed: the user (or .sch load) opens them explicitly.
    // Without this, scope 0 auto-pops on the second app launch because
    // winstate.txt no longer persists scope visibility (scopes are per-sch).
    visible_ = false;
}

// ─────────────────────── Feature-preserving decimation (incremental cache) ────
//
// Each MuxEntry owns a DecimCache that holds:
//   • per-bucket (min,max) extremes for the visible window
//   • the assembled outX/outY arrays passed straight to ImPlot
//   • the source-buffer count/offset captured the last time we processed it
//
// Buckets are TIME-based: width = (xMax-xMin)/nBuckets. That keeps boundaries
// stable as new samples arrive — only the bucket(s) that the new samples fall
// into need to be updated. Index-based bucketing (the original implementation)
// would have shifted boundaries on every sample, defeating the cache.
//
// Cache invalidation:
//   • view changed (xMin / xMax / nBuckets differ from cache key) → full pass
//   • source count regressed (buffer cleared / replaced) → full pass
//   • ring-buffer wrap (count == capacity, so old samples drop off) → full pass
//     each frame; no extra cost beyond the original implementation, but we
//     give up the incremental win for the duration of the wrap
//
// On the fast path (view unchanged, no wrap, count grew by k) we touch O(k)
// samples instead of the O(n) the original implementation walked every frame.

static void decimScanRange(
    const double* xs, const double* ys, int s, int e,
    double xMin, double xMax, double bw, int nBuckets,
    DecimCache& c)
{
    for (int i = s; i < e; i++) {
        double x = xs[i];
        double y = ys[i];
        if (x < xMin) {
            // Track latest sample on the left side as the left edge so the
            // rendered line touches the axis instead of starting inside the plot.
            if (!c.hasLeft || x > c.leftX) {
                c.hasLeft = true; c.leftX = x; c.leftY = y;
            }
            continue;
        }
        if (x > xMax) {
            if (!c.hasRight || x < c.rightX) {
                c.hasRight = true; c.rightX = x; c.rightY = y;
            }
            continue;
        }
        int b = (int)((x - xMin) / bw);
        if (b < 0) b = 0;
        if (b >= nBuckets) b = nBuckets - 1;
        if (!c.bUsed[b]) {
            c.bMinX[b] = x; c.bMinY[b] = y;
            c.bMaxX[b] = x; c.bMaxY[b] = y;
            c.bUsed[b] = 1;
        } else {
            if (y < c.bMinY[b]) { c.bMinY[b] = y; c.bMinX[b] = x; }
            if (y > c.bMaxY[b]) { c.bMaxY[b] = y; c.bMaxX[b] = x; }
        }
    }
}

static int decimateCached(
    const double* xd, const double* yd, int count, int offset, int capacity,
    int generation,
    double xMin, double xMax, int maxPts,
    DecimCache& c)
{
    if (count <= 0 || maxPts < 2 || xMax <= xMin) {
        c.outX.clear(); c.outY.clear();
        return 0;
    }
    int nBuckets = std::max(1, maxPts / 2);
    double bw = (xMax - xMin) / (double)nBuckets;
    if (bw <= 0.0) {
        c.outX.clear(); c.outY.clear();
        return 0;
    }

    bool wrapped       = (count == capacity);
    bool viewChanged   = (c.xMin != xMin) || (c.xMax != xMax) || (c.nBuckets != nBuckets);
    bool countRegress  = (count < c.lastSrcCount);
    bool genChanged    = (c.lastSrcGeneration != generation);
    // A wrapped (full) ring buffer normally forces an O(n) re-scan every frame,
    // because incremental append can't track samples that dropped off the left.
    // But when the buffer content is identical to last frame — same count, same
    // offset, same generation (i.e. the sim is paused/finished and no new sample
    // has been pushed) — the cache is still valid, so we skip the rescan. This is
    // what removes the idle lag on a scope holding a very large (capacity-full)
    // buffer, e.g. after a run with a tiny dt.
    bool contentSame   = (count == c.lastSrcCount) && (offset == c.lastSrcOffset) && !genChanged;
    bool fullPass      = viewChanged || countRegress || genChanged || (c.lastSrcCount < 0)
                       || (wrapped && !contentSame);

    if (fullPass) {
        c.xMin = xMin; c.xMax = xMax; c.nBuckets = nBuckets;
        c.bMinX.assign(nBuckets, 0.0);
        c.bMinY.assign(nBuckets, 0.0);
        c.bMaxX.assign(nBuckets, 0.0);
        c.bMaxY.assign(nBuckets, 0.0);
        c.bUsed.assign(nBuckets, 0);
        c.hasLeft = c.hasRight = false;
        // Walk the buffer in chronological order so edge tracking stays correct.
        if (offset >= count) {
            decimScanRange(xd, yd, 0, count, xMin, xMax, bw, nBuckets, c);
        } else {
            decimScanRange(xd, yd, offset, capacity, xMin, xMax, bw, nBuckets, c);
            decimScanRange(xd, yd, 0, offset,        xMin, xMax, bw, nBuckets, c);
        }
        c.lastSrcCount      = count;
        c.lastSrcOffset     = offset;
        c.lastSrcGeneration = generation;
        c.outDirty          = true;
    } else if (count > c.lastSrcCount) {
        // Incremental: only fold in samples appended since the previous frame.
        // Pre-wrap layout means physical indices [lastSrcCount, count) are the
        // newcomers. We forced fullPass for the wrapped case above, so this
        // branch never sees a wrapped buffer.
        decimScanRange(xd, yd, c.lastSrcCount, count, xMin, xMax, bw, nBuckets, c);
        c.lastSrcCount      = count;
        c.lastSrcOffset     = offset;
        c.lastSrcGeneration = generation;
        c.outDirty          = true;
    }

    if (c.outDirty) {
        c.outX.clear();
        c.outY.clear();
        c.outX.reserve(nBuckets * 2 + 2);
        c.outY.reserve(nBuckets * 2 + 2);
        if (c.hasLeft) {
            c.outX.push_back(c.leftX); c.outY.push_back(c.leftY);
        }
        for (int b = 0; b < nBuckets; b++) {
            if (!c.bUsed[b]) continue;
            // Emit min and max in chronological (X) order so the line follows
            // the true waveform shape inside each bucket.
            double aX = c.bMinX[b], aY = c.bMinY[b];
            double zX = c.bMaxX[b], zY = c.bMaxY[b];
            if (aX > zX) { std::swap(aX, zX); std::swap(aY, zY); }
            c.outX.push_back(aX); c.outY.push_back(aY);
            if (aX != zX) {
                c.outX.push_back(zX); c.outY.push_back(zY);
            }
        }
        if (c.hasRight) {
            c.outX.push_back(c.rightX); c.outY.push_back(c.rightY);
        }
        c.outDirty = false;
    }
    return (int)c.outX.size();
}

// 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ Helpers 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
void ScopeView::ensurePlotYStates(int count) {
    while ((int)plotYStates_.size() < count)
        plotYStates_.push_back({-1.0, 1.0, false});
}

// Insert a plot and keep plotYStates_ positionally in sync so existing
// plots' Y ranges are not disturbed by the index shift.
//
// Root cause of the original bug: ScopeModel re-numbers plot titles on
// every insert/remove.  ImPlot uses the title string as its internal ID 鈥?// once a title changes, ImPlot treats the plot as brand-new and resets its
// cached axis limits to defaults.  Fixing the positional mapping in
// plotYStates_ is necessary but not sufficient; we also need to set
// forceSet=true on every displaced plot so that SetupAxisLimits(Always)
// is called next frame and the correct limits are explicitly restored.
void ScopeView::insertPlot(ScopeModel& scope, int insertAfterIdx) {
    int prevCount = scope.plotCount();
    ensurePlotYStates(prevCount);
    int newIdx = scope.insertPlot(insertAfterIdx);
    if (scope.plotCount() > prevCount) {
        // Splice in a fresh default state for the new (empty) plot.
        plotYStates_.insert(plotYStates_.begin() + newIdx, {-1.0, 1.0, false});
        // All plots at positions > newIdx have been renamed (title +1).
        // Force-restore their Y limits so ImPlot's stale-ID reset is overridden.
        for (int i = newIdx + 1; i < (int)plotYStates_.size(); i++)
            plotYStates_[i].forceSet = true;
        plotStructureChanged_ = true;
    }
}

// Remove a plot and keep plotYStates_ positionally in sync so remaining
// plots' Y ranges are not disturbed by the index shift.
void ScopeView::removePlot(ScopeModel& scope, int index) {
    if (scope.plotCount() <= 1) return;
    ensurePlotYStates(scope.plotCount());
    scope.removePlot(index);
    if (index < (int)plotYStates_.size())
        plotYStates_.erase(plotYStates_.begin() + index);
    // All plots at positions >= index have been renamed (title -1).
    // Force-restore their Y limits so ImPlot's stale-ID reset is overridden.
    for (int i = index; i < (int)plotYStates_.size(); i++)
        plotYStates_[i].forceSet = true;
    plotStructureChanged_ = true;
}

// 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ Smart axis formatting 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
int ScopeView::niceTickRound(int n) {
    // 1-2-5 decade series (with 4, 8 extensions)
    static const int nice[] = {2,4,5,8,10,15,20,25,30,40,50,60,80,100};
    if (n <= nice[0]) return nice[0];
    for (int i = 1; i < (int)(sizeof(nice)/sizeof(nice[0])); i++) {
        if (nice[i] > n) return nice[i-1];
    }
    return nice[(int)(sizeof(nice)/sizeof(nice[0])) - 1];
}

ScopeView::AxisFmtParams ScopeView::computeAxisFmt(
    double rangeMin, double rangeMax, float plotWidthPx)
{
    AxisFmtParams params;

    double range = std::abs(rangeMax - rangeMin);

    // Degenerate range
    if (range < 1e-30) {
        params.decimals = 6;
        return params;
    }

    // Step 1-2: initial tick estimate
    double tickStep = range / 5.0;

    // Step 3: decimal precision from initial tick step
    int decimals = 0;
    if (tickStep > 0 && tickStep < 1.0)
        decimals = (int)std::ceil(-std::log10(tickStep));
    if (decimals > 6) decimals = 6;

    // Step 4: label character length from sample value
    char sample[32];
    snprintf(sample, sizeof(sample), "%.*f", decimals, rangeMin + tickStep);
    int labelLen = (int)std::strlen(sample);
    if (labelLen < 1) labelLen = 1;

    // Step 5: max ticks that fit in plot width, rounded to nice 2-5 series
    float charWidth = ImGui::CalcTextSize("0").x;
    if (charWidth < 1.0f) charWidth = 7.0f;
    int rawTicks = (int)(plotWidthPx / ((float)labelLen * charWidth));
    int maxTicks = niceTickRound(rawTicks);

    // Step 6: recalculate tick step and decimals from nice tick count
    double finalTickStep = range / (double)maxTicks;
    decimals = 0;
    if (finalTickStep > 0 && finalTickStep < 1.0)
        decimals = (int)std::ceil(-std::log10(finalTickStep));
    if (decimals > 6) decimals = 6;

    // Step 7: scientific notation scaling for very small values
    double maxAbsVal = std::max(std::abs(rangeMin), std::abs(rangeMax));
    if (maxAbsVal > 0 && maxAbsVal < 0.06) {
        int exponent = (int)std::floor(std::log10(maxAbsVal));
        double scaleFactor = std::pow(10.0, (double)exponent);

        double scaledTickStep = finalTickStep / scaleFactor;
        decimals = 0;
        if (scaledTickStep > 0 && scaledTickStep < 1.0)
            decimals = (int)std::ceil(-std::log10(scaledTickStep));
        if (decimals > 6) decimals = 6;

        params.scaleFactor  = scaleFactor;
        params.exponent     = exponent;
        params.decimals     = decimals;
        params.useScaledSci = true;
        snprintf(params.annotation, sizeof(params.annotation),
                 "x 1e%d", exponent);
    } else {
        params.decimals     = decimals;
    }

    return params;
}

int ScopeView::axisFormatterCallback(double value, char* buff, int size, void* user_data) {
    auto* p = static_cast<const AxisFmtParams*>(user_data);
    if (p->useScaledSci)
        return snprintf(buff, (size_t)size, "%.*f", p->decimals, value / p->scaleFactor);
    else
        return snprintf(buff, (size_t)size, "%.*f", p->decimals, value);
}

// 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ Undo stack 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
void ScopeView::pushSnapshot(int plotCount) {
    ensurePlotYStates(plotCount);
    ZoomSnapshot snap;
    snap.xMin = xLinkMin_;
    snap.xMax = xLinkMax_;
    snap.plotY.resize(plotCount);
    for (int i = 0; i < plotCount; i++)
        snap.plotY[i] = {plotYStates_[i].yMin, plotYStates_[i].yMax};
    zoomHistory_.push_back(std::move(snap));
    if (zoomHistory_.size() > 64)
        zoomHistory_.erase(zoomHistory_.begin());
}

void ScopeView::applyUndo() {
    if (zoomHistory_.empty()) return;
    const ZoomSnapshot& snap = zoomHistory_.back();
    xLinkMin_ = snap.xMin;
    xLinkMax_ = snap.xMax;
    for (int i = 0; i < (int)snap.plotY.size() && i < (int)plotYStates_.size(); i++) {
        plotYStates_[i].yMin     = snap.plotY[i].yMin;
        plotYStates_[i].yMax     = snap.plotY[i].yMax;
        plotYStates_[i].forceSet = true;
    }
    zoomHistory_.pop_back();
}

// 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ Data-driven auto-fit 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
// Does NOT use ImPlot's AutoFit flag 鈥?gives full control over axis limits
// and ensures correct undo behaviour.
// allData = true  鈫?scan entire stored buffer regardless of visible X range
//                   (used by the "Auto-Fit All" toolbar button)
// allData = false 鈫?scan only the currently visible X range
//                   (used by the right-click "Auto-Fit This Plot" action)
void ScopeView::computeAutoFitPlot(MainViewModel& vm, int plotIndex, bool allData) {
    ScopeModel& scope = vm.scope(scopeIdx_);
    PlotArea*   plot  = scope.getPlot(plotIndex);
    if (!plot) return;

    double yMin =  DBL_MAX;
    double yMax = -DBL_MAX;

    for (auto& entry : plot->entries) {
        if (!entry->visible) continue;
        int count  = entry->buffer.getCount();
        int offset = entry->buffer.getOffset();
        if (count <= 0) continue;
        const double* xd = entry->buffer.getXData();
        const double* yd = entry->buffer.getYData();

        auto scanSeg = [&](int s, int e) {
            const double* xs = xd + s;
            const double* ys = yd + s;
            int len = e - s;
            int lo = 0, hi = len;
            if (!allData) {
                lo = (int)(std::lower_bound(xs, xs + len, xLinkMin_) - xs);
                hi = (int)(std::upper_bound(xs, xs + len, xLinkMax_) - xs);
            }
            for (int i = lo; i < hi; i++) {
                if (ys[i] < yMin) yMin = ys[i];
                if (ys[i] > yMax) yMax = ys[i];
            }
        };

        if (offset >= count) {
            scanSeg(0, count);
        } else {
            scanSeg(offset, count);
            scanSeg(0, offset);
        }
    }

    if (yMin > yMax) { yMin = -1.0; yMax = 1.0; }  // no data

    double margin = (yMax - yMin) * 0.05;
    if (margin < 1e-12)
        margin = std::max(std::abs(yMax) * 0.05, 1e-10);

    ensurePlotYStates(plotIndex + 1);
    plotYStates_[plotIndex] = {yMin - margin, yMax + margin, true};
}

void ScopeView::computeAutoFitAll(MainViewModel& vm) {
    pushSnapshot(vm.scope(scopeIdx_).plotCount());
    // Step 1: reset X axis to full simulation range
    xLinkMin_ = 0.0;
    xLinkMax_ = vm.simConfig().t_end;
    // Step 2: auto-fit Y for each plot (scan all stored data)
    for (int i = 0; i < vm.scope(scopeIdx_).plotCount(); i++)
        computeAutoFitPlot(vm, i, /*allData=*/true);
}

// 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ Main render 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
void ScopeView::render(MainViewModel& vm) {
    // The FFT window is an independent top-level window that MUST be submitted
    // every frame while open — BEFORE any of the scope window's early returns.
    // If it were submitted only at the end of render(), then when the FFT and
    // scope are docked together as tabs, whichever is the background tab fails
    // to submit (scope's Begin returns false → early return), and the dock node
    // oscillates its selected tab every frame → high-frequency flicker.
    if (fftOpen_) renderFftWindow(vm);

    if (!visible_) return;
    // Pending geometry from per-sch load takes priority over imgui.ini.
    if (pendingGeoSet_) {
        pendingGeoSet_ = false;
        ImGui::SetNextWindowPos(pendingPos_,   ImGuiCond_Always);
        ImGui::SetNextWindowSize(pendingSize_, ImGuiCond_Always);
        centerOnFirstRender_ = false;
    } else if (centerOnFirstRender_) {
        centerOnFirstRender_ = false;
        ImGuiWindowSettings* ws = ImGui::FindWindowSettingsByID(ImHashStr(title_.c_str()));
        if (!ws) {
            // First-time render of a freshly created scope (no imgui.ini entry
            // yet). Pick a comfortable default so the plot area is large enough
            // for ImPlot's zoom drag / scroll-wheel zoom to register. The old
            // default of 120x80 px squeezed the plot below ImPlot's interaction
            // thresholds, making zoom appear broken on every new scope.
            ImGuiViewport* vp = ImGui::GetMainViewport();
            ImVec2 sz = {720.0f, 460.0f};
            ImGui::SetNextWindowPos({
                vp->WorkPos.x + (vp->WorkSize.x - sz.x) * 0.5f,
                vp->WorkPos.y + (vp->WorkSize.y - sz.y) * 0.5f
            }, ImGuiCond_Always);
            ImGui::SetNextWindowSize(sz, ImGuiCond_Always);
        }
    }
    ImGuiWindowFlags winFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    if (!ImGui::Begin(title_.c_str(), &visible_, winFlags)) {
        ImGui::End();
        return;
    }

    // Title-bar tooltip: list every distinct source schematic this scope contains,
    // one per line. The window's Begin item is the title bar; IsItemHovered()
    // there fires on title-bar hover, including when the window is docked as a tab.
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)
        && !vm.isBuildPending()) {
        const ScopeModel& sc = vm.scope(scopeIdx_);
        std::vector<int> seenIds;
        for (int pi = 0; pi < sc.plotCount(); pi++) {
            const PlotArea* p = sc.getPlot(pi);
            if (!p) continue;
            for (const auto& e : p->entries) {
                int id = e->sourceSchId;
                if (id < 0) continue;
                if (std::find(seenIds.begin(), seenIds.end(), id) == seenIds.end())
                    seenIds.push_back(id);
            }
        }
        if (!seenIds.empty() && ImGui::BeginTooltip()) {
            ImGui::TextDisabled("Source schematics:");
            for (int id : seenIds) {
                std::string nm = vm.displayNameForSchId(id);
                if (nm.empty()) nm = "Sch#" + std::to_string(id);
                ImGui::BulletText("%s", nm.c_str());
            }
            ImGui::EndTooltip();
        }
    }

    // While build is running on the background thread, skip scope data access
    // to avoid data races on scopes_/entries being rebuilt concurrently.
    if (vm.isBuildPending()) {
        ImGui::TextDisabled("  Building circuit...");
        ImGui::End();
        return;
    }

    // Track which scope is focused so probes target the right scope
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
        vm.setActiveScope(scopeIdx_);

    // Apply deferred load block (written by doLoad when this ScopeView didn't exist yet)
    {
        ScopeModel& sc = vm.scope(scopeIdx_);
        if (sc.hasPendingLoadBlock()) {
            int schId = sc.pendingLoadSchId();
            std::istringstream bss(sc.takePendingLoadBlock());
            loadState(bss, vm, schId);
        }
    }

    // Reset per-frame guards.
    plotStructureChanged_ = false;
    vm.setHoveredSignal("");

    ScopeModel& scope = vm.scope(scopeIdx_);

    // Reset linked X range when this scope's source sch's t_end changes; honour
    // pending restore from loadState.
    //
    // Locking to the OWNER sch (not the active simulation) means: rerunning a
    // different sch with a different t_end never resizes this scope's time
    // range. Mixed scopes (no single owner) keep their range stable; empty
    // scopes follow the most recent active sim so the first auto-populate
    // sees a sensible default.
    {
        bool hasEntries = false;
        for (int pi = 0; pi < scope.plotCount() && !hasEntries; pi++) {
            const PlotArea* p = scope.getPlot(pi);
            if (p && !p->entries.empty()) hasEntries = true;
        }
        double tEnd = -1.0;
        if (!hasEntries) {
            tEnd = vm.simConfig().t_end;            // empty scope: follow active sim
        } else {
            int owner = scope.computeOwnerSchId();
            if (owner >= 0) tEnd = vm.tEndForSchId(owner);
            // mixed (owner == -1): tEnd stays -1, X range is preserved.
        }
        if (tEnd > 0.0 && tEnd != lastTEnd_) {
            lastTEnd_ = tEnd;
            if (pendingXRestore_) {
                xLinkMin_      = pendingXMin_;
                xLinkMax_      = pendingXMax_;
                pendingXRestore_ = false;
            } else {
                xLinkMin_ = 0.0;
                xLinkMax_ = tEnd;
                // POP: if the active run's data has been trimmed to the last N
                // fundamental periods, default to the retained window instead of
                // the full range. Covers scopes created AFTER the trim (probe-
                // created / New Scope / recreated after the invisible-scope cull)
                // which never received a requestXZoom.
                if (vm.popTrimApplied() && tEnd == vm.simConfig().t_end
                    && vm.popRetainStart() > 0.0 && vm.popRetainStart() < tEnd)
                    xLinkMin_ = vm.popRetainStart();
            }
        }
    }

    // POP trim response: applyPopTrim posted a pending X-zoom on this scope's
    // model after trimming its data to the last N fundamental periods. Consume
    // it here — this runs on the scope's next ACTUAL render, so the request
    // survives the window being hidden or docked as a background tab during the
    // run (Begin() early-outs above would swallow a same-frame notification).
    if (scope.hasPendingXZoom()) {
        double zMin, zMax;
        scope.takePendingXZoom(zMin, zMax);
        pushSnapshot(scope.plotCount());
        xLinkMin_ = zMin;
        xLinkMax_ = zMax;
        for (int i = 0; i < scope.plotCount(); i++)
            computeAutoFitPlot(vm, i, /*allData=*/true);
    }

    // Ctrl+Z undo 鈥?only when this window is focused and the user is not typing
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && !ImGui::GetIO().WantTextInput
        && ImGui::GetIO().KeyCtrl
        && ImGui::IsKeyPressed(ImGuiKey_Z, /*repeat=*/false)) {
        applyUndo();
    }

    ensurePlotYStates(scope.plotCount());

    // 鈹€鈹€ Toolbar 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    // Undo — first in the toolbar, shown as a left-arrow button (greyed out when
    // history is empty). Plot insert/remove now lives only in the right-click menu.
    {
        bool hasHistory = !zoomHistory_.empty();
        if (!hasHistory) ImGui::BeginDisabled();
        if (ImGui::ArrowButton("##undo", ImGuiDir_Left)) applyUndo();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Undo zoom / auto-fit  (Ctrl+Z)");
        if (!hasHistory) ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button("Auto-Fit All")) {
        computeAutoFitAll(vm);
    }
    ImGui::SameLine();

    // Export CSV (greyed out until at least one visible signal has data)
    {
        bool hasData = false;
        for (int pi = 0; pi < scope.plotCount() && !hasData; pi++) {
            const PlotArea* p = scope.getPlot(pi);
            if (!p) continue;
            for (const auto& e : p->entries)
                if (e->visible && e->buffer.getCount() > 0) { hasData = true; break; }
        }
        if (!hasData) ImGui::BeginDisabled();
        if (ImGui::Button("Export CSV"))
            ImGui::OpenPopup("ExportCsvPopup");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Export all visible signals of this scope to a CSV file");
        if (!hasData) ImGui::EndDisabled();
        if (ImGui::BeginPopup("ExportCsvPopup")) {
            if (ImGui::MenuItem("All data"))
                exportCsv(vm, /*visibleOnly=*/false);
            if (ImGui::MenuItem("Visible time range only"))
                exportCsv(vm, /*visibleOnly=*/true);
            ImGui::EndPopup();
        }
    }
    ImGui::SameLine();

    // FFT: opens the frequency-domain window for the currently selected plot.
    if (ImGui::Button("FFT")) {
        fftOpen_        = true;
        fftPlotIdx_     = scope.selectedPlot();
        fftAppliedXF0_  = 0.0;   // re-apply the default 5·f0 X width on (re)open
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Frequency-domain (FFT) analysis of the selected plot's signals");
    ImGui::SameLine();

    ImGui::TextDisabled("| Sim time: %.6f s", vm.currentTime());

    // Source-schematics badge: redundant copy of the title-bar tooltip so the
    // hint stays discoverable when the scope is docked as a tab and the title
    // bar isn't directly hoverable.
    {
        std::vector<int> seenIds;
        for (int pi = 0; pi < scope.plotCount(); pi++) {
            const PlotArea* p = scope.getPlot(pi);
            if (!p) continue;
            for (const auto& e : p->entries) {
                int id = e->sourceSchId;
                if (id < 0) continue;
                if (std::find(seenIds.begin(), seenIds.end(), id) == seenIds.end())
                    seenIds.push_back(id);
            }
        }
        if (!seenIds.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("| Sources(%d)", (int)seenIds.size());
            if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
                ImGui::TextDisabled("Source schematics:");
                for (int id : seenIds) {
                    std::string nm = vm.displayNameForSchId(id);
                    if (nm.empty()) nm = "Sch#" + std::to_string(id);
                    ImGui::BulletText("%s", nm.c_str());
                }
                ImGui::EndTooltip();
            }
        }
    }

    ImGui::Separator();

    // Calculate plot height: evenly split the available area, with a 150 px minimum.
    // When the total content exceeds the available height the BeginChild scrollbar appears.
    int   n          = scope.plotCount();
    float avail      = ImGui::GetContentRegionAvail().y
                     - (n - 1) * ImGui::GetStyle().ItemSpacing.y;
    float plotHeight = std::max(avail / n, 150.0f);

    ImGui::BeginChild("##scope_scroll", ImVec2(-1, -1), false, 0);
    for (int i = 0; i < n; i++) {
        ImGui::PushID(i);
        PlotArea* plot = scope.getPlot(i);
        if (plot) {
            // Handle autoFitY flag set externally (e.g. ScopeModel::autoFitAll)
            if (plot->autoFitY) {
                computeAutoFitPlot(vm, i, /*allData=*/true);
                plot->autoFitY = false;
            }
            renderPlot(vm, *plot, i, plotHeight, /*isBottom=*/(i == n - 1));
        }
        ImGui::PopID();
        if (i < n - 1) ImGui::Spacing(); // thin gap instead of full separator
    }
    ImGui::EndChild();

    // Apply any pending zoom action NOW 鈥?after all EndPlot() calls.
    // ImPlot writes linked-axis values back at EndPlot(), so we must set
    // xLinkMin_/xLinkMax_ only after every plot has called EndPlot().
    if (pendingZoom_.active) {
        pendingZoom_.active = false;
        pushSnapshot(scope.plotCount());
        if (pendingZoom_.isH) {
            xLinkMin_ = pendingZoom_.lo;
            xLinkMax_ = pendingZoom_.hi;
        } else {
            ensurePlotYStates(pendingZoom_.plotIdx + 1);
            plotYStates_[pendingZoom_.plotIdx] = {pendingZoom_.lo, pendingZoom_.hi, true};
        }
    }

    ImGui::End();
}

// 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ Per-plot rendering 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
void ScopeView::renderPlot(MainViewModel& vm, PlotArea& plot,
                           int plotIndex, float plotHeight, bool isBottom) {
    ScopeModel& scope = vm.scope(scopeIdx_);

    // Highlight border of the selected plot; gold + thicker when probe is active.
    // Must use ImPlot style API (not ImGui) because the border is an ImPlot element.
    bool selected = (plotIndex == scope.selectedPlot());
    if (selected) {
        bool probeActive = vm.isProbeActive() && (scopeIdx_ == vm.activeScope());
        ImVec4 borderCol = probeActive
            ? ImVec4(1.0f, 0.86f, 0.2f, 1.0f)   // gold when probe mode active
            : ImVec4(0.4f, 0.7f,  1.0f, 1.0f);   // blue otherwise
        float borderW = probeActive ? 3.0f : 2.0f;
        ImPlot::PushStyleColor(ImPlotCol_PlotBorder, borderCol);
        ImPlot::PushStyleVar(ImPlotStyleVar_PlotBorderSize, borderW);
    }

    // Always disable ImPlot's built-in box-select; we implement our own zoom drag.
    ImPlotFlags plotFlags = ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect;

    // Estimate plot width BEFORE BeginPlot 鈥?GetPlotSize() locks the setup phase,
    // so all Setup* calls must precede any non-setup API.
    float plotWidthPx = ImGui::GetContentRegionAvail().x;

    // Compact title area: reduce vertical inner padding from the default (10,10)
    // to (10,3) so the title text sits closer to the plot content.
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(10.0f, 3.0f));
    bool plotOk = ImPlot::BeginPlot(plot.title.c_str(), ImVec2(-1, plotHeight), plotFlags);
    ImPlot::PopStyleVar(); // PlotPadding

    if (!plotOk) {
        if (selected) { ImPlot::PopStyleColor(); ImPlot::PopStyleVar(); }
        return;
    }

    // 鈹€鈹€ Axis setup 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    // X axis: linked so all plots pan/zoom together.
    // Only the bottom plot shows tick labels and the "Time (s)" title;
    // upper plots suppress them to avoid redundant repetition.
    {
        ImPlotAxisFlags xFlags = isBottom
            ? ImPlotAxisFlags_None
            : (ImPlotAxisFlags_NoTickLabels | ImPlotAxisFlags_NoLabel);
        ImPlot::SetupAxis(ImAxis_X1, isBottom ? "Time (s)" : nullptr, xFlags);
    }
    ImPlot::SetupAxisLinks(ImAxis_X1, &xLinkMin_, &xLinkMax_);
    AxisFmtParams xFmt = computeAxisFmt(xLinkMin_, xLinkMax_, plotWidthPx);
    if (isBottom)
        ImPlot::SetupAxisFormat(ImAxis_X1, axisFormatterCallback, &xFmt);

    // Y axis: smart formatting adapts to value range.
    // No AutoFit flag 鈥?we control limits ourselves for accurate undo.
    ImPlot::SetupAxis(ImAxis_Y1, "Value", ImPlotAxisFlags_None);
    ImPlot::SetupLegend(ImPlotLocation_NorthEast);

    // Apply stored Y limits if requested (after auto-fit, V-zoom, undo, or sch load).
    // Keep forceSet true until AFTER the end-of-render GetPlotLimits guard so the
    // freshly-applied range isn't overwritten by ImPlot's stale internal state on
    // the very first frame the plot is rendered (most visible for newly-inserted
    // plots whose ID has no prior axis cache, e.g. plot 1 restored from .sch).
    ensurePlotYStates(plotIndex + 1);
    double yMin = plotYStates_[plotIndex].yMin;
    double yMax = plotYStates_[plotIndex].yMax;
    // Capture forceSet at entry: only this state was actually applied via
    // SetupAxisLimits this frame. If computeAutoFitPlot fires later in this frame
    // (e.g. dblClick on Y axis), it sets forceSet=true again — that one must
    // survive to the NEXT frame so the new margin'd range is applied.
    bool consumedForceSet = plotYStates_[plotIndex].forceSet;
    if (consumedForceSet) {
        ImPlot::SetupAxisLimits(ImAxis_Y1, yMin, yMax, ImPlotCond_Always);
    }
    AxisFmtParams yFmt = computeAxisFmt(yMin, yMax, plotWidthPx);
    ImPlot::SetupAxisFormat(ImAxis_Y1, axisFormatterCallback, &yFmt);

    // 鈹€鈹€ Decimated signal rendering 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    // Target: at most 2 脳 plot pixel width output points per signal.
    // The decimateMinMax algorithm preserves all peaks and valleys by keeping
    // the min-Y and max-Y point within each equal-width bucket.
    int maxPts = std::max(500, static_cast<int>(plotWidthPx * 2.0f));

    for (auto& entry : plot.entries) {
        if (!entry->visible) continue;
        int count  = entry->buffer.getCount();
        int offset = entry->buffer.getOffset();
        if (count <= 0) continue;

        const double* xd = entry->buffer.getXData();
        const double* yd = entry->buffer.getYData();

        ImVec4 col(
            ((entry->color >> 0)  & 0xFF) / 255.0f,
            ((entry->color >> 8)  & 0xFF) / 255.0f,
            ((entry->color >> 16) & 0xFF) / 255.0f,
            1.0f);

        // Cache-driven decimation: assembles outX/outY from the per-bucket cache,
        // touching only the samples that arrived since the previous frame on the
        // fast path. One PlotLine call per signal, even when the ring buffer
        // wraps — segment splitting is handled inside decimateCached.
        ImPlotSpec lineSpec(ImPlotProp_LineColor, col, ImPlotProp_LineWeight, 1.5f);
        const char* entryLbl = entry->effectiveLabel().c_str();
        int n = decimateCached(
            xd, yd, count, offset, (int)entry->buffer.capacity(),
            entry->buffer.generation(),
            xLinkMin_, xLinkMax_, maxPts, entry->decim);
        if (n > 0)
            ImPlot::PlotLine(entryLbl, entry->decim.outX.data(),
                             entry->decim.outY.data(), n, lineSpec);
        if (ImPlot::IsLegendEntryHovered(entryLbl)) {
            vm.setHoveredSignal(entry->signalName);
            // Tooltip names the source schematic so a multi-sch scope (or one
            // that mixes signals from several builds) is unambiguous.
            if (entry->sourceSchId >= 0) {
                std::string nm = vm.displayNameForSchId(entry->sourceSchId);
                if (nm.empty()) nm = "Sch#" + std::to_string(entry->sourceSchId);
                ImGui::SetTooltip("Source: %s", nm.c_str());
            }
        }
    }

    // 鈹€鈹€ Auto-zoom drag (always active) 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    // 鈥?Vertical   screen drag (|dy| 鈮?|dx|) 鈫?H-zoom: select X / time range
    // 鈥?Horizontal screen drag (|dy| < |dx|) 鈫?V-zoom: select Y range (this plot)
    // 鈥?Short click (threshold not exceeded)  鈫?select this plot
    // 鈥?Drag on axis area                     鈫?ImPlot native axis pan (unchanged)
    //
    // ImPlot processes its Pan input at EndPlot().  We set Pan = Middle when the
    // mouse is in the plot body so ImPlot does not consume the drag; we restore
    // Pan = Left when the mouse is on an axis so native axis-drag panning works.
    {
        const float kThresh   = 6.0f;
        bool plotHov  = ImPlot::IsPlotHovered();
        bool axisHov  = ImPlot::IsAxisHovered(ImAxis_X1) || ImPlot::IsAxisHovered(ImAxis_Y1);
        bool lPressed = ImGui::IsMouseClicked(ImGuiMouseButton_Left, /*repeat=*/false);
        bool lReleased= ImGui::IsMouseReleased(ImGuiMouseButton_Left);
        ImVec2 mpos   = ImGui::GetMousePos();

        // Per-plot InputMap override, evaluated just before this plot's EndPlot:
        //   axis hover OR no interaction 鈫?Pan = Left  (axis drag pans normally)
        //   plot body OR active drag     鈫?Pan = Middle (we handle left drag)
        {
            bool suppressPan = autoDragActive_ || (plotHov && !axisHov);
            ImPlot::GetInputMap().Pan =
                suppressPan ? ImGuiMouseButton_Middle : ImGuiMouseButton_Left;
        }

        // Begin drag: left-press inside the plot body (not on an axis)
        if (!autoDragActive_ && lPressed && plotHov && !axisHov) {
            autoDragActive_    = true;
            autoDragDirLocked_ = false;
            autoDragIsH_       = false;
            autoDragStartScr_  = mpos;
            autoDragStartPlot_ = ImPlot::GetPlotMousePos();
            autoDragPlotIdx_   = plotIndex;
        }

        // Hand cursor when hovering the plot body (signals zoom is ready)
        if (plotHov && !axisHov && !autoDragActive_)
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        // Active drag 鈥?originating plot processes direction lock, drawing, release
        if (autoDragActive_ && autoDragPlotIdx_ == plotIndex) {
            float adx = std::abs(mpos.x - autoDragStartScr_.x);
            float ady = std::abs(mpos.y - autoDragStartScr_.y);

            // Determine / update zoom direction with hysteresis to prevent flicker.
            // Initial lock: whichever axis has larger displacement wins (past kThresh).
            // Subsequent re-evaluation: need kHysteresis px advantage to switch mode.
            //   V-zoom 鈫?H-zoom : adx > ady + kHysteresis
            //   H-zoom 鈫?V-zoom : ady > adx + kHysteresis
            const float kHysteresis = 20.0f;
            if (!autoDragDirLocked_) {
                if (adx > kThresh || ady > kThresh) {
                    autoDragDirLocked_ = true;
                    autoDragIsH_       = (adx >= ady);
                }
            } else {
                if (!autoDragIsH_ && adx > ady + kHysteresis)
                    autoDragIsH_ = true;   // V-zoom 鈫?H-zoom
                else if (autoDragIsH_ && ady > adx + kHysteresis)
                    autoDragIsH_ = false;  // H-zoom 鈫?V-zoom
            }

            if (autoDragDirLocked_) {
                ImPlotPoint cur = ImPlot::GetPlotMousePos();
                ImDrawList* dl  = ImPlot::GetPlotDrawList();
                ImVec2 pPos     = ImPlot::GetPlotPos();
                ImVec2 pSize    = ImPlot::GetPlotSize();

                if (autoDragIsH_) {
                    // H-zoom: vertical drag 鈫?X selection band
                    ImVec2 pA = ImPlot::PlotToPixels(autoDragStartPlot_.x, 0.0);
                    ImVec2 pB = ImPlot::PlotToPixels(cur.x,                0.0);
                    float  xA = std::min(pA.x, pB.x), xB = std::max(pA.x, pB.x);
                    dl->AddRectFilled({xA, pPos.y}, {xB, pPos.y + pSize.y},
                                       IM_COL32(100, 150, 255, 50));
                    dl->AddLine({xA, pPos.y}, {xA, pPos.y + pSize.y},
                                 IM_COL32(100, 150, 255, 220), 1.5f);
                    dl->AddLine({xB, pPos.y}, {xB, pPos.y + pSize.y},
                                 IM_COL32(100, 150, 255, 220), 1.5f);
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                } else {
                    // V-zoom: horizontal drag 鈫?Y selection band (this plot only)
                    ImVec2 pA = ImPlot::PlotToPixels(0.0, autoDragStartPlot_.y);
                    ImVec2 pB = ImPlot::PlotToPixels(0.0, cur.y);
                    float  yA = std::min(pA.y, pB.y), yB = std::max(pA.y, pB.y);
                    dl->AddRectFilled({pPos.x, yA}, {pPos.x + pSize.x, yB},
                                       IM_COL32(255, 150, 100, 50));
                    dl->AddLine({pPos.x, yA}, {pPos.x + pSize.x, yA},
                                 IM_COL32(255, 150, 100, 220), 1.5f);
                    dl->AddLine({pPos.x, yB}, {pPos.x + pSize.x, yB},
                                 IM_COL32(255, 150, 100, 220), 1.5f);
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                }
            }

            // Release: commit zoom or (on short click) select this plot
            // Do NOT modify xLinkMin_/xLinkMax_ or plotYStates_ directly here 鈥?            // ImPlot writes linked-axis values back at EndPlot(), which would
            // overwrite them.  Store the intent in pendingZoom_ instead; it is
            // applied after all EndPlot() calls at the bottom of render().
            if (lReleased) {
                if (autoDragDirLocked_) {
                    ImPlotPoint cur = ImPlot::GetPlotMousePos();
                    if (autoDragIsH_) {
                        double lo = std::min(autoDragStartPlot_.x, cur.x);
                        double hi = std::max(autoDragStartPlot_.x, cur.x);
                        if (hi - lo > 1e-15)
                            pendingZoom_ = {true, true, lo, hi, plotIndex};
                    } else {
                        double lo = std::min(autoDragStartPlot_.y, cur.y);
                        double hi = std::max(autoDragStartPlot_.y, cur.y);
                        if (hi - lo > 1e-15)
                            pendingZoom_ = {true, false, lo, hi, plotIndex};
                    }
                } else {
                    // Short click (no direction locked) 鈫?select this plot
                    scope.setSelectedPlot(plotIndex);
                    if (!vm.isProbeActive()) {
                        cursorX_      = autoDragStartPlot_.x;
                        cursorActive_ = true;
                    }
                }
                autoDragActive_    = false;
                autoDragPlotIdx_   = -1;
                autoDragDirLocked_ = false;
            }
        }

        // Non-originating plots: draw the H-zoom X band so it spans all plots
        if (autoDragActive_ && autoDragPlotIdx_ != plotIndex
            && autoDragDirLocked_ && autoDragIsH_) {
            ImPlotPoint cur = ImPlot::GetPlotMousePos();
            ImDrawList* dl  = ImPlot::GetPlotDrawList();
            ImVec2 pPos     = ImPlot::GetPlotPos();
            ImVec2 pSize    = ImPlot::GetPlotSize();
            ImVec2 pA = ImPlot::PlotToPixels(autoDragStartPlot_.x, 0.0);
            ImVec2 pB = ImPlot::PlotToPixels(cur.x,                0.0);
            float  xA = std::min(pA.x, pB.x), xB = std::max(pA.x, pB.x);
            dl->AddRectFilled({xA, pPos.y}, {xB, pPos.y + pSize.y},
                               IM_COL32(100, 150, 255, 50));
            dl->AddLine({xA, pPos.y}, {xA, pPos.y + pSize.y},
                         IM_COL32(100, 150, 255, 220), 1.5f);
            dl->AddLine({xB, pPos.y}, {xB, pPos.y + pSize.y},
                         IM_COL32(100, 150, 255, 220), 1.5f);
        }
    }

    // 鈹€鈹€ Right-click context menu 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    struct HoverVal { std::string lbl; double y; ImU32 col; };
    bool hoverShow = false;
    double hoverT  = 0.0;
    std::vector<HoverVal> hoverVals;
    // -- Hover crosshair + persistent cursor --
    {
        bool plotHov = ImPlot::IsPlotHovered();
        ImDrawList* cdl = ImPlot::GetPlotDrawList();
        ImVec2 pPos  = ImPlot::GetPlotPos();
        ImVec2 pSize = ImPlot::GetPlotSize();

        // Nearest-X helper: returns the sample time in p closest to t
        auto nearestX = [](const PlotArea& p, double t) -> double {
            for (const auto& e : p.entries) {
                int n = e->buffer.getCount();
                if (n == 0) continue;
                int lo = 0, hi = n - 1;
                while (lo < hi) {
                    int mid = (lo + hi) / 2;
                    if (e->buffer.getXAt(mid) < t) lo = mid + 1;
                    else                           hi = mid;
                }
                if (lo > 0) {
                    double d0 = std::abs(e->buffer.getXAt(lo - 1) - t);
                    double d1 = std::abs(e->buffer.getXAt(lo)     - t);
                    if (d0 < d1) lo--;
                }
                return e->buffer.getXAt(lo);
            }
            return t;
        };
        // Nearest-Y helper (binary search over logical ring-buffer indices)
        auto nearestY = [](const ScrollingBuffer& buf, double t) -> double {
            int n = buf.getCount();
            if (n == 0) return 0.0;
            int lo = 0, hi = n - 1;
            while (lo < hi) {
                int mid = (lo + hi) / 2;
                if (buf.getXAt(mid) < t) lo = mid + 1;
                else                     hi = mid;
            }
            if (lo > 0) {
                double d0 = std::abs(buf.getXAt(lo - 1) - t);
                double d1 = std::abs(buf.getXAt(lo)     - t);
                if (d0 < d1) lo--;
            }
            return buf.getYAt(lo);
        };

        // Hover: dashed vertical line + tooltip (hovered plot only, no drag active)
        if (plotHov && !autoDragActive_) {
            ImPlotPoint mp = ImPlot::GetPlotMousePos();
            double snapX = nearestX(plot, mp.x);  // snap to nearest sample
            ImVec2 px = ImPlot::PlotToPixels(snapX, 0.0);
            // Dashed vertical line
            const float kDash = 6.0f, kGap = 4.0f;
            for (float y = pPos.y; y < pPos.y + pSize.y; y += kDash + kGap) {
                float yEnd = std::min(y + kDash, pPos.y + pSize.y);
                cdl->AddLine({px.x, y}, {px.x, yEnd}, IM_COL32(200, 200, 200, 120), 1.0f);
            }
            // Collect hover data (tooltip shown after EndPlot to avoid breaking IsPlotHovered)
            hoverShow = true;
            hoverT    = snapX;
            for (const auto& entry : plot.entries) {
                if (entry->buffer.getCount() == 0) continue;
                double yv = nearestY(entry->buffer, snapX);
                hoverVals.push_back({entry->effectiveLabel(), yv, entry->color});
            }
        }

        // Persistent cursor: yellow line across all plots + value labels
        if (cursorActive_) {
            cursorX_ = nearestX(plot, cursorX_);  // snap to nearest sample
            ImVec2 cpx = ImPlot::PlotToPixels(cursorX_, 0.0);
            if (cpx.x >= pPos.x - 1.0f && cpx.x <= pPos.x + pSize.x + 1.0f) {
                float cx = std::max(cpx.x, pPos.x);
                cdl->AddLine({cx, pPos.y}, {cx, pPos.y + pSize.y},
                              IM_COL32(255, 220, 50, 230), 1.5f);
                // Value labels stacked at top of plot
                float yOff = pPos.y + 4.0f;
                for (const auto& entry : plot.entries) {
                    if (entry->buffer.getCount() == 0) continue;
                    double yv = nearestY(entry->buffer, cursorX_);
                    const char* lbl = entry->effectiveLabel().c_str();
                    char txt[80];
                    snprintf(txt, sizeof(txt), "%s=%.4g", lbl, yv);
                    ImVec2 ts = ImGui::CalcTextSize(txt);
                    float tx = std::min(cx + 3.0f, pPos.x + pSize.x - ts.x - 2.0f);
                    cdl->AddRectFilled({tx - 1, yOff - 1},
                                       {tx + ts.x + 1, yOff + ts.y + 1},
                                       IM_COL32(20, 20, 20, 180));
                    cdl->AddText({tx, yOff}, entry->color, txt);
                    yOff += ts.y + 2.0f;
                }
            }
        }
    }
    if (ImPlot::IsPlotHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup("PlotCtx");
    }
    if (ImGui::BeginPopup("PlotCtx")) {
        renderPlotContextMenu(vm, plotIndex);
        ImGui::EndPopup();
    }

    // ── X-axis context menu: set the visible X range width ──────────────────
    // Right-click on the X-axis strip (below the plot body — not covered by
    // IsPlotHovered, so no conflict with PlotCtx above). The width is applied
    // anchored to the current RIGHT edge (keeps the latest data in view; with
    // POP retention that is the retained tail). Applied via pendingZoom_ so the
    // write happens after EndPlot (linked-axis write-back) with undo support.
    if (ImPlot::IsAxisHovered(ImAxis_X1) && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        xAxisCtxWidth_ = xLinkMax_ - xLinkMin_;   // seed with current width
        ImGui::OpenPopup("XAxisCtx");
    }
    if (ImGui::BeginPopup("XAxisCtx")) {
        auto applyWidth = [&](double w) {
            if (w > 0.0) {
                double hi = xLinkMax_, lo = hi - w;
                if (lo < 0.0) { lo = 0.0; hi = w; }   // clamp to t >= 0
                pendingZoom_ = {true, /*isH=*/true, lo, hi, plotIndex};
                ImGui::CloseCurrentPopup();
            }
        };
        ImGui::TextDisabled("X range width (s)");
        ImGui::SetNextItemWidth(140);
        bool enter = ImGui::InputDouble("##xwidth", &xAxisCtxWidth_, 0.0, 0.0, "%.6g",
                                        ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Apply") || enter) applyWidth(xAxisCtxWidth_);

        // Quick presets: whole fundamental periods (when a switching fundamental
        // was detected) and the full simulation range.
        double f0 = vm.detectedFundamental();
        if (f0 > 0.0) {
            ImGui::Separator();
            char lbl[64];
            for (int k : {1, 2, 5, 10}) {
                snprintf(lbl, sizeof(lbl), "%d period%s of f0 (%.4g s)",
                         k, k == 1 ? "" : "s", (double)k / f0);
                if (ImGui::MenuItem(lbl)) applyWidth((double)k / f0);
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Full range (0 .. t_end)")) {
            double tEnd = lastTEnd_ > 0.0 ? lastTEnd_ : vm.simConfig().t_end;
            pendingZoom_ = {true, /*isH=*/true, 0.0, tEnd, plotIndex};
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // 鈹€鈹€ Scale annotation for scaled-scientific axes 鈹€鈹€
    {
        ImDrawList* dl  = ImPlot::GetPlotDrawList();
        ImVec2 plotPos  = ImPlot::GetPlotPos();
        ImVec2 plotSize = ImPlot::GetPlotSize();
        if (isBottom && xFmt.useScaledSci) {
            ImVec2 ts = ImGui::CalcTextSize(xFmt.annotation);
            dl->AddText({plotPos.x + plotSize.x - ts.x - 4,
                         plotPos.y + plotSize.y - ts.y - 2},
                        IM_COL32(180,180,180,255), xFmt.annotation);
        }
        if (yFmt.useScaledSci) {
            ImVec2 ts = ImGui::CalcTextSize(yFmt.annotation);
            dl->AddText({plotPos.x + 4, plotPos.y + 2},
                        IM_COL32(180,180,180,255), yFmt.annotation);
        }
    }

    // 鈹€鈹€ Double-click 鈫?auto-fit with 5% margin (overrides ImPlot's native fit) 鈹€
    // Check both plot canvas area AND Y axis area so double-click on Y axis ticks
    // is also intercepted (IsPlotHovered returns false for the axis area).
    bool dblClickFit = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
                    && (ImPlot::IsPlotHovered() || ImPlot::IsAxisHovered(ImAxis_Y1));
    if (dblClickFit) {
        computeAutoFitPlot(vm, plotIndex, /*allData=*/false);
    }

    // 鈹€鈹€ Track current Y limits for accurate undo snapshots 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    // GetPlotLimits() returns what ImPlot is actually displaying this frame
    // (including any scroll-wheel zoom or pan the user did with the mouse).
    // We cache this so pushSnapshot() always captures the real current state.
    //
    // Guard 1: skip if a plot was inserted/removed this frame 鈥?plotYStates_
    // was already updated correctly by insertPlot/removePlot.
    // Guard 2: skip if forceSet is pending 鈥?computeAutoFitPlot just wrote
    // margin values into plotYStates_; overwriting them here with the stale
    // pre-fit limits would discard the margin entirely.
    if (!plotStructureChanged_ && !plotYStates_[plotIndex].forceSet) {
        ImPlotRect lim = ImPlot::GetPlotLimits(ImAxis_X1, ImAxis_Y1);
        plotYStates_[plotIndex].yMin = lim.Y.Min;
        plotYStates_[plotIndex].yMax = lim.Y.Max;
    }
    // Only clear the forceSet that was actually consumed by SetupAxisLimits at
    // the top of this render. A late forceSet (set by dblClick computeAutoFitPlot
    // AFTER SetupAxisLimits ran) must survive to next frame, otherwise the 5%
    // margin written into plotYStates would never get applied.
    if (consumedForceSet)
        plotYStates_[plotIndex].forceSet = false;

    ImPlot::EndPlot();
    if (hoverShow && ImGui::BeginTooltip()) {
        {
            char tBuf[64];
            // Always show 4 decimals (after the scale factor in scaled-sci mode).
            if (xFmt.useScaledSci)
                snprintf(tBuf, sizeof(tBuf), "%.4f x 1e%d s",
                         hoverT / xFmt.scaleFactor, xFmt.exponent);
            else
                snprintf(tBuf, sizeof(tBuf), "%.4f s", hoverT);
            ImGui::Text("t = %s", tBuf);
        }
        for (const auto& hv : hoverVals) {
            float r = ((hv.col >>  0) & 0xFF) / 255.0f;
            float g = ((hv.col >>  8) & 0xFF) / 255.0f;
            float b = ((hv.col >> 16) & 0xFF) / 255.0f;
            ImGui::ColorButton("##cv", ImVec4(r, g, b, 1.0f),
                ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder,
                ImVec2(10.0f, 10.0f));
            ImGui::SameLine();
            ImGui::Text("%s = %.4g", hv.lbl.c_str(), hv.y);
        }
        ImGui::EndTooltip();
    }
    if (selected) { ImPlot::PopStyleColor(); ImPlot::PopStyleVar(); }
}

// 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ Context menu 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
void ScopeView::renderPlotContextMenu(MainViewModel& vm, int plotIndex) {
    ScopeModel& scope = vm.scope(scopeIdx_);

    if (ImGui::MenuItem("Insert Plot Above"))
        insertPlot(scope, plotIndex - 1);
    if (ImGui::MenuItem("Insert Plot Below"))
        insertPlot(scope, plotIndex);
    if (ImGui::MenuItem("Delete Plot")) {
        if (scope.plotCount() <= 1)
            scope.clearPlotEntries(0);  // last plot: clear signals, keep plot
        else
            removePlot(scope, plotIndex);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("FFT Analysis")) {
        fftOpen_        = true;
        fftPlotIdx_     = plotIndex;
        fftAppliedXF0_  = 0.0;   // re-apply the default 5·f0 X width on (re)open
    }
    if (cursorActive_ && ImGui::MenuItem("Clear Cursor"))
        cursorActive_ = false;
    renderAddSignalMenu(vm, plotIndex);
    renderRemoveSignalMenu(vm, plotIndex);
}

void ScopeView::renderAddSignalMenu(MainViewModel& vm, int plotIndex) {
    if (!ImGui::BeginMenu("Add Signal")) return;

    ScopeModel& scope = vm.scope(scopeIdx_);
    PlotArea*   plot  = scope.getPlot(plotIndex);

    for (const auto& sig : vm.availableSignals()) {
        // Find existing entry by signalName (not display key) since name may be renamed
        MuxEntry* entry = plot ? plot->findBySignalName(sig.name) : nullptr;
        bool shown = entry && entry->visible;
        const char* dispName = shown ? entry->effectiveLabel().c_str() : sig.name.c_str();
        if (ImGui::MenuItem(dispName, nullptr, shown)) {
            if (!shown && plot) {
                // Pull the active sch's per-sch rawCache into the scope's
                // signalCache_ so addSignalToPlot's backfill finds historical
                // data for any probe (not just signals already on a plot).
                vm.syncRawCacheToScope(scopeIdx_);
                // Tag with the active sch's id so ownership routing can attribute
                // this signal back to the right doc when saving.
                scope.addSignalToPlot(plotIndex, sig.name, ScopeModel::nextColor(),
                                       vm.activeSchDoc().id);
            }
        }
    }

    ImGui::EndMenu();
}

void ScopeView::renderRemoveSignalMenu(MainViewModel& vm, int plotIndex) {
    ScopeModel& scope = vm.scope(scopeIdx_);
    PlotArea*   plot  = scope.getPlot(plotIndex);
    if (!plot) return;

    bool hasVisible = false;
    for (const auto& entry : plot->entries)
        if (entry->visible) { hasVisible = true; break; }
    if (!hasVisible) return;

    if (!ImGui::BeginMenu("Remove Signal")) return;

    for (auto& entry : plot->entries) {
        if (!entry->visible) continue;
        if (ImGui::MenuItem(entry->effectiveLabel().c_str())) {
            scope.removeSignalFromPlot(plotIndex, entry->effectiveLabel());
            break;
        }
    }

    ImGui::EndMenu();
}

// 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ FFT analysis window 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
//
// Independent top-level window. Recomputes the magnitude spectrum of every
// visible signal in the selected plot each frame from the buffered samples
// (cheap: one radix-2 FFT of <=32k points per signal, only while the window is
// open). Frequency comes from the sample span; the analysis window can be the
// scope's visible time range or the whole buffer.
void ScopeView::renderFftWindow(MainViewModel& vm) {
    // Always submit the window (Begin/End) every frame while open — see the note
    // in render(). The build-pending / early-out checks happen AFTER Begin so the
    // window is never skipped, which is what prevents the docked-tab flicker.
    std::string wtitle = "FFT##" + title_;   // stable per-scope id for dock state
    ImGui::SetNextWindowSize(ImVec2(660, 430), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(wtitle.c_str(), &fftOpen_)) {
        ImGui::End();
        return;
    }

    // Skip data access while the circuit rebuilds on the worker thread
    // (scopes_/entries are in flux), but keep the window submitted.
    if (vm.isBuildPending()) {
        ImGui::TextDisabled("  Building circuit...");
        ImGui::End();
        return;
    }

    ScopeModel& scope = vm.scope(scopeIdx_);
    int pc = scope.plotCount();
    if (fftPlotIdx_ >= pc) fftPlotIdx_ = pc - 1;
    if (fftPlotIdx_ < 0)   fftPlotIdx_ = 0;

    // 鈹€鈹€ Toolbar 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    // Plot selector
    {
        const PlotArea* selP = scope.getPlot(fftPlotIdx_);
        std::string preview = selP ? selP->title : "Plot";
        ImGui::SetNextItemWidth(150);
        if (ImGui::BeginCombo("Source", preview.c_str())) {
            for (int i = 0; i < pc; i++) {
                const PlotArea* p = scope.getPlot(i);
                if (!p) continue;
                bool sel = (i == fftPlotIdx_);
                if (ImGui::Selectable(p->title.c_str(), sel)) fftPlotIdx_ = i;
                if (sel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    {
        const char* winNames[] = {"Rectangular", "Hann", "Hamming", "Blackman"};
        ImGui::SetNextItemWidth(130);
        ImGui::Combo("Window", &fftWindow_, winNames, IM_ARRAYSIZE(winNames));
    }
    ImGui::SameLine();
    // N (FFT size). "Auto" uses the native sample count (zero-padded to pow2);
    // an explicit N resamples the window to exactly N points (no padding), which
    // keeps the fundamental/harmonics exactly on bins when f0 is set.
    static const int kNOpts[] = {0, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
    {
        const char* nNames[] = {"Auto", "256", "512", "1024", "2048",
                                "4096", "8192", "16384", "32768"};
        ImGui::SetNextItemWidth(90);
        ImGui::Combo("N", &fftNSel_, nNames, IM_ARRAYSIZE(nNames));
        if (fftNSel_ < 0) fftNSel_ = 0;
        if (fftNSel_ >= IM_ARRAYSIZE(kNOpts)) fftNSel_ = IM_ARRAYSIZE(kNOpts) - 1;
    }

    // Row 2: display options.
    ImGui::Checkbox("dB", &fftDb_);
    ImGui::SameLine();
    ImGui::Checkbox("Log f", &fftLogX_);
    ImGui::SameLine();
    ImGui::Checkbox("Remove DC", &fftRemoveDc_);
    ImGui::SameLine();
    ImGui::Checkbox("Visible range", &fftVisibleRange_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Selects which time span the FFT analyses:\n"
            "  Checked   : the scope's selected / visible time range [xMin, xMax]\n"
            "              \xe2\x80\x94 zoom or pan the scope to change the FFT.\n"
            "  Unchecked : the full simulation range [0, t_end]\n"
            "              \xe2\x80\x94 fixed; scope zoom/pan does NOT change the FFT.");

    // Row 3: fundamental frequency (coherent sampling) + cursor controls.
    // Default f0 to the simulator's auto-detected fundamental (the gate-drive
    // frequency of the largest-current switch) until the user overrides it. A
    // fresh detected value from a new run refreshes the default.
    {
        double det = vm.detectedFundamental();
        if (det > 0.0 && det != fftDetectedF0Applied_) {
            fftDetectedF0Applied_ = det;
            if (!fftF0UserSet_) fftF0_ = det;
        }
    }
    ImGui::SetNextItemWidth(130);
    if (ImGui::InputDouble("f0 (Hz)", &fftF0_, 0.0, 0.0, "%.6g"))
        fftF0UserSet_ = true;   // manual edit latches; stop auto-overriding
    if (fftF0_ < 0.0) fftF0_ = 0.0;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Fundamental frequency. >0 snaps the analysis window to whole\n"
                          "periods (coherent sampling) and the cursor to harmonics k·f0.\n"
                          "0 = analyse the whole span.\n"
                          "Defaults to the auto-detected switching fundamental.");
    ImGui::SameLine();
    if (ImGui::Button("f0 = peak") && fftLastPeakFreq_ > 0.0) {
        fftF0_ = fftLastPeakFreq_;
        fftF0UserSet_ = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Set f0 to the current peak frequency of the first signal");
    ImGui::SameLine();
    if (fftCursorActive_) {
        if (ImGui::Button("Clear Cursor")) fftCursorActive_ = false;
    } else {
        ImGui::TextDisabled("(click plot to place cursor)");
    }

    ImGui::Separator();

    PlotArea* plot = scope.getPlot(fftPlotIdx_);
    if (!plot) { ImGui::TextDisabled("No plot."); ImGui::End(); return; }

    const double tMin = fftVisibleRange_ ? xLinkMin_ : -DBL_MAX;
    const double tMax = fftVisibleRange_ ? xLinkMax_ :  DBL_MAX;

    // 鈹€鈹€ Compute spectra 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    // Cache signatures: ctrlSig captures every analysis parameter, dataSig the
    // source buffers (count + generation). A control change recomputes at once; a
    // pure data change (live simulation) is throttled to avoid per-frame FFTs.
    auto hashComb = [](size_t& h, size_t v) {
        h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    };
    size_t ctrlSig = 0;
    hashComb(ctrlSig, std::hash<int>()(fftPlotIdx_));
    hashComb(ctrlSig, std::hash<int>()(fftWindow_));
    hashComb(ctrlSig, std::hash<int>()((int)fftRemoveDc_));
    hashComb(ctrlSig, std::hash<int>()(kNOpts[fftNSel_]));
    hashComb(ctrlSig, std::hash<double>()(fftF0_));
    hashComb(ctrlSig, std::hash<int>()((int)fftDb_));
    hashComb(ctrlSig, std::hash<int>()((int)fftVisibleRange_));
    if (fftVisibleRange_) {
        hashComb(ctrlSig, std::hash<double>()(xLinkMin_));
        hashComb(ctrlSig, std::hash<double>()(xLinkMax_));
    }
    size_t dataSig = 0;
    for (auto& e : plot->entries) {
        if (!e->visible) continue;
        hashComb(dataSig, std::hash<std::string>()(e->effectiveLabel()));
        hashComb(dataSig, std::hash<int>()(e->buffer.getCount()));
        // offset advances on every push even when the buffer is full (count
        // pinned at capacity); without it a wrapped/streaming buffer looks
        // unchanged and the spectrum freezes (most visible with Visible-range
        // OFF, where the X range never changes to otherwise force a recompute).
        hashComb(dataSig, std::hash<int>()(e->buffer.getOffset()));
        hashComb(dataSig, std::hash<int>()(e->buffer.generation()));
        hashComb(dataSig, std::hash<unsigned>()((unsigned)e->color));
    }

    const bool ctrlChanged = (ctrlSig != fftCtrlSig_);
    const bool dataChanged = (dataSig != fftDataSig_);
    const double nowT = ImGui::GetTime();
    // Recompute on first use / empty cache, any control change, or a throttled
    // data change (>=100 ms since the last recompute).
    const bool recompute = fftLines_.empty() || ctrlChanged
                        || (dataChanged && (nowT - fftLastComputeTime_) >= 0.1);

    if (recompute) {
        fftLines_.clear();
        for (auto& e : plot->entries) {
            if (!e->visible) continue;
            int cnt = e->buffer.getCount();
            if (cnt < 4) continue;
            std::vector<double> ts, ys;
            ts.reserve(cnt); ys.reserve(cnt);
            for (int i = 0; i < cnt; i++) {
                double x = e->buffer.getXAt(i);
                if (x < tMin || x > tMax) continue;
                ts.push_back(x);
                ys.push_back(e->buffer.getYAt(i));
            }
            if ((int)ts.size() < 4) continue;

            fft::Options fo;
            fo.win      = (fft::Window)fftWindow_;
            fo.removeDc = fftRemoveDc_;
            fo.f0       = fftF0_ > 0.0 ? fftF0_ : 0.0;
            fo.nfft     = kNOpts[fftNSel_];
            fft::Spectrum sp = fft::compute(ts, ys, fo);
            if (sp.empty()) continue;

            FftLine fl;
            fl.label = e->effectiveLabel();
            fl.color = e->color;
            fl.plotMag.resize(sp.mag.size());
            for (size_t k = 0; k < sp.mag.size(); k++) {
                if (fftDb_) {
                    double m = sp.mag[k] < 1e-12 ? 1e-12 : sp.mag[k];
                    fl.plotMag[k] = 20.0 * std::log10(m);
                } else {
                    fl.plotMag[k] = sp.mag[k];
                }
            }
            fl.spec = std::move(sp);
            fftLines_.push_back(std::move(fl));
        }
        fftCtrlSig_         = ctrlSig;
        fftDataSig_         = dataSig;
        fftLastComputeTime_ = nowT;
    }

    std::vector<FftLine>& lines = fftLines_;
    if (lines.empty()) {
        ImGui::TextDisabled("No signal with enough samples in the selected range.");
        ImGui::End();
        return;
    }

    // 鈹€鈹€ Peak summary 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    auto fmtHz = [](char* b, size_t n, double hz) {
        if (hz >= 1e6)      std::snprintf(b, n, "%.4g MHz", hz / 1e6);
        else if (hz >= 1e3) std::snprintf(b, n, "%.4g kHz", hz / 1e3);
        else                std::snprintf(b, n, "%.4g Hz",  hz);
    };
    {
        const FftLine& l0 = lines.front();
        fftLastPeakFreq_ = l0.spec.peakFreq;   // for the "f0 = peak" button
        char pk[48]; fmtHz(pk, sizeof(pk), l0.spec.peakFreq);
        ImGui::TextDisabled("fs %.4g kHz  |  df %.4g Hz  |  N %d",
                            l0.spec.fs / 1e3, l0.spec.df, l0.spec.n);
        ImGui::SameLine();
        if (l0.spec.periods > 0)
            ImGui::TextDisabled("|  coherent: %d periods (%.4g ms)",
                                l0.spec.periods, l0.spec.winDur * 1e3);
        ImGui::SameLine();
        ImGui::TextDisabled("|  peak %s (%.4g)", pk, l0.spec.peakMag);

        // Explicit analysis range so it's unambiguous which data the FFT covers
        // (and why an unchecked "Visible range" stays put when the scope zooms).
        double rlo, rhi;
        if (fftVisibleRange_) {
            rlo = xLinkMin_; rhi = xLinkMax_;
        } else {
            rlo = 0.0; rhi = 0.0;
            for (auto& e : plot->entries)
                if (e->visible && e->buffer.getCount() > 0) {
                    rhi = e->buffer.getXAt(e->buffer.getCount() - 1);
                    break;
                }
        }
        ImGui::TextDisabled("FFT range: %.6g \xe2\x80\x93 %.6g s   (%s)", rlo, rhi,
                            fftVisibleRange_ ? "selected / visible"
                                             : "full  0 \xe2\x80\x93 t_end");
    }

    // Nearest-bin value lookup on a spectrum line at an arbitrary frequency.
    // Returns the value in the current display units (linear or dB).
    auto valueAt = [&](const FftLine& fl, double freq) -> double {
        if (fl.spec.df <= 0.0 || fl.plotMag.empty()) return 0.0;
        int idx = (int)std::lround(freq / fl.spec.df);
        if (idx < 0) idx = 0;
        if (idx >= (int)fl.plotMag.size()) idx = (int)fl.plotMag.size() - 1;
        return fl.plotMag[idx];
    };
    // Snap a clicked/hovered frequency to the point of interest: a harmonic of
    // f0 (including k=0 = DC) when f0 is set, else the nearest FFT bin.
    auto snapFreq = [&](double freq) -> double {
        if (fftF0_ > 0.0) {
            int k = (int)std::lround(freq / fftF0_);
            if (k < 0) k = 0;
            return (double)k * fftF0_;
        }
        double df = lines.front().spec.df;
        if (df <= 0.0) return freq;
        int idx = (int)std::lround(freq / df);
        if (idx < 0) idx = 0;
        return (double)idx * df;
    };

    // 鈹€鈹€ Plot 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
    struct FftHoverVal { std::string lbl; double v; ImU32 col; };
    bool   hoverShow = false;
    double hoverF    = 0.0;
    std::vector<FftHoverVal> hoverVals;

    if (ImPlot::BeginPlot("##fftplot", ImVec2(-1, -1))) {
        ImPlot::SetupAxis(ImAxis_X1, "Frequency (Hz)");
        ImPlot::SetupAxis(ImAxis_Y1, fftDb_ ? "Magnitude (dB)" : "Amplitude");
        if (fftLogX_)
            ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Log10);
        // Default X range: the first 5 harmonics ([0, 5·f0]) instead of the full
        // spectrum. Applied when the window opens with a known fundamental and
        // re-applied whenever f0 changes (auto-detect fill-in or user edit);
        // manual pan/zoom afterwards is untouched.
        if (fftF0_ > 0.0 && fftF0_ != fftAppliedXF0_) {
            fftAppliedXF0_ = fftF0_;
            double xlo = fftLogX_ ? fftF0_ * 0.1 : 0.0;   // log axis: 0 is invalid
            ImPlot::SetupAxisLimits(ImAxis_X1, xlo, 5.0 * fftF0_, ImPlotCond_Always);
        }
        ImPlot::SetupLegend(ImPlotLocation_NorthEast);

        for (auto& fl : lines) {
            ImVec4 col(
                ((fl.color >> 0)  & 0xFF) / 255.0f,
                ((fl.color >> 8)  & 0xFF) / 255.0f,
                ((fl.color >> 16) & 0xFF) / 255.0f,
                1.0f);
            ImPlotSpec spec(ImPlotProp_LineColor, col, ImPlotProp_LineWeight, 1.5f);
            // On a log-X axis skip the DC bin (freq 0 is invalid for log10).
            int start = fftLogX_ ? 1 : 0;
            int n = (int)fl.spec.freq.size() - start;
            if (n > 0)
                ImPlot::PlotLine(fl.label.c_str(),
                                 fl.spec.freq.data() + start,
                                 fl.plotMag.data() + start, n, spec);
        }

        ImDrawList* dl   = ImPlot::GetPlotDrawList();
        ImVec2      pPos = ImPlot::GetPlotPos();
        ImVec2      pSize= ImPlot::GetPlotSize();
        ImPlotRect  lim  = ImPlot::GetPlotLimits();

        // 鈹€鈹€ Harmonic reference lines at k路f0 (the frequencies of interest) 鈹€鈹€鈹€鈹€
        if (fftF0_ > 0.0) {
            int kStart = std::max(1, (int)std::floor(lim.X.Min / fftF0_));
            int kEnd   = (int)std::ceil(lim.X.Max / fftF0_);
            if (kEnd - kStart > 256) kEnd = kStart + 256;   // clutter cap
            for (int k = kStart; k <= kEnd; ++k) {
                double fx = (double)k * fftF0_;
                ImVec2 p = ImPlot::PlotToPixels(fx, 0.0);
                if (p.x < pPos.x || p.x > pPos.x + pSize.x) continue;
                dl->AddLine({p.x, pPos.y}, {p.x, pPos.y + pSize.y},
                            IM_COL32(120, 120, 120, 70), 1.0f);
            }
        }

        bool plotHov = ImPlot::IsPlotHovered();

        // 鈹€鈹€ Place / clear cursor 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
        // Left click (press+release without a real drag) places the cursor;
        // right click clears it. ImPlot keeps native pan on left-drag.
        if (plotHov && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            ImVec2 dd = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            if (std::fabs(dd.x) + std::fabs(dd.y) < 4.0f) {
                fftCursorFreq_   = snapFreq(ImPlot::GetPlotMousePos().x);
                fftCursorActive_ = true;
            }
        }
        if (plotHov && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
            fftCursorActive_ = false;

        // 鈹€鈹€ Hover crosshair 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
        if (plotHov) {
            double snap = snapFreq(ImPlot::GetPlotMousePos().x);
            ImVec2 px = ImPlot::PlotToPixels(snap, 0.0);
            const float kDash = 6.0f, kGap = 4.0f;
            for (float y = pPos.y; y < pPos.y + pSize.y; y += kDash + kGap) {
                float yEnd = std::min(y + kDash, pPos.y + pSize.y);
                dl->AddLine({px.x, y}, {px.x, yEnd}, IM_COL32(200, 200, 200, 120), 1.0f);
            }
            hoverShow = true;
            hoverF    = snap;
            for (auto& fl : lines)
                hoverVals.push_back({fl.label, valueAt(fl, snap), fl.color});
        }

        // 鈹€鈹€ Persistent cursor: yellow line + value labels 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
        if (fftCursorActive_) {
            ImVec2 cpx = ImPlot::PlotToPixels(fftCursorFreq_, 0.0);
            if (cpx.x >= pPos.x - 1.0f && cpx.x <= pPos.x + pSize.x + 1.0f) {
                float cx = std::max(cpx.x, pPos.x);
                dl->AddLine({cx, pPos.y}, {cx, pPos.y + pSize.y},
                            IM_COL32(255, 220, 50, 230), 1.5f);
                // Header: frequency + harmonic index (h0 = DC).
                char hdr[64];
                char fbuf[48]; fmtHz(fbuf, sizeof(fbuf), fftCursorFreq_);
                if (fftF0_ > 0.0) {
                    int k = (int)std::lround(fftCursorFreq_ / fftF0_);
                    if (k == 0) std::snprintf(hdr, sizeof(hdr), "%s  DC", fbuf);
                    else        std::snprintf(hdr, sizeof(hdr), "%s  h%d", fbuf, k);
                } else {
                    std::snprintf(hdr, sizeof(hdr), "%s", fbuf);
                }
                float yOff = pPos.y + 4.0f;
                auto drawLabel = [&](const char* txt, ImU32 col) {
                    ImVec2 ts = ImGui::CalcTextSize(txt);
                    float tx = std::min(cx + 3.0f, pPos.x + pSize.x - ts.x - 2.0f);
                    dl->AddRectFilled({tx - 1, yOff - 1}, {tx + ts.x + 1, yOff + ts.y + 1},
                                      IM_COL32(20, 20, 20, 190));
                    dl->AddText({tx, yOff}, col, txt);
                    yOff += ts.y + 2.0f;
                };
                drawLabel(hdr, IM_COL32(255, 220, 50, 255));
                for (auto& fl : lines) {
                    char txt[96];
                    std::snprintf(txt, sizeof(txt), "%s=%.4g%s",
                                  fl.label.c_str(), valueAt(fl, fftCursorFreq_),
                                  fftDb_ ? "dB" : "");
                    drawLabel(txt, fl.color);
                }
            }
        }

        ImPlot::EndPlot();
    }

    // Hover tooltip (after EndPlot so it doesn't disturb IsPlotHovered).
    if (hoverShow && ImGui::BeginTooltip()) {
        char fbuf[48]; fmtHz(fbuf, sizeof(fbuf), hoverF);
        if (fftF0_ > 0.0) {
            int k = (int)std::lround(hoverF / fftF0_);
            if (k == 0) ImGui::Text("f = %s  (DC)", fbuf);
            else        ImGui::Text("f = %s  (h%d)", fbuf, k);
        } else {
            ImGui::Text("f = %s", fbuf);
        }
        for (const auto& hv : hoverVals) {
            float r = ((hv.col >> 0)  & 0xFF) / 255.0f;
            float g = ((hv.col >> 8)  & 0xFF) / 255.0f;
            float b = ((hv.col >> 16) & 0xFF) / 255.0f;
            ImGui::ColorButton("##cv", ImVec4(r, g, b, 1.0f),
                ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder,
                ImVec2(10.0f, 10.0f));
            ImGui::SameLine();
            ImGui::Text("%s = %.4g%s", hv.lbl.c_str(), hv.v, fftDb_ ? "dB" : "");
        }
        ImGui::EndTooltip();
    }

    ImGui::End();
}

// ─────────────────────── CSV export ──────────────────────────────────────────
//
// Exports every visible signal across all plots of this scope. The time column
// comes from the entry with the most samples; all signals produced by one
// simulation run share the same time base, so values line up exactly. Signals
// from a different run (mixed-source scope) are matched by nearest sample time
// — the same snapping the hover crosshair uses.

#ifdef _WIN32
static bool pickCsvSavePath(char* buf, int n) {
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "CSV (comma separated)\0*.csv\0All Files\0*.*\0\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = static_cast<DWORD>(n);
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = "csv";
    buf[0] = '\0';
    return GetSaveFileNameA(&ofn) != 0;
}
#endif

void ScopeView::exportCsv(MainViewModel& vm, bool visibleOnly) {
    ScopeModel& scope = vm.scope(scopeIdx_);

    // Collect visible entries that actually hold data, in plot order.
    std::vector<const MuxEntry*> entries;
    for (int pi = 0; pi < scope.plotCount(); pi++) {
        const PlotArea* p = scope.getPlot(pi);
        if (!p) continue;
        for (const auto& e : p->entries)
            if (e->visible && e->buffer.getCount() > 0)
                entries.push_back(e.get());
    }
    if (entries.empty()) return;

    char path[1024];
#ifdef _WIN32
    if (!pickCsvSavePath(path, sizeof(path))) return;  // user cancelled
#else
    std::snprintf(path, sizeof(path), "scope_export.csv");
#endif

    std::ofstream f(path);
    if (!f) return;

    // Quote fields containing separators so labels like "a,b" stay one column.
    auto csvField = [](const std::string& s) -> std::string {
        if (s.find_first_of(",\"\n") == std::string::npos) return s;
        std::string q = "\"";
        for (char c : s) { if (c == '"') q += '"'; q += c; }
        q += '"';
        return q;
    };
    f << "time";
    for (const auto* e : entries) f << ',' << csvField(e->effectiveLabel());
    f << '\n';

    // Master time base: the entry with the most samples.
    const MuxEntry* master = entries[0];
    for (const auto* e : entries)
        if (e->buffer.getCount() > master->buffer.getCount()) master = e;

    // Nearest-sample lookup over logical ring-buffer indices (X is sorted).
    auto nearestY = [](const ScrollingBuffer& buf, double t) -> double {
        int n = buf.getCount();
        int lo = 0, hi = n - 1;
        while (lo < hi) {
            int mid = (lo + hi) / 2;
            if (buf.getXAt(mid) < t) lo = mid + 1;
            else                     hi = mid;
        }
        if (lo > 0) {
            double d0 = std::abs(buf.getXAt(lo - 1) - t);
            double d1 = std::abs(buf.getXAt(lo)     - t);
            if (d0 < d1) lo--;
        }
        return buf.getYAt(lo);
    };

    char num[40];
    int rows = master->buffer.getCount();
    for (int r = 0; r < rows; r++) {
        double t = master->buffer.getXAt(r);
        if (visibleOnly && (t < xLinkMin_ || t > xLinkMax_)) continue;
        std::snprintf(num, sizeof(num), "%.9g", t);
        f << num;
        for (const auto* e : entries) {
            double y = (e == master) ? e->buffer.getYAt(r)
                                     : nearestY(e->buffer, t);
            std::snprintf(num, sizeof(num), "%.9g", y);
            f << ',' << num;
        }
        f << '\n';
    }
}

// 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€ Scope state persistence 鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€鈹€
//
// Format (appended to .sch after all W lines):
//   XSCOPE <xMin> <xMax>       鈫?'X' prefix avoids collision with 'S' sim-config tag
//   PLOT <title>
//   XSIG <signalName>|<label>|<scale>|<sigNameB>|<colorHex>
//   YRANGE <yMin> <yMax>
//   ENDSCOPE
//
void ScopeView::saveState(std::ostream& out, const MainViewModel& vm) const {
    out << "XSCOPE " << xLinkMin_ << ' ' << xLinkMax_ << '\n';
    // Capture this scope's window pos/size from imgui's settings storage so the
    // .sch file can fully restore the layout independently of imgui.ini.
    ImGuiWindowSettings* ws = ImGui::FindWindowSettingsByID(ImHashStr(title_.c_str()));
    if (ws) {
        out << "XSCOPEGEO " << ws->Pos.x << ' ' << ws->Pos.y
            << ' ' << ws->Size.x << ' ' << ws->Size.y << '\n';
    }

    // Persist computed (virtual) signal definitions used by this scope's entries.
    // ComputedSigs live in MainViewModel and are NOT in the .PROBE list, so without
    // this the entries restore visually but receive no data on Build & Run.
    const ScopeModel& scope = vm.scope(scopeIdx_);
    std::unordered_set<std::string> usedNames;
    for (int pi = 0; pi < scope.plotCount(); pi++) {
        const PlotArea* p = scope.getPlot(pi);
        if (!p) continue;
        for (const auto& e : p->entries) usedNames.insert(e->signalName);
    }
    for (const auto& cs : vm.computedSigs()) {
        if (usedNames.find(cs.name) == usedNames.end()) continue;
        char buf[512];
        std::snprintf(buf, sizeof(buf), "XCSIG %s|%s|%g|%s|%g\n",
            cs.name.c_str(), cs.sigA.c_str(), cs.kA,
            cs.sigB.c_str(), cs.kB);
        out << buf;
    }
}

void ScopeView::loadState(std::istream& in, MainViewModel& vm, int sourceSchId) {
    ScopeModel& scope = vm.scope(scopeIdx_);
    double xMin = 0.0, xMax = 0.01;
    in >> xMin >> xMax;
    xLinkMin_ = xMin; xLinkMax_ = xMax;
    // Arm pending restore so that the next tEnd change doesn't override this range
    pendingXRestore_ = true;
    pendingXMin_     = xMin;
    pendingXMax_     = xMax;

    // Reset scope to empty
    while (scope.plotCount() > 1) scope.removePlot(scope.plotCount() - 1);
    scope.clearPlotEntries(0);
    plotYStates_.clear();
    plotStructureChanged_ = true;

    std::string line;
    std::getline(in, line);  // consume remainder of XSCOPE line
    int plotIdx = -1;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string tag; ss >> tag;
        if (tag == "PLOT") {
            std::string title; std::getline(ss, title);
            if (!title.empty() && title[0] == ' ') title = title.substr(1);
            if (plotIdx < 0) {
                PlotArea* p = scope.getPlot(0);
                if (p) p->title = title;
                plotIdx = 0;
            } else {
                int newIdx = scope.insertPlot(plotIdx, title);
                ensurePlotYStates(scope.plotCount());
                plotIdx = newIdx;
            }
            ensurePlotYStates(scope.plotCount());
        } else if (tag == "XCSIG") {
            // Computed/virtual signal definition: name|sigA|kA|sigB|kB
            std::string encoded; ss >> encoded;
            std::vector<std::string> parts;
            std::string cur;
            for (char c : encoded) {
                if (c == '|') { parts.push_back(cur); cur.clear(); }
                else cur += c;
            }
            parts.push_back(cur);
            while (parts.size() < 5) parts.push_back("");
            const std::string& name = parts[0];
            const std::string& sigA = parts[1];
            double kA = parts[2].empty() ? 1.0 : std::stod(parts[2]);
            const std::string& sigB = parts[3];
            double kB = parts[4].empty() ? 0.0 : std::stod(parts[4]);
            if (!name.empty() && !sigA.empty())
                vm.registerComputedSig(name, sigA, kA, sigB, kB);
        } else if (tag == "XSIG" && plotIdx >= 0) {
            std::string encoded; ss >> encoded;
            // Parse pipe-delimited: sigName|label|scale|sigNameB|colorHex
            std::vector<std::string> parts;
            std::string cur;
            for (char c : encoded) {
                if (c == '|') { parts.push_back(cur); cur.clear(); }
                else cur += c;
            }
            parts.push_back(cur);
            while (parts.size() < 5) parts.push_back("");
            const std::string& sigName  = parts[0];
            const std::string& lbl      = parts[1];
            double scale = parts[2].empty() ? 1.0 : std::stod(parts[2]);
            const std::string& sigNameB = parts[3];
            ImU32 color = parts[4].empty() ? ScopeModel::nextColor()
                : (ImU32)std::stoul(parts[4], nullptr, 16);
            scope.addSignalToPlot(plotIdx, sigName, lbl, color, scale, sigNameB, sourceSchId);
        } else if (tag == "XSCOPEGEO") {
            float px, py, sx, sy;
            ss >> px >> py >> sx >> sy;
            setPendingWindowGeometry({px, py}, {sx, sy});
        } else if (tag == "YRANGE" && plotIdx >= 0) {
            double yMin, yMax; ss >> yMin >> yMax;
            ensurePlotYStates(scope.plotCount());
            if (plotIdx < (int)plotYStates_.size())
                plotYStates_[plotIdx] = {yMin, yMax, true};
            // Suppress the per-plot auto-fit pass that would otherwise run on
            // the first render of a freshly-inserted plot (PlotArea::autoFitY
            // defaults to true) and clobber the YRANGE we just restored. Only
            // plot 0 escaped the bug previously because it was reused from the
            // ScopeModel constructor and its autoFitY had already been cleared.
            if (PlotArea* p = scope.getPlot(plotIdx))
                p->autoFitY = false;
        } else if (tag == "ENDSCOPE") {
            break;
        }
    }
    ensurePlotYStates(scope.plotCount());
}

