#pragma once
#include "views/base_view.h"

class SettingsView : public BaseView {
public:
    SettingsView();
    void render(MainViewModel& vm) override;

private:
    size_t lastDiagCount_ = 0;
};
