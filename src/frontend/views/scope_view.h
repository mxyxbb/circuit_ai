#pragma once
#include "views/base_view.h"

class ScopeView : public BaseView {
public:
    ScopeView();

    void render(MainViewModel& vm) override;

private:
    float historySeconds_ = 5.0f;

    void renderPlot(MainViewModel& vm, class PlotArea& plot, int plotIndex);
    void renderPlotContextMenu(MainViewModel& vm, int plotIndex);
    void renderAddSignalMenu(MainViewModel& vm, int plotIndex);
};
