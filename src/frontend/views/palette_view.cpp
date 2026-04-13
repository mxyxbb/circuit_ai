#include "views/palette_view.h"
#include <imgui.h>

void PaletteView::render(MainViewModel& vm) {
    if (!visible_) return;
    if (!ImGui::Begin(title_.c_str(), &visible_)) {
        ImGui::End();
        return;
    }
    ImGui::TextDisabled("Component Palette (Coming Soon)");
    ImGui::End();
}
