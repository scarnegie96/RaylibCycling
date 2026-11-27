// Explore mode: fly over a generated map without riding it - for building and checking
// worlds. A free camera (WASD / QE, mouse to look, wheel for speed, G to walk on the ground),
// a minimap to jump around on, a readout of what's under you, and keys to make a new map:
//   N / Shift+N  next / previous seed     [ / ]  map size     K  climate     1 / 2  weather / time
//   Tab  minimap     Esc  back
#include <cmath>
#include <ctime>
#include <cstdio>
#include <cstdlib>

#include "App.h"
#include "raymath.h"
#include "world/WorldMap.h"

using namespace ui;

namespace {
const unsigned char BCOL[wmap::BIOMES][3] = {
    {118, 165, 72}, {46, 104, 50}, {70, 104, 84}, {176, 176, 170}, {190, 180, 112},
    {196, 168, 84}, {150, 140, 86}, {232, 204, 146}, {192, 112, 72}, {84, 132, 112},
};
const char* CLIMATES[4] = {"any", "temperate", "desert", "cold"};

Image miniImage(const wmap::Map& M, int px) {
    Image im = GenImageColor(px, px, BLACK);
    Color* out = (Color*)im.data;
    const float res = M.params.size / px;
    std::vector<float> h(size_t(px) * px);
    std::vector<wmap::Sample> sm(size_t(px) * px);
    for (int y = 0; y < px; ++y)
        for (int x = 0; x < px; ++x) {
            sm[size_t(y) * px + x] = M.sample(M.x0 + (x + 0.5f) * res, M.z0 + (y + 0.5f) * res);
            h[size_t(y) * px + x] = sm[size_t(y) * px + x].h;
        }
    for (int y = 0; y < px; ++y)
        for (int x = 0; x < px; ++x) {
            const auto& s = sm[size_t(y) * px + x];
            auto H = [&](int a, int b) { return h[size_t(std::clamp(b, 0, px - 1)) * px + std::clamp(a, 0, px - 1)]; };
            float dx = (H(x + 1, y) - H(x - 1, y)) / (2 * res), dz = (H(x, y + 1) - H(x, y - 1)) / (2 * res);
            float l = std::sqrt(dx * dx + 1 + dz * dz);
            float sh = std::clamp(0.35f + 0.85f * (-dx * -0.55f + 0.7f - dz * -0.45f) / l, 0.f, 1.3f);
            float r = 0, g = 0, b = 0;
            for (int q = 0; q < wmap::BIOMES; ++q) { r += BCOL[q][0] * s.w[q]; g += BCOL[q][1] * s.w[q]; b += BCOL[q][2] * s.w[q]; }
            r += (226 - r) * s.sand * 0.6f; g += (200 - g) * s.sand * 0.6f; b += (150 - b) * s.sand * 0.6f;
            if (s.temp < -1.5f) r = g = b = 240;
            r *= sh; g *= sh; b *= sh;
            if (s.water > s.h) { r = 60; g = 110; b = 190; }
            if (s.road < 6.f) { r = g = b = 40; }
            out[y * px + x] = {(unsigned char)std::clamp(int(r), 0, 255), (unsigned char)std::clamp(int(g), 0, 255), (unsigned char)std::clamp(int(b), 0, 255), 255};
        }
    for (const auto& t : M.towns) {   // towns
        int cx = int((t.x - M.x0) / res), cy = int((t.z - M.z0) / res);
        ImageDrawCircle(&im, cx, cy, t.rank == 2 ? 3 : 2, {250, 90, 60, 255});
    }
    return im;
}
}  // namespace

void App::startExplore() {
    requestMap();
    screen_ = Screen::Explore;
    exPending_ = true;
    drawerOpen_ = pauseMenu_ = helpOpen_ = false;
    toast("Explore: WASD / QE to fly, drag to look, wheel for speed, G walk, N new map, Tab minimap, Esc back", TEXT);
    if (std::getenv("CORERAY_EXPLOREAT")) {   // dev runs: wait for the map so screenshots show it
        mapFuture_.wait();
        exPending_ = false;
        exploreLoad();
    }
}

void App::exploreLoad() {
    roam_ = Roam{};
    Route mr;
    if (!mapRoute(12, mr)) { toast("This map has no roads", WARN); return; }
    showRoute(mr, true);
    exDist_ = 0;
    // start above the road, looking along it
    auto p = course_->at(0);
    exYaw_ = p.heading;
    exPitch_ = -0.35f;
    exPos_ = {p.pos.x - std::sin(p.heading) * 160.f, p.pos.y + 110.f, p.pos.z - std::cos(p.heading) * 160.f};
    if (const char* at = std::getenv("CORERAY_EXPLOREAT")) {   // dev: "x,z,up,yaw,pitch" (up above the ground)
        float x, z, up, yaw, pitch, absY = 0;
        if (std::sscanf(at, "%f,%f,%f,%f,%f,%f", &x, &z, &up, &yaw, &pitch, &absY) >= 5) {
            exPos_ = {x, absY > 0 ? up : course_->terrainHeight(x, z) + up, z};   // 6th field 1: up is a height
            exYaw_ = yaw; exPitch_ = pitch;
        }
    }
    if (exMiniTex_.id) UnloadTexture(exMiniTex_);
    Image im = miniImage(*map_, 300);
    exMiniTex_ = LoadTextureFromImage(im);
    UnloadImage(im);
    exMiniFor_ = map_.get();
}

void App::exploreFrame(float dt) {
    if (exPending_) {
        if (mapFuture_.valid() && mapFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            exPending_ = false;
            exploreLoad();
        }
        return;
    }
    if (!course_) return;
    if (IsKeyPressed(KEY_ESCAPE)) {
        renderer_.setViewScale(1.f);
        screen_ = Screen::Home;
        course_.reset();
        renderer_.setCourse(nullptr);
        return;
    }
    const bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    // a new map: seed, size, climate
    auto regen = [&](const char* what) {
        store::save(prof_, prefs_);
        requestMap();
        exPending_ = true;
        toast(TextFormat("%s: map %d, %d km, %s climate - generating...", what, prefs_.mapSeed, prefs_.mapSizeKm, CLIMATES[std::clamp(prefs_.mapClimate, 0, 3)]), TEXT);
    };
    if (IsKeyPressed(KEY_N)) { prefs_.mapSeed = std::max(1, prefs_.mapSeed + (shift ? -1 : 1)); regen("New map"); return; }
    if (IsKeyPressed(KEY_RIGHT_BRACKET) || IsKeyPressed(KEY_LEFT_BRACKET)) {
        static const int sizes[] = {10, 20, 30, 40};
        int i = 0;
        while (i < 3 && sizes[i] < prefs_.mapSizeKm) ++i;
        i = std::clamp(i + (IsKeyPressed(KEY_RIGHT_BRACKET) ? 1 : -1), 0, 3);
        prefs_.mapSizeKm = sizes[i];
        regen("Size");
        return;
    }
    if (IsKeyPressed(KEY_K)) { prefs_.mapClimate = (prefs_.mapClimate + 1) % 4; regen("Climate"); return; }
    // weather and time of day: keys, or the panel at the bottom left
    if (IsKeyPressed(KEY_ONE)) exStepWeather(shift ? -1 : 1);
    if (IsKeyPressed(KEY_TWO)) cycleTime();
    if (IsKeyPressed(KEY_THREE)) exStepHour(shift ? -0.25 : -1.0);
    if (IsKeyPressed(KEY_FOUR)) exStepHour(shift ? 0.25 : 1.0);
    if (IsKeyPressed(KEY_V)) {
        exViewScale_ = exViewScale_ > 1.5f ? 1.f : 3.f;
        toast(exViewScale_ > 1.5f ? "Draw distance: far (x3)" : "Draw distance: normal", TEXT);
    }
    if (IsKeyPressed(KEY_TAB)) exMini_ = !exMini_;
    if (IsKeyPressed(KEY_G)) exGround_ = !exGround_;
    if (IsKeyPressed(KEY_F)) { prefs_.showFps = !prefs_.showFps; }
    if (IsKeyPressed(KEY_P)) { prefs_.showPerf = !prefs_.showPerf; }
    // the minimap: click to go there
    const float MW = 300.f;
    Rectangle mini{ui::screenW() - MW - 16, 16, MW, MW};
    const bool overMini = (exMini_ && CheckCollisionPointRec(ui::mouseLogical(), mini)) || CheckCollisionPointRec(ui::mouseLogical(), exPanelRect()) ||
                          (prefs_.miniMap && CheckCollisionPointCircle(ui::mouseLogical(), {ui::screenW() - 142, ui::screenH() - 142}, 126));
    if (overMini && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && map_) {
        Vector2 m = ui::mouseLogical();
        float x = map_->x0 + (m.x - mini.x) / MW * map_->params.size, z = map_->z0 + (m.y - mini.y) / MW * map_->params.size;
        exPos_ = {x, course_->terrainHeight(x, z) + 120.f, z};
    }
    // look: drag with either button
    if (!overMini && (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT))) {
        Vector2 d = GetMouseDelta();
        exYaw_ -= d.x * 0.004f;
        exPitch_ = std::clamp(exPitch_ - d.y * 0.004f, -1.5f, 1.4f);
    }
    // arrows: left / right turn, up / down fly forwards / back; Page Up / Down look up and down
    if (IsKeyDown(KEY_LEFT)) exYaw_ += dt * 1.4f;
    if (IsKeyDown(KEY_RIGHT)) exYaw_ -= dt * 1.4f;
    if (IsKeyDown(KEY_PAGE_UP)) exPitch_ = std::min(1.4f, exPitch_ + dt);
    if (IsKeyDown(KEY_PAGE_DOWN)) exPitch_ = std::max(-1.5f, exPitch_ - dt);
    float wheel = GetMouseWheelMove();
    if (wheel != 0) exSpeed_ = std::clamp(exSpeed_ * std::pow(1.25f, wheel), 3.f, 2000.f);
    // fly
    Vector3 fwd{std::sin(exYaw_) * std::cos(exPitch_), std::sin(exPitch_), std::cos(exYaw_) * std::cos(exPitch_)};
    Vector3 right{-std::cos(exYaw_), 0, std::sin(exYaw_)};
    if (exGround_) fwd = Vector3Normalize({fwd.x, 0, fwd.z});
    Vector3 mv{};
    if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP)) mv = Vector3Add(mv, fwd);
    if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN)) mv = Vector3Subtract(mv, fwd);
    if (IsKeyDown(KEY_D)) mv = Vector3Add(mv, right);
    if (IsKeyDown(KEY_A)) mv = Vector3Subtract(mv, right);
    if (IsKeyDown(KEY_E) || IsKeyDown(KEY_SPACE)) mv.y += 1;
    if (IsKeyDown(KEY_Q) || IsKeyDown(KEY_LEFT_CONTROL)) mv.y -= 1;
    float sp = (exGround_ ? std::min(exSpeed_, 25.f) : exSpeed_) * (shift ? 4.f : 1.f);
    exPos_ = Vector3Add(exPos_, Vector3Scale(mv, sp * dt));
    if (map_) {
        float lim = 0.5f * map_->params.size + 2000.f;
        exPos_.x = std::clamp(exPos_.x, -lim, lim);
        exPos_.z = std::clamp(exPos_.z, -lim, lim);
    }
    float g = course_->terrainHeight(exPos_.x, exPos_.z);
    if (exGround_) exPos_.y = g + 1.8f;
    else if (!std::getenv("CORERAY_EXPLOREAT")) exPos_.y = std::clamp(exPos_.y, g + 1.5f, g + 6000.f);
    // the ridden road is just a road here: keep its nearest point for the road streaming
    float nd = course_->nearestDist(exPos_.x, exPos_.z);
    if (nd >= 0) exDist_ = nd;
}

void App::drawExplore() {
    const float W = ui::screenW(), H = ui::screenH();
    if (exPending_ || !map_ || !course_) {
        Rectangle r{W / 2 - 260, H / 2 - 40, 520, 80};
        DrawRectangleRounded(r, 0.2f, 8, {20, 22, 26, 220});
        textCentered(TextFormat("Generating map %d  -  %d km, %s climate", prefs_.mapSeed, prefs_.mapSizeKm, CLIMATES[std::clamp(prefs_.mapClimate, 0, 3)]), r, F::Body, {235, 235, 240, 255});
        return;
    }
    const wmap::Map& M = *map_;
    wmap::Sample s = M.sample(exPos_.x, exPos_.z);
    int b1 = 0, b2 = -1;
    for (int q = 1; q < wmap::BIOMES; ++q) if (s.w[q] > s.w[b1]) b1 = q;
    for (int q = 0; q < wmap::BIOMES; ++q) if (q != b1 && (b2 < 0 || s.w[q] > s.w[b2])) b2 = q;
    int gi = std::clamp(int((exPos_.x - M.x0) / M.cell), 0, M.n - 1), gk = std::clamp(int((exPos_.z - M.z0) / M.cell), 0, M.n - 1);
    float rain = M.precip.empty() ? 0 : M.precip[size_t(gk) * M.n + gi];
    // the nearest town
    const wmap::Town* town = nullptr;
    float td = 1e30f;
    for (const auto& t : M.towns) { float d = std::hypot(t.x - exPos_.x, t.z - exPos_.z); if (d < td) { td = d; town = &t; } }
    Rectangle r{16, 16, 560, 214};
    DrawRectangleRounded(r, 0.08f, 8, {20, 22, 26, 205});
    float x = r.x + 16, y = r.y + 12;
    const Color T{236, 236, 240, 255}, D{170, 172, 180, 255};
    text(TextFormat("Map %d  \xC2\xB7  %.0f km  \xC2\xB7  %s climate", M.params.seed, M.params.size / 1024, CLIMATES[std::clamp(M.params.climate, 0, 3)]), x, y, F::Body, T);
    y += 28;
    text(TextFormat("%d towns  %.0f km of road  %d tunnels  %d bridges  %d rivers  %d lakes", M.stats.towns, M.stats.roadKm, M.stats.tunnels, M.stats.bridges, M.stats.rivers, M.stats.lakes), x, y, F::Small, D);
    y += 26;
    text(TextFormat("(%.0f, %.0f)   ground %.0f m   eye %.0f m above", exPos_.x, exPos_.z, s.h, exPos_.y - s.h), x, y, F::Small, T);
    y += 22;
    text(TextFormat("%s %.0f%%  /  %s %.0f%%", wmap::biomeName(b1), 100 * s.w[b1], wmap::biomeName(b2), 100 * s.w[b2]), x, y, F::Small, T);
    y += 22;
    text(TextFormat("%.1f C   %.0f mm rain   soil moisture %.2f   sand %.2f", s.temp, rain, s.moist, s.sand), x, y, F::Small, T);
    y += 22;
    text(TextFormat("road %s   %s %.1f km", s.road < 1e3f ? TextFormat("%.0f m", s.road) : "far", town ? town->name.c_str() : "-", td / 1000), x, y, F::Small, T);
    y += 26;
    text(TextFormat("speed %.0f m/s%s   draw distance x%.0f (V)", exSpeed_, exGround_ ? " (walking)" : "", exViewScale_), x, y, F::Small, D, 12);
    y += 18;
    text("WASD / arrows move  QE up/down  drag or PgUp/PgDn look  wheel speed  G walk  N map  [ ] size  K climate  Tab  Esc", x, y, F::Small, D, 12);
    drawExplorePanel();
    if (exMini_ && exMiniTex_.id) {
        const float MW = 300.f;
        Rectangle mini{W - MW - 16, 16, MW, MW};
        DrawTexturePro(exMiniTex_, {0, 0, float(exMiniTex_.width), float(exMiniTex_.height)}, mini, {0, 0}, 0, WHITE);
        DrawRectangleLinesEx(mini, 2, {20, 22, 26, 255});
        float cx = mini.x + (exPos_.x - M.x0) / M.params.size * MW, cy = mini.y + (exPos_.z - M.z0) / M.params.size * MW;
        Vector2 tip{cx + std::sin(exYaw_) * 12, cy + std::cos(exYaw_) * 12};
        DrawLineEx({cx, cy}, tip, 3, {255, 60, 200, 255});
        DrawCircle(int(cx), int(cy), 5, {255, 60, 200, 255});
    }
    // the ride HUD's circular topographic minimap (click it to zoom)
    if (prefs_.miniMap) {
        miniMap_.setMap(map_);
        drawMiniMap({W - 16 - 126, H - 16 - 126}, 120, exPos_.x, exPos_.z, exYaw_, -1);
    }
}

// ---- weather and time of day in explore mode
Rectangle App::exPanelRect() const { return {16, ui::screenH() - 118, 560, 102}; }

void App::exStepWeather(int dir) {
    using weather::Kind;
    int n = int(Kind::COUNT);
    int k = prefs_.weatherMode == 2 ? int(weather::kindFromId(prefs_.weatherKind).value_or(Kind::Clear)) : (dir > 0 ? -1 : 0);
    k = ((k + dir) % n + n) % n;
    prefs_.weatherMode = 2;
    prefs_.weatherKind = weather::kindId(Kind(k));
    toast(std::string("Weather: ") + weather::kindLabel(Kind(k)), TEXT);
    store::save(prof_, prefs_);
}

void App::exStepHour(double dh) {
    double h = prefs_.todHour;
    if (prefs_.todMode == 0) {   // from real time: start at the clock
        std::time_t t = std::time(nullptr);
        std::tm lt = *std::localtime(&t);
        h = lt.tm_hour + lt.tm_min / 60.0;
    } else if (prefs_.todMode == 2) h = std::fmod(prefs_.todHour + cycleHours_, 24.0);
    prefs_.todMode = 1;
    cycleHours_ = 0;
    prefs_.todHour = std::fmod(h + dh + 48.0, 24.0);
    store::save(prof_, prefs_);
}

void App::drawExplorePanel() {
    using weather::Kind;
    Rectangle P = exPanelRect();
    DrawRectangleRounded(P, 0.12f, 8, {20, 22, 26, 205});
    const Color T{236, 236, 240, 255}, D{170, 172, 180, 255};
    float x = P.x + 16, y = P.y + 12;
    // weather
    text("Weather", x, y + 8, F::Small, D);
    const char* wl = prefs_.weatherMode == 0 ? "Changing" : prefs_.weatherMode == 1 ? "Live" : weather::kindLabel(weather::kindFromId(prefs_.weatherKind).value_or(Kind::Clear));
    float bx = x + 70;
    if (button({bx, y, 34, 32}, "<")) exStepWeather(-1);
    textCentered(wl, {bx + 38, y, 120, 32}, F::Body, T);
    if (button({bx + 162, y, 34, 32}, ">")) exStepWeather(1);
    if (button({bx + 206, y, 100, 32}, "Changing", prefs_.weatherMode == 0)) { prefs_.weatherMode = 0; store::save(prof_, prefs_); }
    if (button({bx + 314, y, 76, 32}, "Live", prefs_.weatherMode == 1)) { prefs_.weatherMode = 1; fetchLiveWeather(); store::save(prof_, prefs_); }
    y += 44;
    // time of day
    text("Time", x, y + 8, F::Small, D);
    std::string tl;
    if (prefs_.todMode == 0) tl = "Real time";
    else {
        double h = prefs_.todMode == 2 ? std::fmod(prefs_.todHour + cycleHours_, 24.0) : prefs_.todHour;
        int hh = int(h), mm = int((h - hh) * 60);
        tl = TextFormat("%02d:%02d%s", hh, mm, prefs_.todMode == 2 ? " (cycle)" : "");
    }
    if (button({bx, y, 34, 32}, "-")) exStepHour(-1);
    textCentered(tl, {bx + 38, y, 120, 32}, F::Body, T);
    if (button({bx + 162, y, 34, 32}, "+")) exStepHour(1);
    if (button({bx + 206, y, 100, 32}, "Day cycle", prefs_.todMode == 2)) { prefs_.todMode = 2; cycleHours_ = 0; store::save(prof_, prefs_); }
    if (button({bx + 314, y, 76, 32}, "Real", prefs_.todMode == 0)) { prefs_.todMode = 0; store::save(prof_, prefs_); }
    }
