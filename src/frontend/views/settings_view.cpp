#include "views/settings_view.h"
#include "view_model/main_view_model.h"
#include <imgui.h>

SettingsView::SettingsView() : BaseView("Simulation Settings") {}

void SettingsView::render(MainViewModel& vm) {
    if (!visible_) return;
    ImGui::Begin(title_.c_str(), &visible_);

    // Netlist file loader
    ImGui::Text("Netlist File:");
    ImGui::InputText("##path", netlistPath_, sizeof(netlistPath_));
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        vm.loadNetlist(netlistPath_);
    }

    ImGui::Separator();

    // Simulation parameters
    ImGui::Text("Parameters:");
    ImGui::InputDouble("Step dt (s)", &dtInput_, 0.0, 0.0, "%.2e");
    ImGui::InputDouble("End time (s)", &tEndInput_, 0.0, 0.0, "%.2e");

    ImGui::Separator();

    // Control buttons
    if (vm.isSimRunning() && !vm.isSimPaused()) {
        if (ImGui::Button("Pause")) vm.pause();
    } else {
        if (ImGui::Button("Run")) vm.play();
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) vm.reset();

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

    ImGui::End();
}
