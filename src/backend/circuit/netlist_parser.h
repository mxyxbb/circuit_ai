#pragma once
#include "circuit/circuit.h"
#include "common/sim_types.h"
#include <string>
#include <unordered_map>
#include <vector>

struct ParseResult {
    bool success = false;
    std::string error;
    int errorLine = -1;
    Circuit circuit;
    SimConfig config;
    std::vector<SignalInfo> probes;
};

class NetlistParser {
public:
    ParseResult parse(const std::string& filepath);
    ParseResult parseString(const std::string& content);

private:
    bool processLine(const std::string& line, ParseResult& result);
    // Evaluates s as an arithmetic expression (with SPICE magnitude suffixes
    // and .PARAM variables); falls back to lenient "number + suffix" parsing
    // for backward compatibility. Returns 0.0 if nothing parses.
    double parseValue(const std::string& s);
    std::string toUpper(std::string s);

    // .PARAM name=expr definitions, usable in any subsequent value expression.
    // Keys stored uppercase (netlists are case-insensitive).
    std::unordered_map<std::string, double> params_;

    // Parsing .PROBE entries like V(1), I(R1)
    bool parseProbe(const std::string& token, ParseResult& result);
};
