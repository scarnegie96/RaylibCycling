// Activities (ride history + ride detail) and Stats (totals, fitness/fatigue/form, power curve,
// zones, records, FTP history).
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "App.h"
#include "Net.h"
#include "core/Export.h"
#include "core/Metrics.h"
#include "raylib.h"

using namespace ui;

static std::string fmt(const char* f, double v) { char b[96]; std::snprintf(b, sizeof b, f, v); return b; }
static const Color CARD{18, 21, 31, 236};
static const Color CARD2{26, 30, 42, 225};
static const Color SUB{20, 24, 34, 220};

static std::string durLabel(int s) {
    if (s < 60) return std::to_string(s) + "s";
    if (s < 3600) return std::to_string(s / 60) + "m";
    return fmt("%.1fh", s / 3600.0);
}

// ------------------------------------------------------------------ shared ride detail (Activities + post-ride summary)
void App::drawRideDetail(Rectangle r, RideRecord& rec, bool fresh) {
    RideSummary& s = rec.summary;
    const Samples& sm = rec.samples;
    const bool metric = prof_.metric;
    char b[260];
    float x = r.x, y = r.y, w = r.width;
    // header
    auto d = dates::localDate(s.startedAt);
    auto t = dates::parseRfc3339(s.startedAt);
    std::string when;
    if (d && t) {
        std::tm lt{};
        std::time_t tt = *t;
        localtime_r(&tt, &lt);
        std::snprintf(b, sizeof b, "%s %d %s %d, %02d:%02d", dates::weekdayShort(d->weekday()), d->d, dates::monthName(d->m), d->y, lt.tm_hour, lt.tm_min);
        when = b;
    }
    static std::string renaming;
    if (renaming == s.id) {
        textBox({x, y, std::min(520.f, w * 0.45f), 40}, renameBox_, false);
        if (button({x + std::min(520.f, w * 0.45f) + 10, y + 2, 90, 36}, "Save", true) || (IsKeyPressed(KEY_ENTER) && !renameBox_.text.empty())) {
            s.name = renameBox_.text;
            db_.updateSummary(s);
            refreshHistory();
            renaming.clear();
        }
    } else {
        text(s.name, x, y, F::Title, TEXT, 30);
        float nw = measure(s.name, F::Title, 30).x;
        if (button({x + nw + 14, y + 4, 34, 30}, "\xE2\x9C\x8E", false, true, 16)) { renaming = s.id; renameBox_.text = s.name; renameBox_.active = true; }
    }
    std::string sub = when + "  \xC2\xB7  " + rideKindName(s.kind);
    if (s.routeName && s.kind != RideKind::Route) sub += "  \xC2\xB7  " + *s.routeName;
    if (s.routeModified) sub += "  \xC2\xB7  road edited";
    if (s.simulated) sub += "  \xC2\xB7  simulated";
    text(sub, x, y + 42, F::Body, DIM, 16);
    if (s.personalBest || (fresh && newPb_)) textRight("\xE2\x98\x85 Personal best on this route", x + w, y + 10, F::Body, GOOD, 18);
    else if (s.kind == RideKind::Route && s.completed && s.routeTimeS) textRight("Route time " + fmtTime(*s.routeTimeS), x + w, y + 10, F::Body, TEXT, 18);
    y += 74;

    // metric grid
    struct Item { const char* l; std::string v; };
    std::vector<Item> items = {
        {"TIME", fmtTime(s.movingS)},
        {"DISTANCE", fmtDist(s.distanceM, metric)},
        {"CLIMBING", fmt(metric ? "%.0f m" : "%.0f ft", metric ? s.elevationGainM : s.elevationGainM * 3.28084)},
        {"AVG SPEED", fmtSpeed(s.avgSpeedMps, metric) + (metric ? " km/h" : " mph")},
        {"AVG POWER", fmt("%.0f W", s.avgPower)},
        {"NORMALIZED", fmt("%.0f W", s.np)},
        {"MAX POWER", fmt("%.0f W", s.maxPower)},
        {"IF / VI", fmt("%.2f", s.intensityFactor) + " / " + fmt("%.2f", s.variabilityIndex)},
        {"TSS", fmt("%.0f", s.tss)},
        {"WORK", fmt("%.0f kJ", s.workKj)},
        {"CALORIES", fmt("%.0f kcal", s.caloriesHrKcal.value_or(s.caloriesKcal))},
        {"AVG / MAX HR", s.avgHr ? fmt("%.0f", *s.avgHr) + " / " + fmt("%.0f", s.maxHr.value_or(0)) : "--"},
        {"AVG CADENCE", fmt("%.0f rpm", s.avgCadence)},
        {"W/KG (NP)", fmt("%.2f", s.np / std::max(30.0, s.weightKg > 0 ? s.weightKg : prof_.weightKg))},
    };
    int cols = 7;
    float cw = (w - (cols - 1) * 10) / cols, ch = 60;
    for (size_t i = 0; i < items.size(); ++i) {
        float ix = x + (i % cols) * (cw + 10), iy = y + (i / cols) * (ch + 10);
        panel({ix, iy, cw, ch}, CARD2);
        text(items[i].l, ix + 12, iy + 6, F::Small, DIM, 12);
        text(items[i].v, ix + 12, iy + 24, F::Title, TEXT, std::min(26.f, cw / std::max(4.f, float(items[i].v.size())) * 1.7f));
    }
    y += 2 * (ch + 10) + 4;

    if (s.testResult) {
        Rectangle tr{x, y, w, 54};
        panel(tr, Color{255, 92, 56, 40});
        std::string msg = std::string(s.testResult->valid ? "" : "(incomplete) ") + s.testResult->detail + "  \xE2\x86\x92  FTP " + fmt("%.0f W", s.testResult->ftp);
        text(msg, tr.x + 16, tr.y + 15, F::Body, TEXT, 19);
        if (std::fabs(prof_.ftp - s.testResult->ftp) > 0.5 && s.testResult->ftp > 0) {
            if (button({tr.x + tr.width - 220, tr.y + 7, 204, 40}, "Set FTP to " + fmt("%.0f W", s.testResult->ftp), true)) {
                applyFtp(s.testResult->ftp, "Test");
                ftpApplied_ = true;
                toast("FTP updated", GOOD);
            }
        } else textRight("Your FTP", tr.x + tr.width - 16, tr.y + 17, F::Body, GOOD);
        y += 64;
    }
    // fresh ride: eFTP suggestion triggered by this ride
    if (fresh && assessment_ && assessment_->triggerRideId && *assessment_->triggerRideId == s.id && !assessment_->dismissed && assessment_->suggestedFtp &&
        prof_.ftpAuto != FtpAutoMode::Off && (assessment_->suggestion == Suggestion::Increase || assessment_->suggestion == Suggestion::Decrease)) {
        Rectangle tr{x, y, w, 50};
        panel(tr, Color{74, 222, 128, 36});
        std::snprintf(b, sizeof b, "This ride suggests an FTP of %.0f W (%+.0f W).  %s", *assessment_->suggestedFtp, *assessment_->suggestedFtp - prof_.ftp, assessment_->reason.c_str());
        text(b, tr.x + 16, tr.y + 15, F::Body, TEXT, 16);
        if (button({tr.x + tr.width - 220, tr.y + 6, 204, 38}, "Set FTP to " + fmt("%.0f W", *assessment_->suggestedFtp), true)) {
            applyFtp(*assessment_->suggestedFtp, "Estimate");
            toast("FTP updated", GOOD);
        }
        y += 60;
    } else if (fresh && assessment_ && assessment_->autoApplied && assessment_->triggerRideId && *assessment_->triggerRideId == s.id) {
        text("FTP raised automatically to " + fmt("%.0f W", prof_.ftp) + " from this ride.", x, y + 4, F::Body, GOOD, 17);
        y += 32;
    }

    float bottomH = 60;
    float avail = r.y + r.height - y - bottomH;
    float leftW = w * 0.62f;
    float chH = avail * 0.58f;
    Rectangle pc{x, y, leftW, chH};
    panel(pc, SUB);
    text("POWER  \xC2\xB7  HEART RATE  \xC2\xB7  CADENCE", pc.x + 12, pc.y + 6, F::Small, DIM, 12);
    if (sm.size() > 1) {
        Samples cs = sm;
        cs.target.assign(cs.power.size(), 0.f);
        powerChart({pc.x + 12, pc.y + 24, pc.width - 24, pc.height - 32}, cs, s.ftp > 0 ? s.ftp : prof_.ftp, prof_.maxHr, std::max(60, int(sm.size())), true, false);
    }
    float y2 = y + chH + 10, h2 = avail - chH - 10;
    Rectangle ec{x, y2, leftW / 2 - 5, h2};
    panel(ec, SUB);
    text("ELEVATION", ec.x + 12, ec.y + 6, F::Small, DIM, 12);
    if (!sm.ele.empty()) {
        float lo = *std::min_element(sm.ele.begin(), sm.ele.end()), hi = *std::max_element(sm.ele.begin(), sm.ele.end());
        seriesChart({ec.x + 12, ec.y + 24, ec.width - 24, ec.height - 32}, sm.ele, Color{180, 190, 210, 255}, lo - 5, std::max(hi, lo + 20) + 5, true);
    }
    Rectangle sc{x + leftW / 2 + 5, y2, leftW / 2 - 5, h2};
    panel(sc, SUB);
    text("SPEED", sc.x + 12, sc.y + 6, F::Small, DIM, 12);
    if (!sm.speed.empty()) {
        float hi = *std::max_element(sm.speed.begin(), sm.speed.end());
        seriesChart({sc.x + 12, sc.y + 24, sc.width - 24, sc.height - 32}, sm.speed, SKY, 0, std::max(hi, 5.f) * 1.1f, true);
    }
    float rx = x + leftW + 14, rw = w - leftW - 14, ry = y;
    Rectangle zr{rx, ry, rw, avail * 0.34f};
    panel(zr, SUB);
    text("TIME IN POWER ZONES", zr.x + 12, zr.y + 6, F::Small, DIM, 12);
    {
        std::vector<std::string> zl;
        std::vector<Color> zc;
        for (int z = 0; z < 7; ++z) { zl.push_back("Z" + std::to_string(z + 1) + " " + metrics::ZONE_NAMES[z]); zc.push_back(zoneColor(z)); }
        zoneBars({zr.x + 12, zr.y + 24, zr.width - 24, zr.height - 30}, s.zoneS.data(), 7, zl, zc);
    }
    ry += zr.height + 10;
    if (!s.hrZoneS.empty() && s.avgHr) {
        Rectangle hz{rx, ry, rw, avail * 0.22f};
        panel(hz, SUB);
        text("TIME IN HEART-RATE ZONES", hz.x + 12, hz.y + 6, F::Small, DIM, 12);
        std::vector<std::string> zl;
        std::vector<Color> zc;
        auto zones = prof_.hrZones();
        for (auto& z : zones) { zl.push_back("Z" + std::to_string(z.zone) + " " + z.name); zc.push_back(hrZoneColor(z.zone, int(zones.size()))); }
        zoneBars({hz.x + 12, hz.y + 24, hz.width - 24, hz.height - 30}, s.hrZoneS.data(), std::min<int>(int(s.hrZoneS.size()), int(zones.size())), zl, zc);
        ry += hz.height + 10;
    }
    Rectangle br{rx, ry, rw, y + avail - ry};
    panel(br, SUB);
    text("POWER BESTS", br.x + 12, br.y + 6, F::Small, DIM, 12);
    float bx = br.x + 12, by = br.y + 26;
    for (auto& [dd, wv] : s.powerBests) {
        if (bx + 80 > br.x + br.width) { bx = br.x + 12; by += 42; }
        if (by + 38 > br.y + br.height) break;
        text(durLabel(dd), bx, by, F::Small, DIM, 12);
        text(fmt("%.0f W", wv), bx, by + 14, F::Body, TEXT, 17);
        bx += 80;
    }
    if (!s.splits.empty() && by + 70 < br.y + br.height) {
        by += 48;
        text("SPLITS", br.x + 12, by, F::Small, DIM, 12);
        by += 18;
        for (auto& sp : s.splits) {
            if (by + 20 > br.y + br.height) break;
            std::snprintf(b, sizeof b, "%-14s %8s  %7s  %4.0f W  NP %3.0f%s", sp.name.c_str(), fmtTime(sp.durationS).c_str(), fmtDist(sp.distanceM, metric).c_str(), sp.avgPower,
                          sp.np, sp.avgHr ? fmt("  %3.0f bpm", *sp.avgHr).c_str() : "");
            text(b, br.x + 12, by, F::Small, sp.kind == "sector" ? WARN : TEXT, 13);
            by += 19;
        }
    }

    // ---- actions
    float fy = r.y + r.height - bottomH + 10;
    float ax = x;
    auto st = strava::status(db_);
    std::string ust = uploadState_.count(s.id) ? uploadState_[s.id] : "";
    if (s.stravaActivityId) {
        if (button({ax, fy, 190, 44}, "View on Strava")) OpenURL(("https://www.strava.com/activities/" + std::to_string(*s.stravaActivityId)).c_str());
    } else if (ust == "uploading" || ust == "processing") {
        button({ax, fy, 190, 44}, ust == "uploading" ? "Uploading..." : "Strava processing...", false, false);
    } else if (st.connected) {
        if (button({ax, fy, 190, 44}, "Upload to Strava")) stravaUpload(s.id, false);
    } else if (button({ax, fy, 190, 44}, "Connect Strava...")) { page_ = PProfile; profileTab_ = 1; if (screen_ == Screen::Summary) screen_ = Screen::Home; }
    ax += 202;
    if (button({ax, fy, 150, 44}, "Export files")) {
        std::time_t start = dates::parseRfc3339(s.startedAt).value_or(std::time(nullptr));
        std::string dir = exporter::saveRide(s, sm, start, s.name);
        savedDir_ = dir;
        toast("Saved FIT / TCX / CSV to " + dir, GOOD);
    }
    ax += 162;
    if (!savedDir_.empty() && button({ax, fy, 150, 44}, "Open folder")) OpenURL(("file://" + savedDir_).c_str());
    if (!savedDir_.empty()) ax += 162;
    if (!fresh) {
        if (button({ax, fy, 150, 44}, deleteArm_ ? "Really delete?" : "Delete")) {
            if (deleteArm_) {
                db_.deleteRide(s.id);
                refreshHistory();
                detail_.reset();
                deleteArm_ = false;
                toast("Ride deleted");
                return;
            }
            deleteArm_ = true;
        }
        ax += 162;
        if (s.kind == RideKind::Route && s.routeId && findRoute(*s.routeId) && button({ax, fy, 150, 44}, "Ride again")) {
            selRoute_ = *s.routeId;
            setupKind_ = RideKind::Route;
            setupLaps_ = s.laps;
            page_ = PRide;
        }
    }
    if (!ust.empty() && ust != "uploading" && ust != "processing" && ust != "done") text(ust, x, fy - 22, F::Small, BAD, 13);
}

// ------------------------------------------------------------------ activities
void App::drawActivitiesPage(Rectangle r) {
    if (detail_) {
        panel(r, CARD);
        Rectangle inner{r.x + 22, r.y + 62, r.width - 44, r.height - 74};
        if (button({r.x + 22, r.y + 14, 120, 36}, "< All rides")) { detail_.reset(); deleteArm_ = false; return; }
        // notes
        static std::string notesFor;
        if (notesFor != detail_->summary.id) { notesFor = detail_->summary.id; notesBox_.text = detail_->summary.notes; }
        textBox({r.x + 160, r.y + 14, r.width - 182 - 120, 36}, notesBox_, false, "Notes (how did it feel?)");
        if (notesBox_.text != detail_->summary.notes && button({r.x + r.width - 132, r.y + 14, 110, 36}, "Save notes", true)) {
            detail_->summary.notes = notesBox_.text;
            db_.updateSummary(detail_->summary);
            toast("Notes saved", GOOD);
        }
        drawRideDetail(inner, *detail_, false);
        return;
    }
    panel(r, CARD);
    float x = r.x + 22, y = r.y + 18, w = r.width - 44;
    text("Activities", x, y, F::Title, TEXT);
    char b[220];
    std::snprintf(b, sizeof b, "%d rides  \xC2\xB7  %s  \xC2\xB7  %.0f h  \xC2\xB7  %.0f m climbed", stats_.allTime.rides, fmtDist(stats_.allTime.distanceM, prof_.metric).c_str(),
                  stats_.allTime.movingS / 3600, stats_.allTime.elevationM);
    textRight(b, x + w, y + 10, F::Body, DIM, 16);
    y += 52;
    // build rows: month headers + rides
    struct Row { bool header; std::string label; const RideSummary* ride; double hours, km; };
    std::vector<Row> rows;
    std::string curMonth;
    for (auto& rd : rides_) {
        auto d = dates::localDate(rd.startedAt);
        std::string mk = d ? std::string(dates::monthName(d->m)) + " " + std::to_string(d->y) : "Unknown";
        if (mk != curMonth) { rows.push_back({true, mk, nullptr, 0, 0}); curMonth = mk; }
        rows.back().hours += 0;
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) if (it->header) { it->hours += rd.movingS / 3600; it->km += rd.distanceM / 1000; break; }
        rows.push_back({false, "", &rd, 0, 0});
    }
    Rectangle lr{x, y, w, r.y + r.height - y - 14};
    float content = 0;
    for (auto& rw : rows) content += rw.header ? 40 : 58;
    scrollUpdate(lr, actScroll_, content);
    BeginScissorMode(int(lr.x), int(lr.y), int(lr.width), int(lr.height));
    float yy = lr.y - actScroll_;
    if (rows.empty()) textWrapped("No rides yet. Rides you finish (and anything imported from CoreKick in Profile) appear here.", {x, y, w, 60}, F::Body, DIM, 17);
    for (auto& rw : rows) {
        float h = rw.header ? 40 : 58;
        if (yy + h < lr.y || yy > lr.y + lr.height) { yy += h; continue; }
        if (rw.header) {
            text(rw.label, x, yy + 12, F::Body, TEXT, 18);
            std::snprintf(b, sizeof b, "%.1f h  \xC2\xB7  %.0f km", rw.hours, rw.km);
            textRight(b, x + w - 10, yy + 14, F::Small, DIM, 14);
        } else {
            const RideSummary& rd = *rw.ride;
            Rectangle row{x, yy, w - 10, 52};
            bool hv = hovered(row) && CheckCollisionPointRec(GetMousePosition(), lr);
            DrawRectangleRounded(row, 0.2f, 6, hv ? Color{40, 46, 64, 230} : CARD2);
            auto d = dates::localDate(rd.startedAt);
            if (d) {
                text(dates::weekdayShort(d->weekday()), row.x + 14, row.y + 8, F::Small, DIM, 12);
                text(std::to_string(d->d), row.x + 14, row.y + 22, F::Title, TEXT, 22);
            }
            text(rd.name, row.x + 64, row.y + 7, F::Body, TEXT, 17);
            std::string tag = rideKindName(rd.kind);
            if (rd.personalBest) tag += "  \xC2\xB7  \xE2\x98\x85 PB";
            if (rd.stravaActivityId) tag += "  \xC2\xB7  Strava";
            if (rd.simulated) tag += "  \xC2\xB7  simulated";
            text(tag, row.x + 64, row.y + 30, F::Small, rd.personalBest ? GOOD : FAINT, 12);
            struct C { std::string v; const char* l; };
            C cols[] = {{fmtTime(rd.movingS), "time"}, {fmtDist(rd.distanceM, prof_.metric), "distance"}, {fmt("%.0f m", rd.elevationGainM), "climb"},
                        {fmt("%.0f W", rd.np), "NP"}, {fmt("%.0f", rd.tss), "TSS"}, {rd.avgHr ? fmt("%.0f", *rd.avgHr) : "--", "bpm"}};
            float cx = row.x + row.width - 6 * 104 - 10;
            for (auto& c : cols) {
                textRight(c.v, cx + 96, row.y + 8, F::Body, TEXT, 16);
                textRight(c.l, cx + 96, row.y + 30, F::Small, FAINT, 11);
                cx += 104;
            }
            if (hv && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { selRide_ = rd.id; detail_ = db_.ride(rd.id); deleteArm_ = false; savedDir_.clear(); }
        }
        yy += h;
    }
    EndScissorMode();
    scrollBar(lr, actScroll_, content);
}

// ------------------------------------------------------------------ stats
void App::drawStatsPage(Rectangle r) {
    const bool metric = prof_.metric;
    char b[220];
    float gap = 16;
    // ---- totals row
    float cw = (r.width - 4 * gap) / 5, ch = 112;
    struct T { const char* l; const Totals* t; };
    T totals[] = {{"THIS WEEK", &stats_.thisWeek}, {"LAST WEEK", &stats_.lastWeek}, {"THIS MONTH", &stats_.thisMonth}, {"THIS YEAR", &stats_.thisYear}, {"ALL TIME", &stats_.allTime}};
    for (int i = 0; i < 5; ++i) {
        Rectangle c{r.x + i * (cw + gap), r.y, cw, ch};
        panel(c, CARD);
        text(totals[i].l, c.x + 16, c.y + 12, F::Small, DIM, 12);
        const Totals& t = *totals[i].t;
        text(fmt("%.1f h", t.movingS / 3600), c.x + 16, c.y + 30, F::Big, TEXT, 34);
        std::snprintf(b, sizeof b, "%d rides  \xC2\xB7  %s", t.rides, fmtDist(t.distanceM, metric).c_str());
        text(b, c.x + 16, c.y + 70, F::Small, DIM, 13);
        std::snprintf(b, sizeof b, "%.0f m  \xC2\xB7  %.0f kJ  \xC2\xB7  TSS %.0f", t.elevationM, t.workKj, t.tss);
        text(b, c.x + 16, c.y + 88, F::Small, FAINT, 12);
    }
    float y = r.y + ch + gap;
    float rest = r.y + r.height - y;
    float lw = (r.width - gap) * 0.58f, rw = r.width - lw - gap;
    // ---- fitness / fatigue / form
    {
        Rectangle c{r.x, y, lw, rest * 0.5f - gap / 2};
        panel(c, CARD);
        text("FITNESS  \xC2\xB7  FATIGUE  \xC2\xB7  FORM", c.x + 16, c.y + 12, F::Small, DIM, 12);
        int range = segmented({c.x + c.width - 236, c.y + 8, 220, 28}, {"6 wk", "3 mo", "1 yr"}, fitnessRange_);
        if (range >= 0) fitnessRange_ = range;
        int days = fitnessRange_ == 0 ? 42 : fitnessRange_ == 1 ? 91 : 365;
        size_t n = stats_.fitness.size(), s0 = n > size_t(days) ? n - days : 0;
        Series ctl{{}, SKY, 2.5f, true}, atl{{}, Color{236, 72, 153, 255}, 1.6f, false}, tsb{{}, GOOD, 1.6f, false};
        float lo = 0, hi = 10;
        std::vector<std::string> xl;
        for (size_t i = s0; i < n; ++i) {
            auto& f = stats_.fitness[i];
            ctl.v.push_back(float(f.ctl)); atl.v.push_back(float(f.atl)); tsb.v.push_back(float(f.tsb));
            lo = std::min({lo, float(f.tsb)}); hi = std::max({hi, float(f.ctl), float(f.atl)});
        }
        int labels = 6;
        size_t cnt = n - s0;
        for (size_t i = 0; i < cnt; ++i) {
            bool lab = cnt > 1 && (i % std::max<size_t>(1, (cnt - 1) / (labels - 1)) == 0);
            const Date& d = stats_.fitness[s0 + i].date;
            xl.push_back(lab ? fmtDateShort(d.y, d.m, d.d) : "");
        }
        lo = std::floor(lo / 10) * 10; hi = std::ceil(hi / 10) * 10;
        lineChart({c.x + 10, c.y + 44, c.width - 24, c.height - 56}, {ctl, atl, tsb}, lo, hi, xl, 4, "%.0f", 0);
        if (n) {
            auto& f = stats_.fitness.back();
            std::snprintf(b, sizeof b, "Fitness %.0f   Fatigue %.0f   Form %+.0f", f.ctl, f.atl, f.tsb);
            text(b, c.x + 300, c.y + 12, F::Small, TEXT, 13);
        }
        DrawRectangle(int(c.x + 16), int(c.y + 32), 10, 3, SKY);
    }
    // ---- weekly volume
    {
        Rectangle c{r.x, y + rest * 0.5f + gap / 2, lw, rest * 0.5f - gap / 2};
        panel(c, CARD);
        text("WEEKLY HOURS  \xC2\xB7  16 WEEKS", c.x + 16, c.y + 12, F::Small, DIM, 12);
        std::vector<float> hv;
        std::vector<std::string> lb;
        for (size_t i = 0; i < stats_.weekly.size(); ++i) {
            hv.push_back(float(stats_.weekly[i].totals.movingS / 3600));
            const Date& d = stats_.weekly[i].weekStart;
            lb.push_back(i % 2 == (stats_.weekly.size() - 1) % 2 ? fmtDateShort(d.y, d.m, d.d) : "");
        }
        barChart({c.x + 16, c.y + 36, c.width - 32, c.height - 46}, hv, lb, ACCENT, int(hv.size()) - 1, "%.1f", float(prof_.weeklyGoalHours));
        std::snprintf(b, sizeof b, "Goal %.0f h / week  \xC2\xB7  streak %d week%s", prof_.weeklyGoalHours, stats_.weekStreak, stats_.weekStreak == 1 ? "" : "s");
        textRight(b, c.x + c.width - 16, c.y + 12, F::Small, DIM, 12);
    }
    // ---- right column: power curve, zones + records, FTP history
    float rx = r.x + lw + gap;
    {
        Rectangle c{rx, y, rw, rest * 0.42f};
        panel(c, CARD);
        text("POWER CURVE", c.x + 16, c.y + 12, F::Small, DIM, 12);
        int u = segmented({c.x + c.width - 156, c.y + 8, 140, 28}, {"W", "W/kg"}, curveWkg_ ? 1 : 0);
        if (u >= 0) curveWkg_ = u == 1;
        Series all{{}, Color{255, 255, 255, 110}, 1.6f, false}, d90{{}, ACCENT, 2.5f, true};
        std::vector<std::string> xl;
        float hi = 1;
        auto val = [&](const CurvePoint& p) { return float(curveWkg_ ? p.wkg : p.watts); };
        for (auto& p : stats_.powerCurveAll) { all.v.push_back(val(p)); hi = std::max(hi, val(p)); xl.push_back(durLabel(p.durationS)); }
        for (auto& p : stats_.powerCurveAll) {
            float v = 0;
            for (auto& q : stats_.powerCurve90d) if (q.durationS == p.durationS) v = val(q);
            d90.v.push_back(v);
        }
        for (size_t i = 0; i < xl.size(); ++i) if (xl.size() > 8 && i % 2) xl[i] = "";
        if (!all.v.empty()) lineChart({c.x + 10, c.y + 44, c.width - 24, c.height - 56}, {d90, all}, 0, std::ceil(hi * 1.1f / (curveWkg_ ? 1 : 50)) * (curveWkg_ ? 1 : 50), xl, 4, curveWkg_ ? "%.1f" : "%.0f");
        else textCentered("Ride to build your power curve", c, F::Body, FAINT);
        text("90 days", c.x + 140, c.y + 12, F::Small, ACCENT, 12);
        text("all time", c.x + 200, c.y + 12, F::Small, DIM, 12);
    }
    float y2 = y + rest * 0.42f + gap;
    float h2 = rest * 0.58f - gap;
    float half = (rw - gap) / 2;
    {
        Rectangle c{rx, y2, half, h2};
        panel(c, CARD);
        text("ZONES  \xC2\xB7  LAST 28 DAYS", c.x + 16, c.y + 12, F::Small, DIM, 12);
        std::vector<std::string> zl;
        std::vector<Color> zc;
        for (int z = 0; z < 7; ++z) { zl.push_back("Z" + std::to_string(z + 1)); zc.push_back(zoneColor(z)); }
        zoneBars({c.x + 14, c.y + 36, c.width - 28, std::min(200.f, c.height * 0.5f)}, stats_.zoneS28d.data(), 7, zl, zc);
        float yy = c.y + 46 + std::min(200.f, c.height * 0.5f);
        text("RECORDS", c.x + 16, yy, F::Small, DIM, 12);
        yy += 20;
        auto rec = [&](const char* l, const std::optional<RecordRef>& rr, std::string v) {
            if (!rr || yy > c.y + c.height - 20) return;
            text(l, c.x + 16, yy, F::Small, DIM, 13);
            textRight(v, c.x + c.width - 16, yy, F::Small, TEXT, 13);
            yy += 20;
        };
        auto& R = stats_.records;
        rec("Longest ride", R.longestDistance, R.longestDistance ? fmtDist(R.longestDistance->value, metric) : "");
        rec("Longest time", R.longestTime, R.longestTime ? fmtTime(R.longestTime->value) : "");
        rec("Most climbing", R.mostElevation, R.mostElevation ? fmt("%.0f m", R.mostElevation->value) : "");
        rec("Most work", R.mostWork, R.mostWork ? fmt("%.0f kJ", R.mostWork->value) : "");
        rec("Highest NP", R.highestNp, R.highestNp ? fmt("%.0f W", R.highestNp->value) : "");
        rec("Highest TSS", R.highestTss, R.highestTss ? fmt("%.0f", R.highestTss->value) : "");
    }
    {
        Rectangle c{rx + half + gap, y2, half, h2};
        panel(c, CARD);
        text("FTP HISTORY", c.x + 16, c.y + 12, F::Small, DIM, 12);
        auto& fh = stats_.ftpHistory;
        if (fh.size() >= 1) {
            Series s{{}, GOOD, 2.5f, true};
            float lo = 1e9f, hi = 0;
            std::vector<std::string> xl;
            for (auto& e : fh) { s.v.push_back(float(e.ftp)); lo = std::min(lo, float(e.ftp)); hi = std::max(hi, float(e.ftp)); }
            if (s.v.size() == 1) s.v.push_back(s.v[0]);
            float ch2 = std::min(c.height * 0.5f, 180.f);
            lineChart({c.x + 6, c.y + 36, c.width - 18, ch2}, {s}, std::floor((lo - 15) / 10) * 10, std::ceil((hi + 15) / 10) * 10, {}, 3);
            float yy = c.y + 46 + ch2;
            for (auto it = fh.rbegin(); it != fh.rend() && yy < c.y + c.height - 20; ++it) {
                text(it->date, c.x + 16, yy, F::Small, DIM, 13);
                text(it->source, c.x + 110, yy, F::Small, FAINT, 13);
                textRight(fmt("%.0f W", it->ftp), c.x + c.width - 16, yy, F::Small, TEXT, 13);
                yy += 20;
            }
        }
        if (assessment_ && assessment_->estimate) {
            std::snprintf(b, sizeof b, "eFTP %.0f W", assessment_->estimate->estimate);
            textRight(b, c.x + c.width - 16, c.y + 12, F::Small, ACCENT, 12);
        }
    }
}
