#pragma once
#include "components/base_component.h"

class IdealDiode : public BaseComponent {
public:
    IdealDiode(std::string name, int anode, int cathode)
        : name_(std::move(name)), na_(anode), nk_(cathode), isOn_(false) {}

    std::string name() const override { return name_; }
    int maxNode() const override { return std::max(na_, nk_); }

    void stamp(MNASolver& solver, double, double, size_t) override {
        double r = isOn_ ? R_ON : R_OFF;
        solver.stampConductance(na_, nk_, 1.0 / r);
    }

    double getBranchCurrent(const Eigen::VectorXd& x, size_t) const override {
        double va = (na_ > 0) ? x(na_ - 1) : 0.0;
        double vk = (nk_ > 0) ? x(nk_ - 1) : 0.0;
        double r = isOn_ ? R_ON : R_OFF;
        return (va - vk) / r;
    }

    bool updateState(const Eigen::VectorXd& x, size_t) override {
        double va = (na_ > 0) ? x(na_ - 1) : 0.0;
        double vk = (nk_ > 0) ? x(nk_ - 1) : 0.0;
        double vak = va - vk;

        bool changed = false;
        if (isOn_ && vak < -V_THRESHOLD) {
            isOn_ = false;
            changed = true;
        } else if (!isOn_ && vak > V_THRESHOLD) {
            isOn_ = true;
            changed = true;
        }
        return changed;
    }

private:
    std::string name_;
    int na_, nk_;
    bool isOn_;
    static constexpr double R_ON  = 1e-3;
    static constexpr double R_OFF = 1e9;
    static constexpr double V_THRESHOLD = 1e-6;
};
