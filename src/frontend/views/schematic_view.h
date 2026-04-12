#pragma once
#include "views/base_view.h"

class SchematicView : public BaseView {
public:
    SchematicView() : BaseView("Schematic") {}
    void render(MainViewModel& vm) override;
};
