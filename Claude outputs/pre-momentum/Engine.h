#pragma once
// Real-time ride engine (port of CoreKick engine.rs).
// Feed it dt + trainer telemetry each frame; it integrates speed/distance from power on the
// real gradient, drives ERG / SIM / level targets, handles workouts, FTP tests, free-ride
// modes (gradient, power, level, heart-rate), goals, laps, climbs, ghosts, in-ride road
// editing (sectors spliced into the road ahead) and 1 Hz recording.
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "Physics.h"
#include "Records.h"
#include "Route.h"
#include "Workout.h"

// ------------------------------------------------------------------ profile
enum class HrZoneModel { MaxHr, Reserve, Lthr };
enum class FtpAutoMode { Suggest, AutoIncrease, Off };
struct HrZone { int zone; std::string name; double lo; std::optional<double> hi; };

struct Profile {
    std::string name = "Rider";
    double weightKg = 75, bikeWeightKg = 8.5;
    int age = 40;
    bool male = true;
    double ftp = 220;
    double maxHr = 182, restHr = 55;
    bool metric = true;
    double grossEfficiency = 0.24, cda = 0.32, crr = 0.004;
    double trainerDifficulty = 1.0, maxTrainerGrade = 16, minTrainerGrade = -10;
    double weeklyGoalHours = 5;
    bool virtualShifting = false;
    double wheelCircM = 2.105;
    int startGear = 10;
    HrZoneModel hrZoneModel = HrZoneModel::MaxHr;
    double lthr = 0;
    FtpAutoMode ftpAuto = FtpAutoMode::Suggest;
    // heart-rate alerts
    bool hrAlerts = false;
    int hrAlertLo = 2, hrAlertHi = 4;   // allowed zone range (1-based)
    bool hrAlertHigh = true, hrAlertLow = true;
    double hrAlertGraceS = 300, hrAlertRepeatS = 30;
    std::vector<HrZone> hrZones() const;
};
int hrZoneOf(double hr, const std::vector<HrZone>& zones);

// ------------------------------------------------------------------ trainer IO
struct TrainerCommand {
    enum Type { Simulation, TargetPower, Resistance } type = Simulation;
    double gradePct = 0, windMps = 0, crr = 0.004, cw = 0.51;
    double watts = 0;
    double levelPct = 0;
    double massKg = 0;   // rider + bike (sim mode on trainers that take it, e.g. Wahoo legacy); 0 = unknown
    static TrainerCommand sim(double g, double crr, double cw) { TrainerCommand c; c.type = Simulation; c.gradePct = g; c.crr = crr; c.cw = cw; return c; }
    static TrainerCommand erg(double w) { TrainerCommand c; c.type = TargetPower; c.watts = w; return c; }
    static TrainerCommand level(double l) { TrainerCommand c; c.type = Resistance; c.levelPct = l; return c; }
};

struct TrainerInput {
    double powerW = 0, cadenceRpm = 0;
    std::optional<double> heartRate;
    bool connected = false;
    std::optional<double> trainerSpeedKmh;   // the trainer's own flywheel speed (FTMS), for virtual gears (last: positional init elsewhere)
};

// ------------------------------------------------------------------ route profile (fast, uniformly sampled)
struct RouteProfile {
    std::vector<double> ele;  // uniform samples
    double step = 10;         // metres between samples
    double length = 0;
    bool loop = true;
    std::vector<Climb> climbs;
    static RouteProfile fromRoute(const Route& r, double step = 10);
    double elevationAt(double d) const;
    double gradeAt(double d, double half) const;  // fraction, central difference over +-half
    void detectClimbs();
};

// ------------------------------------------------------------------ setup
enum class FreeMode { Gradient, Power, Level, HeartRate };
enum class ControlMode { Sim, Erg, Level };

struct Goal {
    enum Type { None, Time, Distance, Energy, Calories, Elevation } type = None;
    double value = 0;
};
std::string goalLabel(const Goal& g, bool metric);

struct RideSetup {
    RideKind kind = RideKind::Free;
    std::string name;
    Route route;                 // the road (scenery for workouts / tests / free rides)
    bool routeIsScenery = false; // true when the route is only scenery (don't record route_id)
    int laps = 1;
    std::optional<wk::Workout> workout;
    std::optional<wk::TestKind> test;
    std::optional<std::string> eventId;
    FreeMode freeMode = FreeMode::Gradient;
    double freeGradePct = 0, freeWatts = 0, freeLevelPct = 30, freeTargetHr = 0;
    Goal goal;
    GhostTrace ghost;
    bool simulated = false;
};

struct RideEvent { std::string kind, message; };

struct ActiveSector {
    int id = 0;
    std::string name;
    SectorKind kind = SectorKind::Climb;
    double startTotal = 0, endTotal = 0, startD = 0, endD = 0, lengthM = 0, avgGradePct = 0;
    std::optional<Terrain> terrain;
    bool timed = true;
    std::optional<size_t> startedIdx;
    bool done = false;
    std::optional<Split> result;
};

// ------------------------------------------------------------------ engine
class RideEngine {
public:
    RideEngine(const Profile& profile, RideSetup setup, const std::string& startedAt = "");

    // advance; returns a command when the trainer target changed
    std::optional<TrainerCommand> tick(double dt, const TrainerInput& in);

    // ---- actions (keyboard / edit drawer)
    void pause();
    void resume();
    void ergBias(double deltaPct);
    void skipStep();
    void toggleErg();
    void grade(double deltaPct);
    void difficulty(double deltaPct);
    void shift(int delta);
    void setVirtualShifting(bool on) { profile_.virtualShifting = on; forceCmd_ = true; }
    bool virtualShifting() const { return profile_.virtualShifting; }
    void setFreeMode(FreeMode m);
    void freeWatts(double deltaW);
    void level(double deltaPct);
    void hrTarget(double deltaBpm);
    void setGoal(Goal g);
    void lap();
    void addLaps(int delta);
    void addSector(const SectorSpec& spec);
    // endless roads: append to the end of the road (no sector, no message)
    void extendRoute(const std::vector<SegmentSpec>& segs);
    // free roaming: the road ahead grows by these points (distances continue the route's)
    void appendRoutePoints(const std::vector<RoutePoint>& pts);
    bool freeGradientActive() const;  // free ride following the road (Up/Down offset the gradient)
    bool undoSector();
    void extendStep(int seconds);
    void adjustStep(double deltaPct);
    void insertBlocks(const std::vector<wk::Block>& blocks, bool atEnd);
    void oneMoreRep();
    void skipToCooldown();
    void endWorkout();
    void nudge(int dir);  // context-sensitive up/down like CoreKick's arrow keys

    RideRecord finish(const std::string& endedAt, const std::string& id);
    std::vector<RideEvent> drainEvents();

    // ---- state for the UI
    const RideSetup& setup() const { return setup_; }
    const Profile& profile() const { return profile_; }
    const Route& route() const { return setup_.route; }
    const RouteProfile& profileData() const { return route_; }
    int planVersion() const { return planVersion_; }
    bool routeModified() const { return routeModified_; }
    const std::vector<ActiveSector>& sectors() const { return sectors_; }
    bool canUndoSector() const;
    const std::vector<wk::Step>& steps() const { return steps_; }
    const Samples& samples() const { return samples_; }
    const std::vector<Split>& splits() const { return splits_; }
    bool paused() const { return paused_; }
    bool finished() const { return finished_; }
    bool endless() const { return setup_.kind != RideKind::Route; }
    bool structured() const { return !steps_.empty(); }
    bool structuredActive() const { return !steps_.empty() && !workoutComplete_; }
    bool workoutComplete() const { return workoutComplete_; }
    bool routeComplete() const { return routeCompleteAt_.has_value(); }
    std::optional<double> routeTime() const { return routeCompleteAt_; }
    bool started() const { return activeS_ > 0; }
    double elapsedS() const { return activeS_; }
    double movingS() const { return movingS_; }
    double distance() const { return distance_; }
    double lapDistance() const;
    int lapNumber() const { return lap_; }
    int laps() const { return setup_.laps; }
    double ele() const { return ele_; }
    double gradePct() const { return grade_ * 100; }
    double trainerGradePct() const { return trainerGradePct_; }
    double difficultyValue() const { return difficulty_; }
    ControlMode mode() const { return mode_; }
    std::optional<double> targetW() const { return targetW_; }
    double speedMps() const { return speed_; }
    double power() const { return power_; }
    double power3s() const;
    double cadence() const { return cadence_; }
    std::optional<double> heartRate() const { return hr_; }
    int hrZone() const;
    double avgPower() const { return activeS_ > 0 ? kj_ * 1000 / activeS_ : 0; }
    double maxPower() const { return maxPower_; }
    double np() const { return np_; }
    double workKj() const { return kj_; }
    double calories() const;
    double gain() const { return gain_; }
    double tss() const;
    double workoutT() const { return activeS_ + offsetS_; }
    double workoutTotalS() const { return steps_.empty() ? 0 : steps_.back().endS(); }
    std::optional<std::pair<size_t, double>> currentStep() const;
    double ergBiasValue() const { return ergBias_; }
    bool ergDisabled() const { return ergDisabled_; }
    FreeMode freeMode() const { return freeMode_; }
    double freeGrade() const { return freeGradePct_; }
    double freeWattsValue() const { return freeWatts_; }
    double freeLevel() const { return freeLevelPct_; }
    double hrTargetValue() const { return hrTarget_; }
    double hrControlW() const { return hrW_; }
    const Goal& goal() const { return goal_; }
    double goalProgress() const;
    std::string goalRemaining() const;
    bool goalDone() const { return goalDone_; }
    std::optional<double> ghostGapS() const;
    std::optional<double> ghostDistance() const;
    int currentClimb() const;  // index or -1
    int nextClimb() const;
    std::optional<wk::TestResult> liveTestResult() const;
    bool testDone() const { return testDone_; }
    bool shiftingActive() const { return shiftingActive_; }
    int gear() const { return gearIdx_ + 1; }
    double gearRatio() const { return phys::VIRTUAL_GEARS[gearIdx_]; }
    double realGearRatio() const { return realRatio_; }   // the rider's actual chainring/cog, learnt from trainer speed vs cadence
    double splitElapsed() const { return activeS_ - splitStartS_; }
    double splitDistance() const { return distance_ - splitStartD_; }
    double splitAvgPower() const;
    const std::vector<double>& workoutTSamples() const { return workoutTSamples_; }

private:
    void stepRide(double dt);
    std::optional<TrainerCommand> control(double dt);
    void push(const std::string& k, const std::string& m) { events_.push_back({k, m}); }
    void closeManualLap();
    void setRoute(const Route& r);

    Profile profile_;
    phys::Params params_;
    RideSetup setup_;
    RouteProfile route_;
    std::string startedAt_;
    std::vector<wk::Step> steps_;
    int planVersion_ = 1;

    double activeS_ = 0, movingS_ = 0, offsetS_ = 0;
    bool paused_ = false, finished_ = false;
    double speed_ = 0, maxSpeed_ = 0, distance_ = 0, ele_ = 0, gain_ = 0, grade_ = 0, trainerGradePct_ = 0, difficulty_ = 1;
    double kj_ = 0, power_ = 0, cadence_ = 0, maxPower_ = 0, maxCadence_ = 0, np_ = 0;
    std::optional<double> hr_;
    bool connected_ = false;
    std::vector<std::pair<double, double>> recent_;
    struct Acc { double dt = 0, p = 0, c = 0, hr = 0, hrDt = 0, v = 0, target = 0, targetDt = 0; } acc_;
    Samples samples_;
    std::vector<double> workoutTSamples_;

    ControlMode mode_ = ControlMode::Sim;
    std::optional<double> targetW_;
    bool ergDisabled_ = false;
    double ergBias_ = 1.0;
    std::optional<TrainerCommand> lastCmd_;
    double lastCmdAt_ = -1e9, nowS_ = 0;
    bool forceCmd_ = true;

    FreeMode freeMode_ = FreeMode::Gradient;
    double freeGradePct_ = 0, freeWatts_ = 150, freeLevelPct_ = 30, hrTarget_ = 140, hrW_ = 120, hrCtrlT_ = 0;
    Goal goal_;
    bool goalDone_ = false;

    std::vector<Split> splits_;
    size_t splitStartIdx_ = 0, routeLapStartIdx_ = 0;
    double splitStartS_ = 0, splitStartD_ = 0;
    std::vector<ActiveSector> sectors_;
    int nextSectorId_ = 1;
    std::vector<std::pair<Route, std::vector<ActiveSector>>> routeHistory_;
    bool routeModified_ = false;
    std::vector<RideEvent> events_;
    int lap_ = 1;
    std::optional<double> routeCompleteAt_;
    bool workoutComplete_ = false;
    std::optional<size_t> stepIdx_;
    std::set<int> announced_;
    double rampFailS_ = 0;
    bool testDone_ = false;
    int gearIdx_ = 9;
    double cadEma_ = 0;
    double trainerV_ = 0, realRatio_ = 2.43;   // trainer speed (m/s, smoothed); real gear (34x14, the Zwift Cog, until learnt)
    bool haveTrainerV_ = false;
    bool shiftingActive_ = false;
    std::vector<HrZone> hrZones_;
    double npTimer_ = 0;
};

std::string fmtDur(double s);
