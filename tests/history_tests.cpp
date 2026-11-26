// Ports of CoreKick's route / stats / eFTP / events / weather / sky tests,
// plus JSON round-trips against CoreKick's serde layout.
#include <cmath>
#include <cstdio>
#include <string>

#include "core/Dates.h"
#include "core/Engine.h"
#include "core/Json.h"
#include "core/Records.h"
#include "core/Route.h"
#include "core/Weather.h"

static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static Date D(const char* s) { return *Date::parse(s); }

static RideSummary ride(const char* id, const char* date, double tss, double km) {
    RideSummary r;
    r.id = id; r.name = id; r.startedAt = std::string(date) + "T12:00:00+00:00";
    r.tss = tss; r.distanceM = km * 1000; r.movingS = 3600; r.np = 200 + tss;
    r.powerBests = {{5, 600 + tss}, {1200, 250}};
    r.weightKg = 75;
    return r;
}

static RideSummary cpRide(const char* id, const char* date, double cp, double wp, double iff) {
    RideSummary r;
    r.id = id; r.name = id; r.startedAt = std::string(date) + "T18:00:00+00:00";
    r.movingS = 3600; r.avgPower = cp * 0.8; r.maxPower = cp * 3; r.intensityFactor = iff;
    for (int d : {1, 5, 60, 180, 300, 480, 600, 1200}) r.powerBests.push_back({d, d >= 180 ? cp + wp / d : cp * 2});
    return r;
}

int main() {
    // ---------------- dates
    {
        CHECK(D("2026-11-23").weekday() == 0);  // Monday
        CHECK(D("2026-11-02").weekday() == 0);
        CHECK(D("2026-11-23").plus(7).str() == "2026-11-30");
        CHECK(D("2026-03-01").plus(-1).str() == "2026-02-28");
        CHECK(D("2024-03-01").plus(-1).str() == "2024-02-29");
        CHECK(D("2026-11-25").monday().str() == "2026-11-23");
        auto t = dates::parseRfc3339("2026-11-20T18:00:00+01:00");
        CHECK(t && *t == 1795194000);
        CHECK(dates::newId().size() == 36);
        Rng a(42), b(42);
        bool same = true;
        for (int i = 0; i < 100; ++i) same &= a.next() == b.next();
        CHECK(same);
    }
    // ---------------- routes
    {
        struct G { Terrain t; double km, gain; };
        for (G g : {G{Terrain::Rolling, 30, 300}, G{Terrain::Hilly, 50, 800}, G{Terrain::Mountainous, 80, 1800}, G{Terrain::Alpine, 20, 1000}}) {
            GenParams p;
            p.distanceKm = g.km; p.targetGainM = g.gain; p.maxGradePct = 14; p.terrain = g.t; p.finish = Finish::Free; p.seed = 7; p.startEleM = 100;
            Route r = generateRoute(p, "t");
            auto s = r.stats();
            CHECK(std::fabs(s.distanceM - g.km * 1000) < 25);
            CHECK(std::fabs(s.gainM - g.gain) / g.gain < 0.12);
            CHECK(s.maxGradePct <= 14.5);
        }
        GenParams lp; lp.distanceKm = 25; lp.targetGainM = 400; lp.terrain = Terrain::Hilly; lp.finish = Finish::Loop; lp.maxGradePct = 10;
        Route loop = generateRoute(lp, "loop");
        CHECK(loop.isLoop && std::fabs(loop.points.front().ele - loop.points.back().ele) < 1.0);
        GenParams sp = lp; sp.finish = Finish::Summit; sp.terrain = Terrain::Alpine; sp.targetGainM = 1000; sp.distanceKm = 20;
        Route summit = generateRoute(sp, "summit");
        CHECK(summit.points.back().ele > summit.points.front().ele + 300);
        // determinism
        CHECK(generateRoute(lp, "a").points.back().ele == generateRoute(lp, "b").points.back().ele);
        // builder
        Route b = buildFromSegments("b", {{1000, 0}, {2000, 5}, {1000, -2}}, 100, false);
        CHECK(std::fabs(b.distance() - 4000) < 1 && std::fabs(b.elevationAt(3000) - 200) < 1);
        auto st = b.stats();
        CHECK(st.climbs.size() == 1 && std::fabs(st.climbs[0].gainM - 100) < 2);
        // GPX
        const char* gpx = R"(<?xml version="1.0"?><gpx><trk><name>Test Loop</name><trkseg>
            <trkpt lat="55.9500" lon="-3.1900"><ele>50</ele></trkpt>
            <trkpt lat="55.9600" lon="-3.1900"><ele>80</ele></trkpt>
            <trkpt lat="55.9600" lon="-3.1700"><ele>90</ele></trkpt>
            <trkpt lat="55.9501" lon="-3.1901"><ele>51</ele></trkpt></trkseg></trk></gpx>)";
        Route g;
        std::string err;
        CHECK(routeFromGpx(gpx, "", g, err));
        CHECK(g.name == "Test Loop" && g.isLoop);
        CHECK(g.distance() > 3500 && g.distance() < 4500);
        CHECK(!routeFromGpx("<gpx></gpx>", "", g, err));
        // sectors
        Route base = buildFromSegments("flat", {{5000, 0}}, 100, false);
        SectorSpec climb; climb.kind = SectorKind::Climb; climb.lengthM = 1000; climb.gradePct = 6;
        auto segs = climb.build(1);
        auto [l, net] = segmentsSummary(segs);
        CHECK(std::fabs(l - 1000) < 1 && std::fabs(net - 60) < 0.5);
        auto rj = rejoinSegment(net);
        CHECK(rj && rj->gradePct < 0 && std::fabs(rj->lengthM * rj->gradePct / 100 + 60) < 0.5);
        segs.push_back(*rj);
        auto [ins, de] = insertSegments(base, 2000, segs);
        CHECK(std::fabs(ins - (1000 + rj->lengthM)) < 15 && std::fabs(de) < 1.0);
        CHECK(std::fabs(base.distance() - 5000 - ins) < 1);
        CHECK(base.elevationAt(3000) > 150);
        SectorSpec rollers; rollers.kind = SectorKind::Rollers; rollers.lengthM = 2000;
        auto rs = rollers.build(1);
        CHECK(rs.size() % 2 == 0 && std::fabs(segmentsSummary(rs).second) < 1e-6);
        CHECK(climb.displayName() == "Climb 1.0 km @ 6.0%");
        // JSON round-trip
        json::Value v;
        CHECK(json::parse(json::dump(loop.toJson()), v));
        auto back = Route::fromJson(v);
        CHECK(back && back->points.size() == loop.points.size() && back->isLoop && back->genParams && back->genParams->terrain == Terrain::Hilly);
        auto seeds = seedRoutes();
        CHECK(seeds.size() == 6);
        CHECK(seeds[3].stats().climbs.size() >= 1);
    }
    // ---------------- engine sectors
    {
        Profile pr;
        RideSetup su;
        su.kind = RideKind::Route;
        su.route = buildFromSegments("flat", {{8000, 0}}, 100, false);
        RideEngine e(pr, su);
        for (int i = 0; i < 40; ++i) e.tick(0.25, {200, 90, {}, true});
        SectorSpec c; c.kind = SectorKind::Climb; c.lengthM = 600; c.gradePct = 8; c.inM = 200;
        double before = e.route().distance();
        e.addSector(c);
        CHECK(e.route().distance() > before + 500);
        CHECK(e.sectors().size() == 1 && e.routeModified() && e.canUndoSector());
        CHECK(e.undoSector());
        CHECK(std::fabs(e.route().distance() - before) < 1 && e.sectors().empty());
        e.addSector(c);
        double startD = e.sectors()[0].startTotal;
        for (int i = 0; i < 4 * 60 * 15 && !e.sectors()[0].done; ++i) e.tick(0.25, {300, 90, {}, true});
        CHECK(e.sectors()[0].done && e.sectors()[0].result.has_value());
        CHECK(e.distance() > startD);
        bool sectorSplit = false;
        for (auto& s : e.splits()) if (s.kind == "sector") sectorSplit = true;
        CHECK(sectorSplit);
        auto rec = e.finish(dates::nowRfc3339(), "r1");
        CHECK(rec.summary.routeModified && !rec.summary.routeId);
        // JSON round-trip of a full record
        json::Value sv, smv;
        CHECK(json::parse(json::dump(rec.summary.toJson()), sv));
        CHECK(json::parse(json::dump(rec.samples.toJson()), smv));
        RideSummary s2 = RideSummary::fromJson(sv);
        Samples m2 = Samples::fromJson(smv);
        CHECK(s2.id == "r1" && s2.kind == RideKind::Route && std::fabs(s2.np - rec.summary.np) < 1e-6 && s2.splits.size() == rec.summary.splits.size());
        CHECK(m2.size() == rec.samples.size() && m2.t.back() == rec.samples.t.back());
        // integer fields serialise like serde (no decimal point)
        std::string js = json::dump(rec.summary.toJson());
        CHECK(js.find("\"laps\":1,") != std::string::npos);
        // ghost
        GhostTrace g = GhostTrace::fromRecord(rec);
        CHECK(!g.empty());
        double half = rec.samples.distance[rec.samples.size() / 2];
        auto tg = g.timeAtDistance(half);
        CHECK(tg && std::fabs(*tg - double(rec.samples.size() / 2)) < 1.5);
        CHECK(std::fabs(g.distanceAtTime(double(rec.samples.size() / 2)) - half) < 1e-3);
    }
    // ---------------- stats (CoreKick stats.rs tests)
    {
        Date today = D("2026-11-23");
        std::vector<RideSummary> rides = {ride("a", "2026-11-23", 80, 40), ride("b", "2026-11-18", 60, 30), ride("c", "2026-11-10", 50, 25),
                                          ride("d", "2026-06-01", 100, 90)};
        auto s = computeStats(rides, 75, {}, today);
        CHECK(s.allTime.rides == 4 && s.thisWeek.rides == 1 && s.lastWeek.rides == 1 && s.thisMonth.rides == 3);
        CHECK(s.weekStreak == 3);
        CHECK(s.weekly.size() == 16 && s.weekly.back().weekStart.str() == "2026-11-23");
        CHECK(s.records.longestDistance && s.records.longestDistance->rideId == "d");
        auto& last = s.fitness.back();
        CHECK(last.ctl > 0 && last.atl > last.ctl);
        double five = 0, five90 = 0;
        for (auto& c : s.powerCurveAll) if (c.durationS == 5) five = c.watts;
        for (auto& c : s.powerCurve90d) if (c.durationS == 5) five90 = c.watts;
        CHECK(five == 700 && five90 == 680);
        auto e = computeStats({}, 75, {}, today);
        CHECK(e.allTime.rides == 0 && e.weekStreak == 0 && e.powerCurveAll.empty());
    }
    // ---------------- eFTP (CoreKick ftp_estimate.rs tests)
    {
        Date today = D("2026-11-23");
        auto e = estimateFtp({cpRide("a", "2026-11-20", 250, 20000, 0.9)}, today);
        CHECK(e && e->method == "critical_power" && e->estimate == 250 && std::fabs(*e->wPrimeKj - 20) < 0.1 && e->confidence == "high");
        auto a = cpRide("short", "2026-11-10", 270, 20000, 0.9);
        std::vector<std::pair<int, double>> keep;
        for (auto& p : a.powerBests) if (p.first <= 300) keep.push_back(p);
        a.powerBests = keep;
        auto b = cpRide("long", "2026-11-18", 270, 20000, 0.9);
        keep.clear();
        for (auto& p : b.powerBests) if (p.first >= 600) keep.push_back(p);
        b.powerBests = keep;
        auto old = cpRide("old", "2026-06-01", 400, 20000, 0.9);
        auto e2 = estimateFtp({a, b, old}, today);
        CHECK(e2 && e2->estimate == 270 && e2->points.size() == 4 && e2->points[0].rideId == "short" && e2->points[3].rideId == "long");
        auto sim = cpRide("sim", "2026-11-20", 300, 20000, 0.9); sim.simulated = true;
        auto sh = cpRide("short", "2026-11-20", 300, 20000, 0.9); sh.movingS = 600;
        auto np = cpRide("np", "2026-11-20", 300, 20000, 0.9); np.avgPower = 0;
        CHECK(!estimateFtp({sim, sh, np}, today));
        auto f = cpRide("a", "2026-11-20", 250, 20000, 0.9);
        keep.clear();
        for (auto& p : f.powerBests) if (p.first == 1200 || p.first < 180) keep.push_back(p);
        f.powerBests = keep;
        auto e3 = estimateFtp({f}, today);
        CHECK(e3 && e3->method == "best_20min" && e3->estimate == std::round((250 + 20000.0 / 1200) * 0.95) && e3->confidence == "low");
        auto est = estimateFtp({cpRide("a", "2026-11-20", 260, 20000, 0.9)}, today);
        auto as = assessFtp(240, est, today, std::nullopt);
        CHECK(as.suggestion == Suggestion::Increase && as.suggestedFtp == 260.0);
        CHECK(assessFtp(255, est, today, std::nullopt).suggestion == Suggestion::Hold);
        CHECK(assessFtp(290, est, today, std::nullopt).suggestion == Suggestion::Decrease);
        auto easy = estimateFtp({cpRide("e", "2026-11-20", 200, 20000, 0.65)}, today);
        CHECK(assessFtp(260, easy, today, std::nullopt).suggestion == Suggestion::Hold);
        CHECK(assessFtp(250, std::nullopt, today, std::nullopt).suggestion == Suggestion::NotEnoughData);
        auto flat = cpRide("flat", "2026-11-20", 250, 0, 0.9);
        flat.powerBests = {{180, 250}, {300, 250}, {600, 250}, {1200, 250}};
        CHECK(estimateFtp({flat}, today)->method == "best_20min");
        json::Value av;
        CHECK(json::parse(json::dump(as.toJson()), av));
        auto as2 = FtpAssessment::fromJson(av);
        CHECK(as2 && as2->suggestion == Suggestion::Increase && as2->estimate && as2->estimate->points.size() == as.estimate->points.size());
    }
    // ---------------- events (CoreKick events.rs tests)
    {
        auto ev = [](Freq f, std::vector<int> wd, int interval) {
            Event e;
            e.id = "e"; e.name = "E";
            e.recurrence.freq = f; e.recurrence.startDate = "2026-11-02"; e.recurrence.time = "07:30"; e.recurrence.weekdays = wd; e.recurrence.interval = interval;
            return e;
        };
        auto o = occurrences(ev(Freq::Weekly, {1, 3}, 1), D("2026-11-01"), D("2026-11-15"));
        CHECK(o.size() == 4 && o[0].d == 3 && o[1].d == 5 && o[2].d == 10 && o[3].d == 12);
        auto f = occurrences(ev(Freq::Weekly, {}, 2), D("2026-11-01"), D("2026-11-30"));
        CHECK(f.size() == 3 && f[0].d == 2 && f[1].d == 16 && f[2].d == 30);
        CHECK(occurrences(ev(Freq::Once, {}, 1), D("2026-10-01"), D("2026-12-01")).size() == 1);
        CHECK(occurrences(ev(Freq::Daily, {}, 1), D("2026-11-02"), D("2026-11-08")).size() == 7);
        CHECK(occurrences(ev(Freq::EveryNDays, {}, 3), D("2026-11-02"), D("2026-11-11")).size() == 4);
        Event e = ev(Freq::Daily, {}, 1);
        e.recurrence.endDate = "2026-11-04";
        CHECK(occurrences(e, D("2026-11-01"), D("2026-11-30")).size() == 3);
        e.active = false;
        CHECK(occurrences(e, D("2026-11-01"), D("2026-11-30")).empty());
        Event w = ev(Freq::Weekly, {1, 3}, 1);
        w.target.type = EventTarget::Workout; w.target.workoutId = "builtin-vo2-5x3";
        json::Value v;
        CHECK(json::parse(json::dump(w.toJson()), v));
        auto w2 = Event::fromJson(v);
        CHECK(w2 && w2->target.type == EventTarget::Workout && w2->target.workoutId == "builtin-vo2-5x3" && w2->recurrence.weekdays.size() == 2);
        // CoreKick serde event json parses
        const char* ck = R"({"id":"x","name":"Tue/Thu","target":{"type":"route","route_id":"r1","laps":2},"recurrence":{"freq":"weekly","start_date":"2026-11-02","time":"07:30","end_date":null,"weekdays":[1,3],"interval":1},"created_at":"","color":"#1DB870","active":true})";
        CHECK(json::parse(ck, v));
        auto e3 = Event::fromJson(v);
        CHECK(e3 && e3->target.laps == 2 && e3->target.routeId == "r1");
    }
    // ---------------- weather (CoreKick weather.rs + weather.ts)
    {
        using namespace weather;
        CHECK(std::string(kindFromWmo(0, 5, 0)) == "clear");
        CHECK(std::string(kindFromWmo(0, 45, 0)) == "windy");
        CHECK(std::string(kindFromWmo(80, 5, 30)) == "sunshower");
        CHECK(std::string(kindFromWmo(80, 5, 90)) == "drizzle");
        CHECK(std::string(kindFromWmo(73, 55, 100)) == "blizzard");
        CHECK(std::string(kindFromWmo(96, 30, 100)) == "hail");
        CHECK(std::string(kindFromWmo(12345, 0, 0)) == "cloudy");
        auto l = parseCurrent(R"({"current":{"temperature_2m":6.4,"weather_code":63,"cloud_cover":100,"precipitation":0.9,"wind_speed_10m":24.1,"is_day":1}})", "Edinburgh");
        CHECK(l && l->kind == Kind::Rain && l->temperatureC == 6.4 && l->isDay);
        auto p = parsePlaces(R"({"results":[{"name":"Edinburgh","latitude":55.95,"longitude":-3.19,"country":"United Kingdom","admin1":"Scotland"}]})");
        CHECK(p.size() == 1 && p[0].name == "Edinburgh, Scotland, United Kingdom");
        CHECK(geocodeUrl("S\xC3\xA3o Paulo").find("name=S%C3%A3o+Paulo") != std::string::npos);
        auto tl = timeline(42, 6);
        CHECK(!tl.empty() && tl[0].start == 0);
        // weather systems: spells of a few minutes to under an hour, the same for the same seed,
        // never opening in a downpour, and never jumping more than two steps of severity
        auto sev = [](Kind k) {
            switch (k) {
                case Kind::Clear: return 0;
                case Kind::Fair: case Kind::Windy: case Kind::Sunshower: return 1;
                case Kind::Cloudy: case Kind::Mist: return 2;
                case Kind::Overcast: case Kind::Fog: case Kind::Drizzle: return 3;
                case Kind::Rain: case Kind::Sleet: case Kind::Snow: return 4;
                default: return 5;
            }
        };
        int jumps = 0, wetSpells = 0, drySpells = 0;
        for (uint32_t seed = 1; seed <= 200; ++seed) {
            auto T = timeline(seed, 6);
            CHECK(sev(T[0].kind) <= 3);
            for (size_t i = 1; i < T.size(); ++i) {
                CHECK(T[i].start - T[i - 1].start >= 200 && T[i].start - T[i - 1].start <= 2.5 * 3600);   // settled spells can last a while
                jumps += std::abs(sev(T[i].kind) - sev(T[i - 1].kind)) > 2;
                (sev(T[i].kind) >= 4 ? wetSpells : drySpells)++;
            }
        }
        CHECK(jumps == 0);
        CHECK(wetSpells > 0 && drySpells > 3 * wetSpells);   // mostly dry, some rain
        auto t2 = timeline(42, 6);
        CHECK(t2.size() == tl.size() && t2.back().kind == tl.back().kind);
        // temperatures: London in late November ~5 C mean, July ~18 C; colder with height
        CHECK(seasonalMean(51.5, 330) > 3 && seasonalMean(51.5, 330) < 8);
        CHECK(seasonalMean(51.5, 196) > 15 && seasonalMean(51.5, 196) < 21);
        CHECK(temperature(10, 30, Kind::Fair, 1000) < temperature(10, 30, Kind::Fair, 0) - 6);
        CHECK(temperature(10, 30, Kind::Snow, 0) <= 0.5);
        // live: the next hours come from Open-Meteo's hourly arrays (index 0 = this hour)
        auto lh = parseCurrent(R"({"current":{"temperature_2m":6.4,"weather_code":3,"cloud_cover":100,"precipitation":0,"wind_speed_10m":10,"is_day":1},
            "hourly":{"weather_code":[3,61,63,0],"temperature_2m":[6,5.5,5,4],"cloud_cover":[100,100,100,10],"wind_speed_10m":[10,12,14,5],"precipitation_probability":[10,60,80,5]}})", "Leeds");
        CHECK(lh && lh->hourly.size() == 3 && lh->hourly[0].kind == Kind::Drizzle && lh->hourly[1].kind == Kind::Rain && lh->hourly[2].tempC == 4);
        CHECK(forecastUrl(51.5, 0).find("hourly=") != std::string::npos);
        Sim s(1);
        s.mode = Mode::Fixed; s.fixed = Kind::Storm;
        bool flashed = false;
        for (int i = 0; i < 4 * 600; ++i) if (s.update(0.25, i * 0.25, 0, true).lightning) flashed = true;
        CHECK(flashed && s.wet > 0.8 && s.state.rain > 0.9);
        s.fixed = Kind::Fair;
        double maxBow = 0;
        for (int i = 0; i < 4 * 200; ++i) { s.update(0.25, i * 0.25, 0, true); maxBow = std::max(maxBow, s.rainbow); }
        CHECK(maxBow > 0.5);
        State a = adjustForAltitude(stateOf(Kind::Rain), 1.0);
        CHECK(a.rain < 0.01 && a.snow > 0.5);
    }
    // ---------------- sky
    {
        // 2026-06-21 12:00 UTC at Greenwich: sun high in the south
        auto sun = sky::sunPosition(1782043200, 51.48, 0);
        CHECK(sun.elevation > 60 && sun.elevation < 63 && std::fabs(sun.azimuth - M_PI) < 0.1);
        // a known full moon: 2026-03-03 ~11:38 UTC
        auto m = sky::moonPhase(1772537880);
        CHECK(m.illumination > 0.97);
        CHECK(sky::todFromElevation(50) < 0.05 && sky::todFromElevation(-20) == 1);
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
