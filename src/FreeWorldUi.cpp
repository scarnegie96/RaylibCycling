// Free ride on a generated world: choose size / climate / seed on the Ride page, see the
// world as a 3D miniature (MapPreview) and click a road to choose where the ride starts.
// startRide() -> roamStart() then lays the road on from that point, in the chosen direction.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <random>

#include "App.h"
#include "core/Dates.h"
#include "Audio.h"
#include "raymath.h"
#include "world/WorldMap.h"

using namespace ui;

namespace {
const char* CLIMATE_NAMES[4] = {"Any climate", "Temperate", "Desert", "Cold"};
const int SIZES[4] = {10, 20, 30, 40};

const char* compass(float dx, float dz) {
    // the map's north is -z (top of the minimap)
    static const char* names[8] = {"north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west"};
    float a = std::atan2(dx, -dz);
    int i = int(std::lround(a / (PI / 4))) & 7;
    return names[i];
}
std::string mapKey(uint32_t seed, int km, int climate) {   // same form as App::requestMap's
    return std::to_string(seed) + "_" + std::to_string(km) + "km_c" + std::to_string(climate);
}

// "In Highfield" / "Minor road near Highfield"
std::string placeLabel(const wmap::Map& M, float x, float z, int cls, float* townDist = nullptr, int* townIdx = nullptr) {
    int ti = -1;
    float td = 1e30f;
    for (size_t i = 0; i < M.towns.size(); ++i) {
        float d = std::hypot(M.towns[i].x - x, M.towns[i].z - z);
        if (d < td) { td = d; ti = int(i); }
    }
    if (townDist) *townDist = td;
    if (townIdx) *townIdx = ti;
    std::string road = cls == 0 ? "Main road" : "Minor road";
    if (ti >= 0 && td < 450) return "In " + M.towns[ti].name;
    if (ti >= 0) return road + " near " + M.towns[ti].name;
    return road;
}
}  // namespace

std::shared_ptr<const wmap::Map> App::mapIfReady() {
    if (!mapFuture_.valid()) return nullptr;
    if (mapFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return nullptr;
    return mapFuture_.get();
}

void App::defaultSpawn() {
    spawn_ = MapPreview::Spawn{};
    auto m = mapIfReady();
    if (!m || m->nodes.empty()) return;
    const wmap::Map& M = *m;
    // the biggest place near the middle with a way through it
    int best = -1;
    float bs = 1e30f;
    for (size_t i = 0; i < M.nodes.size(); ++i) {
        const auto& N = M.nodes[i];
        if (N.roads.size() < 2) continue;
        float d = std::hypot(N.x - (M.x0 + 0.5f * M.params.size), N.z - (M.z0 + 0.5f * M.params.size));
        float score = d * (N.town >= 0 ? (M.towns[N.town].rank >= 2 ? 0.35f : 0.6f) : 1.f);
        if (score < bs) { bs = score; best = int(i); }
    }
    if (best < 0) return;
    int road = M.nodes[best].roads[0];
    for (int r : M.nodes[best].roads) if (M.roads[r].length > M.roads[road].length) road = r;
    const auto& R = M.roads[road];
    bool fromA = R.a == best;
    int n = int(R.pts.size());
    spawn_.set = true;
    spawn_.road = road;
    spawn_.idx = fromA ? std::min(2, n - 1) : std::max(0, n - 3);
    spawn_.fwd = fromA;
}

std::string App::spawnLabel(const MapPreview::Spawn& s, std::string* detail) const {
    auto m = mapFuture_.valid() && mapFuture_.wait_for(std::chrono::seconds(0)) == std::future_status::ready ? mapFuture_.get() : nullptr;
    if (!m || !s.set || s.road < 0 || s.road >= int(m->roads.size())) return "";
    const wmap::Map& M = *m;
    const auto& R = M.roads[s.road];
    const auto& p = R.pts[std::clamp(s.idx, 0, int(R.pts.size()) - 1)];
    int ti = -1;
    float td = 1e30f;
    std::string road = R.cls == 0 ? "Main road" : "Minor road";
    std::string title = placeLabel(M, p.x, p.z, R.cls, &td, &ti);
    if (detail) {
        int n = int(R.pts.size());
        int j = std::clamp(s.idx + (s.fwd ? 8 : -8), 0, n - 1);
        const auto& q = R.pts[j];
        char b[220];
        std::string towards;
        int end = s.fwd ? R.b : R.a;
        if (end >= 0 && M.nodes[end].town >= 0 && M.nodes[end].town != (td < 450 ? ti : -2)) towards = ", towards " + M.towns[M.nodes[end].town].name;
        if (ti >= 0 && td >= 450)
            std::snprintf(b, sizeof b, "%.1f km from %s  \xC2\xB7  %.0f m up  \xC2\xB7  heading %s%s", td / 1000.f, M.towns[ti].name.c_str(), p.y, compass(q.x - p.x, q.z - p.z), towards.c_str());
        else
            std::snprintf(b, sizeof b, "%s  \xC2\xB7  %.0f m up  \xC2\xB7  heading %s%s", road.c_str(), p.y, compass(q.x - p.x, q.z - p.z), towards.c_str());
        *detail = b;
    }
    return title;
}

// ---- left panel: size, climate, seed
void App::drawWorldOptions(float x, float& y, float w) {
    bool changed = false;
    text("Size", x, y, F::Small, DIM);
    y += 22;
    int cur = 1;
    for (int i = 0; i < 4; ++i) if (prefs_.mapSizeKm == SIZES[i]) cur = i;
    int sz = segmented({x, y, w, 36}, {"10 km", "20 km", "30 km", "40 km"}, cur);
    if (sz >= 0 && SIZES[sz] != prefs_.mapSizeKm) { prefs_.mapSizeKm = SIZES[sz]; changed = true; }
    y += 46;
    text("Climate", x, y, F::Small, DIM);
    y += 22;
    int cl = segmented({x, y, w, 36}, {"Any", "Temperate", "Desert", "Cold"}, std::clamp(prefs_.mapClimate, 0, 3));
    if (cl >= 0 && cl != prefs_.mapClimate) { prefs_.mapClimate = cl; changed = true; }
    y += 46;
    int d = 0;
    if (stepper({x, y, w - 148, 36}, "Seed", "#" + std::to_string(prefs_.mapSeed), &d, 90)) {
        prefs_.mapSeed = (prefs_.mapSeed - 1 + d + 9999) % 9999 + 1;
        changed = true;
    }
    if (button({x + w - 136, y, 136, 36}, "Surprise me")) {
        std::mt19937 rng(std::random_device{}());
        prefs_.mapSeed = int(rng() % 9999) + 1;
        changed = true;
    }
    y += 46;
    drawSaveWorldButton({x, y, w, 36});
    y += 46;
    if (changed) {
        requestMap();
        store::save(prof_, prefs_);
    }
}

// ---- right side: the world in 3D and the chosen start
void App::drawWorldPicker(Rectangle R) {
    panel(R, ui::PANEL);
    requestMap();   // no-op when this map is already built / being built
    float x = R.x + 22, y = R.y + 18, w = R.width - 44;
    const char* clim = CLIMATE_NAMES[std::clamp(prefs_.mapClimate, 0, 3)];
    std::string title = "Generated world";
    if (prefs_.freeWorld == 2)
        if (auto* sw = savedWorldFor(uint32_t(prefs_.mapSeed), prefs_.mapSizeKm, prefs_.mapClimate)) title = sw->name;
    text(title, x, y, F::Title, TEXT, 28);
    // the map's identity as chips on the right of the title
    {
        char b[64];
        std::snprintf(b, sizeof b, "Map #%d  \xC2\xB7  %d km  \xC2\xB7  %s", prefs_.mapSeed, prefs_.mapSizeKm, clim);
        float cw = measure(b, F::Small).x + 24;
        DrawRectangleRounded({x + w - cw, y + 4, cw, 28}, 1, 8, ui::accentA(26));
        text(b, x + w - cw + 12, y + 11, F::Small, ACCENT);
    }
    y += 42;
    auto m = mapIfReady();
    char b[240];
    if (m) {
        float roadKm = 0;
        for (const auto& r : m->roads) roadKm += r.length / 1000.f;
        int rivers = 0;
        for (const auto& rv : m->rivers) if (!rv.dry) ++rivers;
        auto n = [](size_t k, const char* one, const char* many) { return std::to_string(k) + " " + (k == 1 ? one : many); };
        std::string line = n(m->towns.size(), "place", "places") + "  \xC2\xB7  " + std::to_string(int(std::lround(roadKm))) + " km of road  \xC2\xB7  " +
                           n(m->bridges.size(), "bridge", "bridges") + "  \xC2\xB7  " + n(m->tunnels.size(), "tunnel", "tunnels") + "  \xC2\xB7  " +
                           n(size_t(rivers), "river", "rivers") + "  \xC2\xB7  " + n(m->lakes.size(), "lake", "lakes");
        std::snprintf(b, sizeof b, "%s", line.c_str());
        text(b, x, y, F::Body, DIM, 16);
    } else text("A whole world of its own: relief, climate, rivers, towns and the roads between them.", x, y, F::Body, DIM, 16);
    y += 32;

    const float cardH = 76;
    Rectangle V{x, y, w, R.y + R.height - y - cardH - 34};
    if (!m) {
        // still generating: the same frame as the viewport, with progress
        mapPreview_.setMap(nullptr);
        DrawRectangleRounded(V, 14 * 2 / std::min(V.width, V.height), 16, ui::PANEL_HI);
        float t = float(GetTime());
        Vector2 c{V.x + V.width / 2, V.y + V.height / 2 - 26};
        DrawRing(c, 20, 25, t * 300.f, t * 300.f + 250.f, 40, ACCENT);
        std::snprintf(b, sizeof b, "Generating map #%d  \xC2\xB7  %d km, %s", prefs_.mapSeed, prefs_.mapSizeKm, clim);
        textCentered(b, {V.x, c.y + 38, V.width, 26}, F::Body, TEXT, 18);
        static const char* stages[] = {"Raising the land", "Bringing in the weather", "Letting the rivers find their way", "Founding towns", "Laying the roads", "Building bridges and tunnels"};
        int st = int((GetTime() - mapReqAt_) / 2.5) % 6;
        std::snprintf(b, sizeof b, "%s...  %.0f s", stages[st], GetTime() - mapReqAt_);
        textCentered(b, {V.x, c.y + 66, V.width, 22}, F::Small, DIM, 14);
        textCentered("Larger worlds take longer the first time; they're kept for next time.", {V.x, V.y + V.height - 40, V.width, 22}, F::Small, FAINT, 13);
    } else {
        mapPreview_.setMap(m);
        if (spawnKey_ != mapFor_) {
            spawnKey_ = mapFor_;
            bool fromSaved = pendingStart_ && mapKey(pendingStart_->seed, pendingStart_->sizeKm, pendingStart_->climate) == mapFor_ && resolveStart(*m, *pendingStart_);
            if (!fromSaved) defaultSpawn();
            pendingStart_.reset();
            mapPreview_.resetView();
        }
        if (mapPreview_.draw(V, spawn_, GetFrameTime())) sfx::play(sfx::StepChange);
        if (mapPreview_.ready()) {
            // legend (top left)
            struct Item { const char* label; Color fill, edge; bool dot; };
            const Item items[] = {{"Main road", {255, 210, 92, 255}, {70, 62, 50, 255}, false}, {"Minor road", {252, 252, 250, 255}, {78, 82, 92, 255}, false},
                                  {"Bridge", {252, 252, 250, 255}, {214, 112, 52, 255}, false}, {"Tunnel", {120, 124, 132, 255}, {120, 124, 132, 0}, false},
                                  {"Town", {200, 86, 64, 255}, {200, 86, 64, 255}, true}};
            float lw = 24;
            for (const auto& it : items) lw += 30 + measure(it.label, F::Small, 13).x;
            Rectangle lg{V.x + 12, V.y + 12, lw - 12, 30};
            DrawRectangleRounded(lg, 1, 8, Color{255, 255, 255, 228});
            float lx = lg.x + 12;
            for (const auto& it : items) {
                if (it.dot) DrawCircleV({lx + 7, lg.y + 15}, 5, it.fill);
                else {
                    DrawRectangleRounded({lx - 1, lg.y + 11, 18, 8}, 1, 4, it.edge);
                    DrawRectangleRounded({lx + 1, lg.y + 12.5f, 14, 5}, 1, 4, it.fill);
                }
                text(it.label, lx + 22, lg.y + 8, F::Small, DIM, 13);
                lx += 30 + measure(it.label, F::Small, 13).x;
            }
            // reset view (top right)
            if (button({V.x + V.width - 112, V.y + 12, 100, 30}, "Reset view")) mapPreview_.resetView();
            // controls hint (bottom left), fading once the map has been moved
            static float hintA = 1;
            hintA = mapPreview_.interacted() ? std::max(0.f, hintA - GetFrameTime() * 0.6f) : 1.f;
            const char* hint = "Drag to rotate  \xC2\xB7  scroll to zoom  \xC2\xB7  right-drag to move  \xC2\xB7  click a road to start there";
            Vector2 hm = measure(hint, F::Small, 13);
            Rectangle hb{V.x + 12, V.y + V.height - 40, hm.x + 24, 28};
            if (hintA > 0.01f) {
                DrawRectangleRounded(hb, 1, 8, Fade(Color{255, 255, 255, 215}, hintA));
                text(hint, hb.x + 12, hb.y + 7, F::Small, Fade(DIM, hintA), 13);
            }
        }
    }

    // ---- the start card
    Rectangle C{x, V.y + V.height + 16, w, cardH};
    DrawRectangleRounded(C, 0.3f, 8, ui::PANEL2);
    Vector2 pin{C.x + 36, C.y + cardH / 2};
    DrawCircleV(pin, 18, ui::accentA(30));
    DrawCircleV(pin, 9, ACCENT);
    DrawCircleV(pin, 3.5f, Color{235, 255, 244, 255});
    if (m && spawn_.set) {
        std::string det;
        std::string title = spawnLabel(spawn_, &det);
        text("START", C.x + 66, C.y + 12, F::Small, ACCENT, 12);
        text(title, C.x + 66, C.y + 26, F::Body, TEXT, 19);
        text(det, C.x + 66, C.y + 50, F::Small, DIM, 13);
        if (button({C.x + C.width - 128, C.y + (cardH - 38) / 2, 112, 38}, "Turn round")) { spawn_.fwd = !spawn_.fwd; sfx::play(sfx::StepChange); }
    } else if (m) {
        text("Click a road on the map to choose where you start", C.x + 66, C.y + 28, F::Body, DIM, 17);
    } else {
        text("You can choose where to start once the world is ready", C.x + 66, C.y + 28, F::Body, FAINT, 17);
    }
}

// ====================================================================== saved worlds
std::vector<store::SavedWorld>& App::savedWorlds() {
    if (!savedWorldsLoaded_) { savedWorlds_ = store::loadSavedWorlds(); savedWorldsLoaded_ = true; }
    return savedWorlds_;
}

store::SavedWorld* App::savedWorldFor(uint32_t seed, int km, int climate) {
    for (auto& w : savedWorlds())
        if (w.seed == seed && w.sizeKm == km && w.climate == climate) return &w;
    return nullptr;
}

App::MapStart App::pickedStart(const wmap::Map& M) const {
    MapStart st;
    if (!spawn_.set || spawn_.road < 0 || spawn_.road >= int(M.roads.size())) return st;
    const auto& P = M.roads[spawn_.road].pts;
    int n = int(P.size());
    int i = std::clamp(spawn_.idx, 0, n - 1), j = std::clamp(i + (spawn_.fwd ? 4 : -4), 0, n - 1);
    st.set = true;
    st.road = spawn_.road;
    st.idx = i;
    st.fwd = spawn_.fwd;
    st.x = P[i].x;
    st.z = P[i].z;
    float hx = P[j].x - P[i].x, hz = P[j].z - P[i].z, hl = std::max(1e-3f, std::hypot(hx, hz));
    st.hx = hx / hl;
    st.hz = hz / hl;
    return st;
}

bool App::saveWorld(const wmap::Map& M, const MapStart& st) {
    auto& list = savedWorlds();
    const int km = int(std::lround(M.params.size / 1024.f));
    store::SavedWorld* w = savedWorldFor(M.params.seed, km, M.params.climate);
    bool fresh = !w;
    if (fresh) {
        list.push_back({});
        w = &list.back();
        w->id = dates::newId();
        w->seed = M.params.seed;
        w->sizeKm = km;
        w->climate = M.params.climate;
    }
    w->cell = M.params.cell; w->erosion = M.params.erosion; w->aridity = M.params.aridity; w->roads = M.params.roads;
    w->generator = wmap::VERSION;
    w->savedAt = dates::nowRfc3339();
    w->towns = int(M.towns.size());
    w->roadKm = 0;
    for (const auto& r : M.roads) w->roadKm += r.length / 1000.f;
    if (st.set) {
        w->hasStart = true;
        w->road = st.road; w->idx = st.idx; w->fwd = st.fwd;
        w->startX = st.x; w->startZ = st.z; w->headX = st.hx; w->headZ = st.hz;
        int cls = st.road >= 0 && st.road < int(M.roads.size()) ? M.roads[st.road].cls : 1;
        w->startLabel = placeLabel(M, st.x, st.z, cls);
    }
    if (fresh) {
        // named after the biggest place near the start (or the middle of the map)
        float cx = st.set ? st.x : M.x0 + 0.5f * M.params.size, cz = st.set ? st.z : M.z0 + 0.5f * M.params.size;
        int best = -1;
        float bs = 1e30f;
        for (size_t i = 0; i < M.towns.size(); ++i) {
            float d = std::hypot(M.towns[i].x - cx, M.towns[i].z - cz) * (M.towns[i].rank >= 2 ? 0.5f : 1.f);
            if (d < bs) { bs = d; best = int(i); }
        }
        w->name = best >= 0 ? M.towns[best].name + " country" : std::string("Wild country");
    }
    bool ok = store::saveSavedWorlds(list);
    if (!ok) toast("Couldn't write the saved worlds file", BAD);
    return ok;
}

bool App::resolveStart(const wmap::Map& M, const store::SavedWorld& w) {
    if (!w.hasStart || M.roads.empty()) return false;
    // the same generator: the saved road point, if it is still where it was
    if (w.generator == wmap::VERSION && w.road >= 0 && w.road < int(M.roads.size()) && w.idx >= 0 && w.idx < int(M.roads[w.road].pts.size())) {
        const auto& p = M.roads[w.road].pts[w.idx];
        if (std::hypot(p.x - w.startX, p.z - w.startZ) < 25.f) {
            spawn_ = MapPreview::Spawn{true, w.road, w.idx, w.fwd};
            return true;
        }
    }
    // otherwise the nearest road point to where it was, facing the saved way
    int br = -1, bi = 0;
    float bd = 1e30f;
    for (int r = 0; r < int(M.roads.size()); ++r) {
        const auto& P = M.roads[r].pts;
        for (int i = 0; i < int(P.size()); ++i) {
            if (P[i].kind == 1) continue;
            float d = std::hypot(P[i].x - w.startX, P[i].z - w.startZ);
            if (d < bd) { bd = d; br = r; bi = i; }
        }
    }
    if (br < 0) return false;
    const auto& P = M.roads[br].pts;
    int j = std::min(int(P.size()) - 1, bi + 1), i0 = j == bi ? bi - 1 : bi;
    float dx = P[j].x - P[i0].x, dz = P[j].z - P[i0].z;
    spawn_ = MapPreview::Spawn{true, br, bi, dx * w.headX + dz * w.headZ >= 0};
    return true;
}

void App::selectSavedWorld(const store::SavedWorld& w) {
    prefs_.mapSeed = int(w.seed);
    prefs_.mapSizeKm = w.sizeKm;
    prefs_.mapClimate = w.climate;
    pendingStart_ = w;
    spawnKey_.clear();   // re-resolve the start when the picker next draws this map
    requestMap();
    store::save(prof_, prefs_);
}

// "Save this world" / "Update saved start" / "Saved", for the map on the Ride page
void App::drawSaveWorldButton(Rectangle r) {
    auto m = mapIfReady();
    if (!m) { button(r, "Save this world", false, false); return; }
    store::SavedWorld* w = savedWorldFor(m->params.seed, int(std::lround(m->params.size / 1024.f)), m->params.climate);
    MapStart st = pickedStart(*m);
    bool same = w && w->hasStart == st.set && (!st.set || (w->road == st.road && w->idx == st.idx && w->fwd == st.fwd && w->generator == wmap::VERSION));
    const char* label = !w ? "Save this world" : same ? "Saved in your worlds" : "Save this start point";
    if (button(r, label, false, !same) && saveWorld(*m, st))
        toast(w ? "Start point saved" : "World saved - find it under Saved worlds", GOOD);
}

void App::drawSavedWorlds(float x, float& y, float w, float bottom) {
    auto& list = savedWorlds();
    text("Your saved worlds", x, y, F::Small, DIM);
    y += 22;
    if (list.empty()) {
        Rectangle e{x, y, w, 110};
        DrawRectangleRounded(e, 0.15f, 8, ui::PANEL2);
        textWrapped("No saved worlds yet. Pause during a ride on a generated world and choose \xE2\x80\x9C" "Add seed to saved list\xE2\x80\x9D, "
                    "or press \xE2\x80\x9CSave this world\xE2\x80\x9D on the Generated world tab.", {e.x + 16, e.y + 14, e.width - 32, e.height - 20}, F::Body, DIM, 15);
        y += 122;
        return;
    }
    const float rowH = 62;
    Rectangle L{x, y, w, std::max(rowH, std::min(bottom - y - 56, float(list.size()) * (rowH + 6)))};
    static float sc = 0;
    scrollUpdate(L, sc, list.size() * (rowH + 6));
    ui::beginScissor({L.x, L.y, L.width, L.height});
    int removeAt = -1;
    char b[200];
    for (size_t i = 0; i < list.size(); ++i) {
        const auto& sw = list[i];
        Rectangle row{L.x, L.y + i * (rowH + 6) - sc, L.width - 8, rowH};
        if (row.y + rowH < L.y || row.y > L.y + L.height) continue;
        bool sel = prefs_.mapSeed == int(sw.seed) && prefs_.mapSizeKm == sw.sizeKm && prefs_.mapClimate == sw.climate;
        bool h = hovered(row) && CheckCollisionPointRec(ui::mouseLogical(), L);
        DrawRectangleRounded(row, 0.22f, 8, sel ? ui::accentA(40) : h ? ui::HOVER : ui::PANEL2);
        if (sel) DrawRectangleRoundedLinesEx(row, 0.22f, 8, 1.5f, ACCENT);
        text(sw.name, row.x + 14, row.y + 9, F::Body, TEXT, 17);
        auto d = dates::localDate(sw.savedAt);
        std::snprintf(b, sizeof b, "Map #%u  \xC2\xB7  %d km  \xC2\xB7  %s  \xC2\xB7  %d places  \xC2\xB7  saved %s%s", sw.seed, sw.sizeKm, CLIMATE_NAMES[std::clamp(sw.climate, 0, 3)],
                      sw.towns, d ? fmtDateShort(d->y, d->m, d->d).c_str() : "", sw.generator != wmap::VERSION ? "  \xC2\xB7  older generator" : "");
        text(b, row.x + 14, row.y + 36, F::Small, DIM, 13);
        Rectangle del{row.x + row.width - 90, row.y + (rowH - 30) / 2, 78, 30};
        bool overDel = false;
        if (h || sel) {
            overDel = hovered(del);
            if (button(del, "Remove")) removeAt = int(i);
        }
        if (h && !overDel && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !sel) { selectSavedWorld(sw); sfx::play(sfx::StepChange); }
    }
    EndScissorMode();
    scrollBar(L, sc, list.size() * (rowH + 6));
    y += L.height + 10;
    if (removeAt >= 0) {
        std::string nm = list[removeAt].name;
        list.erase(list.begin() + removeAt);
        store::saveSavedWorlds(list);
        toast("Removed " + nm, TEXT);
    }
}
