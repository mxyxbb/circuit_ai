#pragma once
#include "views/base_view.h"

class PaletteView : public BaseView {
public:
    PaletteView() : BaseView("Component Palette") {}
    void render(MainViewModel& vm) override;
};
