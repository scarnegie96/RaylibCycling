#include "Sim.h"

#include <algorithm>
#include <cmath>

namespace sim {

Settings Settings::clamped() const {
    Settings s = *this;
    s.effortW = std::clamp(s.effortW, 0.0, 1500.0);
    s.powerW = std::clamp(s.powerW, 0.0, 2000.0);
    s.hrBpm = std::clamp(s.hrBpm, 30.0, 230.0);
    s.cadenceRpm = std::clamp(s.cadenceRpm, 0.0, 180.0);
    s.variability = std::isfinite(s.variability) ? std::clamp(s.variability, 0.0, 1.0) : 0.3;
    return s;
}

double patternFactor(Pattern p, double t) {
    switch (p) {
        case Pattern::Steady: return 1.0;
        case Pattern::Surges: {
            double c = std::fmod(t, 90.0);
            if (c < 0) c += 90;
            return (c >= 30 && c < 42) ? 1.6 : 1.0;
        }
        case Pattern::Wave: return 1.0 + 0.2 * std::sin(t * 2 * M_PI / 60.0);
        case Pattern::Fade: return std::max(0.7, 1.0 - t / 60.0 * 0.01);
    }
    return 1.0;
}

const char* patternName(Pattern p) {
    switch (p) {
        case Pattern::Steady: return "Steady";
        case Pattern::Surges: return "Surges";
        case Pattern::Wave: return "Wave";
        case Pattern::Fade: return "Fade";
    }
    return "";
}

SimTrainer::SimTrainer(const Profile& p) {
    s_.effortW = std::round(p.ftp * 0.7);
    s_.powerW = std::round(p.ftp * 0.75);
    hr_ = p.restHr;
}

void SimTrainer::set(const Settings& in) {
    Settings s = in.clamped();
    if (s.pattern != s_.pattern || s.powerSource != s_.powerSource) patternT_ = 0;
    s_ = s;
}

TrainerInput SimTrainer::update(double dt, const Profile& prof) {
    const Settings s = s_;
    patternT_ += dt;
    double k = patternFactor(s.pattern, patternT_);
    double noise = s.variability / 0.3;

    double target, cadTarget, lag;
    if (s.powerSource == PowerSource::Fixed) {
        double w = s.powerW * k;
        target = w; cadTarget = w > 0 ? 90 : 0; lag = 0.8;
    } else {
        double effort = std::max(0.0, s.effortW * k);
        if (!hasMode_) { target = effort; cadTarget = effort > 0 ? 88 : 0; }
        else if (mode_.type == TrainerCommand::TargetPower) {
            double watts = mode_.watts;
            if (effort > 0 && watts > effort * 1.35) { target = watts * 0.35; cadTarget = 38; }
            else if (effort <= 0) { target = 0; cadTarget = 0; }
            else { target = watts; cadTarget = 90; }
        } else if (mode_.type == TrainerCommand::Resistance) {
            if (effort <= 0) { target = 0; cadTarget = 0; }
            else { target = effort * (0.55 + mode_.levelPct / 100.0); cadTarget = 88; }
        } else {
            target = effort;
            cadTarget = effort <= 0 ? 0 : std::clamp(92.0 - std::max(0.0, mode_.gradePct) * 2.0, 65.0, 100.0);
        }
        lag = 1.5;
    }
    if (s.cadenceSource == CadenceSource::Fixed) cadTarget = target > 0 ? s.cadenceRpm : 0;
    power_ += (target - power_) * std::min(1.0, dt / lag);
    cadence_ += (cadTarget - cadence_) * std::min(1.0, dt / 1.0);
    double power = std::max(0.0, power_ * (1.0 + N_(rng_) * 0.025 * noise));
    double cadence = std::max(0.0, cadence_ + N_(rng_) * 1.2 * noise);

    std::optional<double> hr;
    switch (s.hrSource) {
        case HrSource::Off: break;
        case HrSource::Fixed:
            hr_ += (s.hrBpm - hr_) * std::min(1.0, dt / 3.0);
            hr = std::round(hr_ + N_(rng_) * 0.8 * noise);
            break;
        case HrSource::Model: {
            double ftp = std::max(prof.ftp, 80.0);
            double rest = prof.restHr, mx = std::max(prof.maxHr, prof.restHr + 40);
            double frac = std::clamp(0.30 + 0.62 * power_ / ftp, 0.0, 1.0);
            double tgt = power_ < 5 ? rest + 15 : rest + (mx - rest) * frac;
            hr_ += (tgt - hr_) * std::min(1.0, dt / 25.0);
            hr = std::round(hr_);
            break;
        }
    }
    if (s.dropout) return {0, 0, hr, false};
    return {power_ < 1 ? 0.0 : std::round(power), cadence_ < 5 ? 0.0 : std::round(cadence), hr, true};
}

}  // namespace sim
