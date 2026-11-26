#include "Trees.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "EzTree.h"
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

// ---------------------------------------------------------------- Zwift-style leaf clusters
// Painted, not photographic: each atlas tile is one clump of a few broad, soft-edged leaves
// fanning up from the bottom centre (where the card attaches), shaded dark at the heart of
// the clump to bright, warm tips. No midribs, no noise.
struct Pal { float dr, dg, db, lr, lg, lb; };

// a soft leaf: pointed oval from (x,y) along `ang`; gradient from base colour to tip colour
void softLeaf(Canvas& c, float x, float y, float ang, float len, float wid, const Pal& p, float shade, float roundness) {
    float ca = std::cos(ang), sa = std::sin(ang);
    int R = int(len + wid) + 2;
    for (int oy = -R; oy <= R; ++oy)
        for (int ox = -R; ox <= R; ++ox) {
            float lu = (ox * ca + oy * sa) / len, lv = (-ox * sa + oy * ca);
            if (lu < 0 || lu > 1) continue;
            float half = wid * std::pow(std::sin(3.14159f * std::pow(lu, 0.8f)), roundness);
            float e = half - std::fabs(lv);
            if (e <= -0.5f) continue;
            float al = std::clamp(e + 0.5f, 0.f, 1.f);
            float t = std::clamp(lu * 0.9f + 0.1f + (lv / std::max(wid, 1.f)) * 0.12f, 0.f, 1.f);   // a little light from one side
            float k = shade;
            c.put(int(x) + ox, int(y) + oy, (p.dr + (p.lr - p.dr) * t) * k, (p.dg + (p.lg - p.dg) * t) * k, (p.db + (p.lb - p.db) * t) * k, al);
        }
}


// Zwift-style foliage mass: a rounded clump (a few overlapping lobes, wider than tall) packed
// with many small leaves, so the silhouette is a ragged leafy edge. Each leaf is shaded by
// where it sits in the clump: dark and cool in the heart and underneath, bright and warm on
// the upper rim, the way a painted canopy reads in sunlight. Drawn back to front.
void paintMass(Canvas& c, int x0, int y0, int C, uint32_t seed, const Pal& p, int count, float leafLen, float leafWid, float roundness) {
    Rng rng(seed);
    struct Lobe { float x, y, r; };
    std::vector<Lobe> lobes;
    const float cx = C * 0.5f, cy = C * 0.54f;
    lobes.push_back({cx, cy, C * 0.27f});
    for (int i = 0; i < 4; ++i) {
        float a = 0.3f + i * 0.85f + rng.r(-0.3f, 0.3f);
        float d = C * rng.r(0.12f, 0.17f);
        lobes.push_back({cx + std::cos(a) * d * 1.25f, cy + std::sin(a) * d * 0.8f, C * rng.r(0.15f, 0.2f)});
    }
    auto inside = [&](float x, float y) {   // 0 at the edge, 1 deep inside
        float best = -1;
        for (auto& l : lobes) best = std::max(best, 1.f - std::hypot(x - l.x, y - l.y) / l.r);
        return best;
    };
    struct L { float x, y, ang, len, wid, depth, height; };
    std::vector<L> leaves;
    for (int tries = 0; tries < count * 6 && int(leaves.size()) < count; ++tries) {
        float x = rng.r(C * 0.08f, C * 0.92f), y = rng.r(C * 0.12f, C * 0.95f);
        float in = inside(x, y);
        if (in < -0.05f) continue;
        // leaves point outwards from the clump's centre, so the rim is made of leaf tips
        float ang = std::atan2(y - cy, x - cx) + rng.r(-0.7f, 0.7f);
        float len = C * leafLen * rng.r(0.75f, 1.15f), wid = len * leafWid * rng.r(0.8f, 1.2f);
        leaves.push_back({x, y, ang, len, wid, in, (y - C * 0.2f) / (C * 0.75f)});
    }
    // back (deep, low) first, rim and top last
    std::sort(leaves.begin(), leaves.end(), [](const L& a, const L& b) { return a.depth - a.height * 0.5f > b.depth - b.height * 0.5f; });
    for (auto& l : leaves) {
        float lit = std::clamp(0.4f + 0.5f * l.height + 0.3f * (0.3f - l.depth), 0.3f, 1.f);   // top and rim catch the light
        Pal q{p.dr * (0.75f + 0.4f * lit), p.dg * (0.75f + 0.4f * lit), p.db * (0.8f + 0.3f * lit),
              p.dr + (p.lr - p.dr) * lit, p.dg + (p.lg - p.dg) * lit, p.db + (p.lb - p.db) * lit};
        softLeaf(c, l.x, l.y, l.ang, l.len, l.wid, q, 1.f, roundness);
    }
}

void paintNeedles(Canvas& c, int x0, int y0, int C, uint32_t seed) {
    Rng rng(seed);
    float bx = x0 + C * 0.5f, by = y0 + C * 0.04f;
    // a stem up the middle with soft layered needle fans either side
    for (int layer = 0; layer < 3; ++layer) {
        float sh = 0.7f + 0.18f * layer;
        for (int k = 0; k < 26; ++k) {
            float t = rng.f();
            float sx = bx + (rng.f() - 0.5f) * C * 0.06f, sy = by + t * C * 0.86f;
            float side = rng.f() < 0.5f ? -1.f : 1.f;
            float ang = 1.5708f - side * rng.r(0.5f, 1.15f);
            float len = C * (0.18f + 0.26f * (1.f - t)) * rng.r(0.8f, 1.1f);
            for (int n = 0; n < 7; ++n) {
                float a = ang + (n - 3) * 0.07f;
                float ex = sx + std::cos(a) * len, ey = sy + std::sin(a) * len;
                float g = (0.17f + 0.1f * n / 6.f) * sh;
                c.line(sx, sy, ex, ey, 2.2f * C / 256.f, 1.f * C / 256.f, g * 0.4f, g * 1.0f + 0.03f, g * 0.5f);
            }
        }
    }
    c.line(bx, by, bx, by + C * 0.9f, 3.f * C / 256.f, 1.f, 0.28f, 0.2f, 0.12f);
}

// ---------------------------------------------------------------- species (ez-tree presets)
struct Species_ { const char* presets[VARIANTS]; float hMin, hMax; int tile; unsigned char hue, bark; bool conifer; };
const Species_ SPECIES_DEF[SPECIES] = {
    {{"oak_medium", "oak_large"}, 11.f, 16.f, 0, 90, 20, false},     // Oak
    {{"ash_medium", "ash_large"}, 13.f, 19.f, 1, 128, 30, false},    // "Beech": ash
    {{"aspen_medium", "aspen_large"}, 12.f, 18.f, 3, 190, 100, false},  // "Birch": aspen (white bark)
    {{"aspen_large", "ash_small"}, 16.f, 22.f, 1, 150, 40, false},   // "Poplar": tall, narrow
    {{"pine_large", "pine_medium"}, 17.f, 24.f, 2, 20, 235, true},   // "Spruce"
    {{"pine_medium", "pine_small"}, 12.f, 18.f, 2, 60, 180, true},   // "Pine"
};
const char* BUSH_PRESETS[BUSHES] = {"bush_1", "bush_2", "bush_3"};

uint32_t g_seed = 1;   // the session's generation seed (set by build / buildBush callers)

// detail levels for our three-level scheme (full mesh / simplified / impostor beyond): fewer,
// bigger leaf clumps than ez-tree's defaults (the Zwift look, and a tenth of the triangles)
eztree::Detail detailFor(bool lo, bool conifer, bool bush) {
    eztree::Detail d;
    if (!lo) { d.sectionStride = 2; d.segmentFactor = 0.7f; d.leafStride = conifer ? 3 : 4; d.leafScale = conifer ? 1.5f : 2.5f; }
    else { d.sectionStride = 6; d.segmentFactor = 0.4f; d.leafStride = bush ? 5 : 7; d.leafScale = bush ? 2.6f : 3.4f; d.billboard = 0; }
    return d;
}

Mesh toMesh(const eztree::Options& o, const eztree::Skeleton& sk, const eztree::Detail& d, float scale, int tile, unsigned char hue,
            unsigned char bark, bool conifer, int maxBranchLevel, float* heightOut) {
    eztree::Geometry g = eztree::mesh(o, sk, d);
    MeshBuilder b;
    float H = 0;
    for (auto& v : g.branchVerts) H = std::max(H, v.p.y * scale);
    for (auto& v : g.leafVerts) H = std::max(H, v.p.y * scale);
    // canopy centre / extent for soft, rounded crown shading
    double cx = 0, cy = 0, cz = 0;
    for (auto& o2 : g.leafOrigins) { cx += o2.x; cy += o2.y; cz += o2.z; }
    size_t nL = std::max<size_t>(1, g.leafOrigins.size());
    Vector3 cc{float(cx / nL) * scale, float(cy / nL) * scale, float(cz / nL) * scale};
    float R = 0.1f;
    for (auto& o2 : g.leafOrigins) R = std::max(R, Vector3Distance({o2.x * scale, o2.y * scale, o2.z * scale}, cc));
    // branches (skip the finest twigs on the simplified level: the leaves hide them)
    std::vector<int> remap(g.branchVerts.size(), -1);
    for (size_t i = 0; i < g.branchIdx.size(); i += 3) {
        bool ok = true;
        for (int k = 0; k < 3; ++k) ok &= g.branchVerts[g.branchIdx[i + k]].branchLevel <= maxBranchLevel;
        if (!ok) continue;
        unsigned short id[3];
        for (int k = 0; k < 3; ++k) {
            uint32_t vi = g.branchIdx[i + k];
            if (remap[vi] < 0) {
                const auto& v = g.branchVerts[vi];
                Vector3 p{v.p.x * scale, v.p.y * scale, v.p.z * scale};
                unsigned char ao = (unsigned char)std::clamp(120.f + 135.f * std::min(1.f, p.y / std::max(1.f, H * 0.45f)), 0.f, 255.f);
                remap[vi] = b.add(p, {v.n.x, v.n.y, v.n.z}, {v.u, v.v * scale}, Color{128, ao, bark, 255});
            }
            id[k] = (unsigned short)remap[vi];
        }
        b.tri(id[0], id[1], id[2]);
    }
    // leaves
    const float u0 = (tile % 2) * 0.5f, v0 = (tile / 2) * 0.5f;
    for (size_t i = 0; i < g.leafVerts.size(); i += 4) {
        unsigned short id[4];
        for (int k = 0; k < 4; ++k) {
            const auto& v = g.leafVerts[i + k];
            Vector3 p{v.p.x * scale, v.p.y * scale, v.p.z * scale};
            // ez-tree's rounded normal, pulled towards "out of the crown" so the canopy shades as
            // one soft volume (conifers: out from the trunk, a cone)
            Vector3 out = conifer ? Vector3{p.x, (p.y - H * 0.55f) * 0.35f + R * 0.25f, p.z} : Vector3Subtract(p, cc);
            out.y += R * 0.3f;
            Vector3 n = Vector3Normalize(Vector3Lerp(Vector3Normalize({v.n.x, v.n.y, v.n.z}), Vector3Normalize(out), 0.7f));
            // ambient occlusion: dark in the heart of the crown, bright at its skin and top
            const auto& lo = g.leafOrigins[i + k];
            Vector3 lp{lo.x * scale, lo.y * scale, lo.z * scale};
            float rr = conifer ? std::hypot(lp.x, lp.z) / std::max(0.5f, R * 0.7f) : Vector3Distance(lp, cc) / R;
            float ao = std::clamp(0.38f + 0.62f * std::pow(std::clamp(rr, 0.f, 1.f), 0.8f) + 0.15f * (lp.y / H - 0.5f), 0.f, 1.f);
            id[k] = b.add(p, n, {u0 + 0.008f + v.u * 0.484f, v0 + 0.008f + v.v * 0.484f}, Color{0, (unsigned char)(ao * 255), hue, 255});
        }
        b.tri(id[0], id[1], id[2]);
        b.tri(id[0], id[2], id[3]);
    }
    if (b.v.size() / 3 > 65000) std::fprintf(stderr, "[trees] %s: %zu vertices (16-bit index overflow)\n", o.name, b.v.size() / 3);
    if (heightOut) *heightOut = H;
    return b.build(true);
}

Built grown(const eztree::Options& base, uint32_t seed, float targetH, int tile, unsigned char hue, unsigned char bark, bool conifer, bool bush) {
    eztree::Options o = base;
    o.seed = seed;
    eztree::Skeleton sk = eztree::grow(o);
    float H0 = 0;
    for (auto& br : sk.branches) for (auto& s : br.sections) H0 = std::max(H0, s.origin.y);
    for (auto& l : sk.leaves) H0 = std::max(H0, l.origin.y + l.size);
    float scale = targetH / std::max(1.f, H0);
    Built out;
    out.hi = toMesh(o, sk, detailFor(false, conifer, bush), scale, tile, hue, bark, conifer, 9, &out.height);
    out.lo = toMesh(o, sk, detailFor(true, conifer, bush), scale, tile, hue, bark, conifer, std::max(1, o.levels - 1), nullptr);
    if (!bush) {
        eztree::Detail sd;
        sd.sectionStride = 8; sd.segmentFactor = 0.3f; sd.leafStride = 12; sd.leafScale = 3.2f; sd.billboard = 0;
        out.shadow = toMesh(o, sk, sd, scale, tile, hue, bark, conifer, 1, nullptr);
    }
    return out;
}

// ---------------------------------------------------------------- rocks
// Chunky, faceted boulders: a subdivided icosahedron pushed about by a few broad lumps, cut by
// a couple of planes (flat faces), the bottom flattened into the ground. Flat-shaded: every
// facet has its own normal, which is what gives stylised rocks their look.
Mesh makeRock(int variant) {
    Rng rng(9001 + variant * 77);
    std::vector<Vector3> V = {{-1, 1.618f, 0}, {1, 1.618f, 0}, {-1, -1.618f, 0}, {1, -1.618f, 0}, {0, -1, 1.618f}, {0, 1, 1.618f},
                              {0, -1, -1.618f}, {0, 1, -1.618f}, {1.618f, 0, -1}, {1.618f, 0, 1}, {-1.618f, 0, -1}, {-1.618f, 0, 1}};
    std::vector<int> F = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
                          3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};
    for (auto& v : V) v = Vector3Normalize(v);
    for (int s = 0; s < 1; ++s) {   // subdivide once: big, readable facets
        std::vector<int> nf;
        std::vector<std::pair<long long, int>> cache;
        auto mid = [&](int a, int b) {
            long long k = (long long)std::min(a, b) << 32 | std::max(a, b);
            for (auto& [kk, id] : cache) if (kk == k) return id;
            V.push_back(Vector3Normalize(Vector3Lerp(V[a], V[b], 0.5f)));
            cache.push_back({k, int(V.size()) - 1});
            return int(V.size()) - 1;
        };
        for (size_t i = 0; i < F.size(); i += 3) {
            int a = F[i], b = F[i + 1], c = F[i + 2], ab = mid(a, b), bc = mid(b, c), ca = mid(c, a);
            nf.insert(nf.end(), {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca});
        }
        F = nf;
    }
    // lumps
    Vector3 lump[5]; float amp[5];
    for (int i = 0; i < 5; ++i) { lump[i] = rng.unit(); amp[i] = rng.r(-0.22f, 0.3f); }
    // cut planes
    Vector3 cutN[3]; float cutD[3];
    for (int i = 0; i < 3; ++i) { cutN[i] = rng.unit(); cutN[i].y = std::fabs(cutN[i].y) * 0.6f + (i == 0 ? 0.5f : 0.f); cutN[i] = Vector3Normalize(cutN[i]); cutD[i] = rng.r(0.72f, 0.9f); }
    float sx = rng.r(0.95f, 1.3f), sz = rng.r(0.85f, 1.15f), sy = rng.r(0.7f, 0.95f);
    for (auto& v : V) {
        float r = 1.f;
        for (int i = 0; i < 5; ++i) r += amp[i] * std::pow(std::max(0.f, Vector3DotProduct(v, lump[i])), 3.f);
        Vector3 p = Vector3Scale(v, r);
        for (int i = 0; i < 3; ++i) {
            float d = Vector3DotProduct(p, cutN[i]);
            if (d > cutD[i]) p = Vector3Subtract(p, Vector3Scale(cutN[i], d - cutD[i]));
        }
        p = {p.x * sx, p.y * sy, p.z * sz};
        if (p.y < -0.15f) p.y = -0.15f + (p.y + 0.15f) * 0.25f;   // sits flat in the ground
        v = p;
    }
    MeshBuilder b;
    for (size_t i = 0; i < F.size(); i += 3) {
        Vector3 a = V[F[i]], c1 = V[F[i + 1]], c2 = V[F[i + 2]];
        Vector3 n = Vector3Normalize(Vector3CrossProduct(Vector3Subtract(c1, a), Vector3Subtract(c2, a)));
        if (Vector3DotProduct(n, Vector3Add(Vector3Add(a, c1), c2)) < 0) { std::swap(c1, c2); n = Vector3Negate(n); }
        for (Vector3 p : {a, c1, c2}) {
            unsigned char ao = (unsigned char)std::clamp(150.f + 120.f * (p.y + 0.15f), 0.f, 255.f);   // darker where it meets the ground
            b.add(p, n, {p.x + p.z, p.y}, Color{255, ao, 0, 255});
        }
        unsigned short k = (unsigned short)(b.v.size() / 3 - 3);
        b.tri(k, k + 1, k + 2);
    }
    return b.build(true);
}

}  // namespace

void setSeed(uint32_t seed) { g_seed = seed ? seed : 1; }

Texture2D leafAtlas(int quality) {
    int C = quality >= 1 ? 512 : 256;
    Canvas c(2 * C);
    // tile 0: broad, rounded leaves (oak, bushes) · 1: slimmer leaflets (ash, poplar)
    // tile 2: needles (pines) · 3: small round coins (aspen)
    // deep, rich greens: dark olive in the shade to a sunny yellow-green on the lit rim
    auto at = [&](int tx, int ty, auto fn) { Canvas t(C); fn(t); for (int y = 0; y < C; ++y) for (int x = 0; x < C; ++x) { int i = y * C + x, o = (ty * C + y) * c.n + tx * C + x; c.r[o] = t.r[i]; c.g[o] = t.g[i]; c.b[o] = t.b[i]; c.a[o] = t.a[i]; } };
    at(0, 0, [&](Canvas& t) { paintMass(t, 0, 0, C, 11, {0.07f, 0.17f, 0.04f, 0.36f, 0.55f, 0.13f}, 150, 0.085f, 0.5f, 0.6f); });   // broadleaf (oak, bushes)
    at(1, 0, [&](Canvas& t) { paintMass(t, 0, 0, C, 12, {0.07f, 0.18f, 0.05f, 0.33f, 0.53f, 0.14f}, 190, 0.08f, 0.3f, 0.8f); });   // slim leaflets (ash, poplar)
    paintNeedles(c, 0, C, C, 13);
    at(1, 1, [&](Canvas& t) { paintMass(t, 0, 0, C, 14, {0.09f, 0.2f, 0.04f, 0.42f, 0.6f, 0.15f}, 210, 0.055f, 0.7f, 0.45f); });   // small round (aspen)
    // bleed colour into transparent texels so mip-maps don't pick up black fringes
    for (int pass = 0; pass < 8; ++pass) {
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
    const Species_& sp = SPECIES_DEF[type / VARIANTS];
    const int var = type % VARIANTS;
    const eztree::Options* o = eztree::preset(sp.presets[var]);
    uint32_t seed = g_seed * 2654435761u + uint32_t(type) * 97u + 13u;
    Rng rng(seed);
    float h = rng.r(sp.hMin, sp.hMax);
    return grown(*o, seed % 65536u, h, sp.tile, sp.hue, sp.bark, sp.conifer, false);
}

Built buildBush(int i) {
    const eztree::Options* o = eztree::preset(BUSH_PRESETS[i % BUSHES]);
    uint32_t seed = g_seed * 2246822519u + uint32_t(i) * 131u + 7u;
    Rng rng(seed);
    float h = rng.r(1.2f, 2.2f);
    return grown(*o, seed % 65536u, h, 0, 110, 25, false, true);
}

Mesh buildRock(int i) { return makeRock(i); }

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

int pickTypeWorld(float alt, float conifer, float r1, float r2) {
    int sp;
    if (r1 < conifer) sp = r1 / std::max(conifer, 1e-3f) < 0.55f - 0.2f * alt ? Pine : Spruce;
    else {
        float u = (r1 - conifer) / std::max(1e-3f, 1.f - conifer);
        float w[4] = {1.0f * (1.f - alt * 0.8f), 0.8f * (1.f - alt * 0.6f), 0.7f * (0.6f + alt * 0.6f), 0.25f * (1.f - alt)};
        int ids[4] = {Oak, Beech, Birch, Poplar};
        float sum = w[0] + w[1] + w[2] + w[3], pick = u * sum;
        int q = 0;
        for (; q < 3; ++q) { if (pick < w[q]) break; pick -= w[q]; }
        sp = ids[q];
    }
    return sp * VARIANTS + (r2 < 0.5f ? 0 : 1);
}

bool isConifer(int type) { int s = type / VARIANTS; return s == Spruce || s == Pine; }

}  // namespace trees
