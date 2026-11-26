#pragma once
// Rider + bike physics for virtual speed (port of CoreKick physics.rs).
//   gravity = m g sin(theta), rolling = Crr m g cos(theta), aero = 1/2 rho CdA (v + wind)^2
// Propulsion = eta P / v, integrated as F = m_eff a so the rider carries momentum.

namespace phys {

constexpr double G = 9.80665;
constexpr double ROTATIONAL_MASS_KG = 1.4;
// Propulsion force is P*eta/v, floored at this speed so a standing start is finite.
// It must sit well below any real climbing speed: at 1.5 m/s a heavy rider on a steep
// grade (e.g. 110 kg, 230 W, 13.6 %) could never get going even though the steady
// speed is ~5 km/h. 0.3 m/s caps the start force at ~3x body weight equivalent.
constexpr double MIN_PROPULSION_SPEED = 0.3;

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
// Virtual shifting in SIM mode: the grade to send so a trainer whose flywheel turns at
// trainerSpeedMps (in the rider's real gear) resists exactly like the virtual bike in a gear
// k times taller (k = virtual ratio / real ratio) on a road of `grade`:
//   m g (s' + crr) + cw v^2  =  k [ m g (s + crr) + cw (k v)^2 ]
// The trainer's own physics then does the rest (resistance rises with speed, like a real
// bike) - no power target to chase, so no ERG feedback loop. Returns a fraction.
double gearSimGrade(double grade, double k, double trainerSpeedMps, const Params& p);

}  // namespace phys
