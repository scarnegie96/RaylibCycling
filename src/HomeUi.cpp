// Shell (sidebar navigation), Home dashboard and the Ride setup page.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "App.h"
#include "Audio.h"
#include "core/Metrics.h"
#include "raylib.h"

using namespace ui;

static std::string fmt(const char* f, double v) { char b[96]; std::snprintf(b, sizeof b, f, v); return b; }
static const Color CARD{255, 255, 255, 255};
static const Color CARD2{246, 248, 251, 255};

// ------------------------------------------------------------------ shell
void App::drawShell() {
    const float W = float(GetScreenWidth()), H = float(GetScreenHeight());
    DrawRectangle(0, 0, int(W), int(H), Color{15, 23, 42, 0});
    Rectangle side{0, 0, 232, H};
    drawSidebar(side);
    Rectangle body{side.width + 24, 22, W - side.width - 48, H - 44};
    // leaving Profile with a new FTP records it in the FTP history
    static int lastPage = -1;
    if (lastPage == PProfile && page_ != PProfile && std::fabs(prof_.ftp - ftpOnOpen_) > 0.5) {
        db_.addFtp({Date::today().str(), prof_.ftp, "Manual"});
        ftpOnOpen_ = prof_.ftp;
        refreshHistory();
    }
    lastPage = page_;
    switch (page_) {
        case PHome: drawHomePage(body); break;
        case PRide: drawRidePage(body); break;
        case PRoutes: drawRoutesPage(body); break;
        case PWorkouts: drawWorkoutsPage(body); break;
        case PEvents: drawEventsPage(body); break;
        case PActivities: drawActivitiesPage(body); break;
        case PStats: drawStatsPage(body); break;
        case PDevices: drawDevicesPage(body); break;
        default: drawProfilePage(body); break;
    }
    if (IsKeyPressed(KEY_ENTER) && !anyTextActive() && page_ == PRide) startRide();
}

void App::drawSidebar(Rectangle r) {
    DrawRectangleRec(r, Color{251, 252, 253, 255});
    DrawLineEx({r.x + r.width, 0}, {r.x + r.width, r.height}, 1, LINE);
    text("CORE", 26, 22, F::Big, TEXT, 44);
    float cw = measure("CORE", F::Big, 44).x;
    text("RAY", 26 + cw + 2, 22, F::Big, ACCENT, 44);
    static const char* names[PAGE_COUNT] = {"Home", "Ride", "Routes", "Workouts", "Events", "Activities", "Stats", "Devices", "Profile"};
    static const char* glyph[PAGE_COUNT] = {"\xE2\x8C\x82", "\xE2\x96\xB6", "\xE2\x97\x86", "\xE2\x89\xA1", "\xE2\x97\xB7", "\xE2\x97\x8F", "\xE2\x96\xA4", "\xE2\x97\x89", "\xE2\x98\xBA"};
    (void)glyph;
    float y = 96;
    for (int i = 0; i < PAGE_COUNT; ++i) {
        if (i == PDevices) y += 14;
        Rectangle it{12, y, r.width - 24, 42};
        bool sel = page_ == i, h = hovered(it);
        if (sel) DrawRectangleRounded(it, 0.3f, 8, Color{15, 157, 88, 40});
        else if (h) DrawRectangleRounded(it, 0.3f, 8, Color{15, 23, 42, 12});
        if (sel) DrawRectangleRounded({it.x, it.y + 10, 3, 22}, 1, 4, ACCENT);
        text(names[i], it.x + 20, it.y + 10, F::Body, sel ? TEXT : DIM, 19);
        if (i == PEvents) {
            auto up = upcoming(events_, Date::today(), 1);
            if (!up.empty()) {
                Rectangle bd{it.x + it.width - 34, it.y + 11, 22, 20};
                DrawRectangleRounded(bd, 1, 6, ACCENT);
                textCentered(std::to_string(up.size()), bd, F::Small, WHITE, 12);
            }
        }
        if (clicked(it)) {
            page_ = i;
            if (i == PActivities) detail_.reset();
            if (i == PProfile) ftpOnOpen_ = prof_.ftp;
        }
        y += 46;
    }
    // world + device status
    float by = r.height - 196;
    DrawLineEx({16, by - 12}, {r.width - 16, by - 12}, 1, LINE);
    auto chipLine = [&](const std::string& label, Color dot, float yy) {
        DrawCircle(28, int(yy + 9), 5, dot);
        text(label, 42, yy, F::Small, DIM, 14);
    };
    Link t = ble_.trainer(), h = ble_.hrm();
    chipLine(simActive() ? "Simulator" : t.state == LinkState::Connected ? t.name : "No trainer", simActive() ? SKY : t.state == LinkState::Connected ? GOOD : FAINT, by);
    chipLine(h.state == LinkState::Connected ? h.name : "No heart rate", h.state == LinkState::Connected ? GOOD : FAINT, by + 24);
    // weather / time chip: click to cycle
    Rectangle wr{12, by + 54, r.width - 24, 58};
    bool wh = hovered(wr);
    DrawRectangleRounded(wr, 0.25f, 8, wh ? Color{15, 23, 42, 16} : Color{15, 23, 42, 8});
    std::string wl = prefs_.weatherMode == 1 ? (live_ ? std::string(weather::kindLabel(live_->kind)) + fmt("  %.0f\xC2\xB0", live_->temperatureC) : "Live weather...")
                                               : weather::kindLabel(wx_.currentKind(wxElapsed_));
    text(wl, wr.x + 12, wr.y + 8, F::Body, TEXT, 16);
    const char* tm = prefs_.todMode == 0 ? "Real time" : prefs_.todMode == 2 ? "Day/night cycle" : "Fixed time";
    char b[80];
    double hh = prefs_.todMode == 0 ? 0 : std::fmod(prefs_.todHour + cycleHours_, 24.0);
    if (prefs_.todMode == 0) { std::time_t now = std::time(nullptr); std::tm lt{}; localtime_r(&now, &lt); hh = lt.tm_hour + lt.tm_min / 60.0; }
    std::snprintf(b, sizeof b, "%s  %02d:%02d  \xC2\xB7  %s", tm, int(hh), int(std::fmod(hh, 1.0) * 60), prefs_.weatherMode == 0 ? "changing" : prefs_.weatherMode == 1 ? "live" : "fixed");
    text(b, wr.x + 12, wr.y + 32, F::Small, FAINT, 12);
    if (wh && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) cycleWeather();
    if (wh && IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) cycleTime();
    text("W weather  T time  M sound", 18, by + 122, F::Small, Color{15, 23, 42, 63}, 12);
    text(sfx::muted() ? "Sound off" : "", 18, by + 140, F::Small, WARN, 12);
}

// ------------------------------------------------------------------ home dashboard
static const char* suggestion(double tsb, int daysSinceHard, const char** why) {
    if (tsb < -25) { *why = "You're carrying a lot of fatigue - spin the legs and recover."; return "builtin-recovery-45"; }
    if (tsb < -12) { *why = "Fatigue is building - keep it aerobic today."; return "builtin-endurance-60"; }
    if (tsb > 12 && daysSinceHard >= 3) { *why = "You're fresh - a good day for VO2 max work."; return "builtin-vo2-5x3"; }
    if (tsb > 4) { *why = "Form is positive - time for some threshold."; return "builtin-threshold-3x10"; }
    *why = "Balanced form - sweet spot builds fitness without digging a hole.";
    return "builtin-sweet-spot-3x15";
}

void App::drawHomePage(Rectangle r) {
    const bool metric = prof_.metric;
    char b[200];
    Date today = Date::today();
    std::time_t now = std::time(nullptr);
    std::tm lt{};
    localtime_r(&now, &lt);
    const char* greet = lt.tm_hour < 12 ? "Good morning" : lt.tm_hour < 18 ? "Good afternoon" : "Good evening";
    text(std::string(greet) + ", " + prof_.name, r.x, r.y, F::Big, TEXT, 40);
    std::snprintf(b, sizeof b, "%s %d %s", dates::weekdayShort(today.weekday()), today.d, dates::monthName(today.m));
    text(b, r.x + 2, r.y + 48, F::Body, DIM, 17);
    if (button({r.x + r.width - 190, r.y + 6, 190, 48}, "Ride now", true)) page_ = PRide;
    float y = r.y + 84;

    // ---- eFTP banner
    if (assessment_ && !assessment_->dismissed && prof_.ftpAuto != FtpAutoMode::Off && assessment_->suggestedFtp &&
        (assessment_->suggestion == Suggestion::Increase || assessment_->suggestion == Suggestion::Decrease)) {
        Rectangle br{r.x, y, r.width, 58};
        bool up = assessment_->suggestion == Suggestion::Increase;
        panel(br, up ? Color{15, 157, 88, 26} : Color{201, 130, 0, 26});
        std::snprintf(b, sizeof b, "%s Your recent rides point to an FTP of %.0f W (%+.0f W).", up ? "\xE2\x86\x91" : "\xE2\x86\x93", *assessment_->suggestedFtp,
                      *assessment_->suggestedFtp - prof_.ftp);
        text(b, br.x + 18, br.y + 8, F::Body, TEXT, 18);
        text(assessment_->reason, br.x + 18, br.y + 32, F::Small, DIM, 13);
        if (button({br.x + br.width - 300, br.y + 10, 170, 38}, fmt("Set FTP to %.0f W", *assessment_->suggestedFtp), true)) {
            applyFtp(*assessment_->suggestedFtp, "Estimate");
            toast("FTP updated", GOOD);
        }
        if (button({br.x + br.width - 120, br.y + 10, 104, 38}, "Dismiss")) {
            assessment_->dismissed = true;
            db_.setMeta("ftp_assessment", json::dump(assessment_->toJson()));
        }
        y += 70;
    }

    // ---- top cards
    float gap = 16, cw = (r.width - 3 * gap) / 4, ch = 150;
    auto card = [&](int i, const char* title) {
        Rectangle c{r.x + i * (cw + gap), y, cw, ch};
        panel(c, CARD);
        text(title, c.x + 18, c.y + 14, F::Small, DIM, 13);
        return c;
    };
    {
        Rectangle c = card(0, "THIS WEEK");
        double hrs = stats_.thisWeek.movingS / 3600.0, goal = std::max(0.5, prof_.weeklyGoalHours);
        std::snprintf(b, sizeof b, "%.1f h", hrs);
        text(b, c.x + 18, c.y + 34, F::Big, TEXT, 44);
        textRight(fmt("of %.0f h", goal), c.x + c.width - 18, c.y + 52, F::Body, DIM, 16);
        progressBar({c.x + 18, c.y + 92, c.width - 36, 8}, float(std::min(1.0, hrs / goal)), hrs >= goal ? GOOD : ACCENT);
        std::snprintf(b, sizeof b, "%d rides  \xC2\xB7  %s  \xC2\xB7  TSS %.0f", stats_.thisWeek.rides, fmtDist(stats_.thisWeek.distanceM, metric).c_str(), stats_.thisWeek.tss);
        text(b, c.x + 18, c.y + 112, F::Small, DIM, 13);
    }
    double ctl = 0, atl = 0, tsb = 0;
    if (!stats_.fitness.empty()) { ctl = stats_.fitness.back().ctl; atl = stats_.fitness.back().atl; tsb = stats_.fitness.back().tsb; }
    {
        Rectangle c = card(1, "FORM");
        const char* lab = tsb > 15 ? "Very fresh" : tsb > 5 ? "Fresh" : tsb > -10 ? "Neutral" : tsb > -25 ? "Tired" : "Very tired";
        Color lc = tsb > 5 ? GOOD : tsb > -10 ? SKY : tsb > -25 ? WARN : BAD;
        text(fmt("%+.0f", tsb), c.x + 18, c.y + 34, F::Big, lc, 44);
        text(lab, c.x + 20 + measure(fmt("%+.0f", tsb), F::Big, 44).x + 8, c.y + 52, F::Body, lc, 17);
        std::snprintf(b, sizeof b, "Fitness %.0f  \xC2\xB7  Fatigue %.0f", ctl, atl);
        text(b, c.x + 18, c.y + 96, F::Small, DIM, 13);
        std::vector<float> tsbs;
        for (size_t i = stats_.fitness.size() > 42 ? stats_.fitness.size() - 42 : 0; i < stats_.fitness.size(); ++i) tsbs.push_back(float(stats_.fitness[i].tsb));
        if (tsbs.size() > 2) seriesChart({c.x + 18, c.y + 116, c.width - 36, 24}, tsbs, lc, -40, 30, false);
    }
    {
        Rectangle c = card(2, "FTP");
        text(fmt("%.0f W", prof_.ftp), c.x + 18, c.y + 34, F::Big, TEXT, 44);
        textRight(fmt("%.2f W/kg", prof_.ftp / prof_.weightKg), c.x + c.width - 18, c.y + 52, F::Body, ACCENT, 17);
        if (assessment_ && assessment_->estimate) {
            std::snprintf(b, sizeof b, "eFTP %.0f W  \xC2\xB7  %s confidence", assessment_->estimate->estimate, assessment_->estimate->confidence.c_str());
            text(b, c.x + 18, c.y + 96, F::Small, DIM, 13);
            text(assessment_->estimate->method == "critical_power" ? "from your critical-power curve (42 days)" : "95% of your best 20 minutes",
                 c.x + 18, c.y + 116, F::Small, FAINT, 12);
        } else text("Ride hard for 3-20 min to get an estimate", c.x + 18, c.y + 96, F::Small, FAINT, 13);
    }
    {
        Rectangle c = card(3, "STREAK");
        text(std::to_string(stats_.weekStreak), c.x + 18, c.y + 34, F::Big, TEXT, 44);
        text(stats_.weekStreak == 1 ? "week" : "weeks", c.x + 24 + measure(std::to_string(stats_.weekStreak), F::Big, 44).x, c.y + 52, F::Body, DIM, 17);
        std::snprintf(b, sizeof b, "%d rides all time  \xC2\xB7  %s", stats_.allTime.rides, fmtDist(stats_.allTime.distanceM, metric).c_str());
        text(b, c.x + 18, c.y + 96, F::Small, DIM, 13);
        std::snprintf(b, sizeof b, "%.0f h this month", stats_.thisMonth.movingS / 3600);
        text(b, c.x + 18, c.y + 116, F::Small, FAINT, 12);
    }
    y += ch + gap;

    // ---- second row: suggestion + upcoming | recent rides + volume
    float colW = (r.width - gap) * 0.48f;
    float rest = r.y + r.height - y;
    Rectangle L{r.x, y, colW, rest};
    Rectangle R{r.x + colW + gap, y, r.width - colW - gap, rest};
    {
        Rectangle s{L.x, L.y, L.width, 168};
        panel(s, CARD);
        text("SUGGESTED TODAY", s.x + 18, s.y + 14, F::Small, DIM, 13);
        int sinceHard = 99;
        for (auto& rd : rides_) {
            if (rd.intensityFactor > 0.88) {
                if (auto d = dates::localDate(rd.startedAt)) sinceHard = int(today - *d);
                break;
            }
        }
        const char* why;
        const char* wid = suggestion(tsb, sinceHard, &why);
        const wk::Workout* w = findWorkout(wid);
        if (w) {
            text(w->name, s.x + 18, s.y + 36, F::Title, TEXT, 24);
            auto e = w->estimate(prof_.ftp);
            std::snprintf(b, sizeof b, "%s  \xC2\xB7  TSS %.0f  \xC2\xB7  %s", fmtTime(e.durationS).c_str(), e.tss, w->category.c_str());
            text(b, s.x + 18, s.y + 68, F::Small, DIM, 14);
            text(why, s.x + 18, s.y + 90, F::Small, FAINT, 13);
            workoutChart({s.x + 18, s.y + 112, s.width - 200, 44}, w->steps(), prof_.ftp, 1.0, -1, nullptr);
            if (button({s.x + s.width - 168, s.y + 116, 150, 38}, "Ride it", true)) {
                selWorkout_ = w->id;
                setupKind_ = RideKind::Workout;
                page_ = PRide;
            }
        }
        Rectangle u{L.x, L.y + 184, L.width, L.height - 184};
        panel(u, CARD);
        text("UPCOMING", u.x + 18, u.y + 14, F::Small, DIM, 13);
        if (button({u.x + u.width - 120, u.y + 8, 104, 30}, "Events", false, true, 14)) page_ = PEvents;
        auto up = upcoming(events_, today, 7);
        float yy = u.y + 48;
        if (up.empty()) textWrapped("No events this week. Schedule recurring rides in Events - a Tuesday threshold session, a Saturday long ride...",
                                    {u.x + 18, yy, u.width - 36, 60}, F::Small, FAINT, 14);
        for (auto& o : up) {
            if (yy > u.y + u.height - 50) break;
            Rectangle row{u.x + 12, yy, u.width - 24, 44};
            DrawRectangleRounded(row, 0.25f, 6, CARD2);
            Color ec = GOOD;
            if (o.color.size() == 7) {
                unsigned v = std::strtoul(o.color.c_str() + 1, nullptr, 16);
                ec = Color{(unsigned char)(v >> 16), (unsigned char)(v >> 8), (unsigned char)v, 255};
            }
            DrawRectangleRounded({row.x + 8, row.y + 10, 4, 24}, 1, 4, ec);
            bool isToday = o.date == today;
            std::snprintf(b, sizeof b, "%s  %s", isToday ? "Today" : o.date == today.plus(1) ? "Tomorrow" : dates::weekdayShort(o.date.weekday()), o.time.c_str());
            text(b, row.x + 22, row.y + 4, F::Small, isToday ? ACCENT : DIM, 13);
            text(o.eventName, row.x + 22, row.y + 21, F::Body, TEXT, 16);
            if (isToday) {
                for (auto& e : events_)
                    if (e.id == o.eventId && button({row.x + row.width - 96, row.y + 7, 86, 30}, "Start", true, true, 14)) startEvent(e);
            }
            yy += 50;
        }
    }
    {
        float vh = 190;
        Rectangle v{R.x, R.y, R.width, vh};
        panel(v, CARD);
        text("WEEKLY HOURS  \xC2\xB7  12 WEEKS", v.x + 18, v.y + 14, F::Small, DIM, 13);
        std::vector<float> hv;
        std::vector<std::string> lb;
        size_t n = stats_.weekly.size();
        for (size_t i = n > 12 ? n - 12 : 0; i < n; ++i) {
            hv.push_back(float(stats_.weekly[i].totals.movingS / 3600));
            const Date& d = stats_.weekly[i].weekStart;
            lb.push_back(i % 2 == (n - 1) % 2 ? fmtDateShort(d.y, d.m, d.d) : "");
        }
        barChart({v.x + 18, v.y + 36, v.width - 36, vh - 48}, hv, lb, ACCENT, int(hv.size()) - 1, "%.1f", float(prof_.weeklyGoalHours));
        Rectangle rr{R.x, R.y + vh + 16, R.width, R.height - vh - 16};
        panel(rr, CARD);
        text("RECENT RIDES", rr.x + 18, rr.y + 14, F::Small, DIM, 13);
        if (button({rr.x + rr.width - 120, rr.y + 8, 104, 30}, "All rides", false, true, 14)) { page_ = PActivities; detail_.reset(); }
        float yy = rr.y + 46;
        if (rides_.empty()) text("No rides yet - your first one will show up here.", rr.x + 18, yy, F::Small, FAINT, 14);
        for (auto& rd : rides_) {
            if (yy > rr.y + rr.height - 46) break;
            Rectangle row{rr.x + 12, yy, rr.width - 24, 42};
            bool h = hovered(row);
            DrawRectangleRounded(row, 0.25f, 6, h ? Color{238, 241, 245, 255} : CARD2);
            auto d = dates::localDate(rd.startedAt);
            text(d ? fmtDateShort(d->y, d->m, d->d) : "", row.x + 12, row.y + 12, F::Small, DIM, 13);
            text(rd.name, row.x + 74, row.y + 11, F::Body, TEXT, 16);
            std::snprintf(b, sizeof b, "%s  \xC2\xB7  %s  \xC2\xB7  %.0f W  \xC2\xB7  TSS %.0f", fmtTime(rd.movingS).c_str(), fmtDist(rd.distanceM, metric).c_str(), rd.np, rd.tss);
            textRight(b, row.x + row.width - 12, row.y + 13, F::Small, DIM, 13);
            if (h && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { page_ = PActivities; selRide_ = rd.id; detail_ = db_.ride(rd.id); }
            yy += 46;
        }
    }
}

// ------------------------------------------------------------------ ride setup
void App::drawRidePage(Rectangle r) {
    float lw = std::min(600.f, r.width * 0.46f);
    Rectangle L{r.x, r.y, lw, r.height};
    panel(L, CARD);
    float x = L.x + 22, y = L.y + 18, w = L.width - 44;
    text("Start a ride", x, y, F::Title, TEXT);
    y += 50;
    static const std::vector<std::string> kinds = {"Route", "Workout", "Test", "Free ride"};
    static const RideKind order[] = {RideKind::Route, RideKind::Workout, RideKind::Test, RideKind::Free};
    int cur = 0;
    for (int i = 0; i < 4; ++i) if (order[i] == setupKind_) cur = i;
    int k = segmented({x, y, w, 42}, kinds, cur);
    if (k >= 0) setupKind_ = order[k];
    y += 58;
    int d;
    char b[200];
    const Route* rt = findRoute(selRoute_);
    switch (setupKind_) {
        case RideKind::Route: {
            text("Route", x, y, F::Small, DIM);
            y += 22;
            Rectangle list{x, y, w, std::min(260.f, L.y + L.height - y - 330)};
            static float sc = 0;
            float rowH = 46;
            scrollUpdate(list, sc, routes_.size() * (rowH + 4));
            BeginScissorMode(int(list.x), int(list.y), int(list.width), int(list.height));
            for (size_t i = 0; i < routes_.size(); ++i) {
                const Route& ro = routes_[i];
                Rectangle row{list.x, list.y + i * (rowH + 4) - sc, list.width - 8, rowH};
                if (row.y + rowH < list.y || row.y > list.y + list.height) continue;
                bool sel = ro.id == selRoute_, h = hovered(row) && CheckCollisionPointRec(GetMousePosition(), list);
                DrawRectangleRounded(row, 0.25f, 6, sel ? Color{15, 157, 88, 55} : h ? Color{238, 241, 245, 255} : CARD2);
                text((ro.favourite ? "\xE2\x98\x85 " : "") + ro.name, row.x + 12, row.y + 5, F::Body, TEXT, 16);
                auto st = profileFor(ro);
                double gain = 0;
                for (size_t j = 1; j < st.ele.size(); ++j) gain += std::max(0.0, st.ele[j] - st.ele[j - 1]);
                std::snprintf(b, sizeof b, "%s  \xC2\xB7  %.0f m  \xC2\xB7  %s", fmtDist(ro.distance(), prof_.metric).c_str(), gain, ro.isLoop ? "loop" : "point to point");
                text(b, row.x + 12, row.y + 26, F::Small, DIM, 12);
                if (h && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { selRoute_ = prefs_.selRoute = ro.id; showRoute(ro); }
            }
            EndScissorMode();
            scrollBar(list, sc, routes_.size() * (rowH + 4));
            y += list.height + 12;
            if (rt && rt->isLoop) {
                if (stepper({x, y, w, 36}, "Laps", std::to_string(setupLaps_), &d)) setupLaps_ = std::clamp(setupLaps_ + d, 1, 20);
                y += 46;
            }
            if (rt) {
                auto best = db_.bestRouteRide(rt->id, rt->isLoop ? setupLaps_ : 1);
                if (best && best->summary.routeTimeS) {
                    if (toggle({x, y, w, 34}, "Race your ghost (PB " + fmtTime(*best->summary.routeTimeS) + ")", raceGhost_)) raceGhost_ = !raceGhost_;
                } else text("No ghost yet - finish this route to set one.", x, y + 6, F::Small, FAINT);
                y += 46;
            }
            if (stepper({x, y, w, 36}, "Trainer difficulty", fmt("%.0f%%", prof_.trainerDifficulty * 100), &d))
                prof_.trainerDifficulty = std::clamp(prof_.trainerDifficulty + d * 0.05, 0.0, 1.5);
            y += 48;
            break;
        }
        case RideKind::Workout: {
            const wk::Workout* wkp = findWorkout(selWorkout_);
            if (!wkp) break;
            text(wkp->name, x, y, F::Title, TEXT, 28);
            y += 36;
            auto e = wkp->estimate(prof_.ftp);
            std::snprintf(b, sizeof b, "%s  \xC2\xB7  TSS %.0f  \xC2\xB7  IF %.2f  \xC2\xB7  %s", fmtTime(e.durationS).c_str(), e.tss, e.intensityFactor, wkp->category.c_str());
            text(b, x, y, F::Body, DIM);
            y += 30;
            y += textWrapped(wkp->description, {x, y, w, 60}, F::Body, DIM, 16) + 8;
            workoutChart({x, y, w, 110}, wkp->steps(), prof_.ftp, 1.0, -1, nullptr);
            y += 124;
            if (button({x, y, 200, 36}, "Choose workout...")) page_ = PWorkouts;
            y += 50;
            if (toggle({x, y, w, 34}, "Resistance mode (you control power)", workoutResistance_)) workoutResistance_ = !workoutResistance_;
            y += 46;
            break;
        }
        case RideKind::Test: {
            for (auto& t : wk::testCatalogue()) {
                Rectangle c{x, y, w, 88};
                bool sel = t.kind == selTest_;
                DrawRectangleRounded(c, 0.15f, 8, sel ? Color{15, 157, 88, 55} : hovered(c) ? Color{238, 241, 245, 255} : CARD2);
                if (sel) DrawRectangleRoundedLinesEx(c, 0.15f, 8, 1.5f, ACCENT);
                text(t.name, c.x + 14, c.y + 8, F::Body, TEXT, 20);
                textRight("~" + std::to_string(t.approxMinutes) + " min", c.x + c.width - 14, c.y + 10, F::Small, DIM);
                textWrapped(t.summary, {c.x + 14, c.y + 34, c.width - 28, 50}, F::Small, DIM, 14);
                if (clicked(c)) selTest_ = t.kind;
                y += 96;
            }
            for (auto& t : wk::testCatalogue())
                if (t.kind == selTest_) y += textWrapped(t.how, {x, y, w, 90}, F::Small, FAINT, 14) + 6;
            break;
        }
        case RideKind::Free: {
            text("Mode", x, y, F::Small, DIM);
            y += 22;
            static const std::vector<std::string> modes = {"Follow the road", "Power (ERG)", "Level", "Heart rate"};
            int m = segmented({x, y, w, 38}, modes, int(setupFreeMode_));
            if (m >= 0) setupFreeMode_ = FreeMode(m);
            y += 50;
            static const char* desc[] = {"SIM mode: the trainer follows the road. Up / Down choose how much the road ahead climbs.",
                                         "ERG: hold fixed watts. Up/Down change the target by 5 W.",
                                         "Resistance level, like a classic trainer. Up/Down change the level.",
                                         "ERG adjusted automatically to hold a target heart rate (needs a strap)."};
            textWrapped(desc[int(setupFreeMode_)], {x, y, w, 40}, F::Small, DIM, 15);
            y += 44;
            text("Goal", x, y, F::Small, DIM);
            y += 22;
            static const std::vector<std::string> goals = {"None", "Time", "Distance", "kJ", "kcal", "Climbing"};
            int g = segmented({x, y, w, 36}, goals, int(setupGoal_.type));
            if (g >= 0) {
                setupGoal_.type = Goal::Type(g);
                static const double defaults[] = {0, 3600, 30000, 600, 600, 500};
                setupGoal_.value = defaults[g];
            }
            y += 48;
            if (setupGoal_.type != Goal::None) {
                static const double steps[] = {0, 300, 5000, 50, 50, 50};
                if (stepper({x, y, w, 36}, "Target", goalLabel(setupGoal_, prof_.metric), &d, 150))
                    setupGoal_.value = std::max(steps[setupGoal_.type], setupGoal_.value + d * steps[setupGoal_.type]);
                y += 48;
            }
            break;
        }
    }
    if (setupKind_ != RideKind::Route) {
        bool endless = setupKind_ == RideKind::Free || sceneryEndless_ || !(rt && rt->isLoop);
        if (setupKind_ != RideKind::Free) {
            text("Scenery", x, y, F::Small, DIM);
            y += 22;
            bool loopOk = rt && rt->isLoop;
            int sv = segmented({x, y, w, 34}, {"Endless road", loopOk ? "Loop: " + rt->name : "Selected loop"}, sceneryEndless_ || !loopOk ? 0 : 1);
            if (sv >= 0) sceneryEndless_ = sv == 0 || !loopOk;
            y += 44;
        }
        if (endless) {
            text("Road to start with", x, y, F::Small, DIM);
            y += 22;
            std::vector<std::string> hs;
            for (int i = 0; i < 5; ++i) hs.push_back(hillinessName(Hilliness(i)));
            int hsel = segmented({x, y, w, 34}, hs, int(hillNext_));
            if (hsel >= 0) hillNext_ = Hilliness(hsel);
            y += 44;
            int dd = 0;
            std::string nv = netNext_ == 0 ? "Level" : std::string(netNext_ > 0 ? "+" : "") + std::to_string(netNext_) + " m / km";
            if (stepper({x, y, w, 34}, "Climbing per km", nv, &dd, 130)) netNext_ = std::clamp(netNext_ + (dd > 0 ? 10 : -10), -120, 150);
            y += 44;
        }
    }
    bool ready = ble_.trainer().state == LinkState::Connected || simActive();
    Rectangle sb{x, L.y + L.height - 92, w, 70};
    if (button(sb, ready ? "START RIDE" : "START  (no trainer - connect one or use the simulator)", true, true, ready ? 24 : 17)) startRide();

    // ---- right: route card
    Rectangle R{L.x + L.width + 20, r.y, r.width - L.width - 20, std::min(360.f, r.height)};
    panel(R, CARD);
    if (rt) {
        x = R.x + 22; y = R.y + 18; w = R.width - 44;
        bool endlessCard = setupKind_ != RideKind::Route && (setupKind_ == RideKind::Free || sceneryEndless_ || !rt->isLoop);
        if (endlessCard) {
            text("Endless road", x, y, F::Title, TEXT, 28);
            y += 44;
            y += textWrapped("A new world every ride: a fresh landscape, rivers and road, built around you as you go. "
                             "While you ride, choose how hilly the road ahead is and how much each kilometre climbs or falls; "
                             "every kilometre not yet built is shaped to match.", {x, y, w, 120}, F::Body, DIM, 16) + 14;
            std::snprintf(b, sizeof b, "Starts %s, %s", hillinessName(hillNext_), netNext_ == 0 ? "level" : (std::string(netNext_ > 0 ? "climbing " : "descending ") + std::to_string(std::abs(netNext_)) + " m per km").c_str());
            DrawRectangleRounded({x, y, measure(b, F::Small).x + 24, 28}, 1, 8, Color{15, 157, 88, 26});
            text(b, x + 12, y + 7, F::Small, ACCENT);
            goto afterCard;
        }
        text(setupKind_ == RideKind::Route ? rt->name : rt->name + " (scenery)", x, y, F::Title, TEXT, 28);
        y += 42;
        auto st = rt->stats();
        std::snprintf(b, sizeof b, "%s %s  \xC2\xB7  %.0f m climbing  \xC2\xB7  max %.1f%%  \xC2\xB7  %s", fmtDist(rt->distance(), prof_.metric).c_str(),
                      rt->isLoop ? "loop" : "point to point", st.gainM, st.maxGradePct, st.difficultyLabel.c_str());
        text(b, x, y, F::Body, DIM, 16);
        y += 30;
        const RouteProfile& rp = profileFor(*rt);
        routeProfile({x, y, w, R.y + R.height - y - 64}, rp, std::fmod(homeDist_, std::max(1.0, rp.length)), -1, prof_.metric);
        y = R.y + R.height - 50;
        float cx = x;
        for (auto& c : st.climbs) {
            std::snprintf(b, sizeof b, "%s  %.1f km @ %.1f%%", c.category == "Uncategorised" ? "Climb" : c.category.c_str(), c.lengthM / 1000, c.avgGradePct);
            float cwid = measure(b, F::Small).x + 20;
            if (cx + cwid > x + w) break;
            DrawRectangleRounded({cx, y, cwid, 26}, 1, 8, Color{234, 237, 242, 255});
            text(b, cx + 10, y + 5, F::Small, TEXT);
            cx += cwid + 8;
        }
    }
afterCard:
    Rectangle T{R.x, R.y + R.height + 16, R.width, r.height - R.height - 16};
    if (T.height > 120) {
        panel(T, CARD);
        float ty = T.y + 16;
        text("While riding", T.x + 22, ty, F::Body, TEXT);
        ty += 30;
        const char* lines[] = {"Space  pause / resume        L  lap        E  edit drawer        R  change the road ahead",
                               "Up / Down  nudge the target        Right  skip step        Backspace  undo road change",
                               "[ ]  virtual gears        C  camera        V  2D / 3D        H  HR alerts        Tab  hide HUD",
                               "W  weather        T  time of day        M  sound        D  simulator data        ?  help",
                               "Drop .gpx files on the window to add routes, .zwo / .erg / .mrc / .txt to add workouts."};
        for (auto* l : lines) {
            if (ty > T.y + T.height - 22) break;
            text(l, T.x + 22, ty, F::Small, DIM, 15);
            ty += 24;
        }
    }
}
