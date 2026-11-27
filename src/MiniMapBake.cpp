// MiniMap's bake: the whole generated world as one stylised topographic image.
// Pure CPU (wmap + raylib's Image types only), so tools/world_map can write it as a PNG too.
//
// Layers, bottom to top:
//   ground   the menu preview's biome palette (MapPreview), sand, snow by temperature,
//            settlements as a warm tint, a soft hillshade from the north-west
//   contours every I metres (I from the map's relief), every 5th bolder; minor ones fade
//            where they would crowd closer than ~25 m
//   water    lakes from the grid (shore from the per-pixel height), salt flats, rivers
//   roads    tunnels dashed, then casings (bridges orange), minor fills, main fills
#include "MiniMap.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

#include "world/WorldMap.h"

namespace {
// stylised palette per biome (Meadow, Forest, Boreal, Alpine, Steppe, Savanna, Scrub, Dunes, Badlands, Wetland) — as MapPreview
const float PAL[wmap::BIOMES][3] = {
    {132, 182, 86}, {62, 122, 66}, {78, 116, 94}, {184, 182, 176}, {200, 190, 122},
    {208, 180, 98}, {164, 152, 96}, {236, 210, 154}, {204, 124, 82}, {96, 146, 122},
};
const unsigned char PAPER[3] = {236, 233, 224};
struct C3 { float r, g, b; };
const C3 RIVER{92, 156, 214}, LAKE{104, 168, 222}, LAKE_DEEP{70, 128, 196}, SHORE{64, 116, 178}, SALT{240, 234, 216};
const C3 MAIN_FILL{255, 210, 92}, MAIN_CASE{70, 62, 50}, MINOR_FILL{252, 252, 250}, MINOR_CASE{78, 82, 92};
const C3 BRIDGE_CASE{214, 112, 52}, TUNNEL{120, 124, 132}, CONTOUR{96, 64, 38};

template <class F>
void parallelRows(int rows, F f) {
    int nt = std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
    std::vector<std::thread> th;
    for (int t = 0; t < nt; ++t)
        th.emplace_back([&, t] {
            for (int k = t; k < rows; k += nt) f(k);
        });
    for (auto& x : th) x.join();
}

// anti-aliased thick polylines into a coverage buffer (max-blended)
struct Cover {
    int R;
    float x0, z0, mpp;   // pixel i covers world x0 + i*mpp .. x0 + (i+1)*mpp
    std::vector<unsigned char> c;
    void clear() { std::fill(c.begin(), c.end(), 0); }
    void seg(float ax, float az, float bx, float bz, float halfPx) {
        // to pixel space (pixel centres at i + 0.5)
        float px0 = (ax - x0) / mpp, pz0 = (az - z0) / mpp, px1 = (bx - x0) / mpp, pz1 = (bz - z0) / mpp;
        float e = halfPx + 1.f;
        int i0 = std::max(0, int(std::floor(std::min(px0, px1) - e))), i1 = std::min(R - 1, int(std::ceil(std::max(px0, px1) + e)));
        int k0 = std::max(0, int(std::floor(std::min(pz0, pz1) - e))), k1 = std::min(R - 1, int(std::ceil(std::max(pz0, pz1) + e)));
        float dx = px1 - px0, dz = pz1 - pz0, l2 = std::max(1e-6f, dx * dx + dz * dz);
        for (int k = k0; k <= k1; ++k)
            for (int i = i0; i <= i1; ++i) {
                float qx = i + 0.5f - px0, qz = k + 0.5f - pz0;
                float t = std::clamp((qx * dx + qz * dz) / l2, 0.f, 1.f);
                float ex = qx - t * dx, ez = qz - t * dz;
                float d = std::sqrt(ex * ex + ez * ez);
                float cov = std::clamp(halfPx + 0.5f - d, 0.f, 1.f);
                if (cov <= 0) continue;
                unsigned char v = (unsigned char)(cov * 255.f + 0.5f);
                unsigned char& o = c[size_t(k) * R + i];
                if (v > o) o = v;
            }
    }
    template <class P>
    void line(const std::vector<P>& pts, float halfPx) {
        for (size_t j = 0; j + 1 < pts.size(); ++j) seg(pts[j].x, pts[j].z, pts[j + 1].x, pts[j + 1].z, halfPx);
    }
};
}  // namespace

std::shared_ptr<MiniMap::Bake> miniMapBake(const wmap::Map& M, int maxRes) {
    auto B = std::make_shared<MiniMap::Bake>();
    const float L = (M.n - 1) * M.cell;
    // ~5 m a pixel, at most maxRes (a 40 km world: 10 m)
    const int R = std::clamp(int(std::ceil(L / 5.f)), 512, std::max(512, maxRes));
    const float mpp = L / R;
    B->R = R; B->L = L; B->x0 = M.x0; B->z0 = M.z0;
    const size_t NP = size_t(R) * R;

    // ---- per-pixel height (bicubic grid)
    std::vector<float> H(NP);
    parallelRows(R, [&](int k) {
        float z = M.z0 + (k + 0.5f) * mpp;
        for (int i = 0; i < R; ++i) H[size_t(k) * R + i] = M.height(M.x0 + (i + 0.5f) * mpp, z);
    });
    float hmin = 1e9f, hmax = -1e9f;
    for (float h : H) { hmin = std::min(hmin, h); hmax = std::max(hmax, h); }
    const float relief = hmax - hmin;
    const float I = relief < 250 ? 5.f : relief < 700 ? 10.f : relief < 1800 ? 20.f : 40.f;   // contour interval (m)

    // ---- ground colour per grid point (palette, sand, snow, damp)
    const int n = M.n;
    std::vector<C3> G(size_t(n) * n);
    parallelRows(n, [&](int k) {
        for (int i = 0; i < n; ++i) {
            size_t q = size_t(k) * n + i;
            float w[wmap::BIOMES];
            M.biomeAt(M.x0 + i * M.cell, M.z0 + k * M.cell, w);
            float r = 0, g = 0, b = 0;
            for (int t = 0; t < wmap::BIOMES; ++t) { r += PAL[t][0] * w[t]; g += PAL[t][1] * w[t]; b += PAL[t][2] * w[t]; }
            float sand = M.sand.empty() ? 0 : M.sand[q];
            r += (232 - r) * sand * 0.6f; g += (208 - g) * sand * 0.6f; b += (156 - b) * sand * 0.6f;
            float moist = M.moist.empty() ? 0.5f : M.moist[q];
            if (moist > 0.75f) { float f = std::min(1.f, (moist - 0.75f) * 4.f); r *= 1 - 0.08f * f; g *= 1 - 0.03f * f; }
            float temp = M.temp.empty() ? 10 : M.temp[q];
            float snow = std::clamp((-0.5f - temp) / 2.f, 0.f, 1.f);
            r += (246 - r) * snow; g += (248 - g) * snow; b += (252 - b) * snow;
            G[q] = {r, g, b};
        }
    });
    auto lakeAt = [&](int i, int k) -> int {
        if (M.lakeId.empty()) return -1;
        return M.lakeId[size_t(std::clamp(k, 0, n - 1)) * n + std::clamp(i, 0, n - 1)];
    };

    // ---- settlements: a warm tint around each place
    std::vector<unsigned char> town(NP, 0);
    for (const auto& t : M.towns) {
        float rad = t.rank >= 3 ? 260.f : t.rank >= 2 ? 170.f : 90.f;
        int ci = int((t.x - M.x0) / mpp), ck = int((t.z - M.z0) / mpp), rp = int(rad / mpp) + 2;
        for (int k = std::max(0, ck - rp); k <= std::min(R - 1, ck + rp); ++k)
            for (int i = std::max(0, ci - rp); i <= std::min(R - 1, ci + rp); ++i) {
                float d = std::hypot((i - ci) * mpp, (k - ck) * mpp);
                float a = std::clamp((rad - d) / (rad * 0.35f), 0.f, 1.f);
                unsigned char v = (unsigned char)(a * 255);
                if (v > town[size_t(k) * R + i]) town[size_t(k) * R + i] = v;
            }
    }

    // ---- ground, hillshade, contours, lakes
    B->rgba.assign(NP * 4, 255);
    unsigned char* px = B->rgba.data();
    const float lx = -0.55f, ly = 0.75f, lz = -0.45f;
    const float ll = std::sqrt(lx * lx + ly * ly + lz * lz);
    parallelRows(R, [&](int k) {
        for (int i = 0; i < R; ++i) {
            size_t p = size_t(k) * R + i;
            float h = H[p];
            auto Hp = [&](int a, int b) { return H[size_t(std::clamp(b, 0, R - 1)) * R + std::clamp(a, 0, R - 1)]; };
            float gx = (Hp(i + 1, k) - Hp(i - 1, k)) / (2 * mpp), gz = (Hp(i, k + 1) - Hp(i, k - 1)) / (2 * mpp);   // m per m
            // ground colour (bilinear from the grid)
            float fx = std::clamp(((i + 0.5f) * mpp) / M.cell, 0.f, float(n - 1) - 1e-3f), fz = std::clamp(((k + 0.5f) * mpp) / M.cell, 0.f, float(n - 1) - 1e-3f);
            int gi = int(fx), gk = int(fz);
            float tx = fx - gi, tz = fz - gk;
            const C3 &a = G[size_t(gk) * n + gi], &b = G[size_t(gk) * n + gi + 1], &c = G[size_t(gk + 1) * n + gi], &d = G[size_t(gk + 1) * n + gi + 1];
            auto bl = [&](float C3::*m) { return (a.*m * (1 - tx) + b.*m * tx) * (1 - tz) + (c.*m * (1 - tx) + d.*m * tx) * tz; };
            float r = bl(&C3::r), g = bl(&C3::g), bb = bl(&C3::b);
            // settlements
            if (float tw = town[p] / 255.f; tw > 0) {
                r += (224 - r) * tw * 0.55f; g += (210 - g) * tw * 0.55f; bb += (194 - bb) * tw * 0.55f;
            }
            // hillshade (relief exaggerated a little so gentle country still reads), higher ground a touch lighter
            {
                float ex = 1.6f;
                float nx = -gx * ex, nz = -gz * ex, nl = std::sqrt(nx * nx + 1 + nz * nz);
                float dot = (nx * lx + ly + nz * lz) / (nl * ll);
                float flat = ly / ll;
                float sh = std::clamp(1.f + 0.85f * (dot - flat), 0.58f, 1.18f);
                float lift = 0.95f + 0.08f * std::clamp((h - hmin) / std::max(1.f, relief), 0.f, 1.f);
                r *= sh * lift; g *= sh * lift; bb *= sh * lift;
            }
            // lakes and salt flats: the highest lake level among the four grid points around
            float level = -1e9f;
            bool playa = false;
            for (int q = 0; q < 4; ++q) {
                int id = lakeAt(gi + (q & 1), gk + (q >> 1));
                if (id >= 0 && id < int(M.lakes.size()) && M.lakes[id].level > level) { level = M.lakes[id].level; playa = M.lakes[id].playa; }
            }
            float depth = level - h;
            bool wet = depth > 0.05f;
            if (wet) {
                if (playa) { r = SALT.r; g = SALT.g; bb = SALT.b; }
                else {
                    float dd = std::clamp(depth / 14.f, 0.f, 1.f);
                    r = LAKE.r + (LAKE_DEEP.r - LAKE.r) * dd; g = LAKE.g + (LAKE_DEEP.g - LAKE.g) * dd; bb = LAKE.b + (LAKE_DEEP.b - LAKE.b) * dd;
                    // a darker line along the shore (about 1.2 px)
                    float grad = std::max(1e-3f, std::sqrt(gx * gx + gz * gz));
                    float dpx = depth / grad / mpp;
                    float s = std::clamp(1.7f - dpx, 0.f, 1.f) * 0.7f;
                    r += (SHORE.r - r) * s; g += (SHORE.g - g) * s; bb += (SHORE.b - bb) * s;
                }
            } else {
                // contours: distance to the nearest level in pixels, from the slope
                float grad = std::sqrt(gx * gx + gz * gz);   // m of height per m
                if (grad > 1e-4f) {
                    auto line = [&](float interval, float widthM, float alpha, float fadeM) {
                        float f = h / interval;
                        float dist = std::fabs(f - std::round(f)) * interval;   // m of height to the level
                        float dpx = dist / grad / mpp;                          // px across the ground
                        float wpx = std::max(0.9f, widthM / mpp);
                        float cov = std::clamp(wpx * 0.5f + 0.5f - dpx, 0.f, 1.f);
                        float spacingM = interval / grad;                       // ground distance between lines
                        cov *= std::clamp((spacingM - fadeM) / fadeM, 0.f, 1.f);
                        return cov * alpha;
                    };
                    float ci = line(I * 5, 12.f, 0.46f, 18.f);
                    float cm = line(I, 7.f, 0.24f, 24.f);
                    float cv = std::max(ci, cm);
                    if (cv > 0) {
                        r += (CONTOUR.r * r / 255 * 1.4f - r) * cv; g += (CONTOUR.g * g / 255 * 1.4f - g) * cv; bb += (CONTOUR.b * bb / 255 * 1.4f - bb) * cv;
                    }
                }
            }
            unsigned char* o = px + p * 4;
            o[0] = (unsigned char)std::clamp(int(r), 0, 255);
            o[1] = (unsigned char)std::clamp(int(g), 0, 255);
            o[2] = (unsigned char)std::clamp(int(bb), 0, 255);
            o[3] = 255;
        }
    });

    // ---- vector layers
    Cover cov{R, M.x0, M.z0, mpp, std::vector<unsigned char>(NP, 0)};
    auto paint = [&](C3 col) {
        parallelRows(R, [&](int k) {
            for (int i = 0; i < R; ++i) {
                size_t p = size_t(k) * R + i;
                unsigned char cv = cov.c[p];
                if (!cv) continue;
                float a = cv / 255.f;
                unsigned char* o = px + p * 4;
                o[0] = (unsigned char)(o[0] + (col.r - o[0]) * a);
                o[1] = (unsigned char)(o[1] + (col.g - o[1]) * a);
                o[2] = (unsigned char)(o[2] + (col.b - o[2]) * a);
            }
        });
        cov.clear();
    };
    auto hpx = [&](float metres) { return std::max(0.7f, metres / mpp); };
    // rivers: wide ones from their channel, small ones still a visible line
    for (const auto& rv : M.rivers) {
        if (rv.dry || rv.pts.size() < 2) continue;
        for (size_t j = 0; j + 1 < rv.pts.size(); ++j) {
            float half = std::clamp(0.5f * (rv.pts[j].half + rv.pts[j + 1].half), 6.f, 40.f);
            cov.seg(rv.pts[j].x, rv.pts[j].z, rv.pts[j + 1].x, rv.pts[j + 1].z, hpx(half));
        }
    }
    paint(RIVER);
    // roads split into runs by kind (0 land, 1 tunnel, 2 bridge)
    struct Run { const wmap::Road* road; int kind; size_t a, b; };   // points a..b inclusive
    std::vector<Run> runs;
    for (const auto& rd : M.roads) {
        if (rd.pts.size() < 2) continue;
        size_t s = 0;
        for (size_t j = 1; j <= rd.pts.size(); ++j)
            if (j == rd.pts.size() || rd.pts[j].kind != rd.pts[s].kind) {
                runs.push_back({&rd, rd.pts[s].kind, s, std::min(j, rd.pts.size() - 1)});   // ends on the next run's first point
                s = j;
            }
    }
    const float MAIN_HALF = 20.f, MINOR_HALF = 14.f, CASE = 10.f;
    auto runLine = [&](const Run& r, float half, bool dashed) {
        float acc = 0;
        for (size_t j = r.a; j < r.b; ++j) {
            const auto &p = r.road->pts[j], &q = r.road->pts[j + 1];
            float l = std::hypot(q.x - p.x, q.z - p.z);
            if (!dashed || std::fmod(acc, 90.f) < 50.f) cov.seg(p.x, p.z, q.x, q.z, hpx(half));
            acc += l;
        }
    };
    for (const auto& r : runs) if (r.kind == 1) runLine(r, (r.road->cls == 0 ? MAIN_HALF : MINOR_HALF) * 0.75f, true);
    paint(TUNNEL);
    for (int cls = 1; cls >= 0; --cls) {
        for (const auto& r : runs) if (r.kind != 1 && r.road->cls == cls) runLine(r, (cls == 0 ? MAIN_HALF : MINOR_HALF) + CASE, false);
        paint(cls == 0 ? MAIN_CASE : MINOR_CASE);
    }
    for (const auto& r : runs) if (r.kind == 2) runLine(r, (r.road->cls == 0 ? MAIN_HALF : MINOR_HALF) + CASE * 1.2f, false);
    paint(BRIDGE_CASE);
    for (int cls = 1; cls >= 0; --cls) {
        for (const auto& r : runs) if (r.kind != 1 && r.road->cls == cls) runLine(r, cls == 0 ? MAIN_HALF : MINOR_HALF, false);
        paint(cls == 0 ? MAIN_FILL : MINOR_FILL);
    }

    // ---- a paper-coloured border, so clamping at the texture edge shows paper beyond the world
    for (int k = 0; k < R; ++k)
        for (int i = 0; i < R; ++i) {
            if (i > 1 && k > 1 && i < R - 2 && k < R - 2) continue;
            unsigned char* o = px + (size_t(k) * R + i) * 4;
            o[0] = PAPER[0]; o[1] = PAPER[1]; o[2] = PAPER[2];
        }
    return B;
}

bool MiniMap::bakeToPng(const wmap::Map& m, const std::string& path, int maxRes) {
    auto B = miniMapBake(m, maxRes);
    Image img{B->rgba.data(), B->R, B->R, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    return ExportImage(img, path.c_str());
}
