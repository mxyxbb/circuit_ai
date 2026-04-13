#include "views/main_view.h"
#include "view_model/main_view_model.h"
#include <imgui.h>
#include <imgui_internal.h>

MainView::MainView()
    : BaseView("CircuitAI"),
      scopeView_(std::make_unique<ScopeView>()),
      settingsView_(std::make_unique<SettingsView>()),
      paletteView_(std::make_unique<PaletteView>()),
      schematicView_(std::make_unique<SchematicView>()) {}

void MainView::render(MainViewModel& vm) {
    // Setup DockSpace on first frame
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::SetNextWindowViewport(viewport->ID);

    ImGuiWindowFlags dockspaceFlags = ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_NoTitleBar |
                                       ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoBringToFrontOnFocus |
                                       ImGuiWindowFlags_NoNavFocus |
                                       ImGuiWindowFlags_MenuBar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    bool dockspaceOpen = ImGui::Begin("DockSpace", nullptr, dockspaceFlags);
    ImGui::PopStyleVar(3);

    ImGuiID dockspaceId = ImGui::GetID("MainDockSpace");

    if (dockspaceOpen && firstFrame_) {
        ImGui::DockBuilderRemoveNode(dockspaceId);
        ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

        ImGuiID dockLeft, dockCenter, dockRight;
        ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.2f, &dockLeft, &dockCenter);
        ImGuiID dockCenterTop, dockCenterBottom;
        ImGui::DockBuilderSplitNode(dockCenter, ImGuiDir_Up, 0.2f, &dockCenterTop, &dockCenterBottom);

        ImGui::DockBuilderDockWindow(settingsView_->title().c_str(), dockLeft);
        ImGui::DockBuilderDockWindow(paletteView_->title().c_str(), dockLeft);
        ImGui::DockBuilderDockWindow(scopeView_->title().c_str(), dockCenterBottom);
        ImGui::DockBuilderDockWindow(schematicView_->title().c_str(), dockCenterTop);

        ImGui::DockBuilderFinish(dockspaceId);
        firstFrame_ = false;
    }

    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    // Menu bar
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("View")) {
            bool sv = scopeView_->isVisible();
            if (ImGui::MenuItem(scopeView_->title().c_str(), nullptr, &sv))
                scopeView_->setVisible(sv);
            bool stv = settingsView_->isVisible();
            if (ImGui::MenuItem(settingsView_->title().c_str(), nullptr, &stv))
                settingsView_->setVisible(stv);
            bool pv = paletteView_->isVisible();
            if (ImGui::MenuItem(paletteView_->title().c_str(), nullptr, &pv))
                paletteView_->setVisible(pv);
            bool scv = schematicView_->isVisible();
            if (ImGui::MenuItem(schematicView_->title().c_str(), nullptr, &scv))
                schematicView_->setVisible(scv);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    ImGui::End(); // DockSpace

    // Render sub-views
    if (scopeView_->isVisible()) scopeView_->render(vm);
    if (settingsView_->isVisible()) settingsView_->render(vm);
    if (paletteView_->isVisible()) paletteView_->render(vm);
    if (schematicView_->isVisible()) schematicView_->render(vm);
}
