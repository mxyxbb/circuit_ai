#include "view_model/main_view_model.h"
#include <algorithm>

// Calculate the ScrollingBuffer capacity needed to store all simulation samples
// at sample_ratio=1.  Capped at MAX_STORED to bound memory usage.
// (~5M doubles × 8 bytes × 2 arrays ≈ 80 MB per signal)
static size_t calcBufferCapacity(double tEnd, double dt) {
    constexpr size_t MAX_STORED = 5'000'000;
    if (tEnd <= 0.0 || dt <= 0.0) return 10000;
    size_t needed = static_cast<size_t>(tEnd / dt) + 1;
    return std::min(needed, MAX_STORED);
}

MainViewModel::MainViewModel()
    : simulator_(std::make_unique<Simulator>()),
      circuit_(std::make_unique<Circuit>()) {}

MainViewModel::~MainViewModel() {
    simulator_->stop();
}

bool MainViewModel::loadNetlist(const std::string& filepath) {
    simulator_->stop();
    circuit_ = std::make_unique<Circuit>();
    scope_.clearAllBuffers();
    signalNameToIdx_.clear();

    NetlistParser parser;
    ParseResult result = parser.parse(filepath);

    if (!result.success) {
        statusMsg_ = "Error line " + std::to_string(result.errorLine) + ": " + result.error;
        return false;
    }

    // Transfer parsed data
    circuit_ = std::make_unique<Circuit>(std::move(result.circuit));
    config_ = result.config;
    probes_ = result.probes;
    netlistPath_ = filepath;

    // Setup simulator
    if (!simulator_->setup(*circuit_, config_, probes_)) {
        statusMsg_ = "Failed to setup simulator";
        return false;
    }

    // Build signal name index
    for (size_t i = 0; i < probes_.size(); i++) {
        signalNameToIdx_[probes_[i].name] = i;
    }

    // Storage side: always ratio=1 (no downsampling); cap buffer at 5M points
    config_.sample_ratio = 1;
    scope_.setBufferCapacity(calcBufferCapacity(config_.t_end, config_.dt));

    // Auto-populate scope with all probe signals in first plot
    autoPopulateScope();

    statusMsg_ = "Loaded: " + filepath + " (" +
                 std::to_string(circuit_->components().size()) + " components, " +
                 std::to_string(probes_.size()) + " probes)";
    return true;
}

void MainViewModel::autoPopulateScope() {
    // Clear existing scope content
    scope_ = ScopeModel();

    // Add all probe signals to the first plot
    PlotArea* plot = scope_.getPlot(0);
    if (!plot) return;

    ScopeModel::resetColorIndex();
    for (const auto& probe : probes_) {
        plot->addSignal(probe.name, ScopeModel::nextColor(), scope_.getBufferCapacity());
    }
}

void MainViewModel::play() {
    if (!circuit_ || circuit_->components().empty()) {
        statusMsg_ = "No circuit loaded";
        return;
    }
    // Always reset to t=0 and clear component state before starting
    simulator_->reset();       // stops thread, resets component history and time to 0
    scope_.clearAllBuffers();  // clear frontend waveform data
    // reset() only restarts if the sim was already running; ensure it's started
    if (!simulator_->isRunning()) {
        simulator_->start();
    }
    statusMsg_ = "Simulation running";
}

void MainViewModel::pause() {
    simulator_->pause();
    statusMsg_ = "Simulation paused";
}

void MainViewModel::reset() {
    simulator_->reset();
    scope_.clearAllBuffers();
    statusMsg_ = "Simulation reset";
}

void MainViewModel::update() {
    // Drain all available samples from the ring buffer
    SimSample sample;
    while (simulator_->consumeSample(sample)) {
        dispatchSample(sample);
    }
}

void MainViewModel::dispatchSample(const SimSample& sample) {
    // For each signal in each plot, push data
    for (int pi = 0; pi < scope_.plotCount(); pi++) {
        PlotArea* plot = scope_.getPlot(pi);
        if (!plot) continue;
        for (auto& entry : plot->entries) {
            auto it = signalNameToIdx_.find(entry->signalName);
            if (it != signalNameToIdx_.end() && it->second < sample.values.size()) {
                entry->buffer.push(sample.time, sample.values[it->second]);
            }
        }
    }
}

void MainViewModel::applySimConfig(double dt, double tEnd) {
    if (dt <= 0.0 || tEnd <= 0.0) return;

    config_.dt    = dt;
    config_.t_end = tEnd;

    // Storage side: always ratio=1; rebuild buffers with new capacity
    config_.sample_ratio = 1;
    scope_.resizeAllBuffers(calcBufferCapacity(tEnd, dt));

    // Re-initialise simulator with updated config (resets to t=0)
    if (circuit_ && !circuit_->components().empty()) {
        simulator_->stop();
        simulator_->setup(*circuit_, config_, probes_);
    }

    statusMsg_ = "Config applied: dt=" + std::to_string(dt)
               + " t_end=" + std::to_string(tEnd);
}

bool MainViewModel::isSimRunning() const { return simulator_->isRunning(); }
bool MainViewModel::isSimPaused()  const { return simulator_->isPaused(); }
double MainViewModel::currentTime() const { return simulator_->currentTime(); }
