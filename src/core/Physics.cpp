#include "Physics.h"

#include <algorithm>
#include <cmath>

namespace phys {

double airDensity(double ele) {
    double e = std::clamp(ele, -400.0, 9000.0);
    return 1.225 * std::pow(1.0 - 2.25577e-5 * e, 4.2559);
}

double Params::cw() const { return 0.5 * airDensity(0.0) * cda; }

double resistiveForce(double v, double grade, double ele, const Params& p) {
    double th = std::atan(grade);
    double va = v + p.windMps;
    return p.massKg * G * std::sin(th) + p.crr * p.massKg * G * std::cos(th) + 0.5 * airDensity(ele) * p.cda * va * std::fabs(va);
}

double powerForSpeed(double v, double grade, double ele, const Params& p) {
    return resistiveForce(v, grade, ele, p) * v / p.drivetrainEff;
}

double steadySpeed(double power, double grade, double ele, const Params& p) {
    auto f = [&](double v) { return power * p.drivetrainEff - resistiveForce(v, grade, ele, p) * v; };
    if (f(0.05) <= 0.0 && resistiveForce(0.0, grade, ele, p) >= 0.0) return 0.0;
    double lo = 0, hi = 60;
    for (int i = 0; i < 80; ++i) {
        double mid = 0.5 * (lo + hi);
        if (f(mid) > 0) lo = mid; else hi = mid;
    }
    return 0.5 * (lo + hi);
}

double integrateSpeed(double v0, double power, double grade, double ele, const Params& p, double dt) {
    if (dt <= 0) return v0;
    double mEff = p.massKg + ROTATIONAL_MASS_KG;
    int n = std::max(1, int(std::ceil(dt / 0.05)));
    double h = dt / n;
    double v = std::max(0.0, v0);
    for (int i = 0; i < n; ++i) {
        double prop = power > 0 ? power * p.drivetrainEff / std::max(v, MIN_PROPULSION_SPEED) : 0.0;
        double net = prop - resistiveForce(v, grade, ele, p);
        v = std::max(0.0, v + net / mEff * h);
    }
    return v;
}

const double VIRTUAL_GEARS[GEAR_COUNT] = {0.75, 0.87, 0.99, 1.11, 1.23, 1.38, 1.53, 1.68, 1.86, 2.04, 2.22, 2.40,
                                          2.61, 2.82, 3.03, 3.24, 3.49, 3.74, 3.99, 4.24, 4.54, 4.84, 5.14, 5.49};

int gearIndexFromGear(int g) { return std::min(std::max(g, 1) - 1, GEAR_COUNT - 1); }

double speedForCadence(double cad, int gi, double circ) {
    return std::max(0.0, cad) / 60.0 * VIRTUAL_GEARS[std::clamp(gi, 0, GEAR_COUNT - 1)] * circ;
}

double gearTargetPower(double cad, int gi, double circ, double grade, double ele, const Params& p) {
    if (cad < 15.0) return 0.0;
    double v = speedForCadence(cad, gi, circ);
    return std::clamp(powerForSpeed(v, grade, ele, p), 0.0, 2000.0);
}

}  // namespace phys
