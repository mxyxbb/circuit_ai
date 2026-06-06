#include "views/scope_view.h"
#include "view_model/main_view_model.h"
#include "view_model/scope_model.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <implot.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_set>

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
    bool fullPass      = viewChanged || countRegress || genChanged || wrapped || (c.lastSrcCount < 0);

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
            }
        }
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
    if (ImGui::Button("+ Plot Above")) {
        insertPlot(scope, scope.selectedPlot() - 1);
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Plot Below")) {
        insertPlot(scope, scope.selectedPlot());
    }
    ImGui::SameLine();
    if (ImGui::Button("Auto-Fit All")) {
        computeAutoFitAll(vm);
    }
    ImGui::SameLine();

    // Undo (greyed out when history is empty)
    {
        bool hasHistory = !zoomHistory_.empty();
        if (!hasHistory) ImGui::BeginDisabled();
        if (ImGui::Button("Undo")) applyUndo();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Undo zoom / auto-fit  (Ctrl+Z)");
        if (!hasHistory) ImGui::EndDisabled();
    }
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

