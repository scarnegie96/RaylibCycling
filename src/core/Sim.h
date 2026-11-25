#pragma once
// Virtual trainer + rider and "test mode" (port of CoreKick sim.rs).
#include <random>

#include "Engine.h"

namespace sim {

enum class PowerSource { Rider, Fixed };
enum class HrSource { Model, Fixed, Off };
enum class CadenceSource { Auto, Fixed };
enum class Pattern { Steady, Surges, Wave, Fade };

struct Settings {
    bool enabled = false;
    double effortW = 175;
    PowerSource powerSource = PowerSource::Rider;
    double powerW = 200;
    Pattern pattern = Pattern::Steady;
    HrSource hrSource = HrSource::Model;
    double hrBpm = 135;
    CadenceSource cadenceSource = CadenceSource::Auto;
    double cadenceRpm = 90;
    double variability = 0.3;  // 0 clean .. 1 very noisy
    bool dropout = false;
    Settings clamped() const;
};

double patternFactor(Pattern p, double t);
const char* patternName(Pattern p);

class SimTrainer {
public:
    explicit SimTrainer(const Profile& p);
    void set(const Settings& s);
    const Settings& settings() const { return s_; }
    void command(const TrainerCommand& c) { mode_ = c; hasMode_ = true; }
    TrainerInput update(double dt, const Profile& p);

private:
    Settings s_;
    double power_ = 0, cadence_ = 0, hr_;
    TrainerCommand mode_{};
    bool hasMode_ = false;
    std::mt19937 rng_{0xC0FFEE};
    std::normal_distribution<double> N_{0.0, 1.0};
    double patternT_ = 0;
};

}  // namespace sim
