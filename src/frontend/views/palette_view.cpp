#include "views/palette_view.h"
#include "view_model/main_view_model.h"
#include "view_model/schematic_model.h"
#include <imgui.h>

void PaletteView::render(MainViewModel& vm) {
    if (!visible_) return;
    if (!ImGui::Begin(title_.c_str(), &visible_)) {
        ImGui::End();
        return;
    }

    ImGui::TextDisabled("Drag to Schematic canvas:");
    ImGui::Separator();

    const float btnW = ImGui::GetContentRegionAvail().x;

    for (const auto& type : SchematicModel::compTypes()) {
        ImGui::PushID(type.id.c_str());

        // Full-width button as the drag source
        ImGui::Button(type.displayName.c_str(), {btnW, 26.0f});

        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None)) {
            // Payload: null-terminated type id string
            ImGui::SetDragDropPayload("COMP_TYPE",
                                      type.id.c_str(),
                                      type.id.size() + 1);
            ImGui::Text("+ %s", type.displayName.c_str());
            ImGui::EndDragDropSource();
        }

        // Tooltip on hover
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
            ImGui::BeginTooltip();
            ImGui::Text("%s", type.displayName.c_str());
            if (!type.params.empty()) {
                ImGui::Separator();
                for (const auto& pd : type.params)
                    ImGui::TextDisabled("  %s = %s", pd.name.c_str(), pd.defaultValue.c_str());
            }
            if (!type.pins.empty()) {
                ImGui::Separator();
                for (const auto& pin : type.pins)
                    ImGui::TextDisabled("  pin: %s", pin.label.c_str());
            }
            ImGui::EndTooltip();
        }

        ImGui::PopID();
    }

    ImGui::End();
}
