#pragma once
#include "circuit/circuit.h"
#include "common/sim_types.h"
#include <string>
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
    double parseValue(const std::string& s);
    std::string toUpper(std::string s);

    // Parsing .PROBE entries like V(1), I(R1)
    bool parseProbe(const std::string& token, ParseResult& result);
};
