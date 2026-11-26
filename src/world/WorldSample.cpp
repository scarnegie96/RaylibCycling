// World queries: smooth height, biome weights, the full sample with fine detail (channels,
// road formation, terraces, micro relief), the road network, the binary cache.
#include <cstdio>
#include <cstring>
#include <fstream>

#include "world/WorldInternal.h"
#include "world/WorldMap.h"

namespace wmap {
using namespace detail;

namespace {
constexpr float HW = 3.6f;
constexpr float SHORE = 0.3f;
inline float cr(float p0, float p1, float p2, float p3, float t) {
    return p1 + 0.5f * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
}
}  // namespace

bool Map::inside(float x, float z, float m) const {
    return x >= x0 + m && z >= z0 + m && x <= x0 + (n - 1) * cell - m && z <= z0 + (n - 1) * cell - m;
}

float Map::height(float x, float z) const {
    float fx = std::clamp((x - x0) / cell, 0.f, float(n - 1) - 1e-3f), fz = std::clamp((z - z0) / cell, 0.f, float(n - 1) - 1e-3f);
    int i = int(fx), k = int(fz);
    float tx = fx - i, tz = fz - k;
    auto G = [&](int a, int b) { return h[size_t(std::clamp(b, 0, n - 1)) * n + std::clamp(a, 0, n - 1)]; };
    float r[4];
    for (int q = 0; q < 4; ++q) {
        int b = k - 1 + q;
        r[q] = cr(G(i - 1, b), G(i, b), G(i + 1, b), G(i + 2, b), tx);
    }
    float v = cr(r[0], r[1], r[2], r[3], tz);
    // Catmull-Rom overshoots at sharp steps: keep within the local cell's range (+ a little)
    float lo = std::min({G(i, k), G(i + 1, k), G(i, k + 1), G(i + 1, k + 1)}), hi = std::max({G(i, k), G(i + 1, k), G(i, k + 1), G(i + 1, k + 1)});
    return std::clamp(v, lo - 0.5f, hi + 0.5f);
}

void Map::biomeAt(float x, float z, float w[BIOMES]) const {
    float fx = std::clamp((x - x0) / ccell, 0.f, float(cn - 1) - 1e-3f), fz = std::clamp((z - z0) / ccell, 0.f, float(cn - 1) - 1e-3f);
    int i = int(fx), k = int(fz);
    float tx = fx - i, tz = fz - k;
    const auto& a = biome[size_t(k) * cn + i];
    const auto& b = biome[size_t(k) * cn + i + 1];
    const auto& c = biome[size_t(k + 1) * cn + i];
    const auto& d = biome[size_t(k + 1) * cn + i + 1];
    for (int q = 0; q < BIOMES; ++q) w[q] = lerpf(lerpf(a[q], b[q], tx), lerpf(c[q], d[q], tx), tz);
}

float Map::roadNearest(float x, float z, float& px, float& pz, float& py, int* cls) const {
    int gx = int(std::floor(x / HASH)), gz = int(std::floor(z / HASH));
    float best = 1e30f;
    for (int dz = -2; dz <= 2; ++dz)
        for (int dx = -2; dx <= 2; ++dx) {
            auto it = roadCells.find(ckey(gx + dx, gz + dz));
            if (it == roadCells.end()) continue;
            for (const Ref& r : it->second) {
                const Road& rd = roads[r.a];
                const RoadPt& a = rd.pts[r.b];
                const RoadPt& b = rd.pts[r.b + 1];
                float t;
                float d = segDist(x, z, a.x, a.z, b.x, b.z, t);
                if (d < best) {
                    best = d;
                    px = lerpf(a.x, b.x, t); pz = lerpf(a.z, b.z, t); py = lerpf(a.y, b.y, t);
                    if (cls) *cls = rd.cls;
                }
            }
        }
    return best > 1e29f ? 1e4f : best;
}

std::vector<Bridge> Map::bridgesNear(float x, float z, float r) const {
    std::vector<Bridge> out;
    int c = int(std::ceil(r / HASH)) + 1;
    int gx = int(std::floor(x / HASH)), gz = int(std::floor(z / HASH));
    for (int dz = -c; dz <= c; ++dz)
        for (int dx = -c; dx <= c; ++dx) {
            auto it = bridgeCells.find(ckey(gx + dx, gz + dz));
            if (it == bridgeCells.end()) continue;
            for (int b : it->second)
                if (std::hypot(bridges[b].x - x, bridges[b].z - z) <= r) out.push_back(bridges[b]);
        }
    return out;
}

void Map::index() {
    riverCells.clear();
    roadCells.clear();
    bridgeCells.clear();
    for (size_t r = 0; r < rivers.size(); ++r)
        for (size_t j = 0; j + 1 < rivers[r].pts.size(); ++j) {
            const RiverPt& a = rivers[r].pts[j];
            const RiverPt& b = rivers[r].pts[j + 1];
            riverCells[ckey(int(std::floor((a.x + b.x) * 0.5f / HASH)), int(std::floor((a.z + b.z) * 0.5f / HASH)))].push_back({int(r), int(j)});
        }
    for (size_t r = 0; r < roads.size(); ++r)
        for (size_t j = 0; j + 1 < roads[r].pts.size(); ++j) {
            const RoadPt& a = roads[r].pts[j];
            const RoadPt& b = roads[r].pts[j + 1];
            roadCells[ckey(int(std::floor((a.x + b.x) * 0.5f / HASH)), int(std::floor((a.z + b.z) * 0.5f / HASH)))].push_back({int(r), int(j)});
        }
    for (size_t b = 0; b < bridges.size(); ++b)
        bridgeCells[ckey(int(std::floor(bridges[b].x / HASH)), int(std::floor(bridges[b].z / HASH)))].push_back(int(b));
}

Sample Map::sample(float x, float z) const {
    Sample s;
    const uint32_t seed = params.seed * 747796405u + 2891336453u;
    float base = height(x, z);
    // biome weights (warped like the landforms, so detail agrees with the shapes)
    {
        float wx = x + 260.f * fbm(x / 900.f, z / 900.f, 3, seed + 61), wz = z + 260.f * fbm(x / 900.f + 7.f, z / 900.f, 3, seed + 62);
        biomeAt(wx, wz, s.w);
    }
    // grid fields
    {
        float fx = std::clamp((x - x0) / cell, 0.f, float(n - 1) - 1e-3f), fz = std::clamp((z - z0) / cell, 0.f, float(n - 1) - 1e-3f);
        int i = int(fx), k = int(fz);
        float tx = fx - i, tz = fz - k;
        auto bl = [&](const std::vector<float>& g) {
            size_t a = size_t(k) * n + i;
            return lerpf(lerpf(g[a], g[a + 1], tx), lerpf(g[a + n], g[a + n + 1], tx), tz);
        };
        s.moist = bl(moist);
        s.sand = bl(sand);
        s.temp = bl(temp);
        // lakes, ponds, salt flats around this point
        float lvl = -1e9f;
        bool playa = false;
        for (int dk = -1; dk <= 2; ++dk)
            for (int di = -1; di <= 2; ++di) {
                int a = std::clamp(i + di, 0, n - 1), b = std::clamp(k + dk, 0, n - 1);
                int l = lakeId[size_t(b) * n + a];
                if (l < 0) continue;
                if (lakes[l].playa) { if (di >= 0 && di <= 1 && dk >= 0 && dk <= 1) playa = true; continue; }
                lvl = std::max(lvl, lakes[l].level);
            }
        if (lvl > -1e8f) {
            if (base < lvl + 0.05f) { s.water = lvl; s.waterW = 1e3f; }
            s.bank = 1.f - sstep(0.f, 2.5f, base - lvl);
            s.bankLevel = lvl;
            s.wet = std::max(s.wet, s.bank * 0.6f);
        }
        if (playa) { s.sediment = 1.f; s.sand = std::max(s.sand, 0.5f); }
    }
    // micro relief by biome: hummocks, rocks breaking the surface, dune ripples...
    float rough = 0.6f * s.w[Meadow] + 0.9f * s.w[Forest] + 1.3f * s.w[Boreal] + 2.2f * s.w[Alpine] + 0.4f * s.w[Steppe] + 0.5f * s.w[Savanna] +
                  1.6f * s.w[Scrub] + 0.9f * s.w[Dunes] + 1.4f * s.w[Badlands] + 0.15f * s.w[Wetland];
    float detail = rough * (fbm(x / 45.f, z / 45.f, 3, seed + 201) + 0.35f * fbm(x / 14.f, z / 14.f, 2, seed + 202));
    // mesa country: sharpen the grid's smooth slopes into ledges and cliffs
    float terr = 0;
    if (s.w[Badlands] > 0.05f) {
        const float stepH = 7.f;
        float q = base / stepH;
        float f = q - std::floor(q);
        float tq = std::floor(q) + sstep(0.55f, 0.95f, f);
        terr = (tq * stepH - base) * sstep(0.3f, 0.75f, s.w[Badlands]) * 0.85f;   // only in real mesa country
    }
    // rivers and washes
    float forced = 0, bestF = 0, bestDD = 1e9f;
    float water = -1e9f, fvx = 0, fvz = 0, wW = 0, bank = 0, bankLevel = -1e9f, sed = 0, wet = 0;
    float fade = 1.f;   // detail fades towards water and roads
    {
        int gx = int(std::floor(x / HASH)), gz = int(std::floor(z / HASH));
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                auto it = riverCells.find(ckey(gx + dx, gz + dz));
                if (it == riverCells.end()) continue;
                for (const Ref& r : it->second) {
                    const River& R = rivers[r.a];
                    const RiverPt& a = R.pts[r.b];
                    const RiverPt& b = R.pts[r.b + 1];
                    float t;
                    float dd = segDist(x, z, a.x, a.z, b.x, b.z, t);
                    float half = lerpf(a.half, b.half, t);
                    float L = lerpf(a.level, b.level, t);
                    if (R.dry) {
                        if (dd > half + 6.f) continue;
                        float f = 1.f - sstep(half * 0.7f, half + 5.f, dd);
                        sed = std::max(sed, f);
                        wet = std::max(wet, 0.12f * f);
                        float ch = L + std::max(0.f, dd - half * 0.6f) * 0.12f;
                        if (f > bestF) { bestF = f * 0.8f; forced = ch; }
                        fade = std::min(fade, sstep(half * 0.5f, half + 10.f, dd));
                        continue;
                    }
                    float depth = lerpf(a.depth, b.depth, t);
                    float bankW = 2.f + half * 0.45f, bankH = 0.6f + depth * 0.25f;
                    fade = std::min(fade, sstep(half, half + bankW + 14.f, dd));
                    if (dd > half + bankW + 16.f) continue;
                    float ch = L + std::max(smin(SHORE * (dd - half), bankH, 0.4f), -depth);
                    ch = std::max(ch, L - depth) - 0.06f * (1.f - sstep(0.f, half, dd)) * depth;
                    float f = 1.f - sstep(half + bankW * 0.6f, half + bankW + 3.f, dd);
                    if (f > bestF + 1e-3f || (f >= bestF - 1e-3f && dd < bestDD)) {
                        bestF = f; forced = ch; bestDD = dd;
                        if (dd < half + bankW) {
                            water = L;
                            float tx = b.x - a.x, tz = b.z - a.z, tl = std::max(1e-3f, std::hypot(tx, tz));
                            float slope = std::max(0.f, a.level - b.level) / tl;
                            float speed = std::clamp(std::sqrt(slope) * 9.f, 0.25f, 2.6f);
                            fvx = tx / tl * speed; fvz = tz / tl * speed;
                            wW = 2 * half;
                        }
                    }
                    bank = std::max(bank, 1.f - sstep(half, half + bankW + 9.f, dd));
                    if (dd < half + bankW + 14.f) bankLevel = std::max(bankLevel, L);
                    wet = std::max(wet, 1.f - sstep(half, half + bankW + 6.f, dd));
                }
            }
    }
    // the road network: the land is formed to the nearest road
    // nearest road, and the nearest one at a clearly different level (the next leg of a
    // switchback, a road passing above): between the two the slope runs from one to the other
    float rd = 1e4f, ry = 0, rd2 = 1e4f, ry2 = 0, rdF = 1e4f, ryF = 0, rdT = 1e4f;   // F: the nearest stretch on the land (formation), T: in a tunnel
    int cls = -1;
    {
        struct C { float d, y; int cls, road, seg; bool form; };
        C cand[48];
        int nc = 0;
        int gx = int(std::floor(x / HASH)), gz = int(std::floor(z / HASH));
        for (int dz = -2; dz <= 2; ++dz)
            for (int dx = -2; dx <= 2; ++dx) {
                auto it = roadCells.find(ckey(gx + dx, gz + dz));
                if (it == roadCells.end()) continue;
                for (const Ref& r : it->second) {
                    const Road& R = roads[r.a];
                    const RoadPt& a = R.pts[r.b];
                    const RoadPt& b = R.pts[r.b + 1];
                    float t;
                    float d = segDist(x, z, a.x, a.z, b.x, b.z, t);
                    if (d > 110.f) continue;
                    if (a.kind == 1 && b.kind == 1) { rdT = std::min(rdT, d); continue; }   // in a tunnel: the land above is untouched
                    bool form = a.kind != 2 && b.kind != 2;      // bridges leave the land alone
                    // past a tunnel portal (beyond the open end of the cutting) nothing is formed
                    if (form && (a.kind == 1 || b.kind == 1)) {
                        float ex = b.x - a.x, ez = b.z - a.z;
                        float tr = ((x - a.x) * ex + (z - a.z) * ez) / std::max(1e-6f, ex * ex + ez * ez);
                        if ((b.kind == 1 && tr > 0.6f) || (a.kind == 1 && tr < 0.4f)) form = false;
                    }
                    C c{d, lerpf(a.y, b.y, t), R.cls, r.a, r.b, form};
                    if (nc < 48) cand[nc++] = c;
                    else {
                        int w = 0;
                        for (int q = 1; q < 48; ++q) if (cand[q].d > cand[w].d) w = q;
                        if (c.d < cand[w].d) cand[w] = c;
                    }
                }
            }
        int best = -1;
        for (int q = 0; q < nc; ++q) {
            if (cand[q].d < rd) { rd = cand[q].d; ry = cand[q].y; cls = cand[q].cls; }
            if (cand[q].form && cand[q].d < rdF) { rdF = cand[q].d; ryF = cand[q].y; best = q; }
        }
        if (rdT < rdF) { rdF = 1e4f; best = -1; }   // over a tunnel (past its portal): the hill stays whole
        if (best >= 0)
            for (int q = 0; q < nc; ++q) {
                if (!cand[q].form || std::fabs(cand[q].y - ryF) <= 2.5f || cand[q].d >= rd2) continue;
                // the same road just along from here isn't a second level
                if (cand[q].road == cand[best].road && std::abs(cand[q].seg - cand[best].seg) < 14) continue;
                rd2 = cand[q].d; ry2 = cand[q].y;
            }
    }
    fade = std::min(fade, sstep(HW + 2.f, 40.f, rdF));
    float land = base + (detail + terr) * fade;
    float protect = 1.f;
    if (rdF < 90.f) {
        float blend = sstep(14.f, 80.f, rdF);
        float carved = ryF - 0.30f - sstep(5.f, 9.f, rdF) * (1.f - sstep(10.f, 16.f, rdF)) * 0.35f;
        land = carved + (land - carved) * blend;
        if (rd2 < 110.f) {
            float c2 = ry2 - 0.30f;
            float t = sstep(HW + 1.f, std::max(HW + 2.f, rdF + rd2 - HW - 1.f), rdF);
            float between = carved + (c2 - carved) * t;
            float w = 1.f - sstep(70.f, 150.f, rdF + rd2);
            land = lerpf(land, between, w);
        }
    }
    if (rdF < 90.f) {   // (under bridges the river keeps its channel)
        protect = sstep(HW + 2.f, HW + 10.f, rdF);
        for (const Bridge& b : bridgesNear(x, z, 40.f)) {
            float db = std::hypot(x - b.x, z - b.z);
            protect = std::max(protect, 1.f - sstep(b.span * 0.5f - 2.f, b.span * 0.5f + 4.f, db));
        }
    }
    s.h = lerpf(land, forced, bestF * protect);
    if (rdF < HW + 3.f && s.waterW >= 1e3f) { s.water = -1e9f; s.waterW = 0; }   // the road's formation holds the lake back
    if (protect > 0.5f && water > -1e8f && water > s.water) { s.water = water; s.flowX = fvx; s.flowZ = fvz; s.waterW = wW; }
    s.bank = std::max(s.bank, bank * protect);
    if (protect > 0.5f) s.bankLevel = std::max(s.bankLevel, bankLevel);
    s.wet = std::max(s.wet, wet * protect + s.w[Wetland] * 0.6f);
    s.sediment = std::max(s.sediment, sed * protect);
    if (sed > 0) s.sand = std::max(s.sand, sed * 0.8f);
    s.road = rd;
    s.roadY = ry;
    s.tunnel = rdT;
    s.roadCls = cls;
    return s;
}

// ------------------------------------------------------------------ cache
namespace {
struct Out {
    std::ofstream f;
    template <class T> void pod(const T& v) { f.write(reinterpret_cast<const char*>(&v), sizeof(T)); }
    template <class T> void vec(const std::vector<T>& v) { uint64_t s = v.size(); pod(s); if (s) f.write(reinterpret_cast<const char*>(v.data()), std::streamsize(s * sizeof(T))); }
    void str(const std::string& s) { uint64_t n = s.size(); pod(n); f.write(s.data(), std::streamsize(n)); }
};
struct In {
    std::ifstream f;
    template <class T> void pod(T& v) { f.read(reinterpret_cast<char*>(&v), sizeof(T)); }
    template <class T> void vec(std::vector<T>& v) {
        uint64_t s = 0; pod(s);
        if (!f || s > (uint64_t(1) << 32)) { f.setstate(std::ios::failbit); return; }
        v.resize(size_t(s));
        if (s) f.read(reinterpret_cast<char*>(v.data()), std::streamsize(s * sizeof(T)));
    }
    void str(std::string& s) { uint64_t n = 0; pod(n); if (n > 4096) { f.setstate(std::ios::failbit); return; } s.resize(size_t(n)); f.read(s.data(), std::streamsize(n)); }
};
constexpr uint32_t MAGIC = 0x4D575243;   // "CRWM"
}  // namespace

bool Map::save(const std::string& path) const {
    Out o;
    o.f.open(path, std::ios::binary);
    if (!o.f) return false;
    o.pod(MAGIC); o.pod(VERSION);
    o.pod(params); o.pod(n); o.pod(cell); o.pod(x0); o.pod(z0); o.pod(cn); o.pod(ccell);
    o.pod(windX); o.pod(windZ); o.pod(snowLine); o.pod(stats);
    o.vec(h); o.vec(temp); o.vec(precip); o.vec(moist); o.vec(q); o.vec(sand); o.vec(lakeId); o.vec(wash); o.vec(biome);
    uint64_t nr = rivers.size(); o.pod(nr);
    for (const River& r : rivers) { o.vec(r.pts); o.pod(r.into); o.pod(r.dry); }
    o.vec(lakes);
    uint64_t nt = towns.size(); o.pod(nt);
    for (const Town& t : towns) { o.pod(t.x); o.pod(t.z); o.pod(t.y); o.pod(t.rank); o.str(t.name); }
    uint64_t nrd = roads.size(); o.pod(nrd);
    for (const Road& r : roads) { o.vec(r.pts); o.pod(r.cls); o.pod(r.a); o.pod(r.b); o.pod(r.length); }
    uint64_t nn = nodes.size(); o.pod(nn);
    for (const Node& d : nodes) { o.pod(d.x); o.pod(d.z); o.pod(d.y); o.vec(d.roads); o.pod(d.town); }
    o.vec(bridges);
    o.vec(tunnels);
    return bool(o.f);
}

bool Map::load(const std::string& path) {
    In i;
    i.f.open(path, std::ios::binary);
    if (!i.f) return false;
    uint32_t magic = 0;
    int ver = 0;
    i.pod(magic); i.pod(ver);
    if (magic != MAGIC || ver != VERSION) return false;
    i.pod(params); i.pod(n); i.pod(cell); i.pod(x0); i.pod(z0); i.pod(cn); i.pod(ccell);
    i.pod(windX); i.pod(windZ); i.pod(snowLine); i.pod(stats);
    i.vec(h); i.vec(temp); i.vec(precip); i.vec(moist); i.vec(q); i.vec(sand); i.vec(lakeId); i.vec(wash); i.vec(biome);
    uint64_t nr = 0; i.pod(nr);
    if (!i.f || nr > 10000000) return false;
    rivers.resize(size_t(nr));
    for (River& r : rivers) { i.vec(r.pts); i.pod(r.into); i.pod(r.dry); }
    i.vec(lakes);
    uint64_t nt = 0; i.pod(nt);
    if (!i.f || nt > 100000) return false;
    towns.resize(size_t(nt));
    for (Town& t : towns) { i.pod(t.x); i.pod(t.z); i.pod(t.y); i.pod(t.rank); i.str(t.name); }
    uint64_t nrd = 0; i.pod(nrd);
    if (!i.f || nrd > 10000000) return false;
    roads.resize(size_t(nrd));
    for (Road& r : roads) { i.vec(r.pts); i.pod(r.cls); i.pod(r.a); i.pod(r.b); i.pod(r.length); }
    uint64_t nn = 0; i.pod(nn);
    if (!i.f || nn > 10000000) return false;
    nodes.resize(size_t(nn));
    for (Node& d : nodes) { i.pod(d.x); i.pod(d.z); i.pod(d.y); i.vec(d.roads); i.pod(d.town); }
    i.vec(bridges);
    i.vec(tunnels);
    if (!i.f || h.size() != size_t(n) * n || biome.size() != size_t(cn) * cn) return false;
    index();
    return true;
}

}  // namespace wmap
