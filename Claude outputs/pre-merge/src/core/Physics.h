#pragma once
// Rider + bike physics for virtual speed (port of CoreKick physics.rs).
//   gravity = m g sin(theta), rolling = Crr m g cos(theta), aero = 1/2 rho CdA (v + wind)^2
// Propulsion = eta P / v, integrated as F = m_eff a so the rider carries momentum.

namespace phys {

constexpr double G = 9.80665;
constexpr double ROTATIONAL_MASS_KG = 1.4;
constexpr double MIN_PROPULSION_SPEED = 1.5;

struct Params {
    double massKg = 83.0;
    double crr = 0.004;
    double cda = 0.32;
    double drivetrainEff = 0.976;
    double windMps = 0.0;
    double cw() const;  // FTMS wind resistance coefficient 1/2 rho CdA at sea level
};

double airDensity(double eleM);
double resistiveForce(double v, double grade, double eleM, const Params& p);
double powerForSpeed(double v, double grade, double eleM, const Params& p);
double steadySpeed(double power, double grade, double eleM, const Params& p);
double integrateSpeed(double v0, double power, double grade, double eleM, const Params& p, double dt);

// Virtual shifting (single-speed trainers, e.g. KICKR CORE + Zwift Cog)
constexpr int GEAR_COUNT = 24;
extern const double VIRTUAL_GEARS[GEAR_COUNT];
int gearIndexFromGear(int gear1Based);
double speedForCadence(double cadenceRpm, int gearIdx, double wheelCircM);
double gearTargetPower(double cadenceRpm, int gearIdx, double wheelCircM, double grade, double eleM, const Params& p);

}  // namespace phys
