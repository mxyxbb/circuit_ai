#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <imgui.h>

// ── Pin definition within a component type ───────────────────────────────────
struct PinDef {
    std::string label;   // "P", "N", "D", "S", "G", "A", "K", ...
    ImVec2 offset;       // canvas-space offset from component centre (unrotated)
};

// ── Parameter definition for a component type ────────────────────────────────
struct ParamDef {
    std::string name;          // label shown in the property panel
    std::string defaultValue;  // initial value as string
};

// ── Static component type descriptor ─────────────────────────────────────────
struct CompTypeDef {
    std::string id;            // "R", "C", "L", "V_DC", ...
    std::string displayName;   // shown in PaletteView
    std::string prefix;        // SPICE name prefix for auto-numbering
    std::vector<PinDef>  pins;
    std::vector<ParamDef> params;
    ImVec2 bodyHalfSize;       // half extents for rendering and hit-test
};

// ── Placed component ──────────────────────────────────────────────────────────
struct SchematicComp {
    int  id = -1;
    std::string typeId;
    std::string instanceName;
    ImVec2 pos{0.0f, 0.0f};          // canvas position (centre)
    int  rotation = 0;               // 0/1/2/3 = 0°/90°/180°/270° CW
    bool mirrorX  = false;           // horizontal flip (applied before rotation)
    std::vector<std::string> paramValues; // by ParamDef index
};

// ── Wire between two component pins ──────────────────────────────────────────
struct SchematicWire {
    int id = -1;
    int fromCompId = -1, fromPinIdx = -1;
    int toCompId   = -1, toPinIdx   = -1;
    std::vector<ImVec2> waypoints;  // intermediate canvas-space points (routed path)
    std::string netName;            // user-assigned net label (empty = auto-numbered)
};

// ── Simulation parameters stored with the schematic ──────────────────────────
struct SchematicSimConfig {
    char dt[32]   = "1e-6";
    char tEnd[32] = "0.01";
};

// ── Top-level schematic model ─────────────────────────────────────────────────
class SchematicModel {
public:
    SchematicModel() = default;

    // Static registry (12 component types)
    static const std::vector<CompTypeDef>& compTypes();
    static const CompTypeDef* findCompType(const std::string& id);

    // Component CRUD
    int  addComp(const std::string& typeId, ImVec2 pos);
    void removeComp(int id);
    void removeWiresForComp(int compId);
    SchematicComp*       findComp(int id);
    const SchematicComp* findComp(int id) const;
    std::vector<SchematicComp>&       comps()       { return comps_; }
    const std::vector<SchematicComp>& comps() const { return comps_; }

    // Wire CRUD
    int  addWire(int fromCompId, int fromPinIdx, int toCompId, int toPinIdx,
                 const std::vector<ImVec2>& waypoints = {});
    void removeWire(int id);
    std::vector<SchematicWire>&       wires()       { return wires_; }
    const std::vector<SchematicWire>& wires() const { return wires_; }
    SchematicWire* findWire(int id) {
        for (auto& w : wires_) if (w.id == id) return &w;
        return nullptr;
    }

    // SPICE netlist generation — returns "" if schematic is empty
    std::string generateNetlist(const SchematicSimConfig& cfg) const;

    // Returns pinKey(compId,pinIdx) → SPICE net number (same logic as generateNetlist)
    std::unordered_map<int,int> computePinNodeMap() const;
    static int pinKey(int compId, int pinIdx) { return compId * 64 + pinIdx; }

    // Returns the user-assigned netName for a given SPICE node ID (empty if none).
    std::string getNetNameForNode(int nodeId) const;
    // Returns netName → nodeId mapping for all wires that have a netName.
    std::unordered_map<std::string, int> computeNetNameToNodeMap() const;

    // Persist / restore schematic (custom .sch text format)
    bool saveToFile(const std::string& path) const;
    bool loadFromFile(const std::string& path);

    void clear();

    SchematicSimConfig simCfg;

private:
    std::vector<SchematicComp> comps_;
    std::vector<SchematicWire> wires_;
    int nextCompId_ = 1;
    int nextWireId_ = 1;
    std::unordered_map<std::string, int> prefixCounts_; // prefix -> count
};
