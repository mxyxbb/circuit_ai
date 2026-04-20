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
#include "view_model/main_view_model.h"
#include "view_model/schematic_model.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <unordered_map>

// ── Win32 file dialog helpers ──────────────────────────────────────────────
#ifdef _WIN32
static bool pickOpenPath(char* buf, int n) {
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = "CircuitAI Schematic\0*.sch\0All Files\0*.*\0\0";
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = static_cast<DWORD>(n);
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
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
    ofn.Flags       = OFN_OVERWRITEPROMPT;
    ofn.lpstrDefExt = "sch";
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

// ── Main render ────────────────────────────────────────────────────────────

void SchematicView::render(MainViewModel& vm) {
    if (!visible_) return;
    if (!ImGui::Begin(title_.c_str(), &visible_)) {
        ImGui::End();
        return;
    }

    SchematicModel& sch = vm.schematic();

    // ── Toolbar ────────────────────────────────────────────────────────────
    if (ioStatusTimer_ > 0.f) ioStatusTimer_ -= ImGui::GetIO().DeltaTime;

    if (ImGui::Button("Build & Run")) vm.buildFromSchematic();
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        sch.clear();
        selectedCompId_ = selectedWireId_ = propEditCompId_ = movingCompId_ = -1;
        multiSelectedIds_.clear(); multiMoveOrigPos_.clear(); selBoxActive_ = false;
        wiringActive_ = false;
    }
    ImGui::SameLine();
#ifdef _WIN32
    if (ImGui::Button("Save")) {
        char path[512] = {};
        if (pickSavePath(path, sizeof(path))) {
            bool ok = sch.saveToFile(path);
            std::snprintf(ioStatus_, sizeof(ioStatus_), ok ? "Saved." : "Save failed!");
            ioStatusTimer_ = 2.5f;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        char path[512] = {};
        if (pickOpenPath(path, sizeof(path))) {
            bool ok = sch.loadFromFile(path);
            if (ok) {
                selectedCompId_ = selectedWireId_ = propEditCompId_ = movingCompId_ = -1;
                multiSelectedIds_.clear(); multiMoveOrigPos_.clear(); selBoxActive_ = false;
                wiringActive_ = false;
            }
            std::snprintf(ioStatus_, sizeof(ioStatus_), ok ? "Loaded." : "Load failed!");
            ioStatusTimer_ = 2.5f;
        }
    }
    if (ioStatusTimer_ > 0.f) {
        ImGui::SameLine();
        bool fail = (std::strstr(ioStatus_, "failed") != nullptr);
        ImGui::TextColored(fail ? ImVec4(1.f,0.35f,0.35f,1.f)
                                : ImVec4(0.3f,1.f,0.45f,1.f), "%s", ioStatus_);
    }
    ImGui::SameLine();
#endif
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
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    // Probe buttons
    {
        bool vActive = (probeMode_ == PM_VProbe);
        bool iActive = (probeMode_ == PM_IProbe);
        if (vActive) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f,0.6f,0.2f,1.0f));
        if (ImGui::SmallButton("V-Probe")) probeMode_ = vActive ? PM_None : PM_VProbe;
        if (vActive) ImGui::PopStyleColor();
        ImGui::SameLine();
        if (iActive) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f,0.6f,0.2f,1.0f));
        if (ImGui::SmallButton("I-Probe")) probeMode_ = iActive ? PM_None : PM_IProbe;
        if (iActive) ImGui::PopStyleColor();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (wiringActive_) {
        ImGui::TextColored({0.3f,1.0f,0.5f,1.0f}, "[WIRING — click pin to finish / click canvas for waypoint / Esc cancel]");
    } else if (probeMode_ == PM_VProbe) {
        ImGui::TextColored({0.3f,1.0f,0.3f,1.0f}, "[V-PROBE — click a wire to add voltage to selected Scope plot]");
    } else if (probeMode_ == PM_IProbe) {
        ImGui::TextColored({0.3f,1.0f,0.3f,1.0f}, "[I-PROBE — click a pin to add its current to selected Scope plot]");
    } else {
        ImGui::TextDisabled("LClick=sel/wire  R=rotate  X=mirror  Ctrl+C=copy  RDrag=pan  Scroll=zoom  Del=delete  LDrag(empty)=multisel  Ctrl+LClick=add sel  DblClick wire=net name");
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
                sch.addComp(typeId, dropCanvas);
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

    ImGui::EndChild();

    renderProperties(vm);

    // ── Wire net name edit popup ─────────────────────────────────────────
    if (editNetWireId_ >= 0) ImGui::OpenPopup("Net Name##wireDlg");
    if (ImGui::BeginPopupModal("Net Name##wireDlg", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Net name (empty = auto-number):");
        ImGui::SetNextItemWidth(200.f);
        bool commit = ImGui::InputText("##netname", editNetNameBuf_, sizeof(editNetNameBuf_),
                                       ImGuiInputTextFlags_EnterReturnsTrue);
        if (commit || ImGui::Button("OK")) {
            SchematicWire* ew = vm.schematic().findWire(editNetWireId_);
            if (ew) ew->netName = editNetNameBuf_;
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

    // ── Escape: cancel wiring ──────────────────────────────────────────────
    if (wiringActive_ && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        wiringActive_ = false; wireFromCompId_ = -1; wireWaypoints_.clear();
    }

    // ── R key: rotate selected component(s) ───────────────────────────────
    if (ImGui::IsKeyPressed(ImGuiKey_R)) {
        auto targets = multiSelectedIds_.empty()
            ? std::vector<int>{selectedCompId_} : multiSelectedIds_;
        for (int cid : targets) {
            SchematicComp* c = sch.findComp(cid);
            if (c) c->rotation = (c->rotation + 1) % 4;
        }
        propEditCompId_ = -1;
    }

    // ── X key: mirror selected component(s) ───────────────────────────────
    if (ImGui::IsKeyPressed(ImGuiKey_X)) {
        auto targets = multiSelectedIds_.empty()
            ? std::vector<int>{selectedCompId_} : multiSelectedIds_;
        for (int cid : targets) {
            SchematicComp* c = sch.findComp(cid);
            if (c) c->mirrorX = !c->mirrorX;
        }
        propEditCompId_ = -1;
    }

    // ── Ctrl+C: copy selected component(s) ────────────────────────────────
    if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C)) {
        std::vector<int> toCopy = multiSelectedIds_.empty()
            ? std::vector<int>{selectedCompId_} : multiSelectedIds_;
        toCopy.erase(std::remove(toCopy.begin(), toCopy.end(), -1), toCopy.end());
        if (!toCopy.empty()) {
            std::unordered_map<int,int> idMap;
            std::vector<int> newIds;
            for (int cid : toCopy) {
                SchematicComp* src = sch.findComp(cid);
                if (!src) continue;
                // Save before addComp — vector reallocation invalidates src pointer
                std::string srcTypeId   = src->typeId;
                ImVec2      srcPos      = src->pos;
                int         srcRot      = src->rotation;
                bool        srcMirrorX  = src->mirrorX;
                auto        srcParams   = src->paramValues;
                ImVec2 newPos = snapGrid({srcPos.x + 40.f, srcPos.y + 40.f});
                int newId = sch.addComp(srcTypeId, newPos);
                SchematicComp* dst = sch.findComp(newId);
                if (dst) {
                    dst->rotation    = srcRot;
                    dst->mirrorX     = srcMirrorX;
                    dst->paramValues = srcParams;
                }
                idMap[cid] = newId;
                newIds.push_back(newId);
            }
            // Copy wires connecting two copied components
            auto wireSnap = sch.wires();
            for (const auto& w : wireSnap) {
                if (idMap.count(w.fromCompId) && idMap.count(w.toCompId))
                    sch.addWire(idMap[w.fromCompId], w.fromPinIdx,
                                idMap[w.toCompId],   w.toPinIdx, w.waypoints);
            }
            if (newIds.size() == 1) {
                selectedCompId_  = newIds[0];
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

    // ── Probe mode left-click ─────────────────────────────────────────────
    if (hovered && probeMode_ != PM_None && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ScopeModel& scope = vm.scope();
        int selPlot = scope.selectedPlot();
        if (probeMode_ == PM_VProbe) {
            // Find nearest wire or node, determine its net number
            float wireHitR = 10.0f / zoom_;
            int nearWireFrom = -1, nearWireFromPin = -1;
            float bestD = wireHitR;
            for (const auto& wire : sch.wires()) {
                const SchematicComp* ca = sch.findComp(wire.fromCompId);
                const SchematicComp* cb = sch.findComp(wire.toCompId);
                if (!ca || !cb) continue;
                std::vector<ImVec2> path;
                path.push_back(pinCanvasPos(*ca, wire.fromPinIdx));
                for (const auto& wp : wire.waypoints) path.push_back(wp);
                path.push_back(pinCanvasPos(*cb, wire.toPinIdx));
                for (size_t i=1;i<path.size();++i) {
                    float d = distPointToSegment(mousePt, path[i-1], path[i]);
                    if (d < bestD) {
                        bestD = d;
                        nearWireFrom    = wire.fromCompId;
                        nearWireFromPin = wire.fromPinIdx;
                    }
                }
            }
            if (nearWireFrom >= 0) {
                auto nodeMap = sch.computePinNodeMap();
                auto it = nodeMap.find(SchematicModel::pinKey(nearWireFrom, nearWireFromPin));
                if (it != nodeMap.end() && it->second != 0) {
                    char sigName[32];
                    std::snprintf(sigName, sizeof(sigName), "V(%d)", it->second);
                    // Check signal exists
                    for (const auto& si : vm.availableSignals()) {
                        if (si.name == sigName) {
                            scope.addSignalToPlot(selPlot, si.name, 0);
                            break;
                        }
                    }
                }
            }
        } else if (probeMode_ == PM_IProbe) {
            // Find nearest pin
            float hitR2 = (12.0f / zoom_) * (12.0f / zoom_);
            for (const auto& comp : sch.comps()) {
                const CompTypeDef* td = SchematicModel::findCompType(comp.typeId);
                if (!td) continue;
                for (int pi=0; pi<(int)td->pins.size(); ++pi) {
                    ImVec2 pPos = pinCanvasPos(comp, pi);
                    float dx=mousePt.x-pPos.x, dy=mousePt.y-pPos.y;
                    if (dx*dx+dy*dy <= hitR2) {
                        char sigName[64];
                        std::snprintf(sigName, sizeof(sigName), "I(%s)", comp.instanceName.c_str());
                        for (const auto& si : vm.availableSignals()) {
                            if (si.name == sigName) {
                                scope.addSignalToPlot(selPlot, si.name, 0);
                                break;
                            }
                        }
                        goto probeHandled;
                    }
                }
            }
            probeHandled:;
        }
        probeMode_ = PM_None;
    }

    // ── Left-click ────────────────────────────────────────────────────────
    if (hovered && probeMode_ == PM_None && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        bool hitPin = false, hitBody = false;

        // Priority 1: pins (use rotated positions)
        for (auto& comp : sch.comps()) {
            const CompTypeDef* td = SchematicModel::findCompType(comp.typeId);
            if (!td) continue;
            for (int pi = 0; pi < (int)td->pins.size(); ++pi) {
                ImVec2 pPos = pinCanvasPos(comp, pi);
                float dx = mousePt.x - pPos.x, dy = mousePt.y - pPos.y;
                if (dx*dx + dy*dy <= hitR*hitR) {
                    if (wiringActive_) {
                        if (comp.id != wireFromCompId_ || pi != wireFromPinIdx_)
                            sch.addWire(wireFromCompId_, wireFromPinIdx_, comp.id, pi, wireWaypoints_);
                        wiringActive_ = false; wireFromCompId_ = -1; wireWaypoints_.clear();
                    } else {
                        wiringActive_   = true;
                        wireFromCompId_ = comp.id;
                        wireFromPinIdx_ = pi;
                        wireWaypoints_.clear();
                    }
                    hitPin = true;
                    break;
                }
            }
            if (hitPin) break;
        }

        // During wiring: check wire-to-wire hit, otherwise add waypoint
        if (!hitPin && wiringActive_) {
            float wireHitR2 = (8.0f / zoom_) * (8.0f / zoom_);
            int hitWireId = -1;
            ImVec2 hitWireSnap = snapGrid(mousePt);
            for (auto& w : sch.wires()) {
                // Build full path including waypoints
                SchematicComp* fc = sch.findComp(w.fromCompId);
                SchematicComp* tc = sch.findComp(w.toCompId);
                if (!fc || !tc) continue;
                const CompTypeDef* ftd = SchematicModel::findCompType(fc->typeId);
                const CompTypeDef* ttd = SchematicModel::findCompType(tc->typeId);
                if (!ftd || !ttd) continue;
                ImVec2 fPos = pinCanvasPos(*fc, w.fromPinIdx);
                ImVec2 tPos = pinCanvasPos(*tc, w.toPinIdx);
                std::vector<ImVec2> path;
                path.push_back(fPos);
                for (auto& wp : w.waypoints) path.push_back(wp);
                path.push_back(tPos);
                for (int si = 0; si + 1 < (int)path.size(); ++si) {
                    float d = distPointToSegment(mousePt, path[si], path[si+1]);
                    if (d*d <= wireHitR2) {
                        hitWireId = w.id;
                        // Snap junction to nearest point on segment
                        ImVec2 a = path[si], b = path[si+1];
                        ImVec2 ab = {b.x-a.x, b.y-a.y};
                        float len2 = ab.x*ab.x + ab.y*ab.y;
                        if (len2 > 0.f) {
                            float t = ((mousePt.x-a.x)*ab.x + (mousePt.y-a.y)*ab.y) / len2;
                            t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
                            hitWireSnap = snapGrid({a.x + t*ab.x, a.y + t*ab.y});
                        }
                        break;
                    }
                }
                if (hitWireId >= 0) break;
            }
            if (hitWireId >= 0) {
                int juncId = insertJunctionOnWire(sch, hitWireId, hitWireSnap);
                if (juncId >= 0) {
                    sch.addWire(wireFromCompId_, wireFromPinIdx_, juncId, 0, wireWaypoints_);
                    wiringActive_ = false; wireFromCompId_ = -1; wireWaypoints_.clear();
                }
            } else {
                wireWaypoints_.push_back(snapGrid(mousePt));
            }
        }

        // Priority 2: component bodies (only when not wiring)
        if (!hitPin && !wiringActive_) {
            for (auto& comp : sch.comps()) {
                const CompTypeDef* td = SchematicModel::findCompType(comp.typeId);
                if (!td) continue;
                // Body half-size swaps on 90°/270°
                float bx = td->bodyHalfSize.x, by = td->bodyHalfSize.y;
                if (comp.rotation % 2 == 1) std::swap(bx, by);
                if (mousePt.x >= comp.pos.x - bx && mousePt.x <= comp.pos.x + bx &&
                    mousePt.y >= comp.pos.y - by && mousePt.y <= comp.pos.y + by) {
                    wiringActive_ = false;
                    wireWaypoints_.clear();

                    if (ImGui::GetIO().KeyCtrl) {
                        // Ctrl+click: toggle in multi-select without starting move
                        auto it = std::find(multiSelectedIds_.begin(), multiSelectedIds_.end(), comp.id);
                        if (it != multiSelectedIds_.end()) {
                            multiSelectedIds_.erase(it);
                            if (selectedCompId_ == comp.id)
                                selectedCompId_ = multiSelectedIds_.empty() ? -1 : multiSelectedIds_[0];
                        } else {
                            multiSelectedIds_.push_back(comp.id);
                            selectedCompId_ = comp.id;
                        }
                        propEditCompId_ = -1;
                    } else {
                        // Normal click: if already in multi-select, start multi-move
                        bool inMulti = !multiSelectedIds_.empty() &&
                            std::find(multiSelectedIds_.begin(), multiSelectedIds_.end(), comp.id)
                                != multiSelectedIds_.end();
                        if (!inMulti) {
                            // Single-select this component
                            multiSelectedIds_.clear();
                            selectedCompId_ = comp.id;
                        }
                        movingCompId_    = comp.id;
                        moveStartCanvas_ = mousePt;
                        moveCompOrigPos_ = comp.pos;
                        // Store original positions of all selected for multi-move
                        multiMoveOrigPos_.clear();
                        auto& toMove = inMulti ? multiSelectedIds_ : std::vector<int>{comp.id};
                        for (int cid : toMove) {
                            SchematicComp* mc = sch.findComp(cid);
                            if (mc) multiMoveOrigPos_.push_back({cid, mc->pos});
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
                    if (d < bestDist) { bestDist = d; bestWireId = wire.id; }
                }
            }
            if (bestWireId != -1) {
                selectedWireId_   = bestWireId;
                selectedCompId_   = -1;
                multiSelectedIds_.clear();
                propEditCompId_   = -1;
                // Double-click on wire → open net name editor
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    editNetWireId_ = bestWireId;
                    SchematicWire* ew = sch.findWire(bestWireId);
                    strncpy(editNetNameBuf_, ew ? ew->netName.c_str() : "",
                            sizeof(editNetNameBuf_)-1);
                    editNetNameBuf_[sizeof(editNetNameBuf_)-1] = '\0';
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
        } else {
            movingCompId_ = -1;
            multiMoveOrigPos_.clear();
        }
    }

    // ── Delete ─────────────────────────────────────────────────────────────
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        if (!multiSelectedIds_.empty()) {
            for (int cid : multiSelectedIds_) sch.removeComp(cid);
            multiSelectedIds_.clear();
            selectedCompId_ = propEditCompId_ = movingCompId_ = -1;
            multiMoveOrigPos_.clear();
            wiringActive_ = false;
        } else if (selectedCompId_ != -1) {
            sch.removeComp(selectedCompId_);
            selectedCompId_ = propEditCompId_ = movingCompId_ = -1;
            wiringActive_ = false;
        } else if (selectedWireId_ != -1) {
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

    // Determine which net to highlight (from scope legend hover)
    const std::string& hovSig = vm.hoveredSignal();
    int hovNet = -1;
    std::unordered_map<int,int> nodeMap;
    if (hovSig.size() > 2 && hovSig[0]=='V' && hovSig[1]=='(') {
        try { hovNet = std::stoi(hovSig.substr(2, hovSig.size()-3)); } catch(...) {}
        if (hovNet >= 0) nodeMap = sch.computePinNodeMap();
    }

    for (const auto& wire : sch.wires()) {
        const SchematicComp* ca = sch.findComp(wire.fromCompId);
        const SchematicComp* cb = sch.findComp(wire.toCompId);
        if (!ca || !cb) continue;
        bool wireSel = (wire.id == selectedWireId_);

        // Highlight if wire is in the hovered net
        bool wireHighlight = false;
        if (hovNet >= 0 && !nodeMap.empty()) {
            auto it = nodeMap.find(SchematicModel::pinKey(wire.fromCompId, wire.fromPinIdx));
            wireHighlight = (it != nodeMap.end() && it->second == hovNet);
        }

        ImU32 col = wireSel        ? IM_COL32(255, 255, 100, 255) :
                    wireHighlight  ? IM_COL32(255, 160,  50, 255) : wireCol;
        float thick = (wireSel || wireHighlight) ? 3.0f * zoom_ : 1.5f * zoom_;

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
        // Dashed bar on the "flat" side (-x before mirrorX), ±24 along winding axis
        ImVec2 top = windSC(*wc, -14.f, -24.f);
        ImVec2 bot = windSC(*wc, -14.f, +24.f);
        ImVec2 mid = windSC(*wc, -14.f,   0.f);
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
        dl->AddLine(sc(-40,0), sc(-5,0),  col, thick);
        dl->AddLine(sc(+5,0),  sc(+40,0), col, thick);
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
        dl->AddLine({ctr.x + 8*z, ctr.y}, {ctr.x - 8*z, ctr.y}, col, thick);
        dl->AddTriangleFilled({ctr.x - 8*z, ctr.y},
                              {ctr.x - 4*z, ctr.y - 4*z},
                              {ctr.x - 4*z, ctr.y + 4*z}, col);
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
        dl->AddLine(sc(-20,0), sc(0,0), nc, thick);  // lead
        // Flag pentagon
        dl->PathLineTo(sc(0,-7)); dl->PathLineTo(sc(+12,-7));
        dl->PathLineTo(sc(+16,0)); dl->PathLineTo(sc(+12,+7));
        dl->PathLineTo(sc(0,+7));
        dl->PathStroke(nc, ImDrawFlags_Closed, thick*0.7f);
        // Label text
        if (!comp.paramValues.empty()) {
            ImVec2 lc = sc(+8,0);
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
        // Polarity dot at P-side, on the flat/core side (-x before mirrorX)
        { ImU32 dc=sel?IM_COL32(255,230,100,255):IM_COL32(220,230,255,240);
          dl->AddCircleFilled(sc(-10,-22),2.5f*z,dc); }
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

    for (const auto& comp : sch.comps()) {
        const CompTypeDef* td = SchematicModel::findCompType(comp.typeId);
        if (!td) continue;
        bool sel = (comp.id == selectedCompId_) ||
                   (!multiSelectedIds_.empty() &&
                    std::find(multiSelectedIds_.begin(), multiSelectedIds_.end(), comp.id)
                        != multiSelectedIds_.end());
        // Highlight component if scope legend hovers its current signal
        const std::string& hovSig = vm.hoveredSignal();
        if (!hovSig.empty() && hovSig.size()>2 && hovSig[0]=='I' && hovSig[1]=='(') {
            std::string hovComp = hovSig.substr(2, hovSig.size()-3);
            if (comp.instanceName == hovComp) sel = true;
        }
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

            // Pin circle
            bool isStart = (wiringActive_ && wireFromCompId_==comp.id && wireFromPinIdx_==pi);
            dl->AddCircleFilled(pinScr, 4.0f*zoom_,
                                isStart ? IM_COL32(50,255,100,255) : IM_COL32(80,200,120,255));

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
    ImGui::TextDisabled("Properties: %s (%s)  |  Rotation: %d°  [R to rotate]",
                        c->instanceName.c_str(), td->displayName.c_str(),
                        c->rotation * 90);

    ImGui::SetNextItemWidth(100.0f);
    ImGui::InputText("Name##prop", propNameBuf_, sizeof(propNameBuf_));
    if (ImGui::IsItemDeactivatedAfterEdit()) c->instanceName = propNameBuf_;

    for (int i = 0; i < (int)td->params.size() && i < 8; ++i) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputText(td->params[i].name.c_str(), propBufs_[i], sizeof(propBufs_[i]));
        if (ImGui::IsItemDeactivatedAfterEdit() && i < (int)c->paramValues.size())
            c->paramValues[i] = propBufs_[i];
    }

    // Rotate button in the property panel as well
    ImGui::SameLine();
    if (ImGui::Button("Rotate 90°")) {
        c->rotation = (c->rotation + 1) % 4;
        propEditCompId_ = -1;  // force buffer refresh next frame
    }
}
