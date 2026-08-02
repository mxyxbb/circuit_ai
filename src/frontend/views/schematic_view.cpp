#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")
#endif

#include "views/schematic_view.h"
#include "views/scope_view.h"
#include "views/schematic_svg_export.h"
#ifdef _WIN32
#include "views/schematic_clipboard_win32.h"
#endif
#include "view_model/main_view_model.h"
#include "view_model/scope_model.h"
#include "view_model/schematic_model.h"
#include "platform/file_dialog.h"
#include "common/expr_eval.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <cmath>
#include <cctype>
#include <cfloat>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <initializer_list>
#include <unordered_map>
#include <unordered_set>
#include <fstream>
#include <sstream>

// ── Win32 file dialog helpers ──────────────────────────────────────────────
// OFN_NOCHANGEDIR: prevents the dialog from mutating the process CWD when the
// user navigates to another folder. Without this, autosave/session files end
// up wherever the user last browsed instead of beside the exe.
#ifdef _WIN32

static bool pickOpenPath(char* buf, int n) {
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "CircuitAI Schematic\0*.sch\0All Files\0*.*\0\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = static_cast<DWORD>(n);
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = "sch";
    buf[0] = '\0';
    return GetOpenFileNameA(&ofn) != 0;
}
static bool pickSavePath(char* buf, int n) {
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "CircuitAI Schematic\0*.sch\0All Files\0*.*\0\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = static_cast<DWORD>(n);
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = "sch";
    buf[0] = '\0';
    return GetSaveFileNameA(&ofn) != 0;
}
static bool pickSvgSavePath(char* buf, int n) {
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "SVG (Inkscape, PowerPoint)\0*.svg\0All Files\0*.*\0\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = static_cast<DWORD>(n);
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    ofn.lpstrDefExt = "svg";
    buf[0] = '\0';
    return GetSaveFileNameA(&ofn) != 0;
}
#endif

SchematicView::SchematicView() : BaseView("Schematic") {}

// ── Polarity helper ─────────────────────────────────────────────────────────

const char* SchematicView::polaritySymbol(const std::string& pinLabel) {
    if (pinLabel == "P" || pinLabel == "A") return "+";
    return nullptr;  // only show "+" on positive pin
}

float SchematicView::distPointToSegment(ImVec2 pt, ImVec2 a, ImVec2 b) {
    ImVec2 ab = {b.x - a.x, b.y - a.y};
    ImVec2 ap = {pt.x - a.x, pt.y - a.y};
    float ab2 = ab.x*ab.x + ab.y*ab.y;
    if (ab2 < 1e-8f) return sqrtf(ap.x*ap.x + ap.y*ap.y);
    float t = (ap.x*ab.x + ap.y*ab.y) / ab2;
    t = std::max(0.0f, std::min(1.0f, t));
    ImVec2 closest = {a.x + t*ab.x, a.y + t*ab.y};
    ImVec2 diff = {pt.x - closest.x, pt.y - closest.y};
    return sqrtf(diff.x*diff.x + diff.y*diff.y);
}

// Point-in-convex-polygon OR within m of its boundary. Winding-agnostic; used by
// the outline-following component hit test. Distance-to-edge is inlined so this
// stays a free function (no access to private static helpers needed).
static bool pointInConvexOrNear(ImVec2 p, const ImVec2* v, int n, float m) {
    for (int i = 0; i < n; ++i) {
        ImVec2 a = v[i], b = v[(i + 1) % n];
        ImVec2 ab = {b.x - a.x, b.y - a.y}, ap = {p.x - a.x, p.y - a.y};
        float ab2 = ab.x*ab.x + ab.y*ab.y;
        float t = ab2 < 1e-8f ? 0.f : (ap.x*ab.x + ap.y*ab.y) / ab2;
        t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
        float dx = p.x - (a.x + t*ab.x), dy = p.y - (a.y + t*ab.y);
        if (sqrtf(dx*dx + dy*dy) <= m) return true;   // within the margin band
    }
    bool pos = false, neg = false;                     // inside test (convex)
    for (int i = 0; i < n; ++i) {
        ImVec2 a = v[i], b = v[(i + 1) % n];
        float cross = (b.x - a.x)*(p.y - a.y) - (b.y - a.y)*(p.x - a.x);
        if (cross > 0.f) pos = true; else if (cross < 0.f) neg = true;
    }
    return !(pos && neg);
}

// Fine-grained component hit test — see header. Symbol geometry is expressed in
// the component's LOCAL frame (the same ox,oy fed to sc() in drawCompSymbol), so
// we first map pt back through the component's translate → rotation → mirror.
bool SchematicView::hitTestCompBody(const SchematicComp& comp, ImVec2 pt, float m) {
    const CompTypeDef* td = SchematicModel::findCompType(comp.typeId);
    if (!td) return false;

    ImVec2 d = { pt.x - comp.pos.x, pt.y - comp.pos.y };
    ImVec2 r = rotateOff(d, (4 - (comp.rotation % 4)) % 4);   // inverse rotation
    ImVec2 L = { comp.mirrorX ? -r.x : r.x, r.y };            // undo mirror

    auto seg = [&](float ax, float ay, float bx, float by) {
        return distPointToSegment(L, {ax, ay}, {bx, by}) <= m;
    };
    auto circ = [&](float cx, float cy, float rad) {
        float dx = L.x - cx, dy = L.y - cy;
        return sqrtf(dx*dx + dy*dy) <= rad + m;
    };
    auto poly = [&](std::initializer_list<ImVec2> pts) {
        return pointInConvexOrNear(L, pts.begin(), (int)pts.size(), m);
    };

    const std::string& id = comp.typeId;
    if (id == "R")
        return poly({{-14.f,-7.f},{14.f,-7.f},{14.f,7.f},{-14.f,7.f}})
            || seg(-40.f,0.f,-14.f,0.f) || seg(14.f,0.f,40.f,0.f);
    if (id == "C")
        return poly({{-5.f,-12.f},{5.f,-12.f},{5.f,12.f},{-5.f,12.f}})
            || seg(-20.f,0.f,-5.f,0.f) || seg(5.f,0.f,20.f,0.f);
    if (id == "L")
        return poly({{-24.f,-7.f},{24.f,-7.f},{24.f,2.f},{-24.f,2.f}})
            || seg(-40.f,0.f,-24.f,0.f) || seg(24.f,0.f,40.f,0.f);
    if (id == "V_DC" || id == "V_SIN" || id == "V_SQUARE" || id == "V_STEP" || id == "I")
        return circ(0.f,0.f,18.f) || seg(-40.f,0.f,-18.f,0.f) || seg(18.f,0.f,40.f,0.f);
    if (id == "VCVS" || id == "VCCS")
        return poly({{0.f,-16.f},{16.f,0.f},{0.f,16.f},{-16.f,0.f}})
            || seg(0.f,-40.f,0.f,-16.f) || seg(0.f,16.f,0.f,40.f) || seg(-40.f,0.f,-26.f,0.f);
    if (id == "OPAMP" || id == "CMP")
        return poly({{-24.f,-24.f},{24.f,0.f},{-24.f,24.f}})
            || seg(-40.f,-20.f,-24.f,-20.f) || seg(-40.f,20.f,-24.f,20.f) || seg(24.f,0.f,40.f,0.f);
    if (id == "D")
        return poly({{-12.f,-10.f},{12.f,0.f},{-12.f,10.f}})
            || seg(-40.f,0.f,-12.f,0.f) || seg(12.f,0.f,40.f,0.f) || seg(12.f,-12.f,12.f,12.f);
    if (id == "JUNC")
        return circ(0.f,0.f,5.f);
    if (id == "NETLABEL")
        return poly({{-20.f,0.f},{-16.f,-7.f},{-4.f,-7.f},{-4.f,7.f},{-16.f,7.f}});
    if (id == "GND")   // asymmetric: stem 0..18 along local +y, ±6 across
        return L.y >= -2.f - m && L.y <= 18.f + m && std::fabs(L.x) <= 6.f + m;
    if (id == "S")     // MOSFET: gate group, channel, D/S rails, leads, body arrow
        return seg(-20.f,0.f,-5.f,0.f) || seg(-20.f,20.f,-5.f,20.f) || seg(-5.f,-15.f,-5.f,20.f)
            || seg(0.f,-20.f,0.f,-10.f)|| seg(0.f,-5.f,0.f,5.f)     || seg(0.f,10.f,0.f,20.f)
            || seg(0.f,-15.f,20.f,-15.f)|| seg(0.f,0.f,20.f,0.f)    || seg(0.f,15.f,20.f,15.f)
            || seg(20.f,-15.f,20.f,-25.f)|| seg(20.f,0.f,20.f,25.f)
            || seg(20.f,-20.f,33.f,-20.f)|| seg(20.f,20.f,33.f,20.f)
            || seg(33.f,-20.f,33.f,-5.f)|| seg(33.f,5.f,33.f,20.f)
            || seg(20.f,-40.f,20.f,-25.f)|| seg(20.f,25.f,20.f,40.f)
            || poly({{0.f,0.f},{10.f,-5.f},{10.f,5.f}});
    if (id == "TX")
        return poly({{-28.f,-18.f},{-16.f,-18.f},{-16.f,18.f},{-28.f,18.f}})
            || poly({{16.f,-18.f},{28.f,-18.f},{28.f,18.f},{16.f,18.f}})
            || seg(-5.f,-18.f,-5.f,18.f) || seg(5.f,-18.f,5.f,18.f)
            || seg(-40.f,-20.f,-22.f,-18.f) || seg(-40.f,20.f,-22.f,18.f)
            || seg(40.f,-20.f,22.f,-18.f)   || seg(40.f,20.f,22.f,18.f);
    if (id == "TX3")
        return poly({{-28.f,-18.f},{-16.f,-18.f},{-16.f,18.f},{-28.f,18.f}})
            || poly({{16.f,-30.f},{28.f,-30.f},{28.f,30.f},{16.f,30.f}})
            || seg(-5.f,-30.f,-5.f,30.f) || seg(5.f,-30.f,5.f,30.f)
            || seg(-40.f,-20.f,-22.f,-18.f) || seg(-40.f,20.f,-22.f,18.f)
            || seg(40.f,-30.f,22.f,-30.f) || seg(40.f,-10.f,22.f,-10.f)
            || seg(40.f,10.f,22.f,10.f)   || seg(40.f,30.f,22.f,30.f);
    if (id == "TX_WIND")
        return poly({{-6.f,-24.f},{6.f,-24.f},{6.f,24.f},{-6.f,24.f}})
            || seg(0.f,-40.f,0.f,-24.f) || seg(0.f,24.f,0.f,40.f);

    // Any unlisted type (TX_CORE, TXN_CUSTOM, …): tighter local-frame AABB.
    return std::fabs(L.x) <= td->bodyHalfSize.x + m &&
           std::fabs(L.y) <= td->bodyHalfSize.y + m;
}

// Auto corner: when a wire segment from → to is diagonal, insert the Manhattan
// corner point (horizontal-first, matching the rubber-band preview) so routed
// wires always stay orthogonal.
static void appendManhattanCorner(std::vector<ImVec2>& wps, ImVec2 from, ImVec2 to) {
    if (std::fabs(from.x - to.x) > 0.5f && std::fabs(from.y - to.y) > 0.5f)
        wps.push_back({to.x, from.y});
}

// ── Coordinate transforms ──────────────────────────────────────────────────

ImVec2 SchematicView::s2c(ImVec2 s, ImVec2 o) const {
    return { (s.x - o.x) / zoom_ - panOffset_.x,
             (s.y - o.y) / zoom_ - panOffset_.y };
}

ImVec2 SchematicView::c2s(ImVec2 c, ImVec2 o) const {
    return { (c.x + panOffset_.x) * zoom_ + o.x,
             (c.y + panOffset_.y) * zoom_ + o.y };
}

ImVec2 SchematicView::snapGrid(ImVec2 pos, float g) {
    return { roundf(pos.x / g) * g, roundf(pos.y / g) * g };
}

// Rotate a 2-D offset by rot quarter-turns clockwise (screen Y-down convention)
ImVec2 SchematicView::rotateOff(ImVec2 off, int rot) {
    float x = off.x, y = off.y;
    switch (((rot % 4) + 4) % 4) {
        case 1: return { -y,  x };   // 90° CW  (screen Y-down: top→right)
        case 2: return { -x, -y };   // 180°
        case 3: return {  y, -x };   // 270° CW
        default:return {  x,  y };   // 0°
    }
}

// Canvas position of pin pi on comp, accounting for mirrorX and rotation
ImVec2 SchematicView::pinCanvasPos(const SchematicComp& comp, int pi) {
    const CompTypeDef* td = SchematicModel::findCompType(comp.typeId);
    if (!td || pi >= (int)td->pins.size()) return comp.pos;
    float ox = td->pins[pi].offset.x;
    float oy = td->pins[pi].offset.y;
    if (comp.mirrorX) ox = -ox;
    ImVec2 roff = rotateOff({ox, oy}, comp.rotation);
    return { comp.pos.x + roff.x, comp.pos.y + roff.y };
}

// ── Save / Load with scope state ──────────────────────────────────────────
// Persistence file names — resolved via platform::appDataPath() so they always
// land beside the exe regardless of CWD changes from native file dialogs.
static const char* kSessionFileName = "session.txt";
static const char* kAutoSaveName    = "autosave.sch";  // legacy single-file fallback
static std::string kSessionFile() { return platform::appDataPath(kSessionFileName); }

// Helper: derive a tab label from a file path (basename without extension).
static std::string deriveDisplayName(const std::string& path) {
    if (path.empty()) return "Untitled";
    std::string name = path;
    auto pos = name.find_last_of("/\\");
    if (pos != std::string::npos) name = name.substr(pos + 1);
    return name;
}

void SchematicView::performAutoSave(MainViewModel& vm) {
    // Per user request: do NOT auto-save .sch contents. Only the user's explicit
    // Save / Save As writes a .sch file. We still record session.txt so that
    // previously-saved tabs can be restored on next launch — untitled docs are
    // discarded on close.
    std::ofstream sf(kSessionFile());
    if (!sf.good()) return;

    // Map original doc indices to session positions so ACTIVE= still points
    // at the right tab after we filter out untitled docs.
    int activeOrig = vm.activeSchIdx();
    int activeSession = -1;
    int sessionPos = 0;
    std::vector<std::string> savedPaths;
    for (int i = 0; i < vm.schDocCount(); i++) {
        const SchematicDoc& doc = vm.schDoc(i);
        if (doc.filePath.empty()) continue;
        if (i == activeOrig) activeSession = sessionPos;
        savedPaths.push_back(doc.filePath);
        sessionPos++;
    }
    if (activeSession < 0) activeSession = 0;
    sf << "ACTIVE=" << activeSession << '\n';
    for (const auto& p : savedPaths) sf << "PATH=" << p << '\n';
}

void SchematicView::doSave(const std::string& path, MainViewModel& vm, bool silent,
                            int docIdx, bool includeScopeState) {
    if (docIdx < 0) docIdx = vm.activeSchIdx();
    if (docIdx < 0 || docIdx >= vm.schDocCount()) return;
    SchematicDoc& doc = vm.schDoc(docIdx);
    SchematicModel& sch = doc.model;
    bool ok = sch.saveToFile(path);
    if (!ok) {
        if (!silent) {
            std::snprintf(ioStatus_, sizeof(ioStatus_), "Save failed!");
            ioStatusTimer_ = 2.5f;
        }
        return;
    }
    if (!includeScopeState) {
        if (!silent) {
            doc.filePath    = path;
            doc.displayName = deriveDisplayName(path);
            if (docIdx == vm.activeSchIdx()) savedFilePath_ = path;
            std::snprintf(ioStatus_, sizeof(ioStatus_), "Saved.");
            ioStatusTimer_ = 2.5f;
        }
        return;
    }
    // Append scope layout — only scopes whose computed owner == this doc's id.
    // Scopes with mixed sources (owner = -1) are NOT saved with any sch.
    {
        std::ofstream f(path, std::ios::app);
        std::ostringstream ss;

        // Collect scope views whose model is owned by this doc.
        std::vector<ScopeView*> ownedViews;
        for (ScopeView* sv : scopeViews_) {
            const ScopeModel& scope = vm.scope(sv->scopeIndex());
            if (scope.computeOwnerSchId() == doc.id)
                ownedViews.push_back(sv);
        }

        ss << "XSCOPE_N " << (int)ownedViews.size() << '\n';
        for (ScopeView* sv : ownedViews) {
            int        idx   = sv->scopeIndex();
            ScopeModel& scope = vm.scope(idx);
            sv->saveState(ss, vm);  // writes XSCOPE/XSCOPEGEO/XCSIG lines
            for (int pi = 0; pi < scope.plotCount(); pi++) {
                const PlotArea* plot = scope.getPlot(pi);
                if (!plot) continue;
                ss << "PLOT " << plot->title << '\n';
                for (const auto& entry : plot->entries) {
                    char buf[512];
                    std::snprintf(buf, sizeof(buf), "XSIG %s|%s|%g|%s|%08X\n",
                        entry->signalName.c_str(), entry->label.c_str(),
                        entry->scale, entry->signalNameB.c_str(),
                        (unsigned int)entry->color);
                    ss << buf;
                }
                PlotYState pys = sv->getPlotYState(pi);
                ss << "YRANGE " << pys.yMin << ' ' << pys.yMax << '\n';
            }
            ss << "ENDSCOPE\n";
        }
        f << ss.str();
    }
    if (!silent) {
        doc.filePath    = path;
        doc.displayName = deriveDisplayName(path);
        if (docIdx == vm.activeSchIdx()) savedFilePath_ = path;
        std::snprintf(ioStatus_, sizeof(ioStatus_), "Saved.");
        ioStatusTimer_ = 2.5f;
    }
}

void SchematicView::doLoad(const std::string& path, MainViewModel& vm) {
    // If the path is already open, just switch to that tab.
    int existing = vm.findSchDocByPath(path);
    if (existing >= 0) {
        vm.setActiveSchIdx(existing);
        savedFilePath_ = path;
        std::snprintf(ioStatus_, sizeof(ioStatus_), "Already open.");
        ioStatusTimer_ = 2.5f;
        return;
    }

    // If the active doc is empty (untitled, no components), load INTO it.
    // Otherwise create a fresh doc.
    int targetIdx = vm.activeSchIdx();
    if (targetIdx < 0 || vm.schDoc(targetIdx).model.comps().size() > 0
        || !vm.schDoc(targetIdx).filePath.empty()) {
        targetIdx = vm.newSchDoc();
    }
    SchematicDoc& doc = vm.schDoc(targetIdx);
    SchematicModel& sch = doc.model;
    bool ok = sch.loadFromFile(path);
    if (ok) {
        selectedCompId_ = selectedWireId_ = propEditCompId_ = movingCompId_ = -1;
        multiSelectedIds_.clear(); multiMoveOrigPos_.clear(); selBoxActive_ = false;
        wiringActive_ = false;
        doc.undoStack.clear(); doc.redoStack.clear();
        undoStack_.clear(); redoStack_.clear();
        doc.filePath    = path;
        doc.displayName = deriveDisplayName(path);

        {
            std::ifstream f(path);
            std::string line;
            int scopeIdx = -1;       // local index within this sch's saved scopes
            // Reuse trailing empty scopes (e.g. the placeholder ScopeModel created
            // in MainViewModel's constructor) so a restored scope occupies their
            // slots instead of being appended after them. Without this, an empty
            // scope 0 lingers and gets auto-populated with all signals on Build.
            int firstNewScope = vm.scopeCount();
            while (firstNewScope > 0) {
                const ScopeModel& sc = vm.scope(firstNewScope - 1);
                bool empty = true;
                for (int pi = 0; pi < sc.plotCount(); pi++) {
                    const PlotArea* p = sc.getPlot(pi);
                    if (p && !p->entries.empty()) { empty = false; break; }
                }
                if (!empty) break;
                firstNewScope--;
            }
            bool inBlock = false;
            std::ostringstream block;

            while (std::getline(f, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();

                // XSCOPE_N: number of scopes saved with this sch
                if (line.size() > 8 && line.substr(0, 8) == "XSCOPE_N") {
                    int n = 0;
                    std::istringstream ss(line.substr(8));
                    ss >> n;
                    // Add n new scopes (each will be tagged with this doc's id)
                    while (vm.scopeCount() < firstNewScope + n) vm.addScope();
                    continue;
                }

                // XSCOPE <xMin> <xMax>: start of a scope block
                if (line.size() > 7 && line.substr(0, 7) == "XSCOPE ") {
                    scopeIdx++;
                    int absIdx = firstNewScope + scopeIdx;
                    while (vm.scopeCount() <= absIdx) vm.addScope();
                    inBlock = true;
                    block.str(""); block.clear();
                    block << line.substr(7) << '\n';
                    continue;
                }

                if (inBlock) {
                    block << line << '\n';
                    if (line == "ENDSCOPE") {
                        inBlock = false;
                        int absIdx = firstNewScope + scopeIdx;
                        // Defer to the scope view if it exists; else stash on the model
                        // so the soon-to-be-created ScopeView can apply it.
                        if (absIdx < (int)scopeViews_.size()) {
                            std::istringstream bss(block.str());
                            scopeViews_[absIdx]->loadState(bss, vm, doc.id);
                            // Loaded scopes are restored visible regardless of the
                            // ScopeView's hidden default.
                            scopeViews_[absIdx]->setVisible(true);
                        } else {
                            vm.scope(absIdx).setPendingLoadBlock(block.str(), doc.id);
                            // Auto-sync in MainView::render creates the missing
                            // ScopeView with visible=true so the pending block
                            // gets consumed on its first render.
                        }
                    }
                }
            }
        }
        vm.setActiveSchIdx(targetIdx);
        savedFilePath_ = path;
    }
    std::snprintf(ioStatus_, sizeof(ioStatus_), ok ? "Loaded." : "Load failed!");
    ioStatusTimer_ = 2.5f;
}

// ── Undo helper ────────────────────────────────────────────────────────────

static void pushUndo(std::deque<SchematicModel>& undoStack,
                     std::deque<SchematicModel>& redoStack,
                     const SchematicModel& current,
                     int maxUndo)
{
    redoStack.clear();
    undoStack.push_back(current);
    while ((int)undoStack.size() > maxUndo)
        undoStack.pop_front();
}

// ── Welcome / empty-state screen ─────────────────────────────────────────
// Shown when every schematic tab has been closed. No doc exists yet, so the
// file counter isn't advanced until the user commits to a new schematic by
// dropping a component or clicking "New Schematic".

void SchematicView::createDocWithComp(MainViewModel& vm, const std::string& typeId,
                                      ImVec2 canvasPos) {
    // Leaving the welcome state: materialize the doc the user is populating.
    vm.newSchDoc();
    // Fresh canvas — clear any stale shared editor state.
    selectedCompId_ = selectedWireId_ = propEditCompId_ = movingCompId_ = -1;
    multiSelectedIds_.clear(); multiMoveOrigPos_.clear();
    wiringActive_ = false;
    undoStack_.clear(); redoStack_.clear();

    SchematicModel& sch = vm.schematic();
    if (typeId == "TXN_CUSTOM") {
        // Custom transformer wizard (matches the canvas drop path).
        txNPending_    = true;
        txNPendingPos_ = canvasPos;
        txNWindings_   = 2;
        std::snprintf(txNGroupBuf_, sizeof(txNGroupBuf_), "TX%d",
                      (int)sch.comps().size() + 1);
        for (int i = 0; i < 6; ++i) std::snprintf(txNTurns_[i], 16, "%d", i == 0 ? 10 : 1);
    } else if (!typeId.empty()) {
        sch.addComp(typeId, canvasPos);
    }
}

void SchematicView::renderWelcome(MainViewModel& vm) {
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x < 1.0f) avail.x = 1.0f;
    if (avail.y < 1.0f) avail.y = 1.0f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(18, 22, 28, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild("##sch_welcome", avail, false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImVec2 origin = ImGui::GetCursorScreenPos();

    // Whole-area invisible button doubles as the DnD drop target: dropping a
    // palette component here creates the first schematic and places it.
    // AllowOverlap so the centered "New Schematic" button drawn on top of it
    // still receives hover/click (otherwise this button claims HoveredId first).
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##welcome_ib", avail);
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("COMP_TYPE")) {
            std::string typeId(static_cast<const char*>(pl->Data));
            ImVec2 dropCanvas = snapGrid(s2c(ImGui::GetMousePos(), origin));
            createDocWithComp(vm, typeId, dropCanvas);
        }
        ImGui::EndDragDropTarget();
    }

    // ── Centered intro block (drawn on top of the invisible button) ──────────
    auto centerLine = [&](const char* txt, ImU32 col, float scale) {
        ImGui::SetWindowFontScale(scale);
        float w = ImGui::CalcTextSize(txt).x;
        ImGui::SetCursorPosX((avail.x - w) * 0.5f);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(txt);
        ImGui::PopStyleColor();
        ImGui::SetWindowFontScale(1.0f);
    };

    ImGui::SetCursorPos({0.0f, avail.y * 0.26f});
    centerLine("CircuitAI", IM_COL32(120, 175, 255, 255), 2.0f);
    ImGui::Dummy({0, 6});
    centerLine("Circuit Simulator", IM_COL32(140, 140, 140, 255), 1.0f);
    ImGui::Dummy({0, 26});
    centerLine("No schematic open.", IM_COL32(205, 205, 205, 255), 1.15f);
    ImGui::Dummy({0, 14});
    centerLine("Drag a component from the Palette onto this canvas to start a new schematic.",
               IM_COL32(150, 150, 150, 255), 1.0f);
    ImGui::Dummy({0, 2});
    centerLine("Or open an existing .sch file from the File menu.",
               IM_COL32(150, 150, 150, 255), 1.0f);
    ImGui::Dummy({0, 22});

    const float btnW = 200.0f;
    ImGui::SetCursorPosX((avail.x - btnW) * 0.5f);
    if (ImGui::Button("+ New Schematic", {btnW, 0.0f})) {
        createDocWithComp(vm, "", {0.0f, 0.0f});  // empty doc, no component
    }

    ImGui::EndChild();
}

// ── Main render ────────────────────────────────────────────────────────────

void SchematicView::render(MainViewModel& vm) {
    // ── Auto-restore last session (runs once on first render) ─────────────
    if (pendingAutoLoad_) {
        pendingAutoLoad_ = false;
        std::ifstream sf(kSessionFile());
        if (sf.good()) {
            std::vector<std::string> paths;
            int activeIdx = 0;
            std::string line;
            while (std::getline(sf, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.rfind("ACTIVE=", 0) == 0) {
                    try { activeIdx = std::stoi(line.substr(7)); } catch (...) {}
                } else if (line.rfind("PATH=", 0) == 0) {
                    paths.push_back(line.substr(5));
                } else if (!line.empty() && line.find('=') == std::string::npos) {
                    // Backward-compat: legacy single-path session.txt format
                    paths.push_back(line);
                }
            }
            // Load each path as its own tab; track auto-save fallbacks specially
            // (so Ctrl+S re-prompts for a real path on restore).
            for (const auto& p : paths) {
                if (p.empty()) continue;
                if (!std::ifstream(p).good()) continue;
                doLoad(p, vm);
                // Detect auto-save fallback files (legacy or per-doc) by basename
                std::string base = p;
                {
                    auto pos = base.find_last_of("/\\");
                    if (pos != std::string::npos) base = base.substr(pos + 1);
                }
                bool isAutoSave = (base == kAutoSaveName) || base.rfind("autosave_", 0) == 0;
                if (isAutoSave) {
                    SchematicDoc& d = vm.activeSchDoc();
                    d.filePath.clear();
                    d.displayName = "Untitled-restored";
                }
            }
            if (activeIdx >= 0 && activeIdx < vm.schDocCount())
                vm.setActiveSchIdx(activeIdx);
        }
    }

    if (!visible_) return;
    if (!ImGui::Begin(title_.c_str(), &visible_)) {
        ImGui::End();
        return;
    }
    // (File operations now live in the main-window menu bar; see MainView.)

    // ── Welcome / empty state ─────────────────────────────────────────────
    // When every tab has been closed there is no active doc. Show a short intro
    // instead of a blank grid, and only materialize a new doc once the user
    // actually drops a component or clicks "New Schematic".
    if (vm.schDocCount() == 0) {
        renderWelcome(vm);
        ImGui::End();
        return;
    }

    // ── Multi-doc tab bar ─────────────────────────────────────────────────
    {
        ImGuiTabBarFlags tbFlags =
            ImGuiTabBarFlags_Reorderable |
            ImGuiTabBarFlags_AutoSelectNewTabs |
            ImGuiTabBarFlags_FittingPolicyScroll;
        if (ImGui::BeginTabBar("##sch_doctabs", tbFlags)) {
            int closeRequest = -1;
            int prevActive   = vm.activeSchIdx();
            for (int i = 0; i < vm.schDocCount(); i++) {
                SchematicDoc& doc = vm.schDoc(i);
                bool open = true;
                std::string label = doc.displayName + "##doc" + std::to_string(doc.id);
                if (ImGui::BeginTabItem(label.c_str(), &open, ImGuiTabItemFlags_None)) {
                    if (vm.activeSchIdx() != i) vm.setActiveSchIdx(i);
                    ImGui::EndTabItem();
                }
                if (!open) closeRequest = i;
            }
            // "+" trailing button = new schematic; tooltip on hover
            if (ImGui::TabItemButton("+##new_sch_tab",
                ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) {
                int newIdx = vm.newSchDoc();
                vm.setActiveSchIdx(newIdx);
                // Reset shared canvas state for the new doc
                selectedCompId_ = selectedWireId_ = propEditCompId_ = movingCompId_ = -1;
                multiSelectedIds_.clear(); multiMoveOrigPos_.clear();
                wiringActive_ = false;
                undoStack_.clear(); redoStack_.clear();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("New schematic");
            ImGui::EndTabBar();

            // Process tab-close request after the tab bar (closing during iteration
            // would invalidate the loop's index assumptions). We DON'T auto-save
            // and we DON'T call closeSchDoc here — MainView consumes the deferred
            // request after this frame so it can tear down the doc's ScopeViews,
            // stop the simulator, and remove the scope models in lockstep.
            if (closeRequest >= 0) {
                pendingCloseDocIdx_ = closeRequest;
                // Reset shared canvas state to avoid stale selection on the new active doc.
                selectedCompId_ = selectedWireId_ = propEditCompId_ = movingCompId_ = -1;
                multiSelectedIds_.clear(); multiMoveOrigPos_.clear();
                wiringActive_ = false;
                undoStack_.clear(); redoStack_.clear();
            }
            // Reset selection when the user switches between existing tabs.
            if (vm.activeSchIdx() != prevActive) {
                selectedCompId_ = selectedWireId_ = propEditCompId_ = movingCompId_ = -1;
                multiSelectedIds_.clear(); multiMoveOrigPos_.clear();
                wiringActive_ = false;
                undoStack_.clear(); redoStack_.clear();
            }
        }
    }

    // Sync local savedFilePath_ with the active doc so existing toolbar/Ctrl+S code works.
    savedFilePath_ = vm.activeSchDoc().filePath;

    SchematicModel& sch = vm.schematic();

    // ── Toolbar ────────────────────────────────────────────────────────────
    if (ioStatusTimer_ > 0.f) ioStatusTimer_ -= ImGui::GetIO().DeltaTime;

    {
        ImVec2 btnSz = ImGui::CalcTextSize("Build & Run");
        btnSz.x += ImGui::GetStyle().FramePadding.x * 2.0f;
        btnSz.y += ImGui::GetStyle().FramePadding.y * 2.0f;
        bool simActive = (vm.isSimRunning() && !vm.isSimPaused()) || vm.isBuildPending();
        if (simActive) {
            ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.7f, 0.15f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.9f, 0.25f, 0.25f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.5f, 0.10f, 0.10f, 1.0f));
            if (ImGui::Button("Stop", btnSz)) {
                vm.cancelBuild();
                vm.stop();
            }
            ImGui::PopStyleColor(3);
        } else {
            if (ImGui::Button("Build & Run", btnSz)) vm.requestBuild();
        }
    }
    // I/O status (Save / Load / Export feedback) appears inline next to
    // Build & Run since the file actions themselves now live in the File menu.
    if (ioStatusTimer_ > 0.f) {
        ImGui::SameLine();
        bool fail = (std::strstr(ioStatus_, "failed") != nullptr);
        ImGui::TextColored(fail ? ImVec4(1.f,0.35f,0.35f,1.f)
                                : ImVec4(0.3f,1.f,0.45f,1.f), "%s", ioStatus_);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    ImGui::TextDisabled("dt:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    ImGui::InputText("##schdt",   sch.simCfg.dt,   sizeof(sch.simCfg.dt));
    ImGui::SameLine();
    ImGui::TextDisabled("t_end:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    ImGui::InputText("##schtend", sch.simCfg.tEnd, sizeof(sch.simCfg.tEnd));
    ImGui::SameLine();
    // ── POP (Periodic Operating Point) ──────────────────────────────────────
    // Keep only the last N fundamental periods after the run completes. The
    // fundamental is auto-detected from the largest-current switch's gate drive.
    ImGui::Checkbox("POP", &sch.simCfg.popEnabled);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Periodic Operating Point mode.\n"
            "After the run completes, discards everything except the last N\n"
            "fundamental-frequency periods before t_end. The fundamental is\n"
            "auto-detected from the gate-drive frequency of the switch carrying\n"
            "the largest current, and seeds the FFT window's f0.");
    if (sch.simCfg.popEnabled) {
        ImGui::SameLine();
        ImGui::TextDisabled("N:");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(50.0f);
        ImGui::InputText("##schpopn", sch.simCfg.popPeriods, sizeof(sch.simCfg.popPeriods),
                         ImGuiInputTextFlags_CharsDecimal);
    }
    ImGui::SameLine();
    {
        bool active = varsWindowOpen_;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f,0.5f,0.7f,1.0f));
        if (ImGui::SmallButton("Vars")) varsWindowOpen_ = !varsWindowOpen_;
        if (active) ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("User variables usable in any numeric field, e.g. fsw=1e6 then tdelay=0.5/fsw");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    // Probe buttons
    {
        bool vActive = (probeMode_ == PM_VProbe);
        bool iActive = (probeMode_ == PM_IProbe);
        if (vActive) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f,0.6f,0.2f,1.0f));
        if (ImGui::SmallButton("V-Probe")) {
            probeMode_ = vActive ? PM_None : PM_VProbe;
            if (vActive) { vProbeDragActive_ = false; vProbeNodeA_ = vProbeNodeB_ = -1; }
        }
        if (vActive) ImGui::PopStyleColor();
        ImGui::SameLine();
        if (iActive) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f,0.6f,0.2f,1.0f));
        if (ImGui::SmallButton("I-Probe")) probeMode_ = iActive ? PM_None : PM_IProbe;
        if (iActive) ImGui::PopStyleColor();
        vm.setProbeActive(probeMode_ != PM_None);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (wiringActive_) {
        ImGui::TextColored({0.3f,1.0f,0.5f,1.0f}, "[WIRING — click pin to finish / click canvas for waypoint / Esc cancel]");
    } else if (probeMode_ == PM_VProbe && vProbeDragActive_) {
        ImGui::TextColored({1.0f,0.85f,0.2f,1.0f}, "[V-PROBE — release on node B for differential V(A-B), or release on same node for V(A)]");
    } else if (probeMode_ == PM_VProbe) {
        ImGui::TextColored({0.3f,1.0f,0.3f,1.0f}, "[V-PROBE — click a wire/node, or press-drag to another node for differential voltage]");
    } else if (probeMode_ == PM_IProbe) {
        ImGui::TextColored({0.3f,1.0f,0.3f,1.0f}, "[I-PROBE — click a pin to add its current to selected Scope plot]");
    } else {
        ImGui::TextDisabled("LClick=sel/wire  R=rotate  X=mirror  Ctrl+C/V=copy/paste  Ctrl+drag sel=duplicate  Drag wire=reroute  RDrag=pan  Del=delete  LDrag=multisel  DblClick wire=net name");
    }
    ImGui::Separator();

    // ── Canvas area ────────────────────────────────────────────────────────
    float propH = (selectedCompId_ != -1) ? 80.0f : 0.0f;
    ImVec2 canvasSize = { ImGui::GetContentRegionAvail().x,
                          ImGui::GetContentRegionAvail().y - propH - 4.0f };
    if (canvasSize.y < 50.0f) canvasSize.y = 50.0f;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(18, 22, 28, 255));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::BeginChild("##sch_canvas", canvasSize, false,
                       ImGuiWindowFlags_NoScrollbar |
                       ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    ImGui::SetCursorPos({0.0f, 0.0f});
    ImVec2 origin = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton("##canvas_ib", canvasSize,
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonRight);
    bool canvasHovered = ImGui::IsItemHovered();

    // DnD drop target
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* pl = ImGui::AcceptDragDropPayload("COMP_TYPE")) {
            std::string typeId(static_cast<const char*>(pl->Data));
            ImVec2 dropCanvas = snapGrid(s2c(ImGui::GetMousePos(), origin));
            if (typeId == "TXN_CUSTOM") {
                txNPending_    = true;
                txNPendingPos_ = dropCanvas;
                txNWindings_   = 2;
                snprintf(txNGroupBuf_, sizeof(txNGroupBuf_), "TX%d",
                         (int)sch.comps().size() + 1);
                for (int i = 0; i < 6; ++i) snprintf(txNTurns_[i], 16, "%d", i==0?10:1);
            } else {
                pushUndo(undoStack_, redoStack_, sch, kMaxUndo);
                bool placed = false;
                if (typeId == "JUNC") {
                    // Dropping a junction onto an existing wire splits the wire
                    // and ties the junction in (a real electrical T-point)
                    // instead of leaving a free-floating dot that only looks
                    // connected.
                    ImVec2 snapPt;
                    int wid = hitTestWire(sch, dropCanvas, 10.0f, &snapPt);
                    if (wid >= 0 && insertJunctionOnWire(sch, wid, snapPt) >= 0)
                        placed = true;
                }
                if (!placed) sch.addComp(typeId, dropCanvas);
            }
        }
        ImGui::EndDragDropTarget();
    }

    handleInput(vm, canvasHovered, origin);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    drawGrid(dl, origin, canvasSize);
    drawWires(dl, vm, origin);
    drawComponents(dl, vm, origin);
    drawRubberBand(dl, vm, origin);

    // Alt+hover over a wire (not wiring): preview the junction tap point where
    // Alt+click would insert a junction and start a new wire.
    if (canvasHovered && !wiringActive_ && probeMode_ == PM_None
        && ImGui::GetIO().KeyAlt) {
        ImVec2 snapPt;
        int wid = hitTestWire(sch, s2c(ImGui::GetMousePos(), origin),
                              6.0f / zoom_, &snapPt);
        if (wid >= 0) {
            ImVec2 s = c2s(snapPt, origin);
            dl->AddCircle(s, 5.0f * zoom_, IM_COL32(80, 220, 120, 230), 0, 2.0f);
        }
    }

    // ── V-probe drag highlights ────────────────────────────────────────────
    if (vProbeDragActive_) {
        ImVec2 sA = c2s(vProbeCanvasA_, origin);
        dl->AddCircleFilled(sA, 8.0f * zoom_, IM_COL32(255, 220, 50, 220));
        dl->AddCircle(sA, 8.0f * zoom_, IM_COL32(255, 255, 120, 255), 0, 2.0f);
        if (vProbeNodeB_ != -1 && vProbeNodeB_ != vProbeNodeA_) {
            ImVec2 sB = c2s(vProbeCanvasB_, origin);
            dl->AddCircleFilled(sB, 8.0f * zoom_, IM_COL32(80, 255, 120, 220));
            dl->AddCircle(sB, 8.0f * zoom_, IM_COL32(150, 255, 180, 255), 0, 2.0f);
            dl->AddLine(sA, sB, IM_COL32(255, 220, 50, 120), 1.5f);
        }
    }

    // ── Schematic name + simulation status overlay ───────────────────────
    {
        // Filename at top-left
        std::string fnameStr = savedFilePath_.empty() ? "(unsaved)" : savedFilePath_;
        {
            auto pos = fnameStr.find_last_of("/\\");
            if (pos != std::string::npos) fnameStr = fnameStr.substr(pos + 1);
        }
        dl->AddText({origin.x + 6.0f, origin.y + 6.0f},
                    IM_COL32(100, 160, 255, 200), fnameStr.c_str());
        // Sim status on line below
        char simTxt[64] = "";
        double t = vm.currentTime();
        if (vm.isSimRunning())
            snprintf(simTxt, sizeof(simTxt), "Running  t = %.4g s", t);
        else if (vm.isSimPaused())
            snprintf(simTxt, sizeof(simTxt), "Paused  t = %.4g s", t);
        if (simTxt[0])
            dl->AddText({origin.x + 6.0f, origin.y + 20.0f},
                        IM_COL32(150, 150, 150, 200), simTxt);
    }

    ImGui::EndChild();

    renderProperties(vm);
    renderVariablesWindow(vm);

    // ── Wire net name edit popup ─────────────────────────────────────────
    if (editNetWireId_ >= 0) ImGui::OpenPopup("Net Name##wireDlg");
    if (ImGui::BeginPopupModal("Net Name##wireDlg", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Net name (empty = auto-number):");
        ImGui::SetNextItemWidth(200.f);
        bool commit = ImGui::InputText("##netname", editNetNameBuf_, sizeof(editNetNameBuf_),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        if (commit || ImGui::Button("OK")) {
            SchematicWire* ew = vm.schematic().findWire(editNetWireId_);
            if (ew) {
                pushUndo(undoStack_, redoStack_, vm.schematic(), kMaxUndo);
                // Rename the scope signal to match the new net name
                auto nodeMap = vm.schematic().computePinNodeMap();
                auto nit = nodeMap.find(SchematicModel::pinKey(ew->fromCompId, ew->fromPinIdx));
                if (nit != nodeMap.end() && nit->second != 0) {
                    int nodeId = nit->second;
                    // If wire has no name, a NETLABEL on same node is the effective name
                    std::string netLabelName;
                    for (const auto& c : vm.schematic().comps())
                        if (c.typeId == "NETLABEL" && !c.paramValues.empty()) {
                            auto cit = nodeMap.find(SchematicModel::pinKey(c.id, 0));
                            if (cit != nodeMap.end() && cit->second == nodeId)
                                { netLabelName = c.paramValues[0]; break; }
                        }
                    std::string oldSig = !ew->netName.empty()
                        ? ("V(" + ew->netName + ")")
                        : (!netLabelName.empty()
                            ? ("V(" + netLabelName + ")")
                            : ("V(" + std::to_string(nodeId) + ")"));
                    std::string newSig = (editNetNameBuf_[0] != '\0')
                        ? ("V(" + std::string(editNetNameBuf_) + ")")
                        : ("V(" + std::to_string(nodeId) + ")");
                    vm.renameSignal(oldSig, newSig);
                    // Sync NETLABEL name on the same net
                    for (auto& c : vm.schematic().comps())
                        if (c.typeId == "NETLABEL" && !c.paramValues.empty()) {
                            auto cit = nodeMap.find(SchematicModel::pinKey(c.id, 0));
                            if (cit != nodeMap.end() && cit->second == nodeId)
                                c.paramValues[0] = editNetNameBuf_;
                        }
                }
                ew->netName = editNetNameBuf_;
            }
            editNetWireId_ = -1;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { editNetWireId_ = -1; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    // ── Custom TX wizard dialog ───────────────────────────────────────────
    if (txNPending_) ImGui::OpenPopup("Custom TX##dlg");
    if (ImGui::BeginPopupModal("Custom TX##dlg", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Group name", txNGroupBuf_, sizeof(txNGroupBuf_));
        ImGui::SliderInt("Windings", &txNWindings_, 2, 6);
        for (int i = 0; i < txNWindings_; ++i) {
            char label[32]; snprintf(label, sizeof(label), "Turns[%d]", i+1);
            ImGui::InputText(label, txNTurns_[i], sizeof(txNTurns_[i]));
        }
        if (ImGui::Button("Create")) {
            SchematicModel& sch = vm.schematic();
            pushUndo(undoStack_, redoStack_, sch, kMaxUndo);
            // TX_CORE at drop position
            int coreId = sch.addComp("TX_CORE", txNPendingPos_);
            SchematicComp* core = sch.findComp(coreId);
            if (core) {
                core->paramValues[0] = txNGroupBuf_;
                char nbuf[8]; snprintf(nbuf, sizeof(nbuf), "%d", txNWindings_);
                core->paramValues[1] = nbuf;
            }
            // TX_WIND components: primary at drop pos, secondaries offset right
            for (int i = 0; i < txNWindings_; ++i) {
                ImVec2 wpos = {txNPendingPos_.x + i * 80.f, txNPendingPos_.y};
                int wid = sch.addComp("TX_WIND", wpos);
                SchematicComp* w = sch.findComp(wid);
                if (w) {
                    w->paramValues[0] = txNGroupBuf_;
                    char ibuf[8]; snprintf(ibuf, sizeof(ibuf), "%d", i+1);
                    w->paramValues[1] = ibuf;
                    w->paramValues[2] = std::string(txNTurns_[i]);
                    if (i > 0) w->mirrorX = true;  // secondary windings face inward
                }
            }
            txNPending_ = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) { txNPending_ = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }

    ImGui::End();
}

// ── Input handling ─────────────────────────────────────────────────────────

void SchematicView::handleInput(MainViewModel& vm, bool hovered, ImVec2 origin) {
    SchematicModel& sch  = vm.schematic();
    ImVec2 mouseScreen   = ImGui::GetMousePos();
    ImVec2 mousePt       = s2c(mouseScreen, origin);
    const float hitR     = 8.0f / zoom_;

    // ── Escape: cancel wiring / probe drag ────────────────────────────────
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        if (wiringActive_) { wiringActive_ = false; wireFromCompId_ = -1; wireWaypoints_.clear(); }
        if (pinPendingActive_) pinPendingActive_ = false;
        if (vProbeDragActive_) { vProbeDragActive_ = false; vProbeNodeA_ = vProbeNodeB_ = -1; probeMode_ = PM_None; }
    }

    // ── Ctrl+S: save ──────────────────────────────────────────────────────────
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S)) {
#ifdef _WIN32
        if (!savedFilePath_.empty()) {
            doSave(savedFilePath_, vm);
        } else {
            char path[512] = {};
            if (pickSavePath(path, sizeof(path)))
                doSave(path, vm);
        }
#endif
    }

    // ── Ctrl+Z: undo / Ctrl+Y or Ctrl+Shift+Z: redo ──────────────────────────
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z) && !ImGui::GetIO().KeyShift) {
        if (!undoStack_.empty()) {
            redoStack_.push_back(sch);
            sch = undoStack_.back();
            undoStack_.pop_back();
            // Deselect to avoid dangling references
            selectedCompId_ = propEditCompId_ = movingCompId_ = selectedWireId_ = -1;
            multiSelectedIds_.clear(); multiMoveOrigPos_.clear(); moveWaypointOrig_.clear();
            wiringActive_ = false;
        }
    }
    if (ImGui::GetIO().KeyCtrl &&
        (ImGui::IsKeyPressed(ImGuiKey_Y) ||
         (ImGui::GetIO().KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z)))) {
        if (!redoStack_.empty()) {
            undoStack_.push_back(sch);
            sch = redoStack_.back();
            redoStack_.pop_back();
            selectedCompId_ = propEditCompId_ = movingCompId_ = selectedWireId_ = -1;
            multiSelectedIds_.clear(); multiMoveOrigPos_.clear(); moveWaypointOrig_.clear();
            wiringActive_ = false;
        }
    }

    // While a text field is focused (e.g. the property-value editor), the canvas
    // keyboard shortcuts below must defer to it — otherwise typing/pasting into a
    // value would rotate/mirror/copy/paste components instead.
    const bool typingInField = ImGui::GetIO().WantTextInput;

    // ── R key: rotate selected component(s) ───────────────────────────────
    if (!typingInField && ImGui::IsKeyPressed(ImGuiKey_R)) {
        auto targets = multiSelectedIds_.empty()
            ? std::vector<int>{selectedCompId_} : multiSelectedIds_;
        bool hasTarget = false;
        for (int cid : targets) if (sch.findComp(cid)) { hasTarget = true; break; }
        if (hasTarget) pushUndo(undoStack_, redoStack_, sch, kMaxUndo);
        for (int cid : targets) {
            SchematicComp* c = sch.findComp(cid);
            if (c) c->rotation = (c->rotation + 1) % 4;
        }
        propEditCompId_ = -1;
    }

    // ── X key: mirror selected component(s) ───────────────────────────────
    if (!typingInField && ImGui::IsKeyPressed(ImGuiKey_X)) {
        auto targets = multiSelectedIds_.empty()
            ? std::vector<int>{selectedCompId_} : multiSelectedIds_;
        bool hasTarget = false;
        for (int cid : targets) if (sch.findComp(cid)) { hasTarget = true; break; }
        if (hasTarget) pushUndo(undoStack_, redoStack_, sch, kMaxUndo);
        for (int cid : targets) {
            SchematicComp* c = sch.findComp(cid);
            if (c) c->mirrorX = !c->mirrorX;
        }
        propEditCompId_ = -1;
    }

    // ── Ctrl+C: store selection in the schematic clipboard ─────────────────
    // (placement happens on Ctrl+V — nothing is added to the canvas here)
    if (!typingInField && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) {
        std::vector<int> toCopy = multiSelectedIds_.empty()
            ? std::vector<int>{selectedCompId_} : multiSelectedIds_;
        toCopy.erase(std::remove(toCopy.begin(), toCopy.end(), -1), toCopy.end());
        std::vector<SchematicComp> snap;
        std::unordered_set<int> ids;
        for (int cid : toCopy) {
            if (const SchematicComp* src = sch.findComp(cid)) {
                snap.push_back(*src);
                ids.insert(cid);
            }
        }
        if (!snap.empty()) {
            clipComps_ = std::move(snap);
            clipWires_.clear();
            for (const auto& w : sch.wires())
                if (ids.count(w.fromCompId) && ids.count(w.toCompId))
                    clipWires_.push_back(w);
            clipRefPos_ = clipComps_[0].pos;   // paste anchors on the first comp
        }
    }

    // ── Ctrl+V: place the clipboard content ────────────────────────────────
    // Pastes at the mouse cursor when it hovers the canvas; otherwise offsets
    // the original location by one grid cell so the copy stays visible.
    if (!typingInField && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V) && !clipComps_.empty()) {
        ImVec2 target = hovered ? snapGrid(mousePt)
                                : ImVec2{clipRefPos_.x + 40.f, clipRefPos_.y + 40.f};
        ImVec2 delta = { snapGrid({target.x - clipRefPos_.x, target.y - clipRefPos_.y}).x,
                         snapGrid({target.x - clipRefPos_.x, target.y - clipRefPos_.y}).y };
        pushUndo(undoStack_, redoStack_, sch, kMaxUndo);
        std::unordered_map<int,int> idMap;
        std::vector<int> newIds;
        for (const auto& src : clipComps_) {
            int newId = sch.addComp(src.typeId, snapGrid({src.pos.x + delta.x,
                                                          src.pos.y + delta.y}));
            if (newId < 0) continue;
            if (SchematicComp* dst = sch.findComp(newId)) {
                dst->rotation    = src.rotation;
                dst->mirrorX     = src.mirrorX;
                dst->paramValues = src.paramValues;
            }
            idMap[src.id] = newId;
            newIds.push_back(newId);
        }
        for (const auto& w : clipWires_) {
            if (!idMap.count(w.fromCompId) || !idMap.count(w.toCompId)) continue;
            std::vector<ImVec2> shifted;
            shifted.reserve(w.waypoints.size());
            for (const auto& wp : w.waypoints)
                shifted.push_back({wp.x + delta.x, wp.y + delta.y});
            sch.addWire(idMap.at(w.fromCompId), w.fromPinIdx,
                        idMap.at(w.toCompId),   w.toPinIdx, shifted);
        }
        if (!newIds.empty()) {
            if (newIds.size() == 1) {
                selectedCompId_ = newIds[0];
                multiSelectedIds_.clear();
            } else {
                multiSelectedIds_ = newIds;
                selectedCompId_   = newIds[0];
            }
            propEditCompId_ = -1;
        }
    }

    // ── Right-mouse pan ────────────────────────────────────────────────────
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
        panningActive_ = true;
    if (panningActive_) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
            ImVec2 d = ImGui::GetIO().MouseDelta;
            panOffset_.x += d.x / zoom_;
            panOffset_.y += d.y / zoom_;
        } else {
            panningActive_ = false;
        }
    }

    // ── Scroll to zoom ─────────────────────────────────────────────────────
    if (hovered) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            float nz = std::max(0.1f, std::min(20.0f, zoom_ * (wheel > 0 ? 1.1f : 1.0f/1.1f)));
            ImVec2 ofs = { mouseScreen.x - origin.x, mouseScreen.y - origin.y };
            panOffset_.x += ofs.x/nz - ofs.x/zoom_;
            panOffset_.y += ofs.y/nz - ofs.y/zoom_;
            zoom_ = nz;
        }
    }

    // ── V-probe drag: update hover node B every frame ─────────────────────
    // Searches both wire segments AND component pins so clicking directly on a
    // component pin (e.g. inductor terminal) is reliably detected.
    if (vProbeDragActive_) {
        auto nodeMap = sch.computePinNodeMap();
        float wireHitR = 14.0f / zoom_;
        float pinHitR  = 12.0f / zoom_;
        int nearNode = -1;
        ImVec2 nearCanvas = mousePt;
        float bestD = wireHitR;
        // Wire segments
        for (const auto& wire : sch.wires()) {
            const SchematicComp* ca = sch.findComp(wire.fromCompId);
            const SchematicComp* cb = sch.findComp(wire.toCompId);
            if (!ca || !cb) continue;
            std::vector<ImVec2> path;
            path.push_back(pinCanvasPos(*ca, wire.fromPinIdx));
            for (const auto& wp : wire.waypoints) path.push_back(wp);
            path.push_back(pinCanvasPos(*cb, wire.toPinIdx));
            for (size_t i = 1; i < path.size(); ++i) {
                float d = distPointToSegment(mousePt, path[i-1], path[i]);
                if (d < bestD) {
                    bestD = d;
                    auto it = nodeMap.find(SchematicModel::pinKey(wire.fromCompId, wire.fromPinIdx));
                    if (it != nodeMap.end() && it->second != 0) {
                        nearNode = it->second;
                        nearCanvas = mousePt;
                    }
                }
            }
        }
        // Component pins (fallback if no wire was close enough)
        if (nearNode == -1) {
            float bestPinD = pinHitR;
            for (const auto& comp : sch.comps()) {
                const CompTypeDef* td = SchematicModel::findCompType(comp.typeId);
                if (!td) continue;
                for (int pi = 0; pi < (int)td->pins.size(); ++pi) {
                    ImVec2 pPos = pinCanvasPos(comp, pi);
                    float dx = mousePt.x - pPos.x, dy = mousePt.y - pPos.y;
                    float d = std::sqrt(dx*dx + dy*dy);
                    if (d < bestPinD) {
                        bestPinD = d;
                        auto it = nodeMap.find(SchematicModel::pinKey(comp.id, pi));
                        if (it != nodeMap.end() && it->second != 0) {
                            nearNode = it->second;
                            nearCanvas = pPos;
                        }
                    }
                }
            }
        }
        vProbeNodeB_ = nearNode;
        vProbeCanvasB_ = nearCanvas;
    }

    // ── V-probe drag: release → add signal ────────────────────────────────
    if (vProbeDragActive_ && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        if (vm.scopeCount() == 0) { int ni = vm.addScope(); vm.setActiveScope(ni); }
        vm.syncRawCacheToScope(vm.activeScope());
        ScopeModel& scope = vm.scope(vm.activeScope());
        int selPlot = scope.selectedPlot();
        // Resolve net names for probe nodes (prefer user-assigned net name over numeric ID)
        std::string netNameA = sch.getNetNameForNode(vProbeNodeA_);
        std::string sigA = netNameA.empty()
            ? ("V(" + std::to_string(vProbeNodeA_) + ")")
            : ("V(" + netNameA + ")");
        // Rename numeric-ID signal → named signal if applicable
        if (!netNameA.empty())
            vm.renameSignal("V(" + std::to_string(vProbeNodeA_) + ")", sigA);

        int activeSchId = vm.activeSchDoc().id;
        if (vProbeNodeB_ == -1 || vProbeNodeB_ == vProbeNodeA_) {
            // Add directly without gating on availableSignals(): the user may
            // probe before Build & Run, in which case probes_ is still empty
            // but the entry must exist so dispatchSample fills it once the
            // simulation starts.
            scope.addSignalToPlot(selPlot, sigA, 0, activeSchId);
        } else {
            std::string netNameB = sch.getNetNameForNode(vProbeNodeB_);
            std::string sigB = netNameB.empty()
                ? ("V(" + std::to_string(vProbeNodeB_) + ")")
                : ("V(" + netNameB + ")");
            if (!netNameB.empty())
                vm.renameSignal("V(" + std::to_string(vProbeNodeB_) + ")", sigB);
            std::string label = sigA + "-" + sigB.substr(2, sigB.size() - 3); // V(A-B)
            label = "V(" + sigA.substr(2, sigA.size()-3) + "-" + sigB.substr(2, sigB.size()-3) + ")";
            vm.registerComputedSig(label, sigA, 1.0, sigB, -1.0);
            scope.addSignalToPlot(selPlot, label, 0, activeSchId);
            vm.retroComputeSig(label);
        }
        vProbeDragActive_ = false;
        vProbeNodeA_ = vProbeNodeB_ = -1;
        probeMode_ = PM_None;
    }

    // ── Probe mode: V-probe press (start drag) / I-probe click ────────────
    if (hovered && probeMode_ == PM_VProbe && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        auto nodeMap = sch.computePinNodeMap();
        float wireHitR = 12.0f / zoom_;
        int foundNode = -1;
        ImVec2 foundCanvas = mousePt;
        float bestD = wireHitR;
        // Search wire segments first
        for (const auto& wire : sch.wires()) {
            const SchematicComp* ca = sch.findComp(wire.fromCompId);
            const SchematicComp* cb = sch.findComp(wire.toCompId);
            if (!ca || !cb) continue;
            std::vector<ImVec2> path;
            path.push_back(pinCanvasPos(*ca, wire.fromPinIdx));
            for (const auto& wp : wire.waypoints) path.push_back(wp);
            path.push_back(pinCanvasPos(*cb, wire.toPinIdx));
            for (size_t i = 1; i < path.size(); ++i) {
                float d = distPointToSegment(mousePt, path[i-1], path[i]);
                if (d < bestD) {
                    bestD = d;
                    auto it = nodeMap.find(SchematicModel::pinKey(wire.fromCompId, wire.fromPinIdx));
                    if (it != nodeMap.end() && it->second != 0) { foundNode = it->second; foundCanvas = mousePt; }
                }
            }
        }
        // Also search component pins if no wire found
        if (foundNode == -1) {
            float pinHitR = 12.0f / zoom_, bestPD = pinHitR;
            for (const auto& comp : sch.comps()) {
                const CompTypeDef* td = SchematicModel::findCompType(comp.typeId);
                if (!td) continue;
                for (int pi = 0; pi < (int)td->pins.size(); ++pi) {
                    ImVec2 pPos = pinCanvasPos(comp, pi);
                    float dx = mousePt.x - pPos.x, dy = mousePt.y - pPos.y;
                    float d = std::sqrt(dx*dx + dy*dy);
                    if (d < bestPD) {
                        bestPD = d;
                        auto it = nodeMap.find(SchematicModel::pinKey(comp.id, pi));
                        if (it != nodeMap.end() && it->second != 0) { foundNode = it->second; foundCanvas = pPos; }
                    }
                }
            }
        }
        if (foundNode != -1) {
            vProbeDragActive_ = true;
            vProbeNodeA_ = foundNode;
            vProbeCanvasA_ = foundCanvas;
            vProbeNodeB_ = -1;
        }
    } else if (hovered && probeMode_ == PM_IProbe && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (vm.scopeCount() == 0) { int ni = vm.addScope(); vm.setActiveScope(ni); }
        vm.syncRawCacheToScope(vm.activeScope());
        ScopeModel& scope = vm.scope(vm.activeScope());
        int selPlot = scope.selectedPlot();
        float hitR2 = (12.0f / zoom_) * (12.0f / zoom_);
        for (const auto& comp : sch.comps()) {
            const CompTypeDef* td = SchematicModel::findCompType(comp.typeId);
            if (!td) continue;
            for (int pi = 0; pi < (int)td->pins.size(); ++pi) {
                ImVec2 pPos = pinCanvasPos(comp, pi);
                float dx = mousePt.x - pPos.x, dy = mousePt.y - pPos.y;
                if (dx*dx + dy*dy <= hitR2) {
                    std::string iSig, iLabel;
                    double iScale = 1.0;
                    bool probeReady = false;

                    if (comp.typeId == "TX_WIND") {
                        // Find TX_CORE parent and sorted winding index
                        if (!comp.paramValues.empty()) {
                            const std::string& grp = comp.paramValues[0];
                            const SchematicComp* txCore = nullptr;
                            for (const auto& tc : sch.comps())
                                if (tc.typeId == "TX_CORE" && !tc.paramValues.empty() && tc.paramValues[0] == grp)
                                    { txCore = &tc; break; }
                            if (txCore) {
                                std::vector<const SchematicComp*> winds;
                                for (const auto& wc : sch.comps())
                                    if (wc.typeId == "TX_WIND" && !wc.paramValues.empty() && wc.paramValues[0] == grp)
                                        winds.push_back(&wc);
                                std::sort(winds.begin(), winds.end(), [](const SchematicComp* a, const SchematicComp* b){
                                    int ai=0, bi=0;
                                    try { if (a->paramValues.size()>1) ai=std::stoi(a->paramValues[1]); } catch(...){}
                                    try { if (b->paramValues.size()>1) bi=std::stoi(b->paramValues[1]); } catch(...){}
                                    return ai < bi;
                                });
                                int wi = -1;
                                for (int i = 0; i < (int)winds.size(); ++i)
                                    if (winds[i]->id == comp.id) { wi = i; break; }
                                if (wi >= 0) {
                                    std::string wSuffix = wi > 0 ? "_W" + std::to_string(wi) : "";
                                    iSig   = "I(" + txCore->instanceName + wSuffix + ")";
                                    const std::string& pinLbl = td->pins[pi].label;
                                    iLabel = "I(" + txCore->instanceName + wSuffix + "-" + pinLbl + ")";
                                    iScale = (pi == 0) ? 1.0 : -1.0;
                                    // Don't gate on availableSignals(): user may probe before
                                    // Build & Run; the entry must exist so dispatchSample fills
                                    // it once the simulation starts.
                                    probeReady = true;
                                }
                            }
                        }
                    } else if (comp.typeId == "TX" || comp.typeId == "TX3") {
                        int wi = pi / 2;
                        iScale = (pi % 2 == 0) ? 1.0 : -1.0;
                        std::string wSuffix = wi > 0 ? "_W" + std::to_string(wi) : "";
                        iSig   = "I(" + comp.instanceName + wSuffix + ")";
                        const std::string& pinLbl = td->pins[pi].label;
                        iLabel = "I(" + comp.instanceName + wSuffix + "-" + pinLbl + ")";
                        probeReady = true;
                    } else {
                        iSig   = "I(" + comp.instanceName + ")";
                        const std::string& pinLbl = td->pins[pi].label;
                        iLabel = "I(" + comp.instanceName + "-" + pinLbl + ")";
                        iScale = (pi == 0) ? 1.0 : -1.0;
                        probeReady = true;
                    }

                    if (probeReady) {
                        vm.registerComputedSig(iLabel, iSig, iScale);
                        scope.addSignalToPlot(selPlot, iLabel, 0, vm.activeSchDoc().id);
                        vm.retroComputeSig(iLabel);
                    }
                    goto probeHandled;
                }
            }
        }
        probeHandled:;
        probeMode_ = PM_None;
    }

    // ── Left-click ────────────────────────────────────────────────────────
    else if (hovered && probeMode_ == PM_None && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        bool hitPin = false, hitBody = false;

        // Priority 1: pins (use rotated positions). Asymmetric hit region:
        // generous in the OUTWARD direction (away from comp body) so the user
        // can grab the pin from its "free" side; tight inward and perpendicular
        // so it doesn't shadow the body or adjacent pins. For pins at the
        // component center (e.g. JUNC, GND) the outward direction is degenerate;
        // fall back to a small symmetric circle.
        // Sizes tuned so wiring is easy to start (generous outward + perp)
        // without stealing body clicks: a press on a pin no longer locks into
        // wiring immediately — see the deferred pinPending_ resolution below.
        const float pinOutward = 16.0f / zoom_;
        const float pinInward  = 6.0f  / zoom_;
        const float pinPerp    = 8.0f  / zoom_;
        for (auto& comp : sch.comps()) {
            const CompTypeDef* td = SchematicModel::findCompType(comp.typeId);
            if (!td) continue;
            for (int pi = 0; pi < (int)td->pins.size(); ++pi) {
                ImVec2 pPos = pinCanvasPos(comp, pi);
                float toPx = mousePt.x - pPos.x, toPy = mousePt.y - pPos.y;
                float ox   = pPos.x - comp.pos.x, oy = pPos.y - comp.pos.y;
                float olen = sqrtf(ox*ox + oy*oy);
                bool pinHit;
                if (olen < 1.0f) {
                    // Center-pin component (JUNC, GND): fall back to small circle.
                    pinHit = (toPx*toPx + toPy*toPy <= hitR*hitR);
                } else {
                    float onx = ox / olen, ony = oy / olen;
                    float along = toPx * onx + toPy * ony;
                    float pxr   = toPx - along * onx;
                    float pyr   = toPy - along * ony;
                    float perp  = sqrtf(pxr*pxr + pyr*pyr);
                    pinHit = (along >= -pinInward && along <= pinOutward && perp <= pinPerp);
                }
                if (pinHit) {
                    if (wiringActive_) {
                        if (comp.id != wireFromCompId_ || pi != wireFromPinIdx_) {
                            pushUndo(undoStack_, redoStack_, sch, kMaxUndo);
                            ImVec2 endPos = pinCanvasPos(comp, pi);
                            const SchematicComp* fromC = sch.findComp(wireFromCompId_);
                            ImVec2 last = wireWaypoints_.empty()
                                ? (fromC ? pinCanvasPos(*fromC, wireFromPinIdx_) : endPos)
                                : wireWaypoints_.back();
                            appendManhattanCorner(wireWaypoints_, last, endPos);
                            sch.addWire(wireFromCompId_, wireFromPinIdx_, comp.id, pi, wireWaypoints_);
                        }
                        wiringActive_ = false; wireFromCompId_ = -1; wireWaypoints_.clear();
                    } else {
                        // Defer the decision to release: a stationary click
                        // starts wiring, a drag past the threshold moves the
                        // component instead (grabbing a comp by its lead, or
                        // GND/JUNC whose pin covers the whole symbol, must not
                        // lock into wiring mode).
                        pinPendingActive_ = true;
                        pinPendingCompId_ = comp.id;
                        pinPendingPinIdx_ = pi;
                        pinPendingStart_  = mousePt;
                    }
                    hitPin = true;
                    break;
                }
            }
            if (hitPin) break;
        }

        // During wiring: check wire-to-wire hit, otherwise add waypoint
        if (!hitPin && wiringActive_) {
            ImVec2 hitWireSnap;
            int hitWireId = hitTestWire(sch, mousePt, 8.0f / zoom_, &hitWireSnap);
            if (hitWireId >= 0) {
                pushUndo(undoStack_, redoStack_, sch, kMaxUndo);
                // Resolve start-pin position BEFORE insertJunctionOnWire — it
                // adds a comp and may reallocate the comps vector.
                const SchematicComp* fromC = sch.findComp(wireFromCompId_);
                ImVec2 last = wireWaypoints_.empty()
                    ? (fromC ? pinCanvasPos(*fromC, wireFromPinIdx_) : hitWireSnap)
                    : wireWaypoints_.back();
                int juncId = insertJunctionOnWire(sch, hitWireId, hitWireSnap);
                if (juncId >= 0) {
                    appendManhattanCorner(wireWaypoints_, last, hitWireSnap);
                    sch.addWire(wireFromCompId_, wireFromPinIdx_, juncId, 0, wireWaypoints_);
                    wiringActive_ = false; wireFromCompId_ = -1; wireWaypoints_.clear();
                }
            } else {
                ImVec2 tgt = snapGrid(mousePt);
                const SchematicComp* fromC = sch.findComp(wireFromCompId_);
                ImVec2 last = wireWaypoints_.empty()
                    ? (fromC ? pinCanvasPos(*fromC, wireFromPinIdx_) : tgt)
                    : wireWaypoints_.back();
                appendManhattanCorner(wireWaypoints_, last, tgt);
                wireWaypoints_.push_back(tgt);
            }
        }

        // Priority 2: component bodies (only when not wiring)
        if (!hitPin && !wiringActive_) {
            // Hit region follows the symbol outline (strokes / filled bodies),
            // expanded by a few screen px converted to canvas units — replacing
            // the old coarse AABB so clicks in empty corners no longer select.
            const float hitMargin = 5.0f / zoom_;
            for (auto& comp : sch.comps()) {
                if (!hitTestCompBody(comp, mousePt, hitMargin)) continue;
                {
                    wiringActive_ = false;
                    wireWaypoints_.clear();

                    if (ImGui::GetIO().KeyCtrl) {
                        bool alreadySel = (comp.id == selectedCompId_) ||
                            std::find(multiSelectedIds_.begin(), multiSelectedIds_.end(), comp.id)
                                != multiSelectedIds_.end();
                        if (alreadySel) {
                            // Ctrl-press on a selected comp: decide on release —
                            // drag past the threshold duplicates the selection,
                            // a plain release keeps the old toggle-out behavior.
                            ctrlDragPending_ = true;
                            ctrlDragCompId_  = comp.id;
                            ctrlDragStart_   = mousePt;
                        } else {
                            // Ctrl+click on an unselected comp: add to multi-select.
                            // On first Ctrl+click, also include the previously
                            // single-selected comp.
                            if (selectedCompId_ != -1 && multiSelectedIds_.empty())
                                multiSelectedIds_.push_back(selectedCompId_);
                            multiSelectedIds_.push_back(comp.id);
                            selectedCompId_ = comp.id;
                            propEditCompId_ = -1;
                        }
                    } else {
                        // Normal click: select + arm move (multi-move if the
                        // comp is already part of the multi-selection)
                        beginCompMove(sch, comp, mousePt);
                    }
                    hitBody = true;
                    break;
                }
            }
        }

        if (!hitPin && !hitBody && !wiringActive_) {
            // Priority 3: wire hit-test
            float wireHitR = 6.0f / zoom_;
            float bestDist = wireHitR;
            int bestWireId = -1;
            int bestSeg    = -1;
            for (const auto& wire : sch.wires()) {
                const SchematicComp* ca = sch.findComp(wire.fromCompId);
                const SchematicComp* cb = sch.findComp(wire.toCompId);
                if (!ca || !cb) continue;
                ImVec2 pa = pinCanvasPos(*ca, wire.fromPinIdx);
                ImVec2 pb = pinCanvasPos(*cb, wire.toPinIdx);
                std::vector<ImVec2> path;
                path.push_back(pa);
                for (const auto& wp : wire.waypoints) path.push_back(wp);
                path.push_back(pb);
                for (size_t i = 1; i < path.size(); ++i) {
                    float d = distPointToSegment(mousePt, path[i-1], path[i]);
                    if (d < bestDist) { bestDist = d; bestWireId = wire.id; bestSeg = (int)i - 1; }
                }
            }
            if (bestWireId != -1 && ImGui::GetIO().KeyAlt) {
                // Alt+click: tap the wire — insert a junction at the click
                // point and start routing a new wire from it (mirror of the
                // end-on-wire auto-junction that already exists).
                ImVec2 snapPt;
                int wid = hitTestWire(sch, mousePt, wireHitR, &snapPt);
                if (wid >= 0) {
                    pushUndo(undoStack_, redoStack_, sch, kMaxUndo);
                    int juncId = insertJunctionOnWire(sch, wid, snapPt);
                    if (juncId >= 0) {
                        wiringActive_   = true;
                        wireFromCompId_ = juncId;
                        wireFromPinIdx_ = 0;
                        wireWaypoints_.clear();
                        selectedWireId_ = -1;
                        selectedCompId_ = -1;
                        multiSelectedIds_.clear();
                        propEditCompId_ = -1;
                    }
                }
            } else if (bestWireId != -1) {
                selectedWireId_   = bestWireId;
                selectedCompId_   = -1;
                multiSelectedIds_.clear();
                propEditCompId_   = -1;
                // Arm segment dragging; it activates once the mouse moves past
                // a small threshold (so plain clicks / double-clicks still work).
                wireDragId_      = bestWireId;
                wireDragSeg_     = bestSeg;
                wireDragActive_  = false;
                wireDragStartPt_ = mousePt;
                // Double-click on wire → open net name editor
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    editNetWireId_ = bestWireId;
                    SchematicWire* ew = sch.findWire(bestWireId);
                    if (ew) {
                        strncpy(editNetNameBuf_, ew->netName.c_str(), sizeof(editNetNameBuf_)-1);
                        editNetNameBuf_[sizeof(editNetNameBuf_)-1] = '\0';
                        // If wire has no name, check for NETLABEL on the same net
                        if (editNetNameBuf_[0] == '\0') {
                            auto nodeMap = sch.computePinNodeMap();
                            auto nit = nodeMap.find(SchematicModel::pinKey(ew->fromCompId, ew->fromPinIdx));
                            if (nit != nodeMap.end() && nit->second != 0) {
                                int nodeId = nit->second;
                                for (const auto& c : sch.comps())
                                    if (c.typeId == "NETLABEL" && !c.paramValues.empty()) {
                                        auto cit = nodeMap.find(SchematicModel::pinKey(c.id, 0));
                                        if (cit != nodeMap.end() && cit->second == nodeId) {
                                            strncpy(editNetNameBuf_, c.paramValues[0].c_str(),
                                                    sizeof(editNetNameBuf_)-1);
                                            editNetNameBuf_[sizeof(editNetNameBuf_)-1] = '\0';
                                            break;
                                        }
                                    }
                            }
                        }
                    } else {
                        editNetNameBuf_[0] = '\0';
                    }
                }
            } else {
                // Start rubber-band selection box
                selBoxActive_       = true;
                selBoxStartCanvas_  = mousePt;
                if (!ImGui::GetIO().KeyCtrl) {
                    selectedWireId_   = -1;
                    selectedCompId_   = -1;
                    multiSelectedIds_.clear();
                    propEditCompId_   = -1;
                }
                wiringActive_ = false;
                wireWaypoints_.clear();
            }
        } else if (hitBody) {
            selectedWireId_ = -1;
        }
    }

    // ── Rubber-band selection box (finish on mouse release) ────────────────
    if (selBoxActive_ && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        ImVec2 bMin = { std::min(selBoxStartCanvas_.x, mousePt.x),
                        std::min(selBoxStartCanvas_.y, mousePt.y) };
        ImVec2 bMax = { std::max(selBoxStartCanvas_.x, mousePt.x),
                        std::max(selBoxStartCanvas_.y, mousePt.y) };
        bool anyAdded = false;
        for (auto& comp : sch.comps()) {
            if (comp.pos.x >= bMin.x && comp.pos.x <= bMax.x &&
                comp.pos.y >= bMin.y && comp.pos.y <= bMax.y) {
                if (std::find(multiSelectedIds_.begin(), multiSelectedIds_.end(), comp.id)
                    == multiSelectedIds_.end())
                    multiSelectedIds_.push_back(comp.id);
                anyAdded = true;
            }
        }
        if (anyAdded) {
            selectedCompId_ = multiSelectedIds_[0];
            propEditCompId_ = -1;
        }
        if (multiSelectedIds_.size() == 1) {
            selectedCompId_ = multiSelectedIds_[0];
            multiSelectedIds_.clear();
        }
        selBoxActive_ = false;
    }

    // ── Wire segment drag: shift an orthogonal segment perpendicular ───────
    if (wireDragId_ != -1) {
        SchematicWire* w = sch.findWire(wireDragId_);
        const SchematicComp* ca = w ? sch.findComp(w->fromCompId) : nullptr;
        const SchematicComp* cb = w ? sch.findComp(w->toCompId)   : nullptr;
        if (!w || !ca || !cb) {
            wireDragId_ = -1; wireDragActive_ = false;
        } else if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (!wireDragActive_) {
                float ddx = mousePt.x - wireDragStartPt_.x;
                float ddy = mousePt.y - wireDragStartPt_.y;
                if (ddx*ddx + ddy*ddy > 25.0f) {   // ~5 canvas px
                    ImVec2 pinA = pinCanvasPos(*ca, w->fromPinIdx);
                    ImVec2 pinB = pinCanvasPos(*cb, w->toPinIdx);
                    std::vector<ImVec2> path;
                    path.push_back(pinA);
                    for (const auto& wp : w->waypoints) path.push_back(wp);
                    path.push_back(pinB);
                    int s = wireDragSeg_;
                    if (s < 0 || s + 1 >= (int)path.size()) {
                        wireDragId_ = -1;
                    } else {
                        ImVec2 a = path[s], b = path[s+1];
                        bool vert  = std::fabs(a.x-b.x) <  0.5f && std::fabs(a.y-b.y) >= 0.5f;
                        bool horiz = std::fabs(a.y-b.y) <  0.5f && std::fabs(a.x-b.x) >= 0.5f;
                        if (!vert && !horiz) {
                            wireDragId_ = -1;   // diagonal / zero-length: not draggable
                        } else {
                            pushUndo(undoStack_, redoStack_, sch, kMaxUndo);
                            // Segment ends attached to pins can't move — insert
                            // waypoint copies of the pin positions so the dragged
                            // segment always sits between two waypoints.
                            int N = (int)w->waypoints.size();
                            if (s == 0) { w->waypoints.insert(w->waypoints.begin(), pinA); s = 1; N++; }
                            if (s == N) { w->waypoints.push_back(pinB); N++; }
                            wireDragWpA_    = s - 1;
                            wireDragWpB_    = s;
                            wireDragVert_   = vert;
                            wireDragActive_ = true;
                        }
                    }
                }
            }
            if (wireDragActive_ && wireDragWpA_ >= 0 &&
                wireDragWpB_ < (int)w->waypoints.size()) {
                if (wireDragVert_) {
                    float nx = snapGrid({mousePt.x, 0.f}).x;
                    w->waypoints[wireDragWpA_].x = nx;
                    w->waypoints[wireDragWpB_].x = nx;
                } else {
                    float ny = snapGrid({0.f, mousePt.y}).y;
                    w->waypoints[wireDragWpA_].y = ny;
                    w->waypoints[wireDragWpB_].y = ny;
                }
            }
        } else {
            // Release: simplify the polyline — drop zero-length segments and
            // collinear midpoints left over from dragging.
            if (wireDragActive_) {
                ImVec2 pinA = pinCanvasPos(*ca, w->fromPinIdx);
                ImVec2 pinB = pinCanvasPos(*cb, w->toPinIdx);
                std::vector<ImVec2> path;
                path.push_back(pinA);
                for (const auto& wp : w->waypoints) path.push_back(wp);
                path.push_back(pinB);
                std::vector<ImVec2> out;
                out.push_back(path.front());
                for (size_t i = 1; i + 1 < path.size(); ++i) {
                    ImVec2 prev = out.back(), cur = path[i], nxt = path[i+1];
                    bool dupPrev = std::fabs(cur.x-prev.x) < 0.5f && std::fabs(cur.y-prev.y) < 0.5f;
                    bool collin  = (std::fabs(prev.x-cur.x) < 0.5f && std::fabs(cur.x-nxt.x) < 0.5f) ||
                                   (std::fabs(prev.y-cur.y) < 0.5f && std::fabs(cur.y-nxt.y) < 0.5f);
                    if (dupPrev || collin) continue;
                    out.push_back(cur);
                }
                w->waypoints.assign(out.begin() + 1, out.end());
            }
            wireDragId_ = -1; wireDragActive_ = false;
            wireDragWpA_ = wireDragWpB_ = -1;
        }
    }

    // ── Ctrl-drag duplicate: resolve the pending Ctrl-press ────────────────
    if (ctrlDragPending_) {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            // Plain Ctrl+click on a selected comp → toggle it OUT of the selection
            if (selectedCompId_ != -1 && multiSelectedIds_.empty())
                multiSelectedIds_.push_back(selectedCompId_);
            auto it = std::find(multiSelectedIds_.begin(), multiSelectedIds_.end(),
                                ctrlDragCompId_);
            if (it != multiSelectedIds_.end()) {
                multiSelectedIds_.erase(it);
                if (selectedCompId_ == ctrlDragCompId_)
                    selectedCompId_ = multiSelectedIds_.empty() ? -1 : multiSelectedIds_[0];
            }
            if (multiSelectedIds_.size() == 1) {
                selectedCompId_ = multiSelectedIds_[0];
                multiSelectedIds_.clear();
            }
            propEditCompId_  = -1;
            ctrlDragPending_ = false;
        } else {
            float ddx = mousePt.x - ctrlDragStart_.x;
            float ddy = mousePt.y - ctrlDragStart_.y;
            if (ddx*ddx + ddy*ddy > 36.0f) {   // ~6 canvas px: it's a drag
                // Duplicate the whole selection in place and hand the copies to
                // the regular move logic — they follow the cursor from here on.
                std::vector<int> ids = multiSelectedIds_.empty()
                    ? std::vector<int>{selectedCompId_} : multiSelectedIds_;
                ids.erase(std::remove(ids.begin(), ids.end(), -1), ids.end());
                if (!ids.empty()) {
                    pushUndo(undoStack_, redoStack_, sch, kMaxUndo);
                    std::unordered_set<int> srcSet(ids.begin(), ids.end());
                    std::unordered_map<int,int> idMap;
                    std::vector<int> newIds;
                    for (int cid : ids) {
                        const SchematicComp* src = sch.findComp(cid);
                        if (!src) continue;
                        // Copy fields before addComp — reallocation invalidates src
                        std::string typeId = src->typeId;
                        ImVec2      pos    = src->pos;
                        int         rot    = src->rotation;
                        bool        mirror = src->mirrorX;
                        auto        params = src->paramValues;
                        int newId = sch.addComp(typeId, pos);
                        if (newId < 0) continue;
                        if (SchematicComp* dst = sch.findComp(newId)) {
                            dst->rotation    = rot;
                            dst->mirrorX     = mirror;
                            dst->paramValues = params;
                        }
                        idMap[cid] = newId;
                        newIds.push_back(newId);
                    }
                    for (const auto& w : std::vector<SchematicWire>(sch.wires())) {
                        if (srcSet.count(w.fromCompId) && srcSet.count(w.toCompId) &&
                            idMap.count(w.fromCompId) && idMap.count(w.toCompId))
                            sch.addWire(idMap.at(w.fromCompId), w.fromPinIdx,
                                        idMap.at(w.toCompId),   w.toPinIdx, w.waypoints);
                    }
                    if (!newIds.empty()) {
                        multiSelectedIds_ = (newIds.size() > 1) ? newIds : std::vector<int>{};
                        selectedCompId_   = newIds[0];
                        propEditCompId_   = -1;
                        // Arm the standard move machinery on the copies
                        int moveId = idMap.count(ctrlDragCompId_) ? idMap.at(ctrlDragCompId_)
                                                                  : newIds[0];
                        movingCompId_    = moveId;
                        moveStartCanvas_ = ctrlDragStart_;
                        if (SchematicComp* mc = sch.findComp(moveId))
                            moveCompOrigPos_ = mc->pos;
                        multiMoveOrigPos_.clear();
                        for (int nid : newIds)
                            if (SchematicComp* mc = sch.findComp(nid))
                                multiMoveOrigPos_.push_back({nid, mc->pos});
                        moveWaypointOrig_.clear();
                        std::unordered_set<int> movedSet(newIds.begin(), newIds.end());
                        for (auto& w : sch.wires())
                            if (!w.waypoints.empty() && movedSet.count(w.fromCompId) &&
                                movedSet.count(w.toCompId))
                                moveWaypointOrig_.push_back({w.id, w.waypoints});
                    }
                }
                ctrlDragPending_ = false;
            }
        }
    }

    // ── Deferred pin press resolution ──────────────────────────────────────
    // Release without movement → start wiring from the pressed pin.
    // Drag past the threshold   → move the component instead.
    if (pinPendingActive_) {
        float dx = mousePt.x - pinPendingStart_.x;
        float dy = mousePt.y - pinPendingStart_.y;
        float th = 5.0f / zoom_;   // ~5 px screen
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            if (dx*dx + dy*dy > th*th) {
                pinPendingActive_ = false;
                if (SchematicComp* pc = sch.findComp(pinPendingCompId_))
                    beginCompMove(sch, *pc, pinPendingStart_);
            }
        } else {
            pinPendingActive_ = false;
            wiringActive_   = true;
            wireFromCompId_ = pinPendingCompId_;
            wireFromPinIdx_ = pinPendingPinIdx_;
            wireWaypoints_.clear();
        }
    }

    // ── Drag to move (single or multi) ────────────────────────────────────
    if (movingCompId_ != -1) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float dx = mousePt.x - moveStartCanvas_.x;
            float dy = mousePt.y - moveStartCanvas_.y;
            if (!multiMoveOrigPos_.empty()) {
                for (auto& [cid, origPos] : multiMoveOrigPos_) {
                    SchematicComp* c = sch.findComp(cid);
                    if (c) c->pos = snapGrid({origPos.x + dx, origPos.y + dy});
                }
            } else {
                SchematicComp* c = sch.findComp(movingCompId_);
                if (c) c->pos = snapGrid({moveCompOrigPos_.x + dx, moveCompOrigPos_.y + dy});
            }
            // Move waypoints for wires fully within the moved set
            float snappedDx = snapGrid({dx, 0}).x;
            float snappedDy = snapGrid({0, dy}).y;
            for (auto& [wid, origWps] : moveWaypointOrig_) {
                SchematicWire* w = sch.findWire(wid);
                if (!w) continue;
                w->waypoints.resize(origWps.size());
                for (size_t i = 0; i < origWps.size(); i++)
                    w->waypoints[i] = {origWps[i].x + snappedDx, origWps[i].y + snappedDy};
            }
        } else {
            movingCompId_ = -1;
            multiMoveOrigPos_.clear();
            moveWaypointOrig_.clear();
        }
    }

    // ── Delete ─────────────────────────────────────────────────────────────
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        if (!multiSelectedIds_.empty()) {
            pushUndo(undoStack_, redoStack_, sch, kMaxUndo);
            for (int cid : multiSelectedIds_) sch.removeComp(cid);
            multiSelectedIds_.clear();
            selectedCompId_ = propEditCompId_ = movingCompId_ = -1;
            multiMoveOrigPos_.clear();
            wiringActive_ = false;
        } else if (selectedCompId_ != -1) {
            pushUndo(undoStack_, redoStack_, sch, kMaxUndo);
            sch.removeComp(selectedCompId_);
            selectedCompId_ = propEditCompId_ = movingCompId_ = -1;
            wiringActive_ = false;
        } else if (selectedWireId_ != -1) {
            pushUndo(undoStack_, redoStack_, sch, kMaxUndo);
            sch.removeWire(selectedWireId_);
            selectedWireId_ = -1;
        }
    }
}

// ── Grid drawing ───────────────────────────────────────────────────────────

void SchematicView::drawGrid(ImDrawList* dl, ImVec2 origin, ImVec2 size) const {
    const float gridScr = 20.0f * zoom_;
    const ImU32 col     = IM_COL32(38, 45, 55, 255);
    float offX = fmodf(panOffset_.x * zoom_, gridScr); if (offX < 0) offX += gridScr;
    float offY = fmodf(panOffset_.y * zoom_, gridScr); if (offY < 0) offY += gridScr;
    for (float x = origin.x + offX; x < origin.x + size.x; x += gridScr)
        dl->AddLine({x, origin.y}, {x, origin.y + size.y}, col, 0.5f);
    for (float y = origin.y + offY; y < origin.y + size.y; y += gridScr)
        dl->AddLine({origin.x, y}, {origin.x + size.x, y}, col, 0.5f);
}

// ── Wire drawing ───────────────────────────────────────────────────────────

void SchematicView::drawWires(ImDrawList* dl, MainViewModel& vm, ImVec2 origin) const {
    const SchematicModel& sch = vm.schematic();
    const ImU32 wireCol = IM_COL32(200, 200, 80, 220);

    // Determine which nets to highlight (from scope legend hover).
    // Supports both numeric V(N), named V(VIN), and differential V(A-B).
    const std::string& hovSig = vm.hoveredSignal();
    int hovNetA = -1, hovNetB = -1;
    std::unordered_map<int,int> nodeMap;
    if (hovSig.size() > 2 && hovSig[0]=='V' && hovSig[1]=='(') {
        std::string inner = hovSig.substr(2, hovSig.size() - 3);  // strip "V(" and ")"
        size_t dash = inner.find('-');
        auto resolveNet = [&](const std::string& s) -> int {
            try { return std::stoi(s); } catch(...) {}
            // Named net: look up in netName→nodeId map
            auto netNameMap = sch.computeNetNameToNodeMap();
            auto it = netNameMap.find(s);
            return (it != netNameMap.end()) ? it->second : -1;
        };
        if (dash == std::string::npos) {
            hovNetA = resolveNet(inner);
        } else {
            hovNetA = resolveNet(inner.substr(0, dash));
            hovNetB = resolveNet(inner.substr(dash + 1));
        }
        if (hovNetA >= 0) nodeMap = sch.computePinNodeMap();
    }

    for (const auto& wire : sch.wires()) {
        const SchematicComp* ca = sch.findComp(wire.fromCompId);
        const SchematicComp* cb = sch.findComp(wire.toCompId);
        if (!ca || !cb) continue;
        bool wireSel = (wire.id == selectedWireId_);

        // Highlight positive net (gold) and negative net (green) independently
        int wireNet = -1;
        if (hovNetA >= 0 && !nodeMap.empty()) {
            auto it = nodeMap.find(SchematicModel::pinKey(wire.fromCompId, wire.fromPinIdx));
            if (it != nodeMap.end()) wireNet = it->second;
        }
        bool hlA = (wireNet >= 0 && wireNet == hovNetA);
        bool hlB = (wireNet >= 0 && wireNet == hovNetB);

        ImU32 col = wireSel ? IM_COL32(255, 255, 100, 255) :
                    hlA     ? IM_COL32(255, 220,  50, 255) :   // positive net: gold
                    hlB     ? IM_COL32( 80, 255, 120, 255) :   // negative net: green
                              wireCol;
        float thick = (wireSel || hlA || hlB) ? 3.0f * zoom_ : 1.5f * zoom_;

        ImVec2 prevScr = c2s(pinCanvasPos(*ca, wire.fromPinIdx), origin);
        for (const auto& wp : wire.waypoints) {
            ImVec2 wpScr = c2s(wp, origin);
            dl->AddLine(prevScr, wpScr, col, thick);
            prevScr = wpScr;
        }
        ImVec2 toScr = c2s(pinCanvasPos(*cb, wire.toPinIdx), origin);
        dl->AddLine(prevScr, toScr, col, thick);

        // Net name label at wire midpoint (between last waypoint and toScr)
        if (!wire.netName.empty()) {
            ImVec2 mid = {(prevScr.x + toScr.x) * 0.5f, (prevScr.y + toScr.y) * 0.5f - 8.f * zoom_};
            ImVec2 ts  = ImGui::CalcTextSize(wire.netName.c_str());
            dl->AddText({mid.x - ts.x * 0.5f, mid.y - ts.y * 0.5f},
                        IM_COL32(120, 220, 255, 220), wire.netName.c_str());
        }
    }
}

// ── Dashed line helper ─────────────────────────────────────────────────────

void SchematicView::drawDashedLine(ImDrawList* dl, ImVec2 a, ImVec2 b,
                                   ImU32 col, float thick, float dashLen, float gapLen) {
    float dx=b.x-a.x, dy=b.y-a.y, len=sqrtf(dx*dx+dy*dy);
    if (len<1.f) return;
    float nx=dx/len, ny=dy/len, t=0.f; bool draw=true;
    while (t<len) {
        float seg=draw?dashLen:gapLen, tEnd=std::min(t+seg,len);
        if (draw) dl->AddLine({a.x+nx*t,a.y+ny*t},{a.x+nx*tEnd,a.y+ny*tEnd},col,thick);
        t=tEnd; draw=!draw;
    }
}

// ── Insert JUNC on existing wire ───────────────────────────────────────────

int SchematicView::insertJunctionOnWire(SchematicModel& sch, int wireId, ImVec2 juncPos) {
    SchematicWire wc; bool found=false;
    for (const auto& w:sch.wires()) { if (w.id==wireId){wc=w;found=true;break;} }
    if (!found) return -1;
    const SchematicComp* ca=sch.findComp(wc.fromCompId);
    const SchematicComp* cb=sch.findComp(wc.toCompId);
    if (!ca||!cb) return -1;
    std::vector<ImVec2> fp;
    fp.push_back(pinCanvasPos(*ca,wc.fromPinIdx));
    for (const auto& wp:wc.waypoints) fp.push_back(wp);
    fp.push_back(pinCanvasPos(*cb,wc.toPinIdx));
    int splitSeg=-1; float bestD=10.f;
    for (int i=1;i<(int)fp.size();++i){
        float d=distPointToSegment(juncPos,fp[i-1],fp[i]);
        if (d<bestD){bestD=d;splitSeg=i;}
    }
    if (splitSeg<0) return -1;
    int juncId=sch.addComp("JUNC",juncPos);
    std::vector<ImVec2> wp1,wp2;
    for (int i=1;i<splitSeg;++i)                wp1.push_back(fp[i]);
    for (int i=splitSeg;i<(int)fp.size()-1;++i) wp2.push_back(fp[i]);
    sch.removeWire(wireId);
    sch.addWire(wc.fromCompId,wc.fromPinIdx,juncId,0,wp1);
    sch.addWire(juncId,0,wc.toCompId,wc.toPinIdx,wp2);
    return juncId;
}

// ── Wire hit-test ──────────────────────────────────────────────────────────

int SchematicView::hitTestWire(const SchematicModel& sch, ImVec2 pt, float maxDist,
                               ImVec2* outSnap) const {
    int bestId = -1;
    float bestD = maxDist;
    ImVec2 snap = snapGrid(pt);
    for (const auto& w : sch.wires()) {
        const SchematicComp* fc = sch.findComp(w.fromCompId);
        const SchematicComp* tc = sch.findComp(w.toCompId);
        if (!fc || !tc) continue;
        std::vector<ImVec2> path;
        path.push_back(pinCanvasPos(*fc, w.fromPinIdx));
        for (const auto& wp : w.waypoints) path.push_back(wp);
        path.push_back(pinCanvasPos(*tc, w.toPinIdx));
        for (size_t i = 1; i < path.size(); ++i) {
            float d = distPointToSegment(pt, path[i-1], path[i]);
            if (d < bestD) {
                bestD  = d;
                bestId = w.id;
                // Grid-snapped projection of pt onto the hit segment
                ImVec2 a = path[i-1], b = path[i];
                ImVec2 ab = {b.x - a.x, b.y - a.y};
                float len2 = ab.x*ab.x + ab.y*ab.y;
                float t = len2 > 0.f
                    ? ((pt.x-a.x)*ab.x + (pt.y-a.y)*ab.y) / len2 : 0.f;
                t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
                snap = snapGrid({a.x + t*ab.x, a.y + t*ab.y});
            }
        }
    }
    if (outSnap) *outSnap = snap;
    return bestId;
}

// ── Component move arming (shared by body-click and pin-drag) ──────────────

void SchematicView::beginCompMove(SchematicModel& sch, SchematicComp& comp, ImVec2 pressPt) {
    bool inMulti = !multiSelectedIds_.empty() &&
        std::find(multiSelectedIds_.begin(), multiSelectedIds_.end(), comp.id)
            != multiSelectedIds_.end();
    if (!inMulti) {
        // Single-select this component
        multiSelectedIds_.clear();
        selectedCompId_ = comp.id;
    }
    selectedWireId_  = -1;
    movingCompId_    = comp.id;
    moveStartCanvas_ = pressPt;
    moveCompOrigPos_ = comp.pos;
    pushUndo(undoStack_, redoStack_, sch, kMaxUndo);
    // Store original positions of all selected for multi-move
    multiMoveOrigPos_.clear();
    const std::vector<int> toMove = inMulti ? multiSelectedIds_
                                            : std::vector<int>{comp.id};
    for (int cid : toMove) {
        SchematicComp* mc = sch.findComp(cid);
        if (mc) multiMoveOrigPos_.push_back({cid, mc->pos});
    }
    // Store original waypoints for wires fully inside the moved set
    moveWaypointOrig_.clear();
    std::unordered_set<int> movedSet;
    for (auto& [cid, origP] : multiMoveOrigPos_) movedSet.insert(cid);
    for (auto& w : sch.wires()) {
        if (!w.waypoints.empty()
            && movedSet.count(w.fromCompId)
            && movedSet.count(w.toCompId))
            moveWaypointOrig_.push_back({w.id, w.waypoints});
    }
    // Refresh property buffers
    if (propEditCompId_ != comp.id) {
        propEditCompId_ = comp.id;
        strncpy(propNameBuf_, comp.instanceName.c_str(), sizeof(propNameBuf_)-1);
        propNameBuf_[sizeof(propNameBuf_)-1] = '\0';
        for (int i = 0; i < 8; ++i) propBufs_[i][0] = '\0';
        for (int i = 0; i < (int)comp.paramValues.size() && i < 8; ++i) {
            strncpy(propBufs_[i], comp.paramValues[i].c_str(), sizeof(propBufs_[i])-1);
            propBufs_[i][sizeof(propBufs_[i])-1] = '\0';
        }
    }
}

// ── TX_CORE coupling dashes ────────────────────────────────────────────────

void SchematicView::drawTxCoreSymbol(ImDrawList* dl, const SchematicComp& txCore,
                                     const SchematicModel& sch, ImVec2 origin) const {
    if (txCore.paramValues.empty()) return;
    const std::string& grp=txCore.paramValues[0];
    std::vector<const SchematicComp*> winds;
    for (const auto& c:sch.comps())
        if (c.typeId=="TX_WIND"&&!c.paramValues.empty()&&c.paramValues[0]==grp)
            winds.push_back(&c);
    if (winds.size()<2) return;
    std::sort(winds.begin(),winds.end(),[](const SchematicComp*a,const SchematicComp*b){
        int ai=0,bi=0;
        try{if(a->paramValues.size()>1)ai=std::stoi(a->paramValues[1]);}catch(...){}
        try{if(b->paramValues.size()>1)bi=std::stoi(b->paramValues[1]);}catch(...){}
        return ai<bi;
    });
    ImU32 dc=IM_COL32(120,155,220,110); float z=zoom_;
    // Helper: apply mirrorX + rotation to a local offset and convert to screen coords
    auto windSC = [&](const SchematicComp& wc, float ox, float oy) -> ImVec2 {
        float mx = wc.mirrorX ? -ox : ox;
        ImVec2 r = rotateOff({mx, oy}, wc.rotation);
        ImVec2 ctr_w = c2s(wc.pos, origin);
        return {ctr_w.x + r.x * z, ctr_w.y + r.y * z};
    };
    std::vector<ImVec2> mids;
    for (const auto* wc:winds){
        // Dashed bar on the "bumpy" side (+x before mirrorX), ±24 along winding axis.
        // Per user request the core line and inter-winding connector live on the
        // same side as the winding bumps now; the polarity dot in TX_WIND moved
        // with it to remain on the core side.
        ImVec2 top = windSC(*wc, +14.f, -24.f);
        ImVec2 bot = windSC(*wc, +14.f, +24.f);
        ImVec2 mid = windSC(*wc, +14.f,   0.f);
        drawDashedLine(dl, top, bot, dc, 1.3f*z, 5.f*z, 3.f*z);
        mids.push_back(mid);
    }
    for (size_t i=1;i<mids.size();++i)
        drawDashedLine(dl,mids[i-1],mids[i],dc,1.0f*z,4.f*z,3.f*z);
}

// ── Per-type standard symbol drawing ──────────────────────────────────────

void SchematicView::drawCompSymbol(ImDrawList* dl, const SchematicComp& comp,
                                   const CompTypeDef& /*td*/, ImVec2 ctr, bool sel)
{
    static constexpr float PI = 3.14159265358979323846f;
    ImU32 col   = sel ? IM_COL32(255,210,50,255) : IM_COL32(110,155,220,255);
    float thick = (sel ? 2.2f : 1.5f) * zoom_;
    float z     = zoom_;

    // Rotate + optional mirror → screen coords
    auto sc = [&](float ox, float oy) -> ImVec2 {
        float mx = comp.mirrorX ? -ox : ox;
        ImVec2 r = rotateOff({mx, oy}, comp.rotation);
        return {ctr.x + r.x * z, ctr.y + r.y * z};
    };

    const std::string& id = comp.typeId;

    // ── Resistor ──────────────────────────────────────────────────────────
    if (id == "R") {
        dl->AddLine(sc(-40,0), sc(-14,0), col, thick);
        dl->AddLine(sc(+14,0), sc(+40,0), col, thick);
        dl->AddLine(sc(-14,-7), sc(+14,-7), col, thick);
        dl->AddLine(sc(+14,-7), sc(+14,+7), col, thick);
        dl->AddLine(sc(+14,+7), sc(-14,+7), col, thick);
        dl->AddLine(sc(-14,+7), sc(-14,-7), col, thick);
    }
    // ── Capacitor ─────────────────────────────────────────────────────────
    else if (id == "C") {
        dl->AddLine(sc(-20,0), sc(-5,0),  col, thick);
        dl->AddLine(sc(+5,0),  sc(+20,0), col, thick);
        dl->AddLine(sc(-5,-12), sc(-5,+12), col, thick*1.5f);
        dl->AddLine(sc(+5,-12), sc(+5,+12), col, thick*1.5f);
    }
    // ── Inductor ──────────────────────────────────────────────────────────
    else if (id == "L") {
        dl->AddLine(sc(-40,0), sc(-24,0), col, thick);
        dl->AddLine(sc(+24,0), sc(+40,0), col, thick);
        const int N = 14;
        float bumpCx[4] = {-18.f, -6.f, +6.f, +18.f};
        for (int b = 0; b < 4; ++b) {
            float cx = bumpCx[b];
            for (int k = 0; k <= N; ++k) {
                float a = PI + (float)k / N * PI;   // π→2π: upward bumps
                dl->PathLineTo(sc(cx + 6.f*cosf(a), 6.f*sinf(a)));
            }
        }
        dl->PathStroke(col, 0, thick);
    }
    // ── Voltage sources — circle stays upright, only leads rotate ─────────
    else if (id == "V_DC" || id == "V_SIN" || id == "V_SQUARE" || id == "V_STEP") {
        dl->AddCircle(ctr, 18.f*z, col, 32, thick);
        dl->AddLine(sc(-40,0), sc(-18,0), col, thick);
        dl->AddLine(sc(+18,0), sc(+40,0), col, thick);

        if (id == "V_DC") {
            // "+" near P pin (rotates with component)
            dl->AddLine(sc(-12, 0), sc(-6,  0), col, thick);   // horizontal bar of +
            dl->AddLine(sc(-9, -3), sc(-9, +3), col, thick);   // vertical bar of +
            // "−" near N pin (rotates with component)
            dl->AddLine(sc(+6, 0), sc(+12, 0), col, thick);    // minus bar
        } else if (id == "V_SIN") {
            const int NS = 16;
            for (int k = 0; k <= NS; ++k) {
                float t  = (float)k / NS;
                dl->PathLineTo({ctr.x + (-12.f + 24.f*t)*z,
                                ctr.y + (-6.f*sinf(2.f*PI*t))*z});
            }
            dl->PathStroke(col, 0, thick);
        } else if (id == "V_SQUARE") {
            dl->PathLineTo({ctr.x - 12*z, ctr.y - 5*z});
            dl->PathLineTo({ctr.x -  4*z, ctr.y - 5*z});
            dl->PathLineTo({ctr.x -  4*z, ctr.y + 5*z});
            dl->PathLineTo({ctr.x +  4*z, ctr.y + 5*z});
            dl->PathLineTo({ctr.x +  4*z, ctr.y - 5*z});
            dl->PathLineTo({ctr.x + 12*z, ctr.y - 5*z});
            dl->PathStroke(col, 0, thick);
        } else { // V_STEP
            dl->PathLineTo({ctr.x - 12*z, ctr.y + 5*z});
            dl->PathLineTo({ctr.x -  4*z, ctr.y + 5*z});
            dl->PathLineTo({ctr.x -  4*z, ctr.y - 5*z});
            dl->PathLineTo({ctr.x + 12*z, ctr.y - 5*z});
            dl->PathStroke(col, 0, thick);
        }
    }
    // ── Current source — circle upright, arrow fixed horizontal ───────────
    else if (id == "I") {
        dl->AddCircle(ctr, 18.f*z, col, 32, thick);
        dl->AddLine(sc(-40,0), sc(-18,0), col, thick);
        dl->AddLine(sc(+18,0), sc(+40,0), col, thick);
        dl->AddLine(sc(+8,0), sc(-8,0), col, thick);
        dl->AddTriangleFilled(sc(-8,0), sc(-4,-4), sc(-4,+4), col);
    }
    // ── Controlled sources (VCVS / VCCS): diamond body ────────────────────
    // Output P on top, N on bottom; single control-sense pin CP on the left
    // with an open-circle terminal (high-impedance voltage sense vs. GND).
    else if (id == "VCVS" || id == "VCCS") {
        // Output leads
        dl->AddLine(sc(0,-40), sc(0,-16), col, thick);
        dl->AddLine(sc(0,+16), sc(0,+40), col, thick);
        // Diamond
        dl->PathLineTo(sc(0,-16));
        dl->PathLineTo(sc(+16,0));
        dl->PathLineTo(sc(0,+16));
        dl->PathLineTo(sc(-16,0));
        dl->PathStroke(col, ImDrawFlags_Closed, thick);
        // Control-sense lead with open terminal
        dl->AddLine(sc(-40,0), sc(-26,0), col, thick);
        dl->AddCircle(sc(-23,0), 3.f*z, col, 12, thick*0.8f);
        // Control polarity "+" above the lead (sense is CP vs. GND)
        dl->AddLine(sc(-36,-7), sc(-30,-7), col, thick);
        dl->AddLine(sc(-33,-10), sc(-33,-4), col, thick);

        if (id == "VCVS") {
            // "+" near P inside the diamond, "−" near N
            dl->AddLine(sc(-3,-7), sc(+3,-7), col, thick);
            dl->AddLine(sc(0,-10), sc(0,-4),  col, thick);
            dl->AddLine(sc(-3,+7), sc(+3,+7), col, thick);
        } else { // VCCS: current arrow pointing toward P (current exits at P)
            dl->AddLine(sc(0,+9), sc(0,-4), col, thick);
            dl->AddTriangleFilled(sc(0,-9), sc(-4,-3), sc(+4,-3), col);
        }
    }
    // ── Op-amp / Comparator: triangle body ────────────────────────────────
    // IN+ top-left, IN- bottom-left, OUT right (single-ended vs GND).
    else if (id == "OPAMP" || id == "CMP") {
        // Triangle
        dl->PathLineTo(sc(-24,-24));
        dl->PathLineTo(sc(+24,0));
        dl->PathLineTo(sc(-24,+24));
        dl->PathStroke(col, ImDrawFlags_Closed, thick);
        // Leads
        dl->AddLine(sc(-40,-20), sc(-24,-20), col, thick);   // IN+
        dl->AddLine(sc(-40,+20), sc(-24,+20), col, thick);   // IN-
        dl->AddLine(sc(+24,0),   sc(+40,0),   col, thick);   // OUT
        // Input polarity inside the triangle
        dl->AddLine(sc(-20,-20), sc(-12,-20), col, thick);   // "+" horizontal
        dl->AddLine(sc(-16,-24), sc(-16,-16), col, thick);   // "+" vertical
        dl->AddLine(sc(-20,+20), sc(-12,+20), col, thick);   // "−"
        if (id == "CMP") {
            // Step glyph marks the comparator's 2-state output
            dl->PathLineTo(sc(-6,+6));
            dl->PathLineTo(sc(0,+6));
            dl->PathLineTo(sc(0,-6));
            dl->PathLineTo(sc(+6,-6));
            dl->PathStroke(col, 0, thick);
        }
    }
    // ── Diode ─────────────────────────────────────────────────────────────
    else if (id == "D") {
        dl->AddLine(sc(-40,0), sc(-12,0), col, thick);
        dl->AddLine(sc(+12,0), sc(+40,0), col, thick);
        dl->PathLineTo(sc(-12,-10));
        dl->PathLineTo(sc(+12,  0));
        dl->PathLineTo(sc(-12,+10));
        dl->PathStroke(col, ImDrawFlags_Closed, thick);
        dl->AddLine(sc(+12,-12), sc(+12,+12), col, thick);
    }
    // ── N-channel MOSFET with body diode ────────────────────────────────────
    // User coords (x,y) Y-up → canvas sc(x, -y). G(-20,0)→extends to sc(-40,0).
    else if (id == "S") {
        // G lead: model G pin → gate bar
        dl->AddLine(sc(-20,  0), sc(-5,  0), col, thick);
        // GRef lead: model GRef pin → gate bar bottom
        dl->AddLine(sc(-20, +20), sc(-5, +20), col, thick);
        // Gate bar: (0,15)→(0,-20) user = sc(0,-15)→sc(0,+20)
        dl->AddLine(sc(-5, -15), sc(-5, +20), col, thick);

        // Channel: 3 segments at x=5
        dl->AddLine(sc(0, -20), sc(0, -10), col, thick);  // (5,20)→(5,10)
        dl->AddLine(sc(0,  -5), sc(0,  +5), col, thick);  // (5,5)→(5,-5)
        dl->AddLine(sc(0, +10), sc(0, +20), col, thick);  // (5,-10)→(5,-20)

        // Horizontal stubs: drain y=-15, body y=0, source y=+15
        dl->AddLine(sc(0, -15), sc(20, -15), col, thick);
        dl->AddLine(sc(0,   0), sc(20,   0), col, thick);
        dl->AddLine(sc(0, +15), sc(20, +15), col, thick);

        // D vertical (15,15)→(15,25): drain stub → up  = sc(15,-15)→sc(15,-25)
        dl->AddLine(sc(20, -15), sc(20, -25), col, thick);
        // S vertical (15,0)→(15,-25): body stub → down = sc(15,0)→sc(15,+25)
        dl->AddLine(sc(20,   0), sc(20, +25), col, thick);

        // D horizontal (15,20)→(23,20) = sc(15,-20)→sc(23,-20)
        dl->AddLine(sc(20, -20), sc(33, -20), col, thick);
        // S horizontal (15,-20)→(23,-20) = sc(15,+20)→sc(23,+20)
        dl->AddLine(sc(20, +20), sc(33, +20), col, thick);

        // Outer bar (23,20)→(23,-20): split for body diode gap at y=-5..+10
        dl->AddLine(sc(33, -20), sc(33,  -5), col, thick);  // upper
        dl->AddLine(sc(33, +5), sc(33, +20), col, thick);  // lower

        // D pin lead: sc(0,-40)→sc(0,-25)→sc(15,-25)  [L to D vertical top]
        dl->AddLine(sc(20, -40), sc(20,  -25), col, thick);

        // S pin lead: sc(15,+25)→sc(15,+40)→sc(0,+40)  [L from S vertical bottom]
        dl->AddLine(sc(20, +25), sc(20, +40), col, thick);

        // Body arrow: left-pointing, tip at sc(0,0), base at sc(5,±5)
        dl->AddTriangleFilled(sc(0, 0), sc(10, -5), sc(10, +5), col);

        // Body diode: up-pointing, cathode tip at sc(23,-5)
        // Cathode bar (23-5,5)→(23+5,5) = sc(18,-5)→sc(28,-5)
        dl->AddLine(sc(28, -5), sc(38, -5), col, thick);
        // Diode triangle: tip sc(23,-5), anode base sc(18,+10)→sc(28,+10)
        dl->AddTriangleFilled(sc(33, -5), sc(28, +5), sc(38, +5), col);

        // Pin labels
        ImU32 lblCol = sel ? IM_COL32(255,230,100,220) : IM_COL32(160,200,255,200);
        float lsz    = 12.0f;
        auto addPL = [&](float ox, float oy, float dx, float dy, const char* txt) {
            ImVec2 ps = sc(ox, oy);
            ImVec2 ts = ImGui::CalcTextSize(txt);
            dl->AddText(nullptr, lsz, ImVec2{ps.x + dx*z - ts.x*.5f, ps.y + dy*z - ts.y*.5f}, lblCol, txt);
        };
        addPL(-20,   -10, -1.8f,  0.0f, "G");
        addPL(-20, +30, -1.8f,  0.0f, "Ref");
        addPL(  10, -40,  0.0f, -1.8f, "D");
        addPL(  10, +40,  0.0f, +1.8f, "S");
    }
    // ── Transformer 2-winding (turns ratio label) ─────────────────────────
    else if (id == "TX") {
        ImU32 coreCol = sel ? IM_COL32(255,210,50,180) : IM_COL32(150,180,230,200);
        dl->AddLine(sc(-5,-18), sc(-5,+18), coreCol, thick*0.8f);
        dl->AddLine(sc(+5,-18), sc(+5,+18), coreCol, thick*0.8f);
        dl->AddLine(sc(-40,-20), sc(-22,-18), col, thick);
        dl->AddLine(sc(-40,+20), sc(-22,+18), col, thick);
        dl->AddLine(sc(+40,-20), sc(+22,-18), col, thick);
        dl->AddLine(sc(+40,+20), sc(+22,+18), col, thick);
        const int NC = 12;
        float primY[3] = {-12.f, 0.f, +12.f};
        for (int b = 0; b < 3; ++b) {
            float yc = primY[b];
            for (int k = 0; k <= NC; ++k) {
                float a = -PI/2.f - (float)k/NC*PI;
                dl->PathLineTo(sc(-22.f + 6.f*cosf(a), yc + 6.f*sinf(a)));
            }
        }
        dl->PathStroke(col, 0, thick);
        for (int b = 0; b < 3; ++b) {
            float yc = primY[b];
            for (int k = 0; k <= NC; ++k) {
                float a = -PI/2.f + (float)k/NC*PI;
                dl->PathLineTo(sc(+22.f + 6.f*cosf(a), yc + 6.f*sinf(a)));
            }
        }
        dl->PathStroke(col, 0, thick);
        // Polarity dots at same-name ends (P1 / P2)
        { ImU32 dc=sel?IM_COL32(255,230,100,255):IM_COL32(220,230,255,240);
          dl->AddCircleFilled(sc(-28,-22),3.5f*z,dc);  // P1 dot
          dl->AddCircleFilled(sc(+28,-22),3.5f*z,dc);  // P2 dot
        }
        // Turns ratio "n1:n2" centred upright
        if (comp.paramValues.size() >= 2) {
            char turns[40];
            std::snprintf(turns, sizeof(turns), "%s:%s",
                          comp.paramValues[0].c_str(), comp.paramValues[1].c_str());
            ImVec2 ts = ImGui::CalcTextSize(turns);
            dl->AddText({ctr.x - ts.x*0.5f, ctr.y - ts.y*0.5f},
                        sel ? IM_COL32(255,210,50,255) : IM_COL32(180,210,255,255), turns);
        }
    }
    // ── Transformer 3-winding ─────────────────────────────────────────────
    else if (id == "TX3") {
        ImU32 coreCol = sel ? IM_COL32(255,210,50,180) : IM_COL32(150,180,230,200);
        dl->AddLine(sc(-5,-30), sc(-5,+30), coreCol, thick*0.8f);
        dl->AddLine(sc(+5,-30), sc(+5,+30), coreCol, thick*0.8f);
        // Lead wires
        dl->AddLine(sc(-40,-20), sc(-22,-18), col, thick);   // P1
        dl->AddLine(sc(-40,+20), sc(-22,+18), col, thick);   // N1
        dl->AddLine(sc(+40,-30), sc(+22,-30), col, thick);   // P2
        dl->AddLine(sc(+40,-10), sc(+22,-10), col, thick);   // N2
        dl->AddLine(sc(+40,+10), sc(+22,+10), col, thick);   // P3
        dl->AddLine(sc(+40,+30), sc(+22,+30), col, thick);   // N3
        const int NC = 12, NS2 = 10;
        // Primary: 3 left-pointing bumps (y -18..+18)
        float primY3[3] = {-12.f, 0.f, +12.f};
        for (int b = 0; b < 3; ++b) {
            float yc = primY3[b];
            for (int k = 0; k <= NC; ++k) {
                float a = -PI/2.f - (float)k/NC*PI;
                dl->PathLineTo(sc(-22.f + 6.f*cosf(a), yc + 6.f*sinf(a)));
            }
        }
        dl->PathStroke(col, 0, thick);
        // Secondary 1: 2 right-pointing bumps r=5 (y -30..-10)
        float sec1Y[2] = {-25.f, -15.f};
        for (int b = 0; b < 2; ++b) {
            float yc = sec1Y[b];
            for (int k = 0; k <= NS2; ++k) {
                float a = -PI/2.f + (float)k/NS2*PI;
                dl->PathLineTo(sc(+22.f + 5.f*cosf(a), yc + 5.f*sinf(a)));
            }
        }
        dl->PathStroke(col, 0, thick);
        // Secondary 2: 2 right-pointing bumps r=5 (y +10..+30)
        float sec2Y[2] = {+15.f, +25.f};
        for (int b = 0; b < 2; ++b) {
            float yc = sec2Y[b];
            for (int k = 0; k <= NS2; ++k) {
                float a = -PI/2.f + (float)k/NS2*PI;
                dl->PathLineTo(sc(+22.f + 5.f*cosf(a), yc + 5.f*sinf(a)));
            }
        }
        dl->PathStroke(col, 0, thick);
        // Turns label "n1:n2:n3"
        if (comp.paramValues.size() >= 3) {
            char turns[48];
            std::snprintf(turns, sizeof(turns), "%s:%s:%s",
                          comp.paramValues[0].c_str(), comp.paramValues[1].c_str(),
                          comp.paramValues[2].c_str());
            ImVec2 ts = ImGui::CalcTextSize(turns);
            dl->AddText({ctr.x - ts.x*0.5f, ctr.y - ts.y*0.5f},
                        sel ? IM_COL32(255,210,50,255) : IM_COL32(180,210,255,255), turns);
        }
        // Polarity dots P1/P2/P3
        { ImU32 dc=sel?IM_COL32(255,230,100,255):IM_COL32(220,230,255,240);
          dl->AddCircleFilled(sc(-28,-22),3.5f*z,dc);
          dl->AddCircleFilled(sc(+28,-32),3.5f*z,dc);
          dl->AddCircleFilled(sc(+28, +8),3.5f*z,dc); }
    }
    // ── Junction dot ──────────────────────────────────────────────────────
    else if (id == "JUNC") {
        dl->AddCircleFilled(ctr, 5.f*z,
            sel ? IM_COL32(255,210,50,255) : IM_COL32(80,210,120,255));
    }
    // ── Net label ─────────────────────────────────────────────────────────
    else if (id == "NETLABEL") {
        ImU32 nc = sel ? IM_COL32(255,210,50,255) : IM_COL32(80,230,120,255);
        // Compact flag pentagon -- the pin is at (-20,0) and the flag tip
        // points to (-4,0) so the connection point IS the flag's leftmost
        // notch. No separate pin lead line; the flag itself indicates the
        // attachment direction.
        dl->PathLineTo(sc(-20,0));
        dl->PathLineTo(sc(-16,-7));
        dl->PathLineTo(sc(-4,-7));
        dl->PathLineTo(sc(-4,+7));
        dl->PathLineTo(sc(-16,+7));
        dl->PathStroke(nc, ImDrawFlags_Closed, thick*0.7f);
        // Label text -- offset away from the flag toward +x so it doesn't
        // overlap the symbol.
        if (!comp.paramValues.empty()) {
            ImVec2 lc = sc(+10,0);
            ImVec2 ts = ImGui::CalcTextSize(comp.paramValues[0].c_str());
            dl->AddText({lc.x-ts.x*.5f, lc.y-ts.y*.5f}, nc, comp.paramValues[0].c_str());
        }
    }
    // ── TX_WIND (individual transformer winding) ──────────────────────────
    else if (id == "TX_WIND") {
        dl->AddLine(sc(0,-40), sc(0,-24), col, thick);  // top lead
        dl->AddLine(sc(0,+24), sc(0,+40), col, thick);  // bottom lead
        // 4 rightward bumps
        float bCy[4]={-18.f,-6.f,+6.f,+18.f};
        for (int b=0;b<4;++b){
            float yc=bCy[b];
            for (int k=0;k<=12;++k){
                float a=-PI/2.f+(float)k/12.f*PI;
                dl->PathLineTo(sc(6.f*cosf(a), yc+6.f*sinf(a)));
            }
        }
        dl->PathStroke(col, 0, thick);
        // Polarity dot at P-side, on the core side (+x before mirrorX -- moved
        // along with the TX_CORE dashed bar/connector to remain on the core).
        { ImU32 dc=sel?IM_COL32(255,230,100,255):IM_COL32(220,230,255,240);
          dl->AddCircleFilled(sc(+10,-22),2.5f*z,dc); }
        // Turns label to the right
        if (comp.paramValues.size() >= 3) {
            char lbl[24]; std::snprintf(lbl,sizeof(lbl),"n=%s",comp.paramValues[2].c_str());
            ImVec2 lp=sc(+18,0); ImVec2 ts=ImGui::CalcTextSize(lbl);
            dl->AddText({lp.x-ts.x*.5f,lp.y-ts.y*.5f},
                sel?IM_COL32(255,210,50,200):IM_COL32(180,210,255,200),lbl);
        }
    }
}

// ── Component drawing ──────────────────────────────────────────────────────

void SchematicView::drawComponents(ImDrawList* dl, MainViewModel& vm, ImVec2 origin) {
    const SchematicModel& sch = vm.schematic();

    // Pre-compute the I-hover target instance name once.
    // The hovered signal can take several forms:
    //   "I(R1)"            — auto-populated probe, simple component
    //   "I(R1-P)"          — interactively probed, with pin-label suffix
    //   "I(TX1_W1)"        — TX winding (auto-populated)
    //   "I(TX1_W1-P)"      — interactively probed TX winding pin
    // Strip the leading "I(" and trailing ")", then drop any "-<pinLabel>"
    // suffix and any "_W<digits>" winding-index suffix to recover the bare
    // SchematicComp::instanceName ("R1" / "TX1" in the examples above).
    std::string hovICompName;
    {
        const std::string& hovSig = vm.hoveredSignal();
        if (hovSig.size() > 3 && hovSig[0] == 'I' && hovSig[1] == '('
            && hovSig.back() == ')') {
            std::string s = hovSig.substr(2, hovSig.size() - 3);
            size_t dash = s.find('-');
            if (dash != std::string::npos) s = s.substr(0, dash);
            size_t wpos = s.rfind("_W");
            if (wpos != std::string::npos && wpos + 2 < s.size()) {
                bool allDigits = true;
                for (size_t i = wpos + 2; i < s.size(); i++)
                    if (!std::isdigit((unsigned char)s[i])) { allDigits = false; break; }
                if (allDigits) s = s.substr(0, wpos);
            }
            hovICompName = std::move(s);
        }
    }

    // Pre-compute: number of wire endpoints terminating at each (compId, pinIdx).
    // Used by the pin-dot renderer to switch styling: 1 wire = "single connection"
    // (small blue), 0 or 2+ wires = existing green/yellow dot. Junctions still
    // draw their own dot via the JUNC component path.
    std::unordered_map<int64_t, int> wireCountPerPin;
    auto pinKey = [](int compId, int pinIdx) -> int64_t {
        return (static_cast<int64_t>(compId) << 16) | static_cast<int64_t>(pinIdx);
    };
    for (const auto& wire : sch.wires()) {
        wireCountPerPin[pinKey(wire.fromCompId, wire.fromPinIdx)]++;
        wireCountPerPin[pinKey(wire.toCompId,   wire.toPinIdx  )]++;
    }

    for (const auto& comp : sch.comps()) {
        const CompTypeDef* td = SchematicModel::findCompType(comp.typeId);
        if (!td) continue;
        bool sel = (comp.id == selectedCompId_) ||
                   (!multiSelectedIds_.empty() &&
                    std::find(multiSelectedIds_.begin(), multiSelectedIds_.end(), comp.id)
                        != multiSelectedIds_.end());
        // Highlight component if scope legend hovers its current signal.
        if (!hovICompName.empty() && comp.instanceName == hovICompName)
            sel = true;
        ImVec2 ctr = c2s(comp.pos, origin);

        // ── GND symbol ────────────────────────────────────────────────────
        if (comp.typeId == "GND") {
            float s   = zoom_;
            ImU32 col = sel ? IM_COL32(255,210,50,255) : IM_COL32(120,180,255,255);
            // Stem and bar directions follow rotation
            ImVec2 sv = rotateOff({0.0f,  1.0f}, comp.rotation);  // stem direction
            ImVec2 pv = rotateOff({1.0f,  0.0f}, comp.rotation);  // bar direction
            // Stem
            dl->AddLine(ctr, {ctr.x+sv.x*8*s, ctr.y+sv.y*8*s}, col, 1.5f*s);
            // Three bars (lengths 10, 6, 2 at distances 8, 12, 16)
            float dists[3] = {8.0f, 12.0f, 16.0f};
            float lens [3] = {10.0f, 6.0f,  2.0f};
            for (int i = 0; i < 3; ++i) {
                float d = dists[i] * s, l = lens[i] * s;
                ImVec2 bc = {ctr.x+sv.x*d, ctr.y+sv.y*d};
                dl->AddLine({bc.x-pv.x*l, bc.y-pv.y*l},
                            {bc.x+pv.x*l, bc.y+pv.y*l}, col, 1.5f*s);
            }
            // Pin dot at top
            dl->AddCircleFilled(ctr, 3.5f*s, IM_COL32(80,200,120,255));
            continue;
        }

        // ── TX_CORE: custom coupling symbol, no standard label/pins ─────────
        if (comp.typeId == "TX_CORE") {
            drawTxCoreSymbol(dl, comp, vm.schematic(), origin);
            continue;
        }

        // ── Standard circuit symbol ───────────────────────────────────────
        drawCompSymbol(dl, comp, *td, ctr, sel);

        // Body half-size kept for label placement only (bodyHalfSize unchanged)
        float bx = td->bodyHalfSize.x, by = td->bodyHalfSize.y;
        if (comp.rotation % 2 == 1) std::swap(bx, by);
        float bxs = bx * zoom_, bys = by * zoom_;
        (void)bxs;

        // Instance name label (suppressed for JUNC/NETLABEL/TX_WIND/TXN_CUSTOM)
        if (comp.typeId != "JUNC" && comp.typeId != "NETLABEL" &&
            comp.typeId != "TX_WIND" && comp.typeId != "TXN_CUSTOM") {
            char lbl[80];
            if (!comp.paramValues.empty())
                snprintf(lbl, sizeof(lbl), "%s=%s", comp.instanceName.c_str(), comp.paramValues[0].c_str());
            else
                snprintf(lbl, sizeof(lbl), "%s", comp.instanceName.c_str());
            ImVec2 ls = ImGui::CalcTextSize(lbl);
            dl->AddText({ctr.x-ls.x*.5f, ctr.y+bys+2*zoom_}, IM_COL32(170,200,170,255), lbl);
        } else if (comp.typeId == "TX_WIND" && comp.paramValues.size() >= 3) {
            // Show txGroup:turns above the winding
            char lbl[64];
            snprintf(lbl, sizeof(lbl), "%s n=%s", comp.paramValues[0].c_str(), comp.paramValues[2].c_str());
            ImVec2 ls = ImGui::CalcTextSize(lbl);
            dl->AddText({ctr.x-ls.x*.5f, ctr.y-bys-14*zoom_}, IM_COL32(170,200,170,255), lbl);
        }

        // ── Pins (mirrorX + rotated positions) ───────────────────────────
        if (comp.typeId != "JUNC")
        for (int pi = 0; pi < (int)td->pins.size(); ++pi) {
            ImVec2 pinCanvas = pinCanvasPos(comp, pi);
            ImVec2 pinScr    = c2s(pinCanvas, origin);

            // Pin circle. Three styles depending on wire count terminating here:
            //   isStart (wire drag in progress): bright green, full size.
            //   exactly 1 wire connected:        small blue dot (clean look for
            //                                    direct pin-to-wire join).
            //   0 or 2+ wires:                   default green dot (signals an
            //                                    unconnected pin or a fan-out;
            //                                    junctions get their own dot
            //                                    via the JUNC component path).
            bool isStart = (wiringActive_ && wireFromCompId_==comp.id && wireFromPinIdx_==pi);
            int wireCount = 0;
            {
                auto it = wireCountPerPin.find(pinKey(comp.id, pi));
                if (it != wireCountPerPin.end()) wireCount = it->second;
            }
            if (isStart) {
                dl->AddCircleFilled(pinScr, 4.0f*zoom_, IM_COL32(50,255,100,255));
            } else if (wireCount == 1) {
                dl->AddCircleFilled(pinScr, 2.5f*zoom_, IM_COL32(120,170,255,255));
            } else {
                dl->AddCircleFilled(pinScr, 4.0f*zoom_, IM_COL32(80,200,120,255));
            }

            // Polarity "+" label (accounts for mirrorX + rotation)
            const char* polSym = polaritySymbol(td->pins[pi].label);
            if (polSym) {
                float ox = td->pins[pi].offset.x;
                float oy = td->pins[pi].offset.y;
                if (comp.mirrorX) ox = -ox;
                float len = sqrtf(ox*ox + oy*oy);
                if (len > 1.0f) {
                    ImVec2 dir = { ox / len, oy / len };
                    ImVec2 labelOff    = { ox + dir.x * 12.0f, oy + dir.y * 12.0f };
                    ImVec2 labelRotOff = rotateOff(labelOff, comp.rotation);
                    ImVec2 labelCanvas = { comp.pos.x + labelRotOff.x, comp.pos.y + labelRotOff.y };
                    ImVec2 labelScr    = c2s(labelCanvas, origin);
                    ImVec2 ts = ImGui::CalcTextSize(polSym);
                    dl->AddText({labelScr.x - ts.x*0.5f, labelScr.y - ts.y*0.5f},
                                IM_COL32(255,120,120,255), polSym);
                }
            }
        }
    }
}

// ── Rubber-band wire / selection box ────────────────────────────────────────

void SchematicView::drawRubberBand(ImDrawList* dl, MainViewModel& vm, ImVec2 origin) const {
    // Selection box
    if (selBoxActive_) {
        ImVec2 bStart = c2s(selBoxStartCanvas_, origin);
        ImVec2 bEnd   = ImGui::GetMousePos();
        float x0 = std::min(bStart.x, bEnd.x), y0 = std::min(bStart.y, bEnd.y);
        float x1 = std::max(bStart.x, bEnd.x), y1 = std::max(bStart.y, bEnd.y);
        dl->AddRectFilled({x0,y0}, {x1,y1}, IM_COL32(100,150,255,30));
        dl->AddRect({x0,y0}, {x1,y1}, IM_COL32(100,150,255,200), 0.f, 0, 1.0f);
    }

    if (!wiringActive_) return;
    const SchematicComp* c = vm.schematic().findComp(wireFromCompId_);
    if (!c) return;

    ImU32 col = IM_COL32(80,220,120,200);
    float thick = 1.5f * zoom_;

    ImVec2 fromScr = c2s(pinCanvasPos(*c, wireFromPinIdx_), origin);

    // Draw through all accumulated waypoints
    for (const auto& wp : wireWaypoints_) {
        ImVec2 wpScr = c2s(wp, origin);
        dl->AddLine(fromScr, wpScr, col, thick);
        fromScr = wpScr;
    }

    // Manhattan-routed segment from last point to mouse (horizontal first, then vertical)
    ImVec2 toScr = ImGui::GetMousePos();
    ImVec2 midScr = { toScr.x, fromScr.y };
    dl->AddLine(fromScr, midScr, col, thick);
    dl->AddLine(midScr, toScr, col, thick);
    dl->AddCircle(toScr, 5.0f * zoom_, col);
}

// ── Property panel ──────────────────────────────────────────────────────────

void SchematicView::renderProperties(MainViewModel& vm) {
    if (selectedCompId_ == -1) return;
    SchematicModel& sch = vm.schematic();
    SchematicComp*  c   = sch.findComp(selectedCompId_);
    if (!c) { selectedCompId_ = propEditCompId_ = -1; return; }
    const CompTypeDef* td = SchematicModel::findCompType(c->typeId);
    if (!td || c->typeId == "GND") return;

    // Multi-edit: every selected component sharing the reference type receives
    // the same param edits. Mixed-type selections edit only the matching ones.
    std::vector<SchematicComp*> targets{c};
    for (int cid : multiSelectedIds_) {
        if (cid == selectedCompId_) continue;
        SchematicComp* mc = sch.findComp(cid);
        if (mc && mc->typeId == c->typeId) targets.push_back(mc);
    }
    const bool multiEdit = targets.size() > 1;

    // Refresh buffers when selection changes
    if (propEditCompId_ != selectedCompId_) {
        propEditCompId_ = selectedCompId_;
        strncpy(propNameBuf_, c->instanceName.c_str(), sizeof(propNameBuf_)-1);
        propNameBuf_[sizeof(propNameBuf_)-1] = '\0';
        for (int i = 0; i < 8; ++i) propBufs_[i][0] = '\0';
        for (int i = 0; i < (int)c->paramValues.size() && i < 8; ++i) {
            strncpy(propBufs_[i], c->paramValues[i].c_str(), sizeof(propBufs_[i])-1);
            propBufs_[i][sizeof(propBufs_[i])-1] = '\0';
        }
    }

    ImGui::Separator();
    if (multiEdit)
        ImGui::TextDisabled("Properties: %d x %s (editing all)  |  [R to rotate]",
                            (int)targets.size(), td->displayName.c_str());
    else
        ImGui::TextDisabled("Properties: %s (%s)  |  Rotation: %d°  [R to rotate]",
                            c->instanceName.c_str(), td->displayName.c_str(),
                            c->rotation * 90);

    if (!multiEdit) {
        // Instance names are unique — no renaming in multi-edit.
        ImGui::SetNextItemWidth(100.0f);
        ImGui::InputText("Name##prop", propNameBuf_, sizeof(propNameBuf_));
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            pushUndo(undoStack_, redoStack_, sch, kMaxUndo);
            c->instanceName = propNameBuf_;
        }
    }

    // Expression-aware numeric parse: variables + arithmetic + SPICE suffixes.
    const auto varMap = sch.variableMap();
    auto parseNum = [&varMap](const std::string& s) -> double {
        return exprv::evalOr(s, &varMap, 0.0);
    };

    // Indices of the freq / tdelay / phase / _linkBy params for V_SQUARE so the
    // bidirectional linkage can update the right cells after each edit.
    int idxFreq = -1, idxTDelay = -1, idxPhase = -1, idxLinkBy = -1;
    if (c->typeId == "V_SQUARE") {
        for (int i = 0; i < (int)td->params.size(); ++i) {
            const std::string& nm = td->params[i].name;
            if      (nm.rfind("freq",   0) == 0) idxFreq   = i;
            else if (nm.rfind("tdelay", 0) == 0) idxTDelay = i;
            else if (nm.rfind("phase",  0) == 0) idxPhase  = i;
            else if (nm == "_linkBy")            idxLinkBy = i;
        }
    }

    // V_SQUARE phase ↔ tdelay linkage: updating one recomputes the other, and
    // "_linkBy" remembers which the user touched so a later freq change knows
    // which to preserve. Applied per component so multi-edit stays consistent.
    auto applyLinkage = [&](SchematicComp& comp, int editedIdx) {
        if (idxFreq < 0 || idxTDelay < 0 || idxPhase < 0 || idxLinkBy < 0) return;
        if (editedIdx != idxFreq && editedIdx != idxTDelay && editedIdx != idxPhase) return;
        if ((int)comp.paramValues.size() <= idxLinkBy) return;

        double freq   = parseNum(comp.paramValues[idxFreq]);
        double tdelay = parseNum(comp.paramValues[idxTDelay]);
        double phase  = parseNum(comp.paramValues[idxPhase]);
        if (freq <= 0.0) return;
        auto fmt = [](double v) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.6g", v);
            return std::string(buf);
        };
        bool byPhase;
        if (editedIdx == idxPhase)       { byPhase = true;  comp.paramValues[idxLinkBy] = "phase"; }
        else if (editedIdx == idxTDelay) { byPhase = false; comp.paramValues[idxLinkBy] = "tdelay"; }
        else /* freq */                  { byPhase = (comp.paramValues[idxLinkBy] == "phase"); }

        if (byPhase) {
            tdelay = phase / 360.0 / freq;
            comp.paramValues[idxTDelay] = fmt(tdelay);
        } else {
            phase = std::fmod(tdelay * freq * 360.0, 360.0);
            if (phase >  180.0) phase -= 360.0;
            if (phase < -180.0) phase += 360.0;
            comp.paramValues[idxPhase] = fmt(phase);
        }
    };

    for (int i = 0; i < (int)td->params.size() && i < 8; ++i) {
        if (td->params[i].hidden) continue;     // skip internal book-keeping params
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputText(td->params[i].name.c_str(), propBufs_[i], sizeof(propBufs_[i]));
        // Live evaluation feedback: show the resolved value (or an error) for
        // expressions / variables while the field is being edited.
        if (ImGui::IsItemActive() || ImGui::IsItemHovered()) {
            double v;
            if (exprv::tryEval(propBufs_[i], &varMap, v))
                ImGui::SetTooltip("= %g", v);
            else if (propBufs_[i][0] != '\0')
                ImGui::SetTooltip("(not a numeric expression)");
        }
        if (ImGui::IsItemDeactivatedAfterEdit() && i < (int)c->paramValues.size()) {
            pushUndo(undoStack_, redoStack_, sch, kMaxUndo);
            for (SchematicComp* tc : targets) {
                if (i >= (int)tc->paramValues.size()) continue;
                tc->paramValues[i] = propBufs_[i];
                applyLinkage(*tc, i);
            }
            // Sync linked cells (tdelay/phase) of the reference comp back into
            // the visible buffers.
            for (int k = 0; k < (int)c->paramValues.size() && k < 8; ++k) {
                if (k == i) continue;
                strncpy(propBufs_[k], c->paramValues[k].c_str(), sizeof(propBufs_[k])-1);
                propBufs_[k][sizeof(propBufs_[k])-1] = '\0';
            }
        }
    }

    // Rotate button in the property panel as well
    ImGui::SameLine();
    if (ImGui::Button("Rotate 90°")) {
        pushUndo(undoStack_, redoStack_, sch, kMaxUndo);
        for (SchematicComp* tc : targets)
            tc->rotation = (tc->rotation + 1) % 4;
        propEditCompId_ = -1;  // force buffer refresh next frame
    }
}

// ── Variables editor window ─────────────────────────────────────────────────

void SchematicView::renderVariablesWindow(MainViewModel& vm) {
    if (!varsWindowOpen_) return;
    SchematicModel& sch = vm.schematic();

    ImGui::SetNextWindowSize({460, 280}, ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Schematic Variables", &varsWindowOpen_)) { ImGui::End(); return; }

    ImGui::TextDisabled("Usable in any numeric field: expressions (+ - * / parentheses),");
    ImGui::TextDisabled("suffixes f p n u m k Meg g. Later rows may reference earlier ones.");
    ImGui::Separator();

    auto isValidName = [](const std::string& s) {
        if (s.empty()) return false;
        if (!std::isalpha((unsigned char)s[0]) && s[0] != '_') return false;
        for (char ch : s)
            if (!std::isalnum((unsigned char)ch) && ch != '_') return false;
        return true;
    };

    auto& vars = sch.variables();
    exprv::VarMap partial;   // grows row by row so each row sees the ones above
    int removeIdx = -1;

    if (ImGui::BeginTable("##varsTable", 4,
                          ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Name",       ImGuiTableColumnFlags_WidthFixed, 110.f);
        ImGui::TableSetupColumn("Expression", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Value",      ImGuiTableColumnFlags_WidthFixed, 100.f);
        ImGui::TableSetupColumn("##del",      ImGuiTableColumnFlags_WidthFixed, 24.f);
        ImGui::TableHeadersRow();

        for (int i = 0; i < (int)vars.size(); ++i) {
            ImGui::PushID(i);
            ImGui::TableNextRow();

            char nameBuf[48], exprBuf[128];
            strncpy(nameBuf, vars[i].name.c_str(), sizeof(nameBuf)-1);
            nameBuf[sizeof(nameBuf)-1] = '\0';
            strncpy(exprBuf, vars[i].expr.c_str(), sizeof(exprBuf)-1);
            exprBuf[sizeof(exprBuf)-1] = '\0';

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            bool nameOk = isValidName(vars[i].name);
            if (!nameOk) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f,0.4f,0.4f,1.f));
            if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf)))
                vars[i].name = nameBuf;
            if (!nameOk) ImGui::PopStyleColor();
            if (ImGui::IsItemDeactivatedAfterEdit())
                pushUndo(undoStack_, redoStack_, sch, kMaxUndo);

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##expr", exprBuf, sizeof(exprBuf)))
                vars[i].expr = exprBuf;
            if (ImGui::IsItemDeactivatedAfterEdit())
                pushUndo(undoStack_, redoStack_, sch, kMaxUndo);

            ImGui::TableNextColumn();
            {
                double v;
                if (isValidName(vars[i].name) && exprv::tryEval(vars[i].expr, &partial, v)) {
                    ImGui::Text("%g", v);
                    std::string key = vars[i].name;
                    std::transform(key.begin(), key.end(), key.begin(), ::toupper);
                    partial[key] = v;
                } else {
                    ImGui::TextColored(ImVec4(1.f,0.4f,0.4f,1.f), "error");
                }
            }

            ImGui::TableNextColumn();
            if (ImGui::SmallButton("X")) removeIdx = i;

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (removeIdx >= 0) {
        pushUndo(undoStack_, redoStack_, sch, kMaxUndo);
        vars.erase(vars.begin() + removeIdx);
    }

    if (ImGui::Button("+ Add variable")) {
        pushUndo(undoStack_, redoStack_, sch, kMaxUndo);
        vars.push_back({"var" + std::to_string(vars.size() + 1), "1"});
    }

    ImGui::End();
}

// ── Public File-action API ─────────────────────────────────────────────────
// These are invoked from the main-window menu bar. They mirror what the
// per-window toolbar buttons used to do; status messages still appear in the
// schematic toolbar (ioStatus_ / ioStatusTimer_).

void SchematicView::fileSave(MainViewModel& vm) {
#ifdef _WIN32
    char path[512] = {};
    if (!savedFilePath_.empty()) {
        strncpy(path, savedFilePath_.c_str(), sizeof(path)-1);
        doSave(path, vm);
    } else if (pickSavePath(path, sizeof(path))) {
        doSave(path, vm);
    }
#else
    (void)vm;
#endif
}

void SchematicView::fileSaveAs(MainViewModel& vm) {
#ifdef _WIN32
    char path[512] = {};
    if (pickSavePath(path, sizeof(path)))
        doSave(path, vm);
#else
    (void)vm;
#endif
}

void SchematicView::fileLoad(MainViewModel& vm) {
#ifdef _WIN32
    char path[512] = {};
    if (pickOpenPath(path, sizeof(path)))
        doLoad(path, vm);
#else
    (void)vm;
#endif
}

void SchematicView::fileExportSvg(MainViewModel& vm) {
#ifdef _WIN32
    char path[512] = {};
    if (pickSvgSavePath(path, sizeof(path))) {
        if (exportSchematicToSvgFile(vm.schematic(), path,
                                     static_cast<double>(svgExportScale_))) {
            std::snprintf(ioStatus_, sizeof(ioStatus_), "SVG exported");
        } else {
            std::snprintf(ioStatus_, sizeof(ioStatus_), "SVG export failed");
        }
        ioStatusTimer_ = 3.0f;
    }
#else
    (void)vm;
#endif
}

void SchematicView::fileCopyImg(MainViewModel& vm) {
#ifdef _WIN32
    if (copySchematicImageToClipboard(vm.schematic(),
                                      static_cast<double>(svgExportScale_))) {
        std::snprintf(ioStatus_, sizeof(ioStatus_), "Image copied to clipboard");
    } else {
        std::snprintf(ioStatus_, sizeof(ioStatus_), "Copy IMG failed");
    }
    ioStatusTimer_ = 3.0f;
#else
    (void)vm;
#endif
}
