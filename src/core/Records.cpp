#include "Records.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>

#include "Metrics.h"

using json::Value;

const char* rideKindId(RideKind k) {
    static const char* n[] = {"free", "route", "workout", "test"};
    return n[int(k)];
}
const char* rideKindName(RideKind k) {
    static const char* n[] = {"Free ride", "Route", "Workout", "Fitness test"};
    return n[int(k)];
}
RideKind rideKindFromId(const std::string& s) {
    if (s == "route") return RideKind::Route;
    if (s == "workout") return RideKind::Workout;
    if (s == "test") return RideKind::Test;
    return RideKind::Free;
}
const char* testKindId(wk::TestKind k) {
    static const char* n[] = {"ramp", "ftp20", "ftp8"};
    return n[int(k)];
}
std::optional<wk::TestKind> testKindFromId(const std::string& s) {
    if (s == "ramp") return wk::TestKind::Ramp;
    if (s == "ftp20") return wk::TestKind::Ftp20;
    if (s == "ftp8") return wk::TestKind::Ftp8;
    return std::nullopt;
}

static Value optNum(const std::optional<double>& v) { return v ? Value(*v) : Value(); }
static std::optional<double> getOptNum(const Value& v, const char* k) { return v.num(k); }
static std::optional<std::string> getOptStr(const Value& v, const char* k) { return v.str(k); }

// ------------------------------------------------------------------ samples
json::Value Samples::toJson() const {
    Value v = Value::object();
    Value tt = Value::array();
    for (auto x : t) tt.push(Value((unsigned)x));
    v["t"] = tt;
    v["power"] = Value::numbers(power); v["cadence"] = Value::numbers(cadence); v["hr"] = Value::numbers(hr);
    v["speed"] = Value::numbers(speed); v["distance"] = Value::numbers(distance); v["ele"] = Value::numbers(ele);
    v["grade"] = Value::numbers(grade); v["target"] = Value::numbers(target);
    return v;
}

Samples Samples::fromJson(const json::Value& v) {
    Samples s;
    s.t = v.numsAs<uint32_t>("t");
    s.power = v.numsAs<float>("power"); s.cadence = v.numsAs<float>("cadence"); s.hr = v.numsAs<float>("hr");
    s.speed = v.numsAs<float>("speed"); s.distance = v.numsAs<float>("distance"); s.ele = v.numsAs<float>("ele");
    s.grade = v.numsAs<float>("grade"); s.target = v.numsAs<float>("target");
    size_t n = s.power.size();
    auto fit = [n](auto& x) { x.resize(n); };
    fit(s.cadence); fit(s.hr); fit(s.speed); fit(s.distance); fit(s.ele); fit(s.grade); fit(s.target);
    if (s.t.size() != n) { s.t.resize(n); for (size_t i = 0; i < n; ++i) s.t[i] = uint32_t(i); }
    return s;
}

// ------------------------------------------------------------------ splits
std::optional<Split> Split::fromSamples(const Samples& s, size_t a, size_t b, const std::string& kind, const std::string& name, int index) {
    b = std::min(b, s.size());
    if (b <= a) return std::nullopt;
    std::vector<double> p, hr, cad;
    for (size_t i = a; i < b; ++i) {
        p.push_back(s.power[i]);
        if (s.hr[i] > 0) hr.push_back(s.hr[i]);
        if (s.cadence[i] > 0) cad.push_back(s.cadence[i]);
    }
    double d0 = a > 0 ? s.distance[a - 1] : 0.0;
    double dist = s.distance[b - 1] - d0;
    double gain = 0, prev = a > 0 ? s.ele[a - 1] : s.ele[a];
    for (size_t i = a; i < b; ++i) {
        double e = s.ele[i];
        if (e > prev && e - prev < 20) gain += e - prev;
        prev = e;
    }
    double dur = double(b - a), e0 = a > 0 ? s.ele[a - 1] : s.ele[a];
    Split sp;
    sp.index = index; sp.kind = kind; sp.name = name;
    sp.startS = s.t.size() > a ? s.t[a] : double(a);
    sp.durationS = dur; sp.distanceM = dist; sp.elevationGainM = gain;
    sp.avgPower = metrics::mean(p);
    sp.maxPower = p.empty() ? 0 : *std::max_element(p.begin(), p.end());
    sp.np = metrics::normalizedPower(p);
    if (!hr.empty()) sp.avgHr = metrics::mean(hr);
    sp.avgCadence = metrics::mean(cad);
    sp.avgSpeedKmh = dist / dur * 3.6;
    sp.avgGradePct = dist > 1 ? (s.ele[b - 1] - e0) / dist * 100 : 0;
    return sp;
}

json::Value Split::toJson() const {
    Value v = Value::object();
    v["index"] = index; v["kind"] = kind; v["name"] = name; v["start_s"] = startS; v["duration_s"] = durationS;
    v["distance_m"] = distanceM; v["elevation_gain_m"] = elevationGainM; v["avg_power"] = avgPower; v["max_power"] = maxPower;
    v["np"] = np; v["avg_hr"] = optNum(avgHr); v["avg_cadence"] = avgCadence; v["avg_speed_kmh"] = avgSpeedKmh; v["avg_grade_pct"] = avgGradePct;
    return v;
}

Split Split::fromJson(const json::Value& v) {
    Split s;
    s.index = int(v.numOr("index", 0)); s.kind = v.strOr("kind", "lap"); s.name = v.strOr("name");
    s.startS = v.numOr("start_s", 0); s.durationS = v.numOr("duration_s", 0); s.distanceM = v.numOr("distance_m", 0);
    s.elevationGainM = v.numOr("elevation_gain_m", 0); s.avgPower = v.numOr("avg_power", 0); s.maxPower = v.numOr("max_power", 0);
    s.np = v.numOr("np", 0); s.avgHr = getOptNum(v, "avg_hr"); s.avgCadence = v.numOr("avg_cadence", 0);
    s.avgSpeedKmh = v.numOr("avg_speed_kmh", 0); s.avgGradePct = v.numOr("avg_grade_pct", 0);
    return s;
}

json::Value testResultJson(const wk::TestResult& r) {
    Value v = Value::object();
    v["kind"] = testKindId(r.kind); v["ftp"] = r.ftp; v["measured_w"] = r.measuredW; v["factor"] = r.factor;
    v["detail"] = r.detail; v["max_power"] = r.maxPower; v["valid"] = r.valid;
    return v;
}

std::optional<wk::TestResult> testResultFromJson(const json::Value& v) {
    if (v.type != Value::Object) return std::nullopt;
    wk::TestResult r;
    r.kind = testKindFromId(v.strOr("kind", "ramp")).value_or(wk::TestKind::Ramp);
    r.ftp = v.numOr("ftp", 0); r.measuredW = v.numOr("measured_w", 0); r.factor = v.numOr("factor", 0);
    r.detail = v.strOr("detail"); r.maxPower = v.numOr("max_power", 0); r.valid = v.boolOr("valid", false);
    return r;
}

// ------------------------------------------------------------------ summary
json::Value RideSummary::toJson() const {
    Value v = Value::object();
    v["id"] = id; v["name"] = name; v["kind"] = rideKindId(kind); v["started_at"] = startedAt; v["ended_at"] = endedAt;
    v["route_id"] = routeId; v["route_name"] = routeName; v["workout_id"] = workoutId; v["workout_name"] = workoutName; v["event_id"] = eventId;
    v["test"] = test ? Value(testKindId(*test)) : Value();
    v["laps"] = laps; v["completed"] = completed; v["route_time_s"] = optNum(routeTimeS); v["personal_best"] = personalBest;
    v["elapsed_s"] = elapsedS; v["moving_s"] = movingS; v["distance_m"] = distanceM; v["elevation_gain_m"] = elevationGainM;
    v["avg_speed_mps"] = avgSpeedMps; v["max_speed_mps"] = maxSpeedMps; v["avg_power"] = avgPower; v["max_power"] = maxPower;
    v["np"] = np; v["intensity_factor"] = intensityFactor; v["tss"] = tss; v["variability_index"] = variabilityIndex;
    v["work_kj"] = workKj; v["calories_kcal"] = caloriesKcal; v["calories_hr_kcal"] = optNum(caloriesHrKcal);
    v["avg_hr"] = optNum(avgHr); v["max_hr"] = optNum(maxHr); v["avg_cadence"] = avgCadence; v["max_cadence"] = maxCadence;
    v["ftp"] = ftp; v["weight_kg"] = weightKg;
    Value pb = Value::array();
    for (auto& [d, w] : powerBests) { Value pr = Value::array(); pr.push(Value(d)); pr.push(Value(w)); pb.push(pr); }
    v["power_bests"] = pb;
    Value z = Value::array();
    for (double s : zoneS) z.push(s);
    v["zone_s"] = z;
    v["test_result"] = testResult ? testResultJson(*testResult) : Value();
    v["notes"] = notes;
    v["splits"] = Value::arrayOf(splits, [](const Split& s) { return s.toJson(); });
    v["route_modified"] = routeModified;
    v["hr_zone_s"] = Value::numbers(hrZoneS);
    v["strava_activity_id"] = stravaActivityId ? Value(*stravaActivityId) : Value();
    v["simulated"] = simulated;
    return v;
}

RideSummary RideSummary::fromJson(const json::Value& v) {
    RideSummary r;
    r.id = v.strOr("id"); r.name = v.strOr("name", "Ride"); r.kind = rideKindFromId(v.strOr("kind", "free"));
    r.startedAt = v.strOr("started_at"); r.endedAt = v.strOr("ended_at");
    r.routeId = getOptStr(v, "route_id"); r.routeName = getOptStr(v, "route_name"); r.workoutId = getOptStr(v, "workout_id");
    r.workoutName = getOptStr(v, "workout_name"); r.eventId = getOptStr(v, "event_id");
    if (auto t = v.str("test")) r.test = testKindFromId(*t);
    r.laps = int(v.numOr("laps", 1)); r.completed = v.boolOr("completed", false); r.routeTimeS = getOptNum(v, "route_time_s");
    r.personalBest = v.boolOr("personal_best", false);
    r.elapsedS = v.numOr("elapsed_s", 0); r.movingS = v.numOr("moving_s", 0); r.distanceM = v.numOr("distance_m", 0);
    r.elevationGainM = v.numOr("elevation_gain_m", 0); r.avgSpeedMps = v.numOr("avg_speed_mps", 0); r.maxSpeedMps = v.numOr("max_speed_mps", 0);
    r.avgPower = v.numOr("avg_power", 0); r.maxPower = v.numOr("max_power", 0); r.np = v.numOr("np", 0);
    r.intensityFactor = v.numOr("intensity_factor", 0); r.tss = v.numOr("tss", 0); r.variabilityIndex = v.numOr("variability_index", 0);
    r.workKj = v.numOr("work_kj", 0); r.caloriesKcal = v.numOr("calories_kcal", 0); r.caloriesHrKcal = getOptNum(v, "calories_hr_kcal");
    r.avgHr = getOptNum(v, "avg_hr"); r.maxHr = getOptNum(v, "max_hr"); r.avgCadence = v.numOr("avg_cadence", 0);
    r.maxCadence = v.numOr("max_cadence", 0); r.ftp = v.numOr("ftp", 0); r.weightKg = v.numOr("weight_kg", 0);
    for (auto& pb : v.arr("power_bests"))
        if (pb.type == Value::Array && pb.a.size() == 2) r.powerBests.push_back({int(pb.a[0].n), pb.a[1].n});
    auto z = v.arr("zone_s");
    for (size_t i = 0; i < 7 && i < z.size(); ++i) r.zoneS[i] = z[i].n;
    if (auto tr = v.get("test_result")) r.testResult = testResultFromJson(*tr);
    r.notes = v.strOr("notes");
    for (auto& s : v.arr("splits")) r.splits.push_back(Split::fromJson(s));
    r.routeModified = v.boolOr("route_modified", false);
    r.hrZoneS = v.numsAs<double>("hr_zone_s");
    if (auto id = v.num("strava_activity_id")) r.stravaActivityId = (unsigned long long)*id;
    r.simulated = v.boolOr("simulated", false);
    return r;
}

// ------------------------------------------------------------------ ghost
GhostTrace GhostTrace::fromRecord(const RideRecord& r) {
    GhostTrace g;
    g.rideId = r.summary.id;
    g.startedAt = r.summary.startedAt;
    g.totalS = r.summary.routeTimeS.value_or(r.summary.elapsedS);
    for (auto x : r.samples.t) g.t.push_back(float(x));
    g.d = r.samples.distance;
    return g;
}

std::optional<double> GhostTrace::timeAtDistance(double dist) const {
    size_t n = d.size();
    if (n == 0 || dist > d[n - 1]) return std::nullopt;
    size_t idx = size_t(std::partition_point(d.begin(), d.end(), [&](float x) { return x < dist; }) - d.begin());
    if (idx == 0) return t[0] * std::min(1.0, dist / std::max<double>(d[0], 1e-6));
    double d0 = d[idx - 1], d1 = d[idx], t0 = t[idx - 1], t1 = t[idx];
    double f = d1 > d0 ? (dist - d0) / (d1 - d0) : 0;
    return t0 + f * (t1 - t0);
}

double GhostTrace::distanceAtTime(double tt) const {
    size_t n = t.size();
    if (n == 0) return 0;
    size_t idx = size_t(std::partition_point(t.begin(), t.end(), [&](float x) { return x < tt; }) - t.begin());
    if (idx == 0) return d[0];
    if (idx >= n) return d[n - 1];
    double t0 = t[idx - 1], t1 = t[idx], d0 = d[idx - 1], d1 = d[idx];
    double f = t1 > t0 ? (tt - t0) / (t1 - t0) : 0;
    return d0 + f * (d1 - d0);
}

// ------------------------------------------------------------------ events
static const char* freqId(Freq f) {
    static const char* n[] = {"once", "daily", "weekly", "every_n_days"};
    return n[int(f)];
}

json::Value Event::toJson() const {
    Value v = Value::object();
    v["id"] = id; v["name"] = name; v["description"] = description; v["created_at"] = createdAt; v["color"] = color; v["active"] = active;
    Value t = Value::object();
    if (target.type == EventTarget::Route) { t["type"] = "route"; t["route_id"] = target.routeId; t["laps"] = target.laps; }
    else if (target.type == EventTarget::Workout) { t["type"] = "workout"; t["workout_id"] = target.workoutId; t["route_id"] = target.workoutRouteId; }
    else { t["type"] = "test"; t["test"] = testKindId(target.test); }
    v["target"] = t;
    Value r = Value::object();
    r["freq"] = freqId(recurrence.freq); r["start_date"] = recurrence.startDate; r["time"] = recurrence.time;
    r["end_date"] = recurrence.endDate;
    Value wd = Value::array();
    for (int d : recurrence.weekdays) wd.push(Value(d));
    r["weekdays"] = wd;
    r["interval"] = recurrence.interval;
    v["recurrence"] = r;
    return v;
}

std::optional<Event> Event::fromJson(const json::Value& v) {
    if (v.type != Value::Object) return std::nullopt;
    Event e;
    e.id = v.strOr("id"); e.name = v.strOr("name", "Event"); e.description = v.strOr("description"); e.createdAt = v.strOr("created_at");
    e.color = v.strOr("color", "#1DB870"); e.active = v.boolOr("active", true);
    if (auto t = v.get("target")) {
        std::string ty = t->strOr("type", "route");
        if (ty == "workout") { e.target.type = EventTarget::Workout; e.target.workoutId = t->strOr("workout_id"); e.target.workoutRouteId = t->str("route_id"); }
        else if (ty == "test") { e.target.type = EventTarget::Test; e.target.test = testKindFromId(t->strOr("test", "ramp")).value_or(wk::TestKind::Ramp); }
        else { e.target.type = EventTarget::Route; e.target.routeId = t->strOr("route_id"); e.target.laps = int(t->numOr("laps", 1)); }
    }
    if (auto r = v.get("recurrence")) {
        std::string f = r->strOr("freq", "weekly");
        e.recurrence.freq = f == "once" ? Freq::Once : f == "daily" ? Freq::Daily : f == "every_n_days" ? Freq::EveryNDays : Freq::Weekly;
        e.recurrence.startDate = r->strOr("start_date", Date::today().str());
        e.recurrence.time = r->strOr("time", "18:00");
        e.recurrence.endDate = r->str("end_date");
        for (auto& w : r->arr("weekdays")) e.recurrence.weekdays.push_back(int(w.n));
        e.recurrence.interval = int(r->numOr("interval", 1));
    }
    return e;
}

bool occursOn(const Event& ev, const Date& d) {
    const Recurrence& r = ev.recurrence;
    auto start = Date::parse(r.startDate);
    if (!start || d < *start) return false;
    if (r.endDate) if (auto end = Date::parse(*r.endDate); end && end->days() < d.days()) return false;
    long interval = std::max(1, r.interval);
    long days = d - *start;
    switch (r.freq) {
        case Freq::Once: return d == *start;
        case Freq::Daily:
        case Freq::EveryNDays: return days % interval == 0;
        case Freq::Weekly: {
            int wd = d.weekday();
            bool okDay = r.weekdays.empty() ? wd == start->weekday() : std::find(r.weekdays.begin(), r.weekdays.end(), wd) != r.weekdays.end();
            long week = (d - start->monday()) / 7;
            return okDay && week % interval == 0;
        }
    }
    return false;
}

std::vector<Date> occurrences(const Event& e, const Date& from, const Date& to) {
    std::vector<Date> out;
    if (!e.active || to < from) return out;
    for (Date d = from; d <= to && out.size() < 400; d = d.plus(1))
        if (occursOn(e, d)) out.push_back(d);
    return out;
}

std::vector<Occurrence> upcoming(const std::vector<Event>& events, const Date& from, int days) {
    Date to = from.plus(std::max(0, days));
    std::vector<Occurrence> v;
    for (auto& e : events)
        for (auto& d : occurrences(e, from, to)) v.push_back({e.id, e.name, e.color, d, e.recurrence.time});
    std::sort(v.begin(), v.end(), [](const Occurrence& a, const Occurrence& b) {
        return a.date.days() != b.date.days() ? a.date.days() < b.date.days() : a.time < b.time;
    });
    return v;
}

std::string recurrenceLabel(const Recurrence& r) {
    std::string s;
    switch (r.freq) {
        case Freq::Once: s = "Once on " + r.startDate; break;
        case Freq::Daily: s = r.interval > 1 ? "Every " + std::to_string(r.interval) + " days" : "Daily"; break;
        case Freq::EveryNDays: s = "Every " + std::to_string(std::max(1, r.interval)) + " days"; break;
        case Freq::Weekly: {
            s = r.interval > 1 ? "Every " + std::to_string(r.interval) + " weeks" : "Weekly";
            if (!r.weekdays.empty()) {
                s += " on ";
                for (size_t i = 0; i < r.weekdays.size(); ++i) s += std::string(i ? ", " : "") + dates::weekdayShort(r.weekdays[i]);
            }
            break;
        }
    }
    return s + " at " + r.time;
}

// ------------------------------------------------------------------ stats
void Totals::add(const RideSummary& r) {
    rides += 1; distanceM += r.distanceM; movingS += r.movingS; elevationM += r.elevationGainM;
    workKj += r.workKj; caloriesKcal += r.caloriesKcal; tss += r.tss;
}

static void rec(std::optional<RecordRef>& best, const RideSummary& r, double value, const Date& d) {
    if (value <= 0) return;
    if (!best || value > best->value) best = RecordRef{r.id, r.name, d, value};
}

static std::vector<CurvePoint> curve(const std::vector<std::pair<Date, const RideSummary*>>& rides, double weight) {
    std::vector<CurvePoint> out;
    for (int d : metrics::BEST_DURATIONS) {
        std::optional<CurvePoint> best;
        for (auto& [date, r] : rides)
            for (auto& [dd, w] : r->powerBests)
                if (dd == d && (!best || w > best->watts)) {
                    double kg = r->weightKg > 0 ? r->weightKg : weight;
                    best = CurvePoint{d, w, kg > 0 ? w / kg : 0, r->id, date};
                }
        if (best) out.push_back(*best);
    }
    return out;
}

AccountStats computeStats(const std::vector<RideSummary>& rides, double weightKg, std::vector<FtpEntry> ftpHistory, const Date& today) {
    AccountStats s;
    std::vector<std::pair<Date, const RideSummary*>> dated;
    for (auto& r : rides) if (auto d = dates::localDate(r.startedAt)) dated.push_back({*d, &r});
    Date thisMonday = today.monday(), lastMonday = thisMonday.plus(-7);
    std::map<long, Totals> byWeek;
    std::map<long, double> tssByDay;
    for (auto& [d, r] : dated) {
        s.allTime.add(*r);
        if (thisMonday <= d) s.thisWeek.add(*r);
        else if (lastMonday <= d) s.lastWeek.add(*r);
        if (d.y == today.y && d.m == today.m) s.thisMonth.add(*r);
        if (d.y == today.y) s.thisYear.add(*r);
        byWeek[d.monday().days()].add(*r);
        tssByDay[d.days()] += r->tss;
        if (today - d < 28) for (int z = 0; z < 7; ++z) s.zoneS28d[z] += r->zoneS[z];
        rec(s.records.longestDistance, *r, r->distanceM, d);
        rec(s.records.longestTime, *r, r->movingS, d);
        rec(s.records.mostElevation, *r, r->elevationGainM, d);
        rec(s.records.mostWork, *r, r->workKj, d);
        if (r->movingS >= 1200) rec(s.records.highestNp, *r, r->np, d);
        rec(s.records.highestTss, *r, r->tss, d);
    }
    for (int k = 15; k >= 0; --k) {
        Date ws = thisMonday.plus(-7 * k);
        auto it = byWeek.find(ws.days());
        s.weekly.push_back({ws, it != byWeek.end() ? it->second : Totals{}});
    }
    long first = tssByDay.empty() ? today.days() : tssByDay.begin()->first;
    long start = std::min(first, today.plus(-180).days());
    double kc = 1 - std::exp(-1.0 / 42), ka = 1 - std::exp(-1.0 / 7);
    double ctl = 0, atl = 0;
    for (long d = start; d <= today.days(); ++d) {
        auto it = tssByDay.find(d);
        double t = it != tssByDay.end() ? it->second : 0;
        double tsb = ctl - atl;
        ctl += (t - ctl) * kc;
        atl += (t - atl) * ka;
        if (today.days() - d < 120) s.fitness.push_back({Date::fromDays(d), t, ctl, atl, tsb});
    }
    long wk = byWeek.count(thisMonday.days()) ? thisMonday.days() : lastMonday.days();
    while (byWeek.count(wk) && byWeek[wk].rides > 0) { ++s.weekStreak; wk -= 7; }
    std::vector<std::pair<Date, const RideSummary*>> recent;
    for (auto& x : dated) if (today - x.first <= 90) recent.push_back(x);
    s.powerCurveAll = curve(dated, weightKg);
    s.powerCurve90d = curve(recent, weightKg);
    s.ftpHistory = std::move(ftpHistory);
    return s;
}

// ------------------------------------------------------------------ eFTP
bool isValidFtpRide(const RideSummary& r) {
    return !r.simulated && r.movingS >= 1200 && r.avgPower > 20 && r.maxPower < 2500 && !r.powerBests.empty();
}

std::optional<std::tuple<double, double, double>> fitCp(const std::vector<std::pair<double, double>>& pts) {
    if (pts.size() < 3) return std::nullopt;
    double n = double(pts.size()), mx = 0, my = 0;
    std::vector<double> xs, ys;
    for (auto& p : pts) { xs.push_back(p.first); ys.push_back(p.first * p.second); mx += p.first; my += p.first * p.second; }
    mx /= n; my /= n;
    double sxx = 0, sxy = 0;
    for (size_t i = 0; i < xs.size(); ++i) { sxx += (xs[i] - mx) * (xs[i] - mx); sxy += (xs[i] - mx) * (ys[i] - my); }
    if (sxx <= 0) return std::nullopt;
    double cp = sxy / sxx, wp = my - cp * mx;
    double ssTot = 0, ssRes = 0;
    for (size_t i = 0; i < xs.size(); ++i) { ssTot += (ys[i] - my) * (ys[i] - my); double e = ys[i] - (cp * xs[i] + wp); ssRes += e * e; }
    double r2 = ssTot > 0 ? 1 - ssRes / ssTot : 0;
    return std::make_tuple(cp, wp, r2);
}

std::optional<FtpEstimate> estimateFtp(const std::vector<RideSummary>& rides, const Date& today) {
    std::vector<std::pair<const RideSummary*, Date>> recent;
    for (auto& r : rides) {
        if (!isValidFtpRide(r)) continue;
        auto d = dates::localDate(r.startedAt);
        if (d && today - *d <= 90 && *d <= today) recent.push_back({&r, *d});
    }
    if (recent.empty()) return std::nullopt;
    bool hard = false;
    for (auto& x : recent) if (x.first->intensityFactor >= 0.85) hard = true;
    static const int CP_DUR[5] = {180, 300, 480, 600, 1200};
    std::vector<EffortPoint> points;
    for (int dur : CP_DUR) {
        std::optional<EffortPoint> best;
        for (auto& [r, d] : recent)
            for (auto& [x, w] : r->powerBests)
                if (x == dur && (!best || w > best->watts)) best = EffortPoint{dur, w, r->id, r->name, d.str()};
        if (best) points.push_back(*best);
    }
    std::optional<double> b20;
    bool shortE = false, longE = false;
    for (auto& p : points) {
        if (p.durationS == 1200) b20 = p.watts;
        if (p.durationS <= 300) shortE = true;
        if (p.durationS >= 600) longE = true;
    }
    if (points.size() >= 3 && shortE && longE) {
        std::vector<std::pair<double, double>> xy;
        double maxP = 0;
        for (auto& p : points) { xy.push_back({double(p.durationS), p.watts}); maxP = std::max(maxP, p.watts); }
        if (auto f = fitCp(xy)) {
            auto [cp, wp, r2] = *f;
            bool plausible = cp >= 50 && cp <= 600 && wp >= 3000 && wp <= 60000 && cp < maxP && r2 > 0.95;
            if (plausible) {
                FtpEstimate e;
                e.estimate = std::round(cp); e.method = "critical_power"; e.cp = cp; e.wPrimeKj = wp / 1000; e.r2 = r2; e.points = points;
                e.confidence = points.size() >= 4 && b20 && r2 > 0.99 ? "high" : "medium";
                e.ridesConsidered = int(recent.size()); e.hardRideInWindow = hard;
                return e;
            }
        }
    }
    if (!b20) return std::nullopt;
    FtpEstimate e;
    e.estimate = std::round(*b20 * 0.95); e.method = "best_20min"; e.points = points; e.confidence = "low";
    e.ridesConsidered = int(recent.size()); e.hardRideInWindow = hard;
    return e;
}

FtpAssessment assessFtp(double cur, std::optional<FtpEstimate> est, const Date& date, std::optional<std::string> trigger) {
    FtpAssessment a;
    a.date = date.str();
    a.currentFtp = cur;
    a.estimate = est;
    a.triggerRideId = trigger;
    a.reason = "Not enough hard efforts in the last 90 days - ride at least 20 minutes with some sustained efforts, or take a test.";
    if (!est) return a;
    double diff = est->estimate - cur;
    a.changeW = diff;
    a.changePct = cur > 0 ? diff / cur * 100 : 0;
    std::string how = est->method == "critical_power"
                          ? "critical-power fit of your best " + std::to_string(est->points.size()) + " efforts (3-20 min) from the last 90 days"
                          : "95 % of your best 20-minute power in the last 90 days";
    if (diff >= std::max(cur * 0.03, 5.0)) {
        a.suggestion = Suggestion::Increase;
        a.suggestedFtp = est->estimate;
        a.reason = "Your riding shows more than your current FTP - " + how + ".";
    } else if (-diff >= std::max(cur * 0.05, 8.0) && est->confidence == "high" && est->hardRideInWindow) {
        a.suggestion = Suggestion::Decrease;
        a.suggestedFtp = est->estimate;
        a.reason = "Your recent maximal efforts are below your FTP - " + how + ". Consider a test to confirm.";
    } else {
        a.suggestion = Suggestion::Hold;
        a.reason = "Your FTP still matches your riding (" + how + ").";
    }
    return a;
}

static const char* suggestionId(Suggestion s) {
    static const char* n[] = {"increase", "decrease", "hold", "not_enough_data"};
    return n[int(s)];
}

json::Value FtpAssessment::toJson() const {
    Value v = Value::object();
    v["date"] = date; v["current_ftp"] = currentFtp; v["suggested_ftp"] = optNum(suggestedFtp); v["change_w"] = changeW; v["change_pct"] = changePct;
    v["suggestion"] = suggestionId(suggestion); v["reason"] = reason; v["trigger_ride_id"] = triggerRideId;
    v["auto_applied"] = autoApplied; v["dismissed"] = dismissed;
    if (estimate) {
        Value e = Value::object();
        e["estimate"] = estimate->estimate; e["method"] = estimate->method; e["cp"] = optNum(estimate->cp); e["w_prime_kj"] = optNum(estimate->wPrimeKj);
        e["r2"] = optNum(estimate->r2); e["confidence"] = estimate->confidence; e["rides_considered"] = estimate->ridesConsidered;
        e["hard_ride_in_window"] = estimate->hardRideInWindow;
        e["points"] = Value::arrayOf(estimate->points, [](const EffortPoint& p) {
            Value o = Value::object();
            o["duration_s"] = p.durationS; o["watts"] = p.watts; o["ride_id"] = p.rideId; o["ride_name"] = p.rideName; o["date"] = p.date;
            return o;
        });
        v["estimate"] = e;
    } else v["estimate"] = nullptr;
    return v;
}

std::optional<FtpAssessment> FtpAssessment::fromJson(const json::Value& v) {
    if (v.type != Value::Object) return std::nullopt;
    FtpAssessment a;
    a.date = v.strOr("date"); a.currentFtp = v.numOr("current_ftp", 0); a.suggestedFtp = v.num("suggested_ftp");
    a.changeW = v.numOr("change_w", 0); a.changePct = v.numOr("change_pct", 0);
    std::string s = v.strOr("suggestion", "not_enough_data");
    a.suggestion = s == "increase" ? Suggestion::Increase : s == "decrease" ? Suggestion::Decrease : s == "hold" ? Suggestion::Hold : Suggestion::NotEnoughData;
    a.reason = v.strOr("reason"); a.triggerRideId = v.str("trigger_ride_id");
    a.autoApplied = v.boolOr("auto_applied", false); a.dismissed = v.boolOr("dismissed", false);
    if (auto e = v.get("estimate"); e && e->type == Value::Object) {
        FtpEstimate x;
        x.estimate = e->numOr("estimate", 0); x.method = e->strOr("method"); x.cp = e->num("cp"); x.wPrimeKj = e->num("w_prime_kj");
        x.r2 = e->num("r2"); x.confidence = e->strOr("confidence"); x.ridesConsidered = int(e->numOr("rides_considered", 0));
        x.hardRideInWindow = e->boolOr("hard_ride_in_window", false);
        for (auto& p : e->arr("points")) x.points.push_back({int(p.numOr("duration_s", 0)), p.numOr("watts", 0), p.strOr("ride_id"), p.strOr("ride_name"), p.strOr("date")});
        a.estimate = x;
    }
    return a;
}
