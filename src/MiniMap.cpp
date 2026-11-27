#include "MiniMap.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "Ui.h"
#include "raymath.h"
#include "rlgl.h"
#include "world/WorldMap.h"

std::shared_ptr<MiniMap::Bake> miniMapBake(const wmap::Map& M, int maxRes);   // MiniMapBake.cpp

MiniMap::~MiniMap() {
    if (job_.valid()) job_.wait();   // the texture is freed by release(), while the window is open
}

void MiniMap::release() {
    if (job_.valid()) job_.wait();
    if (tex_.id) UnloadTexture(tex_);
    tex_ = Texture2D{};
    map_.reset();
    towns_.clear();
}

void MiniMap::setMap(std::shared_ptr<const wmap::Map> m) {
    if (m == map_) return;
    if (job_.valid()) job_.wait();
    if (tex_.id) UnloadTexture(tex_);
    tex_ = Texture2D{};
    towns_.clear();
    map_ = m;
    if (!m) return;
    for (const auto& t : m->towns) towns_.push_back({t.x, t.z, t.rank, t.name});
    job_ = std::async(std::launch::async, [m] {
        auto t0 = std::chrono::steady_clock::now();
        auto b = miniMapBake(*m, 4096);
        TraceLog(LOG_INFO, "[minimap] baked %dx%d in %.2f s", b->R, b->R,
                 std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        return b;
    });
}

namespace {
// raylib wants counter-clockwise triangles; take them either way round
void tri(Vector2 a, Vector2 b, Vector2 c, Color col) {
    if ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x) > 0) std::swap(b, c);
    DrawTriangle(a, b, c, col);
}
}  // namespace

MiniMap::Hit MiniMap::draw(const View& v, float dt) {
    using namespace ui;
    anim_ += dt;
    if (job_.valid() && job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto b = job_.get();
        Image img{b->rgba.data(), b->R, b->R, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        tex_ = LoadTextureFromImage(img);
        GenTextureMipmaps(&tex_);
        SetTextureFilter(tex_, TEXTURE_FILTER_TRILINEAR);
        SetTextureWrap(tex_, TEXTURE_WRAP_CLAMP);
        L_ = b->L; x0_ = b->x0; z0_ = b->z0;
    }
    const Vector2 c = v.centre;
    const float r = v.radius;
    const Vector2 mouse = mouseLogical();
    const bool press = mouseFree() && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    // + / - dial buttons: discs tucked under the lower rim, either side of the range pill,
    // drawn first so the map's frame overlaps them and they seem to slide out from beneath
    struct Dial { Vector2 at, glyph; bool enabled, hover; };
    const float br = 15.f + r * 0.02f;                           // button radius
    auto dial = [&](float sx, bool enabled) {
        const float ang = (90.f + sx * 38.f) * DEG2RAD;          // 38 degrees either side of straight down
        Vector2 dir{std::cos(ang), std::sin(ang)};
        Dial d;
        // far enough out that the glyph, centred in its disc, clears the map's frame (r + 4)
        const float out = r + std::max(br * 0.7f, 4.f + 5.5f + 2.f);
        d.at = {c.x + dir.x * out, c.y + dir.y * out};
        d.glyph = d.at;
        d.enabled = enabled;
        d.hover = enabled && mouseFree() && Vector2Distance(mouse, d.at) <= br && Vector2Distance(mouse, c) > r + 3;
        return d;
    };
    const Dial out = dial(1, v.canZoomOut), in = dial(-1, v.canZoomIn);   // - on the left, + on the right (clockwise = in)
    // shadow, then the buttons, then the frame
    DrawCircleV({c.x, c.y + 2.5f}, r + 6, Color{15, 23, 42, 34});
    for (const Dial* d : {&out, &in}) {
        DrawCircleV({d->at.x, d->at.y + 2}, br + 1.5f, Color{15, 23, 42, 40});
        DrawCircleV(d->at, br, d->hover ? Color{255, 255, 255, 255} : Color{238, 238, 234, 255});
        DrawCircleLinesV(d->at, br, Color{15, 23, 42, 46});
    }
    DrawCircleV(c, r + 4, Color{250, 250, 248, 255});
    DrawRing(c, r + 4, r + 5, 0, 360, 96, Color{15, 23, 42, 40});
    for (const Dial* d : {&out, &in}) {
        const bool plus = d == &in;
        Color ink = !d->enabled ? Color{40, 40, 46, 70} : d->hover ? ACCENT : Color{40, 40, 46, 255};
        const float a = 5.5f, t = 2.2f;
        DrawRectangleRec({d->glyph.x - a, d->glyph.y - t / 2, 2 * a, t}, ink);
        if (plus) DrawRectangleRec({d->glyph.x - t / 2, d->glyph.y - a, t, 2 * a}, ink);
    }
    Hit hit = Hit::None;
    if (press && in.hover) hit = Hit::ZoomIn;
    else if (press && out.hover) hit = Hit::ZoomOut;
    else if (press && CheckCollisionPointCircle(mouse, c, r + 4)) hit = Hit::Map;
    if (!ready()) {
        DrawCircleV(c, r, Color{236, 233, 224, 255});
        DrawRing({c.x, c.y - 10}, 12, 15, anim_ * 360.f, anim_ * 360.f + 260.f, 32, ACCENT);
        textCentered("Drawing the map...", {c.x - r, c.y + 10, 2 * r, 20}, F::Small, Color{90, 90, 96, 255}, 12);
        return hit == Hit::Map ? Hit::None : hit;
    }
    // map rotation: screen direction (sx, sy) shows the world direction R(phi)(sx, sy);
    // heading-up puts the rider's direction of travel at the top
    const float phi = v.northUp ? 0.f : PI - v.heading;
    const float cp = std::cos(phi), sp = std::sin(phi);
    auto toWorldDir = [&](float sx, float sy) { return Vector2{sx * cp - sy * sp, sx * sp + sy * cp}; };
    auto toScreen = [&](float wx, float wz) {   // world point -> screen
        float dx = (wx - v.x) / v.rangeM * r, dz = (wz - v.z) / v.rangeM * r;
        return Vector2{c.x + dx * cp + dz * sp, c.y - dx * sp + dz * cp};
    };
    // the map: a textured fan, clipped to the circle by construction
    {
        const int N = 96;
        const float u0 = (v.x - x0_) / L_, v0 = (v.z - z0_) / L_, rr = v.rangeM / L_;
        // rlBegin resets the draw's texture when the mode changes, and a texture set on a draw
        // that already has vertices starts one with a stale mode: flush, begin, then bind
        rlDrawRenderBatchActive();
        rlBegin(RL_TRIANGLES);
        rlSetTexture(tex_.id);
        rlColor4ub(255, 255, 255, 255);
        for (int i = 0; i < N; ++i) {
            float a0 = 2 * PI * i / N, a1 = 2 * PI * (i + 1) / N;
            Vector2 d0 = toWorldDir(std::cos(a0), std::sin(a0)), d1 = toWorldDir(std::cos(a1), std::sin(a1));
            rlTexCoord2f(u0, v0); rlVertex2f(c.x, c.y);
            rlTexCoord2f(u0 + d1.x * rr, v0 + d1.y * rr); rlVertex2f(c.x + r * std::cos(a1), c.y + r * std::sin(a1));
            rlTexCoord2f(u0 + d0.x * rr, v0 + d0.y * rr); rlVertex2f(c.x + r * std::cos(a0), c.y + r * std::sin(a0));
        }
        rlEnd();
        rlDrawRenderBatchActive();
        rlSetTexture(0);
    }
    auto inside = [&](Vector2 p, float margin) { return Vector2Distance(p, c) <= r - margin; };
    // the road ahead: a white-edged accent line, stopped at the rim
    if (v.ahead.size() >= 2) {
        std::vector<Vector2> pts;
        for (const auto& w : v.ahead) {
            Vector2 p = toScreen(w.x, w.y);
            if (!inside(p, 3)) {
                if (!pts.empty()) {   // end exactly on the rim
                    Vector2 a = pts.back(), d = Vector2Subtract(p, a);
                    float lo = 0, hi = 1;
                    for (int k = 0; k < 12; ++k) { float m = 0.5f * (lo + hi); (inside(Vector2Add(a, Vector2Scale(d, m)), 3) ? lo : hi) = m; }
                    pts.push_back(Vector2Add(a, Vector2Scale(d, lo)));
                }
                break;
            }
            if (pts.empty() || Vector2Distance(pts.back(), p) > 1.5f) pts.push_back(p);
        }
        if (pts.size() >= 2) {
            DrawSplineLinear(pts.data(), int(pts.size()), 7.f, Color{255, 255, 255, 230});
            DrawSplineLinear(pts.data(), int(pts.size()), 4.f, ACCENT);
        }
    }
    // places: a dot and a name pill (villages only when zoomed in)
    for (const auto& t : towns_) {
        if (t.rank < 2 && v.rangeM > 1800) continue;
        Vector2 p = toScreen(t.x, t.z);
        if (!inside(p, 8)) continue;
        float fs = t.rank >= 2 ? 12.f : 11.f;
        Vector2 m = measure(t.name, F::Small, fs);
        Rectangle pill{p.x - m.x / 2 - 6, p.y - m.y - 11, m.x + 12, m.y + 4};
        if (!inside({pill.x, pill.y}, 4) || !inside({pill.x + pill.width, pill.y}, 4)) {
            pill.y = p.y + 7;   // flip under the dot near the top of the circle
            if (!inside({pill.x, pill.y + pill.height}, 4) || !inside({pill.x + pill.width, pill.y + pill.height}, 4)) continue;
        }
        // keep clear of the rider's arrow at the centre
        const Rectangle me{c.x - 17, c.y - 17, 34, 34};
        if (CheckCollisionRecs(pill, me)) {
            pill.y = p.y + 7;
            if (CheckCollisionRecs(pill, me)) pill.y = me.y - pill.height - 2;
        }
        DrawCircleV(p, t.rank >= 2 ? 4.f : 3.f, Color{40, 40, 46, 255});
        DrawCircleV(p, t.rank >= 2 ? 2.2f : 1.6f, WHITE);
        DrawRectangleRounded(pill, 1, 8, Color{255, 255, 255, 215});
        text(t.name, pill.x + 6, pill.y + 2, F::Small, Color{40, 40, 46, 255}, fs);
    }
    // the rider: an arrow at the centre
    {
        Vector2 w{std::sin(v.heading), std::cos(v.heading)};   // world direction of travel
        Vector2 d{w.x * cp + w.y * sp, -w.x * sp + w.y * cp};   // on screen
        Vector2 p{-d.y, d.x};
        auto at = [&](float f, float s) { return Vector2{c.x + d.x * f + p.x * s, c.y + d.y * f + p.y * s}; };
        DrawCircleV(c, 15, Color{255, 255, 255, 70});
        const float k = 1.35f;   // white outline
        tri(at(11 * k, 0), at(-7 * k, 8 * k), at(-3 * k, 0), WHITE);
        tri(at(11 * k, 0), at(-3 * k, 0), at(-7 * k, -8 * k), WHITE);
        tri(at(11, 0), at(-7, 8), at(-3, 0), ACCENT);
        tri(at(11, 0), at(-3, 0), at(-7, -8), ACCENT);
    }
    // rim, north badge, scale
    DrawRing(c, r - 0.5f, r + 4, 0, 360, 96, Color{250, 250, 248, 255});
    {
        Vector2 nd{-sp, -cp};   // world -z (north) on screen
        // on the rim, except where a zoom dial is: then just inside it
        float na = std::atan2(nd.y, nd.x) * RAD2DEG;
        float near = 180;
        for (float da : {90.f - 38.f, 90.f + 38.f}) near = std::min(near, std::fabs(std::remainder(na - da, 360.f)));
        float nr = near < 16.f ? r - 13.f : r + 1.f;
        Vector2 nb{c.x + nd.x * nr, c.y + nd.y * nr};
        DrawCircleV(nb, 10.5f, Color{255, 255, 255, 255});
        DrawCircleV(nb, 9, Color{40, 40, 46, 255});
        textCentered("N", {nb.x - 9, nb.y - 9, 18, 18}, F::Small, WHITE, 11);
    }
    {
        std::string s = v.rangeM >= 1000 ? TextFormat("%.1f km", v.rangeM / 1000.f) : TextFormat("%.0f m", v.rangeM);
        Vector2 m = measure(s, F::Small, 10);
        Rectangle pill{c.x - m.x / 2 - 6, c.y + r - m.y - 6, m.x + 12, m.y + 2};
        DrawRectangleRounded(pill, 1, 8, Color{255, 255, 255, 200});
        text(s, pill.x + 6, pill.y + 1, F::Small, Color{60, 60, 66, 255}, 10);
    }
    return hit;
}
