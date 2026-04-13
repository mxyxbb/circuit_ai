#include "views/scope_view.h"
#include "view_model/main_view_model.h"
#include "view_model/scope_model.h"
#include <imgui.h>
#include <implot.h>
#include <algorithm>
#include <cfloat>
#include <cmath>

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
    // Scan ALL stored data — not just the currently visible X window
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

    // When H-Zoom or V-Zoom is active, move ImPlot's default left-mouse pan to
    // middle mouse so our custom drag does not conflict with ImPlot's pan.
    if (zoomMode_ != ZoomMode::None) {
        ImPlot::GetInputMap().Pan = ImGuiMouseButton_Middle;
    } else {
        ImPlot::GetInputMap().Pan = ImGuiMouseButton_Left;
    }

    // ── Toolbar ───────────────────────────────────────────────────────────────
    if (ImGui::Button("+ Plot Above")) {
        scope.insertPlot(scope.selectedPlot() - 1);
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Plot Below")) {
        scope.insertPlot(scope.selectedPlot());
    }
    ImGui::SameLine();
    if (ImGui::Button("Auto-Fit All")) {
        computeAutoFitAll(vm);
    }
    ImGui::SameLine();

    // H-Zoom toggle (highlighted when active)
    {
        bool active = (zoomMode_ == ZoomMode::HZoom);
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button("H-Zoom")) {
            zoomMode_   = active ? ZoomMode::None : ZoomMode::HZoom;
            dragActive_ = false;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Drag to select horizontal (time) zoom range");
        if (active) ImGui::PopStyleColor();
    }
    ImGui::SameLine();

    // V-Zoom toggle
    {
        bool active = (zoomMode_ == ZoomMode::VZoom);
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Button,
                                  ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
        if (ImGui::Button("V-Zoom")) {
            zoomMode_   = active ? ZoomMode::None : ZoomMode::VZoom;
            dragActive_ = false;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Drag in a plot to select its vertical zoom range");
        if (active) ImGui::PopStyleColor();
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

    // Calculate equal plot height
    int   n          = scope.plotCount();
    float avail      = ImGui::GetContentRegionAvail().y
                     - (n - 1) * ImGui::GetStyle().ItemSpacing.y;
    float plotHeight = (avail / n) > 80.0f ? (avail / n) : 80.0f;

    for (int i = 0; i < n; i++) {
        ImGui::PushID(i);
        PlotArea* plot = scope.getPlot(i);
        if (plot) {
            // Handle autoFitY flag set externally (e.g. ScopeModel::autoFitAll)
            if (plot->autoFitY) {
                computeAutoFitPlot(vm, i, /*allData=*/true);
                plot->autoFitY = false;
            }
            renderPlot(vm, *plot, i, plotHeight);
        }
        ImGui::PopID();
        if (i < n - 1) ImGui::Separator();
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
                           int plotIndex, float plotHeight) {
    ScopeModel& scope = vm.scope();

    // Highlight border of the selected plot
    bool selected = (plotIndex == scope.selectedPlot());
    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    }

    ImPlotFlags plotFlags = ImPlotFlags_NoMenus;
    if (zoomMode_ != ZoomMode::None)
        plotFlags |= ImPlotFlags_NoBoxSelect;  // disable ImPlot's own box-select

    bool plotOk = ImPlot::BeginPlot(plot.title.c_str(), ImVec2(-1, plotHeight), plotFlags);

    // Pop style immediately — border rendering happens inside BeginPlot
    if (selected) {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
    if (!plotOk) return;

    // ── Axis setup ────────────────────────────────────────────────────────────
    // X axis: linked so all plots pan/zoom together
    ImPlot::SetupAxis(ImAxis_X1, "Time (s)");
    ImPlot::SetupAxisLinks(ImAxis_X1, &xLinkMin_, &xLinkMax_);
    ImPlot::SetupAxisFormat(ImAxis_X1, "%.4f");

    // Y axis: fixed-width scientific notation keeps label columns aligned.
    // No AutoFit flag — we control limits ourselves for accurate undo.
    ImPlot::SetupAxis(ImAxis_Y1, "Value", ImPlotAxisFlags_None);
    ImPlot::SetupAxisFormat(ImAxis_Y1, "% .3e");

    // Apply stored Y limits if requested (after auto-fit, V-zoom, or undo)
    ensurePlotYStates(plotIndex + 1);
    if (plotYStates_[plotIndex].forceSet) {
        ImPlot::SetupAxisLimits(ImAxis_Y1,
            plotYStates_[plotIndex].yMin,
            plotYStates_[plotIndex].yMax,
            ImPlotCond_Always);
        plotYStates_[plotIndex].forceSet = false;
    }

    // ── Decimated signal rendering ────────────────────────────────────────────
    // Target: at most 2 × plot pixel width output points per signal.
    // The decimateMinMax algorithm preserves all peaks and valleys by keeping
    // the min-Y and max-Y point within each equal-width bucket.
    float plotWidthPx = ImPlot::GetPlotSize().x;
    int   maxPts      = std::max(500, static_cast<int>(plotWidthPx * 2.0f));

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

    // ── Zoom drag handling ─────────────────────────────────────────────────────
    if (zoomMode_ != ZoomMode::None) {
        // Show a hand cursor to signal that drag-zoom is active
        if (ImPlot::IsPlotHovered())
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

        // Start drag on left-click in any plot
        if (ImPlot::IsPlotHovered()
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !dragActive_) {
            dragStartPlot_ = ImPlot::GetPlotMousePos();
            dragPlotIdx_   = plotIndex;
            dragActive_    = true;
        }

        if (dragActive_) {
            ImPlotPoint cur = ImPlot::GetPlotMousePos();
            ImDrawList* dl  = ImPlot::GetPlotDrawList();
            ImVec2 plotPos  = ImPlot::GetPlotPos();
            ImVec2 plotSize = ImPlot::GetPlotSize();

            if (zoomMode_ == ZoomMode::HZoom) {
                // Draw vertical selection band in EVERY plot (shared X axis)
                ImVec2 pA = ImPlot::PlotToPixels(dragStartPlot_.x, 0.0);
                ImVec2 pB = ImPlot::PlotToPixels(cur.x, 0.0);
                float  xA = std::min(pA.x, pB.x);
                float  xB = std::max(pA.x, pB.x);
                float  y0 = plotPos.y, y1 = plotPos.y + plotSize.y;
                dl->AddRectFilled({xA, y0}, {xB, y1}, IM_COL32(100, 150, 255,  50));
                dl->AddLine({xA, y0}, {xA, y1}, IM_COL32(100, 150, 255, 220), 1.5f);
                dl->AddLine({xB, y0}, {xB, y1}, IM_COL32(100, 150, 255, 220), 1.5f);

            } else if (zoomMode_ == ZoomMode::VZoom && dragPlotIdx_ == plotIndex) {
                // Draw horizontal selection band only in the dragged plot
                ImVec2 pA = ImPlot::PlotToPixels(0.0, dragStartPlot_.y);
                ImVec2 pB = ImPlot::PlotToPixels(0.0, cur.y);
                float  yA = std::min(pA.y, pB.y);
                float  yB = std::max(pA.y, pB.y);
                float  x0 = plotPos.x, x1 = plotPos.x + plotSize.x;
                dl->AddRectFilled({x0, yA}, {x1, yB}, IM_COL32(255, 150, 100,  50));
                dl->AddLine({x0, yA}, {x1, yA}, IM_COL32(255, 150, 100, 220), 1.5f);
                dl->AddLine({x0, yB}, {x1, yB}, IM_COL32(255, 150, 100, 220), 1.5f);
            }

            // Capture drag end on mouse release — handled only in the originating plot.
            // We do NOT modify xLinkMin_/xLinkMax_ or plotYStates_ here because
            // ImPlot writes linked-axis values back at EndPlot(), which would
            // immediately overwrite any changes made inside BeginPlot/EndPlot.
            // Instead we store the intent in pendingZoom_ and apply it AFTER
            // all EndPlot() calls (see the bottom of render()).
            if (dragPlotIdx_ == plotIndex
                && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                if (zoomMode_ == ZoomMode::HZoom) {
                    double lo = std::min(dragStartPlot_.x, cur.x);
                    double hi = std::max(dragStartPlot_.x, cur.x);
                    if (hi - lo > 1e-15)
                        pendingZoom_ = {true, true, lo, hi, plotIndex};
                } else if (zoomMode_ == ZoomMode::VZoom) {
                    double lo = std::min(dragStartPlot_.y, cur.y);
                    double hi = std::max(dragStartPlot_.y, cur.y);
                    if (hi - lo > 1e-15)
                        pendingZoom_ = {true, false, lo, hi, plotIndex};
                }
                dragActive_  = false;
                dragPlotIdx_ = -1;
            }
        }
    }

    // ── Click to select (only when not in zoom mode) ──────────────────────────
    if (zoomMode_ == ZoomMode::None
        && ImPlot::IsPlotHovered()
        && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        scope.setSelectedPlot(plotIndex);
    }

    // ── Right-click context menu ──────────────────────────────────────────────
    if (ImPlot::IsPlotHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup("PlotCtx");
    }
    if (ImGui::BeginPopup("PlotCtx")) {
        renderPlotContextMenu(vm, plotIndex);
        ImGui::EndPopup();
    }

    // ── Track current Y limits for accurate undo snapshots ────────────────────
    // GetPlotLimits() returns what ImPlot is actually displaying this frame
    // (including any scroll-wheel zoom or pan the user did with the mouse).
    // We cache this so pushSnapshot() always captures the real current state.
    {
        ImPlotRect lim = ImPlot::GetPlotLimits(ImAxis_X1, ImAxis_Y1);
        // Only update if we did not force-set this frame (forceSet was cleared above)
        plotYStates_[plotIndex].yMin = lim.Y.Min;
        plotYStates_[plotIndex].yMax = lim.Y.Max;
    }

    ImPlot::EndPlot();
}

// ─────────────────────── Context menu ─────────────────────────────────────
void ScopeView::renderPlotContextMenu(MainViewModel& vm, int plotIndex) {
    ScopeModel& scope = vm.scope();

    if (ImGui::MenuItem("Insert Plot Above"))
        scope.insertPlot(plotIndex - 1);
    if (ImGui::MenuItem("Insert Plot Below"))
        scope.insertPlot(plotIndex);
    if (ImGui::MenuItem("Delete Plot", nullptr, false, scope.plotCount() > 1))
        scope.removePlot(plotIndex);
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
