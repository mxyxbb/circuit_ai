#pragma once
#include "views/base_view.h"
#include "views/scope_view.h"
#include "views/settings_view.h"
#include "views/palette_view.h"
#include "views/schematic_view.h"
#include <memory>
#include <vector>

class MainView : public BaseView {
public:
    MainView();
    void render(MainViewModel& vm) override;
    void performAutoSave(MainViewModel& vm);

private:
    std::vector<std::unique_ptr<ScopeView>> scopeViews_;
    std::unique_ptr<SettingsView>           settingsView_;
    std::unique_ptr<PaletteView>            paletteView_;
    std::unique_ptr<SchematicView>          schematicView_;
    bool firstFrame_        = true;
    bool pendingStateLoad_  = true;
    bool hasSavedSession_   = false;
};
