#include "circuit/netlist_parser.h"
#include "components/passives/resistor.h"
#include "components/passives/capacitor.h"
#include "components/passives/inductor.h"
#include "components/passives/transformer.h"
#include "components/sources/voltage_source.h"
#include "components/sources/current_source.h"
#include "components/sources/square_wave_source.h"
#include "components/sources/step_source.h"
#include "components/semiconductors/ideal_diode.h"
#include "components/semiconductors/ideal_switch.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <cmath>

ParseResult NetlistParser::parse(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        ParseResult r;
        r.error = "Cannot open file: " + filepath;
        return r;
    }
    std::stringstream ss;
    ss << file.rdbuf();
    return parseString(ss.str());
}

ParseResult NetlistParser::parseString(const std::string& content) {
    ParseResult result;
    std::istringstream stream(content);
    std::string line;
    int lineNum = 0;

    while (std::getline(stream, line)) {
        lineNum++;
        // Trim leading/trailing whitespace
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        line = line.substr(start);
        // Find end, stripping comments (after ;)
        size_t semi = line.find(';');
        if (semi != std::string::npos) line = line.substr(0, semi);
        size_t end = line.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) line = line.substr(0, end + 1);
        if (line.empty()) continue;
        if (line[0] == '*') continue; // comment

        result.errorLine = lineNum;
        if (!processLine(line, result)) {
            result.success = false;
            return result;
        }
    }
    result.success = true;
    result.errorLine = -1;
    return result;
}

std::string NetlistParser::toUpper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

double NetlistParser::parseValue(const std::string& s) {
    double val = 0.0;
    char suffix[16] = {};
    if (sscanf(s.c_str(), "%lf%s", &val, suffix) < 1) return 0.0;
    std::string suf = toUpper(suffix);
    if (suf == "F")      return val * 1e-15;
    if (suf == "P")      return val * 1e-12;
    if (suf == "N")      return val * 1e-9;
    if (suf == "U")      return val * 1e-6;
    if (suf == "M" && s.find("MEG") == std::string::npos) return val * 1e-3;
    if (suf == "K")      return val * 1e3;
    if (suf == "MEG")    return val * 1e6;
    if (suf == "G")      return val * 1e9;
    return val;
}

bool NetlistParser::parseProbe(const std::string& token, ParseResult& result) {
    // Format: V(1), I(R1)
    if (token.size() < 3) {
        result.error = "Invalid probe: " + token;
        return false;
    }
    char type = toupper(token[0]);
    size_t parenOpen = token.find('(');
    size_t parenClose = token.find(')');
    if (parenOpen == std::string::npos || parenClose == std::string::npos) {
        result.error = "Invalid probe syntax: " + token;
        return false;
    }
    std::string arg = token.substr(parenOpen + 1, parenClose - parenOpen - 1);

    SignalInfo si;
    si.name = toUpper(token.substr(0, 1)) + "(" + arg + ")";
    if (type == 'V') {
        si.type = SignalInfo::NodeVoltage;
        si.index = std::stoi(arg);
    } else if (type == 'I') {
        si.type = SignalInfo::BranchCurrent;
        // Find component by name
        const BaseComponent* comp = result.circuit.findComponent(arg);
        if (!comp) {
            result.error = "PROBE: unknown component " + arg;
            return false;
        }
        si.index = 0; // will be resolved by simulator
        si.name = "I(" + arg + ")";
    } else {
        result.error = "Unknown probe type: " + token;
        return false;
    }
    result.probes.push_back(si);
    return true;
}

bool NetlistParser::processLine(const std::string& line, ParseResult& result) {
    std::istringstream iss(line);
    std::vector<std::string> tokens;
    std::string tok;
    while (iss >> tok) tokens.push_back(tok);
    if (tokens.empty()) return true;

    std::string first = toUpper(tokens[0]);

    // --- Dot commands ---
    if (first[0] == '.') {
        if (first == ".TRAN" && tokens.size() >= 3) {
            result.config.dt = parseValue(tokens[1]);
            result.config.t_end = parseValue(tokens[2]);
            return true;
        }
        if (first == ".PROBE") {
            for (size_t i = 1; i < tokens.size(); i++) {
                if (!parseProbe(tokens[i], result)) return false;
            }
            return true;
        }
        if (first == ".END") return true;
        return true; // ignore unknown dot commands
    }

    // --- Component lines ---
    char prefix = first[0];
    std::string name = tokens[0];

    try {
        switch (prefix) {
        case 'R': case 'r': {
            if (tokens.size() < 4) { result.error = "R: need N+ N- value"; return false; }
            int np = std::stoi(tokens[1]), nn = std::stoi(tokens[2]);
            double r = parseValue(tokens[3]);
            result.circuit.addComponent(std::make_unique<Resistor>(name, np, nn, r));
            return true;
        }
        case 'C': case 'c': {
            if (tokens.size() < 4) { result.error = "C: need N+ N- value"; return false; }
            int np = std::stoi(tokens[1]), nn = std::stoi(tokens[2]);
            double c = parseValue(tokens[3]);
            result.circuit.addComponent(std::make_unique<Capacitor>(name, np, nn, c));
            return true;
        }
        case 'L': case 'l': {
            if (tokens.size() < 4) { result.error = "L: need N+ N- value"; return false; }
            int np = std::stoi(tokens[1]), nn = std::stoi(tokens[2]);
            double l = parseValue(tokens[3]);
            result.circuit.addComponent(std::make_unique<Inductor>(name, np, nn, l));
            return true;
        }
        case 'V': case 'v': {
            if (tokens.size() < 4) { result.error = "V: need N+ N- type params"; return false; }
            int np = std::stoi(tokens[1]), nn = std::stoi(tokens[2]);
            std::string srcType = toUpper(tokens[3]);

            if (srcType == "DC") {
                double v = parseValue(tokens[4]);
                result.circuit.addComponent(std::make_unique<VoltageSource>(name, np, nn, v));
            } else if (srcType == "SQUARE") {
                double freq = 1e3, duty = 0.5, vhigh = 1.0, vlow = 0.0;
                for (size_t i = 4; i < tokens.size(); i++) {
                    std::string p = toUpper(tokens[i]);
                    if (p.find("FREQ=") == 0) freq = parseValue(p.substr(5));
                    else if (p.find("DUTY=") == 0) duty = std::stod(p.substr(5));
                    else if (p.find("VHIGH=") == 0) vhigh = parseValue(p.substr(6));
                    else if (p.find("VLOW=") == 0) vlow = parseValue(p.substr(5));
                }
                result.circuit.addComponent(std::make_unique<SquareWaveSource>(name, np, nn, freq, duty, vhigh, vlow));
            } else if (srcType == "STEP") {
                double v0 = 0.0, v1 = 1.0, td = 0.0;
                for (size_t i = 4; i < tokens.size(); i++) {
                    std::string p = toUpper(tokens[i]);
                    if (p.find("V0=") == 0) v0 = parseValue(p.substr(3));
                    else if (p.find("V1=") == 0) v1 = parseValue(p.substr(3));
                    else if (p.find("TDELAY=") == 0) td = parseValue(p.substr(7));
                }
                result.circuit.addComponent(std::make_unique<StepSource>(name, np, nn, v0, v1, td));
            } else {
                result.error = "Unknown source type: " + srcType;
                return false;
            }
            return true;
        }
        case 'I': case 'i': {
            if (tokens.size() < 5) { result.error = "I: need N+ N- DC value"; return false; }
            int np = std::stoi(tokens[1]), nn = std::stoi(tokens[2]);
            double i = parseValue(tokens[4]);
            result.circuit.addComponent(std::make_unique<CurrentSource>(name, np, nn, i));
            return true;
        }
        case 'D': case 'd': {
            if (tokens.size() < 3) { result.error = "D: need anode cathode"; return false; }
            int na = std::stoi(tokens[1]), nk = std::stoi(tokens[2]);
            result.circuit.addComponent(std::make_unique<IdealDiode>(name, na, nk));
            return true;
        }
        case 'S': case 's': {
            if (tokens.size() < 5) { result.error = "S: need drain source gate gateRef"; return false; }
            int nd = std::stoi(tokens[1]), ns = std::stoi(tokens[2]);
            int ng = std::stoi(tokens[3]), ngr = std::stoi(tokens[4]);
            result.circuit.addComponent(std::make_unique<IdealSwitch>(name, nd, ns, ng, ngr));
            return true;
        }
        default:
            // Try TX for transformer
            if (first.size() >= 2 && (first[0] == 'T' || first[0] == 't') && toupper(first[1]) == 'X') {
                if (tokens.size() < 6) { result.error = "TX: need N1+ N1- N2+ N2- ratio"; return false; }
                int np1 = std::stoi(tokens[1]), nn1 = std::stoi(tokens[2]);
                int np2 = std::stoi(tokens[3]), nn2 = std::stoi(tokens[4]);
                double ratio = 1.0;
                for (size_t i = 5; i < tokens.size(); i++) {
                    std::string p = toUpper(tokens[i]);
                    if (p.find("RATIO=") == 0) ratio = std::stod(p.substr(6));
                    else ratio = parseValue(tokens[i]);
                }
                result.circuit.addComponent(std::make_unique<Transformer>(name, np1, nn1, np2, nn2, ratio));
                return true;
            }
            result.error = "Unknown component prefix: " + name;
            return false;
        }
    } catch (const std::exception& e) {
        result.error = std::string("Parse error: ") + e.what();
        return false;
    }
}
