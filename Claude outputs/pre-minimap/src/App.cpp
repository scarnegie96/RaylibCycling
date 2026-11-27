#include "App.h"

#include <random>
#include "Terrain.h"

#include <algorithm>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

#include "Audio.h"
#include "Profiler.h"
#include "Net.h"
#include "core/Export.h"
#include "core/Metrics.h"
#include "raylib.h"
#include "raymath.h"

using namespace ui;

static std::atomic<int> g_async{0};

void App::toast(const std::string& s, Color c) {
    toasts_.push_back({s, GetTime(), c});
    while (toasts_.size() > 4) toasts_.pop_front();
}

bool App::simActive() const { return prefs_.simEnabled; }

std::vector<const wk::Workout*> App::allWorkouts() const {
    std::vector<const wk::Workout*> v;
    for (auto& w : builtin_) v.push_back(&w);
    for (auto& w : custom_) v.push_back(&w);
    return v;
}

const wk::Workout* App::findWorkout(const std::string& id) const {
    for (auto* w : allWorkouts()) if (w->id == id) return w;
    return builtin_.empty() ? nullptr : &builtin_[0];
}

const Route* App::findRoute(const std::string& id) const {
    for (auto& r : routes_) if (r.id == id) return &r;
    return routes_.empty() ? nullptr : &routes_[0];
}

const RouteProfile& App::profileFor(const Route& r) {
    std::string key = r.id + ":" + std::to_string(r.points.size()) + ":" + std::to_string(int(r.distance()));
    auto it = profileCache_.find(key);
    if (it != profileCache_.end()) return it->second;
    if (profileCache_.size() > 64) profileCache_.clear();
    return profileCache_[key] = RouteProfile::fromRoute(r, 10);
}

void App::async(std::function<void()> work) {
    ++g_async;
    std::thread([w = std::move(work)] {
        w();
        --g_async;
    }).detach();
}

void App::onMain(std::function<void()> fn) {
    std::lock_guard<std::mutex> lk(mainQm_);
    mainQ_.push_back(std::move(fn));
}

// ------------------------------------------------------------------ data
void App::loadData() {
    routes_ = db_.routes();
    events_ = db_.events();
    if (!findRoute(prefs_.selRoute) || prefs_.selRoute.empty()) prefs_.selRoute = routes_.empty() ? "" : routes_[0].id;
    selRoute_ = prefs_.selRoute;
    if (auto a = db_.meta("ftp_assessment")) {
        json::Value v;
        if (json::parse(*a, v)) assessment_ = FtpAssessment::fromJson(v);
    }
    refreshHistory();
}

#ifdef CORERAY_DOCK_ICON
extern "C" bool coreray_set_dock_icon(const char* pngPath);
#endif
// the app icon: the Dock on macOS, the window / taskbar icon elsewhere
static void setAppIcon() {
    const std::string dir = GetApplicationDirectory();
    for (const std::string& base : {dir + "assets/icon/", std::string("assets/icon/"), dir + "../Resources/assets/icon/"}) {
#ifdef CORERAY_DOCK_ICON
        if (FileExists((base + "coreray-1024.png").c_str()) && coreray_set_dock_icon((base + "coreray-1024.png").c_str())) return;
#else
        if (!FileExists((base + "coreray-256.png").c_str())) continue;
        Image icon = LoadImage((base + "coreray-256.png").c_str());
        if (icon.data) {
            ImageFormat(&icon, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
            SetWindowIcon(icon);
            UnloadImage(icon);
            return;
        }
#endif
    }
}

void App::refreshHistory() {
    rides_ = db_.rideSummaries();
    stats_ = computeStats(rides_, prof_.weightKg, db_.ftpHistory(), Date::today());
}

void App::showRoute(const Route& r, bool force) {
    worldRoute_ = r;
    if (screen_ != Screen::Ride && screen_ != Screen::Explore) return;  // the 3D world only exists while riding (or exploring)
    std::string key = r.id + ":" + std::to_string(r.points.size()) + ":" + std::to_string(int(r.distance())) + ":" + (r.isLoop ? "L" : "P");
    if (!force && key == worldRouteKey_ && course_) return;
    worldRouteKey_ = key;
    auto c = std::make_shared<Course>();
    if (map_ && r.id.rfind("map-", 0) == 0 && !mapPath_.empty()) c->buildWorld(r, map_, mapPath_, !roam_.on);   // a loop (or a roam) on the generated map
    else {
        // a new world every ride (the road is the route's; the land around it is fresh)
        uint32_t seed = worldSeed_ ? worldSeed_ : uint32_t(std::random_device{}());
        c->build(r, seed);
    }
    course_ = c;
    renderer_.setCourse(c);
    renderer_.preload(screen_ == Screen::Ride ? 0.0 : homeDist_);
}

// ------------------------------------------------------------------ FTP
void App::applyFtp(double ftp, const std::string& source) {
    prof_.ftp = std::round(ftp);
    ftpOnOpen_ = prof_.ftp;
    db_.addFtp({Date::today().str(), prof_.ftp, source});
    store::save(prof_, prefs_);
    if (assessment_) {
        assessment_->currentFtp = prof_.ftp;
        assessment_->dismissed = true;
        db_.setMeta("ftp_assessment", json::dump(assessment_->toJson()));
    }
    refreshHistory();
}

void App::reassessFtp(const std::optional<std::string>& trigger, bool allowAuto) {
    Date today = Date::today();
    auto est = estimateFtp(rides_, today);
    FtpAssessment a = assessFtp(prof_.ftp, est, today, trigger);
    if (assessment_ && assessment_->dismissed && assessment_->suggestedFtp && a.suggestedFtp &&
        std::fabs(*assessment_->suggestedFtp - *a.suggestedFtp) < 3 && assessment_->suggestion == a.suggestion)
        a.dismissed = true;
    if (allowAuto && prof_.ftpAuto == FtpAutoMode::AutoIncrease && a.suggestion == Suggestion::Increase && a.suggestedFtp) {
        double old = prof_.ftp;
        applyFtp(*a.suggestedFtp, "Estimate (auto)");
        a.autoApplied = true;
        a.dismissed = true;
        char b[120];
        std::snprintf(b, sizeof b, "FTP raised %.0f \xE2\x86\x92 %.0f W from your recent rides", old, prof_.ftp);
        toast(b, GOOD);
    }
    assessment_ = a;
    db_.setMeta("ftp_assessment", json::dump(a.toJson()));
}

// ------------------------------------------------------------------ run
int App::run(int argc, char** argv) {
    std::string shotPath, dbPath = Db::defaultPath();
    int shotFrames = 90, forceCam = -1, forcePage = -1, sectorAt = -1, strikeAt = -1;
    bool autoRide = false, autoExplore = false, force2d = false, autoSummary = false, demoHistory = false, openDetail = false, openDrawer = false;
    double forceHour = -1, skipAhead = 0;
    std::string forceKind, forceWeather, forceRoute;
    store::load(prof_, prefs_);
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--sim") prefs_.simEnabled = true;
        else if (a == "--ride") autoRide = true;
        else if (a == "--2d") force2d = true;
        else if (a == "--summary") autoSummary = true;
        else if (a == "--screenshot") shotPath = next();
        else if (a == "--frames") shotFrames = std::atoi(next().c_str());
        else if (a == "--cam") forceCam = std::atoi(next().c_str());
        else if (a == "--page" || a == "--tab") forcePage = std::atoi(next().c_str());
        else if (a == "--quality") prefs_.quality = std::atoi(next().c_str());
        else if (a == "--kind") forceKind = next();
        else if (a == "--workout") selWorkout_ = next();
        else if (a == "--hour") forceHour = std::atof(next().c_str());
        else if (a == "--weather") forceWeather = next();
        else if (a == "--route") forceRoute = next();
        else if (a == "--db") dbPath = next();
        else if (a == "--demo-history") demoHistory = true;
        else if (a == "--detail") openDetail = true;
        else if (a == "--drawer") openDrawer = true;
        else if (a == "--sector-at") sectorAt = std::atoi(next().c_str());
        else if (a == "--strike-at") strikeAt = std::atoi(next().c_str());   // dev: lightning on that frame
        else if (a == "--ahead") skipAhead = std::atof(next().c_str());
        else if (a == "--nohud") hudHidden_ = true;
        else if (a == "--perf") { prefs_.showFps = prefs_.showPerf = true; }
        else if (a == "--ptab") profileTab_ = std::atoi(next().c_str());
        else if (a == "--world") { prefs_.scenery = 1; prefs_.mapSeed = std::max(1, std::atoi(next().c_str())); }   // ride a generated map
        else if (a == "--explore") { autoExplore = true; prefs_.mapSeed = std::max(1, std::atoi(next().c_str())); }   // fly over a generated map
        else if (a == "--map-km") prefs_.mapSizeKm = std::atoi(next().c_str());
        else if (a == "--map-climate") prefs_.mapClimate = std::atoi(next().c_str());
        else if (a == "--free-world") prefs_.freeWorld = 1;   // free rides on the generated map (pick the start on the Ride page)
    }
    if (forceKind == "route") setupKind_ = RideKind::Route;
    else if (forceKind == "workout") setupKind_ = RideKind::Workout;
    else if (forceKind == "test") setupKind_ = RideKind::Test;
    else if (forceKind == "free") setupKind_ = RideKind::Free;
    if (forcePage >= 0) page_ = std::clamp(forcePage, 0, PAGE_COUNT - 1);
    if (forceHour >= 0) { prefs_.todMode = 1; prefs_.todHour = forceHour; }
    if (!forceWeather.empty()) {
        if (auto k = weather::kindFromId(forceWeather)) { prefs_.weatherMode = 2; prefs_.weatherKind = forceWeather; }
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI | FLAG_VSYNC_HINT | FLAG_MSAA_4X_HINT);
    InitWindow(1600, 900, "CoreRay");
    setAppIcon();
    for (const std::string& base : {std::string(GetApplicationDirectory()) + "assets/ui/", std::string("assets/ui/"), std::string(GetApplicationDirectory()) + "../Resources/assets/ui/"})
        if (FileExists((base + "settings-cog-64.png").c_str())) {
            {   // white glyph + alpha, so the HUD can tint it any ink (light or dark glass)
                Image im = LoadImage((base + "settings-cog-64.png").c_str());
                ImageFormat(&im, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
                ImageColorReplace(&im, Color{0, 0, 0, 0}, Color{255, 255, 255, 0});
                auto* px = static_cast<Color*>(im.data);
                for (int i = 0; i < im.width * im.height; ++i) px[i] = Color{255, 255, 255, px[i].a};
                cogTex_ = LoadTextureFromImage(im);
                UnloadImage(im);
            }
            GenTextureMipmaps(&cogTex_);
            SetTextureFilter(cogTex_, TEXTURE_FILTER_TRILINEAR);
            break;
        }
    SetWindowMinSize(1180, 700);
    SetExitKey(KEY_NULL);
    ui::loadFonts();
    sfx::init();
    sfx::setMuted(!prefs_.sound);

    db_.open(dbPath);
    db_.seedIfEmpty(prof_.ftp);
    builtin_ = wk::builtinWorkouts();
    custom_ = store::loadCustomWorkouts();
    sim_ = std::make_unique<sim::SimTrainer>(prof_);
    sim_->set(prefs_.sim);
    if (demoHistory && db_.rideSummaries().empty()) {
        // fill a scratch database with three months of simulated training (screenshots / trying the stats)
        auto rs = db_.routes();
        Date today = Date::today();
        uint32_t rnd = 99;
        auto R = [&]() { rnd = rnd * 1664525u + 1013904223u; return (rnd >> 8) / float(1 << 24); };
        for (int day = 84; day >= 1; --day) {
            if (R() < 0.45f) continue;
            RideSetup su;
            su.kind = R() < 0.5f ? RideKind::Route : RideKind::Workout;
            su.route = rs[size_t(R() * rs.size()) % rs.size()];
            if (su.route.distance() > 45000) su.route = rs[0];
            su.name = su.route.name;
            if (su.kind == RideKind::Workout) { su.workout = builtin_[size_t(R() * builtin_.size()) % builtin_.size()]; su.name = su.workout->name; su.routeIsScenery = true; }
            Date d = today.plus(-day);
            char ts[40];
            std::snprintf(ts, sizeof ts, "%sT18:%02d:00+00:00", d.str().c_str(), int(R() * 50));
            Profile p = prof_;
            p.ftp = 200 + (84 - day) * 0.45;
            RideEngine e(p, su, ts);
            sim::SimTrainer st(p);
            sim::Settings ss;
            ss.effortW = p.ftp * (0.72 + R() * 0.25);
            st.set(ss);
            double dur = 2400 + R() * 3000;
            for (double t = 0; t < dur && !e.routeComplete() && !e.workoutComplete(); t += 1.0) {
                auto in = st.update(1.0, p);
                if (auto c = e.tick(1.0, in)) st.command(*c);
            }
            auto rec = e.finish(ts, dates::newId());
            db_.saveRide(rec);
        }
        db_.addFtp({today.plus(-60).str(), 215, "Test"});
    }
    loadData();
    if (!forceRoute.empty()) {
        for (size_t i = 0; i < routes_.size(); ++i)
            if (routes_[i].id == forceRoute || std::to_string(i) == forceRoute) selRoute_ = prefs_.selRoute = routes_[i].id;
    }
    if (!assessment_ || assessment_->date != Date::today().str()) reassessFtp(std::nullopt, true);
    hrAlertsOn_ = prof_.hrAlerts;
    cam_ = CamMode(std::clamp(prefs_.camera, 0, int(CamMode::COUNT) - 1));
    view3d_ = prefs_.startIn3d && !force2d;
    ftpOnOpen_ = prof_.ftp;
    if (const Route* r = findRoute(selRoute_)) gen_.terrain = r->genParams ? r->genParams->terrain : Terrain::Rolling;
    genName_.text = "My route";
    builderName_.text = "Custom climb";
    builderText_.text = "2km 0%\n3km 6%\n1km 9%\n2km -4%";
    stravaId_.text = db_.meta("strava_client_id").value_or("");
    placeBox_.text = prefs_.place;

    renderer_.setRenderScale(renderScaleOf(prefs_.renderScale));
    renderer_.init(prefs_.quality);
    wx_ = weather::Sim(uint32_t(std::time(nullptr) / 3600), false);
    if (prefs_.weatherMode == 1) fetchLiveWeather();
    if (const char* ws = std::getenv("CORERAY_SEED")) worldSeed_ = uint32_t(std::strtoul(ws, nullptr, 10));  // dev: fixed world
    if (const char* h = std::getenv("CORERAY_HILL")) hillNext_ = Hilliness(std::clamp(std::atoi(h), 0, 4));
    if (const char* n = std::getenv("CORERAY_NET")) netNext_ = std::atoi(n);
    if (forceCam >= 0) cam_ = CamMode(forceCam % int(CamMode::COUNT));
    updateWorld(0.f, 0);
    // settle the weather instantly at startup so screenshots / first frames look right
    wx_.update(0.1, 0, 0, true, true);
    if (wx_.state.rain > 0.3) wx_.wet = 0.9;
    if (wx_.state.snow > 0.3) wx_.snowCover = 0.9;

    ble_.setAutoConnect(prefs_.trainerId, prefs_.hrId, prefs_.shifterId);
    ble_.startScan(45);
    if (prefs_.scenery == 1 || prefs_.freeWorld >= 1) requestMap();
    if (autoRide) startRide();
    if (autoExplore) startExplore();
    if (eng_ && skipAhead < 0 && course_) {
        // dev: "--ahead -N" = just before the N-th river bridge on the route
        int want = int(-skipAhead), seen = 0;
        double lastB = -1e9;
        for (double d = 300; d < course_->length(); d += 10) {
            auto p = course_->at(d);
            if (course_->field().bridgesNear(p.pos.x, p.pos.z, 6.f).empty()) continue;
            if (d - lastB < 60) continue;
            lastB = d;
            if (++seen == want) { skipAhead = std::max(1.0, d - 45); break; }
        }
        std::printf("[dev] bridge %d at %.0f m\n", want, skipAhead);
    }
    if (eng_ && skipAhead > 0) {
        // fast-forward the ride (screenshots of later parts of a route)
        TrainerInput in{300, 90, 140.0, true};
        while (eng_->distance() < skipAhead) { eng_->tick(0.5, in); extendEndless(); roamUpdate(); }
        if (endlessRide_) showRoute(eng_->route(), true);
        renderer_.preload(eng_->distance());
    }
    if (eng_ && course_ && roam_.on && std::getenv("CORERAY_JUNCTION")) {
        // dev: ride on to N m before the next junction (with a choice) and pick option K: "N[,K]"
        double before = 700, after = 0; int pick = -1;
        std::sscanf(std::getenv("CORERAY_JUNCTION"), "%lf,%d,%lf", &before, &pick, &after);
        TrainerInput in{300, 90, 140.0, true};
        for (int guard = 0; guard < 200000; ++guard) {
            roamUpdate();
            if (roam_.opts.size() > 1 && course_->length() - eng_->distance() < before) break;
            eng_->tick(0.5, in);
        }
        if (pick >= 0 && pick < int(roam_.opts.size())) roam_.sel = pick;
        for (double end = eng_->distance() + after; eng_->distance() < end;) { eng_->tick(0.5, in); roamUpdate(); }
        renderer_.preload(eng_->distance());
        std::printf("[dev] junction %.0f m ahead, %zu ways\n", course_->length() - eng_->distance(), roam_.opts.size());
    }
    if (eng_ && course_ && std::getenv("CORERAY_RELIEF")) {
        auto p = course_->at(eng_->distance());
        std::printf("[dev] rider at %.1f %.1f %.1f, relief -> %s\n", p.pos.x, p.pos.y, p.pos.z, std::getenv("CORERAY_RELIEF"));
        terrain::writeRelief(course_->field(), p.pos.x - 1000, p.pos.z - 1000, 2000, 4, std::getenv("CORERAY_RELIEF"));
        // where along the road is water close by? (for screenshots)
        for (double d = 0; d < course_->length(); d += 50) {
            auto q = course_->at(d);
            float rx = -std::cos(q.heading), rz = std::sin(q.heading);
            for (float lat : {-60.f, -35.f, 35.f, 60.f}) {
                auto sm = course_->field().macro(q.pos.x + rx * lat, q.pos.z + rz * lat);
                if (sm.water > sm.h + 0.5f) { std::printf("[dev] water %s at %.0f m, lateral %.0f\n", sm.waterW > 500 ? "lake" : "river", d, lat); break; }
            }
        }
        int nans = 0, wet = 0;
        for (int k = 0; k < 100; ++k)
            for (int i = 0; i < 100; ++i) {
                auto sm = course_->field().sample(p.pos.x - 400 + i * 8, p.pos.z - 400 + k * 8);
                nans += std::isnan(sm.h); wet += sm.water > sm.h;
            }
        std::printf("[dev] nan %d wet %d / 10000\n", nans, wet);
    }
    if (openDetail && !rides_.empty()) { page_ = PActivities; selRide_ = rides_[0].id; detail_ = db_.ride(selRide_); }
    if (openDrawer && eng_) { drawerOpen_ = true; drawerTab_ = 2; }

    int frame = 0;
    while (!WindowShouldClose()) {
        float dt = std::min(GetFrameTime(), 0.1f);
        if (!shotPath.empty()) dt = 1.f / 30.f;
        {
            std::vector<std::function<void()>> q;
            { std::lock_guard<std::mutex> lk(mainQm_); q.swap(mainQ_); }
            for (auto& f : q) f();
        }
        ui::beginFrame();
        SetMouseCursor(MOUSE_CURSOR_DEFAULT);
        handleDroppedFiles();
        frameUpdate(dt);
        if (sectorAt >= 0 && frame == sectorAt && eng_) {
            SectorSpec sp;
            sp.kind = SectorKind::Climb; sp.lengthM = 1200; sp.gradePct = 7; sp.inM = 120;
            eng_->addSector(sp);
        }
        if (autoSummary && screen_ == Screen::Ride && frame == shotFrames - 3) endRide(true);
        if (strikeAt >= 0 && frame == strikeAt && course_) {
            auto st = renderer_.lightning(-1000.f);
            if (!std::getenv("CORERAY_NOFLASH")) wx_.flash = 0.9;
            sfx::thunder(st.dist / 343.f, std::clamp(1.25f - st.dist / 6500.f, 0.3f, 1.f), st.pan);
        }

        BeginDrawing();
        ClearBackground(BG);
        bool draw3d = (screen_ == Screen::Ride && view3d_) || screen_ == Screen::Explore;
        const bool profiling = draw3d && course_ && prefs_.showPerf;
        prof::setEnabled(profiling);
        if (profiling) prof::beginFrame();
        if (draw3d && course_) renderer_.render(scene(), dt);
        // appearance: the theme (the HUD glass can follow the sun) and the UI zoom for this screen
        {
            bool hudDark = prefs_.hudStyle == 2 || (prefs_.hudStyle == 0 && ui::themeIsDark(prefs_.theme)) ||
                           (prefs_.hudStyle == 3 && sky_.sunElevation < -1.f);
            ui::applyTheme(prefs_.theme, prefs_.accent, hudDark);
        }
        const bool hudScreen = screen_ == Screen::Ride || screen_ == Screen::Explore;
        ui::pushScale(hudScreen ? ui::autoScale(1000, 640, prefs_.hudSize / 100.f) : ui::autoScale(1180, 740, prefs_.uiSize / 100.f));
        if (screen_ == Screen::Home) drawShell();
        else if (screen_ == Screen::Explore) { drawExplore(); if (prefs_.showFps || prefs_.showPerf) drawPerf(); }
        else if (screen_ == Screen::Ride) {
            // a window on top: the HUD beneath must not take its clicks
            if (devicesModal_ || pauseMenu_) ui::setInputBlocked(true);
            if (!view3d_) drawDashboard2d();
            else if (!hudHidden_) { drawRideHud(); drawRoamChoices(); }
            if (drawerOpen_) drawEditDrawer();
            if (testPanel_ && simActive()) {
                float W = ui::screenW();
                Rectangle r{W - 400, 140, 380, 470};
                panel(r, PANEL);
                drawSimPanel({r.x + 16, r.y + 12, r.width - 32, r.height - 24}, true);
                if (CheckCollisionPointRec(ui::mouseLogical(), r)) ui::setInputBlocked(false);
            }
            if (devicesModal_ || pauseMenu_) ui::setInputBlocked(false);
            if (pauseMenu_) drawPauseMenu();
            else if (devicesModal_) drawDevicesModal();
            if (helpOpen_) drawHelp();
            if (draw3d && (prefs_.showFps || prefs_.showPerf)) drawPerf();
        } else drawSummary();
        drawToasts();
        ui::popScale();
        if (profiling) { prof::mark(prof::Hud); prof::endFrame(); }
        EndDrawing();

        ++frame;
        if (!shotPath.empty() && frame >= shotFrames) {
            TakeScreenshot(shotPath.c_str());
            break;
        }
    }
    if (screen_ == Screen::Ride && eng_ && eng_->elapsedS() > 60) endRide(true);  // never lose a ride on quit
    if (prof_.ftp != ftpOnOpen_) db_.addFtp({Date::today().str(), prof_.ftp, "Manual"});
    prefs_.camera = int(cam_);
    prefs_.sim = sim_->settings();
    prefs_.selRoute = selRoute_;
    store::save(prof_, prefs_);
    for (int i = 0; i < 50 && g_async.load() > 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    mapPreview_.release();
    if (cogTex_.id) UnloadTexture(cogTex_);
    renderer_.shutdown();
    sfx::shutdown();
    ui::unloadFonts();
    CloseWindow();
    return 0;
}

// ------------------------------------------------------------------ files dropped on the window
void App::handleDroppedFiles() {
    if (!IsFileDropped()) return;
    FilePathList list = LoadDroppedFiles();
    for (unsigned i = 0; i < list.count; ++i) {
        std::string path = list.paths[i];
        std::ifstream f(path, std::ios::binary);
        std::stringstream ss;
        ss << f.rdbuf();
        std::string name = path.substr(path.find_last_of("/\\") + 1);
        std::string lower = name;
        for (auto& c : lower) c = char(std::tolower((unsigned char)c));
        if (lower.size() > 4 && lower.substr(lower.size() - 4) == ".gpx") {
            Route r;
            std::string err;
            if (routeFromGpx(ss.str(), "", r, err)) {
                db_.saveRoute(r);
                routes_ = db_.routes();
                selRoute_ = r.id;
                page_ = PRoutes;
                routeView_ = 0;
                char b[200];
                std::snprintf(b, sizeof b, "Imported %s: %.1f km, %.0f m climbing", r.name.c_str(), r.distance() / 1000, r.stats().gainM);
                toast(b, GOOD);
            } else toast(name + ": " + err, BAD);
            continue;
        }
        wk::Workout w;
        std::string err;
        if (wk::importFile(name, ss.str(), prof_.ftp, w, err)) {
            w.id.clear();
            if (store::saveCustomWorkout(w, err)) {
                custom_ = store::loadCustomWorkouts();
                for (auto& c : custom_) if (c.name == w.name) wkSelected_ = c.id;
                page_ = PWorkouts;
                toast("Imported workout: " + w.name, GOOD);
            } else toast(err, BAD);
        } else toast(name + ": " + err, BAD);
    }
    UnloadDroppedFiles(list);
}

// ------------------------------------------------------------------ ride flow
void App::startEvent(const Event& e) {
    pendingEvent_ = e.id;
    switch (e.target.type) {
        case EventTarget::Route:
            setupKind_ = RideKind::Route;
            if (findRoute(e.target.routeId)) selRoute_ = e.target.routeId;
            setupLaps_ = std::max(1, e.target.laps);
            break;
        case EventTarget::Workout:
            setupKind_ = RideKind::Workout;
            selWorkout_ = e.target.workoutId;
            if (e.target.workoutRouteId && findRoute(*e.target.workoutRouteId)) { selRoute_ = *e.target.workoutRouteId; sceneryEndless_ = false; }
            break;
        case EventTarget::Test:
            setupKind_ = RideKind::Test;
            selTest_ = e.target.test;
            break;
    }
    startRide();
}

// endless road: keep a few kilometres built ahead, each new one shaped as the rider asked
void App::extendEndless() {
    if (!endlessRide_ || !eng_) return;
    const Route& r = eng_->route();
    if (r.distance() - eng_->distance() >= ENDLESS_AHEAD_KM * 1000.0) return;
    double ele = r.points.empty() ? 100 : r.points.back().ele;
    double net = std::max(double(netNext_), 15.0 - ele);  // never below sea level
    eng_->extendRoute(endlessKm(endless_, net, hillNext_));
}

void App::startRide() {
    const Route* r = findRoute(selRoute_);
    if (!r) { toast("No route available", BAD); return; }
    RideSetup su;
    su.kind = setupKind_;
    su.simulated = simActive();
    su.eventId = pendingEvent_;
    pendingEvent_.reset();
    endlessRide_ = false;
    roam_ = Roam{};
    rideMapStart_ = MapStart{};
    auto scenery = [&]() {
        // free rides choose their own world on the Ride page; workouts / tests follow Settings
        bool generated = setupKind_ == RideKind::Free ? prefs_.freeWorld >= 1 : prefs_.scenery == 1;
        if (generated) {   // free roaming on the generated map: choose the way at junctions
            Route mr;
            if (roamStart(mr)) { su.route = mr; su.routeIsScenery = true; return; }
        }
        if (sceneryEndless_ || !r->isLoop || setupKind_ == RideKind::Free) {
            // the endless road: a fresh seed, a few kilometres to start, then built ahead as you ride
            std::mt19937_64 rng(worldSeed_ ? worldSeed_ : std::random_device{}());
            endless_ = EndlessGen{};
            endless_.seed = rng() | 1;
            double startEle = 60 + double(rng() % 500);
            su.route = endlessStart(endless_, startEle, ENDLESS_AHEAD_KM + 1, double(netNext_), hillNext_);
            endlessRide_ = true;
        } else su.route = *r;
        su.routeIsScenery = true;
    };
    switch (setupKind_) {
        case RideKind::Route:
            if (prefs_.scenery == 1) {   // the generated map: a loop about the route's length
                Route mr;
                if (mapRoute(std::clamp(r->distance() / 1000.0, 10.0, 60.0), mr)) {
                    su.route = mr;
                    su.name = mr.name;
                    su.laps = setupLaps_;
                    break;
                }
            }
            su.route = *r;
            su.name = r->name;
            su.laps = r->isLoop ? setupLaps_ : 1;
            if (raceGhost_)
                if (auto best = db_.bestRouteRide(r->id, su.laps)) su.ghost = GhostTrace::fromRecord(*best);
            break;
        case RideKind::Workout: {
            const wk::Workout* w = findWorkout(selWorkout_);
            if (!w) return;
            su.workout = *w;
            su.name = w->name;
            scenery();
            break;
        }
        case RideKind::Test:
            su.test = selTest_;
            for (auto& t : wk::testCatalogue()) if (t.kind == selTest_) su.name = t.name;
            scenery();
            break;
        case RideKind::Free:
            su.name = "Free ride";
            su.freeMode = setupFreeMode_;
            su.goal = setupGoal_;
            scenery();
            break;
    }
    rideStartedAt_ = dates::nowRfc3339();
    eng_ = std::make_unique<RideEngine>(prof_, su, rideStartedAt_);
    if (setupKind_ == RideKind::Workout && workoutResistance_) eng_->toggleErg();
    eng_->drainEvents();
    rideStart_ = std::time(nullptr);
    screen_ = Screen::Ride;
    drawerOpen_ = pauseMenu_ = helpOpen_ = false;
    crank_ = 0; lean_ = 0; stand_ = 0; tuck_ = 0; wheelAngle_ = 0; wheelDist_ = -1; lastGear_ = -1; devicesModal_ = false;
    flow_ = flowMean_ = flowVar_ = flowStreak_ = flowBest_ = 0;
    lastAlertT_ = -1e9; lastHrState_ = 0;
    routeSectorsSeen_ = 0;
    cycleHours_ = 0;
    // fresh weather for every ride (the "changing" timeline is seeded per ride)
    auto st = eng_->route().stats();
    wx_ = weather::Sim(uint32_t(std::time(nullptr)), st.maxEle > 1500);
    wxElapsed_ = 0;
    updateWorld(0.f, 0);
    wx_.update(0.1, 0, 0, sky_.sunElevation > 0, true);
    if (wx_.state.rain > 0.3) wx_.wet = 0.9;
    if (wx_.state.snow > 0.3) wx_.snowCover = 0.9;
    showRoute(eng_->route(), true);
    store::save(prof_, prefs_);
    toast(su.name, TEXT);
    if (ble_.trainer().state != LinkState::Connected && !simActive())
        toast("No trainer connected - Devices, or turn on the simulator", WARN);
}

wmap::Params App::mapParams() const {
    wmap::Params p;
    p.seed = uint32_t(std::max(1, prefs_.mapSeed));
    p.size = 1024.f * std::clamp(prefs_.mapSizeKm, 5, 60);   // 20 -> 20.48 km: whole 20 m cells
    p.climate = std::clamp(prefs_.mapClimate, 0, 3);
    return p;
}

void App::requestMap() {
    const wmap::Params P = mapParams();
    std::string key = std::to_string(P.seed) + "_" + std::to_string(int(P.size / 1024)) + "km_c" + std::to_string(P.climate);
    if (mapFuture_.valid() && mapFor_ == key) return;
    mapFor_ = key;
    mapReqAt_ = GetTime();
    const uint32_t seed = P.seed;
    std::filesystem::path dir = std::filesystem::path(Db::defaultPath()).parent_path() / "worlds";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string path = (dir / ("map_" + key + "_v" + std::to_string(wmap::VERSION) + ".bin")).string();
    mapFuture_ = std::async(std::launch::async, [P, path]() -> std::shared_ptr<const wmap::Map> {
        auto m = std::make_shared<wmap::Map>();
        if (m->load(path) && m->params.seed == P.seed && m->params.size == P.size && m->params.climate == P.climate) return m;
        auto g = wmap::generate(P);
        g->save(path);
        std::fprintf(stderr, "[map] generated map %u (%.0f km): %d towns, %.0f km of road (%d tunnels, %d bridges), %d rivers, %d lakes\n", P.seed, P.size / 1024, g->stats.towns, g->stats.roadKm, g->stats.tunnels, g->stats.bridges, g->stats.rivers, g->stats.lakes);
        return g;
    }).share();
}

bool App::mapRoute(double km, Route& out) {
    requestMap();
    map_ = mapFuture_.get();
    if (!map_ || map_->roads.empty()) return false;
    std::mt19937 rng(worldSeed_ ? worldSeed_ : uint32_t(std::random_device{}()));
    std::uniform_real_distribution<float> U(-0.3f, 0.3f);
    std::string name;
    float sx = U(rng) * map_->params.size, sz = U(rng) * map_->params.size;
    if (const char* at = std::getenv("CORERAY_MAPSTART")) std::sscanf(at, "%f,%f", &sx, &sz);   // dev: start near here
    mapPath_ = map_->loop(float(km), rng(), sx, sz, &name);
    if (mapPath_.size() < 10) return false;
    std::fprintf(stderr, "[map] %s: %.1f km from (%.0f, %.0f)\n", name.c_str(), km, mapPath_[0].x, mapPath_[0].z);
    {   // where this ride begins (no single road: the loop is found again by position and heading)
        rideMapStart_ = MapStart{};
        rideMapStart_.set = true;
        rideMapStart_.x = mapPath_[0].x;
        rideMapStart_.z = mapPath_[0].z;
        float hx = mapPath_[4].x - mapPath_[0].x, hz = mapPath_[4].z - mapPath_[0].z, hl = std::max(1e-3f, std::hypot(hx, hz));
        rideMapStart_.hx = hx / hl;
        rideMapStart_.hz = hz / hl;
    }
    out = Route{};
    out.id = "map-" + std::to_string(map_->params.seed);
    out.name = name;
    out.isLoop = true;
    out.source = RouteSource::Generated;
    double d = 0;
    for (size_t j = 0; j < mapPath_.size(); ++j) {
        if (j) d += std::hypot(double(mapPath_[j].x - mapPath_[j - 1].x), double(mapPath_[j].z - mapPath_[j - 1].z));
        out.points.push_back({d, double(mapPath_[j].y)});
    }
    d += std::hypot(double(mapPath_.front().x - mapPath_.back().x), double(mapPath_.front().z - mapPath_.back().z));
    out.points.push_back({d, double(mapPath_.front().y)});
    return true;
}

void App::endRide(bool save) {
    if (!eng_) return;
    ble_.send(TrainerCommand::sim(0, prof_.crr, 0.5 * phys::airDensity(0) * prof_.cda));
    std::string id = dates::newId();
    RideRecord rec = eng_->finish(dates::nowRfc3339(), id);
    eng_.reset();
    endlessRide_ = false;
    course_.reset();
    renderer_.setCourse(nullptr);  // the world is gone until the next ride
    worldRouteKey_.clear();
    savedDir_.clear();
    newPb_ = ftpApplied_ = false;
    if (!save || rec.samples.size() < 10) {
        screen_ = Screen::Home;
        return;
    }
    db_.saveRide(rec);
    savedDir_ = exporter::saveRide(rec.summary, rec.samples, rideStart_, rec.summary.name);
    if (rec.summary.kind == RideKind::Route && rec.summary.completed && rec.summary.routeId) {
        auto best = db_.bestRouteRide(*rec.summary.routeId, rec.summary.laps);
        newPb_ = best && best->summary.id == id;
        if (newPb_) { rec.summary.personalBest = true; db_.updateSummary(rec.summary); }
    }
    refreshHistory();
    afterSave(rec);
    summaryRec_ = rec;
    screen_ = Screen::Summary;
    sfx::play(sfx::Done);
}

void App::afterSave(RideRecord& rec) {
    if (isValidFtpRide(rec.summary)) {
        std::optional<Suggestion> before = assessment_ ? std::optional<Suggestion>(assessment_->suggestion) : std::nullopt;
        reassessFtp(rec.summary.id, true);
        (void)before;
    }
    auto st = strava::status(db_);
    if (st.connected && st.autoUpload && !rec.summary.simulated) stravaUpload(rec.summary.id, true);
}

void App::stravaUpload(const std::string& rideId, bool quiet) {
    auto it = uploadState_.find(rideId);
    if (it != uploadState_.end() && (it->second == "uploading" || it->second == "processing")) return;
    auto rec = db_.ride(rideId);
    if (!rec) return;
    if (rec->summary.stravaActivityId) { toast("Already on Strava", SKY); return; }
    uploadState_[rideId] = "uploading";
    if (!quiet) toast("Uploading to Strava...", SKY);
    RideRecord copy = *rec;
    async([this, copy, rideId]() mutable {
        std::string err;
        auto act = strava::upload(db_, copy, err, [this, rideId](const std::string& st) { onMain([this, rideId, st] { uploadState_[rideId] = st; }); });
        onMain([this, rideId, act, err, copy]() mutable {
            if (act) {
                // re-read: the ride may have been renamed / annotated while the upload ran
                if (auto cur = db_.ride(rideId)) { cur->summary.stravaActivityId = act; db_.updateSummary(cur->summary); }
                else { copy.summary.stravaActivityId = act; db_.updateSummary(copy.summary); }
                uploadState_[rideId] = "done";
                if (detail_ && detail_->summary.id == rideId) detail_->summary.stravaActivityId = act;
                if (summaryRec_ && summaryRec_->summary.id == rideId) summaryRec_->summary.stravaActivityId = act;
                refreshHistory();
                toast("Uploaded to Strava", GOOD);
            } else {
                uploadState_[rideId] = err;
                toast(err, BAD);
            }
        });
    });
}

void App::fetchLiveWeather() {
    if (liveFetching_) return;
    liveFetching_ = true;
    liveFetchedAt_ = GetTime();
    double lat = prefs_.lat, lon = prefs_.lon;
    std::string place = prefs_.place;
    async([this, lat, lon, place] {
        std::string err;
        auto l = liveweather::fetch(lat, lon, place, err);
        onMain([this, l, err] {
            liveFetching_ = false;
            if (l) live_ = l;
            else if (prefs_.weatherMode == 1) toast(err, WARN);
        });
    });
}

TrainerInput App::currentInput(float dt) {
    if (simActive()) return sim_->update(dt, prof_);
    return ble_.input();
}

// ------------------------------------------------------------------ world: sky + weather
static std::time_t localMidnight() {
    std::time_t now = std::time(nullptr);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &now);
#else
    localtime_r(&now, &lt);
#endif
    lt.tm_hour = lt.tm_min = lt.tm_sec = 0;
    lt.tm_isdst = -1;
    return std::mktime(&lt);
}

void App::cycleTime() {
    // presets by where the sun is today at your location, then real time, then the day/night cycle
    struct Pre { const char* name; double elev; bool rising; };
    static const Pre pre[] = {{"Morning", 14, true}, {"Midday", 99, true}, {"Golden hour", 7, false}, {"Dusk", -4.5, false}, {"Night", -99, false}};
    std::time_t mid = localMidnight();
    auto findHour = [&](const Pre& p) {
        double best = 12, bestErr = 1e9, maxEl = -1e9;
        double prevEl = sky::sunPosition(mid, prefs_.lat, prefs_.lon).elevation;
        for (int m = 5; m < 24 * 60; m += 5) {
            double el = sky::sunPosition(mid + m * 60, prefs_.lat, prefs_.lon).elevation;
            bool rising = el > prevEl;
            prevEl = el;
            if (p.elev > 90) { if (el > maxEl) { maxEl = el; best = m / 60.0; } continue; }
            if (p.elev < -90) { if (-el > bestErr * -1 || bestErr > 1e8) { if (bestErr > 1e8 || el < -bestErr) { bestErr = -el; best = m / 60.0; } } continue; }
            if (rising != p.rising) continue;
            double e = std::fabs(el - p.elev);
            if (e < bestErr) { bestErr = e; best = m / 60.0; }
        }
        if (p.elev < -90) best = std::fmod(best, 24.0);
        return best;
    };
    static int idx = -1;
    if (prefs_.todMode == 0) { prefs_.todMode = 2; cycleHours_ = 0; toast("Time: day / night cycle"); }
    else if (prefs_.todMode == 2) { prefs_.todMode = 1; idx = 0; prefs_.todHour = findHour(pre[0]); toast(std::string("Time: ") + pre[0].name); }
    else {
        ++idx;
        if (idx >= 5) { prefs_.todMode = 0; toast("Time: real time at " + prefs_.place); idx = -1; }
        else { prefs_.todHour = findHour(pre[idx]); toast(std::string("Time: ") + pre[idx].name); }
    }
    store::save(prof_, prefs_);
}

void App::cycleWeather() {
    using weather::Kind;
    if (prefs_.weatherMode == 0) { prefs_.weatherMode = 1; fetchLiveWeather(); toast("Weather: live at " + prefs_.place); }
    else if (prefs_.weatherMode == 1) { prefs_.weatherMode = 2; prefs_.weatherKind = weather::kindId(Kind::Clear); }
    else {
        auto k = weather::kindFromId(prefs_.weatherKind).value_or(Kind::Clear);
        int n = int(k) + 1;
        if (n >= int(Kind::COUNT)) { prefs_.weatherMode = 0; toast("Weather: changing"); store::save(prof_, prefs_); return; }
        prefs_.weatherKind = weather::kindId(Kind(n));
    }
    if (prefs_.weatherMode == 2) toast(std::string("Weather: ") + weather::kindLabel(*weather::kindFromId(prefs_.weatherKind)));
    store::save(prof_, prefs_);
}

void App::updateWorld(float dt, double dist) {
    // ---- time
    std::time_t t;
    if (prefs_.todMode == 0) t = std::time(nullptr);
    else {
        cycleHours_ += prefs_.todMode == 2 ? dt * 24.0 / std::max(1.0, prefs_.cycleMinutes * 60.0) : 0.0;
        double h = std::fmod(prefs_.todHour + cycleHours_, 24.0);
        t = localMidnight() + std::time_t(h * 3600);
    }
    auto sun = sky::sunPosition(t, prefs_.lat, prefs_.lon);
    auto moon = sky::moonPosition(t, prefs_.lat, prefs_.lon);
    double v[3];
    sky::skyVector(sun, v);
    sky_.sunDir = Vector3Normalize({float(v[0]), float(v[1]), float(v[2])});
    sky_.sunElevation = float(sun.elevation);
    sky::skyVector(moon.pos, v);
    sky_.moonDir = Vector3Normalize({float(v[0]), float(v[1]), float(v[2])});
    sky_.moonElevation = float(moon.pos.elevation);
    sky_.moonIllum = float(moon.illumination);
    sky_.moonWaxing = moon.waxing;
    sky_.tod = float(sky::todFromElevation(sun.elevation));

    // ---- weather
    wx_.mode = prefs_.weatherMode == 0 ? weather::Mode::Auto : prefs_.weatherMode == 1 ? weather::Mode::Live : weather::Mode::Fixed;
    wx_.fixed = weather::kindFromId(prefs_.weatherKind).value_or(weather::Kind::Fair);
    wx_.live = live_;
    if (prefs_.weatherMode == 1 && GetTime() - liveFetchedAt_ > 15 * 60) fetchLiveWeather();
    double eleReal = course_ ? course_->elevationAt(dist) + course_->eleOffset() : 0;
    double snowiness = std::clamp((eleReal - 1300) / 1100, 0.0, 1.0);
    wxElapsed_ += dt;
    auto ev = wx_.update(dt, wxElapsed_, snowiness, sun.elevation > 0);
    if (ev.lightning) {
        // thunder from where the strike is: sound takes ~3 s per km, and closer is louder
        auto st = renderer_.lightning(float(GetRandomValue(0, 628)) / 100.f);
        float loud = std::clamp(1.25f - st.dist / 6500.f, 0.3f, 1.f);
        sfx::thunder(st.dist / 343.f, float(ev.strength) * loud, st.pan);
    }
    if (ev.changed && dt > 0) toast(std::string("Weather: ") + weather::kindLabel(*ev.changed), SKY);
    // ---- temperature and wind for the weather card (live: as reported)
    {
        std::time_t now = std::time(nullptr);
        std::tm lt{};
#ifdef _WIN32
        localtime_s(&lt, &now);
#else
        localtime_r(&now, &lt);
#endif
        double season = weather::seasonalMean(prefs_.lat, lt.tm_yday + 1);
        double eleForTemp = eleReal;
        // on a generated map its own climate decides (deserts warm, cold maps cold): the map's
        // mean annual temperature at the rider, shifted by the season at home
        if (map_ && course_ && course_->world() && worldRoute_.id.rfind("map-", 0) == 0) {
            auto rp = course_->at(dist);
            double annualHome = 30.0 - 0.38 * std::fabs(prefs_.lat);
            season = map_->sample(rp.pos.x, rp.pos.z).temp + (season - annualHome);
            eleForTemp = 0;   // the map's temperature is already for that height
        }
        wxSeasonC_ = season;
        wxEleForTemp_ = eleForTemp;
        wxSunEle_ = sun.elevation;
        if (wx_.mode == weather::Mode::Live && live_) { wxTempC_ = live_->temperatureC; wxWindKmh_ = live_->windKmh; }
        else {
            double target = weather::temperature(season, sun.elevation, wx_.currentKind(wxElapsed_), eleForTemp);
            wxTempC_ += (target - wxTempC_) * std::min(1.0, double(dt) / 90.0);   // the air warms and cools gradually
            if (dt <= 0 || wxElapsed_ < 3 || std::fabs(target - wxTempC_) > 15) wxTempC_ = target;   // start of a ride: no easing in from nowhere
            wxWindKmh_ = 4 + 46 * wx_.state.wind + 18 * wx_.gust;
        }
    }
    const auto& s = wx_.state;
    fx_.cloud = float(s.cloud); fx_.dark = float(s.dark); fx_.rain = float(s.rain); fx_.snow = float(s.snow);
    fx_.fog = float(s.fog); fx_.wind = float(s.wind); fx_.storm = float(s.storm); fx_.hail = float(s.hail);
    fx_.gust = float(wx_.gust); fx_.wet = float(wx_.wet); fx_.snowCover = float(wx_.snowCover);
    fx_.flash = float(wx_.flash); fx_.rainbow = float(wx_.rainbow); fx_.windDir = float(wx_.windDir);

    // ---- ambience
    sfx::Ambience a;
    float night = std::clamp((sky_.tod - 0.7f) / 0.25f, 0.f, 1.f);
    // rain and hail are heard where they fall: on you when a shower is overhead, off to one
    // side when it's nearby (the renderer knows where the rain cells are; last frame's view)
    auto ps = renderer_.precipSound();
    a.rain = ps.rain;
    a.hail = ps.hail;
    a.rainFarL = ps.farL;
    a.rainFarR = ps.farR;
    a.wind = 0.12f + fx_.wind * 0.7f + fx_.gust * 0.3f;
    a.birds = prefs_.wildlife ? (1 - night) * (1 - ps.rain) * (1 - fx_.snow) * (sky_.sunElevation > -2 ? 1.f : 0.f) : 0.f;
    a.crickets = night * (1 - ps.rain) * (1 - fx_.snow) * (1 - fx_.snowCover);
    sfx::ambience(a);
}

SceneState App::scene() {
    SceneState s;
    s.time = animTime_;
    s.sky = sky_;
    s.wx = fx_;
    s.wildlife = prefs_.wildlife;
    s.lanterns = prefs_.lanterns;
    if (screen_ == Screen::Explore) {
        s.freeCam = true;
        s.hideRider = true;
        s.camPos = exPos_;
        s.camTarget = Vector3Add(exPos_, {std::sin(exYaw_) * std::cos(exPitch_), std::sin(exPitch_), std::cos(exYaw_) * std::cos(exPitch_)});
        s.fov = 62;
        s.dist = exDist_;
        s.speed = 0;
        s.cam = CamMode::Chase;
        return s;
    }
    if (screen_ == Screen::Ride && eng_) {
        s.dist = eng_->distance();
        s.speed = eng_->speedMps();
        s.crankAngle = crank_;
        s.wheelAngle = wheelAngle_;
        s.effort = float(std::clamp(eng_->power3s() / std::max(1.0, prof_.ftp), 0.0, 1.5));
        s.cam = cam_;
        if (auto g = eng_->ghostDistance()) s.pacerDist = *g;
        s.lean = float(lean_);
        s.stand = float(stand_);
        s.tuck = float(tuck_);
    } else {
        s.dist = homeDist_;
        s.speed = 9;
        s.crankAngle = std::fmod(animTime_ * 9.0, 2 * PI);
        s.wheelAngle = std::fmod(homeDist_ / WHEEL_ROLL_R, 2 * PI);
        s.effort = 0.4f;
        s.cam = CamMode::Cinematic;
    }
    return s;
}

// ------------------------------------------------------------------ per frame
void App::frameUpdate(float dt) {
    animTime_ += dt;
    Link t = ble_.trainer(), h = ble_.hrm();
    if (t.state == LinkState::Connected && t.id != prefs_.trainerId) {
        prefs_.trainerId = t.id; prefs_.trainerName = t.name;
        ble_.setAutoConnect(prefs_.trainerId, prefs_.hrId, prefs_.shifterId);
        store::save(prof_, prefs_);
        toast("Trainer connected: " + t.name, GOOD);
    }
    if (h.state == LinkState::Connected && h.id != prefs_.hrId) {
        prefs_.hrId = h.id; prefs_.hrName = h.name;
        ble_.setAutoConnect(prefs_.trainerId, prefs_.hrId, prefs_.shifterId);
        store::save(prof_, prefs_);
        toast("Heart rate connected: " + h.name, GOOD);
    }
    // Zwift shifter: remember it, and turn its presses into virtual gear changes
    Link sh = ble_.shifter();
    if (sh.state == LinkState::Connected && sh.id != prefs_.shifterId) {
        prefs_.shifterId = sh.id; prefs_.shifterName = sh.name;
        ble_.setAutoConnect(prefs_.trainerId, prefs_.hrId, prefs_.shifterId);
        if (!prof_.virtualShifting) { prof_.virtualShifting = true; if (eng_) eng_->setVirtualShifting(true); }
        store::save(prof_, prefs_);
        toast("Shifter connected: " + sh.name + " - virtual shifting is on", GOOD);
    }
    if (int presses = ble_.takeShifts()) {
        if (screen_ == Screen::Ride && eng_) {
            if (!eng_->virtualShifting()) {   // a press is a clear wish to shift
                prof_.virtualShifting = true;
                eng_->setVirtualShifting(true);
                store::save(prof_, prefs_);
                toast("Virtual shifting on", TEXT);
            }
            eng_->shift(presses);
        }
    }
    // a tick for every gear change, whatever made it (shifter, keys, buttons)
    if (screen_ == Screen::Ride && eng_ && eng_->virtualShifting()) {
        int g = eng_->gear();
        if (lastGear_ > 0 && g != lastGear_) sfx::play(g > lastGear_ ? sfx::GearUp : sfx::GearDown);
        lastGear_ = g;
    } else lastGear_ = -1;
    renderer_.setViewScale(screen_ == Screen::Explore ? exViewScale_ : 1.f);
    // world keys (everywhere except while typing)
    if (screen_ == Screen::Explore) {   // explore mode has its own keys (W is forward there)
        exploreFrame(dt);
        updateWorld(dt, exDist_);
        return;
    }
    if (!anyTextActive() && screen_ != Screen::Summary) {
        if (IsKeyPressed(KEY_W)) cycleWeather();
        if (IsKeyPressed(KEY_T)) cycleTime();
        if (IsKeyPressed(KEY_M)) {
            prefs_.sound = !prefs_.sound;
            sfx::setMuted(!prefs_.sound);
            toast(prefs_.sound ? "Sound on" : "Sound off");
            store::save(prof_, prefs_);
        }
    }

    if (screen_ != Screen::Ride || !eng_) {
        homeDist_ += 9.0 * dt;
        updateWorld(dt, homeDist_);
        if (simActive()) sim_->update(dt, prof_);
        return;
    }

    handleRideKeys(dt);
    TrainerInput in = currentInput(dt);
    if (auto cmd = eng_->tick(dt, in)) {
        if (simActive()) sim_->command(*cmd);
        else ble_.send(*cmd);
    }
    handleEvents();
    hrAlerts();
    updateFlow(dt);
    updateWorld(dt, eng_->distance());

    extendEndless();
    roamUpdate();
    // road edited or extended: re-lay the 3D road
    {
        const Route& r = eng_->route();
        std::string key = r.id + ":" + std::to_string(r.points.size()) + ":" + std::to_string(int(r.distance())) + ":" + (r.isLoop ? "L" : "P");
        if (key != worldRouteKey_ && course_) {
            auto c = std::make_shared<Course>(*course_);
            if (endlessRide_) {
                auto box = c->extendFrom(*course_, r);
                course_ = c;
                renderer_.extendCourse(c, box.x0, box.z0, box.x1, box.z1);
            } else {
                c->relay(r, eng_->distance());
                course_ = c;
                renderer_.updateCourse(c);
            }
            worldRouteKey_ = key;
            worldRoute_ = r;
        }
    }

    // rider animation from real effort
    double cad = eng_->cadence();
    if (cad > 5) crank_ = std::fmod(crank_ + cad / 60.0 * 2 * PI * dt, 2 * PI);
    else {
        double target = std::round((crank_ - PI / 2) / PI) * PI + PI / 2;
        crank_ += (target - crank_) * std::min(1.0, double(dt) * 3);
    }
    // Wheels roll exactly with the ground: the rider is drawn 1.4 m right of the course line,
    // and that lane is longer/shorter on bends by (1 + offset * dHeading/ds) (see riderBase).
    {
        double d = eng_->distance();
        double dd = wheelDist_ < 0 ? 0 : d - wheelDist_;
        wheelDist_ = d;
        if (std::fabs(dd) < 100 && course_) {
            double h0 = course_->at(std::max(0.0, d - 2)).heading, h1 = course_->at(d + 2).heading;
            double dh = std::remainder(h1 - h0, 2 * PI) / 4.0;  // rad per metre
            double lane = std::clamp(1.0 + 1.4 * dh, 0.5, 1.5);
            wheelAngle_ = std::fmod(wheelAngle_ + dd * lane / WHEEL_ROLL_R, 2 * PI);
        }
    }
    double g = eng_->gradePct(), p3 = eng_->power3s(), ftp = std::max(prof_.ftp, 50.0);
    double v = eng_->speedMps();
    bool wantStand = (g > 7 && p3 > 0.8 * ftp) || (p3 > 1.4 * ftp && cad > 70) || (cad > 0 && cad < 62 && p3 > 0.9 * ftp);
    standTimer_ += dt;
    if (!wantStand && g > 3 && p3 > 0.5 * ftp && std::fmod(standTimer_, 180.0) < 9) wantStand = true;
    bool wantTuck = (v > 13 && p3 < 0.4 * ftp && g < -2) || (p3 > 1.05 * ftp && std::fabs(g) < 2 && !wantStand);
    if (eng_->paused()) wantStand = wantTuck = false;
    stand_ += ((wantStand ? 1.0 : 0.0) - stand_) * std::min(1.0, double(dt) * 3);
    tuck_ += ((wantTuck ? 1.0 : 0.0) - tuck_) * std::min(1.0, double(dt) * 2);
    {
        auto a = course_->at(eng_->distance() - 6), b = course_->at(eng_->distance() + 6);
        double dh = b.heading - a.heading;
        while (dh > PI) dh -= 2 * PI;
        while (dh < -PI) dh += 2 * PI;
        double target = -std::atan(v * v * (dh / 12.0) / 9.81);
        lean_ += (target - lean_) * std::min(1.0, double(dt) * 3.0);
    }
}

void App::handleEvents() {
    for (auto& e : eng_->drainEvents()) {
        Color c = TEXT;
        if (e.kind == "step") sfx::play(sfx::StepChange);
        else if (e.kind == "lap" || e.kind == "sector" || e.kind == "sector_start") { sfx::play(sfx::Lap); c = SKY; }
        else if (e.kind == "climb") c = WARN;
        else if (e.kind == "goal" || e.kind == "route_complete" || e.kind == "workout_complete" || e.kind == "test_complete") {
            sfx::play(sfx::Done);
            c = GOOD;
        }
        toast(e.message, c);
    }
    if (auto cs = eng_->currentStep()) {
        const auto& st = eng_->steps()[cs->first];
        double rem = st.durationS - cs->second;
        static int lastBeep = -1;
        int sec = int(std::ceil(rem));
        if (sec <= 3 && sec >= 1 && sec != lastBeep && st.durationS > 20) { sfx::play(sfx::Countdown); lastBeep = sec; }
        if (sec > 3) lastBeep = -1;
    }
}

void App::hrAlerts() {
    if (!hrAlertsOn_ || !eng_->heartRate() || eng_->paused()) { lastHrState_ = 0; return; }
    if (eng_->elapsedS() < prof_.hrAlertGraceS) return;
    int z = eng_->hrZone();
    int state = z > prof_.hrAlertHi ? 1 : z < prof_.hrAlertLo ? -1 : 0;
    if (state == 1 && !prof_.hrAlertHigh) state = 0;
    if (state == -1 && !prof_.hrAlertLow) state = 0;
    double now = eng_->elapsedS();
    if (state != 0 && (state != lastHrState_ || now - lastAlertT_ >= prof_.hrAlertRepeatS)) {
        sfx::play(state > 0 ? sfx::HrHigh : sfx::HrLow);
        toast(state > 0 ? "Heart rate above Z" + std::to_string(prof_.hrAlertHi) + " - ease off" : "Heart rate below Z" + std::to_string(prof_.hrAlertLo) + " - push on",
              state > 0 ? BAD : SKY);
        lastAlertT_ = now;
    }
    lastHrState_ = state;
}

void App::handleRideKeys(float dt) {
    (void)dt;
    if (anyTextActive()) return;
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    bool alt = IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT);
    if (IsKeyPressed(KEY_ESCAPE)) {
        if (devicesModal_) devicesModal_ = false;
        else if (helpOpen_) helpOpen_ = false;
        else if (drawerOpen_) drawerOpen_ = false;
        else { pauseMenu_ = !pauseMenu_; if (pauseMenu_) eng_->pause(); }
    }
    if (IsKeyPressed(KEY_SPACE)) {
        if (eng_->paused()) { eng_->resume(); pauseMenu_ = false; } else eng_->pause();
    }
    if (IsKeyPressed(KEY_L)) eng_->lap();
    if (IsKeyPressed(KEY_H)) { hrAlertsOn_ = !hrAlertsOn_; toast(hrAlertsOn_ ? "Heart-rate alerts on" : "Heart-rate alerts off"); }
    if (IsKeyPressed(KEY_E)) drawerOpen_ = !drawerOpen_;
    if (IsKeyPressed(KEY_R) && !endlessRide_) { drawerOpen_ = true; drawerTab_ = 2; }
    if (IsKeyPressed(KEY_D)) {
        if (simActive()) testPanel_ = !testPanel_;
        else toast("Test data needs the simulator (Devices)", WARN);
    }
    if (IsKeyPressed(KEY_V)) view3d_ = !view3d_;
    if (IsKeyPressed(KEY_B)) { renderer_.setGreyboxRider(!renderer_.greyboxRider()); toast(renderer_.greyboxRider() ? "Rider: greybox body" : "Rider: kitted"); }
    if (IsKeyPressed(KEY_TAB)) hudHidden_ = !hudHidden_;
    if (IsKeyPressed(KEY_F)) { prefs_.showFps = !prefs_.showFps; store::save(prof_, prefs_); }
    if (IsKeyPressed(KEY_P)) { prefs_.showPerf = !prefs_.showPerf; store::save(prof_, prefs_); }
    if (IsKeyPressed(KEY_SLASH) || IsKeyPressed(KEY_F1)) helpOpen_ = !helpOpen_;
    if (IsKeyPressed(KEY_C)) { cam_ = CamMode((int(cam_) + 1) % int(CamMode::COUNT)); toast(std::string("Camera: ") + camModeName(cam_)); }
    if (IsKeyPressed(KEY_LEFT_BRACKET) || IsKeyPressed(KEY_PAGE_DOWN)) eng_->shift(-1);
    if (IsKeyPressed(KEY_RIGHT_BRACKET) || IsKeyPressed(KEY_PAGE_UP)) eng_->shift(1);
    if (IsKeyPressed(KEY_RIGHT)) eng_->skipStep();
    if (IsKeyPressed(KEY_BACKSPACE) && eng_->canUndoSector()) { eng_->undoSector(); toast("Road change undone"); }
    for (int dir : {1, -1}) {
        int key = dir > 0 ? KEY_UP : KEY_DOWN;
        if (!(IsKeyPressed(key) || IsKeyPressedRepeat(key))) continue;
        if (shift && simActive()) {
            auto s = sim_->settings();
            if (s.powerSource == sim::PowerSource::Fixed) s.powerW += 10 * dir; else s.effortW += 10 * dir;
            sim_->set(s);
            prefs_.sim = sim_->settings();
        } else if (alt && simActive()) {
            auto s = sim_->settings();
            s.hrSource = sim::HrSource::Fixed;
            s.hrBpm += 2 * dir;
            sim_->set(s);
            prefs_.sim = sim_->settings();
        } else if (endlessRide_ && eng_->freeGradientActive()) {
            netNext_ = std::clamp(netNext_ + 10 * dir, -120, 150);
            char b[96];
            std::snprintf(b, sizeof b, "Road ahead: %s%d m per km from km %d", netNext_ > 0 ? "+" : "", netNext_, endless_.nextKm + 1);
            toast(b);
        } else eng_->nudge(dir);
    }
    if (endlessRide_ && IsKeyPressed(KEY_G)) {  // hilliness of the road ahead
        hillNext_ = Hilliness((int(hillNext_) + 1) % 5);
        toast(std::string("Road ahead: ") + hillinessName(hillNext_));
    }
}

// ------------------------------------------------------------------ toasts
void App::drawToasts() {
    const float W = ui::screenW();
    double now = GetTime();
    while (!toasts_.empty() && now - toasts_.front().t > 4.0) toasts_.pop_front();
    float y = screen_ == Screen::Ride ? (eng_ && eng_->structured() ? 130.f : 26.f) : 24.f;
    for (auto& t : toasts_) {
        float a = float(std::clamp((4.0 - (now - t.t)) / 0.4, 0.0, 1.0)) * float(std::clamp((now - t.t) / 0.15, 0.0, 1.0));
        Vector2 m = measure(t.text, F::Body);
        Rectangle r{(W - m.x - 52) / 2 + (screen_ == Screen::Home ? 106 : 0), y, m.x + 52, 42};
        DrawRectangleRounded({r.x, r.y + 6, r.width, r.height}, 1.f, 12, Color{31, 36, 48, (unsigned char)(22 * a)});
        const bool hud = screen_ == Screen::Ride || screen_ == Screen::Explore;
        DrawRectangleRounded(r, 1.f, 12, ui::withAlpha(hud ? ui::HUD_GLASS : ui::PANEL, (unsigned char)(235 * a)));
        DrawCircle(int(r.x + 18), int(r.y + 21), 4, Color{t.c.r, t.c.g, t.c.b, (unsigned char)(255 * a)});
        text(t.text, r.x + 32, r.y + (42 - m.y) / 2, F::Body, ui::withAlpha(hud ? ui::HUD_INK : ui::TEXT, (unsigned char)(255 * a)));
        y += 48;
    }
}
