#pragma once
#include <string>

class MainViewModel;

class BaseView {
public:
    explicit BaseView(std::string title) : title_(std::move(title)) {}
    virtual ~BaseView() = default;

    virtual void render(MainViewModel& vm) = 0;

    const std::string& title() const { return title_; }
    bool isVisible() const { return visible_; }
    void setVisible(bool v) { visible_ = v; }

protected:
    std::string title_;
    bool visible_ = true;
};
