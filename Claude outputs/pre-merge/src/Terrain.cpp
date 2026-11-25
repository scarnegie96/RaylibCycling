#include "Terrain.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <future>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <unordered_map>

#include "Course.h"

namespace terrain {
namespace {

// ---------------------------------------------------------------- tuning
constexpr float S = 180.f;          // drainage lattice spacing (m)
constexpr int K_UP = 28;            // catchment search radius (lattice cells, ~5 km)
constexpr float RCELL = 128.f;      // river lookup cells (m)
constexpr float INFL_MAX = 560.f;   // farthest a valley wall reaches (m)
constexpr int FLOOD_R = 7;          // depression filling window (lattice cells)
constexpr float A_WATER = 9.f;      // catchment (lattice cells, ~0.3 km2) that keeps water flowing
constexpr float HW = 3.6f;          // road half width
constexpr float BLEND = 64.f;       // erosion tiles cross-fade over 2 * BLEND
constexpr float MARGIN = 48.f;      // ... and simulate this much further out
constexpr float PAD = BLEND + MARGIN;
constexpr float HS = 80.f * CELL;   // SimpleHydrology's height normalisation (mapscale 80 per cell)

const int DX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
const int DZ[8] = {0, 1, 1, 1, 0, -1, -1, -1};

inline int64_t key(int a, int b) { return (int64_t(a) << 32) ^ int64_t(uint32_t(b)); }
inline float sstep(float a, float b, float x) {
    float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3 - 2 * t);
}
inline float smin(float a, float b, float k) {
    float h = std::max(k - std::fabs(a - b), 0.f) / k;
    return std::min(a, b) - h * h * k * 0.25f;
}
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

// sharded, read-mostly cache
template <class V>
struct Cache {
    static constexpr int NS = 64;
    struct Shard {
        std::shared_mutex m;
        std::unordered_map<int64_t, V> map;
    };
    Shard sh[NS];
    Shard& of(int64_t k) { return sh[(uint64_t(k) * 0x9E3779B97F4A7C15ull) >> 58]; }
    bool get(int64_t k, V& out) {
        Shard& s = of(k);
        std::shared_lock<std::shared_mutex> l(s.m);
        auto it = s.map.find(k);
        if (it == s.map.end()) return false;
        out = it->second;
        return true;
    }
    void put(int64_t k, const V& v) {
        Shard& s = of(k);
        std::unique_lock<std::shared_mutex> l(s.m);
        s.map.emplace(k, v);
    }
    template <class F>
    void eraseIf(F f) {
        for (auto& s : sh) {
            std::unique_lock<std::shared_mutex> l(s.m);
            for (auto it = s.map.begin(); it != s.map.end();) it = f(it->first) ? s.map.erase(it) : std::next(it);
        }
    }
    size_t size() {
        size_t n = 0;
        for (auto& s : sh) { std::shared_lock<std::shared_mutex> l(s.m); n += s.map.size(); }
        return n;
    }
};

struct Node { float x = 0, z = 0, h = 0, ry = 0, rd = 1e4f; };
struct Flow { float W = 0; int8_t recv = -1; };

// one straight piece of a river polyline
struct Piece {
    float ax, az, bx, bz, la, lb;   // ends and water level at each end
    float half, depth, bankW, bankH, rv, infl, flood;
    float vx, vz;                   // current (m/s)
    float minx, maxx, minz, maxz;   // influence box
    bool water;
};

struct Lake { float x, z, level, r, depth; };

struct Edge {
    std::vector<Piece> pieces;
    std::vector<Bridge> bridges;
};

struct Cell {
    std::vector<Piece> pieces;
    std::vector<Lake> lakes;
    std::vector<Bridge> bridges;
};

struct Window {
    int N = 0;
    float x0 = 0, z0 = 0;
    std::vector<float> disp, dis;  // ground change (m), stream discharge (0..1)
};

uint32_t g_impl_serial = 1;

}  // namespace

struct Field::Impl {
    const Course& C;
    uint32_t seed;
    uint32_t serial;
    Cache<Node> nodes;
    Cache<float> fills;
    Cache<int8_t> recvs;
    Cache<std::pair<float, float>> ups;   // catchment, lowest water surface upstream
    Cache<float> lakes;
    Cache<std::shared_ptr<const Edge>> edges;
    Cache<std::shared_ptr<const Cell>> cells;
    std::mutex wm;
    std::unordered_map<int64_t, std::shared_future<std::shared_ptr<const Window>>> windows;
    mutable std::atomic<bool> erosionOn{true};
    mutable std::atomic<int> quality{1};
    std::atomic<int> generation{0};
    std::atomic<double> erodeMs{0};

    explicit Impl(const Course& c) : C(c), seed(c.seed() * 747796405u + 2891336453u), serial(g_impl_serial++) {}

    // ------------------------------------------------------------ landform
    // the smooth upland the rivers drain: the road's heights spread over the land, broad
    // swells, and mountain massifs away from the road (taller for steeper routes)
    float upland(float x, float z, float* dCout = nullptr, float* rugOut = nullptr) const {
        float dC, rug;
        float reg = C.regional(x, z, &dC, &rug);
        float wx = x + (fbm(x * 0.0006f, z * 0.0006f, 3, seed + 11) - 0.5f) * 600.f;
        float wz = z + (fbm(x * 0.0006f + 5.2f, z * 0.0006f - 1.3f, 3, seed + 12) - 0.5f) * 600.f;
        float n = fbm(wx * 0.0011f, wz * 0.0011f, 3, seed) - 0.5f;
        float r = 1.f - std::fabs(2.f * fbm(wx * 0.0007f + 13.f, wz * 0.0007f - 7.f, 3, seed + 99) - 1.f);
        float far = std::min(dC, 1800.f);
        float amp = 4.f + 20.f * rug + far * 0.06f;
        float mountains = sstep(300.f, 1600.f, dC) * r * r * (300.f + 240.f * rug);
        if (dCout) *dCout = dC;
        if (rugOut) *rugOut = rug;
        return reg + n * amp * 2.f + mountains;
    }
    // how hard valley walls rise: gentle by the road on flat routes, steep in the mountains
    float roughness(float x, float z, float dC, float rug) const {
        float v = fbm(x * 0.0013f + 31.f, z * 0.0013f - 17.f, 3, seed + 55);
        return (0.16f + 0.55f * rug + sstep(250.f, 1500.f, dC) * (0.55f + 0.5f * rug)) * (0.55f + 0.9f * v);
    }

    // ------------------------------------------------------------ drainage lattice
    Node node(int i, int k) {
        Node n;
        int64_t kk = key(i, k);
        if (nodes.get(kk, n)) return n;
        float x = (i + 0.5f + (hash2(i, k, seed + 201) - 0.5f) * 0.6f) * S;
        float z = (k + 0.5f + (hash2(i, k, seed + 202) - 0.5f) * 0.6f) * S;
        float px, pz, py;
        float d = C.roadNearest(x, z, px, pz, py);
        const float keep = 34.f;  // nodes (river junctions) never sit on the road
        if (d < keep) {
            float dx = x - px, dz = z - pz;
            if (d < 0.01f) { dx = 1; dz = 0; d = 1; }
            x = px + dx / d * keep;
            z = pz + dz / d * keep;
        }
        float ry;
        float rd = C.roadDistance(x, z, &ry);
        float h = upland(x, z);
        if (rd < 90.f) h = h + (ry - h) * (1.f - sstep(14.f, 90.f, rd));
        // near the road the river runs a few metres below it, so water never floods the road
        if (rd < 120.f) h = std::min(h, ry - 4.5f * (1.f - sstep(60.f, 120.f, rd)) - 0.5f);
        n = {x, z, h, ry, rd};
        nodes.put(kk, n);
        return n;
    }
    // filled height: the lowest barrier on any path from the node out of a window around it
    // (priority flood), so depressions fill to their spill point and drain over the pass.
    // A small slope along the outlet path keeps flats draining. Pure function of the node.
    float fill(int i, int k) {
        float W;
        int64_t kk = key(i, k);
        if (fills.get(kk, W)) return W;
        const int R = FLOOD_R, D = 2 * R + 1;
        std::vector<float> w(size_t(D) * D, 1e30f);
        using QE = std::pair<float, int>;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
        for (int b = 0; b < D; ++b)
            for (int a = 0; a < D; ++a)
                if (a == 0 || b == 0 || a == D - 1 || b == D - 1) {
                    float h = node(i + a - R, k + b - R).h;
                    w[size_t(b) * D + a] = h;
                    pq.push({h, b * D + a});
                }
        W = node(i, k).h;
        const int target = R * D + R;
        while (!pq.empty()) {
            auto [cw, id] = pq.top();
            pq.pop();
            if (cw > w[size_t(id)]) continue;
            if (id == target) { W = cw; break; }
            int a = id % D, b = id / D;
            for (int d = 0; d < 8; ++d) {
                int na = a + DX[d], nb = b + DZ[d];
                if (na <= 0 || nb <= 0 || na >= D - 1 || nb >= D - 1) continue;
                int nid = nb * D + na;
                float nw = std::max(node(i + na - R, k + nb - R).h, cw + 0.002f);
                if (nw < w[size_t(nid)]) { w[size_t(nid)] = nw; pq.push({nw, nid}); }
            }
        }
        fills.put(kk, W);
        return W;
    }
    int recv(int i, int k) {
        int8_t r;
        int64_t kk = key(i, k);
        if (recvs.get(kk, r)) return r;
        Node a = node(i, k);
        float Wa = fill(i, k);
        float best = 0;
        r = -1;
        for (int d = 0; d < 8; ++d) {
            Node b = node(i + DX[d], k + DZ[d]);
            float Wb = fill(i + DX[d], k + DZ[d]);
            if (Wb >= Wa) continue;
            float s = (Wa - Wb) / std::hypot(b.x - a.x, b.z - a.z);
            if (s > best) { best = s; r = int8_t(d); }
        }
        recvs.put(kk, r);
        return r;
    }
    // the lake a depression holds: filled no deeper than a fraction of the depression, so
    // big basins keep a modest lake and their outflow cuts a gorge through the rim instead
    float lakeLevel(int i, int k) {
        float lv;
        int64_t kk = key(i, k);
        if (lakes.get(kk, lv)) return lv;
        float W = fill(i, k);
        float floorH = node(i, k).h;
        int a = i, b = k;
        for (int step = 0; step < 48; ++step) {
            Node n = node(a, b);
            if (fill(a, b) - n.h < 0.05f) break;
            floorH = std::min(floorH, n.h);
            int r = recv(a, b);
            if (r < 0) break;
            a += DX[r]; b += DZ[r];
        }
        float depth = W - floorH;
        lv = std::min(W, floorH + std::min(depth, 1.6f + 0.22f * depth));
        lakes.put(kk, std::min(lv, 12.f + floorH));
        return std::min(lv, 12.f + floorH);
    }
    // water surface the node holds on its own: the ground, or its lake
    float surfaceLevel(int i, int k) {
        float h = node(i, k).h;
        if (fill(i, k) - h < 0.05f) return h;
        return std::max(h, lakeLevel(i, k));
    }
    // catchment (lattice cells) and the lowest water surface upstream, both counted within
    // K_UP - still a pure function of the node
    std::pair<float, float> upstream(int i, int k) {
        std::pair<float, float> u;
        int64_t kk = key(i, k);
        if (ups.get(kk, u)) return u;
        std::vector<std::pair<int, int>> st{{i, k}};
        int count = 1;
        float lo = surfaceLevel(i, k);
        while (!st.empty()) {
            auto [a, b] = st.back();
            st.pop_back();
            for (int d = 0; d < 8; ++d) {
                int na = a + DX[d], nb = b + DZ[d];
                if (std::abs(na - i) > K_UP || std::abs(nb - k) > K_UP) continue;
                int r = recv(na, nb);
                if (r >= 0 && na + DX[r] == a && nb + DZ[r] == b) {
                    ++count;
                    lo = std::min(lo, surfaceLevel(na, nb));
                    st.push_back({na, nb});
                }
            }
        }
        u = {float(count), lo};
        ups.put(kk, u);
        return u;
    }
    float area(int i, int k) { return upstream(i, k).first; }
    // river surface at a node: never above anything upstream (breaching), cut a little in
    float riverLevel(int i, int k) {
        auto [A, lo] = upstream(i, k);
        return std::min(surfaceLevel(i, k), lo) - incision(A);
    }
    static float incision(float A) { return 0.35f + 0.11f * std::sqrt(A); }
    static float riverHalf(float A) { return std::clamp(0.62f * std::sqrt(A), 1.0f, 32.f); }
    static float riverDepth(float A) { return std::clamp(0.28f * std::pow(A, 0.38f), 0.35f, 3.6f); }
    static float valleyR(float A) { return std::min(36.f + 6.f * std::sqrt(A), 280.f); }

    std::shared_ptr<const Edge> edge(int i, int k) {
        std::shared_ptr<const Edge> e;
        int64_t kk = key(i, k);
        if (edges.get(kk, e)) return e;
        auto out = std::make_shared<Edge>();
        int r = recv(i, k);
        if (r >= 0) buildEdge(i, k, r, *out);
        edges.put(kk, out);
        return out;
    }

    void buildEdge(int i, int k, int r, Edge& E) {
        const int ri = i + DX[r], rk = k + DZ[r];
        Node n = node(i, k), m = node(ri, rk);
        float A = area(i, k), Am = area(ri, rk);
        // Catmull-Rom through the straightest upstream donor and the next node downstream,
        // so the river is smooth through every junction
        float p0x = 2 * n.x - m.x, p0z = 2 * n.z - m.z;
        {
            float bestDot = -2;
            float ux = n.x - m.x, uz = n.z - m.z, ul = std::hypot(ux, uz);
            for (int d = 0; d < 8; ++d) {
                int ai = i + DX[d], ak = k + DZ[d];
                int ar = recv(ai, ak);
                if (ar < 0 || ai + DX[ar] != i || ak + DZ[ar] != k) continue;
                Node a = node(ai, ak);
                float vx = a.x - n.x, vz = a.z - n.z, vl = std::hypot(vx, vz);
                float dot = (vx * ux + vz * uz) / std::max(1e-3f, vl * ul);
                if (dot > bestDot) { bestDot = dot; p0x = a.x; p0z = a.z; }
            }
        }
        float p3x = 2 * m.x - n.x, p3z = 2 * m.z - n.z;
        int r2 = recv(ri, rk);
        if (r2 >= 0) { Node q = node(ri + DX[r2], rk + DZ[r2]); p3x = q.x; p3z = q.z; }

        const bool water = A >= A_WATER;
        const float half = water ? riverHalf(A) : std::clamp(0.35f * std::sqrt(A), 0.6f, 2.f);
        const float depth = water ? riverDepth(A) : 0.25f + 0.08f * std::sqrt(A);
        const float bankW = water ? 2.f + half * 0.45f : 1.f;
        const float bankH = water ? 0.35f + depth * 0.15f : 0.f;
        const float floodW = water ? half + bankW + 0.9f * std::sqrt(A) : half;
        const float rv = valleyR(A);
        float la = riverLevel(i, k), lb = riverLevel(ri, rk);
        (void)Am;
        {   // rivers leave and enter lakes at the lake's surface
            Lake LA, LB;
            if (lakeAt(i, k, LA)) la = std::max(la, LA.level);
            if (lakeAt(ri, rk, LB)) lb = std::max(lb, LB.level);
        }

        if (lb > la - 0.05f) lb = la - 0.05f;

        // sample the curve, meander it, keep it off the road
        const int M = water ? 12 : 5;
        const float len = std::hypot(m.x - n.x, m.z - n.z);
        const float flat = 1.f - sstep(0.01f, 0.08f, (la - lb) / std::max(1.f, len));
        const float amp = std::min(0.16f * len, 4.f + 1.6f * half) * (0.4f + 0.6f * flat) * (hash2(i * 3 + r, k, seed + 301) * 2 - 1);
        const int waves = hash2(i, k * 7 + r, seed + 302) > 0.5f ? 2 : 1;
        std::vector<float> xs(M + 1), zs(M + 1), ls(M + 1);
        for (int s = 0; s <= M; ++s) {
            float t = float(s) / M, t2 = t * t, t3 = t2 * t;
            auto cr = [&](float a, float b, float c, float d) {
                return 0.5f * ((2 * b) + (-a + c) * t + (2 * a - 5 * b + 4 * c - d) * t2 + (-a + 3 * b - 3 * c + d) * t3);
            };
            float x = cr(p0x, n.x, m.x, p3x), z = cr(p0z, n.z, m.z, p3z);
            float tx = m.x - n.x, tz = m.z - n.z;
            float off = amp * std::sin(3.14159265f * t * waves);
            x += -tz / std::max(1e-3f, len) * off;
            z += tx / std::max(1e-3f, len) * off;
            if (s > 0 && s < M) {
                float px, pz, py;
                float d = C.roadNearest(x, z, px, pz, py);
                float keep = HW + half + bankW + 7.f;
                if (d < keep) {
                    float dx = x - px, dz = z - pz;
                    if (d < 0.01f) { dx = -tz; dz = tx; d = std::max(1e-3f, len); }
                    x = px + dx / d * keep;
                    z = pz + dz / d * keep;
                }
            }
            xs[s] = x; zs[s] = z;
            ls[s] = lerpf(la, lb, t);
        }
        // where it crosses the road: keep the water well under the deck, note a bridge
        if (water && std::min(n.rd, m.rd) < 260.f) {
            for (int s = 0; s < M; ++s) {
                float sl = std::hypot(xs[s + 1] - xs[s], zs[s + 1] - zs[s]);
                int steps = std::max(1, int(sl / 2.f));
                float bestD = 1e9f, bx = 0, bz = 0, by = 0, bt = 0;
                for (int q = 0; q <= steps; ++q) {
                    float t = float(q) / steps;
                    float x = lerpf(xs[s], xs[s + 1], t), z = lerpf(zs[s], zs[s + 1], t);
                    float px, pz, py;
                    float d = C.roadNearest(x, z, px, pz, py);
                    if (d < bestD) { bestD = d; bx = px; bz = pz; by = py; bt = t; }
                }
                if (bestD > HW * 0.8f) continue;
                float cap = by - 3.2f;
                float arcC = s + bt;
                for (int q = 0; q <= M; ++q) {
                    float dist = std::fabs(q - arcC) * len / M;
                    ls[q] = std::min(ls[q], cap + 0.004f * dist);
                }
                Bridge b;
                b.x = bx; b.z = bz; b.roadY = by; b.level = cap;
                b.width = 2 * half;
                b.span = 2 * (half + bankW) + 8.f;
                E.bridges.push_back(b);
            }
        }
        for (int s = 1; s <= M; ++s)  // keep it flowing downhill after the caps
            ls[s] = std::min(ls[s], ls[s - 1] - 0.002f);
        float slope = (ls[0] - ls[M]) / std::max(1.f, len);
        float speed = std::clamp(std::sqrt(std::max(slope, 0.f)) * 9.f, 0.25f, 2.6f);
        float infl = std::min(INFL_MAX, 170.f + 1.4f * rv);
        for (int s = 0; s < M; ++s) {
            Piece p;
            p.ax = xs[s]; p.az = zs[s]; p.bx = xs[s + 1]; p.bz = zs[s + 1];
            p.la = ls[s]; p.lb = ls[s + 1];
            p.flood = floodW;
            p.half = half; p.depth = depth; p.bankW = bankW; p.bankH = bankH; p.rv = rv; p.infl = infl;
            float dl = std::max(1e-3f, std::hypot(p.bx - p.ax, p.bz - p.az));
            p.vx = (p.bx - p.ax) / dl * speed; p.vz = (p.bz - p.az) / dl * speed;
            p.minx = std::min(p.ax, p.bx) - infl; p.maxx = std::max(p.ax, p.bx) + infl;
            p.minz = std::min(p.az, p.bz) - infl; p.maxz = std::max(p.az, p.bz) + infl;
            p.water = water;
            E.pieces.push_back(p);
        }
    }

    // a lake wherever the depression fill is deep enough; its surface is the spill height
    // a lake where a depression is deep enough: an explicit basin around the node with a
    // noisy round shore, its surface the (capped) fill level
    bool lakeAt(int i, int k, Lake& L) {
        Node n = node(i, k);
        float W = fill(i, k);
        if (W - n.h < 6.f) return false;
        for (int d = 0; d < 8; ++d)  // one lake per hollow, at its lowest node
            if (node(i + DX[d], k + DZ[d]).h < n.h) return false;
        if (hash2(i, k, seed + 401) > 0.8f) return false;  // not every hollow holds water
        float level = lakeLevel(i, k);
        float px, pz, py;
        float d = C.roadNearest(n.x, n.z, px, pz, py);
        L.r = std::clamp(40.f + 4.f * std::sqrt(area(i, k)) + 1.2f * (W - n.h), 45.f, 140.f);
        if (d < L.r * 1.6f + 25.f) level = std::min(level, py - 2.5f);
        if (d < L.r + 20.f) return false;  // never lap at the road
        if (level < n.h + 0.8f) return false;
        // no lake perched beside a lower, unrelated river (it would spill over its banks);
        // its own outflow is fine
        {
            int ci[4] = {i, 0, 0, 0}, ck[4] = {k, 0, 0, 0};
            for (int q = 1; q < 4; ++q) {
                int r = recv(ci[q - 1], ck[q - 1]);
                ci[q] = r >= 0 ? ci[q - 1] + DX[r] : ci[q - 1];
                ck[q] = r >= 0 ? ck[q - 1] + DZ[r] : ck[q - 1];
            }
            for (int dk = -2; dk <= 2; ++dk)
                for (int di = -2; di <= 2; ++di) {
                    int mi = i + di, mk = k + dk;
                    bool chain = false;
                    for (int q = 0; q < 4; ++q) chain |= mi == ci[q] && mk == ck[q];
                    if (chain || area(mi, mk) < A_WATER) continue;
                    Node m = node(mi, mk);
                    if (std::hypot(m.x - n.x, m.z - n.z) < L.r + 60.f && riverLevel(mi, mk) < level - 0.5f) return false;
                }
        }
        L.x = n.x; L.z = n.z; L.level = level;
        L.depth = std::max(2.f, level - n.h + 1.5f);
        return true;
    }

    std::shared_ptr<const Cell> cell(int cx, int cz) {
        std::shared_ptr<const Cell> c;
        int64_t kk = key(cx, cz);
        if (cells.get(kk, c)) return c;
        auto out = std::make_shared<Cell>();
        float x0 = cx * RCELL, z0 = cz * RCELL, x1 = x0 + RCELL, z1 = z0 + RCELL;
        int i0 = int(std::floor((x0 - INFL_MAX) / S)) - 1, i1 = int(std::floor((x1 + INFL_MAX) / S)) + 1;
        int k0 = int(std::floor((z0 - INFL_MAX) / S)) - 1, k1 = int(std::floor((z1 + INFL_MAX) / S)) + 1;
        for (int k = k0; k <= k1; ++k)
            for (int i = i0; i <= i1; ++i) {
                auto e = edge(i, k);
                for (const Piece& p : e->pieces)
                    if (p.maxx >= x0 && p.minx <= x1 && p.maxz >= z0 && p.minz <= z1) out->pieces.push_back(p);
                for (const Bridge& b : e->bridges)
                    if (b.x >= x0 - 64 && b.x <= x1 + 64 && b.z >= z0 - 64 && b.z <= z1 + 64) out->bridges.push_back(b);
                Lake L;
                if (lakeAt(i, k, L) && L.x + L.r * 1.5f >= x0 && L.x - L.r * 1.5f <= x1 && L.z + L.r * 1.5f >= z0 && L.z - L.r * 1.5f <= z1)
                    out->lakes.push_back(L);
            }
        cells.put(kk, out);
        return out;
    }

    std::shared_ptr<const Cell> cellAt(float x, float z) {
        int cx = int(std::floor(x / RCELL)), cz = int(std::floor(z / RCELL));
        // tiny per-thread memo: consecutive samples nearly always hit the same cell
        struct Memo { uint32_t serial = 0; int gen = -1; int64_t k = 0; std::shared_ptr<const Cell> c; };
        thread_local Memo memo[4];
        thread_local int next = 0;
        int64_t kk = key(cx, cz);
        int g = generation.load(std::memory_order_relaxed);
        for (auto& m : memo)
            if (m.serial == serial && m.gen == g && m.k == kk && m.c) return m.c;
        auto c = cell(cx, cz);
        Memo& m = memo[next++ & 3];
        m.serial = serial; m.gen = g; m.k = kk; m.c = c;
        return c;
    }

    // ------------------------------------------------------------ macro sample
    Sample macroSample(float x, float z) {
        Sample s;
        auto c = cellAt(x, z);
        float dC, rug;
        const float U = upland(x, z, &dC, &rug);
        const float rough = roughness(x, z, dC, rug);
        // landform: every valley's floor plus its rising walls, soft-minimised, so ridges form
        // where the walls of neighbouring valleys meet
        float land = 1e30f;
        float bestF = 0, forced = 0, water = -1e9f, fvx = 0, fvz = 0, bank = 0, bankLevel = -1e9f, wW = 0;
        const float kRidge = 4.f + 10.f * rough;
        // lakes first: inside one, rivers lose their banks (they flow through it)
        const Lake* lake = nullptr;
        float lakeU = 9.f;
        for (const Lake& L : c->lakes) {
            float dx = x - L.x, dz = z - L.z;
            float wob = 1.f + 0.22f * (vnoise(std::atan2(dz, dx) * 1.6f + L.x * 0.01f, L.z * 0.01f, seed + 77) - 0.5f) * 2.f;
            float u = std::sqrt(dx * dx + dz * dz) / (L.r * wob);
            if (u < lakeU) { lakeU = u; lake = &L; }
        }
        const float inLake = lake ? 1.f - sstep(0.85f, 1.05f, lakeU) : 0.f;
        for (const Piece& p : c->pieces) {
            if (x < p.minx || x > p.maxx || z < p.minz || z > p.maxz) continue;
            float ex = p.bx - p.ax, ez = p.bz - p.az;
            float t = std::clamp(((x - p.ax) * ex + (z - p.az) * ez) / std::max(1e-6f, ex * ex + ez * ez), 0.f, 1.f);
            float qx = p.ax + ex * t - x, qz = p.az + ez * t - z;
            float dd = std::sqrt(qx * qx + qz * qz);
            if (dd > p.infl) continue;
            float L = lerpf(p.la, p.lb, t);
            float u = std::max(0.f, dd - p.flood);
            float wall = 0.9f * rough * (std::sqrt(p.rv * p.rv + u * u) - p.rv) + 0.03f * u;
            float v = L + p.bankH + wall + sstep(0.72f * p.infl, p.infl, dd) * 600.f;
            land = land > 1e29f ? v : smin(land, v, kRidge);
            if (p.water) {
                // parabolic bed meeting a straight bank at the waterline (no curvature there, so
                // the shoreline interpolates smoothly across the mesh), easing onto the bank top
                float u2 = dd / p.half;
                float bs = std::min(2.f * p.depth / p.half, 1.2f);
                float ch = dd < p.half ? L - p.depth * (1.f - u2 * u2) : L + smin(bs * (dd - p.half), p.bankH, 0.25f);
                float f = (1.f - sstep(p.half + p.bankW * 0.6f, p.half + p.bankW + 3.f, dd)) * (1.f - inLake);
                if (f > bestF + 1e-3f || (f > 0.99f && bestF > 0.99f && ch < forced)) {
                    bestF = f; forced = ch;
                    if (dd < p.half + p.bankW) { water = L; fvx = p.vx; fvz = p.vz; wW = 2 * p.half; }
                }
                bank = std::max(bank, 1.f - sstep(p.half, p.half + p.bankW + 14.f, dd));
                if (dd < p.flood + 14.f) bankLevel = std::max(bankLevel, L);
            }
        }
        if (land > 1e29f) land = U;
        land += (fbm(x * 0.011f, z * 0.011f, 3, seed + 7) - 0.5f) * (1.f + 3.f * rough);
        // the road's formation (cuttings, embankments) blends the land into the road; river
        // channels keep their shape right up to it, and pass under it at bridges
        float ry;
        s.road = C.roadDistance(x, z, &ry);
        float protect = 1.f;
        if (s.road < 90.f) {
            float blend = sstep(14.f, 80.f, s.road);
            float carved = ry - 0.30f - sstep(5.f, 9.f, s.road) * (1.f - sstep(10.f, 16.f, s.road)) * 0.35f;
            land = carved + (land - carved) * blend;
            protect = sstep(HW + 2.f, HW + 10.f, s.road);
            for (const Bridge& b : c->bridges) {
                float db = std::hypot(x - b.x, z - b.z);
                protect = std::max(protect, 1.f - sstep(b.span * 0.5f - 2.f, b.span * 0.5f + 4.f, db));
            }
        }
        float h = lerpf(land, forced, bestF * protect);
        // the lake basin: a bowl below the surface, a low rim around it so the water is held,
        // and the shore wherever the land rises through the surface in between
        float lakeLevel = -1e9f;
        if (lake && lakeU < 1.5f && protect > 0.99f) {
            const Lake& L = *lake;
            float bowl = L.level - L.depth * (1.f - lakeU * lakeU * 0.75f);
            h = lerpf(h, smin(h, bowl, 2.f), 1.f - sstep(0.95f, 1.2f, lakeU));
            float rim = sstep(0.95f, 1.08f, lakeU) * (1.f - sstep(1.25f, 1.5f, lakeU)) * (1.f - bestF);
            h = std::max(h, lerpf(h, L.level + 0.35f, rim));
            if (lakeU < 1.12f && h < L.level && L.level > water) { water = L.level; fvx = fvz = 0; wW = 1e3f; }
            bank = std::max(bank, 1.f - sstep(0.85f, 1.3f, lakeU));
            lakeLevel = L.level;
        }
        s.h = h;
        if (protect > 0.5f) s.bankLevel = std::max(bankLevel, lakeLevel);
        if (protect > 0.5f && water > -1e8f) {
            s.water = water;
            s.flowX = fvx; s.flowZ = fvz;
            s.waterW = wW;
        }
        s.bank = bank * protect;
        return s;
    }

    // ------------------------------------------------------------ particle erosion
    std::shared_ptr<const Window> erodeWindow(int wx, int wz) {
        auto t0 = std::chrono::steady_clock::now();
        auto W = std::make_shared<Window>();
        const int N = int((TILE + 2 * PAD) / CELL) + 1;
        W->N = N;
        W->x0 = wx * TILE - PAD;
        W->z0 = wz * TILE - PAD;
        const size_t NN = size_t(N) * N;
        std::vector<float> h(NN), h0(NN), roadD(NN);
        std::vector<uint8_t> fixed(NN), sink(NN);
        for (int k = 0; k < N; ++k)
            for (int i = 0; i < N; ++i) {
                Sample m = macroSample(W->x0 + i * CELL, W->z0 + k * CELL);
                size_t id = size_t(k) * N + i;
                h0[id] = h[id] = m.h / HS;
                roadD[id] = m.road;
                fixed[id] = m.road < HW + 4.5f || m.bank > 0.45f;  // road formation, river banks
                sink[id] = m.water > m.h - 0.05f;
                if (sink[id]) fixed[id] = 1;
            }
        std::vector<float> dis(NN, 0.f), mx(NN, 0.f), mz(NN, 0.f), tdis(NN), tmx(NN), tmz(NN);

        // SimpleHydrology's droplet (water.h), run on this tile
        const float evapRate = 0.001f, depositionRate = 0.1f, minVol = 0.01f, entrainment = 10.f;
        const float gravity = 1.f, momentumTransfer = 1.f, maxdiff = 0.01f, settling = 0.8f;
        const int maxAge = 500;
        const int q = std::clamp(quality.load(), 0, 2);
        const int rounds = q == 0 ? 6 : q == 1 ? 10 : 14;
        const float lrate = 0.25f;
        const float spawnP = 0.22f;
        // SimpleHydrology drops ~0.2 % of cells per cycle; we drop spawnP per round, so scale
        // the tracked discharge / momentum back to its units
        const float trackScale = 0.0022f / spawnP;
        auto at = [&](int i, int k) -> float& { return h[size_t(k) * N + i]; };
        auto oob = [&](float x, float z) { return x < 0 || z < 0 || x >= N - 1 || z >= N - 1; };
        auto hAt = [&](float x, float z) {
            int i = int(x), k = int(z);
            float fx = x - i, fz = z - k;
            return lerpf(lerpf(at(i, k), at(i + 1, k), fx), lerpf(at(i, k + 1), at(i + 1, k + 1), fx), fz);
        };
        auto normal = [&](int i, int k, float& nx, float& nz) {
            int l = std::max(i - 1, 0), r = std::min(i + 1, N - 1), d = std::max(k - 1, 0), u = std::min(k + 1, N - 1);
            float gx = (at(l, k) - at(r, k)) * 80.f / float(r - l), gz = (at(i, d) - at(i, u)) * 80.f / float(u - d);
            float inv = 1.f / std::sqrt(gx * gx + 1.f + gz * gz);
            nx = gx * inv; nz = gz * inv;
        };
        auto cascade = [&](int i, int k) {
            if (i < 1 || k < 1 || i >= N - 1 || k >= N - 1) return;
            size_t c = size_t(k) * N + i;
            if (fixed[c]) return;
            struct P { int i, k; float h, d; };
            std::array<P, 8> sn;
            for (int d = 0; d < 8; ++d) sn[d] = {i + DX[d], k + DZ[d], at(i + DX[d], k + DZ[d]), (DX[d] && DZ[d]) ? 1.41421f : 1.f};
            std::sort(sn.begin(), sn.end(), [](const P& a, const P& b) { return a.h < b.h; });
            for (const P& p : sn) {
                size_t nidx = size_t(p.k) * N + p.i;
                if (fixed[nidx]) continue;
                float diff = h[c] - p.h;
                if (diff == 0) continue;
                float excess = std::fabs(diff) - p.d * maxdiff;
                if (excess <= 0) continue;
                float transfer = settling * excess / 2.f;
                if (diff > 0) { h[c] -= transfer; h[nidx] += transfer; }
                else { h[c] += transfer; h[nidx] -= transfer; }
            }
        };
        const int gi0 = int(std::lround(W->x0 / CELL)), gk0 = int(std::lround(W->z0 / CELL));
        for (int round = 0; round < rounds; ++round) {
            std::fill(tdis.begin(), tdis.end(), 0.f);
            std::fill(tmx.begin(), tmx.end(), 0.f);
            std::fill(tmz.begin(), tmz.end(), 0.f);
            for (int k = 1; k < N - 1; ++k)
                for (int i = 1; i < N - 1; ++i) {
                    // spawn points come from world cells, so overlapping tiles drop the same rain
                    int gi = gi0 + i, gk = gk0 + k;
                    if (hash2(gi, gk, seed + 1000 + round * 7919) > spawnP) continue;
                    if (fixed[size_t(k) * N + i]) continue;
                    float px = i + hash2(gi, gk, seed + 2000 + round) * 0.98f, pz = k + hash2(gi, gk, seed + 3000 + round) * 0.98f;
                    float sx = 0, sz = 0, vol = 1, sed = 0;
                    for (int age = 0;; ++age) {
                        int ix = int(px), iz = int(pz);
                        size_t id = size_t(iz) * N + ix;
                        if (age > maxAge || vol < minVol) { if (!fixed[id]) h[id] += sed; break; }
                        float nx, nz;
                        normal(ix, iz, nx, nz);
                        sx += gravity * nx / vol;
                        sz += gravity * nz / vol;
                        float fx = mx[id], fz = mz[id];
                        float fl = std::sqrt(fx * fx + fz * fz), sl = std::sqrt(sx * sx + sz * sz);
                        if (fl > 0 && sl > 0) {
                            float dot = (fx * sx + fz * sz) / (fl * sl);
                            float k2 = momentumTransfer * dot / (vol + dis[id]);
                            sx += k2 * fx; sz += k2 * fz;
                        }
                        sl = std::sqrt(sx * sx + sz * sz);
                        if (sl > 0) { sx *= 1.41421f / sl; sz *= 1.41421f / sl; }
                        px += sx; pz += sz;
                        tdis[id] += vol;
                        tmx[id] += vol * sx;
                        tmz[id] += vol * sz;
                        bool out = oob(px, pz);
                        float h2 = out ? h[id] - 0.002f : hAt(px, pz);
                        float ceq = (1.f + entrainment * std::erf(0.4f * dis[id])) * (h[id] - h2);
                        if (ceq < 0) ceq = 0;
                        float cdiff = ceq - sed;
                        sed += depositionRate * cdiff;
                        if (!fixed[id]) h[id] -= depositionRate * cdiff;
                        sed /= (1.f - evapRate);
                        vol *= (1.f - evapRate);
                        if (out) break;
                        size_t nid = size_t(int(pz)) * N + int(px);
                        if (sink[nid]) break;  // the river carries it away
                        cascade(int(px), int(pz));
                    }
                }
            for (size_t id = 0; id < NN; ++id) {
                dis[id] = (1.f - lrate) * dis[id] + lrate * tdis[id] * trackScale;
                mx[id] = (1.f - lrate) * mx[id] + lrate * tmx[id] * trackScale;
                mz[id] = (1.f - lrate) * mz[id] + lrate * tmz[id] * trackScale;
            }
        }
        W->disp.resize(NN);
        W->dis.resize(NN);
        for (size_t id = 0; id < NN; ++id) {
            float dm = (h[id] - h0[id]) * HS;
            dm = std::clamp(dm, -30.f, 14.f) * sstep(HW + 4.f, HW + 14.f, roadD[id]);
            W->disp[id] = fixed[id] ? 0.f : dm;
            W->dis[id] = std::erf(0.4f * dis[id]);
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        double cur = erodeMs.load();
        while (!erodeMs.compare_exchange_weak(cur, cur + ms)) {}
        return W;
    }

    std::shared_ptr<const Window> window(int wx, int wz, bool compute) {
        int64_t kk = key(wx, wz);
        std::shared_future<std::shared_ptr<const Window>> f;
        std::promise<std::shared_ptr<const Window>> pr;
        bool mine = false;
        {
            std::lock_guard<std::mutex> lk(wm);
            auto it = windows.find(kk);
            if (it != windows.end()) f = it->second;
            else if (compute) {
                f = pr.get_future().share();
                windows.emplace(kk, f);
                mine = true;
            } else return nullptr;
        }
        if (mine) {
            pr.set_value(erodeWindow(wx, wz));
            return f.get();
        }
        if (compute) return f.get();
        if (f.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return nullptr;
        return f.get();
    }

    std::shared_ptr<const Window> windowMemo(int wx, int wz) {
        struct Memo { uint32_t serial = 0; int gen = -1; int64_t k = 0; std::shared_ptr<const Window> w; };
        thread_local Memo memo[8];
        thread_local int next = 0;
        int64_t kk = key(wx, wz);
        int g = generation.load(std::memory_order_relaxed);
        for (auto& m : memo)
            if (m.serial == serial && m.gen == g && m.k == kk && m.w) return m.w;
        auto w = window(wx, wz, false);
        if (!w) return nullptr;
        Memo& m = memo[next++ & 7];
        m.serial = serial; m.gen = g; m.k = kk; m.w = w;
        return w;
    }

    // partition of unity across the tile aprons: weights of neighbouring tiles sum to one
    static float tileWeight(float u) {  // u = position within the tile's own core (0..TILE)
        if (u < BLEND) return sstep(0.f, 1.f, (u + BLEND) / (2 * BLEND));
        if (u > TILE - BLEND) return 1.f - sstep(0.f, 1.f, (u - TILE + BLEND) / (2 * BLEND));
        return 1.f;
    }

    bool fine(float x, float z, float& disp, float& dis) {
        int wx0 = int(std::floor((x - BLEND) / TILE)), wx1 = int(std::floor((x + BLEND) / TILE));
        int wz0 = int(std::floor((z - BLEND) / TILE)), wz1 = int(std::floor((z + BLEND) / TILE));
        disp = dis = 0;
        for (int wz = wz0; wz <= wz1; ++wz)
            for (int wx = wx0; wx <= wx1; ++wx) {
                float w = tileWeight(x - wx * TILE) * tileWeight(z - wz * TILE);
                if (w <= 0) continue;
                auto W = windowMemo(wx, wz);
                if (!W) { disp = dis = 0; return false; }
                float fx = (x - W->x0) / CELL, fz = (z - W->z0) / CELL;
                int i = std::clamp(int(fx), 0, W->N - 2), k = std::clamp(int(fz), 0, W->N - 2);
                float tx = std::clamp(fx - i, 0.f, 1.f), tz = std::clamp(fz - k, 0.f, 1.f);
                auto bil = [&](const std::vector<float>& g) {
                    size_t a = size_t(k) * W->N + i;
                    return lerpf(lerpf(g[a], g[a + 1], tx), lerpf(g[a + W->N], g[a + W->N + 1], tx), tz);
                };
                disp += w * bil(W->disp);
                dis += w * bil(W->dis);
            }
        return true;
    }

    Sample full(float x, float z) {
        Sample s = macroSample(x, z);
        if (!erosionOn.load(std::memory_order_relaxed)) return s;
        float disp, dis;
        if (!fine(x, z, disp, dis)) return s;
        float before = s.h;
        disp *= 1.f - sstep(0.15f, 0.5f, s.bank);  // banks keep their shape
        s.h += disp;
        if (s.water > -1e8f && s.h > s.water - 0.15f && s.bank > 0.5f) s.h = std::min(s.h, s.water - 0.15f + (s.h - s.water + 0.15f) * 0.3f);
        // dry ground by a river or lake never erodes below its water (no spilled sheets)
        if (s.water < s.h && s.bankLevel > -1e8f && before >= s.bankLevel + 0.1f) s.h = std::max(s.h, s.bankLevel + 0.1f);
        s.wet = sstep(0.45f, 0.985f, dis);
        s.sediment = std::clamp(disp * 0.7f, 0.f, 1.f);
        s.scour = std::clamp(-disp * 0.28f, 0.f, 1.f);
        return s;
    }
};

// ---------------------------------------------------------------- public
Field::Field(const Course& c) : d_(std::make_unique<Impl>(c)) {}
Field::~Field() = default;

Sample Field::macro(float x, float z) const { return d_->macroSample(x, z); }
Sample Field::sample(float x, float z) const { return d_->full(x, z); }
float Field::height(float x, float z, float* roadDist) const {
    Sample s = d_->full(x, z);
    if (roadDist) *roadDist = s.road;
    return s.h;
}

void Field::prepareTile(int tx, int tz) const {
    if (!d_->erosionOn.load()) return;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx) d_->window(tx + dx, tz + dz, true);
}

bool Field::tileReady(int tx, int tz) const {
    for (int dz = -1; dz <= 1; ++dz)
        for (int dx = -1; dx <= 1; ++dx)
            if (!d_->window(tx + dx, tz + dz, false)) return false;
    return true;
}

std::vector<Bridge> Field::bridgesNear(float x, float z, float radius) const {
    std::vector<Bridge> out;
    int c0 = int(std::floor((x - radius) / RCELL)), c1 = int(std::floor((x + radius) / RCELL));
    int k0 = int(std::floor((z - radius) / RCELL)), k1 = int(std::floor((z + radius) / RCELL));
    for (int k = k0; k <= k1; ++k)
        for (int i = c0; i <= c1; ++i)
            for (const Bridge& b : d_->cell(i, k)->bridges) {
                if (std::hypot(b.x - x, b.z - z) > radius) continue;
                bool dup = false;
                for (const Bridge& o : out) dup |= std::hypot(o.x - b.x, o.z - b.z) < 1.f;
                if (!dup) out.push_back(b);
            }
    return out;
}

void Field::trim(float x, float z, float keep) const {
    auto far = [&](int64_t k, float size, float off) {
        int a = int(k >> 32), b = int(int32_t(uint32_t(k & 0xFFFFFFFF)));
        float cx = (a + off) * size, cz = (b + off) * size;
        return std::hypot(cx - x, cz - z) > keep;
    };
    // decode: key() xors the low word, so recover b from the low 32 bits of (k ^ (a << 32))
    auto farKey = [&](int64_t k, float size, float off) {
        int a = int(k >> 32);
        int b = int(int32_t(uint32_t(uint64_t(k ^ (int64_t(a) << 32)) & 0xFFFFFFFFull)));
        int64_t kk = (int64_t(a) << 32) | int64_t(uint32_t(b));
        return far(kk, size, off);
    };
    d_->cells.eraseIf([&](int64_t k) { return farKey(k, RCELL, 0.5f); });
    d_->edges.eraseIf([&](int64_t k) { return farKey(k, S, 0.5f); });
    {
        std::lock_guard<std::mutex> lk(d_->wm);
        for (auto it = d_->windows.begin(); it != d_->windows.end();) {
            if (farKey(it->first, TILE, 0.5f) && it->second.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
                it = d_->windows.erase(it);
            else ++it;
        }
    }
    // nodes, receivers and catchments are small; keep them unless very far (they feed catchments)
    float keepNodes = keep + K_UP * S;
    auto farNode = [&](int64_t k) {
        int a = int(k >> 32);
        int b = int(int32_t(uint32_t(uint64_t(k ^ (int64_t(a) << 32)) & 0xFFFFFFFFull)));
        return std::hypot((a + 0.5f) * S - x, (b + 0.5f) * S - z) > keepNodes;
    };
    d_->nodes.eraseIf(farNode);
    d_->recvs.eraseIf(farNode);
    d_->fills.eraseIf(farNode);
    d_->ups.eraseIf(farNode);
    d_->lakes.eraseIf(farNode);
    d_->generation.fetch_add(1);
}

void Field::setErosion(bool on, int quality) const {
    d_->erosionOn.store(on);
    d_->quality.store(quality);
}

Stats Field::stats() const {
    Stats s;
    {
        std::lock_guard<std::mutex> lk(d_->wm);
        s.windows = int(d_->windows.size());
    }
    s.nodes = int(d_->nodes.size());
    s.cells = int(d_->cells.size());
    s.erodeMs = d_->erodeMs.load();
    return s;
}


void writeRelief(const Field& F, float x0, float z0, float size, float res, const char* out) {
    int N = int(size / res);
    std::vector<terrain::Sample> S(size_t(N) * N);
    for (int k = 0; k < N; ++k)
        for (int i = 0; i < N; ++i) S[size_t(k) * N + i] = F.sample(x0 + i * res, z0 + k * res);
    float mn = 1e9f, mx = -1e9f;
    for (auto& s : S) { mn = std::min(mn, s.h); mx = std::max(mx, s.h); }
    FILE* f = std::fopen(out, "wb");
    std::fprintf(f, "P6 %d %d 255\n", N, N);
    for (int k = N - 1; k >= 0; --k)
        for (int i = 0; i < N; ++i) {
            auto H = [&](int a, int b) { a = std::clamp(a, 0, N - 1); b = std::clamp(b, 0, N - 1); return S[size_t(b) * N + a].h; };
            const auto& s = S[size_t(k) * N + i];
            float gx = (H(i + 1, k) - H(i - 1, k)) / (2 * res), gz = (H(i, k + 1) - H(i, k - 1)) / (2 * res);
            float nx = -gx, ny = 1, nz = -gz, nl = std::sqrt(nx * nx + ny * ny + nz * nz);
            float shade = std::max(0.f, (nx * -0.6f + ny * 0.7f + nz * 0.4f) / nl);
            float t = (s.h - mn) / std::max(1.f, mx - mn);
            float r = 0.35f + 0.45f * t, g = 0.48f + 0.3f * t, b = 0.3f + 0.4f * t;
            if (std::sqrt(gx * gx + gz * gz) > 0.7f) { r = g = b = 0.55f; }
            r = r * (0.25f + shade) + s.sediment * 0.25f; g = g * (0.25f + shade) + s.sediment * 0.2f; b = b * (0.25f + shade);
            r *= 1 - s.scour * 0.3f;
            b += s.wet * 0.25f; g += s.wet * 0.05f;
            if (s.water > s.h) { float dpt = std::min(1.f, (s.water - s.h) / 3.f); r = 0.1f; g = 0.3f - dpt * 0.15f; b = 0.6f - dpt * 0.2f; }
            if (s.road < 3.6f) { r = g = b = 0.15f; }
            unsigned char px[3] = {(unsigned char)std::clamp(int(r * 255), 0, 255), (unsigned char)std::clamp(int(g * 255), 0, 255), (unsigned char)std::clamp(int(b * 255), 0, 255)};
            std::fwrite(px, 1, 3, f);
        }
    std::fclose(f);
}

}  // namespace terrain
