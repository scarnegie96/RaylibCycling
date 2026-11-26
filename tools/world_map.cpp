// world_map: generate a world and write maps of it (PNG).
//   world_map [--seed N] [--size M] [--res M] [--aridity A] [--erosion E] [--loop KM] [--out PREFIX] [--cache FILE]
// Writes PREFIX_terrain.png (biomes, relief, water, roads, towns), PREFIX_biome.png,
// PREFIX_hydro.png (discharge, lakes, salt flats, washes) and PREFIX_climate.png.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "raylib.h"
#include "world/WorldMap.h"

using namespace wmap;

static const unsigned char BCOL[BIOMES][3] = {
    {118, 165, 72},   // meadow
    {46, 104, 50},    // forest
    {70, 104, 84},    // boreal
    {176, 176, 170},  // alpine
    {190, 180, 112},  // steppe
    {196, 168, 84},   // savanna
    {150, 140, 86},   // scrub
    {232, 204, 146},  // dunes
    {192, 112, 72},   // badlands
    {84, 132, 112},   // wetland
};

struct Img {
    int w, h;
    std::vector<unsigned char> px;
    Img(int W, int H) : w(W), h(H), px(size_t(W) * H * 3, 0) {}
    void set(int x, int y, float r, float g, float b) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        unsigned char* p = &px[(size_t(y) * w + x) * 3];
        p[0] = (unsigned char)std::clamp(int(r), 0, 255);
        p[1] = (unsigned char)std::clamp(int(g), 0, 255);
        p[2] = (unsigned char)std::clamp(int(b), 0, 255);
    }
    void dot(int cx, int cy, int r, float R, float G, float B) {
        for (int y = -r; y <= r; ++y)
            for (int x = -r; x <= r; ++x)
                if (x * x + y * y <= r * r) set(cx + x, cy + y, R, G, B);
    }
    void save(const std::string& path) {
        Image im{px.data(), w, h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8};
        ExportImage(im, path.c_str());
    }
};

int main(int argc, char** argv) {
    Params P;
    float res = 10.f, loopKm = 0, cx = 0, cz = 0, view = 0;
    std::string out = "world", cache;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--seed") P.seed = uint32_t(std::stoul(next()));
        else if (a == "--size") P.size = std::stof(next());
        else if (a == "--res") res = std::stof(next());
        else if (a == "--aridity") P.aridity = std::stof(next());
        else if (a == "--erosion") P.erosion = std::stof(next());
        else if (a == "--climate") P.climate = std::stoi(next());
        else if (a == "--noroads") P.roads = false;
        else if (a == "--loop") loopKm = std::stof(next());
        else if (a == "--out") out = next();
        else if (a == "--center") { cx = std::stof(next()); cz = std::stof(next()); }
        else if (a == "--view") view = std::stof(next());
        else if (a == "--cache") cache = next();
    }
    SetTraceLogLevel(LOG_WARNING);
    auto t0 = std::chrono::steady_clock::now();
    std::shared_ptr<Map> M;
    if (!cache.empty()) {
        M = std::make_shared<Map>();
        if (!M->load(cache) || M->params.seed != P.seed) M.reset();
        else std::printf("loaded %s\n", cache.c_str());
    }
    if (!M) {
        M = generate(P, [](const char* s, float f) { std::printf("  %-10s %3.0f%%\n", s, f * 100); std::fflush(stdout); });
        if (!cache.empty()) std::printf("saved cache: %s\n", M->save(cache) ? "ok" : "FAILED");
    }
    double genMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const auto& S = M->stats;
    std::printf("seed %u: %dx%d grid, %.1f s  [relief %.0f, climate %.0f, landforms %.0f, erosion %.0f, hydrology %.0f, roads %.0f, index %.0f ms]\n",
                P.seed, M->n, M->n, genMs / 1000, S.ms[0], S.ms[1], S.ms[2], S.ms[3], S.ms[4], S.ms[5], S.ms[6]);
    std::printf("  %d droplets, %d rivers, %d lakes, %d salt flats, %d towns, %d roads (%.0f km), %d bridges, %d tunnels (%.1f km), snow line %.0f m, wind (%.2f, %.2f)\n",
                S.droplets, S.rivers, S.lakes, S.playas, S.towns, S.roads, S.roadKm, S.bridges, S.tunnels, S.tunnelKm, M->snowLine, M->windX, M->windZ);
    double share[BIOMES] = {};
    for (const auto& b : M->biome) for (int q = 0; q < BIOMES; ++q) share[q] += b[q];
    std::printf("  biomes:");
    for (int q = 0; q < BIOMES; ++q) std::printf(" %s %.0f%%", biomeName(q), 100.0 * share[q] / M->biome.size());
    std::printf("\n");
    float hmin = 1e9f, hmax = -1e9f;
    for (float v : M->h) { hmin = std::min(hmin, v); hmax = std::max(hmax, v); }
    std::printf("  height %.0f .. %.0f m\n", hmin, hmax);
    for (const Town& t : M->towns) std::printf("  %s %-18s (%6.0f, %6.0f) %4.0f m\n", t.rank == 2 ? "town   " : "village", t.name.c_str(), t.x, t.z, t.y);

    // sample the whole map at the requested resolution
    if (view <= 0) { view = P.size; cx = 0; cz = 0; }
    const float vx0 = cx - view / 2, vz0 = cz - view / 2;
    const int W = int(view / res);
    std::vector<Sample> sm(size_t(W) * W);
    auto ts = std::chrono::steady_clock::now();
    {
        std::atomic<int> row{0};
        std::vector<std::thread> th;
        unsigned T = std::max(1u, std::thread::hardware_concurrency());
        for (unsigned t = 0; t < T; ++t)
            th.emplace_back([&] {
                for (int y; (y = row++) < W;)
                    for (int x = 0; x < W; ++x) sm[size_t(y) * W + x] = M->sample(vx0 + (x + 0.5f) * res, vz0 + (y + 0.5f) * res);
            });
        for (auto& t : th) t.join();
    }
    double sampMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - ts).count();
    std::printf("  sampled %dx%d in %.1f s (%.2f us/sample)\n", W, W, sampMs / 1000, sampMs * 1000 / (double(W) * W));
    auto H = [&](int x, int y) { x = std::clamp(x, 0, W - 1); y = std::clamp(y, 0, W - 1); return sm[size_t(y) * W + x].h; };
    auto shade = [&](int x, int y) {
        float dx = (H(x + 1, y) - H(x - 1, y)) / (2 * res), dy = (H(x, y + 1) - H(x, y - 1)) / (2 * res);
        float nx = -dx, ny = 1.f, nz = -dy, l = std::sqrt(nx * nx + ny * ny + nz * nz);
        float d = (nx * -0.55f + ny * 0.7f + nz * -0.45f) / l;
        return std::clamp(0.35f + 0.85f * d, 0.f, 1.3f);
    };
    Img terrain(W, W), bio(W, W), hydro(W, W), clim(W, W);
    for (int y = 0; y < W; ++y)
        for (int x = 0; x < W; ++x) {
            const Sample& s = sm[size_t(y) * W + x];
            float sh = shade(x, y);
            float r = 0, g = 0, b = 0;
            for (int q = 0; q < BIOMES; ++q) { r += BCOL[q][0] * s.w[q]; g += BCOL[q][1] * s.w[q]; b += BCOL[q][2] * s.w[q]; }
            bio.set(x, y, r * (0.55f + 0.45f * sh), g * (0.55f + 0.45f * sh), b * (0.55f + 0.45f * sh));
            // terrain: biome tint, lightened with height, snow, sand, water, roads
            float alt = std::clamp((s.h - hmin) / std::max(1.f, hmax - hmin), 0.f, 1.f);
            float tr = r * (0.85f + 0.3f * alt), tg = g * (0.85f + 0.3f * alt), tb = b * (0.85f + 0.3f * alt);
            float sand = std::clamp(s.sand, 0.f, 1.f);
            tr = tr + (226 - tr) * sand * 0.6f; tg = tg + (200 - tg) * sand * 0.6f; tb = tb + (150 - tb) * sand * 0.6f;
            if (s.temp < -1.5f) { tr = 240; tg = 242; tb = 246; }
            if (s.sediment > 0.5f) { tr = tr * 0.6f + 230 * 0.4f; tg = tg * 0.6f + 220 * 0.4f; tb = tb * 0.6f + 200 * 0.4f; }
            tr *= sh; tg *= sh; tb *= sh;
            if (s.water > s.h) { float d = std::min(1.f, (s.water - s.h) / 4.f); tr = 70 - 30 * d; tg = 120 - 30 * d; tb = 190 - 20 * d; }
            if (s.road < 3.6f) { tr = tg = tb = s.roadCls == 0 ? 40.f : 70.f; }
            else if (s.road < 5.f) { tr = tg = tb = 235.f; }
            terrain.set(x, y, tr, tg, tb);
            // hydrology: discharge on a grey relief
            float gq = 70 + 90 * sh;
            float hr = gq, hg = gq, hb = gq;
            if (s.sediment > 0.4f) { hr = 210; hg = 180; hb = 120; }
            if (s.water > s.h) { float lq = std::clamp(std::log10(std::max(0.01f, s.waterW)) * 0.5f + 0.3f, 0.f, 1.f); hr = 30; hg = 90 + 80 * lq; hb = 220; }
            hydro.set(x, y, hr, hg, hb);
            // climate: precipitation hue (brown dry .. blue wet), temperature as banding
            int gi = std::clamp(int((vx0 + (x + 0.5f) * res - M->x0) / M->cell), 0, M->n - 1);
            int gk = std::clamp(int((vz0 + (y + 0.5f) * res - M->z0) / M->cell), 0, M->n - 1);
            float pmm = M->precip.empty() ? 500 : M->precip[size_t(gk) * M->n + gi];
            float pw = std::clamp(std::log(pmm / 80.f) / std::log(2500.f / 80.f), 0.f, 1.f);
            float band = std::fmod(std::fabs(s.temp), 2.f) < 0.12f ? 0.65f : 1.f;
            clim.set(x, y, (200 - 160 * pw) * sh * band, (160 - 40 * pw + 40 * pw) * sh * band, (90 + 150 * pw) * sh * band);
        }
    // towns
    for (const Town& t : M->towns) {
        int px = int((t.x - vx0) / res), py = int((t.z - vz0) / res);
        int r = t.rank == 2 ? 6 : 4;
        terrain.dot(px, py, r + 2, 20, 20, 20);
        terrain.dot(px, py, r, 250, 90, 60);
        bio.dot(px, py, r, 20, 20, 20);
    }
    // tunnels (dashed black) and bridges (orange) over the terrain map
    for (const Tunnel& t : M->tunnels)
        for (int j = t.i0; j <= t.i1; ++j)
            if ((j / 3) % 2 == 0) { const RoadPt& p = M->roads[t.road].pts[j]; terrain.dot(int((p.x - vx0) / res), int((p.z - vz0) / res), view < 6000 ? 2 : 1, 10, 10, 10); }
    for (const Bridge& b : M->bridges)
        for (int j = b.i0; j <= b.i1; ++j) { const RoadPt& p = M->roads[b.road].pts[j]; terrain.dot(int((p.x - vx0) / res), int((p.z - vz0) / res), view < 6000 ? 2 : 1, 255, 150, 20); }
    // a loop on the network
    if (loopKm > 0) {
        std::string name;
        auto L = M->loop(loopKm, P.seed, 0, 0, &name);
        float len = 0, gain = 0;
        for (size_t j = 1; j < L.size(); ++j) {
            len += std::hypot(L[j].x - L[j - 1].x, L[j].z - L[j - 1].z);
            gain += std::max(0.f, L[j].y - L[j - 1].y);
        }
        std::printf("  loop: %s, %.1f km, %.0f m climbing, %zu points\n", name.c_str(), len / 1000, gain, L.size());
        for (const auto& p : L) terrain.dot(int((p.x - vx0) / res), int((p.z - vz0) / res), view < 6000 ? 1 : 2, 240, 40, 200);
    }
    terrain.save(out + "_terrain.png");
    bio.save(out + "_biome.png");
    hydro.save(out + "_hydro.png");
    clim.save(out + "_climate.png");
    std::printf("wrote %s_{terrain,biome,hydro,climate}.png\n", out.c_str());
    return 0;
}
