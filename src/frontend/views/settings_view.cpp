#include "views/settings_view.h"
#include "view_model/main_view_model.h"
#include "platform/file_dialog.h"
#include <imgui.h>
#include <cstring>

SettingsView::SettingsView() : BaseView("Simulation Settings") {}

void SettingsView::render(MainViewModel& vm) {
    if (!visible_) return;
    if (!ImGui::Begin(title_.c_str(), &visible_)) {
        ImGui::End();
        return;
    }

    // Netlist file loader
    ImGui::Text("Netlist File:");
    ImGui::InputText("##path", netlistPath_, sizeof(netlistPath_));
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        if (vm.loadNetlist(netlistPath_)) {
            dtInput_    = vm.simConfig().dt;
            tEndInput_  = vm.simConfig().t_end;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        std::string path = platform::openFileDialog();
        if (!path.empty()) {
            strncpy(netlistPath_, path.c_str(), sizeof(netlistPath_) - 1);
            netlistPath_[sizeof(netlistPath_) - 1] = '\0';
            if (vm.loadNetlist(netlistPath_)) {
                dtInput_   = vm.simConfig().dt;
                tEndInput_ = vm.simConfig().t_end;
            }
        }
    }

    ImGui::Separator();

    // Simulation parameters
    ImGui::Text("Parameters:");
    ImGui::InputDouble("Step dt (s)", &dtInput_, 0.0, 0.0, "%.2e");
    ImGui::InputDouble("End time (s)", &tEndInput_, 0.0, 0.0, "%.2e");

    // Informational: show estimated stored sample count (always ratio=1)
    if (dtInput_ > 0.0 && tEndInput_ > 0.0) {
        size_t estPts = static_cast<size_t>(tEndInput_ / dtInput_) + 1;
        if (estPts > 5000000) estPts = 5000000; // capped
        ImGui::TextDisabled("  ~%llu pts stored (capped at 5M)", static_cast<unsigned long long>(estPts));
    }

    ImGui::Separator();

    // Control buttons
    if (vm.isSimRunning() && !vm.isSimPaused()) {
        if (ImGui::Button("Pause")) vm.pause();
        ImGui::SameLine();
    }
    if (ImGui::Button("Run")) {   // always resets to t=0 before starting
        vm.applySimConfig(dtInput_, tEndInput_);
        vm.play();
    }

    ImGui::Separator();

    // Status
    ImGui::Text("Status: %s", vm.statusMessage().c_str());
    ImGui::Text("Sim time: %.6f s", vm.currentTime());

    // Available signals list
    ImGui::Separator();
    ImGui::Text("Available Signals:");
    for (const auto& sig : vm.availableSignals()) {
        ImGui::BulletText("%s", sig.name.c_str());
    }

    // ── Diagnostics log ────────────────────────────────────────────────────────
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Diagnostics", ImGuiTreeNodeFlags_DefaultOpen)) {
        const auto& log = vm.diagLog();
        bool newEvents = (log.size() != lastDiagCount_);
        lastDiagCount_ = log.size();

        if (ImGui::Button("Clear##diag")) {
            vm.clearDiagLog();
            lastDiagCount_ = 0;
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%zu entries)", log.size());

        ImGui::BeginChild("##diaglog", ImVec2(0.0f, 180.0f), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& ev : log) {
            ImVec4 col = (ev.level == DiagEvent::Warning) ? ImVec4(1.0f, 0.9f, 0.2f, 1.0f) :
                         (ev.level == DiagEvent::Error)   ? ImVec4(1.0f, 0.3f, 0.3f, 1.0f) :
                                                             ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
            ImGui::TextColored(col, "%s", ev.message.c_str());
        }
        if (newEvents)
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }

    ImGui::End();
}
