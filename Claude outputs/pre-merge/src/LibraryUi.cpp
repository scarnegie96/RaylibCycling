// Routes (library, detail, generator, segment builder, GPX import) and Workouts (library + editor).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>

#include "App.h"
#include "core/Export.h"
#include "core/Metrics.h"
#include "raylib.h"

using namespace ui;

static std::string fmt(const char* f, double v) { char b[96]; std::snprintf(b, sizeof b, f, v); return b; }
static const Color CARD{18, 21, 31, 236};
static const Color CARD2{26, 30, 42, 225};

static const char* sourceName(RouteSource s) {
    switch (s) {
        case RouteSource::Generated: return "Generated";
        case RouteSource::Gpx: return "GPX";
        case RouteSource::Builtin: return "Built-in";
        default: return "Custom";
    }
}

// "2km 0%", "500m 6.5%", "1.2 km at -4%" -> segments
static bool parseSegments(const std::string& text, std::vector<SegmentSpec>& out, std::string& err) {
    out.clear();
    std::stringstream ss(text);
    std::string line;
    int n = 0;
    while (std::getline(ss, line, '\n')) {
        for (auto& c : line) if (c == ',' || c == '@') c = ' ';
        std::string t;
        for (char c : line) if (c != ' ' && c != '\t' && c != '\r') t += char(std::tolower((unsigned char)c));
        if (t.empty()) continue;
        ++n;
        size_t pct = t.find('%');
        if (pct == std::string::npos) { err = "Line " + std::to_string(n) + ": add a gradient like 5%"; return false; }
        size_t at = t.find("at");
        std::string len = t.substr(0, at != std::string::npos && at < pct ? at : std::string::npos);
        // split length and grade: the length ends at "km" / "m"
        size_t unit = len.find("km");
        double mult = 1000;
        if (unit == std::string::npos) { unit = len.find('m'); mult = 1; }
        if (unit == std::string::npos) { err = "Line " + std::to_string(n) + ": add a length like 2km or 800m"; return false; }
        double L = std::atof(len.substr(0, unit).c_str()) * mult;
        size_t gs = unit + (mult > 1 ? 2 : 1);
        std::string g = t.substr(gs, pct - gs);
        if (g.rfind("at", 0) == 0) g = g.substr(2);
        double G = std::atof(g.c_str());
        if (L < 20 || L > 300000) { err = "Line " + std::to_string(n) + ": length must be 20 m - 300 km"; return false; }
        if (std::fabs(G) > 25) { err = "Line " + std::to_string(n) + ": gradient must be within \xC2\xB1" "25%"; return false; }
        out.push_back({L, G});
    }
    if (out.empty()) { err = "Add at least one segment"; return false; }
    return true;
}

// ------------------------------------------------------------------ routes page
void App::drawRoutesPage(Rectangle r) {
    if (routeView_ == 1) { drawRouteGenerator(r); return; }
    if (routeView_ == 2) { drawRouteBuilder(r); return; }
    float lw = std::min(560.f, r.width * 0.42f);
    Rectangle L{r.x, r.y, lw, r.height};
    panel(L, CARD);
    float x = L.x + 18, y = L.y + 14, w = L.width - 36;
    text("Routes", x, y, F::Title, TEXT);
    if (button({L.x + L.width - 236, y + 2, 108, 34}, "+ Generate", false, true, 15)) { routeView_ = 1; genPreview_.reset(); }
    if (button({L.x + L.width - 122, y + 2, 104, 34}, "+ Build", false, true, 15)) routeView_ = 2;
    y += 50;
    static const std::vector<std::string> filters = {"All", "\xE2\x98\x85", "Loops", "A to B", "GPX", "Mine"};
    int f = segmented({x, y, w, 32}, filters, routeFilter_);
    if (f >= 0) routeFilter_ = f;
    y += 44;
    std::vector<const Route*> list;
    for (auto& ro : routes_) {
        bool ok = routeFilter_ == 0 || (routeFilter_ == 1 && ro.favourite) || (routeFilter_ == 2 && ro.isLoop) || (routeFilter_ == 3 && !ro.isLoop) ||
                  (routeFilter_ == 4 && ro.source == RouteSource::Gpx) || (routeFilter_ == 5 && ro.source != RouteSource::Builtin);
        if (ok) list.push_back(&ro);
    }
    Rectangle lr{x, y, w, L.y + L.height - y - 52};
    float rowH = 74;
    scrollUpdate(lr, routeScroll_, list.size() * (rowH + 6));
    BeginScissorMode(int(lr.x), int(lr.y), int(lr.width), int(lr.height));
    char b[200];
    for (size_t i = 0; i < list.size(); ++i) {
        const Route& ro = *list[i];
        Rectangle row{lr.x, lr.y + i * (rowH + 6) - routeScroll_, lr.width - 8, rowH};
        if (row.y + rowH < lr.y || row.y > lr.y + lr.height) continue;
        bool sel = ro.id == selRoute_, h = hovered(row) && CheckCollisionPointRec(GetMousePosition(), lr);
        DrawRectangleRounded(row, 0.2f, 6, sel ? Color{255, 92, 56, 55} : h ? Color{40, 46, 64, 230} : CARD2);
        if (sel) DrawRectangleRoundedLinesEx(row, 0.2f, 6, 1.4f, ACCENT);
        text((ro.favourite ? "\xE2\x98\x85 " : "") + ro.name, row.x + 12, row.y + 8, F::Body, TEXT, 18);
        const RouteProfile& rp = profileFor(ro);
        double gain = 0;
        for (size_t j = 1; j < rp.ele.size(); ++j) gain += std::max(0.0, rp.ele[j] - rp.ele[j - 1]);
        std::snprintf(b, sizeof b, "%s  \xC2\xB7  %.0f m  \xC2\xB7  %s", fmtDist(ro.distance(), prof_.metric).c_str(), gain, sourceName(ro.source));
        text(b, row.x + 12, row.y + 34, F::Small, DIM, 13);
        // mini elevation sparkline
        std::vector<float> ev;
        for (size_t j = 0; j < rp.ele.size(); j += std::max<size_t>(1, rp.ele.size() / 80)) ev.push_back(float(rp.ele[j]));
        float lo = *std::min_element(ev.begin(), ev.end()), hi = *std::max_element(ev.begin(), ev.end());
        seriesChart({row.x + row.width - 160, row.y + 10, 148, rowH - 20}, ev, Color{255, 140, 90, 255}, lo - 5, std::max(hi, lo + 60), true);
        if (h && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { selRoute_ = prefs_.selRoute = ro.id; showRoute(ro); routeDeleteArm_.clear(); }
    }
    EndScissorMode();
    scrollBar(lr, routeScroll_, list.size() * (rowH + 6));
    text("Drop a .gpx file on the window to import it.", x, L.y + L.height - 36, F::Small, FAINT, 14);

    // detail
    Rectangle R{L.x + L.width + 20, r.y, r.width - L.width - 20, r.height};
    const Route* sel = findRoute(selRoute_);
    if (sel) drawRouteDetail(R, *sel);
}

void App::drawRouteDetail(Rectangle R, const Route& rt) {
    panel(R, CARD);
    char b[220];
    float x = R.x + 22, y = R.y + 18, w = R.width - 44;
    text(rt.name, x, y, F::Title, TEXT, 30);
    if (button({R.x + R.width - 64, y, 44, 36}, rt.favourite ? "\xE2\x98\x85" : "\xE2\x98\x86", rt.favourite, true, 20)) {
        Route c = rt;
        c.favourite = !c.favourite;
        db_.saveRoute(c);
        routes_ = db_.routes();
        return;
    }
    y += 42;
    if (!rt.description.empty()) { text(rt.description, x, y, F::Small, DIM, 14); y += 24; }
    auto st = rt.stats();
    struct KV { const char* k; std::string v; };
    std::vector<KV> kv = {{"DISTANCE", fmtDist(rt.distance(), prof_.metric)},
                          {"CLIMBING", fmt("%.0f m", st.gainM)},
                          {"MAX GRADE", fmt("%.1f%%", st.maxGradePct)},
                          {"DIFFICULTY", st.difficultyLabel},
                          {"TYPE", rt.isLoop ? "Loop" : "Point to point"},
                          {"EST. TIME", fmtTime(rt.estimateTime(prof_.ftp * 0.75, [&] { phys::Params p; p.massKg = prof_.weightKg + prof_.bikeWeightKg; p.cda = prof_.cda; p.crr = prof_.crr; return p; }()))}};
    float tw = (w - 5 * 10) / 6;
    for (size_t i = 0; i < kv.size(); ++i) {
        Rectangle c{x + i * (tw + 10), y, tw, 60};
        panel(c, CARD2);
        text(kv[i].k, c.x + 10, c.y + 6, F::Small, DIM, 11);
        text(kv[i].v, c.x + 10, c.y + 26, F::Title, TEXT, std::min(24.f, tw / std::max(5.f, float(kv[i].v.size())) * 1.6f));
    }
    y += 74;
    const RouteProfile& rp = profileFor(rt);
    float ph = std::min(220.f, R.height * 0.3f);
    routeProfile({x, y, w, ph}, rp, -1, -1, prof_.metric);
    y += ph + 12;
    // climbs
    if (!st.climbs.empty()) {
        text("CLIMBS", x, y, F::Small, DIM, 13);
        y += 22;
        for (auto& c : st.climbs) {
            if (y > R.y + R.height - 230) break;
            std::snprintf(b, sizeof b, "%s  \xC2\xB7  km %.1f  \xC2\xB7  %.1f km at %.1f%% (max %.1f%%)  \xC2\xB7  +%.0f m", c.category == "Uncategorised" ? "Climb" : c.category.c_str(),
                          c.startD / 1000, c.lengthM / 1000, c.avgGradePct, c.maxGradePct, c.gainM);
            text(b, x, y, F::Small, TEXT, 14);
            y += 22;
        }
        y += 8;
    }
    // attempts
    auto attempts = db_.routeRides(rt.id);
    text("YOUR RIDES", x, y, F::Small, DIM, 13);
    y += 22;
    std::optional<double> pb;
    for (auto& a : attempts) if (a.completed && a.routeTimeS && (!pb || *a.routeTimeS < *pb)) pb = a.routeTimeS;
    if (attempts.empty()) { text("Not ridden yet.", x, y, F::Small, FAINT, 14); y += 24; }
    for (auto& a : attempts) {
        if (y > R.y + R.height - 110) break;
        auto d = dates::localDate(a.startedAt);
        bool isPb = pb && a.routeTimeS && *a.routeTimeS == *pb && a.completed;
        std::snprintf(b, sizeof b, "%s   %s   %d lap%s   %s   %.0f W", d ? d->str().c_str() : "", a.completed && a.routeTimeS ? fmtTime(*a.routeTimeS).c_str() : "unfinished",
                      a.laps, a.laps == 1 ? "" : "s", fmtDist(a.distanceM, prof_.metric).c_str(), a.avgPower);
        text(b, x, y, F::Small, isPb ? GOOD : TEXT, 14);
        if (isPb) textRight("PB", x + w, y, F::Small, GOOD, 14);
        y += 22;
    }
    float by = R.y + R.height - 62;
    if (button({x, by, 200, 46}, "Ride this route", true)) {
        selRoute_ = prefs_.selRoute = rt.id;
        setupKind_ = RideKind::Route;
        page_ = PRide;
    }
    if (button({x + 212, by, 150, 46}, "Show in 3D")) { showRoute(rt); toast("Showing " + rt.name); }
    if (rt.source != RouteSource::Builtin) {
        bool armed = routeDeleteArm_ == rt.id;
        if (button({x + 374, by, 160, 46}, armed ? "Really delete?" : "Delete", false)) {
            if (armed) {
                db_.deleteRoute(rt.id);
                routes_ = db_.routes();
                selRoute_ = routes_.empty() ? "" : routes_[0].id;
                routeDeleteArm_.clear();
                toast("Route deleted");
            } else routeDeleteArm_ = rt.id;
        }
    }
}

void App::drawRouteGenerator(Rectangle r) {
    panel(r, CARD);
    float x = r.x + 22, y = r.y + 18, w = std::min(520.f, r.width * 0.4f);
    text("Generate a route", x, y, F::Title, TEXT);
    if (button({r.x + r.width - 140, y, 120, 36}, "Back")) routeView_ = 0;
    y += 54;
    int d;
    bool ch = false;
    text("Name", x, y, F::Small, DIM);
    textBox({x, y + 20, w, 38}, genName_, false);
    y += 72;
    text("Terrain", x, y, F::Small, DIM);
    y += 20;
    int t = segmented({x, y, w, 34}, {"Flat", "Rolling", "Hilly", "Mountain", "Alpine"}, int(gen_.terrain));
    if (t >= 0) { gen_.terrain = Terrain(t); gen_.targetGainM = std::round(suggestedGain(gen_.terrain, gen_.distanceKm) / 10) * 10; ch = true; }
    y += 46;
    text("Finish", x, y, F::Small, DIM);
    y += 20;
    int f = segmented({x, y, w, 34}, {"Anywhere", "Loop (back to start)", "Summit"}, int(gen_.finish));
    if (f >= 0) { gen_.finish = Finish(f); ch = true; }
    y += 48;
    if (stepper({x, y, w, 34}, "Distance", fmt("%.0f km", gen_.distanceKm), &d)) {
        gen_.distanceKm = std::clamp(gen_.distanceKm + d * (gen_.distanceKm >= 50 ? 10 : 5), 5.0, 300.0);
        ch = true;
    }
    y += 44;
    if (stepper({x, y, w, 34}, "Climbing", fmt("%.0f m", gen_.targetGainM), &d)) { gen_.targetGainM = std::clamp(gen_.targetGainM + d * 50, 0.0, 8000.0); ch = true; }
    y += 44;
    if (stepper({x, y, w, 34}, "Max gradient", fmt("%.0f%%", gen_.maxGradePct), &d)) { gen_.maxGradePct = std::clamp(gen_.maxGradePct + d, 2.0, 20.0); ch = true; }
    y += 44;
    if (stepper({x, y, w, 34}, "Smoothness", fmt("%.0f%%", gen_.smoothness * 100), &d)) { gen_.smoothness = std::clamp(gen_.smoothness + d * 0.1, 0.0, 1.0); ch = true; }
    y += 44;
    if (stepper({x, y, w, 34}, "Start elevation", fmt("%.0f m", gen_.startEleM), &d)) { gen_.startEleM = std::clamp(gen_.startEleM + d * 100, 0.0, 2500.0); ch = true; }
    y += 44;
    text(fmt("Seed %.0f", double(gen_.seed)), x, y + 8, F::Small, DIM, 14);
    if (button({x + w - 170, y, 170, 34}, "Shuffle \xE2\x86\xBB")) { gen_.seed = uint64_t(GetRandomValue(1, 1000000)); ch = true; }
    y += 50;
    if (ch || !genPreview_) {
        genPreview_ = generateRoute(gen_, genName_.text.empty() ? "Generated route" : genName_.text);
    }
    // preview
    float px = x + w + 30, pw = r.x + r.width - px - 22;
    const Route& g = *genPreview_;
    auto st = g.stats();
    char b[220];
    std::snprintf(b, sizeof b, "%s  \xC2\xB7  %.0f m climbing  \xC2\xB7  max %.1f%%  \xC2\xB7  %zu climbs  \xC2\xB7  %s", fmtDist(g.distance(), prof_.metric).c_str(), st.gainM,
                  st.maxGradePct, st.climbs.size(), st.difficultyLabel.c_str());
    text(b, px, r.y + 74, F::Body, TEXT, 17);
    RouteProfile rp = RouteProfile::fromRoute(g, 10);
    routeProfile({px, r.y + 106, pw, std::min(320.f, r.height - 220)}, rp, -1, -1, prof_.metric);
    float by = r.y + r.height - 70;
    if (button({x, by, 220, 48}, "Save route", true, !genName_.text.empty())) {
        Route s = g;
        s.name = genName_.text;
        if (s.id.empty()) s.id = dates::newId();
        if (s.createdAt.empty()) s.createdAt = dates::nowRfc3339();
        s.source = RouteSource::Generated;
        db_.saveRoute(s);
        routes_ = db_.routes();
        selRoute_ = prefs_.selRoute = s.id;
        routeView_ = 0;
        showRoute(s);
        toast("Saved " + s.name, GOOD);
    }
}

void App::drawRouteBuilder(Rectangle r) {
    panel(r, CARD);
    float x = r.x + 22, y = r.y + 18, w = std::min(460.f, r.width * 0.36f);
    text("Build a route from segments", x, y, F::Title, TEXT);
    if (button({r.x + r.width - 140, y, 120, 36}, "Back")) routeView_ = 0;
    y += 54;
    text("Name", x, y, F::Small, DIM);
    textBox({x, y + 20, w, 38}, builderName_, false);
    y += 72;
    text("Segments - one per line: length and gradient", x, y, F::Small, DIM);
    textBox({x, y + 20, w, 220}, builderText_, true, "2km 0%");
    y += 252;
    if (toggle({x, y, w, 34}, "Loop (ride it for laps)", builderLoop_)) builderLoop_ = !builderLoop_;
    y += 46;
    const char* help[] = {"5km 0%           flat", "3.5km 7%        a climb", "800m -6%        a descent", "Up to 25% either way, 20 m - 300 km each."};
    for (auto* h : help) { text(h, x, y, F::Small, FAINT, 14); y += 20; }
    bool ok = parseSegments(builderText_.text, builderSegs_, builderErr_);
    float px = x + w + 30, pw = r.x + r.width - px - 22;
    if (ok) {
        Route g = buildFromSegments(builderName_.text, builderSegs_, 100, true);
        g.isLoop = builderLoop_;
        auto st = g.stats();
        char b[200];
        std::snprintf(b, sizeof b, "%s  \xC2\xB7  %.0f m climbing  \xC2\xB7  max %.1f%%  \xC2\xB7  %s", fmtDist(g.distance(), prof_.metric).c_str(), st.gainM, st.maxGradePct,
                      st.difficultyLabel.c_str());
        text(b, px, r.y + 74, F::Body, TEXT, 17);
        RouteProfile rp = RouteProfile::fromRoute(g, 10);
        routeProfile({px, r.y + 106, pw, std::min(320.f, r.height - 220)}, rp, -1, -1, prof_.metric);
        if (button({x, r.y + r.height - 70, 220, 48}, "Save route", true, !builderName_.text.empty())) {
            if (g.id.empty()) g.id = dates::newId();
            if (g.createdAt.empty()) g.createdAt = dates::nowRfc3339();
            g.source = RouteSource::Custom;
            g.segments = builderSegs_;
            db_.saveRoute(g);
            routes_ = db_.routes();
            selRoute_ = prefs_.selRoute = g.id;
            routeView_ = 0;
            showRoute(g);
            toast("Saved " + g.name, GOOD);
        }
    } else {
        DrawRectangleRounded({px, r.y + 74, pw, 44}, 0.3f, 6, Color{248, 82, 82, 40});
        text(builderErr_, px + 12, r.y + 86, F::Body, BAD, 16);
    }
}

// ------------------------------------------------------------------ workouts
void App::drawWorkoutsPage(Rectangle r) {
    if (editorOpen_) { drawWorkoutEditor(r); return; }
    float lw = std::min(640.f, r.width * 0.5f);
    Rectangle L{r.x, r.y, lw, r.height};
    panel(L, CARD);
    float x = L.x + 18, y = L.y + 14, w = L.width - 36;
    text("Workouts", x, y, F::Title, TEXT);
    if (button({L.x + L.width - 150, y + 2, 132, 34}, "+ New")) {
        editorOpen_ = true;
        editingId_.clear();
        edName_.text = "My workout";
        edDesc_.text.clear();
        edText_.text = "10m 45-75%\n3x (8m @95%, 4m @55%)\n5m 60-40%";
        edErr_.clear();
    }
    y += 48;
    std::vector<std::string> cats = {"All"};
    for (auto& c : wk::categories()) cats.push_back(c);
    if (!custom_.empty()) cats.push_back("Custom");
    float cx = x;
    for (size_t i = 0; i < cats.size(); ++i) {
        float cwid = measure(cats[i], F::Small).x + 22;
        if (cx + cwid > x + w) { cx = x; y += 32; }
        if (chipButton({cx, y, cwid, 26}, cats[i], wkCategory_ == int(i))) wkCategory_ = int(i);
        cx += cwid + 6;
    }
    y += 34;
    int s = segmented({x, y, 360, 32}, {"Library order", "Duration", "TSS"}, wkSort_);
    if (s >= 0) wkSort_ = s;
    y += 44;
    std::vector<const wk::Workout*> list;
    for (auto* wp : allWorkouts()) {
        if (wkCategory_ == 0) list.push_back(wp);
        else if (cats[std::min<size_t>(wkCategory_, cats.size() - 1)] == "Custom" ? wp->id.rfind("custom-", 0) == 0 : wp->category == cats[std::min<size_t>(wkCategory_, cats.size() - 1)])
            list.push_back(wp);
    }
    if (wkSort_ == 1) std::stable_sort(list.begin(), list.end(), [](auto* a, auto* b) { return a->durationS() < b->durationS(); });
    if (wkSort_ == 2) std::stable_sort(list.begin(), list.end(), [&](auto* a, auto* b) { return a->estimate(prof_.ftp).tss < b->estimate(prof_.ftp).tss; });
    Rectangle listR{x, y, w, L.y + L.height - y - 12};
    float rowH = 62;
    scrollUpdate(listR, wkScroll_, list.size() * (rowH + 6));
    BeginScissorMode(int(listR.x), int(listR.y), int(listR.width), int(listR.height));
    char b[160];
    for (size_t i = 0; i < list.size(); ++i) {
        const auto* wp = list[i];
        Rectangle c{listR.x, listR.y + i * (rowH + 6) - wkScroll_, listR.width - 8, rowH};
        if (c.y + c.height < listR.y || c.y > listR.y + listR.height) continue;
        bool sel = wp->id == wkSelected_;
        bool h = hovered(c) && CheckCollisionPointRec(GetMousePosition(), listR);
        DrawRectangleRounded(c, 0.2f, 8, sel ? Color{255, 92, 56, 55} : h ? Color{40, 46, 64, 230} : CARD2);
        if (sel) DrawRectangleRoundedLinesEx(c, 0.2f, 8, 1.4f, ACCENT);
        text(wp->name, c.x + 14, c.y + 8, F::Body, TEXT, 19);
        auto e = wp->estimate(prof_.ftp);
        std::snprintf(b, sizeof b, "%s  \xC2\xB7  TSS %.0f  \xC2\xB7  %s", fmtTime(e.durationS).c_str(), e.tss, wp->category.c_str());
        text(b, c.x + 14, c.y + 36, F::Small, DIM, 14);
        workoutChart({c.x + c.width - 170, c.y + 8, 156, rowH - 16}, wp->steps(), prof_.ftp, 1.0, -1, nullptr);
        if (h && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) wkSelected_ = wp->id;
    }
    EndScissorMode();
    scrollBar(listR, wkScroll_, list.size() * (rowH + 6));

    const wk::Workout* sel = findWorkout(wkSelected_);
    Rectangle R{L.x + L.width + 20, r.y, r.width - L.width - 20, r.height};
    panel(R, CARD);
    if (!sel) return;
    x = R.x + 22; y = R.y + 18; w = R.width - 44;
    text(sel->name, x, y, F::Title, TEXT);
    y += 44;
    y += textWrapped(sel->description.empty() ? sel->category : sel->description, {x, y, w, 80}, F::Body, DIM, 17) + 10;
    workoutChart({x, y, w, 150}, sel->steps(), prof_.ftp, 1.0, -1, nullptr);
    y += 166;
    auto e = sel->estimate(prof_.ftp);
    struct KV { const char* k; std::string v; };
    std::vector<KV> kv = {{"DURATION", fmtTime(e.durationS)}, {"TSS", fmt("%.0f", e.tss)},     {"IF", fmt("%.2f", e.intensityFactor)},
                          {"NP", fmt("%.0f W", e.np)},          {"WORK", fmt("%.0f kJ", e.workKj)}, {"AVG", fmt("%.0f W", e.avgPower)}};
    float tw = (w - 5 * 10) / 6;
    for (size_t i = 0; i < kv.size(); ++i) {
        Rectangle c{x + i * (tw + 10), y, tw, 62};
        panel(c, CARD2);
        text(kv[i].k, c.x + 10, c.y + 6, F::Small, DIM, 12);
        text(kv[i].v, c.x + 10, c.y + 24, F::Title, TEXT, 26);
    }
    y += 76;
    text("Time in zone", x, y, F::Small, DIM);
    y += 22;
    std::vector<std::string> zl;
    std::vector<Color> zc;
    for (int z = 0; z < 7; ++z) { zl.push_back("Z" + std::to_string(z + 1) + " " + metrics::ZONE_NAMES[z]); zc.push_back(zoneColor(z)); }
    float zh = std::min(170.f, R.y + R.height - y - 80);
    if (zh > 70) zoneBars({x, y, w, zh}, e.zoneS.data(), 7, zl, zc);
    float by = R.y + R.height - 62;
    if (button({x, by, 220, 46}, "Ride this workout", true)) {
        selWorkout_ = sel->id;
        setupKind_ = RideKind::Workout;
        page_ = PRide;
    }
    if (button({x + 232, by, 150, 46}, "Export .zwo")) {
        std::string base = sel->name;
        for (auto& c : base) if (!std::isalnum((unsigned char)c)) c = '_';
        std::string path = exporter::ridesDir() + "/" + base + ".zwo";
        std::ofstream(path) << wk::toZwo(*sel);
        toast("Saved " + path, GOOD);
    }
    bool isCustom = sel->id.rfind("custom-", 0) == 0;
    if (button({x + 394, by, 160, 46}, isCustom ? "Edit" : "Copy & edit")) {
        editorOpen_ = true;
        editingId_ = isCustom ? sel->id : "";
        edName_.text = isCustom ? sel->name : sel->name + " (copy)";
        edDesc_.text = sel->description;
        edText_.text = wk::toText(sel->blocks);
        edErr_.clear();
    }
    if (isCustom && button({x + 566, by, 120, 46}, "Delete")) {
        store::deleteCustomWorkout(sel->id);
        custom_ = store::loadCustomWorkouts();
        wkSelected_ = builtin_[0].id;
        toast("Workout deleted");
    }
}

void App::drawWorkoutEditor(Rectangle r) {
    panel(r, CARD);
    float x = r.x + 22, y = r.y + 16, w = r.width - 44;
    text(editingId_.empty() ? "New workout" : "Edit workout", x, y, F::Title, TEXT);
    y += 50;
    float lw = w * 0.46f;
    text("Name", x, y, F::Small, DIM);
    textBox({x, y + 20, lw, 38}, edName_, false);
    text("Description", x + lw + 20, y, F::Small, DIM);
    textBox({x + lw + 20, y + 20, w - lw - 20, 38}, edDesc_, false);
    y += 74;
    text("Plan (text format)", x, y, F::Small, DIM);
    textBox({x, y + 20, lw, r.y + r.height - y - 110}, edText_, true, "10m 50-75%");
    float hx = x + lw + 20, hw = w - lw - 20, hy = y;
    text("Format", hx, hy, F::Small, DIM);
    hy += 22;
    const char* help[] = {"10m 50-75%                 ramp from 50 % to 75 % FTP", "20m @90% 85rpm             steady with a cadence target",
                          "5x 1m @120% / 1m @50%      intervals", "3x (30s @150%, 30s @50%)   a repeat set (nestable)",
                          "2m free  \xC2\xB7  30s effort @2%   rider-controlled", "1:30 @250w  \xC2\xB7  1h @65%       mm:ss, hours, watts",
                          "One item per line, or separate with commas."};
    for (auto* h : help) { text(h, hx, hy, F::Small, TEXT, 14); hy += 20; }
    hy += 10;
    std::vector<wk::Block> blocks;
    std::string err;
    bool ok = wk::parseText(edText_.text, prof_.ftp, blocks, err);
    if (ok) {
        auto steps = wk::blocksToSteps(blocks);
        workoutChart({hx, hy, hw, 140}, steps, prof_.ftp, 1.0, -1, nullptr);
        hy += 152;
        auto e = wk::estimateSteps(steps, prof_.ftp);
        char b[200];
        std::snprintf(b, sizeof b, "%s  \xC2\xB7  TSS %.0f  \xC2\xB7  IF %.2f  \xC2\xB7  NP %.0f W  \xC2\xB7  %.0f kJ  \xC2\xB7  %zu steps", fmtTime(e.durationS).c_str(), e.tss,
                      e.intensityFactor, e.np, e.workKj, steps.size());
        text(b, hx, hy, F::Body, TEXT, 17);
    } else {
        DrawRectangleRounded({hx, hy, hw, 44}, 0.3f, 6, Color{248, 82, 82, 40});
        text(err, hx + 12, hy + 12, F::Body, BAD, 16);
    }
    float by = r.y + r.height - 70;
    if (button({x, by, 180, 48}, "Save", true, ok && !edName_.text.empty())) {
        wk::Workout wo;
        wo.id = editingId_;
        wo.name = edName_.text;
        wo.description = edDesc_.text;
        wo.category = "Custom";
        wo.blocks = blocks;
        std::string e2;
        if (store::saveCustomWorkout(wo, e2)) {
            custom_ = store::loadCustomWorkouts();
            for (auto& c : custom_) if (c.name == wo.name) wkSelected_ = c.id;
            editorOpen_ = false;
            toast("Workout saved", GOOD);
        } else toast(e2, BAD);
    }
    if (button({x + 192, by, 140, 48}, "Cancel")) editorOpen_ = false;
}
