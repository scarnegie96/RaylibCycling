// Ports of CoreKick's core unit tests, run against the C++ implementation.
#include <cmath>
#include <cstdio>
#include <string>

#include "core/Engine.h"
#include "core/Export.h"
#include "core/Metrics.h"
#include "core/Physics.h"
#include "core/Sim.h"
#include "core/Workout.h"

static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)
#define NEAR(a, b, eps) CHECK(std::fabs(double(a) - double(b)) < (eps))

static Route flatRoute(double len = 20000, double ele = 50) {
    Route r;
    r.id = "flat"; r.name = "Flat";
    r.points = {{0, ele}, {len, ele}};
    return r;
}
static Route hillRoute() {
    Route r;
    r.id = "hill"; r.name = "Hill";
    for (int i = 0; i <= 500; ++i) {
        double d = i * 10;
        r.points.push_back({d, d < 1000 ? 100 : d < 3000 ? 100 + (d - 1000) * 0.06 : 220});
    }
    return r;
}
static RideSetup withRoute(RideSetup s, const Route& r) { s.route = r; return s; }

int main() {
    using namespace phys;
    // ---------------- physics
    {
        Params p; p.massKg = 80;
        double v = steadySpeed(250, 0, 0, p) * 3.6;
        CHECK(v > 35 && v < 39.5);
        double c = steadySpeed(250, 0.08, 0, p) * 3.6;
        CHECK(c > 10 && c < 15);
        Params d;
        for (double g : {-0.03, 0.0, 0.02, 0.07}) {
            double vv = steadySpeed(220, g, 500, d);
            NEAR(powerForSpeed(vv, g, 500, d), 220, 0.5);
        }
        double vi = 0;
        for (int i = 0; i < 600; ++i) vi = integrateSpeed(vi, 200, 0.01, 0, d, 0.25);
        NEAR(vi, steadySpeed(200, 0.01, 0, d), 0.05);
        double vc = 5;
        for (int i = 0; i < 200; ++i) { vc = integrateSpeed(vc, 0, 0.06, 0, d, 0.25); CHECK(vc >= 0); }
        CHECK(vc == 0);
        double s = steadySpeed(0, -0.06, 0, d) * 3.6;
        CHECK(s > 45 && s < 80);
        // steep climbs from a standstill: a heavy rider at modest power must still move
        // (regression: 110 kg + bike, 230 W, 13.6 % used to stall at 0 km/h)
        for (double mass : {83.0, 119.0, 140.0})
            for (double gr : {0.10, 0.136, 0.20}) {
                Params h; h.massKg = mass;
                double vs = steadySpeed(230, gr, 260, h), vh = 0;
                for (int i = 0; i < 400; ++i) vh = integrateSpeed(vh, 230, gr, 260, h, 1.0 / 30);
                CHECK(vs > 0.5);
                NEAR(vh, vs, 0.05);
            }
        // virtual gears in SIM mode: the trainer, turning at v in the real gear, must cost the
        // same power as the virtual bike at k*v on the real road - for any gear, grade, speed
        {
            Params q; q.massKg = 83;
            for (double k : {0.4, 1.0, 1.6, 2.2})
                for (double gr : {-0.03, 0.0, 0.05, 0.1})
                    for (double v : {3.0, 7.0, 10.0}) {
                        double s2 = gearSimGrade(gr, k, v, q);
                        double trainerP = (q.massKg * G * (s2 + q.crr) + q.cw() * v * v) * v;
                        double virtualP = (q.massKg * G * (gr + q.crr) + q.cw() * (k * v) * (k * v)) * (k * v);
                        NEAR(trainerP, virtualP, 0.5);
                    }
            NEAR(gearSimGrade(0.04, 1.0, 8, q), 0.04, 1e-9);                 // same gear: same road
            CHECK(gearSimGrade(0.0, 2.0, 8, q) > gearSimGrade(0.0, 1.2, 8, q));   // taller gear: harder
            CHECK(gearSimGrade(0.08, 0.5, 3, q) < 0.08);                      // climbing gear: easier
        }
        {
            Profile vp; vp.virtualShifting = true; vp.weightKg = 75;
            RideSetup vs; vs.kind = RideKind::Free;
            RideEngine ve(vp, withRoute(vs, flatRoute()));
            std::optional<TrainerCommand> lastV;
            bool anyErg = false;
            auto ride = [&](int n) { for (int i = 0; i < n; ++i) if (auto c = ve.tick(0.25, {220, 90, {}, true, 30.0})) { lastV = c; anyErg |= c->type == TrainerCommand::TargetPower; } };
            ride(40);
            CHECK(lastV && lastV->type == TrainerCommand::Simulation);
            double g0 = lastV->gradePct;
            for (int i = 0; i < 6; ++i) ve.shift(1);
            ride(8);
            CHECK(lastV->gradePct > g0 + 0.5);    // six gears up: clearly harder
            for (int i = 0; i < 12; ++i) ve.shift(-1);
            ride(8);
            CHECK(lastV->gradePct < g0);          // down past the start: easier
            CHECK(!anyErg);                       // never a power target (that was the ERG feedback loop)
            CHECK(ve.realGearRatio() > 1.0 && ve.realGearRatio() < 5.0);
        }
        Params g; g.massKg = 83;
        double w = gearTargetPower(90, gearIndexFromGear(14), 2.105, 0, 0, g);
        CHECK(w > 150 && w < 230);
    }
    // ---------------- metrics
    {
        std::vector<double> p(600, 250);
        NEAR(metrics::normalizedPower(p), 250, 1e-9);
        std::vector<double> v;
        for (int i = 0; i < 1200; ++i) v.push_back((i / 60) % 2 == 0 ? 350 : 100);
        CHECK(metrics::normalizedPower(v) > metrics::mean(v) + 20);
        NEAR(metrics::tss(3600, 250, 250), 100, 1e-9);
        std::vector<double> b(600, 100);
        for (int i = 100; i < 160; ++i) b[i] = 400;
        CHECK(*metrics::bestAverage(b, 60) == 400);
        CHECK(!metrics::bestAverage(b, 601));
        CHECK(metrics::zoneOf(100, 250) == 0 && metrics::zoneOf(250, 250) == 3 && metrics::zoneOf(500, 250) == 6);
        NEAR(metrics::caloriesFromWork(1000, 0.24), 995.9, 1.0);
    }
    // ---------------- workouts
    {
        auto lib = wk::builtinWorkouts();
        CHECK(lib.size() >= 30);
        for (auto& w : lib) {
            auto s = w.steps();
            CHECK(!s.empty());
            for (size_t i = 1; i < s.size(); ++i) CHECK(s[i - 1].endS() == s[i].startS);
        }
        auto find = [&](const char* id) { for (auto& w : lib) if (w.id == id) return w; return wk::Workout{}; };
        CHECK(find("builtin-recovery-45").durationS() == 45 * 60);
        CHECK(find("builtin-endurance-60").durationS() == 3600);
        CHECK(find("builtin-endurance-90").durationS() == 5400);
        CHECK(find("builtin-long-ride-2h").durationS() == 7200);
        auto ss = find("builtin-sweet-spot-2x20");
        CHECK(ss.durationS() == 600 + 2 * 1200 + 2 * 300 + 300);
        auto e = ss.estimate(250);
        CHECK(e.tss > 55 && e.tss < 85 && e.intensityFactor > 0.7 && e.intensityFactor < 0.9);
        auto st = wk::blocksToSteps({wk::Block::ramp(100, 0.5, 1.0), wk::Block::steady(50, 1.2)});
        auto at = wk::stepAt(st, 50);
        CHECK(at && at->first == 0);
        NEAR(*st[0].fractionAt(at->second), 0.75, 1e-9);
        CHECK(wk::stepAt(st, 100)->first == 1);
        CHECK(!wk::stepAt(st, 150));
        auto s2 = wk::blocksToSteps({wk::Block::set(2, {wk::Block::intervals(2, 30, 1.3, 30, 0.5), wk::Block::steady(60, 0.5)})});
        CHECK(s2.size() == 10);
        CHECK(s2[0].label == "Set 1/2 \xC2\xB7 Interval 1/2");
        CHECK(s2[9].label == "Set 2/2 \xC2\xB7 Recovery");

        std::vector<wk::Block> b;
        std::string err;
        CHECK(wk::parseText("10m 50-75%, 3x(8m @95%, 4m @55%)\n5x 1m @120% 100rpm / 1m @50%\n2m free, 30s effort @2%\n1:30 @200w", 200, b, err));
        CHECK(b.size() == 6);
        CHECK(b[0].type == wk::Block::Ramp && b[0].durationS == 600 && std::fabs(b[0].from - 0.5) < 1e-9);
        CHECK(b[1].type == wk::Block::Set && b[1].repeats == 3 && b[1].blocks.size() == 2);
        CHECK(b[2].type == wk::Block::Intervals && b[2].repeats == 5 && b[2].cadence == 100);
        CHECK(b[3].type == wk::Block::Free && !b[3].effort && b[3].durationS == 120);
        CHECK(b[4].type == wk::Block::Free && b[4].effort && b[4].gradePct == 2.0);
        CHECK(b[5].type == wk::Block::Steady && b[5].durationS == 90 && std::fabs(b[5].power - 1.0) < 1e-9);
        CHECK(!wk::parseText("10 @50%", 200, b, err) && err.find("unit") != std::string::npos);
        CHECK(!wk::parseText("3x(5m @90%", 200, b, err) && err.find(")") != std::string::npos);
        CHECK(!wk::parseText("5m banana", 200, b, err));
        CHECK(!wk::parseText("", 200, b, err));
        for (auto& w : lib) {
            std::string t = wk::toText(w.blocks);
            std::vector<wk::Block> back;
            bool ok = wk::parseText(t, 250, back, err);
            if (!ok) std::printf("  roundtrip parse failed for %s: %s\n%s\n", w.name.c_str(), err.c_str(), t.c_str());
            CHECK(ok);
            auto a = wk::estimateSteps(wk::blocksToSteps(back), 250), c = w.estimate(250);
            CHECK(a.durationS == c.durationS);
            CHECK(std::fabs(a.tss - c.tss) < 0.6);
        }
        wk::Workout z;
        auto ou = find("builtin-over-unders");
        CHECK(wk::fromZwo(wk::toZwo(ou), z, err) && z.name == ou.name && z.durationS() == ou.durationS());
        const char* zs = R"(<workout_file><name>Test</name><workout>
            <Warmup Duration="600" PowerLow="0.25" PowerHigh="0.75"/>
            <IntervalsT Repeat="5" OnDuration="60" OffDuration="60" OnPower="1.2" OffPower="0.5" Cadence="100"/>
            <FreeRide Duration="300" FlatRoad="1"/>
            <MaxEffort Duration="20"/>
            <Cooldown Duration="300" PowerLow="0.6" PowerHigh="0.3"/>
          </workout></workout_file>)";
        CHECK(wk::fromZwo(zs, z, err) && z.durationS() == 600 + 600 + 300 + 20 + 300 && z.blocks.size() == 5);
        const char* erg = "[COURSE HEADER]\nFTP = 250\nMINUTES WATTS\n[END COURSE HEADER]\n[COURSE DATA]\n0 125\n10 200\n10 250\n20 250\n20 125\n25 125\n[END COURSE DATA]\n";
        CHECK(wk::fromErgMrc(erg, 200, z, err) && z.durationS() == 1500);
        CHECK(z.blocks[0].type == wk::Block::Ramp && std::fabs(z.blocks[0].from - 0.5) < 1e-9 && std::fabs(z.blocks[0].to - 0.8) < 1e-9);
        CHECK(z.blocks[1].type == wk::Block::Steady && z.blocks[1].durationS == 600 && std::fabs(z.blocks[1].power - 1.0) < 1e-9);
        const char* mrc = "[COURSE HEADER]\nMINUTES PERCENT\n[END COURSE HEADER]\n[COURSE DATA]\n0 50\n5 50\n5 100\n8 100\n[END COURSE DATA]";
        CHECK(wk::fromErgMrc(mrc, 200, z, err) && z.blocks.size() == 2);
        CHECK(wk::importFile("plan.txt", "10m @60%", 200, z, err));
    }
    // ---------------- tests
    {
        auto w = wk::testProtocol(wk::TestKind::Ramp, 250);
        auto s = w.steps();
        CHECK(s.front().label == "Warm-up");
        int ramps = 0;
        for (auto& x : s) if (x.label.rfind("Ramp", 0) == 0) ++ramps;
        CHECK(ramps == 30);
        CHECK(wk::rampParams(250) == std::make_pair(110.0, 15.0));
        std::vector<double> t, p;
        for (int sec = 0; sec < 300 + 15 * 60; ++sec) {
            auto a = wk::stepAt(s, sec);
            t.push_back(sec);
            p.push_back(*s[a->first].fractionAt(a->second) * 250);
        }
        auto r = wk::evaluateTest(wk::TestKind::Ramp, s, t, p);
        CHECK(r && r->measuredW == 320 && r->ftp == 240 && r->valid);
        auto s20 = wk::testProtocol(wk::TestKind::Ftp20, 250).steps();
        const wk::Step* eff = nullptr;
        for (auto& x : s20) if (x.effort && !x.erg) { eff = &x; break; }
        std::vector<double> t2, p2;
        for (int i = 0; i < s20.back().endS(); ++i) { t2.push_back(i); p2.push_back(i >= eff->startS && i < eff->endS() ? 280 : 150); }
        auto r2 = wk::evaluateTest(wk::TestKind::Ftp20, s20, t2, p2);
        CHECK(r2 && r2->ftp == 266 && r2->valid);
    }
    // ---------------- sim
    {
        Profile pr; pr.ftp = 250;
        sim::SimTrainer s(pr);
        s.command(TrainerCommand::erg(200));
        TrainerInput in;
        for (int i = 0; i < 80; ++i) in = s.update(0.25, pr);
        CHECK(std::fabs(in.powerW - 200) < 25 && in.cadenceRpm > 80 && in.heartRate);
        s.command(TrainerCommand::erg(600));
        for (int i = 0; i < 80; ++i) in = s.update(0.25, pr);
        CHECK(in.powerW < 300 && in.cadenceRpm < 50);
        sim::Settings fx; fx.enabled = true; fx.powerSource = sim::PowerSource::Fixed; fx.powerW = 333; fx.variability = 0;
        s.set(fx);
        for (int i = 0; i < 40; ++i) in = s.update(0.25, pr);
        CHECK(in.powerW == 333);
        fx.dropout = true;
        s.set(fx);
        in = s.update(0.25, pr);
        CHECK(!in.connected && in.powerW == 0);
        CHECK(sim::patternFactor(sim::Pattern::Surges, 35) == 1.6);
        NEAR(sim::patternFactor(sim::Pattern::Fade, 600), 0.9, 1e-9);
    }
    // ---------------- engine
    {
        Profile pr; pr.ftp = 250;
        // route ride: hill, 1 lap, sends SIM grade, completes
        RideSetup su; su.kind = RideKind::Route; su.laps = 1;
        RideEngine e(pr, withRoute(su, hillRoute()));
        CHECK(!e.profileData().climbs.empty());
        int sims = 0; double maxGrade = 0;
        for (int i = 0; i < 4 * 60 * 30 && !e.routeComplete(); ++i) {
            auto c = e.tick(0.25, {250, 90, 140.0, true});
            if (c && c->type == TrainerCommand::Simulation) { ++sims; maxGrade = std::max(maxGrade, c->gradePct); }
        }
        CHECK(e.routeComplete());
        CHECK(sims > 5 && maxGrade > 4.0 && maxGrade < 6.5);
        CHECK(e.gain() > 100 && e.gain() < 140);
        CHECK(std::fabs(e.workKj() - 250 * e.elapsedS() / 1000) < 2);
        auto ev = e.drainEvents();
        bool climbAnn = false;
        for (auto& x : ev) if (x.kind == "climb") climbAnn = true;
        CHECK(climbAnn);

        // workout drives ERG
        RideSetup ws; ws.kind = RideKind::Workout;
        wk::Workout w; w.name = "t"; w.blocks = {wk::Block::steady(60, 0.8), wk::Block::steady(60, 1.2)};
        ws.workout = w;
        RideEngine we(pr, withRoute(ws, flatRoute()));
        std::optional<TrainerCommand> last;
        for (int i = 0; i < 4 * 30; ++i) if (auto c = we.tick(0.25, {200, 90, {}, true})) last = c;
        CHECK(last && last->type == TrainerCommand::TargetPower && last->watts == 200);
        for (int i = 0; i < 4 * 60; ++i) if (auto c = we.tick(0.25, {300, 90, {}, true})) last = c;
        CHECK(last->watts == 300);
        we.ergBias(10);
        for (int i = 0; i < 8; ++i) if (auto c = we.tick(0.25, {300, 90, {}, true})) last = c;
        CHECK(last->watts == 330);

        // ramp test failure detection
        RideSetup rs; rs.kind = RideKind::Test; rs.test = wk::TestKind::Ramp;
        RideEngine re(pr, withRoute(rs, flatRoute(200000)));
        for (int i = 0; i < 4 * 60 * 40 && !re.testDone(); ++i) {
            double tgt = re.targetW().value_or(100);
            double p = re.elapsedS() < 300 + 12 * 60 ? tgt : 80;
            re.tick(0.25, {p, p > 100 ? 90.0 : 40.0, {}, true});
        }
        CHECK(re.testDone());
        auto rr = re.liveTestResult();
        CHECK(rr && rr->ftp > 200 && rr->ftp < 260);

        // pause freezes time/energy
        RideSetup fs; fs.kind = RideKind::Free;
        RideEngine fe(pr, withRoute(fs, flatRoute()));
        for (int i = 0; i < 40; ++i) fe.tick(0.25, {200, 90, {}, true});
        double t0 = fe.elapsedS(), k0 = fe.workKj();
        fe.pause();
        auto pc = fe.tick(0.25, {200, 90, {}, true});
        for (int i = 0; i < 40; ++i) fe.tick(0.25, {200, 90, {}, true});
        CHECK(fe.elapsedS() == t0 && fe.workKj() == k0);
        CHECK(pc && pc->type == TrainerCommand::Simulation && pc->gradePct == 0);
        fe.resume();

        // free modes
        fe.setFreeMode(FreeMode::Power);
        std::optional<TrainerCommand> fc;
        for (int i = 0; i < 8; ++i) if (auto c = fe.tick(0.25, {200, 90, {}, true})) fc = c;
        CHECK(fc && fc->type == TrainerCommand::TargetPower);
        fe.setFreeMode(FreeMode::Level);
        for (int i = 0; i < 8; ++i) if (auto c = fe.tick(0.25, {200, 90, {}, true})) fc = c;
        CHECK(fc && fc->type == TrainerCommand::Resistance);
        fe.setFreeMode(FreeMode::HeartRate);
        double w0 = fe.hrControlW();
        for (int i = 0; i < 4 * 30; ++i) fe.tick(0.25, {200, 90, 100.0, true});  // HR well below target -> raise watts
        CHECK(fe.hrControlW() > w0);

        // goals + laps
        Goal g; g.type = Goal::Time; g.value = 30;
        fe.setGoal(g);
        for (int i = 0; i < 4 * 31; ++i) fe.tick(0.25, {200, 90, {}, true});
        CHECK(fe.goalDone() || fe.elapsedS() >= 30);
        fe.lap();
        for (int i = 0; i < 4 * 20; ++i) fe.tick(0.25, {200, 90, {}, true});
        fe.lap();
        int laps = 0;
        for (auto& s : fe.splits()) if (s.kind == "lap") ++laps;
        CHECK(laps == 2);

        // workout editing
        RideEngine ed(pr, withRoute(ws, flatRoute()));
        for (int i = 0; i < 40; ++i) ed.tick(0.25, {200, 90, {}, true});
        int before = ed.steps()[0].durationS;
        ed.extendStep(60);
        CHECK(ed.steps()[0].durationS == before + 60 && ed.steps()[1].startS == ed.steps()[0].endS());
        ed.skipToCooldown();
        CHECK(ed.steps().back().label == "Cool-down");

        // virtual shifting
        Profile vs = pr; vs.virtualShifting = true;
        RideEngine sh(vs, withRoute(fs, flatRoute()));
        std::optional<TrainerCommand> sc;
        for (int i = 0; i < 20; ++i) if (auto c = sh.tick(0.25, {200, 90, {}, true})) sc = c;
        // gears stay in SIM mode (the trainer's own physics), never an ERG target
        CHECK(sc && sc->type == TrainerCommand::Simulation);
        double before2 = sc->gradePct;
        sh.shift(3);
        for (int i = 0; i < 20; ++i) if (auto c = sh.tick(0.25, {200, 90, {}, true})) sc = c;
        CHECK(sc->type == TrainerCommand::Simulation && sc->gradePct > before2);

        // summary + FIT export
        auto rec = fe.finish("2026-11-20T18:00:00+00:00", "ride-1");
        auto& sum = rec.summary;
        CHECK(sum.elapsedS > 0 && sum.np > 0);
        auto fit = exporter::toFit(sum, rec.samples, 1764000000, 0);
        std::string err;
        size_t recs = 0;
        CHECK(exporter::validateFit(fit, err, &recs));
        CHECK(recs == rec.samples.size());
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
