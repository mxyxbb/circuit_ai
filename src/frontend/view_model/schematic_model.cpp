#include "view_model/schematic_model.h"
#include "common/expr_eval.h"
#include <algorithm>
#include <sstream>
#include <fstream>
#include <set>
#include <functional>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
      { {"P",{-20,0}}, {"N",{20,0}} },
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

    // V_SQUARE: phase (degrees) and tdelay (seconds) are linked. Editing one
    // recomputes the other; on freq change we update whichever the user did
    // NOT touch most recently. The trailing "_linkBy" param is hidden from
    // the panel and persists "phase" or "tdelay" across saves.
    { "V_SQUARE", "Voltage Square", "V",
      { {"P",{-40,0}}, {"N",{40,0}} },
      { {"freq (Hz)","1k"}, {"duty","0.5"}, {"Vhigh (V)","5"}, {"Vlow (V)","0"},
        {"tdelay (s)","0"}, {"phase (deg)","0"}, {"_linkBy","tdelay", true} },
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

    // Voltage-controlled sources (SPICE E/G). Output pins P/N on top/bottom,
    // single control-sense pin CP on the left, referenced to GND (netlist
    // emits NC- = 0). VCVS: V(P,N) = gain*V(CP); VCCS: I into P = gm*V(CP).
    { "VCVS",     "Controlled V Source", "E",
      { {"P",{0,-40}}, {"N",{0,40}}, {"CP",{-40,0}} },
      { {"gain", "1"} },
      {26,44} },

    { "VCCS",     "Controlled I Source", "G",
      { {"P",{0,-40}}, {"N",{0,40}}, {"CP",{-40,0}} },
      { {"gm (S)", "1"} },
      {26,44} },

    // Op-amp (finite gain + rail clamp) and ideal comparator. Inputs on the
    // left (IN+ top, IN- bottom), single-ended output on the right (vs GND).
    { "OPAMP",    "Op-Amp",         "OP",
      { {"IN+",{-40,-20}}, {"IN-",{-40,20}}, {"OUT",{40,0}} },
      { {"gain", "100k"}, {"Vmax (V)", "15"}, {"Vmin (V)", "-15"} },
      {28,28} },

    { "CMP",      "Comparator",     "CMP",
      { {"IN+",{-40,-20}}, {"IN-",{-40,20}}, {"OUT",{40,0}} },
      { {"Vhigh (V)", "5"}, {"Vlow (V)", "0"} },
      {28,28} },

    { "D",        "Diode",          "D",
      { {"A",{-40,0}}, {"K",{40,0}} },
      {},
      {24,14} },

    { "S",        "nmos",           "S",
      { {"D",{20,-40}}, {"S",{20,+40}}, {"G",{-20,0}}, {"GRef",{-20,+20}} },
      { {"Ron (Ohm)", "1m"} },
      {40,44} },

    { "TX",       "Transformer",    "TX",
      { {"P1",{-40,-20}}, {"N1",{-40,20}}, {"P2",{40,-20}}, {"N2",{40,20}} },
      { {"turns1","10"}, {"turns2","1"} },
      {24,24} },

    { "TX3",      "Transformer 3W", "TX",
      { {"P1",{-40,-20}}, {"N1",{-40,+20}},
        {"P2",{+40,-30}}, {"N2",{+40,-10}},
        {"P3",{+40,+10}}, {"N3",{+40,+30}} },
      { {"turns1","10"}, {"turns2","1"}, {"turns3","1"} },
      {28,36} },

    { "JUNC",     "Junction",       "",
      { {"J",{0,0}} },
      {},
      {4,4} },

    { "NETLABEL", "Net Label",      "NET",
      { {"NET",{-20,0}} },
      { {"label","NET1"} },
      {24,10} },

    { "TX_WIND",  "TX Winding",     "",
      { {"P",{0,-40}}, {"N",{0,+40}} },
      { {"txGroup","TX1"}, {"windingIdx","1"}, {"turns (n)","1"} },
      {20,44} },

    { "TX_CORE",  "TX Core",        "",
      {},
      { {"txGroup","TX1"}, {"numWindings","2"} },
      {4,40} },

    { "TXN_CUSTOM","Custom TX...",  "",
      {},
      {},
      {10,10} },

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
    variables_.clear();
    nextCompId_ = 1;
    nextWireId_ = 1;
    prefixCounts_.clear();
}

// ── User variables ────────────────────────────────────────────────────────────

std::unordered_map<std::string,double> SchematicModel::variableMap() const {
    std::unordered_map<std::string,double> map;
    for (const auto& v : variables_) {
        if (v.name.empty()) continue;
        std::string key = v.name;
        std::transform(key.begin(), key.end(), key.begin(), ::toupper);
        double val;
        if (exprv::tryEval(v.expr, &map, val))
            map[key] = val;
    }
    return map;
}

// ── Node map (single source of truth for SPICE node numbering) ───────────────

std::unordered_map<int,int> SchematicModel::computePinNodeMap() const {
    const int GND_KEY = -1;
    std::unordered_map<int,int> ufp;
    std::function<int(int)> ufFind = [&](int x) -> int {
        if (!ufp.count(x)) ufp[x] = x;
        if (ufp.at(x) != x) ufp[x] = ufFind(ufp.at(x));
        return ufp.at(x);
    };
    auto ufUnite = [&](int a, int b) {
        int ra = ufFind(a), rb = ufFind(b);
        if (ra != rb) ufp[ra] = rb;
    };
    for (const auto& c : comps_) {
        const CompTypeDef* td = findCompType(c.typeId);
        if (!td) continue;
        for (int i = 0; i < (int)td->pins.size(); ++i)
            ufFind(pinKey(c.id, i));
    }
    for (const auto& c : comps_)
        if (c.typeId == "GND") ufUnite(pinKey(c.id, 0), GND_KEY);
    for (const auto& w : wires_)
        ufUnite(pinKey(w.fromCompId, w.fromPinIdx), pinKey(w.toCompId, w.toPinIdx));
    {
        std::unordered_map<std::string,int> labelKey;
        for (const auto& c : comps_) {
            if (c.typeId != "NETLABEL" || c.paramValues.empty()) continue;
            int key = pinKey(c.id, 0);
            auto it = labelKey.find(c.paramValues[0]);
            if (it == labelKey.end()) labelKey[c.paramValues[0]] = key;
            else ufUnite(key, it->second);
        }
    }
    {
        std::unordered_map<std::string,int> netKey;
        for (const auto& w : wires_) {
            if (w.netName.empty()) continue;
            int key = pinKey(w.fromCompId, w.fromPinIdx);
            auto it = netKey.find(w.netName);
            if (it == netKey.end()) netKey[w.netName] = key;
            else ufUnite(key, it->second);
        }
    }
    int gndRoot = ufFind(GND_KEY);

    // Count pins per net root so unconnected pins can be detected.
    std::unordered_map<int,int> rootPinCount;
    for (const auto& c : comps_) {
        const CompTypeDef* td = findCompType(c.typeId);
        if (!td) continue;
        for (int i = 0; i < (int)td->pins.size(); ++i)
            rootPinCount[ufFind(pinKey(c.id, i))]++;
    }

    // Implicit-ground rule for single-wire gate drives: a floating switch GRef
    // pin or a floating voltage-source N pin is tied to node 0. This lets a
    // standalone drive source connect to a MOSFET gate with one wire (V.P → S.G)
    // — both return pins fall back to GND, closing the gate loop.
    auto implicitGnd = [&](const SchematicComp& c, int pinIdx) -> bool {
        bool candidate =
            (c.typeId == "S" && pinIdx == 3) ||                       // GRef
            ((c.typeId == "V_DC" || c.typeId == "V_SQUARE" ||
              c.typeId == "V_SIN" || c.typeId == "V_STEP") && pinIdx == 1); // N
        if (!candidate) return false;
        int root = ufFind(pinKey(c.id, pinIdx));
        return root != gndRoot && rootPinCount[root] == 1;
    };

    std::unordered_map<int,int> rootToNet;
    rootToNet[gndRoot] = 0;
    int nextNet = 1;
    std::unordered_map<int,int> result;
    for (const auto& c : comps_) {
        const CompTypeDef* td = findCompType(c.typeId);
        if (!td) continue;
        for (int i = 0; i < (int)td->pins.size(); ++i) {
            int key = pinKey(c.id, i);
            if (implicitGnd(c, i)) { result[key] = 0; continue; }
            int root = ufFind(key);
            auto it = rootToNet.find(root);
            int node;
            if (it != rootToNet.end()) node = it->second;
            else { node = nextNet++; rootToNet[root] = node; }
            result[key] = node;
        }
    }
    return result;
}

// ── Netlist generation ────────────────────────────────────────────────────────

std::string SchematicModel::generateNetlist(const SchematicSimConfig& cfg) const {
    if (comps_.empty()) return "";

    // Node numbering: shared with V-probe / net-name lookups so probe node
    // numbers always match the netlist. Includes the implicit-ground rule for
    // floating GRef / source-N pins (single-wire gate drives).
    const std::unordered_map<int,int> nodeMap = computePinNodeMap();
    auto getNet = [&](int compId, int pinIdx) -> int {
        auto it = nodeMap.find(pinKey(compId, pinIdx));
        return (it != nodeMap.end()) ? it->second : 0;
    };

    // Evaluate a numeric parameter (expressions + user variables + suffixes) to
    // a plain literal. Unevaluable strings pass through unchanged so the
    // backend's lenient fallback still sees the raw text.
    const auto varMap = variableMap();
    auto ev = [&](const std::string& s) -> std::string {
        double v;
        if (!exprv::tryEval(s, &varMap, v)) return s;
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.12g", v);
        return std::string(buf);
    };

    // ── Generate SPICE text ───────────────────────────────────────────────────
    std::ostringstream oss;
    oss << "* Generated by CircuitAI Schematic\n";

    std::set<int> usedNets;
    // Track nets only for pins that actually reach the SPICE output; a net used
    // solely by helper symbols (lone JUNC/NETLABEL) must not be probed — its
    // node number would not exist in the parsed circuit.
    auto useNet = [&](int n) { if (n != 0) usedNets.insert(n); };

    for (const auto& comp : comps_) {
        if (comp.typeId == "GND") continue;

        const CompTypeDef* td = findCompType(comp.typeId);
        if (!td) continue;

        std::vector<int> nets;
        for (int i = 0; i < (int)td->pins.size(); ++i)
            nets.push_back(getNet(comp.id, i));

        // Helper: safe param access
        auto p = [&](int i) -> const std::string& {
            static const std::string empty;
            return (i >= 0 && i < (int)comp.paramValues.size())
                   ? comp.paramValues[i] : empty;
        };

        const std::string& n = comp.instanceName;
        bool emitted = true;

        if (comp.typeId == "R") {
            oss << n << ' ' << nets[0] << ' ' << nets[1] << ' ' << ev(p(0)) << '\n';
        } else if (comp.typeId == "C") {
            oss << n << ' ' << nets[0] << ' ' << nets[1] << ' ' << ev(p(0)) << '\n';
        } else if (comp.typeId == "L") {
            oss << n << ' ' << nets[0] << ' ' << nets[1] << ' ' << ev(p(0)) << '\n';
        } else if (comp.typeId == "V_DC") {
            oss << n << ' ' << nets[0] << ' ' << nets[1] << " DC " << ev(p(0)) << '\n';
        } else if (comp.typeId == "V_SQUARE") {
            oss << n << ' ' << nets[0] << ' ' << nets[1]
                << " SQUARE freq=" << ev(p(0)) << " duty=" << ev(p(1))
                << " Vhigh=" << ev(p(2)) << " Vlow=" << ev(p(3))
                << " tdelay=" << ev(p(4)) << '\n';
        } else if (comp.typeId == "V_SIN") {
            oss << n << ' ' << nets[0] << ' ' << nets[1]
                << " SIN freq=" << ev(p(0)) << " vampl=" << ev(p(1))
                << " voff=" << ev(p(2)) << '\n';
        } else if (comp.typeId == "V_STEP") {
            oss << n << ' ' << nets[0] << ' ' << nets[1]
                << " STEP V0=" << ev(p(0)) << " V1=" << ev(p(1))
                << " tdelay=" << ev(p(2)) << '\n';
        } else if (comp.typeId == "I") {
            oss << n << ' ' << nets[0] << ' ' << nets[1] << " DC " << ev(p(0)) << '\n';
        } else if (comp.typeId == "VCVS" || comp.typeId == "VCCS") {
            // E/G: N+ N- NC+ NC- <gain|gm> — control sense referenced to GND
            oss << n << ' ' << nets[0] << ' ' << nets[1]
                << ' ' << nets[2] << " 0 " << ev(p(0)) << '\n';
        } else if (comp.typeId == "OPAMP") {
            oss << n << ' ' << nets[0] << ' ' << nets[1] << ' ' << nets[2]
                << " gain=" << ev(p(0)) << " vmax=" << ev(p(1))
                << " vmin=" << ev(p(2)) << '\n';
        } else if (comp.typeId == "CMP") {
            oss << n << ' ' << nets[0] << ' ' << nets[1] << ' ' << nets[2]
                << " vhigh=" << ev(p(0)) << " vlow=" << ev(p(1)) << '\n';
        } else if (comp.typeId == "D") {
            oss << n << ' ' << nets[0] << ' ' << nets[1] << '\n';
        } else if (comp.typeId == "S") {
            // Older .sch files predate the Ron parameter and may have an empty
            // paramValues here. Emitting "Ron=" with no value would parse to
            // rOn = 0 and crash the solver (1/0 conductance). Only attach the
            // Ron= token when the parameter actually has a value -- the parser
            // falls back to its 1 mΩ default otherwise.
            oss << n << ' ' << nets[0] << ' ' << nets[1]
                << ' ' << nets[2] << ' ' << nets[3];
            if (!p(0).empty()) oss << " Ron=" << ev(p(0));
            oss << '\n';
        } else if (comp.typeId == "TX") {
            oss << n << ' ' << nets[0] << ' ' << nets[1]
                << ' ' << nets[2] << ' ' << nets[3]
                << " turns1=" << ev(p(0)) << " turns2=" << ev(p(1)) << '\n';
        } else if (comp.typeId == "TX3") {
            oss << n << ' ' << nets[0] << ' ' << nets[1]
                << ' ' << nets[2] << ' ' << nets[3]
                << ' ' << nets[4] << ' ' << nets[5]
                << " turns1=" << ev(p(0)) << " turns2=" << ev(p(1))
                << " turns3=" << ev(p(2)) << '\n';
        } else if (comp.typeId == "TX_CORE") {
            emitted = false;
            if (comp.paramValues.empty()) continue;
            const std::string& grp = comp.paramValues[0];
            // Collect TX_WIND components belonging to this group, sorted by windingIdx
            std::vector<const SchematicComp*> winds;
            for (const auto& wc : comps_) {
                if (wc.typeId == "TX_WIND" && !wc.paramValues.empty() && wc.paramValues[0] == grp)
                    winds.push_back(&wc);
            }
            if (winds.empty()) continue;
            std::sort(winds.begin(), winds.end(), [](const SchematicComp* a, const SchematicComp* b){
                int ai=0, bi=0;
                try { if (a->paramValues.size()>1) ai=std::stoi(a->paramValues[1]); } catch(...){}
                try { if (b->paramValues.size()>1) bi=std::stoi(b->paramValues[1]); } catch(...){}
                return ai < bi;
            });
            oss << n;
            for (const auto* wc : winds) {
                int np = getNet(wc->id, 0), nn = getNet(wc->id, 1);
                oss << ' ' << np << ' ' << nn;
                useNet(np); useNet(nn);
            }
            for (int wi = 0; wi < (int)winds.size(); ++wi) {
                const std::string& t = (winds[wi]->paramValues.size() > 2)
                    ? winds[wi]->paramValues[2] : "1";
                oss << " turns" << (wi+1) << '=' << ev(t);
            }
            oss << '\n';
        } else {
            // Helper types (TX_WIND, JUNC, NETLABEL, TXN_CUSTOM): no SPICE output
            emitted = false;
        }

        if (emitted)
            for (int nn : nets) useNet(nn);
    }

    oss << ".TRAN " << ev(cfg.dt) << ' ' << ev(cfg.tEnd) << '\n';
    if (cfg.popEnabled) {
        int n = std::atoi(cfg.popPeriods);
        if (n < 1) n = 1;
        oss << ".POP " << n << '\n';
    }
    // Probe all node voltages
    for (int net : usedNets)
        oss << ".PROBE V(" << net << ")\n";
    // Probe branch current of every component that emits SPICE
    for (const auto& comp : comps_) {
        if (comp.typeId == "GND"      || comp.typeId == "TX_WIND" ||
            comp.typeId == "JUNC"     || comp.typeId == "NETLABEL" ||
            comp.typeId == "TXN_CUSTOM") continue;
        if (comp.typeId == "TX") {
            oss << ".PROBE I(" << comp.instanceName << ")\n";    // winding 0
            oss << ".PROBE I(" << comp.instanceName << "_W1)\n"; // winding 1
        } else if (comp.typeId == "TX3") {
            oss << ".PROBE I(" << comp.instanceName << ")\n";
            oss << ".PROBE I(" << comp.instanceName << "_W1)\n";
            oss << ".PROBE I(" << comp.instanceName << "_W2)\n";
        } else if (comp.typeId == "TX_CORE") {
            // Mirror the component-emission guard above: a core only produces a
            // SPICE component (and thus a probeable branch current) when it has a
            // group AND at least one matching winding. Orphan cores emit nothing,
            // so probing them references a non-existent component and aborts the
            // whole build with "PROBE: unknown component <core>".
            if (comp.paramValues.empty()) continue;
            const std::string& grp = comp.paramValues[0];
            int nWinds = 0;
            for (const auto& wc : comps_)
                if (wc.typeId == "TX_WIND" && !wc.paramValues.empty() && wc.paramValues[0] == grp)
                    ++nWinds;
            if (nWinds == 0) continue;
            oss << ".PROBE I(" << comp.instanceName << ")\n";
            for (int w = 1; w < nWinds; ++w)
                oss << ".PROBE I(" << comp.instanceName << "_W" << w << ")\n";
        } else {
            oss << ".PROBE I(" << comp.instanceName << ")\n";
        }
    }
    oss << ".END\n";

    return oss.str();
}

// ── Schematic file I/O (.sch format) ─────────────────────────────────────────
//
// Format:
//   CSCH1                               — magic/version header
//   S <dt> <tEnd>                       — sim config
//   C <id> <typeId> <name> <x> <y> <rot> [p1|p2|...]   — component
//   W <id> <fc> <fp> <tc> <tp> [wx wy ...]              — wire + waypoints

bool SchematicModel::saveToFile(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return false;

    f << "CSCH3\n";
    // Trailing pop fields are optional; older loaders that read only dt/tEnd
    // simply ignore them (backward compatible).
    f << "S " << simCfg.dt << ' ' << simCfg.tEnd
      << ' ' << (simCfg.popEnabled ? 1 : 0) << ' ' << simCfg.popPeriods << '\n';

    // User variables: "P <name> <expr>" (expr may contain spaces).
    // Older loaders skip unknown tags, so this stays CSCH3-compatible.
    for (const auto& v : variables_)
        if (!v.name.empty())
            f << "P " << v.name << ' ' << v.expr << '\n';

    for (const auto& c : comps_) {
        f << 'C' << ' ' << c.id << ' ' << c.typeId << ' ' << c.instanceName
          << ' ' << c.pos.x << ' ' << c.pos.y << ' ' << c.rotation
          << ' ' << (c.mirrorX ? 1 : 0);
        if (!c.paramValues.empty()) {
            f << ' ';
            for (size_t i = 0; i < c.paramValues.size(); ++i) {
                if (i > 0) f << '|';
                f << c.paramValues[i];
            }
        }
        f << '\n';
    }

    for (const auto& w : wires_) {
        f << 'W' << ' ' << w.id
          << ' ' << w.fromCompId << ' ' << w.fromPinIdx
          << ' ' << w.toCompId   << ' ' << w.toPinIdx;
        for (const auto& wp : w.waypoints)
            f << ' ' << wp.x << ' ' << wp.y;
        if (!w.netName.empty())
            f << " name=" << w.netName;
        f << '\n';
    }

    return f.good();
}

bool SchematicModel::loadFromFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;

    std::string line;
    if (!std::getline(f, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    bool hasMirrorX = (line == "CSCH2" || line == "CSCH3");
    bool hasNetName = (line == "CSCH3");
    if (line != "CSCH1" && line != "CSCH2" && line != "CSCH3") return false;

    clear();
    int maxCompId = 0, maxWireId = 0;

    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        std::istringstream ss(line);
        char tag;
        ss >> tag;

        if (tag == 'P') {
            SchematicVar v;
            if (!(ss >> v.name)) continue;
            std::string rest;
            if (std::getline(ss, rest)) {
                size_t s = rest.find_first_not_of(" \t");
                if (s != std::string::npos) v.expr = rest.substr(s);
            }
            variables_.push_back(std::move(v));
        } else if (tag == 'S') {
            std::string dt, tEnd;
            if (ss >> dt >> tEnd) {
                std::strncpy(simCfg.dt,   dt.c_str(),   sizeof(simCfg.dt)   - 1);
                std::strncpy(simCfg.tEnd, tEnd.c_str(), sizeof(simCfg.tEnd) - 1);
                simCfg.dt  [sizeof(simCfg.dt)   - 1] = '\0';
                simCfg.tEnd[sizeof(simCfg.tEnd) - 1] = '\0';
                // Optional POP fields (added later; absent in older files).
                int popEn = 0;
                std::string popN;
                if (ss >> popEn) simCfg.popEnabled = (popEn != 0);
                if (ss >> popN) {
                    std::strncpy(simCfg.popPeriods, popN.c_str(), sizeof(simCfg.popPeriods) - 1);
                    simCfg.popPeriods[sizeof(simCfg.popPeriods) - 1] = '\0';
                }
            }
        } else if (tag == 'C') {
            SchematicComp c;
            if (!(ss >> c.id >> c.typeId >> c.instanceName
                     >> c.pos.x >> c.pos.y >> c.rotation))
                continue;
            if (hasMirrorX) {
                int mx = 0;
                ss >> mx;
                c.mirrorX = (mx != 0);
            }
            // Remaining: param values joined by '|'
            std::string rest;
            if (std::getline(ss, rest)) {
                size_t s = rest.find_first_not_of(" \t");
                if (s != std::string::npos) {
                    std::istringstream ps(rest.substr(s));
                    std::string tok;
                    while (std::getline(ps, tok, '|'))
                        c.paramValues.push_back(tok);
                }
            }
            // Backfill any params added to the type def AFTER this .sch was
            // saved (e.g. V_SQUARE got phase/_linkBy in 2026-05). Without this,
            // legacy schematics open with a short paramValues vector and the
            // property panel reads out of bounds for the new fields.
            if (const CompTypeDef* tdf = findCompType(c.typeId)) {
                while (c.paramValues.size() < tdf->params.size())
                    c.paramValues.push_back(tdf->params[c.paramValues.size()].defaultValue);
            }
            if (c.id > maxCompId) maxCompId = c.id;
            comps_.push_back(std::move(c));
        } else if (tag == 'W') {
            SchematicWire w;
            if (!(ss >> w.id >> w.fromCompId >> w.fromPinIdx
                     >> w.toCompId >> w.toPinIdx))
                continue;
            std::string tok;
            while (ss >> tok) {
                if (hasNetName && tok.substr(0,5) == "name=") {
                    w.netName = tok.substr(5);
                } else {
                    // two floats for waypoint
                    float wx = std::stof(tok), wy;
                    if (ss >> wy) w.waypoints.push_back({wx, wy});
                }
            }
            if (w.id > maxWireId) maxWireId = w.id;
            wires_.push_back(std::move(w));
        }
    }

    nextCompId_ = maxCompId + 1;
    nextWireId_ = maxWireId + 1;

    // Rebuild prefix counters so subsequent addComp names don't collide
    for (const auto& c : comps_) {
        const CompTypeDef* td = findCompType(c.typeId);
        if (!td || td->prefix.empty()) continue;
        const std::string& pfx = td->prefix;
        if (c.instanceName.size() > pfx.size() &&
            c.instanceName.compare(0, pfx.size(), pfx) == 0)
        {
            try {
                int num = std::stoi(c.instanceName.substr(pfx.size()));
                int& cnt = prefixCounts_[pfx];
                if (num > cnt) cnt = num;
            } catch (...) {}
        }
    }

    return true;
}

std::string SchematicModel::getNetNameForNode(int nodeId) const {
    auto nodeMap = computePinNodeMap();
    for (const auto& w : wires_) {
        if (w.netName.empty()) continue;
        auto it = nodeMap.find(pinKey(w.fromCompId, w.fromPinIdx));
        if (it != nodeMap.end() && it->second == nodeId)
            return w.netName;
    }
    return "";
}

std::unordered_map<std::string, int> SchematicModel::computeNetNameToNodeMap() const {
    auto nodeMap = computePinNodeMap();
    std::unordered_map<std::string, int> result;
    for (const auto& w : wires_) {
        if (w.netName.empty()) continue;
        auto it = nodeMap.find(pinKey(w.fromCompId, w.fromPinIdx));
        if (it != nodeMap.end() && it->second != 0)
            result[w.netName] = it->second;
    }
    return result;
}
