#include "views/palette_view.h"
#include <imgui.h>

void PaletteView::render(MainViewModel& vm) {
    if (!visible_) return;
    ImGui::Begin(title_.c_str(), &visible_);
    ImGui::TextDisabled("Component Palette (Coming Soon)");
    ImGui::End();
}
