#include "views/scope_view.h"
#include "view_model/main_view_model.h"
#include "view_model/scope_model.h"
#include <imgui.h>
#include <implot.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

ScopeView::ScopeView() : BaseView("Scope") {}

// ─────────────────────── Feature-preserving decimation ────────────────────
//
// Divides the visible X window into maxPts/2 buckets.
// Within each bucket the min-Y and max-Y points are kept (in time order),
// which guarantees that every peak and valley is preserved regardless of
// the downsample ratio.  Returns the number of output points.
//
// Parameters
//   xs/ys   – monotonically-increasing-in-X source array (length n)
//   xMin/xMax – currently visible X range (from the linked axis)
//   maxPts  – target output budget (≈ 2 × plot pixel width)
//   outX/outY – output vectors (cleared on entry)
//
static int decimateMinMax(
    const double* xs, const double* ys, int n,
    double xMin, double xMax, int maxPts,
    std::vector<double>& outX, std::vector<double>& outY)
{
    outX.clear();
    outY.clear();
    if (n <= 0 || maxPts < 2) return 0;

    // Binary-search for the visible window [lo, hi)
    int lo = (int)(std::lower_bound(xs, xs + n, xMin) - xs);
    int hi = (int)(std::upper_bound(xs, xs + n, xMax) - xs);
    if (lo > 0) lo--;   // one point before left edge so line reaches the axis
    if (hi < n) hi++;   // one point after right edge
    int vis = hi - lo;
    if (vis <= 0) return 0;

    // All visible points fit — return them verbatim
    if (vis <= maxPts) {
        outX.insert(outX.end(), xs + lo, xs + hi);
        outY.insert(outY.end(), ys + lo, ys + hi);
        return vis;
    }

    // Min-max decimation: divide visible window into nBuckets equal-width buckets.
    // Keep the min-Y and max-Y point from each bucket, output in time order.
    // This preserves all local extrema (peaks and valleys) even at heavy downsampling.
    int nBuckets = std::max(1, maxPts / 2);
    outX.reserve(nBuckets * 2 + 2);
    outY.reserve(nBuckets * 2 + 2);

    for (int b = 0; b < nBuckets; b++) {
        // Integer arithmetic avoids floating-point accumulation across buckets
        int bLo = lo + (int)((long long)vis * b       / nBuckets);
        int bHi = lo + (int)((long long)vis * (b + 1) / nBuckets);
        if (bHi > hi) bHi = hi;
        if (bLo >= bHi) continue;

        // Locate min-Y and max-Y within the bucket
        int minIdx = bLo, maxIdx = bLo;
        for (int i = bLo + 1; i < bHi; i++) {
            if (ys[i] < ys[minIdx]) minIdx = i;
            if (ys[i] > ys[maxIdx]) maxIdx = i;
        }

        // Output in chronological order so the line follows the true waveform
        int first  = std::min(minIdx, maxIdx);
        int second = std::max(minIdx, maxIdx);
        outX.push_back(xs[first]);  outY.push_back(ys[first]);
        if (first != second) {
            outX.push_back(xs[second]); outY.push_back(ys[second]);
        }
    }

    return (int)outX.size();
}

// ─────────────────────── Helpers ──────────────────────────────────────────
void ScopeView::ensurePlotYStates(int count) {
    while ((int)plotYStates_.size() < count)
        plotYStates_.push_back({-1.0, 1.0, false});
}

// Insert a plot and keep plotYStates_ positionally in sync so existing
// plots' Y ranges are not disturbed by the index shift.
//
// Root cause of the original bug: ScopeModel re-numbers plot titles on
// every insert/remove.  ImPlot uses the title string as its internal ID —
// once a title changes, ImPlot treats the plot as brand-new and resets its
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

// ─────────────────────── Smart axis formatting ─────────────────────────────
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

// ─────────────────────── Undo stack ───────────────────────────────────────
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

// ─────────────────────── Data-driven auto-fit ─────────────────────────────
// Does NOT use ImPlot's AutoFit flag — gives full control over axis limits
// and ensures correct undo behaviour.
// allData = true  → scan entire stored buffer regardless of visible X range
//                   (used by the "Auto-Fit All" toolbar button)
// allData = false → scan only the currently visible X range
//                   (used by the right-click "Auto-Fit This Plot" action)
void ScopeView::computeAutoFitPlot(MainViewModel& vm, int plotIndex, bool allData) {
    ScopeModel& scope = vm.scope();
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
    pushSnapshot(vm.scope().plotCount());
    // Step 1: reset X axis to full simulation range
    xLinkMin_ = 0.0;
    xLinkMax_ = vm.simConfig().t_end;
    // Step 2: auto-fit Y for each plot (scan all stored data)
    for (int i = 0; i < vm.scope().plotCount(); i++)
        computeAutoFitPlot(vm, i, /*allData=*/true);
}

// ─────────────────────── Main render ──────────────────────────────────────
void ScopeView::render(MainViewModel& vm) {
    if (!visible_) return;
    if (!ImGui::Begin(title_.c_str(), &visible_,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::End();
        return;
    }

    // Reset per-frame structure-change guard.  Must happen before any
    // insertPlot/removePlot calls (toolbar buttons, context menu) so that
    // the flag correctly reflects whether the plot list changed this frame.
    plotStructureChanged_ = false;

    ScopeModel& scope = vm.scope();

    // Reset linked X range whenever t_end changes (new netlist or config applied)
    {
        double tEnd = vm.simConfig().t_end;
        if (tEnd != lastTEnd_) {
            xLinkMin_ = 0.0;
            xLinkMax_ = tEnd;
            lastTEnd_  = tEnd;
        }
    }

    // Ctrl+Z undo — only when this window is focused and the user is not typing
    // Uses ImGuiFocusedFlags_RootAndChildWindows so child plot panels count.
    // The check is intentionally scoped here so future schematic-view undo
    // (handled in SchematicView::render) cannot fire from the scope window.
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)
        && !ImGui::GetIO().WantTextInput
        && ImGui::GetIO().KeyCtrl
        && ImGui::IsKeyPressed(ImGuiKey_Z, /*repeat=*/false)) {
        applyUndo();
    }

    ensurePlotYStates(scope.plotCount());

    // ── Toolbar ───────────────────────────────────────────────────────────────
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

    ImGui::Separator();

    // Calculate equal plot height.
    // Between plots we emit one ImGui::Spacing() which consumes ItemSpacing.y.
    int   n          = scope.plotCount();
    float avail      = ImGui::GetContentRegionAvail().y
                     - (n - 1) * ImGui::GetStyle().ItemSpacing.y;
    float plotHeight = (avail / n) > 60.0f ? (avail / n) : 60.0f;

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

    // Apply any pending zoom action NOW — after all EndPlot() calls.
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

// ─────────────────────── Per-plot rendering ───────────────────────────────
void ScopeView::renderPlot(MainViewModel& vm, PlotArea& plot,
                           int plotIndex, float plotHeight, bool isBottom) {
    ScopeModel& scope = vm.scope();

    // Highlight border of the selected plot
    bool selected = (plotIndex == scope.selectedPlot());
    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    }

    // Always disable ImPlot's built-in box-select; we implement our own zoom drag.
    ImPlotFlags plotFlags = ImPlotFlags_NoMenus | ImPlotFlags_NoBoxSelect;

    // Estimate plot width BEFORE BeginPlot — GetPlotSize() locks the setup phase,
    // so all Setup* calls must precede any non-setup API.
    float plotWidthPx = ImGui::GetContentRegionAvail().x;

    // Compact title area: reduce vertical inner padding from the default (10,10)
    // to (10,3) so the title text sits closer to the plot content.
    ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(10.0f, 3.0f));
    bool plotOk = ImPlot::BeginPlot(plot.title.c_str(), ImVec2(-1, plotHeight), plotFlags);
    ImPlot::PopStyleVar(); // PlotPadding

    // Pop style immediately — border rendering happens inside BeginPlot
    if (selected) {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
    if (!plotOk) return;

    // ── Axis setup ────────────────────────────────────────────────────────────
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
    // No AutoFit flag — we control limits ourselves for accurate undo.
    ImPlot::SetupAxis(ImAxis_Y1, "Value", ImPlotAxisFlags_None);

    // Apply stored Y limits if requested (after auto-fit, V-zoom, or undo)
    ensurePlotYStates(plotIndex + 1);
    double yMin = plotYStates_[plotIndex].yMin;
    double yMax = plotYStates_[plotIndex].yMax;
    if (plotYStates_[plotIndex].forceSet) {
        ImPlot::SetupAxisLimits(ImAxis_Y1, yMin, yMax, ImPlotCond_Always);
        plotYStates_[plotIndex].forceSet = false;
    }
    AxisFmtParams yFmt = computeAxisFmt(yMin, yMax, plotWidthPx);
    ImPlot::SetupAxisFormat(ImAxis_Y1, axisFormatterCallback, &yFmt);

    // ── Decimated signal rendering ────────────────────────────────────────────
    // Target: at most 2 × plot pixel width output points per signal.
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

        // Decimate one contiguous segment and render it.
        // decX_/decY_ are class members reused each call (no per-frame allocation).
        ImPlotSpec lineSpec(ImPlotProp_LineColor, col, ImPlotProp_LineWeight, 1.5f);
        auto renderSeg = [&](const char* label, int segStart, int segLen) {
            if (segLen <= 0) return;
            int n = decimateMinMax(
                xd + segStart, yd + segStart, segLen,
                xLinkMin_, xLinkMax_, maxPts, decX_, decY_);
            if (n > 0)
                ImPlot::PlotLine(label, decX_.data(), decY_.data(), n, lineSpec);
        };

        if (offset >= count) {
            // Buffer not yet wrapped — one contiguous block [0, count)
            renderSeg(entry->signalName.c_str(), 0, count);
        } else {
            // Wrapped ring buffer: older segment [offset, count), newer [0, offset)
            // Both are monotonically increasing in X; rendered as separate PlotLine
            // calls.  The gap between them is ≤ dt (imperceptible at normal zoom).
            renderSeg(entry->signalName.c_str(), offset, count - offset);
            renderSeg("##seg2", 0, offset);
        }
    }

    // ── Auto-zoom drag (always active) ────────────────────────────────────────
    // • Vertical   screen drag (|dy| ≥ |dx|) → H-zoom: select X / time range
    // • Horizontal screen drag (|dy| < |dx|) → V-zoom: select Y range (this plot)
    // • Short click (threshold not exceeded)  → select this plot
    // • Drag on axis area                     → ImPlot native axis pan (unchanged)
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
        //   axis hover OR no interaction → Pan = Left  (axis drag pans normally)
        //   plot body OR active drag     → Pan = Middle (we handle left drag)
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

        // Active drag — originating plot processes direction lock, drawing, release
        if (autoDragActive_ && autoDragPlotIdx_ == plotIndex) {
            float adx = std::abs(mpos.x - autoDragStartScr_.x);
            float ady = std::abs(mpos.y - autoDragStartScr_.y);

            // Determine / update zoom direction with hysteresis to prevent flicker.
            // Initial lock: whichever axis has larger displacement wins (past kThresh).
            // Subsequent re-evaluation: need kHysteresis px advantage to switch mode.
            //   V-zoom → H-zoom : adx > ady + kHysteresis
            //   H-zoom → V-zoom : ady > adx + kHysteresis
            const float kHysteresis = 20.0f;
            if (!autoDragDirLocked_) {
                if (adx > kThresh || ady > kThresh) {
                    autoDragDirLocked_ = true;
                    autoDragIsH_       = (ady >= adx);
                }
            } else {
                if (!autoDragIsH_ && adx > ady + kHysteresis)
                    autoDragIsH_ = true;   // V-zoom → H-zoom
                else if (autoDragIsH_ && ady > adx + kHysteresis)
                    autoDragIsH_ = false;  // H-zoom → V-zoom
            }

            if (autoDragDirLocked_) {
                ImPlotPoint cur = ImPlot::GetPlotMousePos();
                ImDrawList* dl  = ImPlot::GetPlotDrawList();
                ImVec2 pPos     = ImPlot::GetPlotPos();
                ImVec2 pSize    = ImPlot::GetPlotSize();

                if (autoDragIsH_) {
                    // H-zoom: vertical drag → X selection band
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
                    // V-zoom: horizontal drag → Y selection band (this plot only)
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
            // Do NOT modify xLinkMin_/xLinkMax_ or plotYStates_ directly here —
            // ImPlot writes linked-axis values back at EndPlot(), which would
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
                    // Short click (no direction locked) → select this plot
                    scope.setSelectedPlot(plotIndex);
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

    // ── Right-click context menu ──────────────────────────────────────────────
    if (ImPlot::IsPlotHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup("PlotCtx");
    }
    if (ImGui::BeginPopup("PlotCtx")) {
        renderPlotContextMenu(vm, plotIndex);
        ImGui::EndPopup();
    }

    // ── Scale annotation for scaled-scientific axes ──
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

    // ── Track current Y limits for accurate undo snapshots ────────────────────
    // GetPlotLimits() returns what ImPlot is actually displaying this frame
    // (including any scroll-wheel zoom or pan the user did with the mouse).
    // We cache this so pushSnapshot() always captures the real current state.
    //
    // Guard: if a plot was inserted or removed during this frame (e.g. from
    // the context menu inside this very renderPlot call), the plotYStates_
    // vector has already been correctly updated by insertPlot/removePlot
    // (with forceSet=true and the right yMin/yMax).  Writing GetPlotLimits()
    // here would overwrite those correct values with stale data from whichever
    // plot happened to be at plotIndex before the structural change.
    if (!plotStructureChanged_) {
        ImPlotRect lim = ImPlot::GetPlotLimits(ImAxis_X1, ImAxis_Y1);
        plotYStates_[plotIndex].yMin = lim.Y.Min;
        plotYStates_[plotIndex].yMax = lim.Y.Max;
    }

    ImPlot::EndPlot();
}

// ─────────────────────── Context menu ─────────────────────────────────────
void ScopeView::renderPlotContextMenu(MainViewModel& vm, int plotIndex) {
    ScopeModel& scope = vm.scope();

    if (ImGui::MenuItem("Insert Plot Above"))
        insertPlot(scope, plotIndex - 1);
    if (ImGui::MenuItem("Insert Plot Below"))
        insertPlot(scope, plotIndex);
    if (ImGui::MenuItem("Delete Plot", nullptr, false, scope.plotCount() > 1))
        removePlot(scope, plotIndex);
    if (ImGui::MenuItem("Auto-Fit This Plot")) {
        pushSnapshot(scope.plotCount());
        computeAutoFitPlot(vm, plotIndex);
    }
    ImGui::Separator();
    renderAddSignalMenu(vm, plotIndex);
    renderRemoveSignalMenu(vm, plotIndex);
}

void ScopeView::renderAddSignalMenu(MainViewModel& vm, int plotIndex) {
    if (!ImGui::BeginMenu("Add Signal")) return;

    ScopeModel& scope = vm.scope();
    PlotArea*   plot  = scope.getPlot(plotIndex);

    for (const auto& sig : vm.availableSignals()) {
        MuxEntry* entry = plot ? plot->findEntry(sig.name) : nullptr;
        bool shown = entry && entry->visible;
        if (ImGui::MenuItem(sig.name.c_str(), nullptr, shown)) {
            if (!shown && plot)
                scope.addSignalToPlot(plotIndex, sig.name, ScopeModel::nextColor());
        }
    }

    ImGui::EndMenu();
}

void ScopeView::renderRemoveSignalMenu(MainViewModel& vm, int plotIndex) {
    ScopeModel& scope = vm.scope();
    PlotArea*   plot  = scope.getPlot(plotIndex);
    if (!plot) return;

    bool hasVisible = false;
    for (const auto& entry : plot->entries)
        if (entry->visible) { hasVisible = true; break; }
    if (!hasVisible) return;

    if (!ImGui::BeginMenu("Remove Signal")) return;

    for (auto& entry : plot->entries) {
        if (!entry->visible) continue;
        if (ImGui::MenuItem(entry->signalName.c_str())) {
            scope.removeSignalFromPlot(plotIndex, entry->signalName);
            break;
        }
    }

    ImGui::EndMenu();
}
