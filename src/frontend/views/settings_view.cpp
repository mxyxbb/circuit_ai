#include "views/settings_view.h"
#include "view_model/main_view_model.h"
#include <imgui.h>
#include <string>

SettingsView::SettingsView() : BaseView("Simulation Settings") {}

void SettingsView::render(MainViewModel& vm) {
    if (!visible_) return;
    if (!ImGui::Begin(title_.c_str(), &visible_)) {
        ImGui::End();
        return;
    }

    // ── Memory budget ──────────────────────────────────────────────────────
    // Per-sch buffers multiply memory usage by the number of open schematics,
    // so expose the per-signal sample cap directly. 16 B/sample × N samples
    // × M signals × K schematics ≈ total memory.
    {
        size_t cur = vm.maxStoredSamples();
        int curM = (int)((cur * 16ull) / (1024ull * 1024ull));  // MB per signal
        if (curM < 1) curM = 1;
        ImGui::Text("Max memory per signal:");
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::InputInt("MB##maxmem", &curM, 1, 16)) {
            if (curM < 1) curM = 1;
            if (curM > 4096) curM = 4096;  // 4 GB sanity cap
            size_t newSamples = ((size_t)curM * 1024ull * 1024ull) / 16ull;
            vm.setMaxStoredSamples(newSamples);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(= %zu samples)", vm.maxStoredSamples());
        ImGui::TextDisabled("Multiplied by #signals × #open schematics. Applies to new buffers only.");
        ImGui::Separator();
    }

    // ── Available Signals ──────────────────────────────────────────────────
    ImGui::Text("Available Signals:");
    const auto& sigs = vm.availableSignals();
    if (sigs.empty()) {
        ImGui::TextDisabled("  (none — build from schematic first)");
    } else {
        for (const auto& sig : sigs)
            ImGui::BulletText("%s", sig.name.c_str());
    }

    // ── Diagnostics ────────────────────────────────────────────────────────
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
        if (ImGui::Button("Copy All##diag")) {
            std::string all;
            for (const auto& ev : log) {
                all += ev.message;
                all += '\n';
            }
            ImGui::SetClipboardText(all.c_str());
        }
        ImGui::SameLine();
        ImGui::TextDisabled("(%zu entries)", log.size());

        ImGui::BeginChild("##diaglog", ImVec2(0.0f, 0.0f), true,
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
