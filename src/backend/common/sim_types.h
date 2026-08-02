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

    // POP (Periodic Operating Point) mode. When enabled, the full transient is
    // simulated as usual, then — once the run completes — only the last
    // `pop_periods` fundamental-frequency periods before t_end are retained.
    // The fundamental is auto-detected from the gate-drive frequency of the
    // switching device carrying the largest current (see Simulator).
    bool   pop_enabled  = false;
    int    pop_periods  = 5;  // number of fundamental periods to keep
};

// Diagnostic event emitted by the simulation engine (convergence failures, NaN, etc.)
struct DiagEvent {
    enum Level { Info, Warning, Error };
    Level       level   = Info;
    double      time    = 0.0;
    std::string message;
};
