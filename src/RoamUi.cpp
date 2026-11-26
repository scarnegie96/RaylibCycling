// Free roaming on a generated map: the ride follows the road network, one road at a time.
// Approaching a junction the rider is offered the ways on (left / straight on / right, or
// turn around at a dead end) with where each leads; the choice is taken ~320 m before the
// junction and the 3D road and the trainer's profile grow along it.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

#include "App.h"
#include "Audio.h"
#include "raymath.h"
#include "world/WorldMap.h"

using namespace ui;

namespace {
constexpr float PIf = 3.14159265f;
float wrapPi(float a) {
    while (a > PIf) a -= 2 * PIf;
    while (a < -PIf) a += 2 * PIf;
    return a;
}
// the heading a road leaves a node by (over its first ~25 m)
float leaveHeading(const wmap::Road& R, bool fwd) {
    const auto& P = R.pts;
    int n = int(P.size());
    int a = fwd ? 0 : n - 1, b = fwd ? std::min(n - 1, 5) : std::max(0, n - 6);
    return std::atan2(P[b].x - P[a].x, P[b].z - P[a].z);
}
}  // namespace

std::vector<App::RoamChoice> App::roamOptions(int node, int fromRoad, float inHeading) const {
    std::vector<RoamChoice> out;
    if (!map_ || node < 0) return out;
    const wmap::Map& M = *map_;
    const auto& N = M.nodes[node];
    auto destination = [&](int road, bool fwd, float& km) -> std::string {
        // follow the way on (straightest at each junction) until a place, for up to 12 km
        int r = road, at = node;
        bool f = fwd;
        km = 0;
        for (int hop = 0; hop < 30; ++hop) {
            const auto& R = M.roads[r];
            km += R.length / 1000.f;
            int next = f ? R.b : R.a;
            if (M.nodes[next].town >= 0) return M.towns[M.nodes[next].town].name;
            if (km > 12.f) break;
            float hin = leaveHeading(R, !f) + PIf;   // arriving heading
            int best = -1;
            float bestTurn = 9;
            for (int q : M.nodes[next].roads) {
                if (q == r) continue;
                bool qf = M.roads[q].a == next;
                float t = std::fabs(wrapPi(leaveHeading(M.roads[q], qf) - hin));
                if (t < bestTurn) { bestTurn = t; best = q; }
            }
            if (best < 0) return "dead end";
            at = next;
            f = M.roads[best].a == at;
            r = best;
        }
        return "";
    };
    for (int r : N.roads) {
        if (r == fromRoad && N.roads.size() > 1) continue;
        const auto& R = M.roads[r];
        bool fwd = R.a == node;
        if (R.a == R.b) continue;
        float hOut = r == fromRoad ? inHeading + PIf : leaveHeading(R, fwd);
        float dh = wrapPi(hOut - inHeading);
        RoamChoice c;
        c.road = r;
        c.fwd = fwd;
        c.turn = dh;
        // heading grows turning left here (right of +z is -x)
        if (r == fromRoad) c.label = "TURN AROUND";
        else if (std::fabs(dh) < 0.42f) c.label = "STRAIGHT ON";
        else if (dh > 0) c.label = dh > 1.9f ? "SHARP LEFT" : "LEFT";
        else c.label = dh < -1.9f ? "SHARP RIGHT" : "RIGHT";
        float km;
        std::string d = destination(r, fwd, km);
        c.deadEnd = d == "dead end";
        c.dest = d.empty() ? TextFormat("%.1f km of road", km) : d == "dead end" ? TextFormat("dead end, %.1f km", km) : TextFormat("%s  %.1f km", d.c_str(), km);
        out.push_back(c);
    }
    // left to right as they are on the road
    std::sort(out.begin(), out.end(), [](const RoamChoice& a, const RoamChoice& b) { return a.turn > b.turn; });
    return out;
}

void App::roamAppend(int road, int fromNode) {
    const wmap::Road& R = map_->roads[road];
    bool fwd = R.a == fromNode;
    const int n = int(R.pts.size());
    for (int q = 0; q < n; ++q) {
        const auto& p = R.pts[fwd ? q : n - 1 - q];
        if (q == 0 && !mapPath_.empty()) continue;   // the junction point is already there
        mapPath_.push_back(p);
    }
    roam_.node = fwd ? R.b : R.a;
    roam_.recent.push_back(road);
    if (roam_.recent.size() > 12) roam_.recent.erase(roam_.recent.begin());
    roam_.fromRoad = road;
    roam_.opts.clear();
}

static float pathHeading(const std::vector<wmap::RoadPt>& P) {
    size_t n = P.size();
    if (n < 2) return 0;
    size_t a = n > 6 ? n - 6 : 0;
    return std::atan2(P[n - 1].x - P[a].x, P[n - 1].z - P[a].z);
}

int App::roamDefault(const std::vector<RoamChoice>& o) const {
    int best = 0;
    float bt = 1e9f;
    for (int i = 0; i < int(o.size()); ++i) {
        // the straightest way on, preferring main roads, roads not ridden lately and not dead ends
        float t = std::fabs(o[i].turn) + (o[i].label == "TURN AROUND" ? 10.f : 0.f) - (map_->roads[o[i].road].cls == 0 ? 0.15f : 0.f) + (o[i].deadEnd ? 1.5f : 0.f);
        for (size_t k = 0; k < roam_.recent.size(); ++k)
            if (roam_.recent[k] == o[i].road) t += 2.f + 0.1f * float(k);
        if (t < bt) { bt = t; best = i; }
    }
    return best;
}

bool App::roamStart(Route& out) {
    requestMap();
    map_ = mapFuture_.get();
    if (!map_ || map_->roads.empty()) return false;
    const wmap::Map& M = *map_;
    std::mt19937 rng(worldSeed_ ? worldSeed_ : uint32_t(std::random_device{}()));
    std::uniform_real_distribution<float> U(-0.3f, 0.3f);
    float sx = U(rng) * M.params.size, sz = U(rng) * M.params.size;
    if (const char* at = std::getenv("CORERAY_MAPSTART")) std::sscanf(at, "%f,%f", &sx, &sz);
    // a start picked on the Ride page's 3D map: begin on that road point, heading the chosen way
    if (setupKind_ == RideKind::Free && prefs_.freeWorld >= 1 && spawn_.set && spawnKey_ == mapFor_ && spawn_.road >= 0 &&
        spawn_.road < int(M.roads.size()) && M.roads[spawn_.road].pts.size() >= 2) {
        const wmap::Road& R = M.roads[spawn_.road];
        const int n = int(R.pts.size());
        int i0 = std::clamp(spawn_.idx, 0, n - 1);
        if (spawn_.fwd && i0 > n - 2) i0 = n - 2;   // leave at least one step of road
        if (!spawn_.fwd && i0 < 1) i0 = 1;
        roam_ = Roam{};
        roam_.on = true;
        mapPath_.clear();
        if (spawn_.fwd) for (int q = i0; q < n; ++q) mapPath_.push_back(R.pts[q]);
        else for (int q = i0; q >= 0; --q) mapPath_.push_back(R.pts[q]);
        roam_.node = spawn_.fwd ? R.b : R.a;
        roam_.fromRoad = spawn_.road;
        roam_.recent.push_back(spawn_.road);
        while (mapPath_.size() < 400) {
            auto o = roamOptions(roam_.node, roam_.fromRoad, pathHeading(mapPath_));
            if (o.empty()) break;
            roamAppend(o[roamDefault(o)].road, roam_.node);
        }
        std::string det;
        std::string where = spawnLabel(spawn_, &det);
        out = Route{};
        out.id = "map-" + std::to_string(M.params.seed);
        out.name = where.empty() ? std::string("Roaming map ") + std::to_string(M.params.seed) : (where.rfind("In ", 0) == 0 ? "Roaming from " + where.substr(3) : "Roaming: " + where);
        out.isLoop = false;
        out.source = RouteSource::Generated;
        double d = 0;
        for (size_t j = 0; j < mapPath_.size(); ++j) {
            if (j) d += std::hypot(double(mapPath_[j].x - mapPath_[j - 1].x), double(mapPath_[j].z - mapPath_[j - 1].z));
            out.points.push_back({d, double(mapPath_[j].y)});
        }
        std::fprintf(stderr, "[map] roaming from the picked start: road %d point %d (%s), %.1f km laid\n", spawn_.road, i0, spawn_.fwd ? "forwards" : "backwards", d / 1000);
        rideMapStart_ = pickedStart(M);
        rideMapStart_.idx = i0;
        return true;
    }
    // start at a through-place (a town if there's one close) near that point
    int start = -1;
    float bd = 1e30f;
    for (size_t i = 0; i < M.nodes.size(); ++i) {
        if (M.nodes[i].roads.size() < 2) continue;
        float d = std::hypot(M.nodes[i].x - sx, M.nodes[i].z - sz) * (M.nodes[i].town >= 0 ? 0.6f : 1.f);
        if (d < bd) { bd = d; start = int(i); }
    }
    if (start < 0) return false;
    roam_ = Roam{};
    roam_.on = true;
    mapPath_.clear();
    int first = M.nodes[start].roads[0];
    for (int r : M.nodes[start].roads) if (M.roads[r].length > M.roads[first].length) first = r;
    roamAppend(first, start);
    while (mapPath_.size() < 400) {   // ~2 km to begin with
        auto o = roamOptions(roam_.node, roam_.fromRoad, pathHeading(mapPath_));
        if (o.empty()) break;
        roamAppend(o[roamDefault(o)].road, roam_.node);
    }
    out = Route{};
    out.id = "map-" + std::to_string(M.params.seed);
    int t = M.nodes[start].town;
    out.name = t >= 0 ? "Roaming from " + M.towns[t].name : std::string("Roaming map ") + std::to_string(M.params.seed);
    out.isLoop = false;
    out.source = RouteSource::Generated;
    double d = 0;
    for (size_t j = 0; j < mapPath_.size(); ++j) {
        if (j) d += std::hypot(double(mapPath_[j].x - mapPath_[j - 1].x), double(mapPath_[j].z - mapPath_[j - 1].z));
        out.points.push_back({d, double(mapPath_[j].y)});
    }
    std::fprintf(stderr, "[map] roaming from node %d (%.0f, %.0f), %.1f km laid\n", start, M.nodes[start].x, M.nodes[start].z, d / 1000);
    {   // where this ride begins: the first road, leaving the start node
        const auto& R0 = M.roads[first];
        bool fwd = R0.a == start;
        int n0 = int(R0.pts.size());
        rideMapStart_ = MapStart{};
        rideMapStart_.set = true;
        rideMapStart_.road = first;
        rideMapStart_.fwd = fwd;
        rideMapStart_.idx = fwd ? std::min(2, n0 - 1) : std::max(0, n0 - 3);
        const auto& p0 = R0.pts[rideMapStart_.idx];
        const auto& p1 = R0.pts[std::clamp(rideMapStart_.idx + (fwd ? 4 : -4), 0, n0 - 1)];
        float hx = p1.x - p0.x, hz = p1.z - p0.z, hl = std::max(1e-3f, std::hypot(hx, hz));
        rideMapStart_.x = p0.x; rideMapStart_.z = p0.z; rideMapStart_.hx = hx / hl; rideMapStart_.hz = hz / hl;
    }
    return true;
}

void App::roamUpdate() {
    if (!roam_.on || !eng_ || !course_ || !map_ || !course_->world()) return;
    const double ahead = course_->length() - eng_->distance();
    if (roam_.opts.empty()) {
        roam_.opts = roamOptions(roam_.node, roam_.fromRoad, pathHeading(mapPath_));
        roam_.sel = roam_.opts.empty() ? 0 : roamDefault(roam_.opts);
    }
    // the rider's pick
    if (ahead < 1400 && roam_.opts.size() > 1 && !anyTextActive()) {
        const int keys[4] = {KEY_ONE, KEY_TWO, KEY_THREE, KEY_FOUR};
        for (int i = 0; i < int(roam_.opts.size()) && i < 4; ++i)
            if (IsKeyPressed(keys[i])) { roam_.sel = i; sfx::play(sfx::StepChange); }
    }
    if (ahead > 320 || roam_.opts.empty()) return;
    // commit: lay the chosen road on the end of the course
    const size_t before = mapPath_.size();
    const auto& c = roam_.opts[std::clamp(roam_.sel, 0, int(roam_.opts.size()) - 1)];
    if (std::getenv("CORERAY_ROAMLOG")) std::fprintf(stderr, "[roam] at node %d: %zu ways, taking road %d (%s), course %.0f m\n", roam_.node, roam_.opts.size(), c.road, c.label.c_str(), course_->length());
    roamAppend(c.road, roam_.node);
    std::vector<RoutePoint> add;
    const Route& r0 = eng_->route();
    double d = r0.points.empty() ? 0 : r0.points.back().d;
    for (size_t j = before; j < mapPath_.size(); ++j) {
        d += std::hypot(double(mapPath_[j].x - mapPath_[j - 1].x), double(mapPath_[j].z - mapPath_[j - 1].z));
        add.push_back({d, double(mapPath_[j].y)});
    }
    eng_->appendRoutePoints(add);
    const Route& r = eng_->route();
    auto nc = std::make_shared<Course>(*course_);
    auto box = nc->extendWorld(*course_, r, mapPath_);
    course_ = nc;
    renderer_.extendCourse(nc, box.x0, box.z0, box.x1, box.z1);
    worldRouteKey_ = r.id + ":" + std::to_string(r.points.size()) + ":" + std::to_string(int(r.distance())) + ":" + (r.isLoop ? "L" : "P");
    worldRoute_ = r;
}

void App::drawRoamChoices() {
    if (!roam_.on || !eng_ || !course_ || roam_.opts.size() < 2) return;
    const double ahead = course_->length() - eng_->distance();
    if (ahead > 1400 || ahead < 0) return;
    const float W = ui::screenW(), H = ui::screenH();
    const int n = int(std::min<size_t>(roam_.opts.size(), 4));
    const float bw = 250, bh = 66, gap = 12;
    float x = (W - (n * bw + (n - 1) * gap)) / 2, y = H * 0.73f;
    Rectangle head{W / 2 - 180, y - 40, 360, 30};
    DrawRectangleRounded(head, 0.5f, 8, {20, 22, 26, 200});
    textCentered(TextFormat("Junction in %.0f m  -  choose your way", ahead), head, F::Small, {235, 235, 240, 255});
    for (int i = 0; i < n; ++i) {
        Rectangle b{x + i * (bw + gap), y, bw, bh};
        bool sel = i == roam_.sel;
        bool hover = CheckCollisionPointRec(ui::mouseLogical(), b);
        if (hover && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { roam_.sel = i; sfx::play(sfx::StepChange); }
        DrawRectangleRounded(b, 0.18f, 8, sel ? Color{232, 64, 48, 235} : hover ? Color{60, 64, 72, 225} : Color{24, 26, 30, 210});
        const auto& c = roam_.opts[i];
        text(TextFormat("%d  %s", i + 1, c.label.c_str()), b.x + 14, b.y + 10, F::Body, {245, 245, 248, 255});
        text(c.dest, b.x + 14, b.y + 38, F::Small, sel ? Color{255, 230, 225, 255} : Color{180, 184, 192, 255});
    }
}
