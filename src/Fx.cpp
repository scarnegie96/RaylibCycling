// Weather particles (rain, snow, hail), lightning bolts, and wildlife: sheep, cows,
// horses and deer in the fields beside the road (deer bolt when you get close), plus
// flocks of birds and a bird of prey circling overhead.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "RenderUtil.h"
#include "Renderer.h"
#include "Terrain.h"

using namespace rutil;

namespace {
float frand(uint32_t& s) { s = s * 1664525u + 1013904223u; return (s >> 8) / float(1 << 24); }
constexpr float SEG = 300.f;
}  // namespace

// ------------------------------------------------------------------ particles
// Rain: velocity-aligned textured streaks (lit by the sky, lanterns and lightning) plus
// splashes where drops hit the ground. Snow: soft fluttering flakes, with a few large
// out-of-focus flakes right in front of the camera.
void Renderer::updateParticles(const SceneState& s, float dt) {
    const float R = 26.f, top = 14.f, bottom = -6.f;
    Vector3 c = cam_.position;
    static uint32_t rs = 12345;
    auto respawn = [&](Particle& p, bool anyHeight) {
        p.p = {c.x + (frand(rs) * 2 - 1) * R, c.y + (anyHeight ? bottom + frand(rs) * (top - bottom) : top), c.z + (frand(rs) * 2 - 1) * R};
        p.seed = frand(rs);
    };
    int q = std::clamp(quality_, 0, 2);
    size_t wantRain = size_t((q == 2 ? 7000 : q == 1 ? 4500 : 2500) * std::clamp(s.wx.rain + s.wx.hail * 0.3f, 0.f, 1.f));
    size_t wantSnow = size_t((q == 2 ? 6000 : q == 1 ? 4000 : 2200) * std::clamp(s.wx.snow + s.wx.hail * 0.5f, 0.f, 1.f));
    while (rain_.size() < wantRain) { Particle p; respawn(p, true); rain_.push_back(p); }
    if (rain_.size() > wantRain) rain_.resize(wantRain);
    while (snow_.size() < wantSnow) { Particle p; respawn(p, true); snow_.push_back(p); }
    if (snow_.size() > wantSnow) snow_.resize(wantSnow);
    Vector3 wind{std::sin(s.wx.windDir), 0, std::cos(s.wx.windDir)};
    float ws = s.wx.wind * 7.f + s.wx.gust * 4.f;
    auto wrap = [&](Particle& p) {
        if (p.p.y < c.y + bottom) respawn(p, false);
        if (p.p.x < c.x - R) p.p.x += 2 * R; else if (p.p.x > c.x + R) p.p.x -= 2 * R;
        if (p.p.z < c.z - R) p.p.z += 2 * R; else if (p.p.z > c.z + R) p.p.z -= 2 * R;
    };
    for (auto& p : rain_) {
        p.p.y -= (9.0f + p.seed * 3.f) * dt;
        p.p.x += wind.x * ws * dt;
        p.p.z += wind.z * ws * dt;
        wrap(p);
    }
    float t = float(s.time);
    for (auto& p : snow_) {
        bool hail = p.seed < s.wx.hail * 0.6f;
        float fl = 1.3f + p.seed * 2.4f;  // flutter frequency
        p.p.y -= (hail ? 13.f : 0.9f + p.seed * 0.7f) * dt;
        p.p.x += (wind.x * ws * 0.5f + std::sin(t * fl + p.seed * 40.f) * 0.55f + std::sin(t * fl * 2.3f + p.seed * 9.f) * 0.2f) * dt;
        p.p.z += (wind.z * ws * 0.5f + std::cos(t * fl * 0.8f + p.seed * 30.f) * 0.55f) * dt;
        wrap(p);
    }
    partOrigin_ = c;
    // splashes where the rain lands, around the rider / camera
    for (auto& sp : splashes_) sp.t += dt;
    splashes_.erase(std::remove_if(splashes_.begin(), splashes_.end(), [](const Splash& sp) { return sp.t > 0.45f; }), splashes_.end());
    if (s.wx.rain > 0.05f && course_) {
        splashAcc_ += dt * s.wx.rain * (q == 2 ? 700.f : q == 1 ? 450.f : 220.f);
        Vector3 f = Vector3Normalize(Vector3Subtract(cam_.target, cam_.position));
        while (splashAcc_ > 1.f) {
            splashAcc_ -= 1.f;
            // mostly in front of the camera, within ~18 m
            float ang = (frand(rs) - 0.5f) * 1.8f + std::atan2(f.x, f.z);
            float dist = 1.5f + std::sqrt(frand(rs)) * 17.f;
            float x = c.x + std::sin(ang) * dist, z = c.z + std::cos(ang) * dist;
            float ry, y, rd;
            float d = course_->roadDistance(x, z, &ry);
            if (d < course_->roadHalfWidth() + 0.2f) y = ry + 0.06f;
            else if (groundAt(x, z, y, rd)) y += 0.05f;
            else y = course_->terrainHeight(x, z) + 0.05f;
            splashes_.push_back({{x, y, z}, 0.f, 0.6f + frand(rs) * 0.8f});
        }
    } else splashAcc_ = 0;
}

void Renderer::drawParticles(const SceneState& s) {
    if (rain_.empty() && snow_.empty() && splashes_.empty()) return;
    // how much light falls on a particle: sky + lightning, plus nearby lanterns / headlight
    float sky = std::clamp(0.12f + (1.f - L_.night) * 0.7f - s.wx.dark * 0.3f, 0.05f, 1.f) + s.wx.flash * 2.5f;
    auto lit = [&](const Vector3& p) {
        Vector3 col{sky * 0.78f, sky * 0.82f, sky * 0.9f};
        for (auto& L : lights_) {
            Vector3 dv = Vector3Subtract(L.pos, p);
            float d2 = Vector3DotProduct(dv, dv);
            if (d2 > 400.f) continue;
            float spot = 1.f;
            if (L.cosOut > -0.5f) {
                float cdir = -Vector3DotProduct(Vector3Scale(dv, 1.f / std::sqrt(std::max(d2, 1e-4f))), L.dir);
                spot = std::clamp((cdir - L.cosOut) / std::max(1e-3f, L.cosIn - L.cosOut), 0.f, 1.f);
            }
            float k = spot / (1.f + d2 * 0.25f) * 0.35f;
            col = Vector3Add(col, Vector3Scale(L.col, k));
        }
        return col;
    };
    Vector3 camF = Vector3Normalize(Vector3Subtract(cam_.target, cam_.position));
    Vector3 cr = Vector3Normalize(Vector3CrossProduct(camF, {0, 1, 0}));
    Vector3 cu = Vector3CrossProduct(cr, camF);
    const float INT = 4.f;  // particle shader multiplies colour back up (HDR)
    auto col4 = [&](Vector3 c, float a) { rlColor4f(std::min(1.f, c.x / INT), std::min(1.f, c.y / INT), std::min(1.f, c.z / INT), a); };
    rlDisableDepthMask();
    rlDisableBackfaceCulling();
    BeginShaderMode(particleSh_);
    rutil::setF(particleSh_, "intensity", INT);
    Vector3 wind{std::sin(s.wx.windDir), 0, std::cos(s.wx.windDir)};
    float ws = s.wx.wind * 7.f + s.wx.gust * 4.f;
    if (!rain_.empty()) {
        rlSetTexture(tex_.streak.id);
        rlBegin(RL_QUADS);
        for (auto& p : rain_) {
            Vector3 vel{wind.x * ws, -(9.f + p.seed * 3.f), wind.z * ws};
            Vector3 axis = Vector3Normalize(vel);
            Vector3 toCam = Vector3Subtract(cam_.position, p.p);
            float dc = Vector3Length(toCam);
            if (dc < 0.4f) continue;
            Vector3 side = Vector3Normalize(Vector3CrossProduct(axis, Vector3Scale(toCam, 1.f / dc)));
            float len = 0.35f + p.seed * 0.35f, w = 0.006f + dc * 0.0012f;
            Vector3 a0 = Vector3Add(p.p, Vector3Scale(side, -w)), a1 = Vector3Add(p.p, Vector3Scale(side, w));
            Vector3 e = Vector3Add(p.p, Vector3Scale(axis, -len));
            Vector3 b0 = Vector3Add(e, Vector3Scale(side, -w)), b1 = Vector3Add(e, Vector3Scale(side, w));
            Vector3 c = lit(p.p);
            col4(c, (0.22f + 0.3f * s.wx.rain) * (0.6f + 0.4f * p.seed));
            rlTexCoord2f(0, 1); rlVertex3f(a0.x, a0.y, a0.z);
            rlTexCoord2f(1, 1); rlVertex3f(a1.x, a1.y, a1.z);
            rlTexCoord2f(1, 0); rlVertex3f(b1.x, b1.y, b1.z);
            rlTexCoord2f(0, 0); rlVertex3f(b0.x, b0.y, b0.z);
        }
        rlEnd();
        rlDrawRenderBatchActive();
    }
    if (!splashes_.empty()) {
        // expanding rings on the ground...
        rlSetTexture(tex_.ring.id);
        rlBegin(RL_QUADS);
        for (auto& sp : splashes_) {
            float k = sp.t / 0.45f;
            float r = (0.03f + 0.11f * std::sqrt(k)) * sp.size;
            Vector3 c = lit(sp.p);
            col4(c, (1.f - k) * 0.55f);
            Vector3 p = sp.p;
            rlTexCoord2f(0, 0); rlVertex3f(p.x - r, p.y, p.z - r);
            rlTexCoord2f(0, 1); rlVertex3f(p.x - r, p.y, p.z + r);
            rlTexCoord2f(1, 1); rlVertex3f(p.x + r, p.y, p.z + r);
            rlTexCoord2f(1, 0); rlVertex3f(p.x + r, p.y, p.z - r);
        }
        rlEnd();
        rlDrawRenderBatchActive();
        // ...and a little crown of droplets thrown up
        rlSetTexture(tex_.soft.id);
        rlBegin(RL_QUADS);
        for (auto& sp : splashes_) {
            float t = sp.t;
            if (t > 0.3f) continue;
            Vector3 c = lit(sp.p);
            for (int k = 0; k < 4; ++k) {
                float ang = k * 1.57f + sp.size * 7.f;
                float vh = 0.35f * sp.size, vv = 1.1f * sp.size;
                Vector3 q{sp.p.x + std::cos(ang) * vh * t, sp.p.y + vv * t - 4.9f * t * t, sp.p.z + std::sin(ang) * vh * t};
                if (q.y < sp.p.y) continue;
                float sz = 0.012f;
                col4(c, 0.6f * (1.f - t / 0.3f));
                Vector3 a0 = Vector3Add(q, Vector3Add(Vector3Scale(cr, -sz), Vector3Scale(cu, -sz)));
                Vector3 a1 = Vector3Add(q, Vector3Add(Vector3Scale(cr, sz), Vector3Scale(cu, -sz)));
                Vector3 a2 = Vector3Add(q, Vector3Add(Vector3Scale(cr, sz), Vector3Scale(cu, sz)));
                Vector3 a3 = Vector3Add(q, Vector3Add(Vector3Scale(cr, -sz), Vector3Scale(cu, sz)));
                rlTexCoord2f(0, 1); rlVertex3f(a0.x, a0.y, a0.z);
                rlTexCoord2f(1, 1); rlVertex3f(a1.x, a1.y, a1.z);
                rlTexCoord2f(1, 0); rlVertex3f(a2.x, a2.y, a2.z);
                rlTexCoord2f(0, 0); rlVertex3f(a3.x, a3.y, a3.z);
            }
        }
        rlEnd();
        rlDrawRenderBatchActive();
    }
    if (!snow_.empty()) {
        rlSetTexture(tex_.soft.id);
        rlBegin(RL_QUADS);
        size_t i = 0;
        for (auto& p : snow_) {
            ++i;
            bool hail = p.seed < s.wx.hail * 0.6f;
            Vector3 toCam = Vector3Subtract(cam_.position, p.p);
            float dc = Vector3Length(toCam);
            float sz = hail ? 0.018f : 0.022f + p.seed * 0.035f;
            float alpha = 0.85f;
            // every 40th flake drifts right past the lens: big, soft, faint (bokeh)
            Vector3 q = p.p;
            if (!hail && i % 40 == 0) {
                q = Vector3Add(cam_.position, Vector3Add(Vector3Scale(camF, 1.2f + p.seed * 2.5f),
                                                         Vector3Add(Vector3Scale(cr, (p.p.x - cam_.position.x) * 0.08f), Vector3Scale(cu, std::fmod(p.p.y - cam_.position.y + 40.f, 6.f) - 3.f))));
                sz = 0.06f + p.seed * 0.08f;
                alpha = 0.28f;
            } else if (dc < 0.5f) continue;
            Vector3 c = Vector3Scale(lit(q), 1.15f);
            col4(c, alpha);
            Vector3 a0 = Vector3Add(q, Vector3Add(Vector3Scale(cr, -sz), Vector3Scale(cu, -sz)));
            Vector3 a1 = Vector3Add(q, Vector3Add(Vector3Scale(cr, sz), Vector3Scale(cu, -sz)));
            Vector3 a2 = Vector3Add(q, Vector3Add(Vector3Scale(cr, sz), Vector3Scale(cu, sz)));
            Vector3 a3 = Vector3Add(q, Vector3Add(Vector3Scale(cr, -sz), Vector3Scale(cu, sz)));
            rlTexCoord2f(0, 1); rlVertex3f(a0.x, a0.y, a0.z);
            rlTexCoord2f(1, 1); rlVertex3f(a1.x, a1.y, a1.z);
            rlTexCoord2f(1, 0); rlVertex3f(a2.x, a2.y, a2.z);
            rlTexCoord2f(0, 0); rlVertex3f(a3.x, a3.y, a3.z);
        }
        rlEnd();
        rlDrawRenderBatchActive();
    }
    rlSetTexture(0);
    EndShaderMode();
    rlEnableBackfaceCulling();
    rlEnableDepthMask();
}

// ------------------------------------------------------------------ lightning
Renderer::Strike Renderer::lightning(float azimuth) {
    static uint32_t rs = 999;
    rs ^= uint32_t(time_ * 1000.f);
    Strike st;
    const bool dev = azimuth < -100.f;   // dev: 2.5 km straight ahead of the camera
    if (dev) {
        Vector3 f = Vector3Subtract(cam_.target, cam_.position);
        azimuth = std::atan2(f.x, f.z);
    }
    bolt_.pts.clear();
    // find a storm cloud: the densest cloud / rain cell among a few candidates 0.8..6 km away,
    // preferring the direction the weather picked
    Vector3 best{};
    float bestScore = -1;
    for (int k = 0; k < 14; ++k) {
        float az = azimuth + (frand(rs) - 0.5f) * (k < 7 ? 1.2f : 6.2832f);
        float dist = 1500.f + frand(rs) * 3000.f;
        Vector3 c{cam_.position.x + std::sin(az) * dist, 0, cam_.position.z + std::cos(az) * dist};
        float cell = precipCellAt(c.x, c.z);
        float score = cell + frand(rs) * 0.15f;
        if (score > bestScore) { bestScore = score; best = c; }
    }
    if (dev) best = {cam_.position.x + std::sin(azimuth) * 2500.f, 0, cam_.position.z + std::cos(azimuth) * 2500.f};
    float baseY = cloud_.base;
    best.y = baseY + 40.f + 160.f * frand(rs);   // just inside the cloud base, so it shines through it
    bolt_.cloud = best;
    bolt_.strength = 0.65f + 0.35f * frand(rs);
    bolt_.t = 0;
    // a third of strikes come down to the ground, from the base of that cloud
    bolt_.ground = frand(rs) < 0.35f;
    if (bolt_.ground && course_) {
        Vector3 ground{best.x + (frand(rs) - 0.5f) * 300, 0, best.z + (frand(rs) - 0.5f) * 300};
        ground.y = course_->terrainHeight(ground.x, ground.z);
        Vector3 p{best.x, baseY, best.z};
        for (int i = 0; i <= 14; ++i) {
            float t = i / 14.f;
            Vector3 q = Vector3Lerp(p, ground, t);
            if (i > 0 && i < 14) { q.x += (frand(rs) - 0.5f) * 120.f; q.z += (frand(rs) - 0.5f) * 120.f; }
            bolt_.pts.push_back(q);
        }
    }
    Vector3 d = Vector3Subtract(best, cam_.position);
    st.dist = Vector3Length(d);
    if (std::getenv("CORERAY_VOLDEBUG")) std::fprintf(stderr, "[strike] dist %.0f height %.0f (base %.0f) ground %d\n", st.dist, best.y - cam_.position.y, cloud_.base - cam_.position.y, int(bolt_.ground));
    Vector3 f = Vector3Normalize(Vector3Subtract(cam_.target, cam_.position));
    Vector3 r = Vector3Normalize(Vector3CrossProduct(f, {0, 1, 0}));
    st.pan = std::clamp(Vector3DotProduct(Vector3Normalize(d), r) * 1.2f, -1.f, 1.f);
    return st;
}

// the strike flickers: a main flash, a couple of restrikes, then it dies away
float Renderer::boltLight() const {
    float t = bolt_.t;
    if (t < 0) return 0;
    float k = t < 0.07f ? 1.f : t < 0.13f ? 0.25f : t < 0.2f ? 0.85f : t < 0.25f ? 0.2f : t < 0.29f ? 0.6f
            : 0.25f * std::exp(-(t - 0.29f) * 9.f);
    return k * bolt_.strength;
}

void Renderer::drawBolt() {
    if (bolt_.t < 0 || !bolt_.ground || bolt_.pts.size() < 2) return;
    float t = bolt_.t;
    float on = (t < 0.07f || (t > 0.13f && t < 0.2f) || (t > 0.25f && t < 0.29f)) ? 1.f : 0.15f;
    for (size_t i = 0; i + 1 < bolt_.pts.size(); ++i)
        drawPart(cyl_, alignY(bolt_.pts[i], bolt_.pts[i + 1], 2.2f, 2.2f), MatrixIdentity(), {235, 240, 255, 255}, 1.f, 0.f, false, 0, 80.f * on);
}

// ------------------------------------------------------------------ wildlife
void Renderer::updateWildlife(const SceneState& s, float dt) {
    const Course& C = *course_;
    const float L = C.length();
    const uint32_t seed = C.seed();
    double lap = C.loop() ? std::fmod(std::fmod(s.dist, L) + L, L) : s.dist;
    int nseg = std::max(1, int(L / SEG));
    int s0 = int(std::floor((lap - 450) / SEG)), s1 = int(std::floor((lap + 1300) / SEG));
    std::vector<int> want;
    for (int k = s0; k <= s1; ++k) {
        int kk = C.loop() ? ((k % nseg) + nseg) % nseg : k;
        if (!C.loop() && (kk < 0 || kk >= nseg)) continue;
        want.push_back(kk);
    }
    for (auto it = herds_.begin(); it != herds_.end();)
        if (it->first != -999 && std::find(want.begin(), want.end(), it->first) == want.end()) it = herds_.erase(it); else ++it;
    if (std::getenv("CORERAY_ANIMAL_TEST")) {
        // dev: one of each species standing in the verge just ahead of the rider
        static double placedAt = -1;
        if (placedAt < 0 || std::fabs(s.dist - placedAt) > 30 || !herds_.count(-999)) {
            placedAt = s.dist;
            Herd h;
            h.seg = -999;
            for (int k = 0; k < 4; ++k) {
                auto p = C.at(s.dist + 4 + k * 3.2);
                Animal a;
                a.kind = k;
                a.pos = Vector3Add(toV(p.pos), Vector3Scale(rightOf(p.heading), -(7.5f + (k == 1 || k == 3 ? 1.0f : 0.f))));
                a.pos.y = C.terrainHeight(a.pos.x, a.pos.z);
                a.heading = p.heading + PI_F * 0.5f;
                a.phase = k * 1.7f;
                a.seed = 0.3f + k * 0.17f;
                a.coat = k == 0 ? Color{232, 226, 210, 255} : k == 1 ? Color{22, 20, 20, 255} : k == 2 ? Color{118, 74, 44, 255} : Color{98, 58, 32, 255};
                a.coatB = k == 1 ? Color{236, 232, 224, 255} : Color{236, 226, 210, 255};
                a.patch = k == 1 ? 1.f : 0.f;
                a.graze = k == 0 || k == 3 ? 0.9f : 0.f;
                h.animals.push_back(a);
            }
            herds_[-999] = h;
        }
        for (auto& a : herds_[-999].animals) { a.phase += dt; if (a.kind < 0) a.kind = 0; a.fleeT = -1; }
        return;
    }
    // herds keep out of rivers and lakes (checked at the centre and around the herd's spread)
    auto nearWater = [&](Vector3 c, float r) {
        for (int q = 0; q < 5; ++q) {
            float ox = q == 1 ? r : q == 2 ? -r : 0, oz = q == 3 ? r : q == 4 ? -r : 0;
            auto sm = C.field().sample(c.x + ox, c.z + oz);
            if (sm.water > sm.h - 1.5f || sm.bank > 0.3f) return true;
        }
        return false;
    };
    for (int k : want) {
        if (herds_.count(k)) continue;
        Herd h;
        h.seg = k;
        uint32_t rs = uint32_t(k) * 2654435761u ^ seed;
        float r = frand(rs);
        int kind = r < 0.20f ? 0 : r < 0.33f ? 1 : r < 0.44f ? 2 : r < 0.50f ? 3 : -1;
        if (kind >= 0 && k * SEG > 200) {
            for (int attempt = 0; attempt < 8 && h.animals.empty(); ++attempt) {
                float d = k * SEG + 30 + frand(rs) * (SEG - 60);
                float side = frand(rs) < 0.5f ? 1.f : -1.f;
                float lateral = kind == 2 ? 22 + frand(rs) * 30 : 26 + frand(rs) * 60;
                auto p = C.at(d);
                Vector3 c = Vector3Add(toV(p.pos), Vector3Scale(rightOf(p.heading), side * lateral));
                c.y = C.terrainHeight(c.x, c.z);
                float sx = C.terrainHeight(c.x + 4, c.z) - C.terrainHeight(c.x - 4, c.z), sz = C.terrainHeight(c.x, c.z + 4) - C.terrainHeight(c.x, c.z - 4);
                float slope = std::sqrt(sx * sx + sz * sz) / 8.f;
                float forest = fbm(c.x * 0.0035f, c.z * 0.0035f, 4, 77);
                bool okForest = kind == 2 ? (forest > 0.38f && forest < 0.66f) : forest < 0.56f;
                if (slope > 0.42f || !okForest || nearWater(c, 20.f)) continue;
                int n = kind == 0 ? 5 + int(frand(rs) * 8) : kind == 1 ? 3 + int(frand(rs) * 5) : 2 + int(frand(rs) * 3);
                float spread = kind == 0 ? 13.f : kind == 1 ? 18.f : kind == 2 ? 7.f : 15.f;
                for (int i = 0; i < n; ++i) {
                    Animal a;
                    a.kind = kind;
                    a.pos = {c.x + (frand(rs) - 0.5f) * 2 * spread, 0, c.z + (frand(rs) - 0.5f) * 2 * spread};
                    if (C.roadDistance(a.pos.x, a.pos.z) < 9) continue;
                    a.pos.y = C.terrainHeight(a.pos.x, a.pos.z);
                    a.heading = frand(rs) * 2 * PI_F;
                    a.phase = frand(rs) * 30;
                    a.scale = 0.88f + frand(rs) * 0.24f;
                    float cr = frand(rs);
                    a.seed = frand(rs);
                    if (kind == 0) a.coat = a.coatB = cr < 0.1f ? Color{46, 42, 40, 255} : Color{232, 226, 210, 255};
                    else if (kind == 1) {
                        // herds share a breed: Holstein, Hereford-ish red, or Highland-ish cream
                        float breed = hash2(k, 77, seed);
                        if (breed < 0.45f) { a.coat = Color{22, 20, 20, 255}; a.coatB = Color{236, 232, 224, 255}; a.patch = 1.f; }
                        else if (breed < 0.8f) { a.coat = Color{128, 64, 34, 255}; a.coatB = Color{232, 224, 212, 255}; a.patch = cr < 0.5f ? 0.6f : 0.f; }
                        else { a.coat = Color{208, 190, 160, 255}; a.coatB = a.coat; }
                    } else if (kind == 2) { a.coat = Color{118, 74, 44, 255}; a.coatB = Color{236, 226, 210, 255}; }
                    else {
                        a.coat = cr < 0.4f ? Color{98, 58, 32, 255} : cr < 0.7f ? Color{36, 28, 24, 255} : cr < 0.88f ? Color{200, 192, 180, 255} : Color{120, 82, 50, 255};
                        if (cr >= 0.88f) { a.coatB = Color{240, 236, 228, 255}; a.patch = 0.9f; }
                    }
                    h.animals.push_back(a);
                }
            }
        }
        herds_[k] = std::move(h);
    }
    if (std::getenv("CORERAY_DEBUG")) {
        static int fr = 0;
        if (++fr % 60 == 0) {
            int n = 0, kinds[4] = {};
            for (auto& [k, h] : herds_) for (auto& a : h.animals) if (a.kind >= 0) { ++n; kinds[a.kind]++; }
            std::fprintf(stderr, "[wildlife] %zu segments, %d animals (sheep %d cows %d deer %d horses %d), birds %zu\n", herds_.size(), n, kinds[0], kinds[1], kinds[2], kinds[3], birds_.size());
        }
    }
    // behaviour
    auto rp = C.at(s.dist);
    Vector3 rider = toV(rp.pos);
    float t = float(s.time);
    for (auto& [k, h] : herds_)
        for (auto& a : h.animals) {
            if (a.kind < 0) continue;
            a.phase += dt;
            float dx = a.pos.x - rider.x, dz = a.pos.z - rider.z;
            float dr = std::sqrt(dx * dx + dz * dz);
            if (a.fleeT >= 0) {
                a.fleeT += dt;
                float runSpeed = a.kind == 2 ? 10.f : a.kind == 3 ? 6.f : 2.5f;
                a.speed += (runSpeed - a.speed) * std::min(1.f, dt * 3);
                if (a.kind != 2 && a.fleeT > 3.5f) a.fleeT = -1;              // settle down again
                if (a.kind == 2 && a.fleeT > 9.f) { a.kind = -1; continue; }  // gone into the woods
                a.graze += (0 - a.graze) * std::min(1.f, dt * 6);
            } else {
                bool scare = (a.kind == 2 && dr < 48) || (a.kind == 3 && dr < 26 && s.speed > 6) || (a.kind == 0 && dr < 16);
                if (scare) {
                    a.fleeT = 0;
                    a.heading = std::atan2(dx, dz) + (std::fmod(a.phase * 7.3f, 1.f) - 0.5f) * 0.8f;
                } else {
                    // graze, wander a few steps now and then
                    float cyc = 10.f + std::fmod(a.scale * 37.f, 8.f);
                    bool walk = std::fmod(a.phase, cyc) < 2.6f;
                    a.speed += ((walk ? 0.55f : 0.f) - a.speed) * std::min(1.f, dt * 2);
                    a.graze += ((walk ? 0.1f : 0.9f) - a.graze) * std::min(1.f, dt * 1.5f);
                    a.heading += std::sin(t * 0.21f + a.scale * 13.f) * 0.25f * dt;
                    if (C.roadDistance(a.pos.x, a.pos.z) < 8.f) {  // keep off the road
                        int idx;
                        C.roadDistance(a.pos.x, a.pos.z, nullptr, &idx);
                        if (idx >= 0) {
                            auto& rpnt = C.points()[size_t(idx)].pos;
                            a.heading = std::atan2(a.pos.x - rpnt.x, a.pos.z - rpnt.z);
                        }
                    }
                }
            }
            a.pos.x += std::sin(a.heading) * a.speed * dt;
            a.pos.z += std::cos(a.heading) * a.speed * dt;
            a.pos.y = C.terrainHeight(a.pos.x, a.pos.z);
        }
    // birds (daylight, not in heavy rain)
    bool birdsOut = L_.night < 0.45f && s.wx.rain < 0.6f && s.wx.snow < 0.6f;
    if (!birdsOut) birds_.clear();
    else if (birds_.empty()) {
        uint32_t rs = seed ^ 0xB1D5u;
        for (int f = 0; f < 2; ++f) {
            float rad = 14 + frand(rs) * 10, hgt = 26 + frand(rs) * 22, spd = 0.45f + frand(rs) * 0.25f;
            Vector3 c{(frand(rs) - 0.5f) * 240, 0, 80 + frand(rs) * 160};
            int n = 7 + int(frand(rs) * 7);
            for (int i = 0; i < n; ++i) birds_.push_back({c, rad + (frand(rs) - 0.5f) * 6, hgt + (frand(rs) - 0.5f) * 5, spd * (f ? -1.f : 1.f), frand(rs) * 6.28f, 0.22f + frand(rs) * 0.08f, false});
        }
        birds_.push_back({{(frand(rs) - 0.5f) * 200, 0, 150}, 55, 105, 0.12f, 0, 0.9f, true});
    }
}

void Renderer::drawWildlife(bool sp, const Vector3& focus, float radius) {
    for (auto& [k, h] : herds_)
        for (auto& a : h.animals) {
            if (a.kind < 0) continue;
            if (sp) { if (std::fabs(a.pos.x - focus.x) > radius || std::fabs(a.pos.z - focus.z) > radius) continue; }
            else {
                Vector3 v = Vector3Subtract(a.pos, cam_.position);
                float d = Vector3Length(v);
                if (d > 320.f) continue;
                Vector3 f = Vector3Normalize(Vector3Subtract(cam_.target, cam_.position));
                if (d > 6 && Vector3DotProduct(v, f) / d < 0.2f) continue;
            }
            drawAnimal(a, sp);
        }
}

void Renderer::drawBirds(const SceneState& s) {
    if (birds_.empty()) return;
    auto rp = course_->at(s.dist);
    Vector3 rider = toV(rp.pos);
    float t = float(s.time);
    // flocks drift along with the rider, a little ahead
    Vector3 fwd = fwdOf(rp.heading), right = rightOf(rp.heading);
    for (auto& b : birds_) {
        float ang = b.phase + t * b.speed;
        Vector3 center = Vector3Add(rider, Vector3Add(Vector3Scale(right, b.c.x), Vector3Scale(fwd, b.c.z)));
        float ground = course_->terrainHeight(center.x, center.z);
        Vector3 p{center.x + std::cos(ang) * b.radius, ground + b.height + std::sin(t * 0.7f + b.phase) * 2.f, center.z + std::sin(ang) * b.radius};
        float heading = std::atan2(-std::sin(ang) * (b.speed > 0 ? 1.f : -1.f), std::cos(ang) * (b.speed > 0 ? 1.f : -1.f));
        float flap = b.raptor ? std::sin(t * 1.5f + b.phase) * 0.15f + 0.1f : std::sin(t * 14.f + b.phase * 7.f) * 0.7f;
        if (b.raptor && std::fmod(t + b.phase, 9.f) > 2.f) flap = 0.08f;  // mostly soaring
        Matrix base = MatrixMultiply(MatrixMultiply(MatrixScale(b.size, b.size, b.size), MatrixRotateY(heading)), MatrixTranslate(p.x, p.y, p.z));
        Color col = b.raptor ? Color{70, 52, 36, 255} : Color{30, 30, 34, 255};
        drawPart(sphere_, MatrixScale(0.25f, 0.22f, 0.7f), base, col, 0.8f, 0.1f, false);
        for (float side : {-1.f, 1.f}) {
            Matrix w = MatrixMultiply(MatrixMultiply(MatrixScale(1.1f, 0.04f, 0.35f), MatrixTranslate(side * 0.55f, 0, 0)), MatrixRotateZ(side * flap));
            drawPart(cube_, w, base, col, 0.8f, 0.1f, false);
        }
    }
}
