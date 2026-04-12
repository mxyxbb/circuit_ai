#include "view_model/main_view_model.h"

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
        plot->addSignal(probe.name, ScopeModel::nextColor());
    }
}

void MainViewModel::play() {
    if (!circuit_ || circuit_->components().empty()) {
        statusMsg_ = "No circuit loaded";
        return;
    }
    simulator_->start();
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

bool MainViewModel::isSimRunning() const { return simulator_->isRunning(); }
bool MainViewModel::isSimPaused()  const { return simulator_->isPaused(); }
double MainViewModel::currentTime() const { return simulator_->currentTime(); }
