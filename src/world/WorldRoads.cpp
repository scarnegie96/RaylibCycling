// Towns and the road network laid over the finished land.
#include <atomic>
#include <cstdlib>
#include <functional>
#include <cstdio>
#include <map>
#include <numeric>
#include <queue>
#include <random>
#include <set>

#include "world/WorldInternal.h"
#include "world/WorldMap.h"

namespace wmap {
using namespace detail;

namespace {

// 32 directions (~8 degrees apart): a road can traverse a steep face nearly on the contour,
// so it winds up a mountainside in hairpins instead of having to go through it
const int NB = 32;
const int MX[NB] = {1, 0, -1, 0, 1, -1, 1, -1, 2, 1, -1, -2, -2, -1, 1, 2, 3, 1, -1, -3, -3, -1, 1, 3, 3, 2, -2, -3, -3, -2, 2, 3};
const int MZ[NB] = {0, 1, 0, -1, 1, 1, -1, -1, 1, 2, 2, 1, -1, -2, -2, -1, 1, 3, 3, 1, -1, -3, -3, -1, 2, 3, 3, 2, -2, -3, -3, -2};
constexpr float HW = 3.6f;   // road half width (matches Course::roadHalfWidth)

std::string placeName(std::mt19937& r, int rank) {
    static const char* A[] = {"Ash", "Bram", "Cal", "Dun", "Eld", "Fen", "Gil", "Hart", "Ivy", "Kel", "Lang", "Mar", "Nor", "Oak", "Pen",
                              "Quar", "Rook", "Sand", "Thorn", "Ul", "Ven", "Wes", "Yar", "Brack", "Cold", "Red", "Stone", "High", "Elm", "Wolf"};
    static const char* B[] = {"ford", "by", "ton", "ley", "wick", "stead", "bridge", "mere", "dale", "thorpe", "holm", "field", "combe",
                              "well", "burn", "haven", "moor", "cross", "gate", "worth"};
    std::string s = std::string(A[r() % (sizeof(A) / sizeof(*A))]) + B[r() % (sizeof(B) / sizeof(*B))];
    if (rank == 2 && r() % 4 == 0) s = "Great " + s;
    return s;
}

struct Router {
    Map& M;
    int n;
    float cell;
    std::vector<float> hs;          // smoothed ground for grades
    std::vector<uint8_t> lake;      // impassable water
    std::vector<float> riverW;      // channel width under the cell (0 = none)
    std::vector<float> rdx, rdz;    // river direction
    std::vector<float> biomeCost;   // dunes, marsh, ...
    std::vector<int> roadOf;        // cell -> road id
    explicit Router(Map& m) : M(m), n(m.n), cell(m.cell) {}
    size_t id(int i, int k) const { return size_t(k) * n + i; }
    float X(int i) const { return M.x0 + i * cell; }
    float Z(int k) const { return M.z0 + k * cell; }

    void prepare() {
        const size_t NN = size_t(n) * n;
        hs = M.h;
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<float> s(hs);
            parallelFor(n - 2, [&](int kk) {
                int k = kk + 1;
                for (int i = 1; i < n - 1; ++i) {
                    float a = 0;
                    for (int dk = -1; dk <= 1; ++dk) for (int di = -1; di <= 1; ++di) a += hs[id(i + di, k + dk)];
                    s[id(i, k)] = a / 9.f;
                }
            });
            hs.swap(s);
        }
        lake.assign(NN, 0);
        for (size_t c = 0; c < NN; ++c)
            if (M.lakeId[c] >= 0 && !M.lakes[M.lakeId[c]].playa) lake[c] = 1;
        riverW.assign(NN, 0.f);
        rdx.assign(NN, 0.f);
        rdz.assign(NN, 0.f);
        for (const River& R : M.rivers) {
            if (R.dry) continue;
            for (size_t j = 0; j + 1 < R.pts.size(); ++j) {
                const RiverPt& p = R.pts[j];
                const RiverPt& q = R.pts[j + 1];
                float w = 2 * p.half;
                if (w < 1.2f) continue;
                int r = std::max(0, int(std::ceil((p.half + 2.f) / cell)));
                int ci = int(std::lround((p.x - M.x0) / cell)), ck = int(std::lround((p.z - M.z0) / cell));
                float tx = q.x - p.x, tz = q.z - p.z, tl = std::max(1e-3f, std::hypot(tx, tz));
                for (int dk = -r; dk <= r; ++dk)
                    for (int di = -r; di <= r; ++di) {
                        int a = ci + di, b = ck + dk;
                        if (a < 0 || b < 0 || a >= n || b >= n) continue;
                        if (std::hypot(X(a) - p.x, Z(b) - p.z) > p.half + 0.75f * cell) continue;
                        size_t c = id(a, b);
                        if (w > riverW[c]) { riverW[c] = w; rdx[c] = tx / tl; rdz[c] = tz / tl; }
                    }
            }
        }
        biomeCost.assign(NN, 1.f);
        parallelFor(n, [&](int k) {
            for (int i = 0; i < n; ++i) {
                float w[BIOMES];
                M.biomeAt(X(i), Z(k), w);
                biomeCost[id(i, k)] = 1.f + 3.5f * w[Dunes] + 2.5f * w[Wetland] + 0.4f * w[Badlands] + (M.wash[id(i, k)] ? 0.6f : 0.f);
            }
        });
        roadOf.assign(NN, -1);
    }

    // A* between two cells; returns cells (start..end) or empty
    std::vector<int> route(int s, int t, int cls, float relax = 1.f, const std::vector<uint8_t>* avoid = nullptr, float reuseFac = 0.3f) {
        const float gs = cls == 0 ? 0.05f : 0.07f, gmax = cls == 0 ? 0.09f : 0.115f;   // (slack under the profile caps for the land's steps)
        int si = s % n, sk = s / n, ti = t % n, tk = t / n;
        float dist = std::hypot(float(si - ti), float(sk - tk));
        // room to wind up and down: big climbs need a longer road than the straight line
        float dh = std::fabs(hs[s] - hs[t]);
        int margin = int(std::max({70.f, dist * 0.55f, dh / (cls == 0 ? 0.08f : 0.11f) / cell * 0.35f}) * relax);
        int i0 = std::max(1, std::min(si, ti) - margin), i1 = std::min(n - 2, std::max(si, ti) + margin);
        int k0 = std::max(1, std::min(sk, tk) - margin), k1 = std::min(n - 2, std::max(sk, tk) + margin);
        int W = i1 - i0 + 1, H = k1 - k0 + 1;
        auto L = [&](int i, int k) { return (k - k0) * W + (i - i0); };
        std::vector<float> g(size_t(W) * H, 1e30f);
        std::vector<int> par(size_t(W) * H, -1);
        std::vector<int8_t> pdir(size_t(W) * H, -1);
        std::vector<uint8_t> closed(size_t(W) * H, 0);
        std::vector<float> leg(size_t(W) * H, 1e4f);   // straight distance since the last real turn
        using QE = std::pair<float, int>;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
        g[L(si, sk)] = 0;
        pq.push({0, L(si, sk)});
        const float tx = X(ti), tz = Z(tk);
        float dl[NB];
        for (int d = 0; d < NB; ++d) dl[d] = std::hypot(float(MX[d]), float(MZ[d]));
        bool found = false;
        while (!pq.empty()) {
            auto [f, l] = pq.top();
            pq.pop();
            if (closed[l]) continue;
            closed[l] = 1;
            int i = l % W + i0, k = l / W + k0;
            if (i == ti && k == tk) { found = true; break; }
            size_t c = id(i, k);
            int pd = pdir[l];
            for (int d = 0; d < NB; ++d) {
                int a = i + MX[d], b = k + MZ[d];
                if (a < i0 || b < k0 || a > i1 || b > k1) continue;
                int nl = L(a, b);
                if (closed[nl]) continue;
                size_t nc = id(a, b);
                if (lake[nc]) continue;
                if (avoid && (*avoid)[nc]) continue;
                // cells a long move passes over
                size_t mids[3];
                int nm = 0;
                bool blocked = false;
                {
                    int st = std::max(std::abs(MX[d]), std::abs(MZ[d]));
                    for (int q = 1; q < st; ++q) {
                        int mi = i + int(std::lround(float(MX[d] * q) / st)), mk = k + int(std::lround(float(MZ[d] * q) / st));
                        size_t mc = id(mi, mk);
                        if (mc == c || mc == nc) continue;
                        if (lake[mc]) blocked = true;
                        mids[nm++] = mc;
                    }
                }
                if (blocked) continue;
                float len = dl[d] * cell;
                // grade on the real ground (a smoothed grid would hide mesa cliffs and canyon walls),
                // the larger of the step itself and the smoothed trend
                float gr = std::max(std::fabs(M.h[nc] - M.h[c]), std::fabs(hs[nc] - hs[c])) / len;
                if (nm) {   // (and no hidden step along a long move)
                    float seg = len / float(nm + 1);
                    size_t prev = c;
                    for (int q = 0; q <= nm; ++q) {
                        size_t cur = q < nm ? mids[q] : nc;
                        gr = std::max(gr, 0.7f * std::fabs(M.h[cur] - M.h[prev]) / seg);
                        prev = cur;
                    }
                }
                // steeper than a road can ride: it has to go through (a cutting, then a tunnel),
                // which costs - so the router prefers to wind up with hairpins, but will tunnel
                // through a ridge when going round is much longer
                float fac = 1.f + 1.5f * (std::min(gr, gmax) / gs) * (std::min(gr, gmax) / gs);
                if (gr > gmax) fac += 70.f + 60.f * std::min(gr - gmax, 0.5f);
                fac *= 0.5f * (biomeCost[c] + biomeCost[nc]);
                bool reuse = roadOf[nc] >= 0 && roadOf[c] >= 0;
                if (reuse) fac *= reuseFac;
                float cost = len * fac;
                // bridges: short and square to the current
                float rw = riverW[nc];
                for (int q = 0; q < nm; ++q) rw = std::max(rw, riverW[mids[q]]);
                if (rw > 0 && !reuse && riverW[c] <= 0) {
                    if (rw > 45.f) continue;
                    size_t rc = nc;
                    for (int q = 0; q < nm; ++q) if (riverW[mids[q]] > 0) { rc = mids[q]; break; }
                    float mxn = MX[d] / dl[d], mzn = MZ[d] / dl[d];
                    float along = std::fabs(mxn * rdx[rc] + mzn * rdz[rc]);
                    cost += 140.f + 14.f * rw + 600.f * along * along;
                }
                if (riverW[c] > 0 && riverW[nc] > 0 && !reuse) cost += 400.f;   // never along a river
                // every metre climbed costs (engineers and cyclists both go round when they can)
                cost += std::min(std::fabs(M.h[nc] - M.h[c]), gmax * len) * (cls == 0 ? 6.f : 4.f);
                if (pd >= 0) {
                    float dot = (MX[d] * MX[pd] + MZ[d] * MZ[pd]) / (dl[d] * dl[pd]);
                    float ang = std::acos(std::clamp(dot, -1.f, 1.f));
                    cost += ang * ang * (cls == 0 ? 60.f : 35.f);
                    // a hairpin is expensive, and only after a long straight leg (no zigzags)
                    // no turn sharper than ~100 degrees: a hairpin is two right-angle bends
                    // with a stretch between, so its legs lie well apart
                    if (ang > 1.75f) continue;
                    if (ang > 1.2f) {
                        if (leg[l] < 60.f) continue;
                        cost += cls == 0 ? 260.f : 160.f;
                    }
                }
                float ng = g[l] + cost;
                if (ng < g[nl]) {
                    g[nl] = ng;
                    par[nl] = l;
                    pdir[nl] = int8_t(d);
                    {
                        float ang = 0;
                        if (pd >= 0) ang = std::acos(std::clamp((MX[d] * MX[pd] + MZ[d] * MZ[pd]) / (dl[d] * dl[pd]), -1.f, 1.f));
                        leg[nl] = ang > 0.5f ? len : leg[l] + len;
                    }
                    float h = 0.55f * std::hypot(X(a) - tx, Z(b) - tz);
                    pq.push({ng + h, nl});
                }
            }
        }
        if (!found) return {};
        std::vector<int> path;
        for (int l = L(ti, tk); l >= 0; l = par[l]) path.push_back(int(id(l % W + i0, l / W + k0)));
        std::reverse(path.begin(), path.end());
        return path;
    }
    void mark(const std::vector<int>& path, int road) {
        for (size_t j = 0; j + 1 < path.size(); ++j) {
            int a = path[j], b = path[j + 1];
            int ai = a % n, ak = a / n, bi = b % n, bk = b / n;
            int steps = std::max(std::abs(bi - ai), std::abs(bk - ak)) * 2;
            for (int s = 0; s <= steps; ++s) {
                int i = int(std::lround(ai + (bi - ai) * float(s) / steps)), k = int(std::lround(ak + (bk - ak) * float(s) / steps));
                // the road and a cell either side: a later route that comes this close joins it
                for (int dk = -1; dk <= 1; ++dk)
                    for (int di = -1; di <= 1; ++di) {
                        int a2 = i + di, b2 = k + dk;
                        if (a2 < 0 || b2 < 0 || a2 >= n || b2 >= n) continue;
                        size_t c = id(a2, b2);
                        if (roadOf[c] < 0) roadOf[c] = road;
                    }
            }
        }
    }
};

struct Proto {                     // a road before the network is split at its junctions
    std::vector<RoadPt> pts;
    int cls = 1;
    int joinA = -1, joinB = -1;    // road it branches from at each end
    int townA = -1, townB = -1;
};

void chaikin(std::vector<RoadPt>& p, int iters) {
    for (int it = 0; it < iters; ++it) {
        if (p.size() < 3) return;
        std::vector<RoadPt> o;
        o.push_back(p.front());
        for (size_t j = 0; j + 1 < p.size(); ++j) {
            const RoadPt& a = p[j];
            const RoadPt& b = p[j + 1];
            o.push_back({lerpf(a.x, b.x, 0.25f), lerpf(a.z, b.z, 0.25f), lerpf(a.y, b.y, 0.25f)});
            o.push_back({lerpf(a.x, b.x, 0.75f), lerpf(a.z, b.z, 0.75f), lerpf(a.y, b.y, 0.75f)});
        }
        o.push_back(p.back());
        p.swap(o);
    }
}
std::vector<RoadPt> resample(const std::vector<RoadPt>& p, float step) {
    std::vector<RoadPt> o;
    if (p.size() < 2) return p;
    std::vector<float> acc(p.size(), 0.f);
    for (size_t j = 1; j < p.size(); ++j) acc[j] = acc[j - 1] + std::hypot(p[j].x - p[j - 1].x, p[j].z - p[j - 1].z);
    int N = std::max(1, int(std::lround(acc.back() / step)));
    size_t j = 0;
    for (int s = 0; s <= N; ++s) {
        float t = acc.back() * s / N;
        while (j + 2 < p.size() && acc[j + 1] < t) ++j;
        float f = (t - acc[j]) / std::max(1e-6f, acc[j + 1] - acc[j]);
        f = std::clamp(f, 0.f, 1.f);
        o.push_back({lerpf(p[j].x, p[j + 1].x, f), lerpf(p[j].z, p[j + 1].z, f), lerpf(p[j].y, p[j + 1].y, f)});
    }
    return o;
}
float polyLen(const std::vector<RoadPt>& p) {
    float L = 0;
    for (size_t j = 1; j < p.size(); ++j) L += std::hypot(p[j].x - p[j - 1].x, p[j].z - p[j - 1].z);
    return L;
}
// nearest point on a polyline: segment index and parameter
float nearestOn(const std::vector<RoadPt>& p, float x, float z, size_t& seg, float& t) {
    float best = 1e30f;
    seg = 0; t = 0;
    for (size_t j = 0; j + 1 < p.size(); ++j) {
        float tt;
        float d = segDist(x, z, p[j].x, p[j].z, p[j + 1].x, p[j + 1].z, tt);
        if (d < best) { best = d; seg = j; t = tt; }
    }
    return best;
}

}  // namespace

void buildRoads(Map& M, std::mt19937& rng, const Progress& progress) {
    Router R(M);
    R.prepare();
    const int n = M.n;
    const float cell = M.cell;
    auto X = [&](int i) { return M.x0 + i * cell; };
    auto Z = [&](int k) { return M.z0 + k * cell; };
    auto U = [&]() { return std::uniform_real_distribution<float>(0.f, 1.f)(rng); };

    // ---------------------------------------------------------------- towns
    {
        // distance to permanent water
        const size_t NN = size_t(n) * n;
        std::vector<float> dw(NN, 1e9f);
        for (size_t c = 0; c < NN; ++c)
            if (R.lake[c] || R.riverW[c] > 2.f) dw[c] = 0;
        for (int k = 0; k < n; ++k)
            for (int i = 0; i < n; ++i) {
                float& v = dw[R.id(i, k)];
                if (i > 0) v = std::min(v, dw[R.id(i - 1, k)] + cell);
                if (k > 0) v = std::min(v, dw[R.id(i, k - 1)] + cell);
            }
        for (int k = n - 1; k >= 0; --k)
            for (int i = n - 1; i >= 0; --i) {
                float& v = dw[R.id(i, k)];
                if (i < n - 1) v = std::min(v, dw[R.id(i + 1, k)] + cell);
                if (k < n - 1) v = std::min(v, dw[R.id(i, k + 1)] + cell);
            }
        struct Cand { float s; int i, k; };
        std::vector<Cand> cands;
        const int edge = int(900.f / cell), st = 4;
        for (int k = edge; k < n - edge; k += st)
            for (int i = edge; i < n - edge; i += st) {
                size_t c = R.id(i, k);
                if (R.lake[c] || R.riverW[c] > 0) continue;
                float sx = (R.hs[R.id(i + 5, k)] - R.hs[R.id(i - 5, k)]) / (10 * cell);
                float sz = (R.hs[R.id(i, k + 5)] - R.hs[R.id(i, k - 5)]) / (10 * cell);
                float slope = std::hypot(sx, sz);
                float flat = sstep(0.09f, 0.015f, slope);
                // and no cliff or canyon edge anywhere close
                float lo = 1e9f, hi = -1e9f;
                for (int dk = -4; dk <= 4; dk += 2)
                    for (int di = -4; di <= 4; di += 2) { float v = M.h[R.id(i + di, k + dk)]; lo = std::min(lo, v); hi = std::max(hi, v); }
                if (hi - lo > 22.f) continue;
                float d = dw[c];
                float water = std::exp(-((d - 180.f) * (d - 180.f)) / (2 * 380.f * 380.f)) * sstep(25.f, 70.f, d);
                float w[BIOMES];
                M.biomeAt(X(i), Z(k), w);
                float pen = 0.95f * w[Dunes] + 0.75f * w[Wetland] + 0.7f * w[Alpine] + 0.35f * w[Badlands] + 0.2f * w[Boreal];
                if (M.temp[c] < 1.f) pen = 1;
                float s = flat * (0.35f + 0.65f * water) * (1.f - std::min(1.f, pen)) + 0.08f * hashf(i, k, M.params.seed + 7);
                if (s > 0.2f) cands.push_back({s, i, k});
            }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.s > b.s; });
        const float area = M.params.size * M.params.size / 4.2e8f;   // 1 for 20 km
        const int maxTowns = std::max(2, int(7 * area)), maxVillages = std::max(5, int(40 * area));
        auto farEnough = [&](float x, float z, float d) {
            for (const Town& t : M.towns) if (std::hypot(t.x - x, t.z - z) < d) return false;
            return true;
        };
        for (const Cand& c : cands) {
            if (int(M.towns.size()) >= maxTowns) break;
            if (!farEnough(X(c.i), Z(c.k), 4200.f)) continue;
            M.towns.push_back({X(c.i), Z(c.k), M.grid(c.i, c.k), 2, ""});
        }
        int nt = int(M.towns.size());
        for (const Cand& c : cands) {
            if (int(M.towns.size()) >= nt + maxVillages) break;
            if (c.s < 0.27f) break;
            if (!farEnough(X(c.i), Z(c.k), 1350.f)) continue;
            M.towns.push_back({X(c.i), Z(c.k), M.grid(c.i, c.k), 1, ""});
        }
        // every map has at least three places, so its roads can make a loop (harsh deserts,
        // ice and dune seas can leave too few good sites): take the flattest dry ground left
        if (M.towns.size() < 3) {
            std::vector<Cand> any;
            for (int k = edge; k < n - edge; k += st)
                for (int i = edge; i < n - edge; i += st) {
                    size_t c = R.id(i, k);
                    if (R.lake[c] || R.riverW[c] > 0) continue;
                    float lo = 1e9f, hi = -1e9f;
                    for (int dk = -4; dk <= 4; dk += 2)
                        for (int di = -4; di <= 4; di += 2) { float v = M.h[R.id(i + di, k + dk)]; lo = std::min(lo, v); hi = std::max(hi, v); }
                    any.push_back({-(hi - lo) + 2.f * hashf(i, k, M.params.seed + 11), i, k});
                }
            std::sort(any.begin(), any.end(), [](const Cand& a, const Cand& b) { return a.s > b.s; });
            for (float spacing : {2500.f, 1500.f, 900.f})
                for (const Cand& c : any) {
                    if (M.towns.size() >= 3) break;
                    if (!farEnough(X(c.i), Z(c.k), spacing)) continue;
                    M.towns.push_back({X(c.i), Z(c.k), M.grid(c.i, c.k), 1, ""});
                }
        }
        std::set<std::string> used;
        for (Town& t : M.towns) {
            std::string s;
            do s = placeName(rng, t.rank); while (used.count(s));
            used.insert(s);
            t.name = s;
        }
        M.stats.towns = int(M.towns.size());
    }
    if (M.towns.size() < 2) return;
    if (progress) progress("roads", 0.75f);

    // ---------------------------------------------------------------- the links
    const int P = int(M.towns.size());
    struct E { int a, b; float len; int cls; bool tree = false; };
    std::vector<E> gab;
    for (int a = 0; a < P; ++a)
        for (int b = a + 1; b < P; ++b) {
            float mx = 0.5f * (M.towns[a].x + M.towns[b].x), mz = 0.5f * (M.towns[a].z + M.towns[b].z);
            float r2 = 0.25f * ((M.towns[a].x - M.towns[b].x) * (M.towns[a].x - M.towns[b].x) + (M.towns[a].z - M.towns[b].z) * (M.towns[a].z - M.towns[b].z));
            bool ok = true;
            for (int c = 0; c < P && ok; ++c)
                if (c != a && c != b && (M.towns[c].x - mx) * (M.towns[c].x - mx) + (M.towns[c].z - mz) * (M.towns[c].z - mz) < r2) ok = false;
            if (!ok) continue;
            float len = std::sqrt(r2) * 2;
            gab.push_back({a, b, len, (M.towns[a].rank == 2 && M.towns[b].rank == 2) ? 0 : 1, false});
        }
    std::sort(gab.begin(), gab.end(), [](const E& x, const E& y) { return x.len < y.len; });
    std::vector<int> uf(P);
    std::iota(uf.begin(), uf.end(), 0);
    std::function<int(int)> find = [&](int x) { return uf[x] == x ? x : uf[x] = find(uf[x]); };
    std::vector<E> links;
    for (E e : gab) {
        int ra = find(e.a), rb = find(e.b);
        e.tree = ra != rb;
        if (e.tree) uf[ra] = rb;
        if (e.tree || (e.len < 11000.f && U() < 0.92f)) links.push_back(e);
    }
    // the main roads first, then the minor ones, each shortest first
    std::stable_sort(links.begin(), links.end(), [](const E& x, const E& y) { return x.cls < y.cls; });

    // ---------------------------------------------------------------- route them
    std::vector<Proto> protos;
    std::vector<int> built(P);
    std::iota(built.begin(), built.end(), 0);
    std::function<int(int)> bfind = [&](int x) { return built[x] == x ? x : built[x] = bfind(built[x]); };
    auto lay = [&](const E& e, const std::vector<int>& path) {
        // split into runs of new road; where it runs along an existing road it reuses it
        std::vector<int> run;
        int joinFrom = -1;
        bool fresh = false;
        auto flush = [&](int joinTo, bool endsAtTown) {
            if (run.size() >= 2) {
                Proto p;
                p.cls = e.cls;
                p.joinA = joinFrom; p.joinB = joinTo;
                if (joinFrom < 0) p.townA = e.a;
                if (endsAtTown) p.townB = e.b;
                for (int c : run) p.pts.push_back({X(c % n), Z(c / n), M.h[c]});
                int rid = int(protos.size());
                R.mark(run, rid);
                protos.push_back(std::move(p));
            }
            run.clear();
        };
        for (size_t j = 0; j < path.size(); ++j) {
            int c = path[j];
            int owner = R.roadOf[c];
            if (owner >= 0) {
                if (fresh) { run.push_back(c); flush(owner, false); }
                run.clear();
                run.push_back(c);
                joinFrom = owner;
                fresh = false;
            } else {
                run.push_back(c);
                fresh = true;
            }
        }
        if (fresh) flush(-1, true);
        built[bfind(e.a)] = bfind(e.b);
    };
    auto cellOf = [&](const Town& T) { return int(R.id(int(std::lround((T.x - M.x0) / cell)), int(std::lround((T.z - M.z0) / cell)))); };
    int done = 0;
    for (const E& e : links) {
        auto path = R.route(cellOf(M.towns[e.a]), cellOf(M.towns[e.b]), e.cls);
        if (std::getenv("WMAP_PROFILE")) std::printf("    link %s-%s %.1f km cls %d: %zu cells\n", M.towns[e.a].name.c_str(), M.towns[e.b].name.c_str(), e.len / 1000, e.cls, path.size());
        if (progress) progress("roads", 0.75f + 0.15f * float(++done) / links.size());
        if (path.size() < 2) continue;
        if (!e.tree) {   // optional links that need a staircase of hairpins or a long detour aren't built
            int hairpins = 0;
            float plen = 0;
            for (size_t j = 1; j < path.size(); ++j) {
                plen += std::hypot(float(path[j] % n - path[j - 1] % n), float(path[j] / n - path[j - 1] / n)) * cell;
                if (j + 1 < path.size()) {
                    float ax = float(path[j] % n - path[j - 1] % n), az = float(path[j] / n - path[j - 1] / n);
                    float bx = float(path[j + 1] % n - path[j] % n), bz = float(path[j + 1] / n - path[j] / n);
                    float c = (ax * bx + az * bz) / std::max(1e-3f, std::hypot(ax, az) * std::hypot(bx, bz));
                    hairpins += c < 0.35f;
                }
            }
            if (hairpins > 4 || plen > 2.4f * e.len) continue;
        }
        lay(e, path);
    }
    // every place reachable: any town left on its own is joined to the nearest connected one
    // (with a wider search), until the network is one piece
    for (int pass = 0; pass < 3; ++pass) {
        std::map<int, int> size;
        for (int t = 0; t < P; ++t) size[bfind(t)]++;
        if (size.size() <= 1) break;
        int mainRoot = std::max_element(size.begin(), size.end(), [](auto& a, auto& b) { return a.second < b.second; })->first;
        for (int t = 0; t < P; ++t) {
            if (bfind(t) == mainRoot) continue;
            int best = -1;
            float bd = 1e30f;
            for (int u = 0; u < P; ++u)
                if (bfind(u) == mainRoot) {
                    float d = std::hypot(M.towns[t].x - M.towns[u].x, M.towns[t].z - M.towns[u].z);
                    if (d < bd) { bd = d; best = u; }
                }
            if (best < 0) continue;
            E e{t, best, bd, 1, true};
            auto path = R.route(cellOf(M.towns[t]), cellOf(M.towns[best]), 1, 2.5f);
            if (path.size() >= 2) lay(e, path);
        }
    }

    // ---------------------------------------------------------------- shape them, then the graph
    // (a function: it is rebuilt after the dead-end pass below adds roads)
    auto buildGraph = [&](std::vector<Proto> protos) {
        for (Proto& p : protos) {
            chaikin(p.pts, 3);
            p.pts = resample(p.pts, 5.f);
        }
        // branch roads start exactly on the road they leave
        struct Split { int road; size_t seg; float t; float x, z; };
        std::vector<Split> splits;
        struct Taken { float along; size_t seg; float t, x, z; };
        std::vector<std::vector<Taken>> accepted(protos.size());
        for (size_t r = 0; r < protos.size(); ++r) {
            Proto& p = protos[r];
            for (int end = 0; end < 2; ++end) {
                int j = end == 0 ? p.joinA : p.joinB;
                if (j < 0 || j == int(r) || p.pts.size() < 3) continue;
                RoadPt& e = end == 0 ? p.pts.front() : p.pts.back();
                size_t seg;
                float t;
                float d = nearestOn(protos[j].pts, e.x, e.z, seg, t);
                if (d > 60.f) continue;
                const auto& a = protos[j].pts[seg];
                const auto& b = protos[j].pts[seg + 1];
                float sx = lerpf(a.x, b.x, t), sz = lerpf(a.z, b.z, t);
                // a junction a few metres from the other road's end is that end, and one a few
                // metres from another junction on that road is that junction (no stub roads)
                bool atEnd = false;
                {
                    const auto& Q = protos[j].pts;
                    float along = 0, total = polyLen(Q);
                    for (size_t q = 0; q < seg; ++q) along += std::hypot(Q[q + 1].x - Q[q].x, Q[q + 1].z - Q[q].z);
                    along += t * std::hypot(b.x - a.x, b.z - a.z);
                    if (along < 90.f) { sx = Q.front().x; sz = Q.front().z; atEnd = true; }
                    else if (total - along < 90.f) { sx = Q.back().x; sz = Q.back().z; atEnd = true; }
                    else {
                        for (const auto& ex : accepted[j])
                            if (std::fabs(ex.along - along) < 80.f) { seg = ex.seg; t = ex.t; sx = ex.x; sz = ex.z; along = ex.along; break; }
                        accepted[j].push_back({along, seg, t, sx, sz});
                    }
                }
                // ease the last stretch onto the junction
                float dx = sx - e.x, dz = sz - e.z;
                float accd = 0, easeLen = std::max(60.f, 2.5f * std::hypot(dx, dz));
                int N = int(p.pts.size());
                for (int q = 0; q < N; ++q) {
                    int qi = end == 0 ? q : N - 1 - q;
                    if (q > 0) {
                        int qp = end == 0 ? qi - 1 : qi + 1;
                        accd += std::hypot(p.pts[qi].x - p.pts[qp].x, p.pts[qi].z - p.pts[qp].z);
                    }
                    float f = 1.f - sstep(0.f, easeLen, accd);
                    if (f <= 0) break;
                    p.pts[qi].x += dx * f;
                    p.pts[qi].z += dz * f;
                }
                if (!atEnd) splits.push_back({j, seg, t, sx, sz});
            }
        }
        // ---------------------------------------------------------------- the network graph
        // split every road where another branches off it, then join ends into nodes
        std::vector<std::vector<std::pair<size_t, float>>> cuts(protos.size());
        for (const Split& sp : splits) cuts[sp.road].push_back({sp.seg, sp.t});
        M.roads.clear();
        M.nodes.clear();
        auto nodeAt = [&](float x, float z) {
            for (size_t i = 0; i < M.nodes.size(); ++i)
                if (std::hypot(M.nodes[i].x - x, M.nodes[i].z - z) < 6.f) return int(i);
            Node nd;
            nd.x = x; nd.z = z;
            M.nodes.push_back(nd);
            return int(M.nodes.size() - 1);
        };
        for (size_t r = 0; r < protos.size(); ++r) {
            const Proto& p = protos[r];
            if (p.pts.size() < 2) continue;
            auto cs = cuts[r];
            std::sort(cs.begin(), cs.end(), [](auto& a, auto& b) { return a.first < b.first || (a.first == b.first && a.second < b.second); });
            std::vector<RoadPt> cur;
            cur.push_back(p.pts[0]);
            size_t ci = 0;
            auto emit = [&]() {
                if (cur.size() < 2 || polyLen(cur) < 3.f) return;
                Road rd;
                rd.pts = cur;
                rd.cls = p.cls;
                rd.length = polyLen(cur);
                M.roads.push_back(rd);
            };
            for (size_t j = 0; j + 1 < p.pts.size(); ++j) {
                while (ci < cs.size() && cs[ci].first == j) {
                    const auto& a = p.pts[j];
                    const auto& b = p.pts[j + 1];
                    float t = cs[ci].second;
                    RoadPt m{lerpf(a.x, b.x, t), lerpf(a.z, b.z, t), 0.f};
                    cur.push_back(m);
                    emit();
                    cur.clear();
                    cur.push_back(m);
                    ++ci;
                }
                cur.push_back(p.pts[j + 1]);
            }
            emit();
        }
        for (size_t r = 0; r < M.roads.size(); ++r) {
            Road& rd = M.roads[r];
            rd.pts = resample(rd.pts, 5.f);
            rd.length = polyLen(rd.pts);
            rd.a = nodeAt(rd.pts.front().x, rd.pts.front().z);
            rd.b = nodeAt(rd.pts.back().x, rd.pts.back().z);
            M.nodes[rd.a].roads.push_back(int(r));
            M.nodes[rd.b].roads.push_back(int(r));
        }
    };
    buildGraph(protos);

    // ---------------------------------------------------------------- no dead ends
    // Every road must lead on: a place joined by a single road (or any other end of the
    // network) gets a second road to another part of the network, far enough away by road
    // that it makes a real loop, leaving in a different direction from the road it has.
    // Then the graph is rebuilt, until no node has a single road.
    {
        const size_t NN = size_t(n) * n;
        auto cellAt = [&](float x, float z) {
            int i = std::clamp(int(std::lround((x - M.x0) / cell)), 1, n - 2), k = std::clamp(int(std::lround((z - M.z0) / cell)), 1, n - 2);
            return int(R.id(i, k));
        };
        auto nearestTown = [&](float x, float z) {
            int best = 0;
            float bd = 1e30f;
            for (int t = 0; t < P; ++t) {
                float d = std::hypot(M.towns[t].x - x, M.towns[t].z - z);
                if (d < bd) { bd = d; best = t; }
            }
            return best;
        };
        for (int iter = 0; iter < 5; ++iter) {
            std::vector<int> dead;
            for (size_t i = 0; i < M.nodes.size(); ++i)
                if (M.nodes[i].roads.size() == 1) dead.push_back(int(i));
            if (std::getenv("WMAP_PROFILE")) std::printf("    dead ends pass %d: %zu\n", iter, dead.size());
            if (dead.empty()) break;
            std::vector<uint8_t> fixedNode(M.nodes.size(), 0);
            bool added = false;
            for (int u : dead) {
                if (fixedNode[u]) continue;
                const Node& U0 = M.nodes[u];
                // road distance from u to every node
                std::vector<float> gd(M.nodes.size(), 1e30f);
                std::vector<int> viaRoad(M.nodes.size(), -1);
                {
                    using QE = std::pair<float, int>;
                    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
                    gd[u] = 0;
                    q.push({0.f, u});
                    while (!q.empty()) {
                        auto [d, x] = q.top();
                        q.pop();
                        if (d > gd[x]) continue;
                        for (int r : M.nodes[x].roads) {
                            int y = M.roads[r].a == x ? M.roads[r].b : M.roads[r].a;
                            if (d + M.roads[r].length < gd[y]) { gd[y] = d + M.roads[r].length; viaRoad[y] = r; q.push({gd[y], y}); }
                        }
                    }
                }
                struct Cand { int v; float score; bool alongside; };
                std::vector<Cand> cands;
                for (size_t v = 0; v < M.nodes.size(); ++v) {
                    if (int(v) == u || M.nodes[v].roads.empty()) continue;
                    float eu = std::hypot(M.nodes[v].x - U0.x, M.nodes[v].z - U0.z);
                    if (eu < 250.f || eu > 14000.f) continue;
                    // the loop it closes must be worth riding (not a short-cut across the stub's own bend)
                    if (gd[v] - eu < 600.f) continue;
                    float sc = eu * (M.nodes[v].roads.size() == 1 ? 0.6f : 1.f);
                    cands.push_back({int(v), sc, false});
                }
                std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.score < y.score; });
                // last resort (e.g. two places, one road): a second road to somewhere already reachable,
                // kept off the existing way there, so the two make a loop
                for (size_t v = 0; v < M.nodes.size(); ++v) {
                    if (int(v) == u || M.nodes[v].roads.empty() || gd[v] > 1e29f) continue;
                    float eu = std::hypot(M.nodes[v].x - U0.x, M.nodes[v].z - U0.z);
                    if (eu < 600.f || eu > 14000.f) continue;
                    cands.push_back({int(v), 1e6f + eu, true});
                }
                std::stable_sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) { return x.score < y.score; });
                // keep off the road u already has (the router's own cells for it, which can sit a
                // little off the smoothed line) for its first ~650 m, so the new one leaves another way
                auto ownersOf = [&](int r, float maxFromStart, bool fromA) {
                    std::set<int> own;
                    const auto& Q = M.roads[r].pts;
                    float acc = 0;
                    for (size_t j = 0; j < Q.size(); ++j) {
                        size_t qi = fromA ? j : Q.size() - 1 - j;
                        if (j) {
                            size_t qp = fromA ? qi - 1 : qi + 1;
                            acc += std::hypot(Q[qi].x - Q[qp].x, Q[qi].z - Q[qp].z);
                        }
                        if (acc > maxFromStart) break;
                        int c = cellAt(Q[qi].x, Q[qi].z), ci = c % n, ck = c / n;
                        for (int dk = -2; dk <= 2; ++dk)
                            for (int di = -2; di <= 2; ++di) {
                                int a2 = ci + di, b2 = ck + dk;
                                if (a2 >= 0 && b2 >= 0 && a2 < n && b2 < n && R.roadOf[R.id(a2, b2)] >= 0) own.insert(R.roadOf[R.id(a2, b2)]);
                            }
                    }
                    return own;
                };
                // mask cells owned by `own` lying between rmin and rmax of (x, z), and the smoothed line itself
                auto maskOwned = [&](std::vector<uint8_t>& av, const std::set<int>& own, float x, float z, float rmin, float rmax) {
                    int ci = cellAt(x, z) % n, ck = cellAt(x, z) / n;
                    int rad = int(rmax / cell) + 3;
                    for (int k = std::max(0, ck - rad); k <= std::min(n - 1, ck + rad); ++k)
                        for (int i = std::max(0, ci - rad); i <= std::min(n - 1, ci + rad); ++i) {
                            size_t c = R.id(i, k);
                            if (R.roadOf[c] < 0 || !own.count(R.roadOf[c])) continue;
                            float d = std::hypot(X(i) - x, Z(k) - z);
                            if (d >= rmin && d <= rmax) av[c] = 1;
                        }
                };
                std::vector<uint8_t> avoid(NN, 0);
                for (int r : U0.roads) {
                    bool fromA = M.roads[r].a == u;
                    auto own = ownersOf(r, 700.f, fromA);
                    maskOwned(avoid, own, U0.x, U0.z, 45.f, 650.f);
                    // and the smoothed line, as before
                    const auto& Q = M.roads[r].pts;
                    float acc = 0;
                    for (size_t j = 0; j < Q.size(); ++j) {
                        size_t qi = fromA ? j : Q.size() - 1 - j;
                        if (j) {
                            size_t qp = fromA ? qi - 1 : qi + 1;
                            acc += std::hypot(Q[qi].x - Q[qp].x, Q[qi].z - Q[qp].z);
                        }
                        if (acc > 650.f) break;
                        if (acc < 45.f) continue;
                        int c = cellAt(Q[qi].x, Q[qi].z), ci = c % n, ck = c / n;
                        for (int dk = -2; dk <= 2; ++dk)
                            for (int di = -2; di <= 2; ++di) {
                                int a2 = ci + di, b2 = ck + dk;
                                if (a2 >= 0 && b2 >= 0 && a2 < n && b2 < n) avoid[R.id(a2, b2)] = 1;
                            }
                    }
                }
                int cu = cellAt(U0.x, U0.z);
                avoid[cu] = 0;
                if (std::getenv("WMAP_PROFILE")) std::printf("      dead end %d (%.0f, %.0f): %zu candidates, town %d\n", u, U0.x, U0.z, cands.size(), U0.town);
                auto markRoad = [&](std::vector<uint8_t>& av, int r, float skipA, float skipB) {
                    const auto& Q = M.roads[r].pts;
                    float total = M.roads[r].length, acc = 0;
                    for (size_t j = 0; j < Q.size(); ++j) {
                        if (j) acc += std::hypot(Q[j].x - Q[j - 1].x, Q[j].z - Q[j - 1].z);
                        if (acc < skipA || total - acc < skipB) continue;
                        int c = cellAt(Q[j].x, Q[j].z), ci = c % n, ck = c / n;
                        for (int dk = -2; dk <= 2; ++dk)
                            for (int di = -2; di <= 2; ++di) {
                                int a2 = ci + di, b2 = ck + dk;
                                if (a2 >= 0 && b2 >= 0 && a2 < n && b2 < n) av[R.id(a2, b2)] = 1;
                            }
                    }
                };
                int tried = 0;
                for (size_t ci = 0; ci < cands.size() && tried < 6; ++ci) {
                    int v = cands[ci].v;
                    if (cands[ci].alongside && tried > 3) break;
                    ++tried;
                    std::vector<uint8_t> av2;
                    const std::vector<uint8_t>* use = &avoid;
                    if (cands[ci].alongside) {   // keep off every road of the existing way from u to v
                        av2 = avoid;
                        for (int x = v; x != u && viaRoad[x] >= 0;) {
                            int r = viaRoad[x];
                            int y = M.roads[r].a == x ? M.roads[r].b : M.roads[r].a;
                            markRoad(av2, r, y == u || x == u ? 45.f : 0.f, x == v || y == v ? 45.f : 0.f);
                            auto own = ownersOf(r, 1e9f, true);
                            float mx = 0.5f * (M.nodes[x].x + M.nodes[y].x), mz = 0.5f * (M.nodes[x].z + M.nodes[y].z);
                            std::vector<uint8_t> tmp(NN, 0);
                            maskOwned(tmp, own, mx, mz, 0.f, M.roads[r].length * 0.6f + 200.f);
                            for (size_t c = 0; c < NN; ++c)
                                if (tmp[c] && std::hypot(X(int(c % n)) - U0.x, Z(int(c / n)) - U0.z) > 45.f &&
                                    std::hypot(X(int(c % n)) - M.nodes[v].x, Z(int(c / n)) - M.nodes[v].z) > 45.f) av2[c] = 1;
                            x = y;
                        }
                        av2[cu] = 0;
                        use = &av2;
                    }
                    // a clean new road: first try touching no other road on the way (one piece, no
                    // braiding across the old one); then allow crossings; no discount for running along roads
                    const int cv = cellAt(M.nodes[v].x, M.nodes[v].z);
                    std::vector<uint8_t> clean(*use);
                    for (size_t c = 0; c < NN; ++c) {
                        if (R.roadOf[c] < 0) continue;
                        float x = X(int(c % n)), z = Z(int(c / n));
                        if (std::hypot(x - U0.x, z - U0.z) < 60.f || std::hypot(x - M.nodes[v].x, z - M.nodes[v].z) < 120.f) continue;
                        clean[c] = 1;
                    }
                    clean[cu] = 0;
                    clean[cv] = 0;
                    auto path = R.route(cu, cv, 1, 2.5f, &clean, 1.f);
                    if (path.size() < 2) path = R.route(cu, cv, 1, 6.f, &clean, 1.f);
                    if (path.size() < 2) path = R.route(cu, cv, 1, 2.5f, use, 1.f);
                    if (path.size() < 2) path = R.route(cu, cv, 1, 6.f, use, 1.f);   // boxed in: search wider
                    if (path.size() < 2) continue;
                    E e{nearestTown(U0.x, U0.z), nearestTown(M.nodes[v].x, M.nodes[v].z), cands[ci].score, 1, true};
                    size_t before = protos.size();
                    lay(e, path);
                    if (std::getenv("WMAP_PROFILE")) {
                        std::printf("      path %zu cells, start owner %d, end owner %d, %zu new pieces:", path.size(), R.roadOf[path.front()], R.roadOf[path.back()], protos.size() - before);
                        for (size_t q = before; q < protos.size(); ++q) std::printf(" [%zu pts join %d/%d]", protos[q].pts.size(), protos[q].joinA, protos[q].joinB);
                        std::printf("\n");
                    }
                    fixedNode[u] = 1;
                    if (M.nodes[v].roads.size() == 1) fixedNode[v] = 1;
                    added = true;
                    if (std::getenv("WMAP_PROFILE")) std::printf("      dead end %d (%.0f, %.0f) -> node %d, %.1f km by road before\n", u, U0.x, U0.z, v, gd[v] / 1000);
                    break;
                }
            }
            if (!added) break;
            buildGraph(protos);
        }
        // last resort: a spur that could not be joined anywhere is taken out (back to its
        // junction), so what is left is all loops - unless that would leave no network at all
        {
            std::vector<int> deg(M.nodes.size(), 0);
            for (const Road& rd : M.roads) { deg[rd.a]++; deg[rd.b]++; }
            std::vector<uint8_t> gone(M.roads.size(), 0);
            std::vector<int> st;
            for (size_t i = 0; i < deg.size(); ++i) if (deg[i] == 1) st.push_back(int(i));
            while (!st.empty()) {
                int x = st.back();
                st.pop_back();
                if (deg[x] != 1) continue;
                for (int r : M.nodes[x].roads) {
                    if (gone[r]) continue;
                    gone[r] = 1;
                    int y = M.roads[r].a == x ? M.roads[r].b : M.roads[r].a;
                    deg[x]--;
                    if (--deg[y] == 1) st.push_back(y);
                    break;
                }
            }
            size_t kept = 0;
            for (size_t r = 0; r < gone.size(); ++r) kept += !gone[r];
            if (kept > 0 && kept < M.roads.size()) {
                if (std::getenv("WMAP_PROFILE")) std::printf("    pruned %zu spur roads that could not be looped\n", M.roads.size() - kept);
                std::vector<Road> roads;
                for (size_t r = 0; r < M.roads.size(); ++r) if (!gone[r]) roads.push_back(std::move(M.roads[r]));
                std::vector<int> remap(M.nodes.size(), -1);
                std::vector<Node> nodes;
                for (Road& rd : roads)
                    for (int* e : {&rd.a, &rd.b}) {
                        if (remap[*e] < 0) { remap[*e] = int(nodes.size()); Node nd = M.nodes[*e]; nd.roads.clear(); nodes.push_back(nd); }
                        *e = remap[*e];
                    }
                for (size_t r = 0; r < roads.size(); ++r) { nodes[roads[r].a].roads.push_back(int(r)); nodes[roads[r].b].roads.push_back(int(r)); }
                M.roads = std::move(roads);
                M.nodes = std::move(nodes);
            }
        }
    }

    // ---------------------------------------------------------------- profiles
    // The road lies on the land: the ground under it, lightly smoothed, held to a rideable
    // grade (12 % main roads, 15 % minor). Where the land is steeper than that the road goes
    // into it (a cutting, then a tunnel); where it would fly high over the land it is a
    // bridge. Rivers are bridged with clearance; lake shores are kept dry.
    struct Lift { float s, need, reach = 1e9f; };   // (reach: how far its ramps may run)
    // main roads first, longest first: a road meets a junction at the height the roads already
    // through it have there (a branch out of a tunnel starts in the tunnel)
    std::vector<int> order(M.roads.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return M.roads[a].cls != M.roads[b].cls ? M.roads[a].cls < M.roads[b].cls : M.roads[a].length > M.roads[b].length;
    });
    std::vector<float> nodeY(M.nodes.size(), std::numeric_limits<float>::quiet_NaN());
    for (int ri : order) {
        Road& rd = M.roads[ri];
        const size_t N = rd.pts.size();
        std::vector<float> g(N), y(N), s(N, 0.f);
        for (size_t j = 0; j < N; ++j) {
            g[j] = M.height(rd.pts[j].x, rd.pts[j].z);
            if (j) s[j] = s[j - 1] + std::hypot(rd.pts[j].x - rd.pts[j - 1].x, rd.pts[j].z - rd.pts[j - 1].z);
        }
        y = g;
        for (int pass = 0; pass < 2; ++pass) {   // ~ +-15 m smoothing
            std::vector<float> o(y);
            for (size_t j = 0; j < N; ++j) {
                float a = 0, w = 0;
                for (int k = -3; k <= 3; ++k) {
                    long q = long(j) + k;
                    if (q < 0 || q >= long(N)) continue;
                    float ww = std::exp(-0.5f * (k / 1.6f) * (k / 1.6f));
                    a += y[q] * ww; w += ww;
                }
                o[j] = a / w;
            }
            y.swap(o);
        }
        std::vector<Lift> lifts;
        for (size_t j = 0; j + 1 < N; ++j) {
            int ci = int(std::lround((rd.pts[j].x - M.x0) / cell)), ck = int(std::lround((rd.pts[j].z - M.z0) / cell));
            if (ci < 0 || ck < 0 || ci >= n || ck >= n) continue;
            if (R.riverW[R.id(ci, ck)] > 0) {
                for (const River& Rv : M.rivers) {
                    if (Rv.dry) continue;
                    for (size_t q = 0; q + 1 < Rv.pts.size(); ++q) {
                        const RiverPt& a = Rv.pts[q];
                        const RiverPt& b = Rv.pts[q + 1];
                        if (std::fabs(a.x - rd.pts[j].x) > 40.f || std::fabs(a.z - rd.pts[j].z) > 40.f) continue;
                        float rx = rd.pts[j + 1].x - rd.pts[j].x, rz = rd.pts[j + 1].z - rd.pts[j].z;
                        float sx2 = b.x - a.x, sz2 = b.z - a.z;
                        float den = rx * sz2 - rz * sx2;
                        if (std::fabs(den) < 1e-9f) continue;
                        float qx = a.x - rd.pts[j].x, qz = a.z - rd.pts[j].z;
                        float tt = (qx * sz2 - qz * sx2) / den, uu = (qx * rz - qz * rx) / den;
                        if (tt < 0 || tt > 1 || uu < 0 || uu > 1) continue;
                        float level = lerpf(a.level, b.level, uu), half = lerpf(a.half, b.half, uu);
                        if (half < 0.6f) continue;   // a culvert
                        lifts.push_back({lerpf(s[j], s[j + 1], tt), level + 4.2f + 0.15f * half});   // room for an arch
                        rd.cross.push_back({lerpf(s[j], s[j + 1], tt), half});
                    }
                }
            }
            // lake shores at the road's own level
            float lv = -1e9f;
            for (int dk = -2; dk <= 2; ++dk)
                for (int di = -2; di <= 2; ++di) {
                    int a2 = ci + di, b2 = ck + dk;
                    if (a2 < 0 || b2 < 0 || a2 >= n || b2 >= n) continue;
                    int l = M.lakeId[R.id(a2, b2)];
                    if (l >= 0 && !M.lakes[l].playa) lv = std::max(lv, M.lakes[l].level);
                }
            if (lv > -1e8f && y[j] < lv + 1.2f && lv < g[j] + 3.f) lifts.push_back({s[j], lv + 1.2f, 40.f});   // a shore: only locally
        }
        // rideable: cut down through what's too steep, rather than up and over it
        const float gc = rd.cls == 0 ? 0.12f : 0.15f;
        // the grade-held profiles under the land (all cutting) and over it (all embankment);
        // the road takes the middle way where they are close, cut and fill in balance - on a long
        // climb that is steeper in steps than the cap it stays with the land instead of falling
        // ever further behind it; through real relief it goes under (a tunnel)
        std::vector<float> up(y), dn(y);
        for (size_t j = 1; j < N; ++j) up[j] = std::min(up[j], up[j - 1] + gc * (s[j] - s[j - 1]));
        for (long j = long(N) - 2; j >= 0; --j) up[j] = std::min(up[j], up[j + 1] + gc * (s[j + 1] - s[j]));
        for (size_t j = 1; j < N; ++j) dn[j] = std::max(dn[j], dn[j - 1] - gc * (s[j] - s[j - 1]));
        for (long j = long(N) - 2; j >= 0; --j) dn[j] = std::max(dn[j], dn[j + 1] - gc * (s[j + 1] - s[j]));
        for (size_t j = 0; j < N; ++j) up[j] += 0.5f * std::min(dn[j] - up[j], 24.f);   // (big relief: under it)
        // a river or shore far above the road's own line is passed under (the road is in a
        // tunnel or a deep cutting there), not climbed to
        {
            auto lineAt = [&](float sq) {
                size_t j = size_t(std::lower_bound(s.begin(), s.end(), sq) - s.begin());
                return up[std::min(j, N - 1)];
            };
            std::vector<Lift> keep;
            for (const Lift& L : lifts) {
                if (L.need - lineAt(L.s) < 18.f) { keep.push_back(L); continue; }
                for (size_t q = 0; q < rd.cross.size(); ++q)
                    if (std::fabs(rd.cross[q].first - L.s) < 0.01f) { rd.cross.erase(rd.cross.begin() + long(q)); break; }
            }
            lifts.swap(keep);
        }
        // ... and where something (a bridge) has to stay up, its ramps fill in
        for (size_t j = 0; j < N; ++j) {
            float need = -1e9f;
            for (const Lift& L : lifts)
                if (std::fabs(s[j] - L.s) < L.reach) need = std::max(need, L.need - gc * std::max(0.f, std::fabs(s[j] - L.s) - 8.f));
            y[j] = std::max(up[j], need);
        }
        for (int end = 0; end < 2; ++end) {
            float pin = nodeY[end == 0 ? rd.a : rd.b];
            if (std::isnan(pin)) continue;
            for (size_t j = 0; j < N; ++j) {
                float d = end == 0 ? s[j] : s[N - 1] - s[j];
                y[j] = std::clamp(y[j], pin - gc * d, pin + gc * d);
            }
        }
        if (std::isnan(nodeY[rd.a])) nodeY[rd.a] = y.front();
        if (std::isnan(nodeY[rd.b])) nodeY[rd.b] = y.back();
        for (size_t j = 0; j < N; ++j) rd.pts[j].y = y[j];
    }
    // every road meeting at a node meets at one height: the mean of the ends, each road eased
    // onto it (only small differences: they all follow the same land)
    for (size_t i = 0; i < M.nodes.size(); ++i) {
        Node& nd = M.nodes[i];
        double sum = 0;
        int cnt = 0;
        for (int r : nd.roads) {
            const Road& rd = M.roads[r];
            if (rd.a == int(i)) { sum += rd.pts.front().y; ++cnt; }
            if (rd.b == int(i)) { sum += rd.pts.back().y; ++cnt; }
        }
        if (!cnt) continue;
        nd.y = float(sum / cnt);
        for (int r : nd.roads) {
            Road& rd = M.roads[r];
            for (int end = 0; end < 2; ++end) {
                if ((end == 0 ? rd.a : rd.b) != int(i)) continue;
                float delta = nd.y - (end == 0 ? rd.pts.front().y : rd.pts.back().y);
                if (std::fabs(delta) < 0.01f) continue;
                float span = std::clamp(std::fabs(delta) / 0.05f, 60.f, std::max(60.f, rd.length * 0.5f));
                float acc = 0;
                int N = int(rd.pts.size());
                for (int q = 0; q < N; ++q) {
                    int qi = end == 0 ? q : N - 1 - q;
                    if (q > 0) { int qp = end == 0 ? qi - 1 : qi + 1; acc += std::hypot(rd.pts[qi].x - rd.pts[qp].x, rd.pts[qi].z - rd.pts[qp].z); }
                    if (acc > span) break;
                    rd.pts[qi].y += delta * (1.f - sstep(0.f, span, acc));
                }
            }
        }
    }

    // ---------------------------------------------------------------- tunnels and bridges
    // deeper than a portal's height below the land for a while: a tunnel; well above the land
    // (a valley, a river, a lake): a bridge
    const float TUNNEL_DEPTH = 7.f, BRIDGE_CLEAR = 7.f;   // (lower than that: an embankment)
    M.bridges.clear();
    M.tunnels.clear();
    for (size_t r = 0; r < M.roads.size(); ++r) {
        Road& rd = M.roads[r];
        const int N = int(rd.pts.size());
        std::vector<float> g(N);
        std::vector<uint8_t> wet(N, 0);
        for (int j = 0; j < N; ++j) {
            g[j] = M.height(rd.pts[j].x, rd.pts[j].z);
            int ci = std::clamp(int(std::lround((rd.pts[j].x - M.x0) / cell)), 0, n - 1), ck = std::clamp(int(std::lround((rd.pts[j].z - M.z0) / cell)), 0, n - 1);
            (void)ci; (void)ck;
            rd.pts[j].kind = 0;
        }
        // the river crossings found while profiling: bridged across their width and banks
        {
            float sj = 0;
            for (int j = 0; j < N; ++j) {
                if (j) sj += std::hypot(rd.pts[j].x - rd.pts[j - 1].x, rd.pts[j].z - rd.pts[j - 1].z);
                for (auto [sc, half] : rd.cross) if (std::fabs(sj - sc) < half + 4.f) wet[j] = 1;
            }
        }
        auto runs = [&](auto pred, float minLen, auto fn) {
            int j = 0;
            while (j < N) {
                if (!pred(j)) { ++j; continue; }
                int a = j;
                while (j + 1 < N && pred(j + 1)) ++j;
                int b = j;
                float len = 5.f * (b - a + 1);
                if (len >= minLen) fn(a, b);
                ++j;
            }
        };
        // deep under the land, with short gaps between closed up (one tunnel, not a string of them)
        std::vector<uint8_t> deep(N, 0);
        for (int j = 0; j < N; ++j) deep[j] = g[j] - rd.pts[j].y > TUNNEL_DEPTH;
        for (int j = 0, last = -1; j < N; ++j) {
            if (!deep[j]) continue;
            if (last >= 0 && j - last > 1 && j - last <= 12)
                for (int q = last + 1; q < j; ++q) deep[q] = g[q] - rd.pts[q].y > 2.f;
            last = j;
        }
        runs([&](int j) { return deep[j] != 0; }, 40.f, [&](int a, int b) {
            for (int q = a; q <= b; ++q) rd.pts[q].kind = 1;
            M.tunnels.push_back({int(r), a, b});
        });
        auto bridgeRun = [&](int a, int b) {
            // a little onto the banks either side
            a = std::max(0, a - 1);
            b = std::min(N - 1, b + 1);
            for (int q = a; q <= b; ++q) if (rd.pts[q].kind == 0) rd.pts[q].kind = 2;
            int m = (a + b) / 2;
            float lvl = 1e9f, width = 0;
            for (int q = a; q <= b; ++q) lvl = std::min(lvl, g[q]);
            // the river it crosses, if any: its surface and width
            for (const River& Rv : M.rivers) {
                if (Rv.dry) continue;
                for (const RiverPt& rp : Rv.pts)
                    if (std::fabs(rp.x - rd.pts[m].x) < 5.f * (b - a + 2) && std::fabs(rp.z - rd.pts[m].z) < 5.f * (b - a + 2)) {
                        size_t sg;
                        float tt;
                        if (nearestOn(rd.pts, rp.x, rp.z, sg, tt) < rp.half + 2.f && int(sg) >= a - 1 && int(sg) <= b) { lvl = std::min(lvl, rp.level); width = std::max(width, 2 * rp.half); }
                    }
            }
            float hx = rd.pts[std::min(b, N - 1)].x - rd.pts[a].x, hz = rd.pts[std::min(b, N - 1)].z - rd.pts[a].z, hl = std::max(1e-3f, std::hypot(hx, hz));
            Bridge br;
            br.x = rd.pts[m].x; br.z = rd.pts[m].z; br.roadY = rd.pts[m].y;
            br.level = lvl; br.span = 5.f * (b - a) + 2.f; br.width = width;
            br.hx = hx / hl; br.hz = hz / hl;
            br.road = int(r); br.i0 = a; br.i1 = b;
            M.bridges.push_back(br);
        };
        // river crossings: across the water and on over low banks (up to ~80 m each side)
        runs([&](int j) { return rd.pts[j].kind == 0 && wet[j]; }, 1.f, [&](int a, int b) {
            for (int q = 0; q < 16 && a > 0 && rd.pts[a - 1].kind == 0 && rd.pts[a - 1].y - g[a - 1] > 4.f; ++q) --a;
            for (int q = 0; q < 16 && b < N - 1 && rd.pts[b + 1].kind == 0 && rd.pts[b + 1].y - g[b + 1] > 4.f; ++q) ++b;
            bridgeRun(a, b);
        });
        // over dry land: a real viaduct (high, a while), else just an embankment (the land is
        // filled up to it)
        runs([&](int j) { return rd.pts[j].kind == 0 && rd.pts[j].y - g[j] > BRIDGE_CLEAR; }, 30.f, [&](int a, int b) {
            float peak = 0;
            for (int q = a; q <= b; ++q) peak = std::max(peak, rd.pts[q].y - g[q]);
            if (peak >= 15.f) bridgeRun(a, b);
        });
    }

    // ---------------------------------------------------------------- cut and fill
    // where the road is on the land, the grid is shaped to it (the fine formation is analytic,
    // at sample time); tunnels and bridges leave the land alone
    {
        const size_t NN = size_t(n) * n;
        std::vector<float> nd(NN, 1e9f), ny(NN, 0.f);
        std::vector<uint8_t> free(NN, 0);
        const float reach = 40.f;
        for (const Road& rd : M.roads)
            for (size_t j = 0; j < rd.pts.size(); j += 2) {
                const RoadPt& p = rd.pts[j];
                int ci = int(std::lround((p.x - M.x0) / cell)), ck = int(std::lround((p.z - M.z0) / cell));
                int rr = int(std::ceil(reach / cell));
                for (int dk = -rr; dk <= rr; ++dk)
                    for (int di = -rr; di <= rr; ++di) {
                        int a = ci + di, b = ck + dk;
                        if (a < 0 || b < 0 || a >= n || b >= n) continue;
                        float d = std::hypot(X(a) - p.x, Z(b) - p.z);
                        size_t c = R.id(a, b);
                        if (p.kind != 0) { if (d < 14.f) free[c] = 1; continue; }
                        if (d < nd[c]) { nd[c] = d; ny[c] = p.y; }
                    }
            }
        for (size_t c = 0; c < NN; ++c) {
            if (nd[c] > reach || R.lake[c] || free[c] || R.riverW[c] > 0) continue;
            float w0 = HW + 3.f;
            float lim = std::max(0.f, nd[c] - w0) * 0.7f;
            float target = ny[c] - 0.3f;
            float shaped = target + std::clamp(M.h[c] - target, -lim, lim);
            M.h[c] = lerpf(shaped, M.h[c], sstep(12.f, 34.f, nd[c]));
        }
    }
    for (size_t t = 0; t < M.towns.size(); ++t) {
        Town& T = M.towns[t];
        float best = 1e9f;
        int bn = -1;
        for (size_t i = 0; i < M.nodes.size(); ++i) {
            float d = std::hypot(M.nodes[i].x - T.x, M.nodes[i].z - T.z);
            if (d < best) { best = d; bn = int(i); }
        }
        if (bn >= 0 && best < 120.f && M.nodes[bn].town < 0) { M.nodes[bn].town = int(t); T.y = M.nodes[bn].y; }
    }
    // culverts: a stream crossing a road with no bridge runs under it, below the formation
    M.index();
    for (River& Rv : M.rivers) {
        bool changed = false;
        for (RiverPt& p : Rv.pts) {
            float px, pz, py;
            float d = M.roadNearest(p.x, p.z, px, pz, py);
            if (d > HW + p.half + 6.f) continue;
            bool bridged = false;
            for (const Bridge& b : M.bridgesNear(p.x, p.z, 60.f)) bridged |= std::hypot(b.x - p.x, b.z - p.z) < b.span * 0.5f + 6.f;
            if (bridged) continue;
            float cap = py - 1.2f - p.depth;
            if (p.level > cap) { p.level = cap; changed = true; }
        }
        if (changed)
            for (size_t j = 1; j < Rv.pts.size(); ++j) Rv.pts[j].level = std::min(Rv.pts[j].level, Rv.pts[j - 1].level - 0.001f);
    }
    float km = 0, tkm = 0;
    for (const Road& rd : M.roads) km += rd.length / 1000.f;
    for (const Tunnel& t : M.tunnels) tkm += 5.f * (t.i1 - t.i0) / 1000.f;
    M.stats.roads = int(M.roads.size());
    M.stats.roadKm = km;
    M.stats.bridges = int(M.bridges.size());
    M.stats.tunnels = int(M.tunnels.size());
    M.stats.tunnelKm = tkm;
}

// ==================================================================== loops on the network
std::vector<RoadPt> Map::loop(float targetKm, uint32_t seed, float sx, float sz, std::string* name) const {
    std::vector<RoadPt> out;
    if (nodes.empty() || roads.empty()) return out;
    const float L = targetKm * 1000.f;
    auto dijkstra = [&](int src, const std::vector<float>& pen, std::vector<float>& dist, std::vector<int>& via) {
        dist.assign(nodes.size(), 1e30f);
        via.assign(nodes.size(), -1);
        using QE = std::pair<float, int>;
        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> pq;
        dist[src] = 0;
        pq.push({0, src});
        while (!pq.empty()) {
            auto [d, u] = pq.top();
            pq.pop();
            if (d > dist[u]) continue;
            for (int r : nodes[u].roads) {
                const Road& rd = roads[r];
                int v = rd.a == u ? rd.b : rd.a;
                float nd = d + rd.length * pen[r];
                if (nd < dist[v]) { dist[v] = nd; via[v] = r; pq.push({nd, v}); }
            }
        }
    };
    auto pathTo = [&](int src, int dst, const std::vector<int>& via) {
        std::vector<int> rs;
        int u = dst;
        while (u != src && via[u] >= 0) {
            int r = via[u];
            rs.push_back(r);
            u = roads[r].a == u ? roads[r].b : roads[r].a;
        }
        std::reverse(rs.begin(), rs.end());
        return u == src ? rs : std::vector<int>{};
    };
    // starts: the towns and junctions nearest the requested point
    std::vector<std::pair<float, int>> starts;
    for (size_t i = 0; i < nodes.size(); ++i)
        if (!nodes[i].roads.empty())   // through-places: a dead end would start the loop with a U-turn
            starts.push_back({std::hypot(nodes[i].x - sx, nodes[i].z - sz) * (nodes[i].town >= 0 ? 0.7f : 1.f) * (nodes[i].roads.size() < 2 ? 4.f : 1.f), int(i)});
    if (starts.empty()) return out;
    std::sort(starts.begin(), starts.end());
    if (starts.size() > 8) starts.resize(8);
    std::mt19937 rng(seed * 2654435761u + 99u);
    std::vector<int> bestRoads;
    float bestScore = 1e30f;
    int A = starts[0].second;
    for (auto [sd, a] : starts) {
        std::vector<float> pen(roads.size(), 1.f), d1;
        std::vector<int> v1;
        dijkstra(a, pen, d1, v1);
        std::vector<int> cand;
        for (size_t i = 0; i < nodes.size(); ++i)
            if (int(i) != a && d1[i] < 1e29f && d1[i] > 0.15f * L && d1[i] < 0.6f * L) cand.push_back(int(i));
        std::shuffle(cand.begin(), cand.end(), rng);
        if (cand.size() > 16) cand.resize(16);
        for (int B : cand) {
            auto p1 = pathTo(a, B, v1);
            if (p1.empty()) continue;
            std::vector<float> pen2(roads.size(), 1.f);
            for (int r : p1) pen2[r] = 6.f;
            std::vector<float> d2;
            std::vector<int> v2;
            dijkstra(B, pen2, d2, v2);
            auto p2 = pathTo(B, a, v2);
            if (p2.empty()) continue;
            float len = 0, overlap = 0;
            std::set<int> s1(p1.begin(), p1.end());
            for (int r : p1) len += roads[r].length;
            for (int r : p2) { len += roads[r].length; if (s1.count(r)) overlap += 2 * roads[r].length; }
            // a lollipop (a stem ridden both ways to a loop) is fine; a pure out-and-back is not
            float score = std::fabs(len - L) / L + 1.5f * overlap / len + 0.15f * sd / 5000.f;
            if (overlap > 0.97f * len) score += 2.f;
            if (score < bestScore) {
                bestScore = score;
                A = a;
                bestRoads = p1;
                bestRoads.insert(bestRoads.end(), p2.begin(), p2.end());
            }
        }
    }
    if (bestRoads.empty()) return out;
    // stitch the polylines in travel order
    int u = A;
    std::vector<RoadPt> pts;
    for (int r : bestRoads) {
        const Road& rd = roads[r];
        bool fwd = rd.a == u;
        if (fwd) for (size_t j = 0; j < rd.pts.size(); ++j) { if (!pts.empty() && j == 0) continue; pts.push_back(rd.pts[j]); }
        else for (size_t j = rd.pts.size(); j-- > 0;) { if (!pts.empty() && j + 1 == rd.pts.size()) continue; pts.push_back(rd.pts[j]); }
        u = fwd ? rd.b : rd.a;
    }
    // round the corners at junctions a little and resample
    chaikin(pts, 1);
    out = resample(pts, 4.f);
    if (out.size() > 2 && std::hypot(out.front().x - out.back().x, out.front().z - out.back().z) < 1.f) out.pop_back();
    if (name) {
        int ta = nodes[A].town;
        std::string nm = ta >= 0 ? towns[ta].name : "the crossroads";
        *name = "Loop from " + nm;
    }
    return out;
}

}  // namespace wmap
