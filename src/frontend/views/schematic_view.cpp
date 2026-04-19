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
    if (wiringActive_) {
        ImGui::TextColored({0.3f,1.0f,0.5f,1.0f}, "[WIRING — click pin to finish / click canvas for waypoint / Esc cancel]");
    } else {
        ImGui::TextDisabled("LClick=sel/wire  R=rotate  X=mirror  Ctrl+C=copy  RDrag=pan  Scroll=zoom  Del=delete  LDrag(empty)=multisel  Ctrl+LClick=add sel");
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
            sch.addComp(typeId, dropCanvas);
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
                ImVec2 newPos = snapGrid({src->pos.x + 40.f, src->pos.y + 40.f});
                int newId = sch.addComp(src->typeId, newPos);
                SchematicComp* dst = sch.findComp(newId);
                if (dst) {
                    dst->rotation    = src->rotation;
                    dst->mirrorX     = src->mirrorX;
                    dst->paramValues = src->paramValues;
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

    // ── Left-click ────────────────────────────────────────────────────────
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
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

        // During wiring: canvas click adds a waypoint
        if (!hitPin && wiringActive_) {
            wireWaypoints_.push_back(snapGrid(mousePt));
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
    for (const auto& wire : sch.wires()) {
        const SchematicComp* ca = sch.findComp(wire.fromCompId);
        const SchematicComp* cb = sch.findComp(wire.toCompId);
        if (!ca || !cb) continue;
        bool wireSel = (wire.id == selectedWireId_);
        ImU32 col = wireSel ? IM_COL32(255, 255, 100, 255) : wireCol;
        float thick = wireSel ? 3.0f * zoom_ : 1.5f * zoom_;

        ImVec2 prevScr = c2s(pinCanvasPos(*ca, wire.fromPinIdx), origin);
        for (const auto& wp : wire.waypoints) {
            ImVec2 wpScr = c2s(wp, origin);
            dl->AddLine(prevScr, wpScr, col, thick);
            prevScr = wpScr;
        }
        ImVec2 toScr = c2s(pinCanvasPos(*cb, wire.toPinIdx), origin);
        dl->AddLine(prevScr, toScr, col, thick);
    }
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
    // ── Switch: G/GRef on left, D/S vertically on right ───────────────────
    else if (id == "S") {
        // Gate terminal (left) — G at pin(-40,-20), GRef at pin(-40,+20)
        dl->AddLine(sc(-40,-20), sc( -5,-20), col, thick);   // G lead
        dl->AddLine(sc(-40,+20), sc( -5,+20), col, thick);   // GRef lead
        dl->AddLine(sc( -5,-24), sc( -5,+24), col, thick);   // gate bar
        dl->AddLine(sc( -5,  0), sc( +5,  0), col, thick);   // control line
        // D-S contacts (right) — D at pin(+40,-20), S at pin(+40,+20)
        dl->AddLine(sc(+40,-20), sc(+16,-20), col, thick);   // D lead
        dl->AddLine(sc(+40,+20), sc(+16,+20), col, thick);   // S lead
        dl->AddCircleFilled(sc(+16,-20), 3.f*z, col);         // D contact dot
        dl->AddCircleFilled(sc(+16,+20), 3.f*z, col);         // S contact dot
        dl->AddLine(sc(+16,-20), sc(+8,+8), col, thick);      // open switch arm

        // Small pin-label letters near each pin endpoint (outward from body)
        ImU32 lblCol = sel ? IM_COL32(255,230,100,220) : IM_COL32(160,200,255,200);
        float lsz    = 12.0f;
        auto addPinLabel = [&](float ox, float oy, const char* text) {
            ImVec2 ps = sc(ox, oy);
            float dx = ps.x - ctr.x, dy = ps.y - ctr.y;
            float len = sqrtf(dx*dx + dy*dy);
            if (len < 1.f) return;
            // Offset label outward from body center
            float nx = dx/len, ny = dy/len;
            ImVec2 ts = ImGui::CalcTextSize(text);
            dl->AddText(nullptr, lsz,
                {ps.x + nx*4.f - ts.x*0.5f, ps.y + ny*4.f - ts.y*0.5f},
                lblCol, text);
        };
        float lbxos = 20.f;
        addPinLabel(-40+lbxos,-20+lsz, "G");
        addPinLabel(-40+lbxos,+20-lsz, "GRef");
        addPinLabel(+40-lbxos,-20+lsz, "D");
        addPinLabel(+40-lbxos,+20-lsz, "S");
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

        // ── Standard circuit symbol ───────────────────────────────────────
        drawCompSymbol(dl, comp, *td, ctr, sel);

        // Body half-size kept for label placement only (bodyHalfSize unchanged)
        float bx = td->bodyHalfSize.x, by = td->bodyHalfSize.y;
        if (comp.rotation % 2 == 1) std::swap(bx, by);
        float bxs = bx * zoom_, bys = by * zoom_;
        (void)bxs;

        // Instance name (+ first param) below/beside body
        char lbl[80];
        if (!comp.paramValues.empty())
            snprintf(lbl, sizeof(lbl), "%s=%s", comp.instanceName.c_str(), comp.paramValues[0].c_str());
        else
            snprintf(lbl, sizeof(lbl), "%s", comp.instanceName.c_str());
        ImVec2 ls = ImGui::CalcTextSize(lbl);
        dl->AddText({ctr.x-ls.x*.5f, ctr.y+bys+2*zoom_}, IM_COL32(170,200,170,255), lbl);

        // ── Pins (mirrorX + rotated positions) ───────────────────────────
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
