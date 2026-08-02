#pragma once
#include "views/base_view.h"
#include "view_model/fft.h"   // fft::Spectrum (cached FFT line)
#include <vector>
#include <string>
#include <iosfwd>
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
    explicit ScopeView(int scopeIdx = 0);
    void render(MainViewModel& vm) override;

    int  scopeIndex() const { return scopeIdx_; }
    // Only updates the lookup index; title is fixed at construction so ImGui
    // dock state is not disturbed when scopes are re-indexed after removal.
    void setScopeIndex(int idx) { scopeIdx_ = idx; }

    // Center this scope in the viewport on its first render (used for newly created scopes).
    // If imgui.ini already has a saved position, the saved position is used instead.
    void setCenterOnFirstRender() { centerOnFirstRender_ = true; }

    // Scope layout persistence (called by SchematicView save/load).
    // sourceSchId tags every loaded MuxEntry with the SchematicDoc::id of the
    // owning sch, so ownership routing knows which sch a scope belongs to.
    void saveState(std::ostream& out, const MainViewModel& vm) const;
    void loadState(std::istream& in, MainViewModel& vm, int sourceSchId = -1);
    PlotYState getPlotYState(int idx) const {
        if (idx >= 0 && idx < (int)plotYStates_.size()) return plotYStates_[idx];
        return {-1.0, 1.0, false};
    }

    // Window-geometry overrides applied on the next render. Used when restoring
    // a scope from a .sch file: the saved Pos/Size must override imgui.ini.
    void setPendingWindowGeometry(const ImVec2& pos, const ImVec2& size) {
        pendingPos_ = pos; pendingSize_ = size; pendingGeoSet_ = true;
    }

private:
    int  scopeIdx_ = 0;          // which ScopeModel in MainViewModel this view tracks
    bool pendingXRestore_ = false;  // if true, apply pendingX* on next tEnd change
    double pendingXMin_   = 0.0;
    double pendingXMax_   = 0.01;
    // ── CSV export ───────────────────────────────────────────────────────────
    // Exports every visible signal of this scope (all plots) to a CSV file
    // chosen via the native save dialog. visibleOnly=true limits rows to the
    // current X-axis (time) range of the scope window.
    void exportCsv(MainViewModel& vm, bool visibleOnly);

    // ── Rendering ────────────────────────────────────────────────────────────
    void renderPlot(MainViewModel& vm, PlotArea& plot, int plotIndex, float plotHeight, bool isBottom);
    void renderPlotContextMenu(MainViewModel& vm, int plotIndex);
    void renderAddSignalMenu(MainViewModel& vm, int plotIndex);
    void renderRemoveSignalMenu(MainViewModel& vm, int plotIndex);

    // ── FFT / frequency-domain analysis ──────────────────────────────────────
    // Independent top-level window (one per scope) showing the magnitude
    // spectrum of the visible signals in the selected plot, computed from the
    // current visible time range (or all buffered data).
    void renderFftWindow(MainViewModel& vm);

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

    // Persistent cursor (set by single-click, cleared via context menu)
    bool   cursorActive_ = false;
    double cursorX_      = 0.0;

    // X-axis context menu (right-click on the X-axis strip): editable width of
    // the visible X range. Seeded with the current width when the menu opens.
    double xAxisCtxWidth_ = 0.0;

    bool   centerOnFirstRender_ = false;

    // ── FFT window state ───────────────────────────────────────────────────────
    bool   fftOpen_         = false;  // is the FFT window shown
    int    fftPlotIdx_      = 0;      // which plot's signals to analyse
    int    fftWindow_       = 0;      // fft::Window enum (0=Rect,1=Hann,2=Hamming,3=Blackman)
                                      // default Rectangular: with coherent sampling (f0 set,
                                      // whole periods) no window is needed and Rect gives
                                      // exact harmonic amplitudes
    bool   fftDb_           = false;  // magnitude in dB (20·log10) vs linear
    bool   fftLogX_         = false;  // log10 frequency axis
    bool   fftRemoveDc_     = true;   // subtract mean before windowing
    bool   fftVisibleRange_ = true;   // analyse visible time range vs all data
    double fftF0_           = 0.0;    // user-specified fundamental (Hz); 0 = auto
    // f0 defaults to the simulator's auto-detected fundamental until the user
    // edits it. fftF0UserSet_ latches on manual edit so the auto value stops
    // overriding; fftDetectedF0Applied_ tracks the last detected value pushed in
    // so a *new* detected fundamental (next run) refreshes the default.
    bool   fftF0UserSet_        = false;
    double fftDetectedF0Applied_ = 0.0;
    int    fftNSel_         = 0;      // index into the N (FFT size) option list; 0 = Auto
    double fftLastPeakFreq_ = 0.0;    // peak of the first signal last frame (for "f0 = peak")
    // f0 for which the default X range (first 5 harmonics, [0, 5·f0]) was last
    // applied. 0 = not applied yet; reset when the FFT window is (re)opened so
    // the default re-applies. A change of f0 (auto-detect or user edit) also
    // re-applies the default width.
    double fftAppliedXF0_   = 0.0;
    // FFT persistent cursor: snaps to harmonics (k·f0, incl. DC) when f0 is set,
    // else to the nearest FFT bin. Mirrors the scope's time-domain cursor.
    bool   fftCursorActive_ = false;
    double fftCursorFreq_   = 0.0;

    // ── FFT spectrum cache ─────────────────────────────────────────────────────
    // The magnitude spectrum is expensive (O(N log N) per signal), so it is only
    // recomputed when the inputs change: a control edit (plot/window/N/f0/dB/range)
    // recomputes immediately; live sample growth is throttled to ~10 Hz. This is
    // what keeps the FFT window at full frame rate when the data is static.
    struct FftLine {
        std::string         label;
        ImU32               color = 0;
        fft::Spectrum       spec;
        std::vector<double> plotMag;   // linear or dB, ready for ImPlot
    };
    std::vector<FftLine> fftLines_;
    size_t fftCtrlSig_        = 0;     // hash of control params at last compute
    size_t fftDataSig_        = 0;     // hash of source data at last compute
    double fftLastComputeTime_ = -1.0; // ImGui time of last recompute (throttle)

    // Pending window geometry override (set by setPendingWindowGeometry).
    bool   pendingGeoSet_ = false;
    ImVec2 pendingPos_    = {0, 0};
    ImVec2 pendingSize_   = {900, 450};
};
