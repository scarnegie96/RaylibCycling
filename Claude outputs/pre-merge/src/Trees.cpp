#include "Trees.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "RenderUtil.h"

using namespace rutil;

namespace trees {
namespace {

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed * 2891336453u + 747796405u) { for (int i = 0; i < 4; ++i) f(); }
    float f() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (s & 0xFFFFFF) / float(0x1000000); }
    float r(float a, float b) { return a + (b - a) * f(); }
    Vector3 unit() {
        float z = r(-1, 1), a = r(0, 2 * PI_F), q = std::sqrt(1 - z * z);
        return {q * std::cos(a), z, q * std::sin(a)};
    }
};

// ======================================================================== leaf atlas
struct Canvas {
    int n;
    std::vector<float> r, g, b, a;
    explicit Canvas(int size) : n(size), r(size * size, 0.f), g(size * size, 0.f), b(size * size, 0.f), a(size * size, 0.f) {}
    void put(int x, int y, float cr, float cg, float cb, float al) {
        if (x < 0 || y < 0 || x >= n || y >= n || al <= 0) return;
        int i = y * n + x;
        float k = std::min(1.f, al);
        r[i] = r[i] * (1 - k) + cr * k; g[i] = g[i] * (1 - k) + cg * k; b[i] = b[i] * (1 - k) + cb * k;
        a[i] = std::max(a[i], k);
    }
    // thick anti-aliased line
    void line(float x0, float y0, float x1, float y1, float w0, float w1, float cr, float cg, float cb) {
        float len = std::hypot(x1 - x0, y1 - y0);
        int steps = std::max(1, int(len * 2));
        for (int s = 0; s <= steps; ++s) {
            float t = float(s) / steps;
            float x = x0 + (x1 - x0) * t, y = y0 + (y1 - y0) * t, w = (w0 + (w1 - w0) * t) * 0.5f;
            int R = int(std::ceil(w + 1));
            for (int oy = -R; oy <= R; ++oy)
                for (int ox = -R; ox <= R; ++ox) {
                    float d = std::hypot(float(ox) + (x - std::floor(x)) - 0.5f, float(oy) + (y - std::floor(y)) - 0.5f);
                    put(int(x) + ox, int(y) + oy, cr, cg, cb, std::clamp(w + 0.5f - d, 0.f, 1.f));
                }
        }
    }
    // a leaf: from (x,y) along angle, with a pointed lanceolate outline and a darker midrib
    void leaf(float x, float y, float ang, float len, float wid, float cr, float cg, float cb, float serr) {
        float ca = std::cos(ang), sa = std::sin(ang);
        int R = int(len + wid) + 2;
        for (int oy = -R; oy <= R; ++oy)
            for (int ox = -R; ox <= R; ++ox) {
                float lu = (ox * ca + oy * sa) / len, lv = (-ox * sa + oy * ca);
                if (lu < 0 || lu > 1) continue;
                float w = wid * std::pow(std::sin(PI_F * std::pow(lu, 0.8f)), 0.75f) * (1 + serr * std::sin(lu * 46.f));
                float e = w - std::fabs(lv);
                if (e < -0.5f) continue;
                float al = std::clamp(e + 0.5f, 0.f, 1.f);
                float shade = 0.82f + 0.3f * lu - 0.18f * std::fabs(lv) / std::max(1.f, wid);
                float rib = std::fabs(lv) < std::max(0.7f, wid * 0.07f) ? 0.78f : 1.f;
                float vein = 0.94f + 0.06f * std::sin((lu * 9.f + std::fabs(lv) / std::max(1.f, wid) * 3.f) * PI_F);
                put(int(x) + ox, int(y) + oy, cr * shade * rib * vein, cg * shade * rib * vein, cb * shade * rib, al);
            }
    }
};

void paintBroad(Canvas& c, int x0, int y0, int C, uint32_t seed) {
    Rng rng(seed);
    float cx = x0 + C * 0.5f, cy = y0 + C * 0.92f;
    // a forked twig
    std::vector<std::pair<float, float>> pts;
    float px = cx, py = cy;
    for (int i = 0; i < 9; ++i) {
        float nx = px + rng.r(-0.04f, 0.04f) * C, ny = py - 0.085f * C;
        c.line(px, py, nx, ny, 5.f - i * 0.4f, 4.6f - i * 0.4f, 0.24f, 0.17f, 0.10f);
        pts.push_back({nx, ny});
        px = nx; py = ny;
    }
    for (int k = 0; k < 4; ++k) {
        auto [sx, sy] = pts[2 + k * 2 > 8 ? 8 : 2 + k * 2];
        float ex = sx + (k % 2 ? 1 : -1) * rng.r(0.18f, 0.3f) * C, ey = sy - rng.r(0.08f, 0.2f) * C;
        c.line(sx, sy, ex, ey, 3.5f, 2.f, 0.24f, 0.17f, 0.10f);
        pts.push_back({ex, ey});
    }
    for (int i = 0; i < 64; ++i) {
        auto [sx, sy] = pts[size_t(rng.f() * pts.size()) % pts.size()];
        float ang = rng.r(0, 2 * PI_F);
        float len = rng.r(0.13f, 0.21f) * C;
        float hue = rng.f();
        float cr = 0.16f + 0.1f * hue, cg = 0.34f + 0.12f * rng.f(), cb = 0.07f + 0.04f * hue;
        float ox = sx + std::cos(ang) * rng.r(0, 0.05f) * C, oy = sy + std::sin(ang) * rng.r(0, 0.05f) * C;
        // keep the leaf inside the cell (mip-map safety margin)
        float tx = ox + std::cos(ang) * len, ty = oy + std::sin(ang) * len;
        if (tx < x0 + 14 || tx > x0 + C - 14 || ty < y0 + 14 || ty > y0 + C - 14) continue;
        c.leaf(ox, oy, ang, len, len * rng.r(0.3f, 0.4f), cr, cg, cb, 0.06f);
    }
}

void paintSmall(Canvas& c, int x0, int y0, int C, uint32_t seed) {
    Rng rng(seed);
    for (int t = 0; t < 4; ++t) {
        float px = x0 + C * (0.2f + 0.2f * t), py = y0 + C * 0.06f;
        for (int i = 0; i < 10; ++i) {
            float nx = px + rng.r(-0.03f, 0.03f) * C + (t - 1.5f) * 0.01f * C, ny = py + 0.085f * C;
            c.line(px, py, nx, ny, 2.2f, 1.8f, 0.26f, 0.2f, 0.14f);
            for (int l = 0; l < 3; ++l) {
                float ang = PI_F * 0.5f + rng.r(-1.3f, 1.3f);
                float len = rng.r(0.055f, 0.085f) * C;
                float tx = nx + std::cos(ang) * len, ty = ny + std::sin(ang) * len;
                if (tx < x0 + 12 || tx > x0 + C - 12 || ty < y0 + 12 || ty > y0 + C - 12) continue;
                c.leaf(nx, ny, ang, len, len * 0.5f, 0.28f + 0.08f * rng.f(), 0.44f + 0.1f * rng.f(), 0.1f, 0.12f);
            }
            px = nx; py = ny;
        }
    }
}

void paintSpray(Canvas& c, int x0, int y0, int C, uint32_t seed) {
    Rng rng(seed);
    // main stem bottom -> top, side twigs, dense short needles on every twig
    struct Tw { float x0, y0, x1, y1; };
    std::vector<Tw> tw;
    tw.push_back({x0 + C * 0.5f, y0 + C * 0.96f, x0 + C * 0.5f, y0 + C * 0.05f});
    for (int i = 0; i < 9; ++i) {
        float y = y0 + C * (0.88f - i * 0.09f);
        float L = C * (0.38f - i * 0.025f);
        for (float s : {-1.f, 1.f}) tw.push_back({x0 + C * 0.5f, y, x0 + C * 0.5f + s * L * 0.95f, y - L * 0.55f});
    }
    for (auto& t : tw) {
        c.line(t.x0, t.y0, t.x1, t.y1, 2.5f, 1.5f, 0.22f, 0.17f, 0.12f);
        float len = std::hypot(t.x1 - t.x0, t.y1 - t.y0);
        float dx = (t.x1 - t.x0) / len, dy = (t.y1 - t.y0) / len;
        int nn = int(len / 2.2f);
        for (int k = 0; k < nn; ++k) {
            float f = float(k) / nn;
            float px = t.x0 + (t.x1 - t.x0) * f, py = t.y0 + (t.y1 - t.y0) * f;
            for (float s : {-1.f, 1.f}) {
                float ang = std::atan2(dy, dx) + s * rng.r(0.6f, 1.1f);
                float nl = C * rng.r(0.028f, 0.04f);
                float g = rng.f();
                c.line(px, py, px + std::cos(ang) * nl, py + std::sin(ang) * nl, 1.8f, 1.f, 0.07f + 0.04f * g, 0.2f + 0.08f * g, 0.12f + 0.04f * g);
            }
        }
    }
}

void paintTufts(Canvas& c, int x0, int y0, int C, uint32_t seed) {
    Rng rng(seed);
    float cs[4][2] = {{0.3f, 0.3f}, {0.7f, 0.32f}, {0.3f, 0.7f}, {0.68f, 0.7f}};
    for (auto& q : cs) {
        float cx = x0 + q[0] * C, cy = y0 + q[1] * C;
        c.line(cx, cy + 0.2f * C, cx, cy, 5.f, 3.f, 0.3f, 0.2f, 0.12f);
        for (int k = 0; k < 70; ++k) {
            float ang = rng.r(0, 2 * PI_F);
            float len = rng.r(0.09f, 0.17f) * C;
            float g = rng.f();
            c.line(cx, cy, cx + std::cos(ang) * len, cy + std::sin(ang) * len * 0.9f, 2.2f, 0.8f, 0.12f + 0.05f * g, 0.27f + 0.08f * g, 0.12f + 0.03f * g);
        }
    }
}

// ======================================================================== tree skeletons
struct Card { Vector3 c, U, V; int cell; };
struct Seg { Vector3 a, b; float ra, rb; int depth; };

struct Params {
    bool excurrent;          // trunk to the top (conifers, birch, poplar) vs. splitting crown
    float height, trunkR, firstBranch;
    int limbs, depth;
    float angle, lenRatio, upward, droop, crook;
    int cell;
    float leafSize;
    int leavesPerTip;
    bool spray, tufts;
    unsigned char bark;      // 0 brown, 85 birch, 170 pine, 255 spruce grey
};

Params params(Species s, Rng& rng) {
    switch (s) {
        case Oak: return {false, rng.r(13, 16), 0.42f, rng.r(3.2f, 4.2f), 5, 3, 0.95f, 0.62f, 0.15f, 0.25f, 0.35f, 0, 1.5f, 7, false, false, 0};
        case Beech: return {false, rng.r(16, 19), 0.38f, rng.r(4.5f, 5.5f), 4, 3, 0.62f, 0.68f, 0.45f, 0.1f, 0.2f, 0, 1.35f, 7, false, false, 0};
        case Birch: return {true, rng.r(14, 17), 0.17f, rng.r(4.f, 5.5f), 0, 2, 0.85f, 0.55f, 0.25f, 0.55f, 0.25f, 1, 1.0f, 6, false, false, 85};
        case Poplar: return {true, rng.r(20, 24), 0.3f, rng.r(2.5f, 3.5f), 0, 2, 0.38f, 0.5f, 0.75f, 0.05f, 0.15f, 1, 1.1f, 6, false, false, 0};
        case Spruce: return {true, rng.r(19, 24), 0.32f, rng.r(1.0f, 1.8f), 0, 1, 1.45f, 0.42f, 0.0f, 0.3f, 0.08f, 2, 1.3f, 0, true, false, 255};
        default: return {true, rng.r(17, 21), 0.3f, rng.r(10.f, 12.5f), 0, 2, 1.0f, 0.6f, 0.35f, 0.15f, 0.25f, 3, 1.3f, 4, false, true, 170};
    }
}

struct Tree {
    std::vector<Seg> segs;
    std::vector<Card> cards;
    Rng rng;
    Params P;
    explicit Tree(uint32_t seed) : rng(seed) {}

    Vector3 perp(Vector3 d) {
        Vector3 a = std::fabs(d.y) < 0.95f ? Vector3{0, 1, 0} : Vector3{1, 0, 0};
        Vector3 u = Vector3Normalize(Vector3CrossProduct(d, a));
        float t = rng.r(0, 2 * PI_F);
        Vector3 w = Vector3CrossProduct(d, u);
        return Vector3Normalize(Vector3Add(Vector3Scale(u, std::cos(t)), Vector3Scale(w, std::sin(t))));
    }
    void addLeaves(Vector3 p, Vector3 dir, float spread) {
        for (int k = 0; k < P.leavesPerTip; ++k) {
            Vector3 c = Vector3Add(p, Vector3Scale(rng.unit(), spread * rng.r(0.2f, 1.f)));
            c = Vector3Add(c, Vector3Scale(dir, rng.r(-0.3f, 0.5f) * spread));
            float sz = P.leafSize * rng.r(0.75f, 1.25f);
            Vector3 n = rng.unit();
            Vector3 U = Vector3Scale(perp(n), sz * 0.5f);
            Vector3 V = Vector3Scale(Vector3Normalize(Vector3CrossProduct(n, U)), sz * 0.5f);
            cards.push_back({c, U, V, P.cell});
        }
    }
    // a branch made of a few bent segments; spawns children; returns the tip
    void branch(Vector3 p, Vector3 dir, float len, float r, int depth) {
        const int n = depth == 0 ? 4 : 3;
        Vector3 cur = p;
        float rr = r;
        std::vector<std::pair<Vector3, Vector3>> path;  // points + dirs
        for (int i = 0; i < n; ++i) {
            dir = Vector3Normalize(Vector3Add(dir, Vector3Add(Vector3Scale(rng.unit(), P.crook * 0.5f), {0, P.upward * 0.25f - P.droop * 0.25f * float(depth) / std::max(1, P.depth), 0})));
            Vector3 nx = Vector3Add(cur, Vector3Scale(dir, len / n));
            float rn = r * (1.f - 0.65f * float(i + 1) / n);
            segs.push_back({cur, nx, rr, rn, depth});
            path.push_back({nx, dir});
            cur = nx;
            rr = rn;
        }
        if (depth >= P.depth) {
            addLeaves(cur, dir, P.leafSize * 1.2f);
            if (depth > 0) addLeaves(path[n / 2].first, dir, P.leafSize);
            return;
        }
        int kids = depth == 0 ? P.limbs : 3;
        for (int k = 0; k < kids; ++k) {
            float t = depth == 0 ? 1.f : rng.r(0.45f, 1.f);
            auto at = path[std::min(n - 1, int(t * n))];
            Vector3 cd = Vector3Normalize(Vector3Add(Vector3Scale(at.second, std::cos(P.angle)), Vector3Scale(perp(at.second), std::sin(P.angle))));
            branch(at.first, cd, len * P.lenRatio * rng.r(0.8f, 1.15f), rr * 1.4f * rng.r(0.8f, 1.f) + 0.01f, depth + 1);
        }
        if (depth > 0) addLeaves(cur, dir, P.leafSize);
    }
    void grow(Species s) {
        P = params(s, rng);
        if (!P.excurrent) {
            Vector3 top{rng.r(-0.3f, 0.3f), P.firstBranch, rng.r(-0.3f, 0.3f)};
            segs.push_back({{0, -0.3f, 0}, top, P.trunkR * 1.25f, P.trunkR, 0});
            float limbLen = (P.height - P.firstBranch) * 0.62f;
            for (int k = 0; k < P.limbs; ++k) {
                float az = 2 * PI_F * (k + rng.r(-0.3f, 0.3f)) / P.limbs;
                Vector3 d = Vector3Normalize({std::cos(az) * std::sin(P.angle), std::cos(P.angle * 0.6f), std::sin(az) * std::sin(P.angle)});
                branch(top, d, limbLen * rng.r(0.8f, 1.1f), P.trunkR * 0.7f, 1);
            }
            return;
        }
        // excurrent: trunk to the top with side branches along it
        const int tn = 10;
        Vector3 cur{0, -0.3f, 0};
        Vector3 lean{rng.r(-0.04f, 0.04f), 1, rng.r(-0.04f, 0.04f)};
        for (int i = 0; i < tn; ++i) {
            float t = float(i + 1) / tn;
            Vector3 nx = Vector3Add(cur, Vector3Scale(Vector3Normalize(Vector3Add(lean, Vector3Scale(rng.unit(), P.crook * 0.15f))), (P.height + 0.3f) / tn));
            segs.push_back({cur, nx, P.trunkR * (1.15f - 0.95f * (t - 1.f / tn)), P.trunkR * (1.15f - 0.95f * t) + 0.02f, 0});
            cur = nx;
        }
        Vector3 top = cur;
        if (s == Spruce) {
            for (float y = P.firstBranch; y < P.height - 0.4f; y += rng.r(0.32f, 0.45f)) {
                float h = y / P.height;
                float L = (1.f - h) * P.height * 0.26f + 0.35f;
                int whorl = 5 + int(rng.f() * 3);
                float az0 = rng.r(0, 2 * PI_F);
                Vector3 base{0, y, 0};
                for (int k = 0; k < whorl; ++k) {
                    float az = az0 + 2 * PI_F * k / whorl + rng.r(-0.2f, 0.2f);
                    Vector3 d = Vector3Normalize({std::cos(az), -0.12f - 0.25f * (1 - h), std::sin(az)});
                    Vector3 tip = Vector3Add(base, Vector3Scale(d, L));
                    tip.y += L * 0.12f * (h);  // tips turn up a little near the top
                    segs.push_back({base, tip, 0.05f * (1 - h) + 0.015f, 0.01f, 1});
                    // needle sprays along the branch, lying roughly flat, drooping outward
                    int sprays = std::max(1, int(L / 0.42f));
                    for (int q = 0; q < sprays; ++q) {
                        float f = 0.3f + 0.7f * (q + 0.5f) / sprays;
                        Vector3 c = Vector3Lerp(base, tip, f);
                        c.y -= 0.12f * f * L;
                        Vector3 along = Vector3Normalize(Vector3Subtract(tip, base));
                        Vector3 side = Vector3Normalize(Vector3CrossProduct(along, {0, 1, 0}));
                        float sz = P.leafSize * rng.r(0.8f, 1.15f) * (0.6f + 0.5f * (1 - h));
                        Vector3 Vd = Vector3Normalize(Vector3Add(along, {0, -0.25f, 0}));
                        Vector3 U = Vector3Scale(side, sz * 0.45f);
                        Vector3 V = Vector3Scale(Vd, sz * 0.6f);
                        cards.push_back({c, U, V, 2});
                        // a second, upright spray crossing the first gives the branch body from the side
                        Vector3 upv = Vector3Normalize(Vector3CrossProduct(side, Vd));
                        cards.push_back({Vector3Add(c, {0, 0.05f, 0}), Vector3Scale(upv, sz * 0.3f), Vector3Scale(Vd, sz * 0.55f), 2});
                    }
                }
            }
            // leader
            cards.push_back({Vector3Add(top, {0, 0.2f, 0}), {0.25f, 0, 0}, {0, 0.6f, 0}, 2});
            cards.push_back({Vector3Add(top, {0, 0.2f, 0}), {0, 0, 0.25f}, {0, 0.6f, 0}, 2});
            return;
        }
        // birch / poplar / pine: side branches between firstBranch and the top
        float y0 = P.firstBranch, y1 = P.height * 0.97f;
        int nb = s == Pine ? 7 : s == Poplar ? 16 : 11;
        for (int k = 0; k < nb; ++k) {
            float y = y0 + (y1 - y0) * (k + rng.r(0.f, 0.8f)) / nb;
            float h = (y - y0) / std::max(0.1f, y1 - y0);
            float L;
            if (s == Poplar) L = (0.25f + 0.75f * std::sin(PI_F * std::min(1.f, h * 0.9f + 0.1f))) * 2.6f;
            else if (s == Pine) L = (1.f - h * 0.55f) * 4.2f;
            else L = (0.4f + 0.6f * std::sin(PI_F * (0.15f + h * 0.8f))) * 4.2f;
            float az = rng.r(0, 2 * PI_F);
            Vector3 d = Vector3Normalize({std::cos(az) * std::sin(P.angle), std::cos(P.angle), std::sin(az) * std::sin(P.angle)});
            branch({0, y, 0}, d, L, P.trunkR * 0.45f * (1.f - h * 0.6f) + 0.02f, 1);
        }
        addLeaves(top, {0, 1, 0}, P.leafSize);
    }
};

void tube(MeshBuilder& b, const Seg& s, int sides, unsigned char bark, float& vAcc) {
    Vector3 d = Vector3Subtract(s.b, s.a);
    float len = Vector3Length(d);
    if (len < 1e-4f) return;
    d = Vector3Scale(d, 1.f / len);
    Vector3 ref = std::fabs(d.y) < 0.95f ? Vector3{0, 1, 0} : Vector3{1, 0, 0};
    Vector3 u = Vector3Normalize(Vector3CrossProduct(d, ref));
    Vector3 w = Vector3CrossProduct(d, u);
    float slope = (s.ra - s.rb) / len;
    unsigned short base = (unsigned short)(b.v.size() / 3);
    for (int end = 0; end < 2; ++end) {
        Vector3 c = end ? s.b : s.a;
        float r = end ? s.rb : s.ra;
        float v = vAcc + (end ? len : 0.f);
        for (int j = 0; j <= sides; ++j) {
            float a = 2 * PI_F * j / sides;
            Vector3 rad = Vector3Add(Vector3Scale(u, std::cos(a)), Vector3Scale(w, std::sin(a)));
            Vector3 nn = Vector3Normalize(Vector3Add(rad, Vector3Scale(d, slope)));
            unsigned char ao = (unsigned char)std::clamp(170.f + 85.f * std::min(1.f, c.y / 6.f), 0.f, 255.f);
            b.add(Vector3Add(c, Vector3Scale(rad, r)), nn, {float(j) / sides, v}, Color{128, ao, bark, 255});
        }
    }
    vAcc += len;
    for (int j = 0; j < sides; ++j) {
        unsigned short a0 = base + j, a1 = base + j + 1, b0 = base + sides + 1 + j, b1 = base + sides + 2 + j;
        b.tri(a0, b0, a1);
        b.tri(a1, b0, b1);
    }
}

Mesh assemble(const Tree& t, bool lo, unsigned char bark, uint32_t seed) {
    MeshBuilder b;
    float v = 0;
    for (auto& s : t.segs) {
        if (lo && s.depth > 1) continue;
        if (!lo && s.rb < 0.004f && s.depth > 2) continue;
        int sides = lo ? (s.depth == 0 ? 6 : 4) : (s.depth == 0 ? 10 : s.depth == 1 ? 7 : 5);
        tube(b, s, sides, bark, v);
    }
    // crown centre & size for soft "spherical" foliage normals and ambient occlusion
    Vector3 cc{0, 0, 0};
    for (auto& c : t.cards) cc = Vector3Add(cc, c.c);
    if (!t.cards.empty()) cc = Vector3Scale(cc, 1.f / t.cards.size());
    float cr = 0.1f;
    for (auto& c : t.cards) cr = std::max(cr, Vector3Distance(c.c, cc));
    Rng rng(seed);
    size_t step = lo ? 5 : 1;
    float grow = lo ? 2.1f : 1.f;
    for (size_t i = 0; i < t.cards.size(); i += step) {
        const Card& c = t.cards[i];
        Vector3 U = Vector3Scale(c.U, grow), V = Vector3Scale(c.V, grow);
        Vector3 radial = Vector3Subtract(c.c, cc);
        float rl = Vector3Length(radial);
        radial = rl > 1e-3f ? Vector3Scale(radial, 1.f / rl) : Vector3{0, 1, 0};
        Vector3 cn = Vector3Normalize(Vector3CrossProduct(U, V));
        if (Vector3DotProduct(cn, radial) < 0) cn = Vector3Negate(cn);
        Vector3 nn = Vector3Normalize(Vector3Add(Vector3Scale(radial, 0.75f), Vector3Scale(cn, 0.25f)));
        if (c.cell == 2) nn = Vector3Normalize(Vector3Add(nn, {0, 0.6f, 0}));  // sprays face the sky
        float ao = std::clamp(0.3f + 0.75f * rl / cr, 0.3f, 1.f);
        unsigned char hue = (unsigned char)(rng.f() * 255);
        float u0 = (c.cell % 2) * 0.5f, v0 = (c.cell / 2) * 0.5f;
        auto uv = [&](float su, float sv) { return Vector2{u0 + 0.004f + (su * 0.5f + 0.5f) * 0.492f, v0 + 0.004f + (sv * 0.5f + 0.5f) * 0.492f}; };
        Color col{0, (unsigned char)(ao * 255), hue, 255};
        unsigned short a = b.add(Vector3Subtract(Vector3Subtract(c.c, U), V), nn, uv(-1, 1), col);
        unsigned short bb = b.add(Vector3Subtract(Vector3Add(c.c, U), V), nn, uv(1, 1), col);
        unsigned short cc2 = b.add(Vector3Add(Vector3Add(c.c, U), V), nn, uv(1, -1), col);
        unsigned short d = b.add(Vector3Add(Vector3Subtract(c.c, U), V), nn, uv(-1, -1), col);
        b.tri(a, bb, cc2);
        b.tri(a, cc2, d);
        if (b.v.size() / 3 > 64000) break;
    }
    return b.build(true);
}

}  // namespace

Texture2D leafAtlas(int quality) {
    int C = quality >= 1 ? 512 : 256;
    Canvas c(2 * C);
    paintBroad(c, 0, 0, C, 11);
    paintSmall(c, C, 0, C, 12);
    paintSpray(c, 0, C, C, 13);
    paintTufts(c, C, C, C, 14);
    // bleed colour into transparent texels so mip-maps don't pick up black fringes
    for (int pass = 0; pass < 6; ++pass) {
        Canvas d = c;
        for (int y = 1; y < c.n - 1; ++y)
            for (int x = 1; x < c.n - 1; ++x) {
                int i = y * c.n + x;
                if (c.a[i] > 0.01f) continue;
                float sr = 0, sg = 0, sb = 0, n = 0;
                for (int k : {i - 1, i + 1, i - c.n, i + c.n})
                    if (c.a[k] > 0.01f || (c.r[k] + c.g[k] + c.b[k]) > 0) { sr += c.r[k]; sg += c.g[k]; sb += c.b[k]; n += 1; }
                if (n > 0) { d.r[i] = sr / n; d.g[i] = sg / n; d.b[i] = sb / n; }
            }
        for (int i = 0; i < c.n * c.n; ++i) if (c.a[i] <= 0.01f) { c.r[i] = d.r[i]; c.g[i] = d.g[i]; c.b[i] = d.b[i]; }
    }
    std::vector<unsigned char> px(size_t(c.n) * c.n * 4);
    for (int i = 0; i < c.n * c.n; ++i) {
        px[i * 4 + 0] = (unsigned char)std::clamp(c.r[i] * 255.f, 0.f, 255.f);
        px[i * 4 + 1] = (unsigned char)std::clamp(c.g[i] * 255.f, 0.f, 255.f);
        px[i * 4 + 2] = (unsigned char)std::clamp(c.b[i] * 255.f, 0.f, 255.f);
        px[i * 4 + 3] = (unsigned char)std::clamp(c.a[i] * 255.f, 0.f, 255.f);
    }
    Image im{px.data(), c.n, c.n, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    Texture2D t = LoadTextureFromImage(im);
    GenTextureMipmaps(&t);
    SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
    SetTextureWrap(t, TEXTURE_WRAP_CLAMP);
    return t;
}

Built build(int type, int quality) {
    (void)quality;
    Species s = Species(type / VARIANTS);
    uint32_t seed = 1000u + uint32_t(type) * 7919u;
    Tree t(seed);
    t.grow(s);
    Built out;
    out.height = t.P.height;
    out.hi = assemble(t, false, t.P.bark, seed + 1);
    out.lo = assemble(t, true, t.P.bark, seed + 2);
    return out;
}

int pickType(float alt, float forest, float r1, float r2) {
    // valley: broadleaves; higher up conifers take over
    float w[SPECIES] = {1.0f, 0.8f, 0.7f, 0.25f, 0.3f, 0.35f};
    w[Oak] *= 1.f - alt * 0.8f;
    w[Beech] *= (1.f - alt * 0.6f) * (0.5f + forest);
    w[Poplar] *= 1.f - alt;
    w[Birch] *= 0.6f + alt * 0.6f;
    w[Spruce] *= 0.4f + alt * 3.f;
    w[Pine] *= 0.6f + alt * 1.6f;
    float sum = 0;
    for (float x : w) sum += x;
    float pick = r1 * sum;
    int sp = 0;
    for (; sp < SPECIES - 1; ++sp) { if (pick < w[sp]) break; pick -= w[sp]; }
    return sp * VARIANTS + (r2 < 0.5f ? 0 : 1);
}

bool isConifer(int type) { int s = type / VARIANTS; return s == Spruce || s == Pine; }

}  // namespace trees
