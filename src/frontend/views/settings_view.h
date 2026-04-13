#pragma once
#include "views/base_view.h"

class SettingsView : public BaseView {
public:
    SettingsView();
    void render(MainViewModel& vm) override;

private:
    char netlistPath_[260] = "";
    double dtInput_ = 1e-6;
    double tEndInput_ = 0.01;
};
