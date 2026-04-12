#pragma once
#include "common/sim_types.h"
#include "engine/mna_solver.h"
#include "circuit/circuit.h"
#include "circuit/netlist_parser.h"
#include "components/passives/resistor.h"
#include "components/passives/capacitor.h"
#include "components/passives/inductor.h"
#include "components/sources/voltage_source.h"
#include "components/sources/step_source.h"
#include "components/sources/square_wave_source.h"
#include <cstdio>

// Run simulation synchronously and dump first N steps to a CSV file
inline void diagRunAndDump(const char* netlistPath, const char* csvPath, int maxSteps = 200) {
    NetlistParser parser;
    ParseResult result = parser.parse(netlistPath);
    if (!result.success) {
        fprintf(stderr, "PARSE ERROR line %d: %s\n", result.errorLine, result.error.c_str());
        return;
    }

    fprintf(stderr, "=== Diag: %s ===\n", netlistPath);
    fprintf(stderr, "Components: %zu, Nodes: %d, Probes: %zu\n",
            result.circuit.components().size(),
            result.circuit.nodeCount(),
            result.probes.size());
    fprintf(stderr, "dt=%.2e, t_end=%.4e\n", result.config.dt, result.config.t_end);

    // Setup solver
    size_t n = result.circuit.nodeCount();
    size_t m = 0;
    std::vector<size_t> extraAbs;
    for (const auto& comp : result.circuit.components()) {
        extraAbs.push_back(n + m);
        m += comp->extraVariableCount();
    }
    fprintf(stderr, "n=%zu, m=%zu, matrix=%zu\n", n, m, n + m);

    MNASolver solver;
    solver.init(n, m);

    // Build probe map
    struct ProbeInfo { std::string name; int idx; }; // idx: -1=NodeVoltage, else component index
    std::vector<ProbeInfo> probes;
    for (const auto& p : result.probes) {
        probes.push_back({p.name, static_cast<int>(p.type == SignalInfo::NodeVoltage ? -1 : 0)});
    }

    FILE* fp = fopen(csvPath, "w");
    if (!fp) { fprintf(stderr, "Cannot open %s\n", csvPath); return; }

    // CSV header
    fprintf(fp, "step,time");
    for (int i = 1; i <= (int)n; i++) fprintf(fp, ",V(%d)", i);
    for (size_t i = 0; i < m; i++) fprintf(fp, ",I_extra%zu", i);
    fprintf(fp, "\n");

    double t = 0;
    double dt = result.config.dt;
    double t_end = result.config.t_end;
    int stepCount = 0;

    while (t < t_end && stepCount < maxSteps) {
        solver.clear();

        size_t ci = 0;
        for (const auto& comp : result.circuit.components()) {
            comp->stamp(solver, dt, t, extraAbs[ci]);
            ci++;
        }

        const auto& x = solver.solve();

        // Dump matrix on step 0 for debugging
        if (stepCount == 0) {
            fprintf(stderr, "\nMatrix A (step 0):\n");
            for (size_t r = 0; r < n + m; r++) {
                for (size_t c = 0; c < n + m; c++) {
                    fprintf(stderr, "%10.4f ", solver.getA()(r, c));
                }
                fprintf(stderr, " | %10.4f\n", solver.getB()(r));
            }
            fprintf(stderr, "\n");
        }

        // Dump to CSV (first 200 steps, then every 1000th)
        if (stepCount < 200 || stepCount % 1000 == 0) {
            fprintf(fp, "%d,%.9f", stepCount, t);
            for (size_t i = 0; i < n; i++) fprintf(fp, ",%.9f", x(i));
            for (size_t i = n; i < n + m; i++) fprintf(fp, ",%.9f", x(i));
            fprintf(fp, "\n");
        }

        // Commit history
        ci = 0;
        for (const auto& comp : result.circuit.components()) {
            comp->commitHistory(x, extraAbs[ci]);
            ci++;
        }

        // Print first 5 steps to stderr for quick check
        if (stepCount < 5) {
            fprintf(stderr, "Step %d t=%.3e:", stepCount, t);
            for (size_t i = 0; i < n; i++) fprintf(stderr, " V%d=%.6f", (int)i+1, x(i));
            for (size_t i = n; i < n+m; i++) fprintf(stderr, " I%zu=%.6f", i-n, x(i));
            fprintf(stderr, "\n");
        }

        t += dt;
        stepCount++;
    }

    fclose(fp);
    fprintf(stderr, "Dumped %d steps to %s\n", stepCount, csvPath);
}
