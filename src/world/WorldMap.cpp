// World generation: relief, climate, biomes, landforms, erosion, hydrology.
// (Roads: WorldRoads.cpp. Queries and fine detail: WorldSample.cpp.)
#include "world/WorldMap.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <numeric>
#include <queue>
#include <random>

#include "world/WorldInternal.h"

namespace wmap {
using namespace detail;

const char* biomeName(int b) {
    static const char* N[BIOMES] = {"meadow", "forest", "boreal", "alpine", "steppe", "savanna", "scrub", "dunes", "badlands", "wetland"};
    return b >= 0 && b < BIOMES ? N[b] : "?";
}

void buildRoads(Map& M, std::mt19937& rng, const Progress& progress);   // WorldRoads.cpp

namespace {

constexpr float LAPSE = 0.0065f;      // C per m
constexpr float GQ = 4.f;             // game scale on runoff: rivers a little bigger than life
constexpr float YEAR = 3.156e7f;
constexpr float Q_STREAM = 0.15f;     // m3/s: the smallest stream drawn as water
constexpr float Q_WASH = 0.45f;       // potential discharge that cuts a dry wash

const int DX8[8] = {1, 1, 0, -1, -1, -1, 0, 1};
const int DZ8[8] = {0, 1, 1, 1, 0, -1, -1, -1};

struct Range { std::vector<float> xs, zs; float H, W; };

struct Gen {
    Map& M;
    const Params& P;
    std::mt19937 rng;
    int n;
    float cell, x0, z0;
    uint32_t seed;
    std::vector<float> macro;            // relief before landforms
    std::vector<float> tbase;            // sea-level temperature (coarse)
    std::vector<float> pc;               // precipitation (coarse)
    std::vector<float> erg;              // sand supply (coarse)
    std::vector<Range> ranges;

    Gen(Map& m, const Params& p) : M(m), P(p), rng(p.seed * 2654435761u + 7u) {
        seed = p.seed * 747796405u + 2891336453u;
        cell = p.cell;
        n = int(std::lround(p.size / cell)) + 1;
        x0 = z0 = -0.5f * (n - 1) * cell;
        M.params = p;
        M.n = n; M.cell = cell; M.x0 = x0; M.z0 = z0;
        M.ccell = cell * 4;
        M.cn = (n - 1) / 4 + 1;
    }
    float U() { return std::uniform_real_distribution<float>(0.f, 1.f)(rng); }
    float X(int i) const { return x0 + i * cell; }
    float Z(int k) const { return z0 + k * cell; }
    size_t id(int i, int k) const { return size_t(k) * n + i; }
    float CX(int i) const { return x0 + i * M.ccell; }
    float CZ(int k) const { return z0 + k * M.ccell; }
    template <class V>
    float bilinC(const V& g, float x, float z) const {   // coarse-grid lookup
        float fx = std::clamp((x - x0) / M.ccell, 0.f, float(M.cn - 1) - 1e-3f), fz = std::clamp((z - z0) / M.ccell, 0.f, float(M.cn - 1) - 1e-3f);
        int i = int(fx), k = int(fz);
        float tx = fx - i, tz = fz - k;
        auto G = [&](int a, int b) { return g[size_t(b) * M.cn + a]; };
        return lerpf(lerpf(G(i, k), G(i + 1, k), tx), lerpf(G(i, k + 1), G(i + 1, k + 1), tx), tz);
    }

    // ------------------------------------------------------------ 1. relief
    void relief() {
        float wa = U() * 2 * PI;
        M.windX = std::cos(wa); M.windZ = std::sin(wa);
        const float half = 0.5f * P.size;
        auto addRange = [&](float cx, float cz, float dir, float len, float H, float W) {
            Range r;
            r.H = H; r.W = W;
            const int K = 14;
            float x = cx - std::cos(dir) * len * 0.5f, z = cz - std::sin(dir) * len * 0.5f;
            float d = dir;
            for (int i = 0; i < K; ++i) {
                r.xs.push_back(x); r.zs.push_back(z);
                d += (U() - 0.5f) * 0.5f;
                d = dir + std::clamp(d - dir, -0.6f, 0.6f);
                x += std::cos(d) * len / (K - 1);
                z += std::sin(d) * len / (K - 1);
            }
            ranges.push_back(r);
        };
        // Everything is in real units, so a bigger map is more country, not a stretched one:
        // ranges are 12-24 km long and 1.5-2.6 km wide whatever the map size; their number
        // grows with the area. The first main range lies across the wind, a little upwind of
        // the middle, so one side is wet and the other in its rain shadow.
        const float area = P.size * P.size / (20480.f * 20480.f);   // in 20 km maps
        const int mains = std::max(1, int(std::lround(area * (0.8f + 0.4f * U()))));
        for (int r = 0; r < mains; ++r) {
            float off = r == 0 ? -half * (0.05f + 0.2f * U()) : (U() - 0.5f) * P.size * 0.7f;
            float cx = M.windX * off + (r == 0 ? (U() - 0.5f) * 3000.f : (U() - 0.5f) * P.size * 0.7f);
            float cz = M.windZ * off + (r == 0 ? (U() - 0.5f) * 3000.f : (U() - 0.5f) * P.size * 0.7f);
            float dir = wa + PI * 0.5f + (U() - 0.5f) * (r == 0 ? 0.7f : 1.6f);
            addRange(cx, cz, dir, 12000.f + 12000.f * U(), 1000.f + 650.f * U(), 1700.f + 900.f * U());
        }
        int minor = std::max(1, int(std::lround(area * (1.f + 1.5f * U()))));
        for (int r = 0; r < minor; ++r) {
            float cx = (U() - 0.5f) * P.size * 0.85f, cz = (U() - 0.5f) * P.size * 0.85f;
            addRange(cx, cz, U() * 2 * PI, 5000.f + 5000.f * U(), 280.f + 520.f * U(), 900.f + 700.f * U());
        }
        macro.assign(size_t(n) * n, 0.f);
        parallelFor(n, [&](int k) {
            for (int i = 0; i < n; ++i) {
                float x = X(i), z = Z(k);
                float base = 170.f + 120.f * fbm(x / 9000.f, z / 9000.f, 3, seed + 1) + 55.f * fbm(x / 3200.f, z / 3200.f, 3, seed + 2);
                float m = 0;
                for (const Range& r : ranges) {
                    float best = 1e9f, along = 0, acc = 0, total = 0;
                    for (size_t s = 0; s + 1 < r.xs.size(); ++s) total += std::hypot(r.xs[s + 1] - r.xs[s], r.zs[s + 1] - r.zs[s]);
                    for (size_t s = 0; s + 1 < r.xs.size(); ++s) {
                        float t;
                        float sl = std::hypot(r.xs[s + 1] - r.xs[s], r.zs[s + 1] - r.zs[s]);
                        float d = segDist(x, z, r.xs[s], r.zs[s], r.xs[s + 1], r.zs[s + 1], t);
                        if (d < best) { best = d; along = (acc + t * sl) / total; }
                        acc += sl;
                    }
                    float taper = sstep(0.f, 0.18f, along) * sstep(1.f, 0.82f, along);
                    // the spine wanders across the range so peaks and cols alternate
                    float wob = r.W * 0.35f * fbm(x / 4000.f, z / 4000.f, 2, seed + 9);
                    float d = std::max(0.f, best + wob);
                    float rid = ridged(x / 3400.f, z / 3400.f, 5, seed + 3);
                    float core = r.H * taper * std::exp(-1.5f * (d / r.W) * (d / r.W)) * (0.45f + 0.85f * rid);
                    float foot = 0.2f * r.H * taper * std::exp(-(d / (2.8f * r.W)) * (d / (2.8f * r.W)));
                    m += core + foot;
                }
                macro[id(i, k)] = base + m;
            }
        });
    }

    // ------------------------------------------------------------ 2. climate
    void climate() {
        const int cn = M.cn;
        const float cc = M.ccell;
        std::vector<float> hc(size_t(cn) * cn);
        for (int k = 0; k < cn; ++k)
            for (int i = 0; i < cn; ++i) hc[size_t(k) * cn + i] = macro[id(std::min(i * 4, n - 1), std::min(k * 4, n - 1))];
        auto HC = [&](float x, float z) { return bilinC(hc, x, z); };
        // temperature at sea level: the seed's climate, a regional gradient, some noise
        float tsea = 7.5f + 9.5f * U();
        if (P.climate == 1) tsea = 9.f + 4.f * U();
        if (P.climate == 2) tsea = 16.f + 4.f * U();
        if (P.climate == 3) tsea = 3.f + 4.f * U();
        const float ga = U() * 2 * PI, gx = std::cos(ga), gz = std::sin(ga);
        tbase.assign(size_t(cn) * cn, 0.f);
        for (int k = 0; k < cn; ++k)
            for (int i = 0; i < cn; ++i) {
                float x = CX(i), z = CZ(k);
                tbase[size_t(k) * cn + i] = tsea + 0.3f * (x * gx + z * gz) / 1000.f + 1.3f * fbm(x / 7000.f, z / 7000.f, 3, seed + 21);   // 0.3 C per km
            }
        // precipitation: moist air comes in on the wind and rains out - steadily over the
        // plains, heavily up windward slopes; it sinks and dries behind the mountains
        const float wx = M.windX, wz = M.windZ;
        const float ds = 2 * cc;
        std::vector<int> order(size_t(cn) * cn);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int a, int b) {
            return CX(a % cn) * wx + CZ(a / cn) * wz < CX(b % cn) * wx + CZ(b / cn) * wz;
        });
        std::vector<float> qv(size_t(cn) * cn, 1.f);
        pc.assign(size_t(cn) * cn, 0.f);
        // the seed's climate: about one map in three is a dry one, with true deserts in its lee
        float dry = U() < 0.33f ? 0.6f + 0.5f * U() : 0.f;
        if (P.climate == 1) dry = -0.2f;
        if (P.climate == 2) dry = 1.6f + 0.4f * U();   // a desert: only the windward mountains get real rain
        if (P.climate == 3) dry = 0.1f;
        const float wet = std::exp(-0.6f * (P.aridity + dry)) * (0.85f + 0.3f * U());
        const float kb = 0.22f / 20000.f, ko = 6.5e-4f;
        for (int c : order) {
            int i = c % cn, k = c / cn;
            float x = CX(i), z = CZ(k);
            float ux = x - wx * ds, uz = z - wz * ds;
            float qin = 1.f;
            if (ux >= x0 && uz >= z0 && ux <= x0 + P.size && uz <= z0 + P.size) qin = bilinC(qv, ux, uz);
            float dz = HC(x, z) - HC(ux, uz);
            float up = std::max(0.f, dz), down = std::max(0.f, -dz);
            float frac = 1.f - std::exp(-(kb * ds + ko * up));
            float rain = qin * frac * (1.f - std::min(0.85f, down / ds * 6.f));   // sinking air: little rain
            // moisture recycled from the land below slowly refills the air downwind
            qv[c] = qin - rain + (1.f - (qin - rain)) * 0.028f * ds / 1000.f;
            pc[c] = rain / ds;
        }
        // to mm/yr: ~850 mm on the windward plains
        const float scale = 850.f / kb * wet;
        for (int c = 0; c < cn * cn; ++c) {
            float x = CX(c % cn), z = CZ(c / cn);
            pc[c] = std::clamp(pc[c] * scale * std::exp(0.25f * fbm(x / 5000.f, z / 5000.f, 3, seed + 31)), 40.f, 3200.f);
        }
        for (int pass = 0; pass < 3; ++pass) {   // weather systems don't follow grid lines
            std::vector<float> s(pc);
            for (int k = 1; k < cn - 1; ++k)
                for (int i = 1; i < cn - 1; ++i) {
                    float a = 0;
                    for (int dk = -1; dk <= 1; ++dk) for (int di = -1; di <= 1; ++di) a += pc[size_t(k + dk) * cn + i + di];
                    s[size_t(k) * cn + i] = a / 9.f;
                }
            pc.swap(s);
        }
        erg.assign(size_t(cn) * cn, 0.f);
        for (int c = 0; c < cn * cn; ++c) {
            float x = CX(c % cn), z = CZ(c / cn);
            erg[c] = sstep(-0.35f, 0.15f, fbm(x / 6500.f, z / 6500.f, 3, seed + 41));
        }
        double tsum = 0;
        for (float t : tbase) tsum += t;
        M.snowLine = float(tsum / tbase.size()) / LAPSE + 150.f;
    }

    // ------------------------------------------------------------ 3. biomes
    void biomes(const std::vector<float>& hgt, bool withWetland, const std::vector<float>* wetFine) {
        const int cn = M.cn;
        M.biome.assign(size_t(cn) * cn, {});
        struct C { float t, p, st; };
        static const C cen[BIOMES] = {
            {10.f, 800.f, 4.5f},   // meadow
            {9.f, 1450.f, 4.5f},   // forest
            {3.f, 650.f, 3.5f},    // boreal
            {-2.5f, 900.f, 3.f},   // alpine
            {8.f, 330.f, 4.5f},    // steppe
            {18.5f, 650.f, 3.5f},  // savanna
            {15.f, 470.f, 3.5f},   // scrub
            {16.f, 170.f, 7.f},    // dunes
            {12.f, 190.f, 5.f},    // badlands
            {0.f, 1.f, 1.f},       // wetland (from hydrology)
        };
        parallelFor(cn, [&](int k) {
            for (int i = 0; i < cn; ++i) {
                size_t c = size_t(k) * cn + i;
                float x = CX(i), z = CZ(k);
                auto HG = [&](int a, int b) { return hgt[id(std::clamp(a * 4, 0, n - 1), std::clamp(b * 4, 0, n - 1))]; };
                float hh = HG(i, k);
                // dunes gather in low, open basins; dry mountains are rock
                float slope = std::hypot(HG(i + 2, k) - HG(i - 2, k), HG(i, k + 2) - HG(i, k - 2)) / (4 * M.ccell);
                float basin = sstep(0.09f, 0.03f, slope) * sstep(950.f, 550.f, hh);
                float T = tbase[c] - LAPSE * hh + 1.2f * fbm(x / 1500.f, z / 1500.f, 3, seed + 51);
                float lp = std::log(pc[c]) + 0.18f * fbm(x / 1700.f, z / 1700.f, 3, seed + 52);
                std::array<float, BIOMES> w{};
                float sum = 0;
                for (int b = 0; b < Wetland; ++b) {
                    float dt = (T - cen[b].t) / cen[b].st, dp = (lp - std::log(cen[b].p)) / (b == Dunes ? 0.62f : 0.5f);
                    float v = std::exp(-(dt * dt + dp * dp));
                    if (b == Dunes) v *= erg[c] * basin;
                    if (b == Badlands) v *= 1.f - 0.85f * erg[c];
                    if (b == Alpine) v = std::max(v, sstep(1.5f, -1.5f, T));   // cold is alpine whatever the rain
                    w[b] = std::pow(v, 1.6f);
                    sum += w[b];
                }
                if (sum <= 1e-12f) { w[Meadow] = 1; sum = 1; }
                for (auto& v : w) v /= sum;
                if (withWetland && wetFine) {   // average the fine wetland weight over the coarse cell
                    float a = 0;
                    int cnt = 0;
                    for (int dk = -2; dk <= 2; ++dk)
                        for (int di = -2; di <= 2; ++di) {
                            int fi = std::clamp(i * 4 + di, 0, n - 1), fk = std::clamp(k * 4 + dk, 0, n - 1);
                            a += (*wetFine)[id(fi, fk)];
                            ++cnt;
                        }
                    float ww = std::min(0.9f, a / cnt);
                    for (auto& v : w) v *= 1.f - ww;
                    w[Wetland] = ww;
                }
                M.biome[c] = w;
            }
        });
    }
    std::array<float, BIOMES> wAt(float x, float z) const {
        // biome weights at a point, its position warped so borders are ragged
        float wx = x + 260.f * fbm(x / 900.f, z / 900.f, 3, seed + 61), wz = z + 260.f * fbm(x / 900.f + 7.f, z / 900.f, 3, seed + 62);
        float fx = std::clamp((wx - x0) / M.ccell, 0.f, float(M.cn - 1) - 1e-3f), fz = std::clamp((wz - z0) / M.ccell, 0.f, float(M.cn - 1) - 1e-3f);
        int i = int(fx), k = int(fz);
        float tx = fx - i, tz = fz - k;
        std::array<float, BIOMES> r{};
        auto& a = M.biome[size_t(k) * M.cn + i];
        auto& b = M.biome[size_t(k) * M.cn + i + 1];
        auto& c = M.biome[size_t(k + 1) * M.cn + i];
        auto& d = M.biome[size_t(k + 1) * M.cn + i + 1];
        for (int q = 0; q < BIOMES; ++q) r[q] = lerpf(lerpf(a[q], b[q], tx), lerpf(c[q], d[q], tx), tz);
        return r;
    }

    // ------------------------------------------------------------ 4. landforms
    float dune(float x, float z) const {
        const float wx = M.windX, wz = M.windZ;
        float u = x * wx + z * wz, v = -x * wz + z * wx;
        float vw = v + 700.f * fbm(u / 3500.f, v / 3500.f, 2, seed + 71);
        float uw = u + 260.f * fbm(u / 2500.f + 3.f, v / 2500.f, 2, seed + 72);
        // transverse dunes: a gentle windward slope, a steep slip face; crests break into
        // segments (barchanoid ridges)
        float ph = uw / 430.f + 0.35f * fbm(vw / 1100.f, 0.5f, 2, seed + 73);
        float t = ph - std::floor(ph);
        float prof = t < 0.74f ? std::pow(t / 0.74f, 1.4f) : std::pow(1.f - (t - 0.74f) / 0.26f, 0.7f);
        prof = prof * prof * (3 - 2 * prof);
        float seg = sstep(-0.25f, 0.45f, fbm(vw / 1300.f, uw / 2400.f, 3, seed + 74));
        // linear (seif) dunes running with the wind where sand is scarcer
        float seif = 1.f - std::fabs(std::sin(vw / 380.f * PI));
        seif = seif * seif * sstep(-0.1f, 0.5f, fbm(u / 4500.f, v / 4500.f, 2, seed + 75));
        return 24.f * prof * seg + 18.f * seif * (1.f - seg) + 3.f * fbm(x / 160.f, z / 160.f, 2, seed + 76);
    }
    float mesa(float x, float z) const {
        float wx = x + 500.f * fbm(x / 2500.f, z / 2500.f, 3, seed + 81), wz = z + 500.f * fbm(x / 2500.f, z / 2500.f + 9.f, 3, seed + 82);
        float nn = 0.5f + 0.5f * fbm(wx / 2300.f, wz / 2300.f, 4, seed + 83);
        // tier edges are cliffs, broken here and there by talus ramps and gaps (the way up)
        float brk = sstep(0.15f, 0.55f, fbm(x / 900.f, z / 900.f, 2, seed + 85));
        float w = 0.03f + 0.16f * brk;
        float tiers = 75.f * sstep(0.50f, 0.50f + w, nn) + 45.f * sstep(0.62f, 0.62f + 0.85f * w, nn) + 30.f * sstep(0.735f, 0.735f + 0.7f * w, nn);
        return tiers + 7.f * fbm(x / 500.f, z / 500.f, 3, seed + 84);
    }
    float style(int b, float x, float z) const {
        switch (b) {
            case Meadow: return 38.f * fbm(x / 1500.f, z / 1500.f, 4, seed + 91) + 15.f * fbm(x / 560.f, z / 560.f, 3, seed + 92);
            case Forest: return 62.f * fbm(x / 1300.f, z / 1300.f, 4, seed + 93) + 18.f * fbm(x / 480.f, z / 480.f, 3, seed + 94);
            case Boreal: {   // knobby glacial country: hummocks and kettle hollows
                float hum = 13.f * fbm(x / 520.f, z / 520.f, 3, seed + 95);
                float ket = sstep(0.25f, 0.65f, fbm(x / 460.f, z / 460.f, 2, seed + 96));
                return hum - 16.f * ket;
            }
            case Alpine: return 170.f * (ridged(x / 1700.f, z / 1700.f, 5, seed + 97) - 0.42f);
            case Steppe: return 11.f * fbm(x / 2600.f, z / 2600.f, 3, seed + 98) + 3.f * fbm(x / 500.f, z / 500.f, 2, seed + 99);
            case Savanna: {  // flat plains with the odd granite dome (inselberg)
                float dome = sstep(0.42f, 0.85f, fbm(x / 1400.f, z / 1400.f, 3, seed + 101) * 0.5f + 0.5f);
                return 8.f * fbm(x / 2000.f, z / 2000.f, 3, seed + 102) + 85.f * dome * dome * dome;
            }
            case Scrub: return 48.f * (ridged(x / 1150.f, z / 1150.f, 4, seed + 103) - 0.38f) + 9.f * fbm(x / 380.f, z / 380.f, 3, seed + 104);
            case Dunes: return dune(x, z);
            case Badlands: return mesa(x, z);
            default: return 0.f;
        }
    }
    void landforms() {
        M.h.assign(size_t(n) * n, 0.f);
        parallelFor(n, [&](int k) {
            for (int i = 0; i < n; ++i) {
                float x = X(i), z = Z(k);
                auto w = wAt(x, z);
                float add = 0;
                for (int b = 0; b < BIOMES; ++b)
                    if (w[b] > 0.004f) add += w[b] * style(b, x, z);
                // dunes and playa basins sit on smoothed-out ground
                M.h[id(i, k)] = macro[id(i, k)] + add;
            }
        });
    }

    // ------------------------------------------------------------ 5. erosion
    void erode() {
        if (P.erosion <= 0) return;
        const float K = 30.f * cell;             // height units for the droplet model
        std::vector<float> hn(M.h.size());
        for (size_t c = 0; c < hn.size(); ++c) hn[c] = M.h[c] / K;
        // per-cell rain and erodibility
        std::vector<float> rainW(size_t(n) * n), soft(size_t(n) * n);
        parallelFor(n, [&](int k) {
            for (int i = 0; i < n; ++i) {
                float x = X(i), z = Z(k);
                auto w = wAt(x, z);
                float p = bilinC(pc, x, z);
                rainW[id(i, k)] = std::clamp(p / 900.f, 0.04f, 1.8f) * (1.f - 0.97f * w[Dunes]);
                soft[id(i, k)] = 1.f + 0.6f * w[Badlands] - 0.3f * w[Alpine] - 0.2f * w[Savanna];
            }
        });
        const int R = 2;
        struct BW { int dx, dz; float w; };
        std::vector<BW> brush;
        float bsum = 0;
        for (int dz = -R; dz <= R; ++dz)
            for (int dx = -R; dx <= R; ++dx) {
                float d = std::sqrt(float(dx * dx + dz * dz));
                if (d <= R) { brush.push_back({dx, dz, 1.f - d / R}); bsum += 1.f - d / R; }
            }
        for (auto& b : brush) b.w /= bsum;
        const int drops = int(float(n) * n * 0.55f * P.erosion);
        // strips across the map: droplets run in alternate strips at once (they never travel
        // further than a strip's width), so threads never touch the same cells
        unsigned T = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
        const int strips = int(T) * 2;
        const int sw = (n + strips - 1) / strips;
        std::atomic<int> total{0};
        for (int phase = 0; phase < 2; ++phase) {
            std::vector<std::thread> th;
            for (int s = phase; s < strips; s += 2)
                th.emplace_back([&, s] {
                    std::mt19937 r(seed + uint32_t(s) * 7919u + uint32_t(phase));
                    std::uniform_real_distribution<float> uf(0.f, 1.f);
                    const int k0 = s * sw, k1 = std::min(n - 1, (s + 1) * sw);
                    const int budget = int(float(drops) * float(k1 - k0) / float(n) / 1.f);
                    int done = 0;
                    for (int attempt = 0; attempt < budget * 3 && done < budget; ++attempt) {
                        float px = 2 + uf(r) * (n - 5), pz = k0 + uf(r) * (k1 - k0 - 1);
                        if (pz < 2 || pz > n - 3) continue;
                        if (uf(r) > rainW[id(int(px), int(pz))] / 1.8f) continue;
                        ++done;
                        float dxv = 0, dzv = 0, speed = 1, water = 1, sed = 0;
                        for (int life = 0; life < 48; ++life) {
                            int ix = int(px), iz = int(pz);
                            if (ix < 1 || iz < 1 || ix >= n - 2 || iz >= n - 2) break;
                            if (iz < k0 - 40 || iz > k1 + 40) break;
                            float fx = px - ix, fz = pz - iz;
                            float h00 = hn[id(ix, iz)], h10 = hn[id(ix + 1, iz)], h01 = hn[id(ix, iz + 1)], h11 = hn[id(ix + 1, iz + 1)];
                            float gx = (h10 - h00) * (1 - fz) + (h11 - h01) * fz;
                            float gz = (h01 - h00) * (1 - fx) + (h11 - h10) * fx;
                            float hOld = h00 * (1 - fx) * (1 - fz) + h10 * fx * (1 - fz) + h01 * (1 - fx) * fz + h11 * fx * fz;
                            dxv = dxv * 0.05f - gx * 0.95f;
                            dzv = dzv * 0.05f - gz * 0.95f;
                            float l = std::sqrt(dxv * dxv + dzv * dzv);
                            if (l < 1e-9f) break;
                            dxv /= l; dzv /= l;
                            float nx = px + dxv, nz = pz + dzv;
                            int jx = int(nx), jz = int(nz);
                            if (jx < 1 || jz < 1 || jx >= n - 2 || jz >= n - 2) break;
                            float gx2 = nx - jx, gz2 = nz - jz;
                            float hNew = hn[id(jx, jz)] * (1 - gx2) * (1 - gz2) + hn[id(jx + 1, jz)] * gx2 * (1 - gz2) + hn[id(jx, jz + 1)] * (1 - gx2) * gz2 + hn[id(jx + 1, jz + 1)] * gx2 * gz2;
                            float dh = hNew - hOld;
                            float cap = std::max(-dh * speed * water * 4.f, 0.0005f);
                            if (sed > cap || dh > 0) {
                                float dep = dh > 0 ? std::min(dh, sed) : (sed - cap) * 0.3f;
                                sed -= dep;
                                hn[id(ix, iz)] += dep * (1 - fx) * (1 - fz);
                                hn[id(ix + 1, iz)] += dep * fx * (1 - fz);
                                hn[id(ix, iz + 1)] += dep * (1 - fx) * fz;
                                hn[id(ix + 1, iz + 1)] += dep * fx * fz;
                            } else {
                                float er = std::min((cap - sed) * 0.3f * soft[id(ix, iz)], -dh);
                                for (const BW& b : brush) {
                                    int cx = ix + b.dx, cz = iz + b.dz;
                                    if (cx < 0 || cz < 0 || cx >= n || cz >= n) continue;
                                    float& hv = hn[id(cx, cz)];
                                    float e = std::min(hv, er * b.w);
                                    hv -= e;
                                    sed += e;
                                }
                            }
                            speed = std::sqrt(std::max(0.f, speed * speed - dh * 4.f));
                            water *= 0.98f;
                            px = nx; pz = nz;
                        }
                    }
                    total += done;
                });
            for (auto& t : th) t.join();
        }
        M.stats.droplets = total;
        for (size_t c = 0; c < hn.size(); ++c) M.h[c] = hn[c] * K;
        // thermal slumping to each material's angle of repose (sand ~32 deg, rock steeper,
        // mesa caprock steeper still)
        std::vector<float> talus(size_t(n) * n);
        parallelFor(n, [&](int k) {
            for (int i = 0; i < n; ++i) {
                auto w = wAt(X(i), Z(k));
                float t = 0.80f * (1 - w[Dunes] - w[Badlands] - w[Alpine]) + 0.62f * w[Dunes] + 1.6f * w[Badlands] + 1.1f * w[Alpine];
                talus[id(i, k)] = t * cell;
            }
        });
        for (int it = 0; it < 10; ++it) {
            std::vector<float> d(M.h.size(), 0.f);
            parallelFor(n - 2, [&](int kk) {
                int k = kk + 1;
                for (int i = 1; i < n - 1; ++i) {
                    float hc = M.h[id(i, k)];
                    float maxd = 0, tot = 0;
                    float ex[8];
                    for (int q = 0; q < 8; ++q) {
                        float dist = (q & 1) ? 1.4142f : 1.f;
                        float diff = hc - M.h[id(i + DX8[q], k + DZ8[q])] - talus[id(i, k)] * dist;
                        ex[q] = diff > 0 ? diff : 0;
                        tot += ex[q];
                        maxd = std::max(maxd, ex[q]);
                    }
                    if (tot <= 0) continue;
                    float move = maxd * 0.25f;
                    d[id(i, k)] -= move;   // the receivers are credited in a second sweep
                    (void)move;
                }
            });
            // second sweep: hand the material to the neighbours in proportion (gather form)
            std::vector<float> g(M.h.size(), 0.f);
            parallelFor(n - 2, [&](int kk) {
                int k = kk + 1;
                for (int i = 1; i < n - 1; ++i) {
                    float gain = 0;
                    for (int q = 0; q < 8; ++q) {
                        int si = i + DX8[q], sk = k + DZ8[q];
                        if (si < 1 || sk < 1 || si >= n - 1 || sk >= n - 1) continue;
                        float mv = -d[id(si, sk)];
                        if (mv <= 0) continue;
                        // share of the source's excess that falls towards this cell
                        float hs = M.h[id(si, sk)];
                        float tot = 0, mine = 0;
                        for (int r = 0; r < 8; ++r) {
                            float dist = (r & 1) ? 1.4142f : 1.f;
                            float diff = hs - M.h[id(si + DX8[r], sk + DZ8[r])] - talus[id(si, sk)] * dist;
                            if (diff > 0) { tot += diff; if (si + DX8[r] == i && sk + DZ8[r] == k) mine = diff; }
                        }
                        if (tot > 0) gain += mv * mine / tot;
                    }
                    g[id(i, k)] = gain;
                }
            });
            for (size_t c = 0; c < M.h.size(); ++c) M.h[c] += d[c] + g[c];
        }
    }

    // ------------------------------------------------------------ 6. hydrology
    std::vector<double> F;
    std::vector<int> recv;
    std::vector<float> qn;            // discharge with no losses (potential)
    std::vector<float> area;          // upstream cells
    std::vector<float> ai;            // aridity index P / PET
    std::vector<int> comp;            // depression id
    std::vector<int> compOut;         // depression -> outlet cell
    std::vector<std::vector<int>> compCells;

    void flood() {
        const size_t NN = size_t(n) * n;
        F.assign(NN, 0.0);
        std::vector<uint8_t> closed(NN, 0);
        using QE = std::pair<double, int>;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
        for (int k = 0; k < n; ++k)
            for (int i = 0; i < n; ++i)
                if (i == 0 || k == 0 || i == n - 1 || k == n - 1) {
                    size_t c = id(i, k);
                    F[c] = M.h[c];
                    closed[c] = 1;
                    pq.push({F[c], int(c)});
                }
        const double eps = 2e-5;
        while (!pq.empty()) {
            auto [f, c] = pq.top();
            pq.pop();
            int i = c % n, k = c / n;
            for (int q = 0; q < 8; ++q) {
                int a = i + DX8[q], b = k + DZ8[q];
                if (a < 0 || b < 0 || a >= n || b >= n) continue;
                size_t nc = id(a, b);
                if (closed[nc]) continue;
                closed[nc] = 1;
                F[nc] = std::max(double(M.h[nc]), f + eps);
                pq.push({F[nc], int(nc)});
            }
        }
        recv.assign(NN, -1);
        parallelFor(n, [&](int k) {
            for (int i = 0; i < n; ++i) {
                if (i == 0 || k == 0 || i == n - 1 || k == n - 1) continue;
                size_t c = id(i, k);
                double best = 0;
                int r = -1;
                for (int q = 0; q < 8; ++q) {
                    size_t nc = id(i + DX8[q], k + DZ8[q]);
                    double s = (F[c] - F[nc]) / ((q & 1) ? 1.41421356 : 1.0);
                    if (s > best) { best = s; r = int(nc); }
                }
                recv[c] = r;
            }
        });
        // depressions worth a lake: connected cells filled more than 0.3 m
        comp.assign(NN, -1);
        compOut.clear();
        compCells.clear();
        for (size_t s = 0; s < NN; ++s) {
            if (comp[s] >= 0 || F[s] - M.h[s] < 0.3) continue;
            int cid = int(compCells.size());
            compCells.emplace_back();
            std::vector<int> st{int(s)};
            comp[s] = cid;
            int out = int(s);
            while (!st.empty()) {
                int c = st.back();
                st.pop_back();
                compCells[cid].push_back(c);
                if (F[c] < F[out]) out = c;
                int i = c % n, k = c / n;
                for (int q = 0; q < 8; ++q) {
                    int a = i + DX8[q], b = k + DZ8[q];
                    if (a < 0 || b < 0 || a >= n || b >= n) continue;
                    size_t nc = id(a, b);
                    if (comp[nc] >= 0 || F[nc] - M.h[nc] < 0.3) continue;
                    comp[nc] = cid;
                    st.push_back(int(nc));
                }
            }
            compOut.push_back(out);
        }
    }

    void discharge() {
        const size_t NN = size_t(n) * n;
        std::vector<int> order(NN);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return F[a] > F[b]; });
        M.temp.assign(NN, 0.f);
        M.precip.assign(NN, 0.f);
        ai.assign(NN, 1.f);
        std::vector<float> runoff(NN), pet(NN);
        parallelFor(n, [&](int k) {
            for (int i = 0; i < n; ++i) {
                size_t c = id(i, k);
                float x = X(i), z = Z(k);
                float T = bilinC(tbase, x, z) - LAPSE * M.h[c];
                float Pm = bilinC(pc, x, z);
                M.temp[c] = T;
                M.precip[c] = Pm;
                float pe = std::max(200.f, 350.f + 45.f * T);
                float aet = Pm * pe / std::sqrt(Pm * Pm + pe * pe);   // Budyko-style water balance
                runoff[c] = (Pm - aet) / 1000.f * cell * cell / YEAR * GQ;
                pet[c] = pe;
                ai[c] = Pm / pe;
            }
        });
        M.q.assign(NN, 0.f);
        qn.assign(NN, 0.f);
        area.assign(NN, 0.f);
        M.lakes.clear();
        M.lakeId.assign(NN, -1);
        std::vector<int> outComp(NN, -1);
        for (int c = 0; c < int(compOut.size()); ++c) outComp[compOut[c]] = c;
        for (int c : order) {
            M.q[c] += runoff[c];
            qn[c] += runoff[c];
            area[c] += 1.f;
            int cid = outComp[c];
            if (cid >= 0) {   // a lake's evaporation, or an endorheic basin's total loss
                lake(cid, M.q[c], pet[c], M.precip[c]);
                M.q[c] = std::max(0.f, M.q[c] - lastLoss);
            }
            // rivers in dry country lose water through their beds
            if (comp[c] < 0 && ai[c] < 1.f && M.q[c] > 0) {
                float w = 4.5f * std::sqrt(M.q[c]);
                M.q[c] = std::max(0.f, M.q[c] - (1.f - ai[c]) * 3e-5f * GQ * w * cell);
            }
            int r = recv[c];
            if (r >= 0) { M.q[r] += M.q[c]; qn[r] += qn[c]; area[r] += area[c]; }
        }
    }
    int lastLakeComp = -1;
    float lastLoss = 0;
    // decide what a depression holds: a lake to its spill point, a smaller endorheic lake
    // where evaporation balances the inflow, a salt flat, or nothing
    void lake(int cid, float inflow, float pe, float precipMm) {
        lastLakeComp = cid;
        lastLoss = 0;
        auto& cells = compCells[cid];
        std::vector<float> hs;
        hs.reserve(cells.size());
        for (int c : cells) hs.push_back(M.h[c]);
        std::sort(hs.begin(), hs.end());
        float spill = float(F[compOut[cid]]);
        float evap = std::max(0.f, (1.1f * pe - precipMm) / 1000.f / YEAR * GQ);   // m/s over open water
        float A_full = float(cells.size()) * cell * cell;
        Lake L;
        L.depth = -1;
        float level;
        if (inflow >= evap * A_full) {
            level = spill - 0.05f;
            L.full = true;
            lastLoss = evap * A_full;
        } else {
            size_t k = 0;
            while (k + 1 < hs.size() && inflow >= evap * float(k + 2) * cell * cell) ++k;
            level = std::min(spill - 0.05f, hs[k] + 0.25f);
            L.full = false;
            lastLoss = 1e30f;    // nothing leaves
        }
        float depth = level - hs.front();
        int wetCells = 0;
        for (float v : hs) wetCells += v < level;
        if (depth < 0.8f || wetCells < 4) {
            // too shallow for open water: a salt flat in a big dry basin, else nothing
            if (!L.full && cells.size() > 60 && ai[compOut[cid]] < 0.6f) {
                L.playa = true;
                L.level = hs.front() + 1.2f;
            } else return;
        } else L.level = level;
        double sx = 0, sz = 0;
        int cnt = 0;
        int lid = int(M.lakes.size());
        for (int c : cells)
            if (M.h[c] < L.level) {
                M.lakeId[c] = int16_t(lid);
                sx += X(c % n); sz += Z(c / n);
                ++cnt;
            }
        if (cnt == 0 || lid > 30000) { if (cnt) for (int c : cells) if (M.lakeId[c] == lid) M.lakeId[c] = -1; return; }
        L.cx = float(sx / cnt); L.cz = float(sz / cnt);
        L.area = cnt * cell * cell;
        L.depth = L.level - hs.front();
        M.lakes.push_back(L);
    }

    // trace rivers (and dry washes) into smooth polylines
    void rivers() {
        const size_t NN = size_t(n) * n;
        auto isLake = [&](size_t c) { return M.lakeId[c] >= 0 && !M.lakes[M.lakeId[c]].playa; };
        for (int kind = 0; kind < 2; ++kind) {
            auto on = [&](size_t c) -> bool {
                if (isLake(c)) return false;
                if (kind == 0) return M.q[c] >= Q_STREAM;
                return M.q[c] < Q_STREAM && qn[c] >= Q_WASH && ai[c] < 0.65f;
            };
            // main donor of every channel cell: the biggest inflow
            std::vector<int> mainDonor(NN, -1);
            for (size_t c = 0; c < NN; ++c) {
                if (!on(c) || recv[c] < 0) continue;
                int r = recv[c];
                if (!on(size_t(r))) continue;
                if (mainDonor[r] < 0 || qn[c] > qn[mainDonor[r]]) mainDonor[r] = int(c);
            }
            std::vector<int> owner(NN, -1);
            std::vector<std::vector<int>> chains;
            for (size_t c = 0; c < NN; ++c) {
                if (!on(c)) continue;
                // a head: no channel cell flows in as the main stem
                if (mainDonor[c] >= 0) continue;
                std::vector<int> ch{int(c)};
                int cur = int(c);
                while (true) {
                    int r = recv[cur];
                    if (r < 0) break;
                    ch.push_back(r);
                    if (!on(size_t(r)) || mainDonor[r] != cur) break;   // a lake, the end of a wash, or a confluence
                    cur = r;
                }
                chains.push_back(std::move(ch));
            }
            const int base = int(M.rivers.size());
            for (size_t ci = 0; ci < chains.size(); ++ci)
                for (size_t j = 0; j + 1 < chains[ci].size(); ++j) owner[chains[ci][j]] = base + int(ci);
            for (size_t ci = 0; ci < chains.size(); ++ci) {
                auto& ch = chains[ci];
                if (ch.size() < 4) continue;
                River R;
                R.dry = kind == 1;
                int last = ch.back();
                if (owner[last] >= 0 && owner[last] != base + int(ci)) R.into = owner[last];
                // cell centres -> Chaikin-smoothed curve
                std::vector<float> xs, zs, qs, hs;
                float run = 1e30f;
                for (int c : ch) {
                    xs.push_back(X(c % n)); zs.push_back(Z(c / n));
                    qs.push_back(kind == 0 ? M.q[c] : qn[c] * 0.15f);
                    float hc = M.h[c];
                    if (isLake(size_t(c))) hc = std::max(hc, M.lakes[M.lakeId[c]].level);
                    run = std::min(run, hc);
                    hs.push_back(run);
                }
                for (int it = 0; it < 3; ++it) {
                    std::vector<float> a, b, cq, ch2;
                    a.push_back(xs.front()); b.push_back(zs.front()); cq.push_back(qs.front()); ch2.push_back(hs.front());
                    for (size_t j = 0; j + 1 < xs.size(); ++j) {
                        for (float t : {0.25f, 0.75f}) {
                            a.push_back(lerpf(xs[j], xs[j + 1], t)); b.push_back(lerpf(zs[j], zs[j + 1], t));
                            cq.push_back(lerpf(qs[j], qs[j + 1], t)); ch2.push_back(lerpf(hs[j], hs[j + 1], t));
                        }
                    }
                    a.push_back(xs.back()); b.push_back(zs.back()); cq.push_back(qs.back()); ch2.push_back(hs.back());
                    xs.swap(a); zs.swap(b); qs.swap(cq); hs.swap(ch2);
                }
                // meanders: gentle where it's steep, bigger on the flats
                float acc = 0;
                for (size_t j = 0; j < xs.size(); ++j) {
                    size_t a = j > 0 ? j - 1 : j, b = j + 1 < xs.size() ? j + 1 : j;
                    float tx = xs[b] - xs[a], tz = zs[b] - zs[a], tl = std::max(1e-3f, std::hypot(tx, tz));
                    if (j) acc += std::hypot(xs[j] - xs[j - 1], zs[j] - zs[j - 1]);
                    float slope = std::fabs(hs[a] - hs[b]) / tl;
                    float half = 2.25f * std::sqrt(qs[j]);
                    float endFade = sstep(0.f, 60.f, acc) * sstep(0.f, 60.f, float(xs.size() - 1 - j) * 6.f);
                    float amp = (3.f + 2.2f * half) * (1.f - sstep(0.004f, 0.04f, slope)) * endFade;
                    float off = amp * gnoise(acc / (60.f + 10.f * half), float(ci) * 0.37f, seed + 111);
                    xs[j] += tz / tl * off;
                    zs[j] -= tx / tl * off;
                }
                for (size_t j = 0; j < xs.size(); ++j) {
                    RiverPt p;
                    p.x = xs[j]; p.z = zs[j]; p.q = qs[j];
                    p.half = kind == 0 ? std::clamp(2.25f * std::sqrt(qs[j]), 0.5f, 30.f) : std::clamp(3.f + 2.f * std::sqrt(qn[ch[std::min(ch.size() - 1, j * ch.size() / xs.size())]]), 3.f, 14.f);
                    p.depth = kind == 0 ? std::clamp(0.35f * std::pow(qs[j], 0.38f), 0.25f, 3.f) : 0.5f;
                    p.level = hs[j] - (kind == 0 ? 0.25f + 0.05f * std::sqrt(qs[j]) : 0.3f);
                    R.pts.push_back(p);
                }
                M.rivers.push_back(std::move(R));
            }
            // fix up "into" indices: chain index -> river index (short chains were dropped)
        }
        // remap into: chains were indexed before dropping; rebuild by nearest end match
        for (auto& R : M.rivers) {
            if (R.into < 0) continue;
            R.into = -1;
            const RiverPt& e = R.pts.back();
            float best = 30.f;
            for (size_t j = 0; j < M.rivers.size(); ++j) {
                const River& O = M.rivers[j];
                if (&O == &R || O.dry != R.dry) continue;
                for (size_t q = 0; q < O.pts.size(); q += 2) {
                    float d = std::hypot(O.pts[q].x - e.x, O.pts[q].z - e.z);
                    if (d < best) { best = d; R.into = int(j); }
                }
            }
        }
        // a tributary meets the main river at its level (never below it)
        for (int pass = 0; pass < 3; ++pass)
            for (auto& R : M.rivers) {
                if (R.into < 0) continue;
                const River& O = M.rivers[R.into];
                const RiverPt& e = R.pts.back();
                float best = 1e9f, lv = e.level;
                for (const auto& p : O.pts) {
                    float d = std::hypot(p.x - e.x, p.z - e.z);
                    if (d < best) { best = d; lv = p.level; }
                }
                R.pts.back().level = lv;
                for (int j = int(R.pts.size()) - 2; j >= 0; --j) R.pts[j].level = std::max(R.pts[j].level, R.pts[j + 1].level + 0.001f);
            }
        int nr = 0;
        for (auto& R : M.rivers) nr += !R.dry;
        M.stats.rivers = nr;
    }

    // carve valleys around the channels: wide and gentle in soft open country, canyons in
    // the badlands and gorges in the mountains. Every cell takes the profile of its nearest
    // channel point (a nearest-seed distance transform), so the cost is one pass over the grid.
    void carve() {
        struct Seed { float x, z, level, half, depth, q, wall; bool dry; };
        std::vector<Seed> seeds;
        const size_t NN = size_t(n) * n;
        std::vector<int> near(NN, -1);
        for (const River& R : M.rivers)
            for (size_t j = 0; j < R.pts.size(); j += 2) {
                const RiverPt& p = R.pts[j];
                int i = int(std::lround((p.x - x0) / cell)), k = int(std::lround((p.z - z0) / cell));
                if (i < 0 || k < 0 || i >= n || k >= n) continue;
                size_t c = id(i, k);
                if (near[c] >= 0) {   // one seed per cell: the lowest (the bigger river at a confluence)
                    Seed& o = seeds[near[c]];
                    if (p.level >= o.level && !(o.dry && !R.dry)) continue;
                }
                auto w = wAt(p.x, p.z);
                float wall = 0.2f + 0.08f * w[Forest] + 1.3f * w[Badlands] + 0.6f * w[Alpine] + 0.45f * w[Scrub] - 0.08f * w[Steppe] - 0.1f * w[Wetland];
                Seed sd{p.x, p.z, p.level, p.half, p.depth, p.q, std::max(0.08f, wall), R.dry};
                if (near[c] >= 0) seeds[near[c]] = sd;
                else { near[c] = int(seeds.size()); seeds.push_back(sd); }
            }
        auto dist2 = [&](int s, int i, int k) { float dx = X(i) - seeds[s].x, dz = Z(k) - seeds[s].z; return dx * dx + dz * dz; };
        // each channel's valley is a cone (floodplain, then walls rising at its own slope); the
        // land takes the lowest of all the cones that reach it, found by flooding outwards from
        // the channels in order of height (a cell stops the flood once the cone is above it)
        auto profile = [&](const Seed& p, float d) {
            float bankH = p.dry ? 0.3f : 0.6f + p.depth * 0.25f;
            float flood = p.half + (p.dry ? 2.f : 3.f + 0.8f * std::sqrt(p.q));
            float reach = p.dry ? flood + 60.f : std::min(500.f, flood + 160.f / p.wall);
            if (d > reach) return 1e30f;
            float u = std::max(0.f, d - flood);
            float t = p.level + bankH + p.wall * u + 0.0006f * u * u;
            if (d < p.half) t = std::min(t, p.level - p.depth * 0.6f);
            return t;
        };
        std::vector<float> nh(M.h);
        std::vector<float> best(NN, 1e30f);
        std::vector<int> owner(NN, -1);
        using QE = std::pair<float, int>;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
        for (int k = 0; k < n; ++k)
            for (int i = 0; i < n; ++i) {
                size_t c = id(i, k);
                if (near[c] < 0) continue;
                float t = profile(seeds[near[c]], std::sqrt(dist2(near[c], i, k)));
                if (t < best[c]) { best[c] = t; owner[c] = near[c]; pq.push({t, int(c)}); }
            }
        while (!pq.empty()) {
            auto [t, c] = pq.top();
            pq.pop();
            if (t > best[c]) continue;
            if (t >= M.h[c]) continue;   // the cone is above the land here: nothing beyond is carved by it
            nh[c] = std::min(nh[c], t);
            int i = c % n, k = c / n, s = owner[c];
            for (int q = 0; q < 8; ++q) {
                int a = i + DX8[q], b = k + DZ8[q];
                if (a < 0 || b < 0 || a >= n || b >= n) continue;
                size_t nc = id(a, b);
                float tv = profile(seeds[s], std::sqrt(dist2(s, a, b)));
                if (tv < best[nc]) { best[nc] = tv; owner[nc] = s; pq.push({tv, int(nc)}); }
            }
        }
        // lakes stay as they were (their level is set by their basin)
        for (size_t c = 0; c < nh.size(); ++c)
            if (M.lakeId[c] < 0) M.h[c] = nh[c];
        // wash beds and their sand
        M.wash.assign(M.h.size(), 0);
        for (const River& R : M.rivers) {
            if (!R.dry) continue;
            for (const RiverPt& p : R.pts) {
                int i = int(std::lround((p.x - x0) / cell)), k = int(std::lround((p.z - z0) / cell));
                int r = int(std::ceil(p.half / cell));
                for (int dk = -r; dk <= r; ++dk)
                    for (int di = -r; di <= r; ++di) {
                        int a = i + di, b = k + dk;
                        if (a >= 0 && b >= 0 && a < n && b < n) M.wash[id(a, b)] = 1;
                    }
            }
        }
    }

    // wetlands where flat ground gathers water in a humid climate, with marsh pools
    void wetlands(std::vector<float>& wetW) {
        const size_t NN = size_t(n) * n;
        wetW.assign(NN, 0.f);
        parallelFor(n - 2, [&](int kk) {
            int k = kk + 1;
            for (int i = 1; i < n - 1; ++i) {
                size_t c = id(i, k);
                float sx = (M.h[id(i + 1, k)] - M.h[id(i - 1, k)]) / (2 * cell);
                float sz = (M.h[id(i, k + 1)] - M.h[id(i, k - 1)]) / (2 * cell);
                float slope = std::sqrt(sx * sx + sz * sz);
                float twi = std::log((area[c] * cell + 1.f) / std::max(slope, 0.0015f));
                float wv = sstep(11.5f, 14.5f, twi) * sstep(0.75f, 1.15f, ai[c]) * sstep(1.f, 4.f, M.temp[c]);
                wetW[c] = wv;
            }
        });
        for (int pass = 0; pass < 3; ++pass) {
            std::vector<float> s(wetW);
            parallelFor(n - 2, [&](int kk) {
                int k = kk + 1;
                for (int i = 1; i < n - 1; ++i) {
                    float a = 0;
                    for (int dk = -1; dk <= 1; ++dk) for (int di = -1; di <= 1; ++di) a += wetW[id(i + di, k + dk)];
                    s[id(i, k)] = a / 9.f;
                }
            });
            wetW.swap(s);
        }
        // marsh ground is flat: ease it towards its surroundings' mean
        std::vector<float> blur(M.h);
        parallelFor(n - 4, [&](int kk) {
            int k = kk + 2;
            for (int i = 2; i < n - 2; ++i) {
                float a = 0;
                for (int dk = -2; dk <= 2; ++dk) for (int di = -2; di <= 2; ++di) a += M.h[id(i + di, k + dk)];
                blur[id(i, k)] = a / 25.f;
            }
        });
        for (size_t c = 0; c < NN; ++c)
            if (M.lakeId[c] < 0) M.h[c] = lerpf(M.h[c], std::min(M.h[c], blur[c]), wetW[c] * 0.8f);
        // pools: patches of standing water in the wettest parts
        std::vector<uint8_t> seen(NN, 0);
        int ponds = 0;
        for (int k = 2; k < n - 2; ++k)
            for (int i = 2; i < n - 2; ++i) {
                size_t c = id(i, k);
                auto isPool = [&](size_t cc) {
                    int a = int(cc % n), b = int(cc / n);
                    return wetW[cc] > 0.5f && M.lakeId[cc] < 0 && gnoise(X(a) / 140.f, Z(b) / 140.f, seed + 121) > 0.25f - 0.4f * (wetW[cc] - 0.5f);
                };
                if (seen[c] || !isPool(c)) continue;
                std::vector<int> st{int(c)}, cells;
                seen[c] = 1;
                double hs = 0;
                while (!st.empty()) {
                    int cc = st.back();
                    st.pop_back();
                    cells.push_back(cc);
                    hs += M.h[cc];
                    int a = cc % n, b = cc / n;
                    for (int q = 0; q < 8; q += 2) {
                        int na = a + DX8[q], nb = b + DZ8[q];
                        if (na < 1 || nb < 1 || na >= n - 1 || nb >= n - 1) continue;
                        size_t nc = id(na, nb);
                        if (seen[nc] || !isPool(nc)) continue;
                        seen[nc] = 1;
                        st.push_back(int(nc));
                    }
                }
                if (cells.size() < 3 || M.lakes.size() > 30000) continue;
                Lake L;
                L.pond = true;
                L.level = float(hs / cells.size()) + 0.2f;
                double sx = 0, sz = 0;
                int lid = int(M.lakes.size()), cnt = 0;
                for (int cc : cells) {
                    M.h[cc] = std::min(M.h[cc], L.level - 0.35f);
                    M.lakeId[cc] = int16_t(lid);
                    sx += X(cc % n); sz += Z(cc / n);
                    ++cnt;
                }
                L.cx = float(sx / cnt); L.cz = float(sz / cnt);
                L.area = cnt * cell * cell;
                L.depth = 0.35f;
                L.full = true;
                M.lakes.push_back(L);
                ++ponds;
            }
        (void)ponds;
    }

    void surfaceFields(const std::vector<float>& wetW) {
        const size_t NN = size_t(n) * n;
        M.moist.assign(NN, 0.f);
        M.sand.assign(NN, 0.f);
        // distance to water (rivers, lakes) by a two-pass chamfer transform
        std::vector<float> dw(NN, 1e9f);
        for (size_t c = 0; c < NN; ++c)
            if (M.lakeId[c] >= 0 && !M.lakes[M.lakeId[c]].playa) dw[c] = 0;
        for (const River& R : M.rivers) {
            if (R.dry) continue;
            for (const RiverPt& p : R.pts) {
                int i = int(std::lround((p.x - x0) / cell)), k = int(std::lround((p.z - z0) / cell));
                if (i >= 0 && k >= 0 && i < n && k < n) dw[id(i, k)] = 0;
            }
        }
        for (int k = 0; k < n; ++k)
            for (int i = 0; i < n; ++i) {
                float& v = dw[id(i, k)];
                if (i > 0) v = std::min(v, dw[id(i - 1, k)] + cell);
                if (k > 0) v = std::min(v, dw[id(i, k - 1)] + cell);
                if (i > 0 && k > 0) v = std::min(v, dw[id(i - 1, k - 1)] + 1.414f * cell);
                if (i < n - 1 && k > 0) v = std::min(v, dw[id(i + 1, k - 1)] + 1.414f * cell);
            }
        for (int k = n - 1; k >= 0; --k)
            for (int i = n - 1; i >= 0; --i) {
                float& v = dw[id(i, k)];
                if (i < n - 1) v = std::min(v, dw[id(i + 1, k)] + cell);
                if (k < n - 1) v = std::min(v, dw[id(i, k + 1)] + cell);
                if (i < n - 1 && k < n - 1) v = std::min(v, dw[id(i + 1, k + 1)] + 1.414f * cell);
                if (i > 0 && k < n - 1) v = std::min(v, dw[id(i - 1, k + 1)] + 1.414f * cell);
            }
        parallelFor(n, [&](int k) {
            for (int i = 0; i < n; ++i) {
                size_t c = id(i, k);
                float x = X(i), z = Z(k);
                auto w = wAt(x, z);
                float base = std::clamp(0.62f * ai[c], 0.f, 1.f);
                float riparian = std::exp(-dw[c] / (60.f + 120.f * (1.f - std::min(1.f, ai[c]))));
                M.moist[c] = std::clamp(base + 0.55f * riparian * (1.f - base * 0.5f) + 0.5f * wetW[c], 0.f, 1.f);
                float s = sstep(0.05f, 0.45f, w[Dunes] + 0.2f * gnoise(x / 300.f, z / 300.f, seed + 131));
                // sand sheets: flat ground in true desert is sand even between the dune fields
                {
                    float sx = (M.h[id(std::min(i + 1, n - 1), k)] - M.h[id(std::max(i - 1, 0), k)]) / (2 * cell);
                    float sz = (M.h[id(i, std::min(k + 1, n - 1))] - M.h[id(i, std::max(k - 1, 0))]) / (2 * cell);
                    float flat = sstep(0.12f, 0.03f, std::hypot(sx, sz));
                    float desert = sstep(320.f, 160.f, M.precip[c]);
                    s = std::max(s, desert * flat * sstep(-0.3f, 0.3f, gnoise(x / 700.f, z / 700.f, seed + 132)) * 0.9f);
                }
                if (M.wash[c]) s = std::max(s, 0.85f);
                if (M.lakeId[c] >= 0 && M.lakes[M.lakeId[c]].playa) s = std::max(s, 0.6f);
                // sandy beaches on lake shores in dry country
                if (dw[c] < 30.f && ai[c] < 0.7f) s = std::max(s, 0.6f * (1.f - dw[c] / 30.f));
                M.sand[c] = s;
            }
        });
    }
};

}  // namespace

std::shared_ptr<Map> generate(const Params& p, const Progress& progress) {
    auto M = std::make_shared<Map>();
    Gen G(*M, p);
    auto t = std::chrono::steady_clock::now();
    int stage = 0;
    auto lap = [&](const char* what, float frac) {
        auto now = std::chrono::steady_clock::now();
        M->stats.ms[std::min(stage++, 9)] = std::chrono::duration<double, std::milli>(now - t).count();
        t = now;
        if (progress) progress(what, frac);
    };
    if (progress) progress("relief", 0.f);
    G.relief();
    lap("climate", 0.08f);
    G.climate();
    G.biomes(G.macro, false, nullptr);
    lap("landforms", 0.14f);
    G.landforms();
    lap("erosion", 0.3f);
    G.erode();
    lap("hydrology", 0.55f);
    const bool prof = std::getenv("WMAP_PROFILE") != nullptr;
    auto sub = [&, t0 = std::chrono::steady_clock::now()](const char* what) mutable {
        auto now = std::chrono::steady_clock::now();
        if (prof) std::printf("    %-10s %7.0f ms\n", what, std::chrono::duration<double, std::milli>(now - t0).count());
        t0 = now;
    };
    G.flood(); sub("flood");
    G.discharge(); sub("discharge");
    G.rivers(); sub("rivers");
    G.carve(); sub("carve");
    std::vector<float> wetW;
    G.wetlands(wetW); sub("wetlands");
    G.biomes(M->h, true, &wetW);
    G.surfaceFields(wetW); sub("surface");
    for (const auto& L : M->lakes) { M->stats.lakes += !L.playa && !L.pond; M->stats.playas += L.playa; }
    lap("roads", 0.7f);
    G.macro.clear(); G.macro.shrink_to_fit();
    if (p.roads) buildRoads(*M, G.rng, progress);
    lap("index", 0.97f);
    M->index();
    lap("done", 1.f);
    return M;
}

}  // namespace wmap
