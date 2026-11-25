// Devices (+ simulator test mode) and Profile (rider, heart rate, bike & trainer, training,
// Strava + CoreKick connections, world: location, time of day, weather, graphics, sound).
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "App.h"
#include "Audio.h"
#include "Net.h"
#include "raylib.h"

using namespace ui;

static std::string fmt(const char* f, double v) { char b[96]; std::snprintf(b, sizeof b, f, v); return b; }
static const Color CARD{18, 21, 31, 236};
static const Color CARD2{26, 30, 42, 225};

// ------------------------------------------------------------------ devices
void App::drawDevicesPage(Rectangle r) {
    float lw = std::min(720.f, r.width * 0.55f);
    Rectangle L{r.x, r.y, lw, r.height};
    panel(L, CARD);
    float x = L.x + 22, y = L.y + 16, w = L.width - 44;
    char b[200];
    text("Devices", x, y, F::Title, TEXT);
    textRight(ble_.status(), x + w, y + 12, F::Small, ble_.available() ? DIM : WARN);
    y += 52;
    TrainerInput live = ble_.input();
    for (int k = 0; k < 2; ++k) {
        Link l = k == 0 ? ble_.trainer() : ble_.hrm();
        Rectangle c{x, y, w, 64};
        panel(c, CARD2);
        Color dot = l.state == LinkState::Connected ? GOOD : l.state == LinkState::Disconnected ? FAINT : WARN;
        DrawCircle(int(c.x + 20), int(c.y + 32), 7, dot);
        text(k == 0 ? "Smart trainer" : "Heart rate", c.x + 38, c.y + 8, F::Body, TEXT);
        static const char* st[] = {"not connected", "connecting...", "connected", "reconnecting..."};
        std::snprintf(b, sizeof b, "%s  %s  %s", l.name.c_str(), st[int(l.state)], l.detail.c_str());
        text(b, c.x + 38, c.y + 36, F::Small, DIM, 14);
        if (l.state != LinkState::Disconnected && button({c.x + c.width - 130, c.y + 14, 116, 36}, "Disconnect")) {
            if (k == 0) { ble_.disconnectTrainer(); prefs_.trainerId.clear(); } else { ble_.disconnectHrm(); prefs_.hrId.clear(); }
            ble_.setAutoConnect(prefs_.trainerId, prefs_.hrId);
            store::save(prof_, prefs_);
        }
        y += 74;
    }
    std::snprintf(b, sizeof b, "Live:  %s W   %s rpm   %s bpm", live.connected ? std::to_string(int(live.powerW)).c_str() : "--",
                  live.connected ? std::to_string(int(live.cadenceRpm)).c_str() : "--", live.heartRate ? std::to_string(int(*live.heartRate)).c_str() : "--");
    text(b, x, y, F::Body, TEXT);
    y += 36;
    bool scanning = ble_.scanning();
    if (button({x, y, 200, 40}, scanning ? "Stop scanning" : "Scan for devices", !scanning, ble_.available())) {
        if (scanning) ble_.stopScan(); else ble_.startScan(60);
    }
    if (scanning) text(std::string("Scanning") + std::string(int(GetTime() * 3) % 4, '.'), x + 214, y + 10, F::Body, DIM);
    y += 54;
    auto devs = ble_.devices();
    if (devs.empty())
        textWrapped("Pedal to wake the KICKR, put on (or start broadcasting) your heart-rate strap, then scan. Close Zwift / the Wahoo app first - "
                    "only one app can control the trainer.", {x, y, w, 80}, F::Body, DIM, 16);
    for (auto& d : devs) {
        if (y > L.y + L.height - 140) break;
        Rectangle c{x, y, w, 50};
        DrawRectangleRounded(c, 0.25f, 8, CARD2);
        text(d.name, c.x + 14, c.y + 6, F::Body, TEXT, 19);
        const char* tag = d.kind == DevKind::Trainer ? (d.ftms ? "FTMS trainer" : "Trainer") : d.kind == DevKind::HeartRate ? "Heart rate" : "Power meter";
        std::snprintf(b, sizeof b, "%s  \xC2\xB7  %d dBm", tag, d.rssi);
        text(b, c.x + 14, c.y + 29, F::Small, DIM, 13);
        Link lt = ble_.trainer(), lh = ble_.hrm();
        if (d.kind == DevKind::Trainer) {
            bool on = lt.id == d.id && lt.state == LinkState::Connected;
            if (button({c.x + c.width - 150, c.y + 8, 136, 34}, on ? "Trainer \xE2\x9C\x93" : "Use as trainer", !on)) ble_.connectTrainer(d.id);
        } else if (d.kind == DevKind::HeartRate) {
            bool on = lh.id == d.id && lh.state == LinkState::Connected;
            if (button({c.x + c.width - 150, c.y + 8, 136, 34}, on ? "Heart rate" : "Use as HRM", !on)) ble_.connectHrm(d.id);
        }
        y += 58;
    }
    textWrapped("KICKR CORE: update firmware in the Wahoo app (FTMS control arrived in 2021). COROS / Garmin watches: enable heart-rate broadcast, "
                "and disconnect the watch from your phone first. If the trainer drops out, CoreRay reconnects and re-sends the target.",
                {x, L.y + L.height - 74, w, 70}, F::Small, FAINT, 13);
    Rectangle R{L.x + L.width + 20, r.y, r.width - L.width - 20, r.height};
    panel(R, CARD);
    drawSimPanel({R.x + 20, R.y + 16, R.width - 40, R.height - 32}, false);
}

void App::drawSimPanel(Rectangle r, bool compact) {
    float x = r.x, y = r.y, w = r.width;
    text(compact ? "Test data" : "Simulator", x, y, F::Title, TEXT, compact ? 24 : 34);
    y += compact ? 36 : 48;
    if (!compact) {
        textWrapped("A virtual trainer and rider so you can try everything without the bike. Rides on the simulator never count as real efforts.",
                    {x, y, w, 60}, F::Small, DIM, 15);
        y += 48;
        if (toggle({x, y, w, 34}, "Use the simulator instead of Bluetooth", prefs_.simEnabled)) {
            prefs_.simEnabled = !prefs_.simEnabled;
            store::save(prof_, prefs_);
        }
        y += 50;
    }
    sim::Settings s = sim_->settings();
    bool ch = false;
    int d;
    text("Power", x, y, F::Small, DIM);
    y += 20;
    int ps = segmented({x, y, w, 32}, {"Simulated rider", "Fixed watts"}, int(s.powerSource));
    if (ps >= 0) { s.powerSource = sim::PowerSource(ps); ch = true; }
    y += 40;
    if (s.powerSource == sim::PowerSource::Rider) {
        if (stepper({x, y, w, 30}, "Rider effort", fmt("%.0f W", s.effortW), &d)) { s.effortW += d * 10; ch = true; }
    } else if (stepper({x, y, w, 30}, "Fixed power", fmt("%.0f W", s.powerW), &d)) { s.powerW += d * 10; ch = true; }
    y += 40;
    int pt = segmented({x, y, w, 30}, {"Steady", "Surges", "Wave", "Fade"}, int(s.pattern));
    if (pt >= 0) { s.pattern = sim::Pattern(pt); ch = true; }
    y += 42;
    text("Heart rate", x, y, F::Small, DIM);
    y += 20;
    int hs = segmented({x, y, w, 30}, {"Model", "Fixed", "Off"}, int(s.hrSource));
    if (hs >= 0) { s.hrSource = sim::HrSource(hs); ch = true; }
    y += 38;
    if (s.hrSource == sim::HrSource::Fixed) {
        if (stepper({x, y, w, 30}, "Heart rate", fmt("%.0f bpm", s.hrBpm), &d)) { s.hrBpm += d * 2; ch = true; }
        y += 38;
    }
    int cs = segmented({x, y, w * 0.5f - 6, 30}, {"Auto cadence", "Fixed"}, int(s.cadenceSource));
    if (cs >= 0) { s.cadenceSource = sim::CadenceSource(cs); ch = true; }
    if (s.cadenceSource == sim::CadenceSource::Fixed) {
        int d2;
        if (stepper({x + w * 0.5f + 6, y, w * 0.5f - 6, 30}, "", fmt("%.0f rpm", s.cadenceRpm), &d2, 80)) { s.cadenceRpm += d2 * 5; ch = true; }
    }
    y += 40;
    if (stepper({x, y, w, 30}, "Noise", fmt("%.0f%%", s.variability * 100), &d)) { s.variability += d * 0.1; ch = true; }
    y += 40;
    if (toggle({x, y, w, 30}, "Trainer signal dropout", s.dropout)) { s.dropout = !s.dropout; ch = true; }
    y += 40;
    if (ch) { sim_->set(s); prefs_.sim = sim_->settings(); }
    if (compact) text("Shift+Up/Down power 10 W  \xC2\xB7  Alt+Up/Down HR 2 bpm", x, y, F::Small, FAINT, 13);
}

// ------------------------------------------------------------------ profile
void App::drawProfilePage(Rectangle r) {
    int t = segmented({r.x, r.y, 520, 40}, {"Rider & training", "Connections", "World"}, profileTab_);
    if (t >= 0) profileTab_ = t;
    Rectangle body{r.x, r.y + 56, r.width, r.height - 56};
    float colW = (body.width - 40) / 3;
    int d;
    char b[220];
    bool changed = false;
    auto row = [&](Rectangle c, float& y, const std::string& label, const std::string& val, auto apply) {
        if (stepper({c.x + 20, y, c.width - 40, 34}, label, val, &d, 120)) { apply(d); changed = true; }
        y += 44;
    };
    if (profileTab_ == 0) {
        Rectangle A{body.x, body.y, colW, body.height};
        panel(A, CARD);
        float y = A.y + 16;
        text("Rider", A.x + 20, y, F::Title, TEXT);
        y += 52;
        row(A, y, "FTP", fmt("%.0f W", prof_.ftp), [&](int k) { prof_.ftp = std::clamp(prof_.ftp + k * 5, 60.0, 600.0); });
        row(A, y, "Weight", fmt(prof_.metric ? "%.1f kg" : "%.0f lb", prof_.metric ? prof_.weightKg : prof_.weightKg * 2.20462),
            [&](int k) { prof_.weightKg = std::clamp(prof_.weightKg + k * (prof_.metric ? 0.5 : 0.4536), 30.0, 180.0); });
        row(A, y, "Bike weight", fmt("%.1f kg", prof_.bikeWeightKg), [&](int k) { prof_.bikeWeightKg = std::clamp(prof_.bikeWeightKg + k * 0.5, 4.0, 25.0); });
        row(A, y, "Age", std::to_string(prof_.age), [&](int k) { prof_.age = std::clamp(prof_.age + k, 10, 99); });
        if (toggle({A.x + 20, y, A.width - 40, 34}, prof_.male ? "Sex: male" : "Sex: female", !prof_.male)) { prof_.male = !prof_.male; changed = true; }
        y += 44;
        if (toggle({A.x + 20, y, A.width - 40, 34}, "Metric units", prof_.metric)) { prof_.metric = !prof_.metric; changed = true; }
        y += 52;
        std::snprintf(b, sizeof b, "%.2f W/kg", prof_.ftp / prof_.weightKg);
        text(b, A.x + 20, y, F::Title, ACCENT, 30);
        y += 52;
        text("TRAINING", A.x + 20, y, F::Small, DIM, 13);
        y += 22;
        row(A, y, "Weekly goal", fmt("%.0f h", prof_.weeklyGoalHours), [&](int k) { prof_.weeklyGoalHours = std::clamp(prof_.weeklyGoalHours + k, 1.0, 30.0); });
        text("Automatic FTP from your rides", A.x + 20, y, F::Small, DIM, 13);
        y += 20;
        int fa = segmented({A.x + 20, y, A.width - 40, 32}, {"Suggest", "Raise automatically", "Off"}, int(prof_.ftpAuto));
        if (fa >= 0) { prof_.ftpAuto = FtpAutoMode(fa); changed = true; }
        y += 40;
        if (assessment_ && assessment_->estimate) {
            std::snprintf(b, sizeof b, "eFTP %.0f W (%s, %s confidence)", assessment_->estimate->estimate,
                          assessment_->estimate->method == "critical_power" ? "critical power" : "best 20 min", assessment_->estimate->confidence.c_str());
            text(b, A.x + 20, y, F::Small, TEXT, 14);
            y += 20;
        }
        if (assessment_) textWrapped(assessment_->reason, {A.x + 20, y, A.width - 40, 40}, F::Small, FAINT, 13);

        Rectangle B{A.x + colW + 20, body.y, colW, body.height};
        panel(B, CARD);
        y = B.y + 16;
        text("Heart rate", B.x + 20, y, F::Title, TEXT);
        y += 52;
        row(B, y, "Max HR", fmt("%.0f", prof_.maxHr), [&](int k) { prof_.maxHr = std::clamp(prof_.maxHr + k, 120.0, 230.0); });
        row(B, y, "Resting HR", fmt("%.0f", prof_.restHr), [&](int k) { prof_.restHr = std::clamp(prof_.restHr + k, 30.0, 100.0); });
        text("Zone model", B.x + 20, y, F::Small, DIM);
        y += 20;
        int zm = segmented({B.x + 20, y, B.width - 40, 32}, {"% max", "Reserve", "LTHR"}, int(prof_.hrZoneModel));
        if (zm >= 0) { prof_.hrZoneModel = HrZoneModel(zm); changed = true; }
        y += 42;
        if (prof_.hrZoneModel == HrZoneModel::Lthr)
            row(B, y, "Threshold HR", fmt("%.0f", prof_.lthr > 0 ? prof_.lthr : prof_.maxHr * 0.9),
                [&](int k) { prof_.lthr = std::clamp((prof_.lthr > 0 ? prof_.lthr : prof_.maxHr * 0.9) + k, 100.0, 210.0); });
        auto zones = prof_.hrZones();
        for (auto& z : zones) {
            std::snprintf(b, sizeof b, "Z%d %s", z.zone, z.name.c_str());
            DrawRectangleRounded({B.x + 20, y + 4, 8, 16}, 1, 4, hrZoneColor(z.zone, int(zones.size())));
            text(b, B.x + 36, y, F::Small, TEXT, 15);
            std::snprintf(b, sizeof b, z.hi ? "%.0f - %.0f bpm" : "%.0f+ bpm", z.lo, z.hi.value_or(0));
            textRight(b, B.x + B.width - 20, y, F::Small, DIM, 15);
            y += 22;
        }
        y += 12;
        if (toggle({B.x + 20, y, B.width - 40, 34}, "Heart-rate alerts", prof_.hrAlerts)) { prof_.hrAlerts = !prof_.hrAlerts; hrAlertsOn_ = prof_.hrAlerts; changed = true; }
        y += 44;
        int nz = int(zones.size());
        row(B, y, "Stay above zone", "Z" + std::to_string(prof_.hrAlertLo), [&](int k) { prof_.hrAlertLo = std::clamp(prof_.hrAlertLo + k, 1, prof_.hrAlertHi); });
        row(B, y, "Stay below zone", "Z" + std::to_string(prof_.hrAlertHi), [&](int k) { prof_.hrAlertHi = std::clamp(prof_.hrAlertHi + k, prof_.hrAlertLo, nz); });
        row(B, y, "Warm-up grace", fmtTime(prof_.hrAlertGraceS), [&](int k) { prof_.hrAlertGraceS = std::clamp(prof_.hrAlertGraceS + k * 60, 0.0, 1800.0); });

        Rectangle C{B.x + colW + 20, body.y, colW, body.height};
        panel(C, CARD);
        y = C.y + 16;
        text("Bike & trainer", C.x + 20, y, F::Title, TEXT);
        y += 52;
        text("Position (CdA)", C.x + 20, y, F::Small, DIM);
        y += 20;
        static const double cdas[] = {0.40, 0.32, 0.28, 0.24};
        int ci = 1;
        for (int i = 0; i < 4; ++i) if (std::fabs(prof_.cda - cdas[i]) < 0.005) ci = i;
        int cn = segmented({C.x + 20, y, C.width - 40, 32}, {"Upright", "Hoods", "Drops", "TT"}, ci);
        if (cn >= 0) { prof_.cda = cdas[cn]; changed = true; }
        y += 42;
        text("Road surface (Crr)", C.x + 20, y, F::Small, DIM);
        y += 20;
        static const double crrs[] = {0.003, 0.004, 0.006, 0.008};
        int ri = 1;
        for (int i = 0; i < 4; ++i) if (std::fabs(prof_.crr - crrs[i]) < 0.0005) ri = i;
        int rn = segmented({C.x + 20, y, C.width - 40, 32}, {"Smooth", "Asphalt", "Rough", "Gravel"}, ri);
        if (rn >= 0) { prof_.crr = crrs[rn]; changed = true; }
        y += 44;
        row(C, y, "Trainer difficulty", fmt("%.0f%%", prof_.trainerDifficulty * 100), [&](int k) { prof_.trainerDifficulty = std::clamp(prof_.trainerDifficulty + k * 0.05, 0.0, 1.5); });
        row(C, y, "Max trainer grade", fmt("%.0f%%", prof_.maxTrainerGrade), [&](int k) { prof_.maxTrainerGrade = std::clamp(prof_.maxTrainerGrade + k, 2.0, 25.0); });
        row(C, y, "Min trainer grade", fmt("%.0f%%", prof_.minTrainerGrade), [&](int k) { prof_.minTrainerGrade = std::clamp(prof_.minTrainerGrade + k, -20.0, 0.0); });
        row(C, y, "Gross efficiency", fmt("%.0f%%", prof_.grossEfficiency * 100), [&](int k) { prof_.grossEfficiency = std::clamp(prof_.grossEfficiency + k * 0.01, 0.15, 0.30); });
        if (toggle({C.x + 20, y, C.width - 40, 34}, "Virtual shifting (Zwift Cog)", prof_.virtualShifting)) { prof_.virtualShifting = !prof_.virtualShifting; changed = true; }
        y += 44;
        if (prof_.virtualShifting) {
            row(C, y, "Start gear", std::to_string(prof_.startGear) + " / 24", [&](int k) { prof_.startGear = std::clamp(prof_.startGear + k, 1, 24); });
            row(C, y, "Wheel circumference", fmt("%.0f mm", prof_.wheelCircM * 1000), [&](int k) { prof_.wheelCircM = std::clamp(prof_.wheelCircM + k * 0.005, 1.9, 2.4); });
        }
        phys::Params pp;
        pp.massKg = prof_.weightKg + prof_.bikeWeightKg; pp.cda = prof_.cda; pp.crr = prof_.crr;
        std::snprintf(b, sizeof b, "250 W = %s %s on the flat, %s up 6%%", fmtSpeed(phys::steadySpeed(250, 0, 0, pp), prof_.metric).c_str(),
                      prof_.metric ? "km/h" : "mph", fmtSpeed(phys::steadySpeed(250, 0.06, 0, pp), prof_.metric).c_str());
        textWrapped(b, {C.x + 20, std::min(y, C.y + C.height - 50), C.width - 40, 40}, F::Body, TEXT, 16);
    } else if (profileTab_ == 1) {
        // ---- Strava
        float half = (body.width - 20) / 2;
        Rectangle A{body.x, body.y, half, body.height};
        panel(A, CARD);
        float x = A.x + 22, y = A.y + 16, w = A.width - 44;
        text("Strava", x, y, F::Title, TEXT);
        auto st = strava::status(db_);
        textRight(st.connected ? "Connected as " + st.athleteName : st.configured ? "Not connected" : "Not set up", x + w, y + 12, F::Body, st.connected ? GOOD : DIM, 16);
        y += 54;
        y += textWrapped("CoreRay uploads rides straight to Strava as Virtual Rides. Strava needs you to create your own (free) API app once:", {x, y, w, 60}, F::Body,
                         DIM, 16) + 8;
        const char* steps[] = {"1.  Open strava.com/settings/api and create an app (any name and website).",
                               "2.  Set \xE2\x80\x9C" "Authorization Callback Domain\xE2\x80\x9D to  127.0.0.1",
                               "3.  Paste the Client ID and Client Secret below, then Connect."};
        for (auto* s : steps) { text(s, x, y, F::Small, TEXT, 14); y += 22; }
        if (button({x, y + 4, 230, 34}, "Open strava.com/settings/api", false, true, 14)) OpenURL("https://www.strava.com/settings/api");
        y += 52;
        text("Client ID", x, y, F::Small, DIM);
        textBox({x, y + 20, 200, 38}, stravaId_, false, "12345");
        text("Client Secret", x + 216, y, F::Small, DIM);
        textBox({x + 216, y + 20, w - 216, 38}, stravaSecret_, false, st.configured ? "(saved - paste to replace)" : "40 hex characters");
        y += 72;
        if (button({x, y, 140, 40}, "Save keys", false, !stravaId_.text.empty() && !stravaSecret_.text.empty())) {
            std::string err;
            if (strava::saveClient(db_, stravaId_.text, stravaSecret_.text, err)) { toast("Strava keys saved", GOOD); stravaSecret_.text.clear(); }
            else toast(err, BAD);
        }
        static bool connecting = false;
        if (button({x + 152, y, 200, 40}, connecting ? "Waiting for browser..." : st.connected ? "Reconnect" : "Connect to Strava", !st.connected, st.configured && !connecting)) {
            connecting = true;
            toast("Approve CoreRay in your browser", SKY);
            async([this] {
                std::string err;
                bool ok = strava::connect(db_, err);
                onMain([this, ok, err] {
                    connecting = false;
                    if (ok) toast("Connected to Strava as " + strava::status(db_).athleteName, GOOD);
                    else toast(err, BAD);
                });
            });
        }
        if (st.connected && button({x + 364, y, 140, 40}, "Disconnect")) {
            async([this] { strava::disconnect(db_); onMain([this] { toast("Disconnected from Strava"); }); });
        }
        y += 56;
        if (toggle({x, y, w, 34}, "Upload every ride automatically (not simulator rides)", st.autoUpload)) strava::setAuto(db_, !st.autoUpload);
        y += 50;
        int uploaded = 0;
        for (auto& rd : rides_) if (rd.stravaActivityId) ++uploaded;
        std::snprintf(b, sizeof b, "%d of %zu rides are on Strava. Upload older ones from Activities.", uploaded, rides_.size());
        text(b, x, y, F::Small, FAINT, 14);

        // ---- CoreKick
        Rectangle B{body.x + half + 20, body.y, half, body.height};
        panel(B, CARD);
        x = B.x + 22; y = B.y + 16; w = B.width - 44;
        text("CoreKick", x, y, F::Title, TEXT);
        y += 54;
        bool have = store::coreKickAvailable();
        y += textWrapped(have ? "Bring everything across from CoreKick: your profile, custom workouts, every ride (with full 1-second data), routes, events, "
                                "FTP history and Strava keys. Rides already here are kept; running it again only adds what's new."
                              : "CoreKick's database wasn't found on this computer (" + store::coreKickDbPath() + ").",
                         {x, y, w, 100}, F::Body, DIM, 16) + 14;
        if (have && button({x, y, 300, 46}, "Import from CoreKick", true)) {
            std::vector<wk::Workout> imp;
            std::string m1, m2;
            store::importCoreKick(prof_, imp, m1);
            custom_ = store::loadCustomWorkouts();
            db_.importCoreKick(store::coreKickDbPath(), m2);
            loadData();
            reassessFtp(std::nullopt, false);
            importMsg_ = m1 + "\n" + m2;
            changed = true;
            toast("CoreKick import finished", GOOD);
        }
        y += 60;
        if (!importMsg_.empty()) textWrapped(importMsg_, {x, y, w, 80}, F::Small, TEXT, 14);
        y += 90;
        text("Your data", x, y, F::Small, DIM, 13);
        y += 22;
        text(Db::defaultPath(), x, y, F::Small, FAINT, 13);
        y += 20;
        text(store::dir() + "  (profile, workouts)", x, y, F::Small, FAINT, 13);
    } else {
        // ---- world
        Rectangle A{body.x, body.y, colW, body.height};
        panel(A, CARD);
        float x = A.x + 20, y = A.y + 16, w = A.width - 40;
        text("Location", x, y, F::Title, TEXT);
        y += 50;
        y += textWrapped("Used for the real sun and moon (and the moon's phase), real-time mode and live weather.", {x, y, w, 50}, F::Small, DIM, 14) + 8;
        textBox({x, y, w - 100, 38}, placeBox_, false, "Town or city");
        static bool searching = false;
        if (button({x + w - 90, y, 90, 38}, searching ? "..." : "Search", true, !placeBox_.text.empty() && !searching)) {
            searching = true;
            std::string q = placeBox_.text;
            async([this, q] {
                std::string err;
                auto res = liveweather::search(q, err);
                onMain([this, res, err] {
                    searching = false;
                    placeResults_ = res;
                    if (res.empty()) toast(err.empty() ? "No places found" : err, WARN);
                });
            });
        }
        y += 48;
        for (auto& p : placeResults_) {
            if (y > A.y + A.height - 120) break;
            Rectangle rr{x, y, w, 34};
            bool h = hovered(rr);
            DrawRectangleRounded(rr, 0.3f, 6, h ? Color{40, 46, 64, 230} : CARD2);
            text(p.name, rr.x + 10, rr.y + 8, F::Small, TEXT, 14);
            if (h && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
                prefs_.place = p.name; prefs_.lat = p.lat; prefs_.lon = p.lon;
                placeBox_.text = p.name;
                placeResults_.clear();
                live_.reset();
                if (prefs_.weatherMode == 1) fetchLiveWeather();
                changed = true;
            }
            y += 40;
        }
        std::snprintf(b, sizeof b, "%s  (%.2f, %.2f)", prefs_.place.c_str(), prefs_.lat, prefs_.lon);
        textWrapped(b, {x, y + 6, w, 40}, F::Body, TEXT, 15);
        y += 50;
        auto moon = sky::moonPhase(std::time(nullptr));
        const char* phase = moon.illumination < 0.03 ? "New moon" : moon.illumination > 0.97 ? "Full moon" : moon.illumination > 0.45 && moon.illumination < 0.55 ? (moon.waxing ? "First quarter" : "Last quarter")
                            : moon.illumination < 0.5 ? (moon.waxing ? "Waxing crescent" : "Waning crescent") : (moon.waxing ? "Waxing gibbous" : "Waning gibbous");
        std::snprintf(b, sizeof b, "Moon tonight: %s, %.0f%% lit", phase, moon.illumination * 100);
        text(b, x, y, F::Small, DIM, 14);

        Rectangle B{A.x + colW + 20, body.y, colW, body.height};
        panel(B, CARD);
        x = B.x + 20; y = B.y + 16; w = B.width - 40;
        text("Time & weather", x, y, F::Title, TEXT);
        y += 52;
        text("Time of day", x, y, F::Small, DIM);
        y += 20;
        int tm = segmented({x, y, w, 34}, {"Real time", "Fixed", "Day/night cycle"}, prefs_.todMode);
        if (tm >= 0) { prefs_.todMode = tm; cycleHours_ = 0; changed = true; }
        y += 44;
        if (prefs_.todMode != 0) {
            double h = prefs_.todHour;
            std::snprintf(b, sizeof b, "%02d:%02d", int(h), int(std::fmod(h, 1.0) * 60 + 0.5) % 60);
            row(B, y, prefs_.todMode == 1 ? "Time" : "Starts at", b, [&](int k) { prefs_.todHour = std::fmod(prefs_.todHour + k * 0.5 + 24, 24.0); cycleHours_ = 0; });
        }
        if (prefs_.todMode == 2)
            row(B, y, "A full day takes", fmt("%.0f min", prefs_.cycleMinutes), [&](int k) { prefs_.cycleMinutes = std::clamp(prefs_.cycleMinutes + k * 5, 5.0, 240.0); });
        std::snprintf(b, sizeof b, "Sun %.0f\xC2\xB0 above the horizon", sky_.sunElevation);
        if (sky_.sunElevation < 0) std::snprintf(b, sizeof b, "Sun %.0f\xC2\xB0 below the horizon - lanterns light the road", -sky_.sunElevation);
        text(b, x, y, F::Small, FAINT, 13);
        y += 30;
        text("Weather", x, y, F::Small, DIM);
        y += 20;
        int wm = segmented({x, y, w, 34}, {"Changing", "Live", "Fixed"}, prefs_.weatherMode);
        if (wm >= 0) { prefs_.weatherMode = wm; if (wm == 1) fetchLiveWeather(); changed = true; }
        y += 44;
        if (prefs_.weatherMode == 2) {
            auto k = weather::kindFromId(prefs_.weatherKind).value_or(weather::Kind::Fair);
            if (stepper({x, y, w, 34}, "Conditions", weather::kindLabel(k), &d, 150)) {
                int n = (int(k) + d + int(weather::Kind::COUNT)) % int(weather::Kind::COUNT);
                prefs_.weatherKind = weather::kindId(weather::Kind(n));
                changed = true;
            }
            y += 44;
        } else if (prefs_.weatherMode == 1) {
            if (live_) std::snprintf(b, sizeof b, "%s, %.0f\xC2\xB0" "C, wind %.0f km/h  (%s)", weather::kindLabel(live_->kind), live_->temperatureC, live_->windKmh, live_->place.c_str());
            else std::snprintf(b, sizeof b, "%s", liveFetching_ ? "Fetching the weather..." : "No live weather yet");
            textWrapped(b, {x, y, w, 40}, F::Small, TEXT, 14);
            y += 40;
        } else {
            textWrapped("Each ride gets its own weather that drifts through clear spells, cloud, showers and the odd storm.", {x, y, w, 40}, F::Small, FAINT, 13);
            y += 44;
        }
        std::snprintf(b, sizeof b, "Now: %s  \xC2\xB7  road %s", weather::kindLabel(wx_.currentKind(wxElapsed_)), wx_.snowCover > 0.3 ? "snowy" : wx_.wet > 0.3 ? "wet" : "dry");
        text(b, x, y, F::Small, DIM, 13);

        Rectangle C{B.x + colW + 20, body.y, colW, body.height};
        panel(C, CARD);
        x = C.x + 20; y = C.y + 16; w = C.width - 40;
        text("World & sound", x, y, F::Title, TEXT);
        y += 52;
        text("Graphics", x, y, F::Small, DIM);
        y += 20;
        int q = segmented({x, y, w, 32}, {"Battery", "Balanced", "High"}, prefs_.quality);
        if (q >= 0 && q != prefs_.quality) { prefs_.quality = q; renderer_.setQuality(q); changed = true; }
        y += 46;
        if (toggle({x, y, w, 34}, "Wildlife (sheep, cows, horses, deer, birds)", prefs_.wildlife)) { prefs_.wildlife = !prefs_.wildlife; changed = true; }
        y += 44;
        if (toggle({x, y, w, 34}, "Lanterns light the road at night", prefs_.lanterns)) { prefs_.lanterns = !prefs_.lanterns; changed = true; }
        y += 44;
        if (toggle({x, y, w, 34}, "Sound (ambience, chimes, alerts)", prefs_.sound)) { prefs_.sound = !prefs_.sound; sfx::setMuted(!prefs_.sound); changed = true; }
        y += 44;
        if (toggle({x, y, w, 34}, "Start rides in 3D", prefs_.startIn3d)) { prefs_.startIn3d = !prefs_.startIn3d; changed = true; }
        y += 54;
        const char* keys[] = {"W  cycle the weather", "T  cycle the time of day", "M  sound on / off", "C  camera   V  2D / 3D"};
        for (auto* k : keys) { text(k, x, y, F::Small, FAINT, 14); y += 22; }
    }
    if (changed) store::save(prof_, prefs_);
}
