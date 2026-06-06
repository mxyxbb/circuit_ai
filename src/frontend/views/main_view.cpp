#include "views/main_view.h"
#include "view_model/main_view_model.h"
#include "platform/file_dialog.h"
#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

// Resolve relative to the exe directory so the file follows the binary regardless
// of CWD changes from native open/save dialogs.
static std::string kWinStateFile() { return platform::appDataPath("winstate.txt"); }

void MainView::performAutoSave(MainViewModel& vm) {
    schematicView_->performAutoSave(vm);
    // Save per-window visibility for the three docked panels.
    // Scope count/state is now per-sch (saved in each .sch file's XSCOPE blocks).
    std::ofstream wf(kWinStateFile());
    if (wf.good()) {
        wf << "settings=" << (settingsView_->isVisible()  ? 1 : 0) << '\n';
        wf << "palette="  << (paletteView_->isVisible()   ? 1 : 0) << '\n';
        wf << "schematic="<< (schematicView_->isVisible() ? 1 : 0) << '\n';
    }
}

MainView::MainView()
    : BaseView("CircuitAI"),
      settingsView_(std::make_unique<SettingsView>()),
      paletteView_(std::make_unique<PaletteView>()),
      schematicView_(std::make_unique<SchematicView>()) {
    scopeViews_.push_back(std::make_unique<ScopeView>(0));
    schematicView_->addScopeView(scopeViews_[0].get());
}

void MainView::render(MainViewModel& vm) {
    // Restore per-window visibility from last session (runs once)
    if (pendingStateLoad_) {
        pendingStateLoad_ = false;
        std::ifstream wf(kWinStateFile());
        if (wf.good()) {
            hasSavedSession_ = true;
            std::string line;
            while (std::getline(wf, line)) {
                auto eq = line.find('=');
                if (eq == std::string::npos) continue;
                std::string key = line.substr(0, eq);
                std::string val = line.substr(eq + 1);
                bool vis = (!val.empty() && val[0] == '1');
                if      (key == "settings")  settingsView_->setVisible(vis);
                else if (key == "palette")   paletteView_->setVisible(vis);
                else if (key == "schematic") schematicView_->setVisible(vis);
                // legacy "scopes=N" key is ignored — scope visibility is per-sch now
            }
        } else {
            // Fresh start (no saved session): palette + schematic open, rest hidden
            settingsView_->setVisible(false);
            for (auto& sv : scopeViews_) sv->setVisible(false);
        }
    }

    // Sync scope view list with vm scope count
    // (vm.addScope() is called by doLoad or the "New Scope" menu item)
    while ((int)scopeViews_.size() < vm.scopeCount()) {
        int idx = (int)scopeViews_.size();
        auto sv = std::make_unique<ScopeView>(idx);
        // Auto-synced scopes come from external requests (.sch load, etc.) —
        // surface them. Scopes the user keeps closed are not in vm at all.
        sv->setVisible(true);
        schematicView_->addScopeView(sv.get());
        scopeViews_.push_back(std::move(sv));
    }

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
        firstFrame_ = false;
        // Build the default layout only on first-ever run (no saved session and no imgui.ini layout).
        // If winstate.txt exists the user has a prior session — trust imgui.ini entirely.
        if (!hasSavedSession_ && ImGui::DockBuilderGetNode(dockspaceId) == nullptr) {
            ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dockspaceId, viewport->WorkSize);

            ImGuiID dockLeft, dockCenter;
            ImGui::DockBuilderSplitNode(dockspaceId, ImGuiDir_Left, 0.22f, &dockLeft, &dockCenter);

            // Palette and Settings share the left panel as tabs
            ImGui::DockBuilderDockWindow(paletteView_->title().c_str(), dockLeft);
            ImGui::DockBuilderDockWindow(settingsView_->title().c_str(), dockLeft);
            // Schematic fills the center; scope is not docked (floats when created)
            ImGui::DockBuilderDockWindow(schematicView_->title().c_str(), dockCenter);

            ImGui::DockBuilderFinish(dockspaceId);
        }
    }

    ImGui::DockSpace(dockspaceId, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);

    // Menu bar
    if (ImGui::BeginMenuBar()) {
        // ── File menu (acts on the active schematic) ──────────────────────
        if (ImGui::BeginMenu("File")) {
#ifdef _WIN32
            if (ImGui::MenuItem("Save", "Ctrl+S"))      schematicView_->fileSave(vm);
            if (ImGui::MenuItem("Save As..."))          schematicView_->fileSaveAs(vm);
            if (ImGui::MenuItem("Load..."))             schematicView_->fileLoad(vm);
            ImGui::Separator();
            if (ImGui::MenuItem("Export SVG..."))       schematicView_->fileExportSvg(vm);
            if (ImGui::MenuItem("Copy IMG"))            schematicView_->fileCopyImg(vm);
            ImGui::Separator();
            ImGui::TextDisabled("Export scale");
            ImGui::SetNextItemWidth(120.0f);
            ImGui::DragFloat("##svgscale_main", &schematicView_->svgExportScaleRef(),
                             0.1f, 0.5f, 10.0f, "%.1fx");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Output size multiplier for Export SVG / Copy IMG.\n"
                    "1.0x = canvas units; 2.0x ≈ 192-DPI feel.");
#else
            ImGui::TextDisabled("File ops are Windows-only.");
#endif
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            for (auto& sv : scopeViews_) {
                bool vis = sv->isVisible();
                if (ImGui::MenuItem(sv->title().c_str(), nullptr, &vis))
                    sv->setVisible(vis);
            }
            if (ImGui::MenuItem("New Scope")) {
                int idx = vm.addScope();
                auto sv = std::make_unique<ScopeView>(idx);
                sv->setCenterOnFirstRender();
                sv->setVisible(true);
                schematicView_->addScopeView(sv.get());
                scopeViews_.push_back(std::move(sv));
            }
            ImGui::Separator();
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

    for (auto& sv : scopeViews_)
        if (sv->isVisible()) sv->render(vm);
    if (settingsView_->isVisible()) settingsView_->render(vm);
    if (paletteView_->isVisible())  paletteView_->render(vm);
    schematicView_->render(vm);  // always called: pendingAutoLoad_ must fire regardless of visibility

    // Remove scope views that were closed this frame (iterate from end so
    // re-indexing of later entries doesn't affect earlier iteration).
    for (int i = (int)scopeViews_.size() - 1; i >= 0; i--) {
        if (!scopeViews_[i]->isVisible()) {
            int removedIdx = scopeViews_[i]->scopeIndex();
            schematicView_->removeScopeView(scopeViews_[i].get());
            vm.removeScope(removedIdx);
            scopeViews_.erase(scopeViews_.begin() + i);
            // Decrement scopeIdx_ for all views whose model shifted down
            for (auto& sv : scopeViews_)
                if (sv->scopeIndex() > removedIdx)
                    sv->setScopeIndex(sv->scopeIndex() - 1);
        }
    }

    // Handle deferred sch tab-close requested by SchematicView this frame.
    // Done after all rendering so we never tear down a view that's mid-render.
    int closeIdx = schematicView_->takePendingCloseDoc();
    if (closeIdx >= 0 && closeIdx < vm.schDocCount()) {
        int closingDocId = vm.schDoc(closeIdx).id;

        // 1. Snapshot the scope indices owned solely by the closing doc.
        std::vector<int> scopesToRemove = vm.scopesOwnedByDoc(closeIdx);
        std::sort(scopesToRemove.rbegin(), scopesToRemove.rend());  // descending

        // 2. If the closing doc drove the most recent build, its scopes carry
        //    that simulation's data — stop the simulator and discard the
        //    associated circuit/probes/raw cache so nothing keeps writing into
        //    buffers that are about to be destroyed.
        if (vm.lastBuiltDocId() == closingDocId)
            vm.stopAndClearSim();

        // 3. Tear down each owned scope: remove its ScopeView, shift remaining
        //    scopeIdx_ values, then remove the scope model.
        for (int sIdx : scopesToRemove) {
            for (int i = (int)scopeViews_.size() - 1; i >= 0; i--) {
                if (scopeViews_[i]->scopeIndex() == sIdx) {
                    schematicView_->removeScopeView(scopeViews_[i].get());
                    scopeViews_.erase(scopeViews_.begin() + i);
                    break;
                }
            }
            for (auto& sv : scopeViews_)
                if (sv->scopeIndex() > sIdx) sv->setScopeIndex(sv->scopeIndex() - 1);
            vm.removeScope(sIdx);
        }

        // 4. Finally drop the doc itself. closeSchDoc's own scope-removal pass
        //    is now a no-op since we already removed the owned scopes above.
        vm.closeSchDoc(closeIdx);
    }
}
