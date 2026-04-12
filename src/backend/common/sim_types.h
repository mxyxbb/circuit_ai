#pragma once
#include <string>
#include <vector>

// Single data sample produced by the simulation engine
struct SimSample {
    double time = 0.0;
    std::vector<double> values; // indexed by SignalInfo order
};

// Describes a signal to observe
struct SignalInfo {
    enum Type { NodeVoltage, BranchCurrent };
    Type        type;
    int         index;  // node number or component index
    std::string name;   // e.g. "V(1)", "I(R1)"
};

// Simulation parameters
struct SimConfig {
    double dt           = 1e-6;
    double t_end        = 0.01;
    int    sample_ratio = 1; // push one sample every N steps
};
