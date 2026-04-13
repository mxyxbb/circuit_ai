#pragma once
#include "views/base_view.h"
#include <vector>
#include <implot.h>   // ImPlotPoint

class MainViewModel;
struct PlotArea;

// ── Zoom interaction mode ────────────────────────────────────────────────────
enum class ZoomMode { None, HZoom, VZoom };

// Zoom action captured inside BeginPlot/EndPlot and applied after all EndPlot
// calls — necessary because ImPlot writes back linked-axis values at EndPlot,
// which would overwrite any xLinkMin_/xLinkMax_ changes made inside the plot.
struct PendingZoom {
    bool   active  = false;
    bool   isH     = true;   // true = H-zoom (X axis), false = V-zoom (Y axis)
    double lo      = 0.0;
    double hi      = 0.0;
    int    plotIdx = -1;     // which plot (used for V-zoom)
};

// Per-plot Y axis state — written each frame from GetPlotLimits(),
// applied next frame via SetupAxisLimits(ImPlotCond_Always) when forceSet is true.
struct PlotYState {
    double yMin     = -1.0;
    double yMax     =  1.0;
    bool   forceSet = false;  // if true: force-apply this frame, then clear
};

// Full view-state snapshot stored on the undo stack.
struct ZoomSnapshot {
    double xMin = 0.0, xMax = 0.01;
    struct PY { double yMin, yMax; };
    std::vector<PY> plotY;
};

class ScopeView : public BaseView {
public:
    ScopeView();
    void render(MainViewModel& vm) override;

private:
    // ── Rendering ────────────────────────────────────────────────────────────
    void renderPlot(MainViewModel& vm, PlotArea& plot, int plotIndex, float plotHeight);
    void renderPlotContextMenu(MainViewModel& vm, int plotIndex);
    void renderAddSignalMenu(MainViewModel& vm, int plotIndex);
    void renderRemoveSignalMenu(MainViewModel& vm, int plotIndex);

    // ── Data-driven auto-fit (scans visible data, no ImPlot AutoFit flag) ────
    // allData=true  → scan entire buffer (used by Auto-Fit All)
    // allData=false → scan only current visible X range (used by per-plot fit)
    void computeAutoFitPlot(MainViewModel& vm, int plotIndex, bool allData = false);
    void computeAutoFitAll(MainViewModel& vm);

    // ── Undo stack ────────────────────────────────────────────────────────────
    void pushSnapshot(int plotCount);
    void applyUndo();

    // ── Helpers ───────────────────────────────────────────────────────────────
    void ensurePlotYStates(int count);

    // X axis: shared linked range across all plots
    double xLinkMin_ = 0.0;
    double xLinkMax_ = 0.01;
    double lastTEnd_  = -1.0;

    // Y axis state, one entry per plot (grows lazily)
    std::vector<PlotYState> plotYStates_;

    // Zoom mode and drag state
    ZoomMode    zoomMode_      = ZoomMode::None;
    bool        dragActive_    = false;
    ImPlotPoint dragStartPlot_;   // plot coordinates at drag start
    int         dragPlotIdx_   = -1;

    // Undo history (capped at 64 entries)
    std::vector<ZoomSnapshot> zoomHistory_;

    // Zoom action deferred until after all EndPlot() calls
    PendingZoom pendingZoom_;

    // Per-frame decimation workspace — reused across signals each frame
    std::vector<double> decX_, decY_;
};
