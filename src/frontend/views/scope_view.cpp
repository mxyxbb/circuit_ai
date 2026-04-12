#include "views/scope_view.h"
#include "view_model/main_view_model.h"
#include "view_model/scope_model.h"
#include <imgui.h>
#include <implot.h>

ScopeView::ScopeView() : BaseView("Scope") {}

void ScopeView::render(MainViewModel& vm) {
    if (!visible_) return;
    ImGui::Begin(title_.c_str(), &visible_, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    ScopeModel& scope = vm.scope();

    // Toolbar
    if (ImGui::Button("+ Plot Above")) {
        int sel = scope.selectedPlot();
        scope.insertPlot(sel - 1);
    }
    ImGui::SameLine();
    if (ImGui::Button("+ Plot Below")) {
        int sel = scope.selectedPlot();
        scope.insertPlot(sel);
    }
    ImGui::SameLine();
    if (ImGui::Button("Auto-Fit All")) {
        scope.autoFitAll();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("| Sim time: %.6f s", vm.currentTime());

    ImGui::Separator();

    // Render each plot
    for (int i = 0; i < scope.plotCount(); i++) {
        ImGui::PushID(i);
        PlotArea* plot = scope.getPlot(i);
        if (plot) renderPlot(vm, *plot, i);
        ImGui::PopID();
        if (i < scope.plotCount() - 1) ImGui::Separator();
    }

    ImGui::End();
}

void ScopeView::renderPlot(MainViewModel& vm, PlotArea& plot, int plotIndex) {
    ScopeModel& scope = vm.scope();
    float plotHeight = std::max(150.0f, (ImGui::GetContentRegionAvail().y - 40) / scope.plotCount());

    bool selected = (plotIndex == scope.selectedPlot());
    if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.4f, 0.7f, 1.0f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    }

    if (ImPlot::BeginPlot(plot.title.c_str(), ImVec2(-1, plotHeight))) {
        ImPlot::SetupAxes("Time (s)", "Value");
        ImPlot::SetupAxisScale(ImAxis_X1, ImPlotScale_Linear);
        ImPlot::SetupAxisFormat(ImAxis_X1, "%.4f");

        if (plot.autoFitY) {
            ImPlot::SetupAxisScale(ImAxis_Y1, ImPlotScale_Linear);
        }

        for (auto& entry : plot.entries) {
            if (!entry->visible) continue;
            ImVec4 col(
                ((entry->color >> 0) & 0xFF) / 255.0f,
                ((entry->color >> 8) & 0xFF) / 255.0f,
                ((entry->color >> 16) & 0xFF) / 255.0f,
                1.0f);
            ImPlotSpec spec(ImPlotProp_LineColor, col, ImPlotProp_LineWeight, 1.5f);

            // Handle ring buffer: data may wrap around
            int count = entry->buffer.getCount();
            int offset = entry->buffer.getOffset();
            if (count > 0) {
                if (offset >= count) {
                    ImPlot::PlotLine(entry->signalName.c_str(),
                                     entry->buffer.getXData(),
                                     entry->buffer.getYData(), count, spec);
                } else {
                    int seg1 = count - offset;
                    const double* xd = entry->buffer.getXData();
                    const double* yd = entry->buffer.getYData();
                    ImPlot::PlotLine(entry->signalName.c_str(),
                                     xd + offset, yd + offset, seg1, spec);
                    ImPlot::PlotLine("##seg2", xd, yd, offset, spec);
                }
            }
        }

        // Click to select
        if (ImPlot::IsPlotHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            scope.setSelectedPlot(plotIndex);
        }

        // Right-click context menu
        if (ImGui::BeginPopupContextItem("PlotCtx")) {
            renderPlotContextMenu(vm, plotIndex);
            ImGui::EndPopup();
        }
        // Open context on right-click in plot area
        if (ImPlot::IsPlotHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            ImGui::OpenPopup("PlotCtx");
        }

        ImPlot::EndPlot();
    }

    if (selected) {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }

    // Auto-fit button per plot
    ImGui::SameLine();
    plot.autoFitY = false; // will be set by ImPlot interaction
}

void ScopeView::renderPlotContextMenu(MainViewModel& vm, int plotIndex) {
    ScopeModel& scope = vm.scope();

    if (ImGui::MenuItem("Insert Plot Above")) {
        scope.insertPlot(plotIndex - 1);
    }
    if (ImGui::MenuItem("Insert Plot Below")) {
        scope.insertPlot(plotIndex);
    }
    if (ImGui::MenuItem("Delete Plot", nullptr, false, scope.plotCount() > 1)) {
        scope.removePlot(plotIndex);
    }
    ImGui::Separator();
    renderAddSignalMenu(vm, plotIndex);
}

void ScopeView::renderAddSignalMenu(MainViewModel& vm, int plotIndex) {
    if (!ImGui::BeginMenu("Add Signal")) return;

    ScopeModel& scope = vm.scope();
    PlotArea* plot = scope.getPlot(plotIndex);

    for (const auto& sig : vm.availableSignals()) {
        bool alreadyAdded = plot && plot->findEntry(sig.name);
        if (ImGui::MenuItem(sig.name.c_str(), nullptr, alreadyAdded)) {
            if (!alreadyAdded && plot) {
                scope.addSignalToPlot(plotIndex, sig.name, ScopeModel::nextColor());
            }
        }
    }

    ImGui::EndMenu();
}
