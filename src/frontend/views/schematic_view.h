#pragma once
#include "views/base_view.h"
#include "view_model/schematic_model.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <utility>
#include <deque>
#include <algorithm>

class SchematicView : public BaseView {
public:
    SchematicView();
    void render(MainViewModel& vm) override;
    void addScopeView(class ScopeView* sv) { scopeViews_.push_back(sv); }
    void removeScopeView(class ScopeView* sv) {
        scopeViews_.erase(std::remove(scopeViews_.begin(), scopeViews_.end(), sv), scopeViews_.end());
    }
    void performAutoSave(MainViewModel& vm);

    // Set when the user closes a doc tab. MainView::render consumes this so it
    // can tear down the doc's ScopeViews + sim state in lockstep before calling
    // closeSchDoc on the model.
    int  takePendingCloseDoc() { int v = pendingCloseDocIdx_; pendingCloseDocIdx_ = -1; return v; }

    // ── File-action API (callable from the main window menu) ──────────────────
    // Each opens a native dialog if needed, then performs the action on the
    // active schematic. Status messages surface on the schematic toolbar.
    void fileSave(MainViewModel& vm);
    void fileSaveAs(MainViewModel& vm);
    void fileLoad(MainViewModel& vm);
    void fileExportSvg(MainViewModel& vm);
    void fileCopyImg(MainViewModel& vm);
    float& svgExportScaleRef() { return svgExportScale_; }

private:
    // ── Canvas pan / zoom ───────────────────────────────────────────────────
    ImVec2 panOffset_{200.0f, 150.0f};  // canvas-space translation
    float  zoom_ = 1.0f;

    // ── Selection & movement ────────────────────────────────────────────────
    int    selectedCompId_ = -1;
    int    selectedWireId_ = -1;
    int    movingCompId_   = -1;
    ImVec2 moveStartCanvas_{0, 0};  // canvas pos when move began
    ImVec2 moveCompOrigPos_{0, 0};  // component's original position

    // ── Multi-select ─────────────────────────────────────────────────────────
    std::vector<int> multiSelectedIds_;                   // additional selected comp ids
    bool   selBoxActive_      = false;
    ImVec2 selBoxStartCanvas_ = {0, 0};
    std::vector<std::pair<int,ImVec2>> multiMoveOrigPos_; // compId → original pos for multi-move
    std::vector<std::pair<int,std::vector<ImVec2>>> moveWaypointOrig_; // wireId → original waypoints

    // ── Wire drawing ────────────────────────────────────────────────────────
    bool wiringActive_   = false;
    int  wireFromCompId_ = -1;
    int  wireFromPinIdx_ = -1;
    std::vector<ImVec2> wireWaypoints_;  // intermediate points accumulated during wiring

    // ── Panning (right-mouse) ────────────────────────────────────────────────
    bool panningActive_ = false;

    // ── Property editor buffers ─────────────────────────────────────────────
    int  propEditCompId_ = -1;
    char propNameBuf_[64]  = {};
    char propBufs_[8][64]  = {};    // up to 8 params per component

    // ── Save / Load status ───────────────────────────────────────────────────
    char  ioStatus_[64] = {};
    float ioStatusTimer_ = 0.f;

    // ── SVG export ───────────────────────────────────────────────────────────
    // Scale factor applied to the SVG document size (the viewBox stays in canvas
    // units so this is purely a "rendered size" multiplier; vector quality is
    // unaffected). 1.0 ≈ 96 DPI screen rendering, 2.0 ≈ 192 DPI, 3.0 ≈ 288 DPI.
    float svgExportScale_ = 2.0f;
    // Active doc's filePath, synced from MainViewModel each frame so existing
    // code (toolbar, status overlay, Ctrl+S handler) keeps working unchanged.
    std::string savedFilePath_;
    std::vector<class ScopeView*> scopeViews_;  // registered by MainView

    bool pendingAutoLoad_ = true;  // try to restore last session on first render
    int  pendingCloseDocIdx_ = -1; // doc index whose tab the user just closed

    // doSave/doLoad take an explicit doc index so multi-doc save/load is
    // unambiguous. -1 = active doc.
    // includeScopeState=false suppresses XSCOPE_N + per-scope blocks; kept as an
    // escape hatch for circuit-only exports.
    void doSave(const std::string& path, MainViewModel& vm, bool silent = false,
                int docIdx = -1, bool includeScopeState = true);
    void doLoad(const std::string& path, MainViewModel& vm);

    // ── Wire net name editing ────────────────────────────────────────────────
    int  editNetWireId_  = -1;
    char editNetNameBuf_[64] = {};

    // ── Probe mode ───────────────────────────────────────────────────────────
    enum ProbeMode { PM_None, PM_VProbe, PM_IProbe };
    ProbeMode probeMode_ = PM_None;

    // ── V-probe drag state ────────────────────────────────────────────────────
    bool    vProbeDragActive_ = false;
    int     vProbeNodeA_      = -1;
    ImVec2  vProbeCanvasA_    = {0, 0};
    int     vProbeNodeB_      = -1;
    ImVec2  vProbeCanvasB_    = {0, 0};

    // ── Undo / Redo ───────────────────────────────────────────────────────────
    static constexpr int kMaxUndo = 50;
    std::deque<SchematicModel> undoStack_;
    std::deque<SchematicModel> redoStack_;

    // ── Custom transformer wizard ─────────────────────────────────────────────
    bool   txNPending_    = false;
    ImVec2 txNPendingPos_ = {0,0};
    int    txNWindings_   = 2;
    char   txNTurns_[6][16] = {};
    char   txNGroupBuf_[32]  = {};

    // ── Coordinate helpers ───────────────────────────────────────────────────
    ImVec2 s2c(ImVec2 screenPt, ImVec2 origin) const;   // screen → canvas
    ImVec2 c2s(ImVec2 canvasPt, ImVec2 origin) const;   // canvas → screen
    static ImVec2 snapGrid(ImVec2 pos, float g = 20.0f);
    // Rotate a pin offset by the component's rotation (0/1/2/3 = 0/90/180/270° CW)
    static ImVec2 rotateOff(ImVec2 off, int rot);
    // Canvas position of pin pi on comp (rotation applied)
    static ImVec2 pinCanvasPos(const struct SchematicComp& comp, int pi);

    // ── Helpers ─────────────────────────────────────────────────────────────
    static const char* polaritySymbol(const std::string& pinLabel);
    static float distPointToSegment(ImVec2 pt, ImVec2 a, ImVec2 b);

    // ── Sub-renderers ────────────────────────────────────────────────────────
    void handleInput(MainViewModel& vm, bool hovered, ImVec2 origin);
    void drawGrid(ImDrawList* dl, ImVec2 origin, ImVec2 size) const;
    void drawWires(ImDrawList* dl, MainViewModel& vm, ImVec2 origin) const;
    void drawComponents(ImDrawList* dl, MainViewModel& vm, ImVec2 origin);
    void drawCompSymbol(ImDrawList* dl, const struct SchematicComp& comp,
                        const struct CompTypeDef& td, ImVec2 ctr, bool sel);
    void drawTxCoreSymbol(ImDrawList* dl, const struct SchematicComp& txCore,
                          const struct SchematicModel& sch, ImVec2 origin) const;
    void drawRubberBand(ImDrawList* dl, MainViewModel& vm, ImVec2 origin) const;
    void renderProperties(MainViewModel& vm);

    int  insertJunctionOnWire(struct SchematicModel& sch, int wireId, ImVec2 juncPos);
    static void drawDashedLine(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col,
                               float thick, float dashLen, float gapLen);
};
