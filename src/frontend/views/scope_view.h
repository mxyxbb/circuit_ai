#pragma once
#include "views/base_view.h"
#include <vector>
#include <imgui.h>    // ImVec2
#include <implot.h>   // ImPlotPoint

class MainViewModel;
class ScopeModel;
struct PlotArea;

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
    void renderPlot(MainViewModel& vm, PlotArea& plot, int plotIndex, float plotHeight, bool isBottom);
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
    // Insert/remove a plot while keeping plotYStates_ positionally in sync.
    void insertPlot(ScopeModel& scope, int insertAfterIdx);
    void removePlot(ScopeModel& scope, int index);

    // ── Smart axis formatting ──────────────────────────────────────────────────
    struct AxisFmtParams {
        double scaleFactor  = 1.0;    // divisor for scaled-sci mode (e.g. 1e-4)
        int    exponent     = 0;      // for annotation label (e.g. -4)
        int    decimals     = 4;      // digits after decimal point
        bool   useScaledSci = false;  // true = divide tick value by scaleFactor
        char   annotation[32] = {};   // "x 1e-4", empty when unused
    };
    static int  niceTickRound(int n);
    static AxisFmtParams computeAxisFmt(double rangeMin, double rangeMax, float plotWidthPx);
    static int  axisFormatterCallback(double value, char* buff, int size, void* user_data);

    // X axis: shared linked range across all plots
    double xLinkMin_ = 0.0;
    double xLinkMax_ = 0.01;
    double lastTEnd_  = -1.0;

    // Y axis state, one entry per plot (grows lazily)
    std::vector<PlotYState> plotYStates_;

    // Auto-zoom drag state (always active — no manual mode toggle needed)
    // Direction is auto-detected after kDragThresh pixels:
    //   vertical screen drag   (|dy| ≥ |dx|) → H-zoom (X / time axis)
    //   horizontal screen drag (|dy| < |dx|) → V-zoom (Y axis, dragged plot only)
    // Axis-area drags are not intercepted; ImPlot handles them as native pan.
    bool        autoDragActive_    = false;
    bool        autoDragDirLocked_ = false;
    bool        autoDragIsH_       = false;   // true = H-zoom, false = V-zoom
    ImVec2      autoDragStartScr_  = {};       // screen position at drag start
    ImPlotPoint autoDragStartPlot_ = {};       // plot coords at drag start
    int         autoDragPlotIdx_   = -1;

    // Undo history (capped at 64 entries)
    std::vector<ZoomSnapshot> zoomHistory_;

    // Zoom action deferred until after all EndPlot() calls
    PendingZoom pendingZoom_;

    // Set to true when insertPlot/removePlot is called during a render frame.
    // Prevents the end-of-renderPlot Y-limits tracking from overwriting the
    // correct state that insertPlot/removePlot already wrote (with forceSet=true).
    // Reset to false at the start of each render() call.
    bool plotStructureChanged_ = false;

    // Per-frame decimation workspace — reused across signals each frame
    std::vector<double> decX_, decY_;
};
