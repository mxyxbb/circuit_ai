#include "views/schematic_view.h"
#include "view_model/main_view_model.h"
#include "view_model/schematic_model.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <cmath>
#include <cstring>
#include <algorithm>

SchematicView::SchematicView() : BaseView("Schematic") {}

// ── Polarity helper ─────────────────────────────────────────────────────────

const char* SchematicView::polaritySymbol(const std::string& pinLabel) {
    if (pinLabel == "P" || pinLabel == "A") return "+";
    if (pinLabel == "N" || pinLabel == "K") return "\xe2\x88\x92"; // unicode minus
    return nullptr;
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
        case 1: return {  y, -x };   // 90° CW
        case 2: return { -x, -y };   // 180°
        case 3: return { -y,  x };   // 270° CW
        default:return {  x,  y };   // 0°
    }
}

// Canvas position of pin pi on comp, accounting for rotation
ImVec2 SchematicView::pinCanvasPos(const SchematicComp& comp, int pi) {
    const CompTypeDef* td = SchematicModel::findCompType(comp.typeId);
    if (!td || pi >= (int)td->pins.size()) return comp.pos;
    ImVec2 roff = rotateOff(td->pins[pi].offset, comp.rotation);
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
    if (ImGui::Button("Build & Run")) vm.buildFromSchematic();
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        sch.clear();
        selectedCompId_ = selectedWireId_ = propEditCompId_ = movingCompId_ = -1;
        wiringActive_ = false;
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
    ImGui::TextDisabled("|");
    ImGui::SameLine();
    if (wiringActive_) {
        ImGui::TextColored({0.3f,1.0f,0.5f,1.0f}, "[WIRING — click pin to finish / click canvas for waypoint / Esc cancel]");
    } else {
        ImGui::TextDisabled("LClick=sel/wire  R=rotate  RDrag=pan  Scroll=zoom  Del=delete");
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

    // ── R key: rotate selected component ──────────────────────────────────
    if (selectedCompId_ != -1 && ImGui::IsKeyPressed(ImGuiKey_R)) {
        SchematicComp* c = sch.findComp(selectedCompId_);
        if (c) {
            c->rotation = (c->rotation + 1) % 4;
            propEditCompId_ = -1;  // force prop buffer refresh
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
                    selectedCompId_  = comp.id;
                    movingCompId_    = comp.id;
                    moveStartCanvas_ = mousePt;
                    moveCompOrigPos_ = comp.pos;
                    wiringActive_    = false;
                    wireWaypoints_.clear();
                    // Refresh property buffers if needed
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
                // Test all segments: fromPin → waypoints → toPin
                std::vector<ImVec2> path;
                path.push_back(pa);
                for (const auto& wp : wire.waypoints) path.push_back(wp);
                path.push_back(pb);
                for (size_t i = 1; i < path.size(); ++i) {
                    float d = distPointToSegment(mousePt, path[i-1], path[i]);
                    if (d < bestDist) {
                        bestDist = d;
                        bestWireId = wire.id;
                    }
                }
            }
            if (bestWireId != -1) {
                selectedWireId_  = bestWireId;
                selectedCompId_  = -1;
                propEditCompId_  = -1;
            } else {
                selectedWireId_  = -1;
                selectedCompId_  = -1;
                propEditCompId_  = -1;
                wiringActive_    = false;
                wireWaypoints_.clear();
            }
        } else if (hitBody) {
            selectedWireId_ = -1;  // component selected → clear wire selection
        }
    }

    // ── Drag to move ───────────────────────────────────────────────────────
    if (movingCompId_ != -1) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float dx = mousePt.x - moveStartCanvas_.x;
            float dy = mousePt.y - moveStartCanvas_.y;
            SchematicComp* c = sch.findComp(movingCompId_);
            if (c) c->pos = snapGrid({ moveCompOrigPos_.x + dx, moveCompOrigPos_.y + dy });
        } else {
            movingCompId_ = -1;
        }
    }

    // ── Delete ─────────────────────────────────────────────────────────────
    if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
        if (selectedCompId_ != -1) {
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

// ── Component drawing ──────────────────────────────────────────────────────

void SchematicView::drawComponents(ImDrawList* dl, MainViewModel& vm, ImVec2 origin) {
    const SchematicModel& sch = vm.schematic();

    for (const auto& comp : sch.comps()) {
        const CompTypeDef* td = SchematicModel::findCompType(comp.typeId);
        if (!td) continue;
        bool sel = (comp.id == selectedCompId_);
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

        // ── Generic box body (body half-size swaps on 90°/270°) ───────────
        float bx = td->bodyHalfSize.x, by = td->bodyHalfSize.y;
        if (comp.rotation % 2 == 1) std::swap(bx, by);
        float bxs = bx * zoom_, bys = by * zoom_;
        ImVec2 tl = {ctr.x-bxs, ctr.y-bys}, br = {ctr.x+bxs, ctr.y+bys};

        ImU32 bodyFill   = IM_COL32(40, 70, 110, 255);
        ImU32 bodyBorder = sel ? IM_COL32(255,210,50,255) : IM_COL32(110,155,220,255);
        float thick      = sel ? 2.0f*zoom_ : 1.2f*zoom_;
        dl->AddRectFilled(tl, br, bodyFill, 3.0f*zoom_);
        dl->AddRect(tl, br, bodyBorder, 3.0f*zoom_, 0, thick);

        // Type abbreviation centred in body
        const char* abbr = td->id.c_str();
        if (comp.typeId=="V_DC"||comp.typeId=="V_SQUARE"||
            comp.typeId=="V_SIN"||comp.typeId=="V_STEP") abbr="V";
        ImVec2 ts = ImGui::CalcTextSize(abbr);
        dl->AddText({ctr.x-ts.x*.5f, ctr.y-ts.y*.5f}, IM_COL32(200,225,255,255), abbr);

        // Rotation indicator: small arc/triangle for 90°/180°/270°
        if (comp.rotation != 0) {
            char rot_label[4];
            snprintf(rot_label, sizeof(rot_label), "%d°", comp.rotation * 90);
            ImVec2 rl = ImGui::CalcTextSize(rot_label);
            dl->AddText({ctr.x+bxs-rl.x-2*zoom_, ctr.y-bys+1*zoom_},
                        IM_COL32(160,160,80,200), rot_label);
        }

        // Instance name (+ first param) below/beside body
        char lbl[80];
        if (!comp.paramValues.empty())
            snprintf(lbl, sizeof(lbl), "%s=%s", comp.instanceName.c_str(), comp.paramValues[0].c_str());
        else
            snprintf(lbl, sizeof(lbl), "%s", comp.instanceName.c_str());
        ImVec2 ls = ImGui::CalcTextSize(lbl);
        dl->AddText({ctr.x-ls.x*.5f, ctr.y+bys+2*zoom_}, IM_COL32(170,200,170,255), lbl);

        // ── Pins (rotated positions) ──────────────────────────────────────
        for (int pi = 0; pi < (int)td->pins.size(); ++pi) {
            ImVec2 roff     = rotateOff(td->pins[pi].offset, comp.rotation);
            ImVec2 pinCanvas= {comp.pos.x+roff.x, comp.pos.y+roff.y};
            ImVec2 pinScr   = c2s(pinCanvas, origin);

            // Wire stub: from body edge to pin
            float dx = roff.x, dy = roff.y;
            float t  = 1.0f;
            if (fabsf(dx) > 0.0f) t = std::min(t, bx / fabsf(dx));
            if (fabsf(dy) > 0.0f) t = std::min(t, by / fabsf(dy));
            ImVec2 edgeScr = c2s({comp.pos.x+dx*t, comp.pos.y+dy*t}, origin);
            dl->AddLine(edgeScr, pinScr, IM_COL32(100,140,190,200), 1.2f*zoom_);

            // Pin circle
            bool isStart = (wiringActive_ && wireFromCompId_==comp.id && wireFromPinIdx_==pi);
            dl->AddCircleFilled(pinScr, 4.0f*zoom_,
                                isStart ? IM_COL32(50,255,100,255) : IM_COL32(80,200,120,255));

            // Polarity label (+ or −)
            const char* polSym = polaritySymbol(td->pins[pi].label);
            if (polSym) {
                ImVec2 pinOff = td->pins[pi].offset;  // unrotated
                float len = sqrtf(pinOff.x*pinOff.x + pinOff.y*pinOff.y);
                if (len > 1.0f) {
                    ImVec2 dir = { pinOff.x / len, pinOff.y / len };
                    ImVec2 labelOff = { pinOff.x + dir.x * 12.0f, pinOff.y + dir.y * 12.0f };
                    ImVec2 labelRotOff = rotateOff(labelOff, comp.rotation);
                    ImVec2 labelCanvasPos = { comp.pos.x + labelRotOff.x, comp.pos.y + labelRotOff.y };
                    ImVec2 labelScr = c2s(labelCanvasPos, origin);
                    ImVec2 ts = ImGui::CalcTextSize(polSym);
                    bool isPlus = (td->pins[pi].label == "P" || td->pins[pi].label == "A");
                    ImU32 polCol = isPlus ? IM_COL32(255,120,120,255) : IM_COL32(120,120,255,255);
                    dl->AddText({labelScr.x - ts.x*0.5f, labelScr.y - ts.y*0.5f}, polCol, polSym);
                }
            }
        }
    }
}

// ── Rubber-band wire ────────────────────────────────────────────────────────

void SchematicView::drawRubberBand(ImDrawList* dl, MainViewModel& vm, ImVec2 origin) const {
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
