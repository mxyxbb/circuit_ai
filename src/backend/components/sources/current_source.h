#pragma once
#include "components/base_component.h"

class CurrentSource : public BaseComponent {
public:
    CurrentSource(std::string name, int np, int nn, double current)
        : name_(std::move(name)), np_(np), nn_(nn), i_(current) {}

    std::string name() const override { return name_; }
    int maxNode() const override { return std::max(np_, nn_); }

    void stamp(MNASolver& solver, double, double, size_t) override {
        solver.stampRhs(np_, i_);
        solver.stampRhs(nn_, -i_);
    }

private:
    std::string name_;
    int np_, nn_;
    double i_;
};
