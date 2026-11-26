#include "Engine.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Metrics.h"

// ------------------------------------------------------------------ profile
std::vector<HrZone> Profile::hrZones() const {
    double mx = std::max(maxHr, 100.0);
    double rest = std::min(restHr, mx - 20);
    std::vector<HrZone> z;
    if (hrZoneModel == HrZoneModel::Lthr) {
        double l = lthr > 0 ? lthr : mx * 0.9;
        z = {{1, "Recovery", 0, l * 0.81},          {2, "Aerobic", l * 0.81, l * 0.90},       {3, "Tempo", l * 0.90, l * 0.94},
             {4, "Sub-threshold", l * 0.94, l * 1.0}, {5, "Supra-threshold", l * 1.0, l * 1.03}, {6, "Aerobic capacity", l * 1.03, l * 1.07},
             {7, "Anaerobic", l * 1.07, std::nullopt}};
    } else {
        auto at = [&](double p) { return hrZoneModel == HrZoneModel::MaxHr ? mx * p : rest + (mx - rest) * p; };
        z = {{1, "Recovery", at(0.5), at(0.6)}, {2, "Endurance", at(0.6), at(0.7)}, {3, "Tempo", at(0.7), at(0.8)},
             {4, "Threshold", at(0.8), at(0.9)}, {5, "VO2 Max", at(0.9), std::nullopt}};
    }
    for (auto& x : z) { x.lo = std::round(x.lo); if (x.hi) x.hi = std::round(*x.hi); }
    return z;
}

int hrZoneOf(double hr, const std::vector<HrZone>& zones) {
    for (auto it = zones.rbegin(); it != zones.rend(); ++it)
        if (hr >= it->lo) return it->zone;
    return 1;
}

// ------------------------------------------------------------------ route profile
double RouteProfile::elevationAt(double d) const {
    if (ele.empty()) return 0;
    if (loop) { d = std::fmod(d, length); if (d < 0) d += length; }
    else d = std::clamp(d, 0.0, length);
    double fi = d / step;
    size_t i = size_t(fi);
    size_t n = ele.size();
    size_t a = std::min(i, n - 1), b = loop ? (i + 1) % n : std::min(i + 1, n - 1);
    double f = fi - std::floor(fi);
    return ele[a] + (ele[b] - ele[a]) * f;
}

double RouteProfile::gradeAt(double d, double half) const {
    half = std::max(half, step);
    double a = d - half, b = d + half;
    if (!loop) { a = std::max(0.0, a); b = std::min(length, b); }
    if (b - a < 1e-3) return 0;
    return (elevationAt(b) - elevationAt(a)) / (b - a);
}

RouteProfile RouteProfile::fromRoute(const Route& r, double step) {
    RouteProfile p;
    p.step = step;
    p.loop = r.isLoop;
    double L = r.distance();
    size_t n = size_t(std::max(2.0, std::floor(L / step)));
    for (size_t i = 0; i <= n; ++i) p.ele.push_back(r.elevationAt(std::min(L, i * step)));
    if (p.loop && p.ele.size() > 2) p.ele.pop_back();  // last sample wraps to the first
    p.length = p.loop ? double(p.ele.size()) * step : L;
    p.detectClimbs();
    return p;
}

void RouteProfile::detectClimbs() {
    std::vector<std::pair<double, double>> grid;
    for (double d = 0; d <= length; d += 25) grid.push_back({d, elevationAt(d)});
    climbs = ::detectClimbs(grid);
}

// ------------------------------------------------------------------ helpers
std::string fmtDur(double s) {
    int t = int(std::max(0.0, std::round(s)));
    char b[32];
    if (t >= 3600) std::snprintf(b, sizeof b, "%d:%02d:%02d", t / 3600, (t / 60) % 60, t % 60);
    else std::snprintf(b, sizeof b, "%d:%02d", t / 60, t % 60);
    return b;
}

std::string goalLabel(const Goal& g, bool metric) {
    char b[64];
    switch (g.type) {
        case Goal::Time: return fmtDur(g.value);
        case Goal::Distance:
            if (metric) std::snprintf(b, sizeof b, "%.1f km", g.value / 1000); else std::snprintf(b, sizeof b, "%.1f mi", g.value / 1609.344);
            return b;
        case Goal::Energy: std::snprintf(b, sizeof b, "%.0f kJ", g.value); return b;
        case Goal::Calories: std::snprintf(b, sizeof b, "%.0f kcal", g.value); return b;
        case Goal::Elevation: std::snprintf(b, sizeof b, "%.0f m climbing", g.value); return b;
        default: return "";
    }
}

// ------------------------------------------------------------------ engine
RideEngine::RideEngine(const Profile& profile, RideSetup setup, const std::string& startedAt)
    : profile_(profile), setup_(std::move(setup)), startedAt_(startedAt.empty() ? dates::nowRfc3339() : startedAt) {
    if (setup_.route.points.size() < 2) {
        // rides without scenery get a long, flat virtual road
        setup_.route = Route{};
        setup_.route.name = "Virtual road";
        setup_.route.points = {{0, 50}, {200000, 50}};
        setup_.routeIsScenery = true;
    }
    route_ = RouteProfile::fromRoute(setup_.route);
    params_.massKg = profile_.weightKg + profile_.bikeWeightKg;
    params_.crr = profile_.crr;
    params_.cda = profile_.cda;
    if (setup_.test) setup_.workout = wk::testProtocol(*setup_.test, profile_.ftp);
    setup_.laps = std::clamp(setup_.laps, 1, 50);
    if (setup_.workout) steps_ = setup_.workout->steps();
    ele_ = route_.elevationAt(0);
    difficulty_ = std::clamp(profile_.trainerDifficulty, 0.0, 1.5);
    double ftp = std::max(profile_.ftp, 50.0);
    freeMode_ = setup_.freeMode;
    freeGradePct_ = std::clamp(setup_.freeGradePct, profile_.minTrainerGrade, profile_.maxTrainerGrade);
    freeWatts_ = setup_.freeWatts > 0 ? std::clamp(setup_.freeWatts, 30.0, 1500.0) : std::round(ftp * 0.65);
    freeLevelPct_ = std::clamp(setup_.freeLevelPct, 0.0, 100.0);
    hrTarget_ = setup_.freeTargetHr > 0 ? setup_.freeTargetHr : std::round(profile_.restHr + 0.65 * (profile_.maxHr - profile_.restHr));
    hrW_ = std::round(ftp * 0.55);
    goal_ = setup_.goal;
    gearIdx_ = phys::gearIndexFromGear(profile_.startGear);
    hrZones_ = profile_.hrZones();
}

void RideEngine::pause() { if (!paused_) { paused_ = true; forceCmd_ = true; push("pause", "Ride paused"); } }
void RideEngine::resume() { if (paused_) { paused_ = false; forceCmd_ = true; push("resume", "Ride resumed"); } }

std::optional<std::pair<size_t, double>> RideEngine::currentStep() const {
    if (!structuredActive()) return std::nullopt;
    return wk::stepAt(steps_, workoutT());
}

void RideEngine::ergBias(double d) {
    ergBias_ = std::clamp(ergBias_ + d / 100.0, 0.5, 1.5);
    forceCmd_ = true;
    char b[48]; std::snprintf(b, sizeof b, "Intensity %.0f%%", ergBias_ * 100); push("bias", b);
}
void RideEngine::skipStep() {
    if (auto cs = currentStep()) {
        offsetS_ = steps_[cs->first].endS() - activeS_;
        forceCmd_ = true;
        push("skip", "Skipped \xE2\x80\x9C" + steps_[cs->first].label + "\xE2\x80\x9D");
    }
}
void RideEngine::toggleErg() {
    ergDisabled_ = !ergDisabled_;
    forceCmd_ = true;
    push("mode", ergDisabled_ ? "Resistance mode - you control power" : "ERG mode on");
}
void RideEngine::grade(double d) {
    freeGradePct_ = std::round(std::clamp(freeGradePct_ + d, profile_.minTrainerGrade, profile_.maxTrainerGrade) * 10) / 10;
}
void RideEngine::difficulty(double d) {
    difficulty_ = std::clamp(difficulty_ + d / 100, 0.0, 1.5);
    char b[48]; std::snprintf(b, sizeof b, "Trainer difficulty %.0f%%", difficulty_ * 100); push("difficulty", b);
}
void RideEngine::shift(int d) {
    int g = std::clamp(gearIdx_ + d, 0, phys::GEAR_COUNT - 1);
    if (g != gearIdx_) { gearIdx_ = g; forceCmd_ = true; }
}
void RideEngine::setFreeMode(FreeMode m) {
    double ftp = std::max(profile_.ftp, 50.0);
    if (m == FreeMode::HeartRate) { double p3 = power3s(); hrW_ = p3 > 30 ? std::round(p3) : std::round(ftp * 0.55); hrCtrlT_ = 0; }
    if (m == FreeMode::Power && power3s() > 30 && freeMode_ != FreeMode::Power) freeWatts_ = std::round(power3s() / 5) * 5;
    freeMode_ = m;
    forceCmd_ = true;
    static const char* n[] = {"Gradient mode", "Power (ERG) mode", "Resistance level mode", "Heart-rate mode"};
    push("mode", n[int(m)]);
}
void RideEngine::freeWatts(double d) { freeWatts_ = std::clamp(freeWatts_ + d, 30.0, 1500.0); }
void RideEngine::level(double d) { freeLevelPct_ = std::clamp(freeLevelPct_ + d, 0.0, 100.0); }
void RideEngine::hrTarget(double d) { hrTarget_ = std::clamp(hrTarget_ + d, 80.0, 200.0); }
void RideEngine::setGoal(Goal g) { goal_ = g; goalDone_ = false; }
void RideEngine::lap() { closeManualLap(); }

void RideEngine::addLaps(int delta) {
    if (setup_.kind != RideKind::Route) return;
    int n = std::clamp(setup_.laps + delta, lap_, 50);
    setup_.laps = n;
    if (routeCompleteAt_ && distance_ < route_.length * n) routeCompleteAt_.reset();
    push("laps", "Riding " + std::to_string(n) + (n == 1 ? " lap" : " laps"));
}

void RideEngine::setRoute(const Route& r) {
    setup_.route = r;
    route_ = RouteProfile::fromRoute(r);
    announced_.clear();
}

bool RideEngine::canUndoSector() const {
    if (sectors_.empty() || routeHistory_.empty()) return false;
    const auto& s = sectors_.back();
    return !s.startedIdx && !s.done && distance_ < s.startTotal;
}

bool RideEngine::undoSector() {
    if (!canUndoSector()) {
        push("sector", "Can't undo - you've already reached that sector");
        return false;
    }
    auto [r, secs] = routeHistory_.back();
    routeHistory_.pop_back();
    setRoute(r);
    sectors_ = secs;
    routeModified_ = !routeHistory_.empty() || std::any_of(sectors_.begin(), sectors_.end(), [](const ActiveSector& x) { return x.startedIdx.has_value(); });
    push("sector", "Removed the last sector");
    ++planVersion_;
    forceCmd_ = true;
    return true;
}

void RideEngine::extendRoute(const std::vector<SegmentSpec>& segs) {
    Route r = setup_.route;
    insertSegments(r, r.distance(), segs);
    setRoute(r);
    forceCmd_ = true;
}

void RideEngine::appendRoutePoints(const std::vector<RoutePoint>& pts) {
    Route r = setup_.route;
    for (const auto& p : pts)
        if (r.points.empty() || p.d > r.points.back().d + 1e-6) r.points.push_back(p);
    setRoute(r);
    forceCmd_ = true;
}

void RideEngine::addSector(const SectorSpec& spec) {
    double len = route_.length;
    double lapD = lapDistance();
    if (routeCompleteAt_) { push("sector", "The route is finished - add a lap first"); return; }
    double gap = std::max(spec.inM, 30.0 + speed_ * 4.0);
    // loops are ridden lap after lap: splice into the current lap's road
    double at = (spec.atEnd || lapD + gap >= len) ? len : lapD + gap;
    auto segs = spec.build(uint64_t(nextSectorId_) * 7919 + uint64_t(distance_));
    if (segs.empty()) { push("sector", "That sector has no length"); return; }
    auto [secLen, net] = segmentsSummary(segs);
    if (spec.rejoin) if (auto rj = rejoinSegment(net)) segs.push_back(*rj);
    auto before = std::make_pair(setup_.route, sectors_);
    Route r = setup_.route;
    bool wasLoop = r.isLoop;
    // a loop's profile stores the closing point implicitly; make it explicit while editing
    auto [ins, de] = insertSegments(r, at, segs);
    (void)de;
    r.isLoop = wasLoop;
    double startTotal = distance_ + (at - lapD);
    for (auto& s : sectors_)
        if (s.startD >= at && !s.startedIdx) { s.startD += ins; s.endD += ins; s.startTotal += ins; s.endTotal += ins; }
    ActiveSector a;
    a.id = nextSectorId_++;
    a.name = spec.displayName();
    a.kind = spec.kind;
    a.startTotal = startTotal;
    a.endTotal = startTotal + secLen;
    a.startD = at;
    a.endD = at + secLen;
    a.lengthM = secLen;
    a.avgGradePct = secLen > 0 ? net / secLen * 100 : 0;
    if (spec.kind == SectorKind::Terrain) a.terrain = spec.terrain;
    a.timed = spec.timed;
    sectors_.push_back(a);
    routeHistory_.push_back(before);
    setRoute(r);
    routeModified_ = true;
    if (!setup_.ghost.empty()) { setup_.ghost = GhostTrace{}; push("ghost", "Ghost removed - the route has changed"); }
    char b[96];
    if (at >= len - 1) std::snprintf(b, sizeof b, "at the end of the route");
    else std::snprintf(b, sizeof b, "in %.0f m", at - lapD);
    push("sector", a.name + " added " + b);
    ++planVersion_;
    forceCmd_ = true;
}

void RideEngine::extendStep(int seconds) {
    if (auto cs = currentStep()) {
        auto [i, into] = *cs;
        int mn = std::max(1, int(std::ceil(into)) + 5);
        steps_[i].durationS = std::max(mn, steps_[i].durationS + seconds);
        wk::reflow(steps_, i + 1);
        forceCmd_ = true;
    }
}
void RideEngine::adjustStep(double dp) {
    if (auto cs = currentStep()) {
        auto& s = steps_[cs->first];
        if (s.erg) { s.from = std::clamp(s.from + dp / 100, 0.2, 4.0); s.to = std::clamp(s.to + dp / 100, 0.2, 4.0); forceCmd_ = true; }
    }
}
void RideEngine::insertBlocks(const std::vector<wk::Block>& blocks, bool atEnd) {
    auto nw = wk::blocksToSteps(blocks);
    if (nw.empty()) return;
    size_t n = nw.size();
    if (steps_.empty()) {
        offsetS_ = -activeS_;
        steps_ = nw;
        workoutComplete_ = false;
        stepIdx_.reset();
    } else if (workoutComplete_ || !currentStep()) {
        int end = steps_.back().endS();
        for (auto& s : nw) s.startS += end;
        offsetS_ = end - activeS_;
        steps_.insert(steps_.end(), nw.begin(), nw.end());
        workoutComplete_ = false;
        stepIdx_.reset();
    } else {
        size_t i = currentStep()->first;
        size_t pos = atEnd ? steps_.size() : i + 1;
        steps_.insert(steps_.begin() + pos, nw.begin(), nw.end());
        wk::reflow(steps_, pos);
    }
    push("plan", "Added " + std::to_string(n) + (n == 1 ? " step" : " steps"));
    forceCmd_ = true;
}
void RideEngine::oneMoreRep() {
    auto cs = currentStep();
    if (!cs) return;
    size_t i = cs->first;
    int e = -1;
    for (int k = int(i); k >= 0; --k) if (steps_[k].effort) { e = k; break; }
    if (e < 0) { push("plan", "No interval to repeat yet"); return; }
    std::vector<wk::Step> add{steps_[e]};
    if (size_t(e) + 1 < steps_.size() && !steps_[e + 1].effort) add.push_back(steps_[e + 1]);
    for (auto& s : add) s.label = s.effort ? "Extra rep" : "Recover";
    size_t pos = i + 1;
    if (steps_[i].effort && pos < steps_.size() && !steps_[pos].effort) ++pos;
    steps_.insert(steps_.begin() + pos, add.begin(), add.end());
    wk::reflow(steps_, pos);
    push("plan", "One more rep added");
    forceCmd_ = true;
}
void RideEngine::skipToCooldown() {
    if (auto cs = currentStep()) {
        size_t i = cs->first;
        steps_.resize(i + 1);
        wk::Step c;
        c.startS = steps_[i].endS(); c.durationS = 300; c.erg = true; c.from = 0.6; c.to = 0.4; c.label = "Cool-down";
        steps_.push_back(c);
        offsetS_ = c.startS - activeS_;
        push("plan", "Cooling down");
        forceCmd_ = true;
    }
}
void RideEngine::endWorkout() {
    if (auto cs = currentStep()) {
        auto [i, into] = *cs;
        steps_.resize(i + 1);
        steps_[i].durationS = std::max(1, int(std::ceil(into)));
        offsetS_ = steps_[i].endS() - activeS_;
        forceCmd_ = true;
    }
}

void RideEngine::nudge(int dir) {
    if (structuredActive()) {
        auto cs = currentStep();
        if (cs && steps_[cs->first].erg && !ergDisabled_) ergBias(dir);
        else difficulty(dir * 10);
        return;
    }
    if (setup_.kind == RideKind::Free) {
        switch (freeMode_) {
            case FreeMode::Gradient: grade(dir * 0.5); break;
            case FreeMode::Power: freeWatts(dir * 5); break;
            case FreeMode::Level: level(dir * 5); break;
            case FreeMode::HeartRate: hrTarget(dir * 2); break;
        }
        return;
    }
    difficulty(dir * 10);
}

double RideEngine::lapDistance() const {
    if (route_.length <= 0) return distance_;
    if (routeCompleteAt_) return route_.length;
    return std::fmod(distance_, route_.length);
}

double RideEngine::power3s() const {
    if (recent_.empty()) return power_;
    double s = 0;
    for (auto& r : recent_) s += r.second;
    return s / recent_.size();
}

int RideEngine::hrZone() const { return hr_ ? hrZoneOf(*hr_, hrZones_) : 0; }
double RideEngine::calories() const { return metrics::caloriesFromWork(kj_, profile_.grossEfficiency); }
double RideEngine::tss() const { return metrics::tss(activeS_, np_, profile_.ftp); }

double RideEngine::splitAvgPower() const {
    if (samples_.size() <= splitStartIdx_) return power3s();
    double s = 0;
    for (size_t i = splitStartIdx_; i < samples_.size(); ++i) s += samples_.power[i];
    return s / (samples_.size() - splitStartIdx_);
}

bool RideEngine::freeGradientActive() const {
    return setup_.kind == RideKind::Free && !structuredActive() && freeMode_ == FreeMode::Gradient;
}

void RideEngine::closeManualLap() {
    int n = 1;
    for (auto& s : splits_) if (s.kind == "lap") ++n;
    if (auto sp = Split::fromSamples(samples_, splitStartIdx_, samples_.size(), "lap", "Lap " + std::to_string(n), n)) {
        char b[96];
        std::snprintf(b, sizeof b, "Lap %d: %s \xC2\xB7 %.0f W", n, fmtDur(sp->durationS).c_str(), sp->avgPower);
        push("lap", b);
        splits_.push_back(*sp);
    }
    splitStartIdx_ = samples_.size();
    splitStartS_ = activeS_;
    splitStartD_ = distance_;
}

std::optional<TrainerCommand> RideEngine::tick(double dt, const TrainerInput& in) {
    dt = std::clamp(dt, 0.0, 2.0);
    nowS_ += dt;
    connected_ = in.connected;
    power_ = in.connected ? std::max(0.0, in.powerW) : 0.0;
    cadence_ = in.connected ? std::max(0.0, in.cadenceRpm) : 0.0;
    hr_ = (in.heartRate && *in.heartRate > 0) ? in.heartRate : std::nullopt;
    cadEma_ += (cadence_ - cadEma_) * std::min(1.0, dt / 0.6);
    // the trainer's flywheel speed, and from it the real gear the rider is in (speed per crank turn)
    haveTrainerV_ = in.connected && in.trainerSpeedKmh && *in.trainerSpeedKmh > 0.5;
    if (haveTrainerV_) {
        trainerV_ += (*in.trainerSpeedKmh / 3.6 - trainerV_) * std::min(1.0, dt / 1.5);
        if (cadence_ > 45 && trainerV_ > 2.5) {
            double est = trainerV_ / (cadence_ / 60.0 * profile_.wheelCircM);
            if (est > 1.0 && est < 5.0) realRatio_ += (est - realRatio_) * std::min(1.0, dt / 20.0);
        }
    } else trainerV_ = cadEma_ / 60.0 * profile_.wheelCircM * realRatio_;
    // the ride clock starts on the first pedal stroke
    bool rolling = activeS_ > 0 || power_ > 0;
    if (!paused_ && !finished_ && dt > 0 && rolling) stepRide(dt);
    return control(dt);
}

void RideEngine::stepRide(double dt) {
    activeS_ += dt;
    kj_ += power_ * dt / 1000;
    maxPower_ = std::max(maxPower_, power_);
    maxCadence_ = std::max(maxCadence_, cadence_);
    recent_.push_back({activeS_, power_});
    while (!recent_.empty() && activeS_ - recent_.front().first > 3.0) recent_.erase(recent_.begin());

    double lapD = lapDistance();
    double offset = freeGradientActive() ? freeGradePct_ / 100 : 0;
    double physGrade = routeCompleteAt_ ? 0.0 : route_.gradeAt(lapD, 10) + offset;
    grade_ = physGrade;
    speed_ = phys::integrateSpeed(speed_, power_, physGrade, ele_, params_, dt);
    maxSpeed_ = std::max(maxSpeed_, speed_);
    if (speed_ > 0.3 || power_ > 0) movingS_ += dt;
    int prevLap = lap_;
    distance_ += speed_ * dt;

    // laps / completion
    double len = route_.length;
    if (len > 0) {
        if (endless()) {
            int ln = int(std::floor(distance_ / len)) + 1;
            if (ln != lap_) { lap_ = ln; announced_.clear(); }
        } else if (!routeCompleteAt_) {
            int laps = setup_.laps;
            int ln = std::min(int(std::floor(distance_ / len)) + 1, laps + 1);
            if (ln > lap_) {
                if (auto sp = Split::fromSamples(samples_, routeLapStartIdx_, samples_.size(), "route_lap", "Route lap " + std::to_string(lap_), lap_)) splits_.push_back(*sp);
                routeLapStartIdx_ = samples_.size();
            }
            if (distance_ >= len * laps) {
                routeCompleteAt_ = activeS_;
                push("route_complete", "Route complete in " + fmtDur(activeS_) + "!");
            } else if (ln > lap_) {
                lap_ = ln;
                announced_.clear();
                push("lap", "Lap " + std::to_string(ln) + " of " + std::to_string(laps));
            }
        }
    }

    double newEle = route_.elevationAt(lapDistance());
    if (lap_ == prevLap && newEle > ele_) gain_ += newEle - ele_;
    ele_ = newEle;

    // climb announcements
    if (!routeCompleteAt_) {
        double ld = lapDistance();
        for (size_t i = 0; i < route_.climbs.size(); ++i) {
            const auto& c = route_.climbs[i];
            double to = c.startD - ld;
            if (to > 0 && to <= 600 && !announced_.count(int(i))) {
                announced_.insert(int(i));
                char b[128];
                std::snprintf(b, sizeof b, "%s climb in %.0f m: %.1f km at %.1f%%", c.category == "Uncategorised" ? "Short" : c.category.c_str(), to,
                              c.lengthM / 1000, c.avgGradePct);
                push("climb", b);
            }
        }
    }

    // ramp failure detection
    if (setup_.test == wk::TestKind::Ramp && !testDone_) {
        if (auto st = wk::stepAt(steps_, workoutT())) {
            if (steps_[st->first].label.rfind("Ramp", 0) == 0) {
                double target = targetW_.value_or(0);
                bool failing = cadence_ < 45 || power_ < 0.65 * target;
                rampFailS_ = failing ? rampFailS_ + dt : std::max(0.0, rampFailS_ - dt);
                if (rampFailS_ >= 8) {
                    testDone_ = true;
                    for (auto& s : steps_) if (s.label == "Cool-down") { offsetS_ = s.startS - activeS_; break; }
                    char b[128];
                    if (auto r = liveTestResult()) std::snprintf(b, sizeof b, "Ramp done! Best minute %.0f W -> FTP %.0f W", r->measuredW, r->ftp);
                    else std::snprintf(b, sizeof b, "Ramp done!");
                    push("test_complete", b);
                    forceCmd_ = true;
                }
            }
        }
    }

    // heart-rate controller
    if (setup_.kind == RideKind::Free && !structuredActive() && freeMode_ == FreeMode::HeartRate) {
        hrCtrlT_ += dt;
        if (hrCtrlT_ >= 4) {
            hrCtrlT_ = 0;
            if (hr_) {
                double err = hrTarget_ - *hr_, ftp = std::max(profile_.ftp, 50.0);
                hrW_ = std::round(std::clamp(hrW_ + std::clamp(err * 1.5, -15.0, 15.0), 0.3 * ftp, 1.3 * ftp));
            }
        }
    }

    // 1 Hz recording
    acc_.dt += dt; acc_.p += power_ * dt; acc_.c += cadence_ * dt; acc_.v += speed_ * dt;
    if (hr_) { acc_.hr += *hr_ * dt; acc_.hrDt += dt; }
    if (targetW_) { acc_.target += *targetW_ * dt; acc_.targetDt += dt; }
    while (double(samples_.size()) + 1.0 <= activeS_) {
        double n = std::max(acc_.dt, 1e-9);
        samples_.t.push_back(uint32_t(samples_.size()));
        samples_.power.push_back(float(acc_.p / n));
        samples_.cadence.push_back(float(acc_.c / n));
        samples_.speed.push_back(float(acc_.v / n));
        samples_.hr.push_back(acc_.hrDt > 0 ? float(acc_.hr / acc_.hrDt) : 0.f);
        samples_.target.push_back(acc_.targetDt > 0 ? float(acc_.target / acc_.targetDt) : 0.f);
        samples_.distance.push_back(float(distance_));
        samples_.ele.push_back(float(ele_));
        samples_.grade.push_back(float(grade_ * 100));
        workoutTSamples_.push_back(double(samples_.size() - 1) + offsetS_);
        acc_ = Acc{};
    }
    npTimer_ += dt;
    if (npTimer_ >= 2) { npTimer_ = 0; np_ = metrics::normalizedPower(samples_.powerD()); }

    // timed sectors
    size_t nS = samples_.size();
    for (auto& sc : sectors_) {
        if (sc.done) continue;
        if (!sc.startedIdx && distance_ >= sc.startTotal) {
            sc.startedIdx = nS;
            if (sc.timed) push("sector", "Sector started: " + sc.name);
        }
        if (sc.startedIdx && distance_ >= sc.endTotal && nS > *sc.startedIdx) {
            sc.done = true;
            if (sc.timed) {
                if (auto sp = Split::fromSamples(samples_, *sc.startedIdx, nS, "sector", sc.name, sc.id)) {
                    char b[160];
                    std::snprintf(b, sizeof b, "%s: %s \xC2\xB7 %.0f W", sc.name.c_str(), fmtDur(sp->durationS).c_str(), sp->avgPower);
                    push("sector", b);
                    sc.result = *sp;
                    splits_.push_back(*sp);
                }
            }
        }
    }

    // goal
    if (goal_.type != Goal::None && !goalDone_ && goalProgress() >= 1.0) {
        goalDone_ = true;
        push("goal", "Goal reached: " + goalLabel(goal_, profile_.metric) + "!");
    }
}

double RideEngine::goalProgress() const {
    double have = 0;
    switch (goal_.type) {
        case Goal::Time: have = activeS_; break;
        case Goal::Distance: have = distance_; break;
        case Goal::Energy: have = kj_; break;
        case Goal::Calories: have = calories(); break;
        case Goal::Elevation: have = gain_; break;
        default: return 0;
    }
    return goal_.value <= 0 ? 1.0 : std::clamp(have / goal_.value, 0.0, 1.0);
}

std::string RideEngine::goalRemaining() const {
    char b[64];
    switch (goal_.type) {
        case Goal::Time: return fmtDur(std::max(0.0, goal_.value - activeS_)) + " to go";
        case Goal::Distance: std::snprintf(b, sizeof b, "%.2f km to go", std::max(0.0, (goal_.value - distance_) / 1000)); return b;
        case Goal::Energy: std::snprintf(b, sizeof b, "%.0f kJ to go", std::max(0.0, goal_.value - kj_)); return b;
        case Goal::Calories: std::snprintf(b, sizeof b, "%.0f kcal to go", std::max(0.0, goal_.value - calories())); return b;
        case Goal::Elevation: std::snprintf(b, sizeof b, "%.0f m to climb", std::max(0.0, goal_.value - gain_)); return b;
        default: return "";
    }
}

std::optional<TrainerCommand> RideEngine::control(double dt) {
    double ftp = std::max(profile_.ftp, 50.0);
    std::optional<std::pair<double, bool>> stepGrade;
    ControlMode mode = ControlMode::Sim;
    std::optional<double> target;

    if (structuredActive()) {
        if (auto st = wk::stepAt(steps_, workoutT())) {
            auto [i, into] = *st;
            if (stepIdx_ != i) {
                bool first = !stepIdx_ && i == 0;
                stepIdx_ = i;
                if (!first) {
                    const auto& s = steps_[i];
                    char b[160];
                    if (s.erg) std::snprintf(b, sizeof b, "%s \xC2\xB7 %.0f W", s.label.c_str(), s.from * ftp * ergBias_);
                    else std::snprintf(b, sizeof b, "%s \xC2\xB7 ride by feel", s.label.c_str());
                    push("step", b);
                }
            }
            const auto& s = steps_[i];
            if (s.erg) {
                target = std::round(s.fractionAt(into).value_or(0.5) * ftp * ergBias_);
                if (!ergDisabled_) mode = ControlMode::Erg;
            } else stepGrade = std::make_pair(s.gradePct, s.effort);
        } else {
            workoutComplete_ = true;
            stepIdx_.reset();
            push("workout_complete", setup_.test ? "Test finished - great work!" : setup_.kind == RideKind::Workout ? "Workout complete!"
                                                                                                                      : "Intervals done - back to free riding");
            forceCmd_ = true;
        }
    } else if (setup_.kind == RideKind::Free) {
        switch (freeMode_) {
            case FreeMode::Gradient: break;
            case FreeMode::Power: mode = ControlMode::Erg; target = freeWatts_; break;
            case FreeMode::HeartRate: mode = ControlMode::Erg; target = hrW_; break;
            case FreeMode::Level: mode = ControlMode::Level; break;
        }
    }
    mode_ = mode;
    targetW_ = target;

    double lapD = lapDistance();
    bool hasRoute = route_.length > 0 && !routeCompleteAt_;
    double desired;
    if (stepGrade && (stepGrade->second || !hasRoute)) desired = stepGrade->first;
    else if (hasRoute) {
        double look = lapD + speed_;
        double half = std::max(speed_ * 2, 10.0);
        desired = route_.gradeAt(look, half) * 100 * difficulty_ + (freeGradientActive() ? freeGradePct_ : 0);
    } else if (setup_.kind == RideKind::Free) desired = freeGradePct_;
    else desired = 0;
    if (paused_) desired = 0;
    double alpha = dt > 0 ? std::min(1.0, dt) : 0;
    if (forceCmd_ && !lastCmd_) trainerGradePct_ = desired;
    else trainerGradePct_ += (desired - trainerGradePct_) * alpha;
    trainerGradePct_ = std::clamp(trainerGradePct_, profile_.minTrainerGrade, profile_.maxTrainerGrade);

    shiftingActive_ = profile_.virtualShifting && mode_ == ControlMode::Sim && !paused_ && !finished_;
    // virtual shifting stays in SIM mode: the grade is adjusted for the gear (see phys::gearSimGrade)
    double simGrade = trainerGradePct_;
    if (shiftingActive_) {
        double k = phys::VIRTUAL_GEARS[gearIdx_] / std::max(0.5, realRatio_);
        simGrade = std::clamp(phys::gearSimGrade(trainerGradePct_ / 100, k, trainerV_, params_) * 100, -10.0, 20.0);
    }

    TrainerCommand cmd;
    if (paused_ || finished_) cmd = TrainerCommand::sim(0, params_.crr, params_.cw());
    else if (mode_ == ControlMode::Sim) cmd = TrainerCommand::sim(std::round(simGrade * 10) / 10, params_.crr, std::round(params_.cw() * 100) / 100);
    else if (mode_ == ControlMode::Erg) cmd = TrainerCommand::erg(std::round(targetW_.value_or(100)));
    else cmd = TrainerCommand::level(std::round(freeLevelPct_ * 2) / 2);
    cmd.massKg = params_.massKg;

    double since = nowS_ - lastCmdAt_;
    bool changed = true;
    if (lastCmd_ && lastCmd_->type == cmd.type) {
        if (cmd.type == TrainerCommand::Simulation) changed = std::fabs(lastCmd_->gradePct - cmd.gradePct) >= 0.1 && since >= 0.5;
        else if (cmd.type == TrainerCommand::TargetPower) changed = std::fabs(lastCmd_->watts - cmd.watts) >= 1 && since >= 0.5;
        else changed = std::fabs(lastCmd_->levelPct - cmd.levelPct) >= 0.5;
    }
    if (forceCmd_ || changed || since >= 5) {
        forceCmd_ = false;
        lastCmd_ = cmd;
        lastCmdAt_ = nowS_;
        return cmd;
    }
    return std::nullopt;
}

std::optional<wk::TestResult> RideEngine::liveTestResult() const {
    if (!setup_.test) return std::nullopt;
    return wk::evaluateTest(*setup_.test, steps_, workoutTSamples_, samples_.powerD());
}

std::optional<double> RideEngine::ghostDistance() const {
    if (setup_.ghost.empty() || activeS_ <= 0) return std::nullopt;
    return setup_.ghost.distanceAtTime(activeS_);
}

std::optional<double> RideEngine::ghostGapS() const {
    // >0: you're ahead of the ghost by that many seconds
    if (setup_.ghost.empty() || activeS_ <= 0) return std::nullopt;
    auto t = setup_.ghost.timeAtDistance(distance_);
    if (!t) return std::nullopt;
    return *t - activeS_;
}

int RideEngine::currentClimb() const {
    double ld = lapDistance();
    for (size_t i = 0; i < route_.climbs.size(); ++i)
        if (ld >= route_.climbs[i].startD && ld <= route_.climbs[i].endD) return int(i);
    return -1;
}

int RideEngine::nextClimb() const {
    double ld = lapDistance();
    int best = -1;
    double bd = 1e18;
    for (size_t i = 0; i < route_.climbs.size(); ++i) {
        double to = route_.climbs[i].startD - ld;
        if (to < 0 && route_.loop) to += route_.length;
        if (to > 0 && to < bd) { bd = to; best = int(i); }
    }
    return best;
}

std::vector<RideEvent> RideEngine::drainEvents() {
    std::vector<RideEvent> e;
    e.swap(events_);
    return e;
}

RideRecord RideEngine::finish(const std::string& endedAt, const std::string& id) {
    finished_ = true;
    bool anyLap = std::any_of(splits_.begin(), splits_.end(), [](const Split& x) { return x.kind == "lap"; });
    if (anyLap && samples_.size() > splitStartIdx_) closeManualLap();
    RideRecord rec;
    RideSummary& s = rec.summary;
    auto p = samples_.powerD();
    s.id = id;
    s.kind = setup_.kind;
    s.startedAt = startedAt_;
    s.endedAt = endedAt;
    s.name = setup_.name.empty() ? rideKindName(setup_.kind) : setup_.name;
    if (!setup_.routeIsScenery && !routeModified_ && !setup_.route.id.empty() && setup_.kind == RideKind::Route) {
        s.routeId = setup_.route.id;
        s.routeName = setup_.route.name;
    } else if (!setup_.route.name.empty() && setup_.route.name != "Virtual road") s.routeName = setup_.route.name;
    if (setup_.workout) { s.workoutId = setup_.workout->id; s.workoutName = setup_.workout->name; }
    s.eventId = setup_.eventId;
    s.test = setup_.test;
    s.laps = setup_.laps;
    s.completed = routeCompleteAt_.has_value() || workoutComplete_;
    if (setup_.kind == RideKind::Route && routeCompleteAt_ && !routeModified_) s.routeTimeS = routeCompleteAt_;
    s.elapsedS = activeS_;
    s.movingS = movingS_;
    s.distanceM = distance_;
    s.elevationGainM = gain_;
    s.avgSpeedMps = movingS_ > 0 ? distance_ / movingS_ : 0;
    s.maxSpeedMps = maxSpeed_;
    s.avgPower = avgPower();
    s.maxPower = maxPower_;
    s.np = metrics::normalizedPower(p);
    s.intensityFactor = metrics::intensityFactor(s.np, profile_.ftp);
    s.tss = metrics::tss(activeS_, s.np, profile_.ftp);
    s.variabilityIndex = s.avgPower > 0 ? s.np / s.avgPower : 0;
    s.workKj = kj_;
    s.caloriesKcal = calories();
    double hr = 0, hrMax = 0, cad = 0;
    int hrN = 0, cadN = 0;
    s.hrZoneS.assign(hrZones_.size(), 0);
    for (size_t i = 0; i < samples_.size(); ++i) {
        if (samples_.hr[i] > 0) {
            hr += samples_.hr[i]; ++hrN; hrMax = std::max<double>(hrMax, samples_.hr[i]);
            int z = hrZoneOf(samples_.hr[i], hrZones_);
            if (z >= 1 && size_t(z) <= s.hrZoneS.size()) s.hrZoneS[z - 1] += 1;
        }
        if (samples_.cadence[i] > 0) { cad += samples_.cadence[i]; ++cadN; }
    }
    if (hrN) {
        s.avgHr = hr / hrN;
        s.maxHr = hrMax;
        s.caloriesHrKcal = metrics::caloriesFromHr(*s.avgHr, activeS_ / 60, profile_.weightKg, profile_.age, profile_.male);
    }
    s.avgCadence = cadN ? cad / cadN : 0;
    s.maxCadence = maxCadence_;
    s.ftp = profile_.ftp;
    s.weightKg = profile_.weightKg;
    for (auto& [d, w] : metrics::powerBests(p)) s.powerBests.push_back({d, w});
    s.zoneS = metrics::timeInZones(p, profile_.ftp);
    s.testResult = liveTestResult();
    s.splits = splits_;
    s.routeModified = routeModified_;
    s.simulated = setup_.simulated;
    rec.samples = samples_;
    return rec;
}
