#include "view_model/schematic_model.h"
#include <algorithm>
#include <sstream>
#include <set>
#include <functional>
#include <cmath>
#include <unordered_map>

// ── Static component type registry ───────────────────────────────────────────
// Pin offsets: ±40 px in the primary direction (= 2 grid cells at 20 px/cell).
// bodyHalfSize: {24,14} for 2-pin, {24,24} for 4-pin, {10,10} for GND symbol.

static const std::vector<CompTypeDef> s_compTypes = {
    { "R",        "Resistor",       "R",
      { {"P",{-40,0}}, {"N",{40,0}} },
      { {"R (\xce\xa9)", "1k"} },           // Ω in UTF-8
      {24,14} },

    { "C",        "Capacitor",      "C",
      { {"P",{-40,0}}, {"N",{40,0}} },
      { {"C (F)", "1u"} },
      {24,14} },

    { "L",        "Inductor",       "L",
      { {"P",{-40,0}}, {"N",{40,0}} },
      { {"L (H)", "10m"} },
      {24,14} },

    { "V_DC",     "Voltage DC",     "V",
      { {"P",{-40,0}}, {"N",{40,0}} },
      { {"V (V)", "5"} },
      {24,14} },

    { "V_SQUARE", "Voltage Square", "V",
      { {"P",{-40,0}}, {"N",{40,0}} },
      { {"freq (Hz)","1k"}, {"duty","0.5"}, {"Vhigh (V)","5"}, {"Vlow (V)","0"}, {"tdelay (s)","0"} },
      {24,14} },

    { "V_SIN",    "Voltage Sin",    "V",
      { {"P",{-40,0}}, {"N",{40,0}} },
      { {"freq (Hz)","50"}, {"Vampl (V)","170"}, {"Voff (V)","0"} },
      {24,14} },

    { "V_STEP",   "Voltage Step",   "V",
      { {"P",{-40,0}}, {"N",{40,0}} },
      { {"V0 (V)","0"}, {"V1 (V)","5"}, {"tdelay (s)","1e-3"} },
      {24,14} },

    { "I",        "Current Source", "I",
      { {"P",{-40,0}}, {"N",{40,0}} },
      { {"I (A)", "1m"} },
      {24,14} },

    { "D",        "Diode",          "D",
      { {"A",{-40,0}}, {"K",{40,0}} },
      {},
      {24,14} },

    { "S",        "Switch",         "S",
      { {"D",{0,-40}}, {"S",{0,40}}, {"G",{-40,0}}, {"GRef",{40,0}} },
      {},
      {24,24} },

    { "TX",       "Transformer",    "TX",
      { {"P1",{-40,-20}}, {"N1",{-40,20}}, {"P2",{40,-20}}, {"N2",{40,20}} },
      { {"ratio","10"} },
      {24,24} },

    { "GND",      "Ground",         "",
      { {"GND",{0,0}} },
      {},
      {10,10} },
};

const std::vector<CompTypeDef>& SchematicModel::compTypes() {
    return s_compTypes;
}

const CompTypeDef* SchematicModel::findCompType(const std::string& id) {
    for (const auto& t : s_compTypes)
        if (t.id == id) return &t;
    return nullptr;
}

// ── Component CRUD ────────────────────────────────────────────────────────────

int SchematicModel::addComp(const std::string& typeId, ImVec2 pos) {
    const CompTypeDef* td = findCompType(typeId);
    if (!td) return -1;

    SchematicComp comp;
    comp.id     = nextCompId_++;
    comp.typeId = typeId;
    comp.pos    = pos;

    // Auto-generate instance name: prefix + incrementing counter
    if (!td->prefix.empty()) {
        int& cnt = prefixCounts_[td->prefix];
        comp.instanceName = td->prefix + std::to_string(++cnt);
    } else {
        // GND and others without prefix get typeId + id
        comp.instanceName = typeId + std::to_string(comp.id);
    }

    // Initialise parameters to defaults
    for (const auto& pd : td->params)
        comp.paramValues.push_back(pd.defaultValue);

    comps_.push_back(std::move(comp));
    return comps_.back().id;
}

void SchematicModel::removeComp(int id) {
    removeWiresForComp(id);
    comps_.erase(
        std::remove_if(comps_.begin(), comps_.end(),
                       [id](const SchematicComp& c){ return c.id == id; }),
        comps_.end());
}

void SchematicModel::removeWiresForComp(int compId) {
    wires_.erase(
        std::remove_if(wires_.begin(), wires_.end(),
                       [compId](const SchematicWire& w){
                           return w.fromCompId == compId || w.toCompId == compId;
                       }),
        wires_.end());
}

SchematicComp* SchematicModel::findComp(int id) {
    for (auto& c : comps_) if (c.id == id) return &c;
    return nullptr;
}
const SchematicComp* SchematicModel::findComp(int id) const {
    for (const auto& c : comps_) if (c.id == id) return &c;
    return nullptr;
}

// ── Wire CRUD ─────────────────────────────────────────────────────────────────

int SchematicModel::addWire(int fromCompId, int fromPinIdx,
                            int toCompId,   int toPinIdx,
                            const std::vector<ImVec2>& waypoints) {
    // Prevent self-connection and duplicates
    if (fromCompId == toCompId && fromPinIdx == toPinIdx) return -1;
    for (const auto& w : wires_) {
        if ((w.fromCompId == fromCompId && w.fromPinIdx == fromPinIdx &&
             w.toCompId   == toCompId   && w.toPinIdx   == toPinIdx ) ||
            (w.fromCompId == toCompId   && w.fromPinIdx == toPinIdx   &&
             w.toCompId   == fromCompId && w.toPinIdx   == fromPinIdx))
            return w.id;
    }
    SchematicWire wire;
    wire.id         = nextWireId_++;
    wire.fromCompId = fromCompId; wire.fromPinIdx = fromPinIdx;
    wire.toCompId   = toCompId;   wire.toPinIdx   = toPinIdx;
    wire.waypoints  = waypoints;
    wires_.push_back(wire);
    return wires_.back().id;
}

void SchematicModel::removeWire(int id) {
    wires_.erase(
        std::remove_if(wires_.begin(), wires_.end(),
                       [id](const SchematicWire& w){ return w.id == id; }),
        wires_.end());
}

void SchematicModel::clear() {
    comps_.clear();
    wires_.clear();
    nextCompId_ = 1;
    nextWireId_ = 1;
    prefixCounts_.clear();
}

// ── Netlist generation ────────────────────────────────────────────────────────

std::string SchematicModel::generateNetlist(const SchematicSimConfig& cfg) const {
    if (comps_.empty()) return "";

    // ── Union-Find (map-based, handles negative keys) ─────────────────────────
    const int GND_KEY = -1;
    std::unordered_map<int,int> ufp;  // parent map

    std::function<int(int)> ufFind = [&](int x) -> int {
        if (!ufp.count(x)) ufp[x] = x;
        if (ufp.at(x) != x) ufp[x] = ufFind(ufp.at(x));  // path compression
        return ufp.at(x);
    };
    auto ufUnite = [&](int a, int b) {
        int ra = ufFind(a), rb = ufFind(b);
        if (ra != rb) ufp[ra] = rb;
    };

    // Encode (compId, pinIdx) as a single int (each compId < 2^24, pinIdx < 64)
    auto pinKey = [](int compId, int pinIdx) { return compId * 64 + pinIdx; };

    // Initialise every pin vertex
    for (const auto& c : comps_) {
        const CompTypeDef* td = findCompType(c.typeId);
        if (!td) continue;
        for (int i = 0; i < (int)td->pins.size(); ++i)
            ufFind(pinKey(c.id, i));
    }

    // GND symbols → unite with GND_KEY
    for (const auto& c : comps_)
        if (c.typeId == "GND") ufUnite(pinKey(c.id, 0), GND_KEY);

    // Wires → unite connected pins
    for (const auto& w : wires_)
        ufUnite(pinKey(w.fromCompId, w.fromPinIdx),
                pinKey(w.toCompId,   w.toPinIdx));

    // Assign net numbers: GND root → 0, rest → 1,2,3...
    int gndRoot = ufFind(GND_KEY);
    std::unordered_map<int,int> rootToNet;
    rootToNet[gndRoot] = 0;
    int nextNet = 1;

    auto getNet = [&](int compId, int pinIdx) -> int {
        int root = ufFind(pinKey(compId, pinIdx));
        auto it = rootToNet.find(root);
        if (it != rootToNet.end()) return it->second;
        rootToNet[root] = nextNet++;
        return rootToNet.at(root);
    };

    // ── Generate SPICE text ───────────────────────────────────────────────────
    std::ostringstream oss;
    oss << "* Generated by CircuitAI Schematic\n";

    std::set<int> usedNets;

    for (const auto& comp : comps_) {
        if (comp.typeId == "GND") continue;

        const CompTypeDef* td = findCompType(comp.typeId);
        if (!td) continue;

        // Collect nets and track which non-zero nets exist
        std::vector<int> nets;
        for (int i = 0; i < (int)td->pins.size(); ++i) {
            int n = getNet(comp.id, i);
            nets.push_back(n);
            if (n != 0) usedNets.insert(n);
        }

        // Helper: safe param access
        auto p = [&](int i) -> const std::string& {
            static const std::string empty;
            return (i >= 0 && i < (int)comp.paramValues.size())
                   ? comp.paramValues[i] : empty;
        };

        const std::string& n = comp.instanceName;

        if (comp.typeId == "R") {
            oss << n << ' ' << nets[0] << ' ' << nets[1] << ' ' << p(0) << '\n';
        } else if (comp.typeId == "C") {
            oss << n << ' ' << nets[0] << ' ' << nets[1] << ' ' << p(0) << '\n';
        } else if (comp.typeId == "L") {
            oss << n << ' ' << nets[0] << ' ' << nets[1] << ' ' << p(0) << '\n';
        } else if (comp.typeId == "V_DC") {
            oss << n << ' ' << nets[0] << ' ' << nets[1] << " DC " << p(0) << '\n';
        } else if (comp.typeId == "V_SQUARE") {
            oss << n << ' ' << nets[0] << ' ' << nets[1]
                << " SQUARE freq=" << p(0) << " duty=" << p(1)
                << " Vhigh=" << p(2) << " Vlow=" << p(3)
                << " tdelay=" << p(4) << '\n';
        } else if (comp.typeId == "V_SIN") {
            oss << n << ' ' << nets[0] << ' ' << nets[1]
                << " SIN freq=" << p(0) << " vampl=" << p(1)
                << " voff=" << p(2) << '\n';
        } else if (comp.typeId == "V_STEP") {
            oss << n << ' ' << nets[0] << ' ' << nets[1]
                << " STEP V0=" << p(0) << " V1=" << p(1)
                << " tdelay=" << p(2) << '\n';
        } else if (comp.typeId == "I") {
            oss << n << ' ' << nets[0] << ' ' << nets[1] << " DC " << p(0) << '\n';
        } else if (comp.typeId == "D") {
            oss << n << ' ' << nets[0] << ' ' << nets[1] << '\n';
        } else if (comp.typeId == "S") {
            oss << n << ' ' << nets[0] << ' ' << nets[1]
                << ' ' << nets[2] << ' ' << nets[3] << '\n';
        } else if (comp.typeId == "TX") {
            oss << n << ' ' << nets[0] << ' ' << nets[1]
                << ' ' << nets[2] << ' ' << nets[3]
                << " ratio=" << p(0) << '\n';
        }
    }

    oss << ".TRAN " << cfg.dt << ' ' << cfg.tEnd << '\n';
    for (int net : usedNets)
        oss << ".PROBE V(" << net << ")\n";
    oss << ".END\n";

    return oss.str();
}
