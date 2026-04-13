#include "views/schematic_view.h"
#include <imgui.h>

void SchematicView::render(MainViewModel& vm) {
    if (!visible_) return;
    if (!ImGui::Begin(title_.c_str(), &visible_)) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("Schematic Canvas (Coming Soon)");
    ImGui::End();
}
