// Ride screen: 3D HUD, 2D dashboard, workout step panel, edit drawer, pause menu, help.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "App.h"
#include "raylib.h"

using namespace ui;

static std::string fmt(const char* f, double v) { char b[64]; std::snprintf(b, sizeof b, f, v); return b; }

static void metricTile(Rectangle r, const char* label, const std::string& value, const char* unit, Color accent, float size = 60,
                       const std::string& badge = "", Color badgeC = WHITE) {
    panel(r);
    DrawRectangleRounded({r.x + 9, r.y + 12, 3, r.height - 24}, 1, 4, accent);
    text(label, r.x + 20, r.y + 9, F::Small, DIM, 13);
    Vector2 m = measure(value, F::Big, size);
    text(value, r.x + 18, r.y + r.height - m.y - 2, F::Big, TEXT, size);
    if (unit && *unit) text(unit, r.x + 24 + m.x, r.y + r.height - 28, F::Small, DIM, 15);
    if (!badge.empty()) {
        Rectangle zr{r.x + r.width - 46, r.y + 9, 36, 20};
        DrawRectangleRounded(zr, 0.5f, 6, badgeC);
        textCentered(badge, zr, F::Small, BLACK, 13);
    }
}

static std::string modeLabel(const RideEngine& e, char* b, size_t n) {
    switch (e.mode()) {
        case ControlMode::Erg:
            if (e.setup().kind == RideKind::Free && e.freeMode() == FreeMode::HeartRate && !e.structuredActive())
                std::snprintf(b, n, "HR %.0f bpm \xE2\x86\x92 %.0f W", e.hrTargetValue(), e.targetW().value_or(0));
            else std::snprintf(b, n, "ERG %.0f W", e.targetW().value_or(0));
            break;
        case ControlMode::Level: std::snprintf(b, n, "LEVEL %.0f%%", e.freeLevel()); break;
        default:
            if (e.shiftingActive()) std::snprintf(b, n, "GEAR %d/24  SIM %.1f%%", e.gear(), e.trainerGradePct());
            else std::snprintf(b, n, "SIM %.1f%%", e.trainerGradePct());
    }
    return b;
}

void App::drawStepPanel(Rectangle br) {
    const RideEngine& e = *eng_;
    char b[200];
    panel(br);
    auto cs = e.currentStep();
    if (!cs) {
        textCentered(e.workoutComplete() ? "Workout complete" : "Get ready", br, F::Title, GOOD, 28);
        return;
    }
    const auto& st = e.steps()[cs->first];
    double rem = st.durationS - cs->second;
    text(st.label, br.x + 18, br.y + 10, F::Body, TEXT, 19);
    textRight(fmtTime(rem), br.x + br.width - 18, br.y + 4, F::Title, rem <= 3.5 ? WARN : TEXT, 34);
    if (st.erg) {
        double tgt = e.targetW().value_or(0);
        text(e.ergDisabled() ? "TARGET (resistance mode)" : "TARGET", br.x + 18, br.y + 42, F::Small, DIM, 12);
        text(fmt("%.0f W", tgt), br.x + 18, br.y + 56, F::Title, powerZoneColor(tgt, prof_.ftp), 32);
        double diff = e.power3s() - tgt;
        Color cc = std::fabs(diff) < tgt * 0.05 ? GOOD : std::fabs(diff) < tgt * 0.12 ? WARN : BAD;
        progressBar({br.x + 18, br.y + br.height - 9, br.width - 36, 4}, float(std::clamp(e.power3s() / std::max(1.0, tgt * 1.5), 0.0, 1.0)), cc);
    } else {
        text(st.effort ? "ALL-OUT EFFORT - YOU CONTROL POWER" : "RIDE BY FEEL", br.x + 18, br.y + 46, F::Small, st.effort ? ACCENT : DIM, 13);
        if (st.effort && e.setup().test) {
            double avg = e.splitAvgPower();
            (void)avg;
        }
    }
    if (st.cadence) {
        Color cc = std::fabs(e.cadence() - *st.cadence) < 5 ? GOOD : WARN;
        text(fmt("%.0f rpm target", *st.cadence), br.x + 140, br.y + 64, F::Small, cc, 15);
    }
    if (cs->first + 1 < e.steps().size()) {
        const auto& nx = e.steps()[cs->first + 1];
        if (nx.erg) std::snprintf(b, sizeof b, "Next: %s  %.0f W  %s", nx.label.c_str(), nx.from * prof_.ftp * e.ergBiasValue(), fmtTime(nx.durationS).c_str());
        else std::snprintf(b, sizeof b, "Next: %s  %s", nx.label.c_str(), fmtTime(nx.durationS).c_str());
        textRight(b, br.x + br.width - 18, br.y + 66, F::Small, DIM, 14);
    }
    if (std::fabs(e.ergBiasValue() - 1) > 0.001) textRight(fmt("Intensity %.0f%%", e.ergBiasValue() * 100), br.x + br.width - 18, br.y + 46, F::Small, WARN, 13);
}

void App::drawRideHud() {
    const RideEngine& e = *eng_;
    const float W = float(GetScreenWidth()), H = float(GetScreenHeight());
    const float pad = 16;
    const bool metric = prof_.metric;
    char b[200];

    // ---- left column
    float tw = 226, y = pad;
    double p3 = e.power3s();
    int pz = 0;
    {
        double f = p3 / std::max(1.0, prof_.ftp);
        static const double bd[6] = {0.55, 0.75, 0.90, 1.05, 1.20, 1.50};
        while (pz < 6 && f > bd[pz]) ++pz;
    }
    metricTile({pad, y, tw, 112}, "POWER  3s", std::to_string(int(std::lround(p3))), "W", powerZoneColor(p3, prof_.ftp), 72,
               "Z" + std::to_string(pz + 1), powerZoneColor(p3, prof_.ftp));
    textRight(fmt("%.2f W/kg", p3 / prof_.weightKg), pad + tw - 54, y + 11, F::Small, DIM, 13);
    y += 122;
    if (e.heartRate()) {
        int z = e.hrZone();
        int nz = int(prof_.hrZones().size());
        metricTile({pad, y, tw, 96}, "HEART RATE", std::to_string(int(std::lround(*e.heartRate()))), "bpm", hrZoneColor(z, nz), 58, "Z" + std::to_string(z),
                   hrZoneColor(z, nz));
        float beat = float(std::pow(std::max(0.0, std::sin(animTime_ * *e.heartRate() / 60.0 * PI)), 8.0));
        DrawCircle(int(pad + tw - 64), int(y + 19), 4 + beat * 3, HR_RED);
        if (hrAlertsOn_) text("ALERTS", pad + tw - 104, y + 70, F::Small, FAINT, 11);
    } else metricTile({pad, y, tw, 96}, "HEART RATE", "--", "bpm", FAINT, 58);
    y += 106;
    metricTile({pad, y, tw, 96}, "CADENCE", std::to_string(int(std::lround(e.cadence()))), "rpm", Color{200, 200, 220, 255}, 58);
    y += 106;
    metricTile({pad, y, tw, 96}, "SPEED", fmtSpeed(e.speedMps(), metric), metric ? "km/h" : "mph", SKY, 58);
    y += 106;
    // mode chip
    {
        std::string ml = modeLabel(e, b, sizeof b);
        Rectangle mr{pad, y, tw, 34};
        panel(mr);
        textCentered(ml, mr, F::Body, e.mode() == ControlMode::Erg ? ACCENT : TEXT, 18);
    }

    // ---- right column
    float rx = W - pad - tw;
    y = pad + 40;
    auto small = [&](const char* label, const std::string& v, Color c = TEXT) {
        panel({rx, y, tw, 54});
        text(label, rx + 14, y + 7, F::Small, DIM, 12);
        textRight(v, rx + tw - 14, y + 16, F::Title, c, 30);
        y += 60;
    };
    small(e.paused() ? "TIME (PAUSED)" : "TIME", fmtTime(e.elapsedS()), e.paused() ? WARN : TEXT);
    if (!e.endless()) small("REMAINING", fmtDist(std::max(0.0, e.profileData().length * e.laps() - e.distance()), metric));
    else small("DISTANCE", fmtDist(e.distance(), metric));
    small("GRADIENT", fmt("%+.1f%%", e.gradePct()), gradeColor(e.gradePct() / 100));
    small("ELEVATION GAIN", fmt(metric ? "%.0f m" : "%.0f ft", metric ? e.gain() : e.gain() * 3.28084));
    std::snprintf(b, sizeof b, "%.0f / %.0f W", e.avgPower(), e.np());
    small("AVG / NP", b);
    std::snprintf(b, sizeof b, "%.0f kJ  %.0f", e.workKj(), e.tss());
    small("WORK  /  TSS", b);
    if (auto gap = e.ghostGapS()) {
        std::snprintf(b, sizeof b, "%s%s", *gap >= 0 ? "+" : "-", fmtTime(std::fabs(*gap)).c_str());
        small("GHOST (PB)", b, *gap >= 0 ? GOOD : BAD);
    }
    if (e.setup().kind == RideKind::Route) small("LAP", std::to_string(std::min(e.lapNumber(), e.laps())) + " / " + std::to_string(e.laps()));

    // ---- device chips
    {
        float cx = W - pad;
        auto chipFor = [&](const Link& l, const char* none, bool trainer) {
            Color c = l.state == LinkState::Connected ? GOOD : l.state == LinkState::Disconnected ? FAINT : WARN;
            std::string label = l.state == LinkState::Disconnected || l.name.empty() ? none : l.name;
            if (trainer && simActive()) { label = "Simulator"; c = SKY; }
            float wdt;
            chip(cx - measure(label, F::Small).x - 34, pad, label, c, &wdt);
            cx -= wdt + 8;
        };
        chipFor(ble_.hrm(), "No HRM", false);
        chipFor(ble_.trainer(), "No trainer", true);
    }

    // ---- top centre: step panel / ride banner + goal + climb
    float bw = std::min(600.f, W - 2 * tw - 4 * pad);
    float cy = pad;
    if (e.structured()) {
        drawStepPanel({(W - bw) / 2, cy, bw, 92});
        cy += 100;
    } else {
        Rectangle br{(W - bw) / 2, cy, bw, 58};
        panel(br);
        text(e.setup().name == course_->name() ? e.setup().name : e.setup().name + "  \xC2\xB7  " + course_->name(), br.x + 18, br.y + 8, F::Body, TEXT, 19);
        if (e.setup().kind == RideKind::Free) {
            static const char* fm[] = {"Gradient %+.1f%%", "Power %.0f W", "Level %.0f%%", "HR target %.0f bpm"};
            double v = e.freeMode() == FreeMode::Gradient ? e.freeGrade() : e.freeMode() == FreeMode::Power ? e.freeWattsValue()
                     : e.freeMode() == FreeMode::Level ? e.freeLevel() : e.hrTargetValue();
            std::snprintf(b, sizeof b, fm[int(e.freeMode())], v);
            text(std::string(b) + "   (Up / Down)", br.x + 18, br.y + 34, F::Small, DIM, 14);
        } else {
            std::snprintf(b, sizeof b, "Difficulty %.0f%%", e.difficultyValue() * 100);
            text(b, br.x + 18, br.y + 34, F::Small, DIM, 14);
        }
        cy += 66;
    }
    if (e.goal().type != Goal::None) {
        Rectangle gr{(W - bw) / 2, cy, bw, 36};
        panel(gr);
        text("Goal " + goalLabel(e.goal(), metric), gr.x + 14, gr.y + 9, F::Small, TEXT, 14);
        textRight(e.goalDone() ? "Done!" : e.goalRemaining(), gr.x + gr.width - 14, gr.y + 9, F::Small, e.goalDone() ? GOOD : DIM, 14);
        progressBar({gr.x + 200, gr.y + 15, gr.width - 360, 6}, float(e.goalProgress()), GOOD);
        cy += 44;
    }
    {
        int ci = e.currentClimb();
        int ni = e.nextClimb();
        if (ci >= 0 || ni >= 0) {
            const Climb& c = e.profileData().climbs[ci >= 0 ? ci : ni];
            Rectangle cr{(W - bw) / 2, cy, bw, 36};
            panel(cr);
            double ld = e.lapDistance();
            if (ci >= 0) {
                double prog = (ld - c.startD) / c.lengthM;
                std::snprintf(b, sizeof b, "%s  \xC2\xB7  %s to the top  \xC2\xB7  %.0f m to climb", c.category == "Uncategorised" ? "Climb" : c.category.c_str(),
                              fmtDist(c.endD - ld, metric).c_str(), std::max(0.0, c.topEle - e.ele()));
                text(b, cr.x + 14, cr.y + 9, F::Small, WARN, 14);
                progressBar({cr.x + cr.width - 150, cr.y + 15, 136, 6}, float(prog), WARN);
            } else {
                double to = c.startD - ld;
                if (to < 0) to += e.profileData().length;
                std::snprintf(b, sizeof b, "Next: %s in %s  \xC2\xB7  %.1f km at %.1f%%", c.category == "Uncategorised" ? "climb" : c.category.c_str(),
                              fmtDist(to, metric).c_str(), c.lengthM / 1000, c.avgGradePct);
                text(b, cr.x + 14, cr.y + 9, F::Small, DIM, 14);
            }
        }
    }

    if (e.currentClimb() >= 0 || e.nextClimb() >= 0) cy += 44;
    drawSectorBanner((W - bw) / 2, cy, bw);
    // weather / time chip
    {
        std::string wl = std::string(weather::kindLabel(wx_.currentKind(wxElapsed_)));
        if (wx_.wet > 0.4 && wx_.snowCover < 0.3) wl += "  \xC2\xB7  wet road";
        if (wx_.snowCover > 0.3) wl += "  \xC2\xB7  snow";
        if (sky_.tod > 0.8f) wl += "  \xC2\xB7  night";
        Vector2 m = measure(wl, F::Small, 13);
        text(wl, W - pad - m.x, H - std::min(220.f, H * 0.26f) - pad - 22, F::Small, Color{255, 255, 255, 140}, 13);
    }
    // ---- bottom panel
    float ph = std::min(220.f, H * 0.26f);
    Rectangle bp{pad, H - ph - pad, W - 2 * pad, ph};
    panel(bp, Color{10, 12, 18, 195});
    float split = bp.width * 0.56f;
    Rectangle chart{bp.x + 16, bp.y + 30, split - 28, bp.height - 44};
    if (e.structured()) {
        text("WORKOUT", bp.x + 16, bp.y + 8, F::Small, DIM, 13);
        workoutChart(chart, e.steps(), prof_.ftp, e.ergBiasValue(), e.workoutT(), &e.samples(), &e.workoutTSamples());
    } else {
        text("POWER  \xC2\xB7  HEART RATE  \xC2\xB7  CADENCE   (last 5 min)", bp.x + 16, bp.y + 8, F::Small, DIM, 13);
        powerChart(chart, e.samples(), prof_.ftp, prof_.maxHr, 300);
    }
    DrawLineEx({bp.x + split, bp.y + 12}, {bp.x + split, bp.y + bp.height - 12}, 1, LINE);
    float px = bp.x + split + 16, pw = bp.width - split - 32;
    text("NEXT 2 KM", px, bp.y + 8, F::Small, DIM, 13);
    upcomingProfile({px, bp.y + 28, pw, bp.height * 0.40f}, e.profileData(), e.lapDistance(), 2000, metric);
    float ry = bp.y + 36 + bp.height * 0.40f;
    text("ROUTE", px, ry, F::Small, DIM, 13);
    textRight(fmtDist(e.lapDistance(), metric) + " / " + fmtDist(e.profileData().length, metric), px + pw, ry, F::Small, DIM, 13);
    routeProfile({px, ry + 18, pw, bp.y + bp.height - ry - 30}, e.profileData(), e.lapDistance(),
                 e.ghostDistance() ? std::fmod(*e.ghostDistance(), e.profileData().length) : -1, metric);

    if (!e.started()) {
        Rectangle r{W / 2 - 240, H * 0.38f, 480, 64};
        panel(r, Color{10, 12, 18, 225});
        bool ok = ble_.trainer().state == LinkState::Connected || simActive();
        textCentered(ok ? "Start pedalling to begin" : "No trainer - Esc for the menu", r, F::Title, TEXT, 28);
    }
    if (e.paused() && !pauseMenu_) {
        Rectangle r{W / 2 - 160, H * 0.38f, 320, 60};
        panel(r, Color{10, 12, 18, 225});
        textCentered("PAUSED  -  Space to resume", r, F::Body, WARN, 20);
    }
    text("Space pause  L lap  E edit  R road ahead  V 2D  C camera  W weather  T time  ? help", pad, H - ph - pad - 22, F::Small, Color{255, 255, 255, 110}, 13);
}

// ------------------------------------------------------------------ 2D dashboard (CoreKick style)
void App::drawDashboard2d() {
    const RideEngine& e = *eng_;
    const float W = float(GetScreenWidth()), H = float(GetScreenHeight());
    const bool metric = prof_.metric;
    char b[200];
    DrawRectangleGradientV(0, 0, int(W), int(H), Color{12, 15, 22, 255}, Color{6, 8, 12, 255});
    float pad = 20;
    // header
    text(e.setup().name, pad, 14, F::Title, TEXT, 28);
    std::string ml = modeLabel(e, b, sizeof b);
    textRight(ml, W - pad, 20, F::Body, e.mode() == ControlMode::Erg ? ACCENT : TEXT);
    // metric row
    struct M { const char* l; std::string v; const char* u; Color c; };
    double p3 = e.power3s();
    std::vector<M> ms = {
        {"POWER 3s", std::to_string(int(std::lround(p3))), "W", powerZoneColor(p3, prof_.ftp)},
        {"HEART RATE", e.heartRate() ? std::to_string(int(*e.heartRate())) : "--", "bpm", hrZoneColor(e.hrZone(), int(prof_.hrZones().size()))},
        {"CADENCE", std::to_string(int(std::lround(e.cadence()))), "rpm", Color{200, 200, 220, 255}},
        {"SPEED", fmtSpeed(e.speedMps(), metric), metric ? "km/h" : "mph", SKY},
        {"TIME", fmtTime(e.elapsedS()), "", TEXT},
        {"DISTANCE", fmtDist(e.distance(), metric), "", TEXT},
        {"GRADE", fmt("%+.1f", e.gradePct()), "%", gradeColor(e.gradePct() / 100)},
    };
    float mw = (W - 2 * pad - 6 * 12) / 7, my = 60, mh = 110;
    for (size_t i = 0; i < ms.size(); ++i) metricTile({pad + i * (mw + 12), my, mw, mh}, ms[i].l, ms[i].v, ms[i].u, ms[i].c, i < 4 ? 64 : 44);
    float y = my + mh + 14;
    // second row: W/kg, avg/np, kJ/kcal, TSS, ghost, lap
    std::snprintf(b, sizeof b, "%.2f W/kg   \xC2\xB7   avg %.0f W   \xC2\xB7   NP %.0f W   \xC2\xB7   %.0f kJ  \xC2\xB7  %.0f kcal   \xC2\xB7   TSS %.0f   \xC2\xB7   +%.0f m",
                  p3 / prof_.weightKg, e.avgPower(), e.np(), e.workKj(), e.calories(), e.tss(), e.gain());
    text(b, pad, y, F::Body, DIM, 18);
    if (auto gap = e.ghostGapS()) {
        std::snprintf(b, sizeof b, "Ghost %s%s", *gap >= 0 ? "+" : "-", fmtTime(std::fabs(*gap)).c_str());
        textRight(b, W - pad, y, F::Body, *gap >= 0 ? GOOD : BAD, 18);
    }
    y += 34;
    float rest = H - y - pad;
    // step panel / route
    if (e.structured()) {
        drawStepPanel({pad, y, W * 0.42f, 92});
        Rectangle wc{pad + W * 0.42f + 16, y, W - 2 * pad - W * 0.42f - 16, 92};
        panel(wc);
        workoutChart({wc.x + 12, wc.y + 10, wc.width - 24, wc.height - 20}, e.steps(), prof_.ftp, e.ergBiasValue(), e.workoutT(), &e.samples(),
                     &e.workoutTSamples());
        y += 104;
        rest -= 104;
    }
    float h1 = rest * 0.42f;
    Rectangle rp{pad, y, W - 2 * pad, h1};
    panel(rp);
    text("ROUTE", rp.x + 14, rp.y + 8, F::Small, DIM, 13);
    std::snprintf(b, sizeof b, "%s / %s   lap %d", fmtDist(e.lapDistance(), metric).c_str(), fmtDist(e.profileData().length, metric).c_str(), e.lapNumber());
    textRight(b, rp.x + rp.width - 14, rp.y + 8, F::Small, DIM, 13);
    routeProfile({rp.x + 14, rp.y + 28, rp.width - 28, rp.height - 38}, e.profileData(), e.lapDistance(),
                 e.ghostDistance() ? std::fmod(*e.ghostDistance(), e.profileData().length) : -1, metric);
    y += h1 + 12;
    float h2 = rest - h1 - 12;
    Rectangle up{pad, y, W * 0.36f, h2};
    panel(up);
    text("NEXT 2 KM", up.x + 14, up.y + 8, F::Small, DIM, 13);
    int ci = e.currentClimb();
    if (ci >= 0) {
        const Climb& c = e.profileData().climbs[ci];
        std::snprintf(b, sizeof b, "%s to the top", fmtDist(c.endD - e.lapDistance(), metric).c_str());
        textRight(b, up.x + up.width - 14, up.y + 8, F::Small, WARN, 13);
    }
    upcomingProfile({up.x + 14, up.y + 28, up.width - 28, up.height - 38}, e.profileData(), e.lapDistance(), 2000, metric);
    Rectangle pc{up.x + up.width + 12, y, W - 2 * pad - up.width - 12, h2};
    panel(pc);
    text("POWER  \xC2\xB7  HEART RATE  \xC2\xB7  CADENCE   (5 min)", pc.x + 14, pc.y + 8, F::Small, DIM, 13);
    powerChart({pc.x + 14, pc.y + 28, pc.width - 28, pc.height - 38}, e.samples(), prof_.ftp, prof_.maxHr, 300);
    if (!e.started()) {
        Rectangle r{W / 2 - 240, H * 0.45f, 480, 64};
        panel(r, Color{10, 12, 18, 235});
        textCentered("Start pedalling to begin", r, F::Title, TEXT, 28);
    }
}

// ------------------------------------------------------------------ edit drawer
void App::drawEditDrawer() {
    RideEngine& e = *eng_;
    const float W = float(GetScreenWidth()), H = float(GetScreenHeight());
    Rectangle r{W - 460, 0, 460, H};
    DrawRectangleRec(r, Color{10, 12, 18, 246});
    DrawLineEx({r.x, 0}, {r.x, H}, 1, LINE);
    float x = r.x + 20, y = 18, w = r.width - 40;
    text("Edit ride", x, y, F::Title, TEXT, 28);
    if (button({r.x + r.width - 56, y, 36, 32}, "x")) drawerOpen_ = false;
    y += 48;
    static const std::vector<std::string> tabs = {"Intervals", "Ride", "Road ahead", "Insert"};
    int t = segmented({x, y, w, 36}, tabs, drawerTab_);
    if (t >= 0) drawerTab_ = t;
    y += 52;
    char b[160];
    auto btn = [&](const char* label, bool enabled = true) {
        bool c = button({x, y, w, 40}, label, false, enabled, 17);
        y += 48;
        return c;
    };
    auto pair = [&](const char* a, const char* bb, bool en, int& which) {
        which = 0;
        if (button({x, y, w / 2 - 5, 40}, a, false, en, 17)) which = 1;
        if (button({x + w / 2 + 5, y, w / 2 - 5, 40}, bb, false, en, 17)) which = 2;
        y += 48;
    };
    int k;
    if (drawerTab_ == 0) {
        bool on = e.structuredActive() && e.currentStep().has_value();
        if (!e.structured()) {
            textWrapped("This ride has no intervals. Use Insert to add a set - ERG takes over for it, then hands back.", {x, y, w, 60}, F::Body, DIM, 16);
            y += 70;
        } else if (auto cs = e.currentStep()) {
            text("Current: " + e.steps()[cs->first].label, x, y, F::Body, TEXT, 17);
            y += 32;
        }
        pair("Step -1 min", "Step +1 min", on, k);
        if (k == 1) e.extendStep(-60); else if (k == 2) e.extendStep(60);
        pair("Step -30 s", "Step +30 s", on, k);
        if (k == 1) e.extendStep(-30); else if (k == 2) e.extendStep(30);
        pair("Target -5%", "Target +5%", on, k);
        if (k == 1) e.adjustStep(-5); else if (k == 2) e.adjustStep(5);
        std::snprintf(b, sizeof b, "Intensity %.0f%%", e.ergBiasValue() * 100);
        text(b, x, y + 4, F::Body, DIM, 16);
        y += 30;
        pair("Intensity -1%", "Intensity +1%", e.structured(), k);
        if (k == 1) e.ergBias(-1); else if (k == 2) e.ergBias(1);
        if (btn("One more rep", on)) e.oneMoreRep();
        if (btn("Skip this step  (Right arrow)", on)) e.skipStep();
        if (btn("Skip to cool-down", on)) e.skipToCooldown();
        if (btn(e.ergDisabled() ? "Switch back to ERG" : "Switch to resistance mode", e.structured())) e.toggleErg();
        if (btn("End workout here", on)) e.endWorkout();
    } else if (drawerTab_ == 1) {
        if (e.setup().kind == RideKind::Free) {
            text("Free-ride mode", x, y, F::Small, DIM);
            y += 20;
            int m = segmented({x, y, w, 36}, {"Gradient", "Power", "Level", "HR"}, int(e.freeMode()));
            if (m >= 0) e.setFreeMode(FreeMode(m));
            y += 48;
            pair("Target -", "Target +", true, k);
            if (k) e.nudge(k == 1 ? -1 : 1);
        }
        text("Goal", x, y, F::Small, DIM);
        y += 20;
        int g = segmented({x, y, w, 34}, {"None", "Time", "Dist", "kJ", "kcal", "Climb"}, int(e.goal().type));
        if (g >= 0) {
            static const double defaults[] = {0, 3600, 30000, 600, 600, 500};
            Goal ng; ng.type = Goal::Type(g); ng.value = defaults[g];
            e.setGoal(ng);
        }
        y += 44;
        if (e.goal().type != Goal::None) {
            static const double steps[] = {0, 300, 5000, 50, 50, 50};
            pair("Goal -", "Goal +", true, k);
            if (k) { Goal ng = e.goal(); ng.value = std::max(steps[ng.type], ng.value + (k == 1 ? -1 : 1) * steps[ng.type]); e.setGoal(ng); }
        }
        std::snprintf(b, sizeof b, "Trainer difficulty %.0f%%", e.difficultyValue() * 100);
        text(b, x, y + 4, F::Body, DIM, 16);
        y += 30;
        pair("Difficulty -10%", "Difficulty +10%", true, k);
        if (k) e.difficulty(k == 1 ? -10 : 10);
        if (e.setup().kind == RideKind::Route) {
            std::snprintf(b, sizeof b, "Laps: %d", e.laps());
            text(b, x, y + 4, F::Body, DIM, 16);
            y += 30;
            pair("Remove lap", "Add lap", true, k);
            if (k) e.addLaps(k == 1 ? -1 : 1);
        }
        if (btn("Record a lap  (L)")) e.lap();
        if (prof_.virtualShifting) {
            std::snprintf(b, sizeof b, "Gear %d / 24  (ratio %.2f)", e.gear(), e.gearRatio());
            text(b, x, y + 4, F::Body, DIM, 16);
            y += 30;
            pair("Shift down  [", "Shift up  ]", true, k);
            if (k) e.shift(k == 1 ? -1 : 1);
        }
    } else if (drawerTab_ == 2) {
        drawRoadAhead(x, y, w);
    } else {
        textWrapped("Add intervals to this ride in the text format, e.g.  5x 1m @120% / 1m @50%  or  3x (30s @150%, 30s @50%).", {x, y, w, 60},
                    F::Small, DIM, 15);
        y += 56;
        textBox({x, y, w, 120}, insertText_, true, "5x 1m @120% / 1m @50%");
        y += 130;
        std::vector<wk::Block> blocks;
        bool ok = !insertText_.text.empty() && wk::parseText(insertText_.text, prof_.ftp, blocks, insertErr_);
        if (ok) {
            auto st = wk::blocksToSteps(blocks);
            workoutChart({x, y, w, 70}, st, prof_.ftp, 1.0, -1, nullptr);
            y += 80;
            text(fmtTime(st.back().endS()) + "  \xC2\xB7  " + std::to_string(st.size()) + " steps", x, y, F::Small, TEXT);
            y += 26;
        } else if (!insertText_.text.empty()) {
            textWrapped(insertErr_, {x, y, w, 40}, F::Small, BAD, 14);
            y += 40;
        }
        if (button({x, y, w / 2 - 5, 42}, "Insert next", true, ok)) { e.insertBlocks(blocks, false); insertText_.text.clear(); }
        if (button({x + w / 2 + 5, y, w / 2 - 5, 42}, "Add at end", false, ok)) { e.insertBlocks(blocks, true); insertText_.text.clear(); }
        y += 56;
        text("Quick sets", x, y, F::Small, DIM);
        y += 22;
        const char* quick[][2] = {{"5 x 30/30 @130%", "5x 30s @130% / 30s @50%"}, {"3 x 3 min VO2", "3x 3m @118% / 3m @50%"},
                                  {"2 x 8 min threshold", "2x 8m @100% / 4m @55%"}, {"10 min sweet spot", "10m @90%"}, {"Sprint x 3", "3x (10s effort @2%, 2m @50%)"}};
        for (auto& q : quick) {
            if (button({x, y, w, 36}, q[0], false, true, 16)) {
                std::vector<wk::Block> qb;
                std::string err;
                if (wk::parseText(q[1], prof_.ftp, qb, err)) e.insertBlocks(qb, false);
            }
            y += 42;
        }
    }
    // block clicks to the HUD beneath
    if (CheckCollisionPointRec(GetMousePosition(), r)) ui::setInputBlocked(true);
}

void App::drawPauseMenu() {
    const float W = float(GetScreenWidth()), H = float(GetScreenHeight());
    DrawRectangle(0, 0, int(W), int(H), Color{0, 0, 0, 130});
    Rectangle r{W / 2 - 220, H / 2 - 190, 440, 380};
    panel(r, Color{14, 17, 26, 248});
    textCentered("Paused", {r.x, r.y + 12, r.width, 50}, F::Title, TEXT);
    textCentered(fmtTime(eng_->elapsedS()) + "  \xC2\xB7  " + fmtDist(eng_->distance(), prof_.metric), {r.x, r.y + 58, r.width, 24}, F::Body, DIM);
    float y = r.y + 100;
    if (button({r.x + 40, y, r.width - 80, 50}, "Resume", true)) { pauseMenu_ = false; eng_->resume(); }
    y += 62;
    if (button({r.x + 40, y, r.width - 80, 50}, view3d_ ? "Switch to 2D dashboard" : "Switch to 3D world")) view3d_ = !view3d_;
    y += 62;
    if (button({r.x + 40, y, r.width - 80, 50}, "End ride & save")) { pauseMenu_ = false; endRide(true); }
    y += 62;
    if (button({r.x + 40, y, r.width - 80, 50}, "Discard ride")) { pauseMenu_ = false; endRide(false); }
}

void App::drawHelp() {
    const float W = float(GetScreenWidth()), H = float(GetScreenHeight());
    Rectangle r{W / 2 - 340, H / 2 - 290, 680, 580};
    panel(r, Color{14, 17, 26, 248});
    text("Keys", r.x + 24, r.y + 16, F::Title, TEXT);
    const char* rows[][2] = {{"Space", "Pause / resume"}, {"L", "Record a lap"}, {"E", "Edit drawer (intervals, goal, laps, insert sets)"},
                             {"Up / Down", "Nudge target: intensity, watts, gradient, level, HR, difficulty"}, {"Right", "Skip to the next workout step"},
                             {"[ ]  PgUp / PgDn", "Virtual gears (Zwift Cog)"}, {"H", "Heart-rate alerts on / off"}, {"V", "2D dashboard / 3D world"},
                             {"R", "Change the road ahead: add a climb, descent, sprint..."}, {"Backspace", "Undo the last road change"},
                             {"C", "Camera: chase, cinematic, side, rider, drone, close, eyes"}, {"T / W / M", "Time of day / weather / sound"}, {"D", "Simulator test data"},
                             {"Shift+Up/Down", "Simulator power +-10 W"}, {"Alt+Up/Down", "Simulator heart rate +-2 bpm"}, {"Tab", "Hide the HUD"},
                             {"Esc", "Menu"}};
    float y = r.y + 70;
    for (auto& rw : rows) {
        text(rw[0], r.x + 30, y, F::Body, ACCENT, 17);
        text(rw[1], r.x + 220, y, F::Body, TEXT, 17);
        y += 27;
    }
}

// ------------------------------------------------------------------ sectors (road ahead)
void App::drawSectorBanner(float x, float& y, float w) {
    const RideEngine& e = *eng_;
    char b[200];
    for (auto& s : e.sectors()) {
        if (s.done && (!s.result || e.distance() > s.endTotal + 400)) continue;
        Rectangle r{x, y, w, 36};
        panel(r);
        double d = e.distance();
        if (d < s.startTotal) {
            std::snprintf(b, sizeof b, "%s in %s", s.name.c_str(), fmtDist(s.startTotal - d, prof_.metric).c_str());
            text(b, r.x + 14, r.y + 9, F::Small, SKY, 14);
        } else if (!s.done) {
            double prog = (d - s.startTotal) / std::max(1.0, s.endTotal - s.startTotal);
            std::snprintf(b, sizeof b, "%s  \xC2\xB7  %s to go%s", s.name.c_str(), fmtDist(s.endTotal - d, prof_.metric).c_str(), s.timed ? "  \xC2\xB7  timed" : "");
            text(b, r.x + 14, r.y + 9, F::Small, WARN, 14);
            progressBar({r.x + r.width - 150, r.y + 15, 136, 6}, float(prog), WARN);
        } else if (s.result) {
            std::snprintf(b, sizeof b, "%s done  \xC2\xB7  %s  \xC2\xB7  %.0f W", s.name.c_str(), fmtTime(s.result->durationS).c_str(), s.result->avgPower);
            text(b, r.x + 14, r.y + 9, F::Small, GOOD, 14);
        }
        y += 44;
    }
}

void App::drawRoadAhead(float x, float& y, float w) {
    RideEngine& e = *eng_;
    char b[200];
    int d;
    textWrapped("Splice something into the road in front of you. The 3D road is re-laid ahead and the trainer follows the new gradient.", {x, y, w, 50}, F::Small, DIM, 14);
    y += 46;
    static const char* kinds[] = {"Climb", "Descent", "Flat", "Rollers", "Sprint", "Terrain"};
    static const SectorKind kk[] = {SectorKind::Climb, SectorKind::Descent, SectorKind::Flat, SectorKind::Rollers, SectorKind::Sprint, SectorKind::Terrain};
    float bw = (w - 2 * 8) / 3;
    for (int i = 0; i < 6; ++i)
        if (chipButton({x + (i % 3) * (bw + 8), y + (i / 3) * 38, bw, 32}, kinds[i], sector_.kind == kk[i])) {
            sector_.kind = kk[i];
            if (kk[i] == SectorKind::Climb) { sector_.gradePct = std::max(2.0, std::fabs(sector_.gradePct)); }
            if (kk[i] == SectorKind::Descent) { sector_.gradePct = -std::max(2.0, std::fabs(sector_.gradePct)); }
            if (kk[i] == SectorKind::Sprint) sector_.lengthM = 300;
            if (kk[i] == SectorKind::Terrain && !sector_.terrain) sector_.terrain = Terrain::Hilly;
        }
    y += 84;
    double step = sector_.lengthM >= 2000 ? 500 : sector_.lengthM >= 600 ? 200 : 100;
    if (stepper({x, y, w, 34}, "Length", fmtDist(sector_.lengthM, prof_.metric), &d, 120)) sector_.lengthM = std::clamp(sector_.lengthM + d * step, 100.0, 30000.0);
    y += 42;
    if (sector_.kind == SectorKind::Climb || sector_.kind == SectorKind::Descent) {
        std::snprintf(b, sizeof b, "%+.1f%%", sector_.gradePct);
        if (stepper({x, y, w, 34}, "Gradient", b, &d, 120)) {
            double g = std::fabs(sector_.gradePct) + d * 0.5;
            g = std::clamp(g, 1.0, 20.0);
            sector_.gradePct = sector_.kind == SectorKind::Descent ? -g : g;
        }
        y += 42;
    } else if (sector_.kind == SectorKind::Terrain) {
        int t = segmented({x, y, w, 32}, {"Rolling", "Hilly", "Mountain", "Alpine"}, std::max(0, int(sector_.terrain.value_or(Terrain::Hilly)) - 1));
        if (t >= 0) sector_.terrain = Terrain(t + 1);
        y += 42;
    }
    if (stepper({x, y, w, 34}, "Starts in", fmtDist(sector_.inM, prof_.metric), &d, 120)) sector_.inM = std::clamp(sector_.inM + d * 100.0, 100.0, 5000.0);
    y += 42;
    if (toggle({x, y, w, 32}, "Return to the original height afterwards", sector_.rejoin)) sector_.rejoin = !sector_.rejoin;
    y += 40;
    if (toggle({x, y, w, 32}, "Time it (shows as a split)", sector_.timed)) sector_.timed = !sector_.timed;
    y += 44;
    auto segs = sector_.build(1);
    auto [len, net] = segmentsSummary(segs);
    std::snprintf(b, sizeof b, "%s  \xC2\xB7  %s  \xC2\xB7  %+.0f m", sector_.displayName().c_str(), fmtDist(len, prof_.metric).c_str(), net);
    text(b, x, y, F::Small, TEXT, 14);
    y += 26;
    if (button({x, y, w, 44}, "Add to the road", true)) {
        e.addSector(sector_);
        toast(sector_.displayName() + " added ahead", SKY);
    }
    y += 54;
    if (button({x, y, w, 38}, "Undo last change  (Backspace)", false, e.canUndoSector(), 15)) { e.undoSector(); toast("Road change undone"); }
    y += 52;
    // quick picks
    text("Quick picks", x, y, F::Small, DIM);
    y += 22;
    struct Q { const char* l; SectorKind k; double len, g; };
    Q qs[] = {{"1 km @ 8% kicker", SectorKind::Climb, 1000, 8}, {"5 km @ 5% climb", SectorKind::Climb, 5000, 5}, {"300 m sprint", SectorKind::Sprint, 300, 0},
              {"2 km rollers", SectorKind::Rollers, 2000, 0}};
    for (auto& q : qs) {
        if (button({x, y, w, 34}, q.l, false, true, 14)) {
            SectorSpec sp;
            sp.kind = q.k; sp.lengthM = q.len; sp.gradePct = q.g; sp.inM = 300;
            e.addSector(sp);
            toast(sp.displayName() + " added ahead", SKY);
        }
        y += 40;
    }
    // upcoming profile with the change
    if (y + 90 < GetScreenHeight()) {
        text("NEXT 3 KM", x, y + 6, F::Small, DIM, 12);
        upcomingProfile({x, y + 24, w, 70}, e.profileData(), e.lapDistance(), 3000, prof_.metric);
    }
}
