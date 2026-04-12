#pragma once
#include "components/base_component.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class Circuit {
public:
    void addComponent(std::unique_ptr<BaseComponent> comp) {
        int mn = comp->maxNode();
        if (mn > maxNode_) maxNode_ = mn;
        totalExtraVars_ += comp->extraVariableCount();
        nameIndex_[comp->name()] = comp.get();
        components_.push_back(std::move(comp));
    }

    int nodeCount() const { return maxNode_; }
    size_t extraVarCount() const { return totalExtraVars_; }
    const std::vector<std::unique_ptr<BaseComponent>>& components() const { return components_; }

    const BaseComponent* findComponent(const std::string& name) const {
        auto it = nameIndex_.find(name);
        return (it != nameIndex_.end()) ? it->second : nullptr;
    }

private:
    std::vector<std::unique_ptr<BaseComponent>> components_;
    std::unordered_map<std::string, BaseComponent*> nameIndex_;
    int maxNode_ = 0;
    size_t totalExtraVars_ = 0;
};
