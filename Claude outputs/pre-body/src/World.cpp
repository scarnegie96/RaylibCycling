// Streaming world: 512 m terrain chunks at four levels of detail, built on worker threads
// around the camera (so roads of any length work), vegetation per chunk, the road ribbon,
// km posts, start / finish arches and the night-time lanterns.
#include <algorithm>
#include <cmath>

#include "RenderUtil.h"
#include "Renderer.h"
#include "Profiler.h"
#include "Terrain.h"
#include "Ui.h"

using namespace rutil;

namespace {
constexpr float CS = 512.f;  // chunk size (m)
// quads per 512 m chunk by quality and detail level; the nearest level is fine enough for
// stream banks (16-bit indices allow up to 224 quads + skirts)
const int QUADS[3][4] = {{80, 48, 16, 8}, {160, 96, 24, 12}, {224, 112, 32, 16}};
const float VIEW[3] = {3800.f, 5500.f, 7500.f};
const float VEG_DIST[3] = {1400.f, 2200.f, 3200.f};
const float VEG_SPACING[3] = {12.f, 9.f, 7.f};
constexpr float LANTERN_SPACING = 100.f;  // a lamp every 100 m, alternating sides

int lodFor(float d) { return d < 700 ? 0 : d < 1700 ? 1 : d < 3600 ? 2 : 3; }
float boxDist(int cx, int cz, Vector3 f) {
    float x0 = cx * CS, z0 = cz * CS;
    float dx = std::max({x0 - f.x, 0.f, f.x - (x0 + CS)}), dz = std::max({z0 - f.z, 0.f, f.z - (z0 + CS)});
    return std::sqrt(dx * dx + dz * dz);
}
}  // namespace

struct Renderer::ChunkData {
    int cx = 0, cz = 0, lod = 0, version = 0, world = 0;
    MeshBuilder mb, water;
    Vector3 center{};
    float radius = 0;
    bool hasVeg = false, fine = false;
    std::vector<Matrix> veg[VEG_SETS];
    std::vector<Vector3> vegPos[VEG_SETS];
    std::vector<float> hgrid, rgrid, wgrid;
    int gridQ = 0;
};

// ------------------------------------------------------------------ workers
void Renderer::startWorkers() {
    quit_ = false;
    unsigned n = std::max(2u, std::min(6u, std::thread::hardware_concurrency() > 1 ? std::thread::hardware_concurrency() - 1 : 1u));
    for (unsigned i = 0; i < n; ++i) workers_.emplace_back([this] { workerLoop(); });
}

void Renderer::stopWorkers() {
    {
        std::lock_guard<std::mutex> lk(qm_);
        quit_ = true;
        jobs_.clear();
    }
    qcv_.notify_all();
    for (auto& t : workers_) if (t.joinable()) t.join();
    workers_.clear();
    for (auto* d : done_) delete d;
    done_.clear();
    inFlight_.clear();
}

void Renderer::workerLoop() {
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lk(qm_);
            qcv_.wait(lk, [&] { return quit_ || !jobs_.empty(); });
            if (quit_) return;
            j = jobs_.front();
            jobs_.pop_front();
            ++active_;
        }
        if (j.lod < 0) {  // an erosion tile: run it, nothing to upload
            j.course->field().prepareTile(j.cx, j.cz);
            std::lock_guard<std::mutex> lk(qm_);
            --active_;
            continue;
        }
        auto* d = new ChunkData(buildChunk(j, quality_));
        d->world = j.version >> 16;
        d->version = j.version & 0xFFFF;
        {
            std::lock_guard<std::mutex> lk(qm_);
            done_.push_back(d);
            --active_;
        }
    }
}

Renderer::ChunkData Renderer::buildChunk(const Job& j, int quality) {
    const Course& C = *j.course;
    ChunkData d;
    d.cx = j.cx; d.cz = j.cz; d.lod = j.lod;
    const int Q = QUADS[std::clamp(quality, 0, 2)][j.lod];
    const float step = CS / Q;
    const float x0 = j.cx * CS, z0 = j.cz * CS;
    // border of K samples: normals need one, curvature is taken over ~8 m whatever the LOD
    const int K = std::clamp(int(std::lround(8.f / step)), 1, 4);
    const int G = Q + 1 + 2 * K;
    // near chunks get the particle-eroded surface (the erosion tiles are built here, on the
    // worker, the first time any chunk needs them); far chunks the landform + rivers only
    const terrain::Field& F = C.field();
    // erosion runs as its own job; until it's done near chunks use the landform + rivers
    const bool fine = j.lod <= 1 && F.tileReady(j.cx, j.cz);
    d.fine = fine;
    std::vector<terrain::Sample> sm(size_t(G) * G);
    std::vector<float> h(size_t(G) * G), rd(size_t(G) * G);
    for (int z = 0; z < G; ++z)
        for (int x = 0; x < G; ++x) {
            float wx = x0 + (x - K) * step, wz = z0 + (z - K) * step;
            size_t id = size_t(z) * G + x;
            sm[id] = fine ? F.sample(wx, wz) : F.macro(wx, wz);
            h[id] = sm[id].h;
            rd[id] = sm[id].road;
        }
    auto H = [&](int x, int z) { return h[size_t(z + K) * G + (x + K)]; };  // x,z in -K..Q+K
    auto SM = [&](int x, int z) -> const terrain::Sample& { return sm[size_t(z + K) * G + (x + K)]; };
    float minY = 1e9f, maxY = -1e9f;
    MeshBuilder& b = d.mb;
    const float hw = C.roadHalfWidth();
    auto u8 = [](float v) { return (unsigned char)std::clamp(int(v * 255.f + 0.5f), 0, 255); };
    for (int z = 0; z <= Q; ++z)
        for (int x = 0; x <= Q; ++x) {
            float y = H(x, z);
            minY = std::min(minY, y); maxY = std::max(maxY, y);
            Vector3 nn = Vector3Normalize({H(x - 1, z) - H(x + 1, z), 2 * step, H(x, z - 1) - H(x, z + 1)});
            const terrain::Sample& q = SM(x, z);
            float shoulder = 1.f - smooth(hw + 1.f, 11.f, q.road);
            float under = q.water > y ? std::min(1.f, (q.water - y) / 3.f) : 0.f;
            // colour: road shoulder, streams, fresh sediment; uv: scoured ground, shore / underwater
            float shore = std::max(q.bank * 0.5f, under > 0.f ? 0.5f + 0.5f * under : 0.f);
            // alpha = curvature (0.5 flat, > 0.5 concave: hollows, valley floors, the foot of
            // cliffs; < 0.5 convex: ridges, spurs, crests), scaled per metre so LODs agree
            float cs = K * step;
            float lap = (H(x - K, z) + H(x + K, z) + H(x, z - K) + H(x, z + K) - 4.f * y) / (cs * cs);
            float curv = std::clamp(0.5f + lap * 14.f, 0.f, 1.f);
            b.add({x0 + x * step, y, z0 + z * step}, nn, {q.scour, shore},
                  {u8(shoulder), u8(q.wet), u8(q.sediment), u8(curv)});
            {   // low-frequency texturing fields (geology, moisture, patches, tone, strata), so the
                // terrain shader doesn't evaluate ~20 noise octaves per pixel
                const float X = x0 + x * step, Z = z0 + z * step;
                float wx = fbm(X * 0.0018f + 3.1f, Z * 0.0018f, 3, 901) - 0.5f, wz = fbm(X * 0.0018f, Z * 0.0018f + 7.7f, 3, 902) - 0.5f;
                float region = fbm(X * 0.0009f + wx * 1.6f, Z * 0.0009f + wz * 1.6f, 3, 903);
                float moist = fbm(X * 0.0024f + wx * 1.2f + 11.f, Z * 0.0024f + wz * 1.2f, 3, 904);
                float patch = fbm(X * 0.011f + wx * 2.f, Z * 0.011f + wz * 2.f, 3, 905);
                float tone = fbm(X * 0.03f + wx * 3.f, Z * 0.03f + wz * 3.f, 3, 906);
                float band = wx * 2.5f + fbm(X * 0.004f, Z * 0.004f, 3, 907) * 0.9f;
                b.extra({region, moist, patch, tone}, {band, 0});
            }
        }
    for (int z = 0; z < Q; ++z)
        for (int x = 0; x < Q; ++x) {
            unsigned short a = z * (Q + 1) + x, bb = (z + 1) * (Q + 1) + x;
            b.tri(a, bb, a + 1);
            b.tri(a + 1, bb, bb + 1);
        }
    // skirts hide cracks between neighbouring chunks of different detail
    const float skirt = step * 1.5f + 2.f;
    auto edge = [&](int x, int z) { return (unsigned short)(z * (Q + 1) + x); };
    auto addSkirt = [&](std::vector<unsigned short> e, bool flip) {
        std::vector<unsigned short> lo;
        for (auto i : e) {
            Vector3 p{b.v[i * 3], b.v[i * 3 + 1] - skirt, b.v[i * 3 + 2]};
            Vector3 nn{b.n[i * 3], b.n[i * 3 + 1], b.n[i * 3 + 2]};
            lo.push_back(b.add(p, nn, {b.t[i * 2], b.t[i * 2 + 1]}, {b.c[i * 4], 0, 0, b.c[i * 4 + 3]}));
            b.extra({b.tg[i * 4], b.tg[i * 4 + 1], b.tg[i * 4 + 2], b.tg[i * 4 + 3]}, {b.t2[i * 2], 0});
        }
        for (size_t k = 0; k + 1 < e.size(); ++k) {
            if (flip) { b.tri(e[k], e[k + 1], lo[k]); b.tri(lo[k], e[k + 1], lo[k + 1]); }
            else { b.tri(e[k], lo[k], e[k + 1]); b.tri(lo[k], lo[k + 1], e[k + 1]); }
        }
    };
    std::vector<unsigned short> e0, e1, e2, e3;
    for (int i = 0; i <= Q; ++i) { e0.push_back(edge(i, 0)); e1.push_back(edge(i, Q)); e2.push_back(edge(0, i)); e3.push_back(edge(Q, i)); }
    addSkirt(e0, false); addSkirt(e1, true); addSkirt(e2, true); addSkirt(e3, false);
    // water: each river as a ribbon along its smooth centreline and each lake as a disc,
    // both with an analytic, soft edge (the water pass covers terrain that sits only a hair
    // above the surface, so shores follow these curves, not the terrain grid)
    {
        std::vector<terrain::Waterway> ways;
        std::vector<terrain::LakeShape> lakes;
        F.waterIn(x0, z0, x0 + CS, z0 + CS, ways, lakes);
        MeshBuilder& wb = d.water;
        const float minW = j.lod >= 2 ? 6.f : 0.f;  // far away only the bigger rivers show
        const float cols[5] = {-1.f, -0.55f, 0.f, 0.55f, 1.f};
        for (const auto& w : ways) {
            const size_t n = w.v.size();
            if (n < 2 || w.v[0].half * 2 < minW) continue;
            auto section = [&](size_t i) {  // 5 vertices across the river at centreline vertex i
                const auto& v = w.v[i];
                const auto& pa = w.v[i > 0 ? i - 1 : i];
                const auto& pb = w.v[i + 1 < n ? i + 1 : i];
                float tx = pb.x - pa.x, tz = pb.z - pa.z, tl = std::max(1e-3f, std::hypot(tx, tz));
                float rx = tz / tl, rz = -tx / tl;
                float we = v.half + 1.0f;  // edge just beyond the waterline
                float speed = std::hypot(v.vx, v.vz);
                unsigned short first = 0;
                for (int c = 0; c < 5; ++c) {
                    unsigned short id = wb.add({v.x + rx * we * cols[c], v.level, v.z + rz * we * cols[c]}, {0, 1, 0}, {v.vx, v.vz},
                                               {u8(std::fabs(cols[c])), u8(speed / 3.f), 0, 255});
                    if (c == 0) first = id;
                }
                return first;
            };
            for (size_t i = 0; i + 1 < n; ++i) {
                float mx = 0.5f * (w.v[i].x + w.v[i + 1].x), mz = 0.5f * (w.v[i].z + w.v[i + 1].z);
                if (mx < x0 || mx >= x0 + CS || mz < z0 || mz >= z0 + CS) continue;  // owned by another chunk
                if (wb.v.size() / 3 > 60000) break;
                unsigned short a0 = section(i), b0 = section(i + 1);
                for (int c = 0; c < 4; ++c) {
                    wb.tri(a0 + c, a0 + c + 1, b0 + c);
                    wb.tri(a0 + c + 1, b0 + c + 1, b0 + c);
                }
            }
        }
        for (const auto& L : lakes) {
            if (wb.v.size() / 3 > 60000) break;
            const int N = terrain::LakeShape::N;
            unsigned short c0 = wb.add({L.x, L.level, L.z}, {0, 1, 0}, {0, 0}, {0, 0, 0, 255});
            unsigned short ring = 0;
            for (int a = 0; a < N; ++a) {
                float ang = 6.2831853f * a / N, cx = std::cos(ang), cz = std::sin(ang);
                for (int q = 0; q < 2; ++q) {  // inner ring (fully opaque), shore ring (fades)
                    float rr = L.radius[a] * (q == 0 ? 0.82f : 1.04f);
                    unsigned short id = wb.add({L.x + cx * rr, L.level, L.z + cz * rr}, {0, 1, 0}, {0, 0}, {u8(q == 0 ? 0.8f : 1.f), 0, 0, 255});
                    if (a == 0 && q == 0) ring = id;
                }
            }
            for (int a = 0; a < N; ++a) {
                unsigned short i0 = ring + 2 * a, i1 = ring + 2 * ((a + 1) % N);
                wb.tri(c0, i1, i0);
                wb.tri(i0, i1, i0 + 1);
                wb.tri(i1, i1 + 1, i0 + 1);
            }
        }
    }
    if (j.lod == 0) {  // keep the exact surface for grass placement
        d.gridQ = Q;
        d.hgrid.resize(size_t(Q + 1) * (Q + 1));
        d.rgrid.resize(size_t(Q + 1) * (Q + 1));
        d.wgrid.resize(size_t(Q + 1) * (Q + 1));
        for (int z = 0; z <= Q; ++z)
            for (int x = 0; x <= Q; ++x) {
                d.hgrid[size_t(z) * (Q + 1) + x] = H(x, z);
                d.rgrid[size_t(z) * (Q + 1) + x] = rd[size_t(z + K) * G + (x + K)];
                const terrain::Sample& q = SM(x, z);
                d.wgrid[size_t(z) * (Q + 1) + x] = q.water > -1e8f ? q.water - H(x, z) : -50.f;
            }
    }
    d.center = {x0 + CS / 2, (minY + maxY) / 2, z0 + CS / 2};
    d.radius = std::sqrt(2 * (CS / 2) * (CS / 2) + (maxY - minY) * (maxY - minY) / 4);

    // vegetation (deterministic per world cell, so trees don't move when detail changes)
    if (j.lod <= 2) {
        d.hasVeg = true;
        auto sample = [&](float wx, float wz) {
            float fx = (wx - x0) / step, fz = (wz - z0) / step;
            int ix = std::clamp(int(fx), -1, Q), iz = std::clamp(int(fz), -1, Q);
            float tx = std::clamp(fx - ix, 0.f, 1.f), tz = std::clamp(fz - iz, 0.f, 1.f);
            return (H(ix, iz) * (1 - tx) + H(ix + 1, iz) * tx) * (1 - tz) + (H(ix, iz + 1) * (1 - tx) + H(ix + 1, iz + 1) * tx) * tz;
        };
        const float sp = VEG_SPACING[std::clamp(quality, 0, 2)];
        const float snow = C.maxElevation() + 120.f;
        const auto bridges = F.bridgesNear(x0 + CS / 2, z0 + CS / 2, CS);
        auto wetAt = [&](float wx, float wz) {  // water depth at the nearest grid sample (+ = submerged)
            int ix = std::clamp(int(std::lround((wx - x0) / step)), -1, Q + 1), iz = std::clamp(int(std::lround((wz - z0) / step)), -1, Q + 1);
            const terrain::Sample& q = SM(ix, iz);
            return q.water > -1e8f ? q.water - q.h : -50.f;
        };
        int i0 = int(std::ceil(x0 / sp)), i1 = int(std::ceil((x0 + CS) / sp));
        int k0 = int(std::ceil(z0 / sp)), k1 = int(std::ceil((z0 + CS) / sp));
        for (int k = k0; k < k1; ++k)
            for (int i = i0; i < i1; ++i) {
                float jx = (i + hash2(i, k, 71) * 0.9f - 0.45f) * sp, jz = (k + hash2(i, k, 72) * 0.9f - 0.45f) * sp;
                if (jx < x0 || jx >= x0 + CS || jz < z0 || jz >= z0 + CS) continue;
                float forest = fbm(jx * 0.0035f, jz * 0.0035f, 4, 77);
                float p = smooth(0.48f, 0.62f, forest) * 0.8f + 0.015f;
                float roll = hash2(i, k, 73);
                const bool rock = false;   // (rocks have their own scatter below)
                if (roll > p && !rock) continue;
                float y = sample(jx, jz);
                if (wetAt(jx, jz) > -1.2f || y > snow - 30.f) continue;
                float sx = sample(jx + 3, jz) - sample(jx - 3, jz), sz = sample(jx, jz + 3) - sample(jx, jz - 3);
                float slope = std::sqrt(sx * sx + sz * sz) / 6.f;
                if (slope > (rock ? 1.2f : 0.75f)) continue;
                if (C.roadDistance(jx, jz) < hw + (rock ? 5.f : 6.5f)) continue;
                bool nearBridge = false;
                for (const auto& br : bridges) nearBridge |= std::hypot(br.x - jx, br.z - jz) < br.span * 0.5f + 12.f;
                if (nearBridge) continue;
                float rot = hash2(i, k, 74) * 2 * PI_F;
                if (rock) {
                    float sc = 0.6f + hash2(i, k, 75) * 2.2f;
                    (void)sc;
                    continue;
                }
                float alt = smooth(C.minElevation() + 10.f, C.maxElevation() + 40.f, y);
                int type = trees::pickType(alt, forest, hash2(i, k, 77), hash2(i, k, 78));
                float sc = 0.72f + hash2(i, k, 79) * 0.5f;
                float aspect = 0.92f + hash2(i, k, 80) * 0.16f;
                d.veg[type].push_back(MatrixMultiply(MatrixMultiply(MatrixScale(sc, sc * aspect, sc), MatrixRotateY(rot)), MatrixTranslate(jx, y - 0.15f, jz)));
                d.vegPos[type].push_back({jx, y, jz});
            }
        // bushes and rocks (near chunks): a finer scatter. Bushes crowd the forest margins and
        // sprinkle the meadows; rocks gather in boulder fields and on the steeper ground.
        if (j.lod <= 1) {
            const float bs = quality >= 2 ? 4.5f : quality == 1 ? 6.f : 8.f;
            int b0 = int(std::ceil(x0 / bs)), b1 = int(std::ceil((x0 + CS) / bs));
            int c0 = int(std::ceil(z0 / bs)), c1 = int(std::ceil((z0 + CS) / bs));
            for (int k = c0; k < c1; ++k)
                for (int i = b0; i < b1; ++i) {
                    float jx = (i + hash2(i, k, 171) * 0.9f - 0.45f) * bs, jz = (k + hash2(i, k, 172) * 0.9f - 0.45f) * bs;
                    if (jx < x0 || jx >= x0 + CS || jz < z0 || jz >= z0 + CS) continue;
                    float forest = fbm(jx * 0.0035f, jz * 0.0035f, 4, 77);
                    float rocky = fbm(jx * 0.006f, jz * 0.006f, 3, 501);
                    float clump = vnoise(jx * 0.06f, jz * 0.06f, 503);
                    float edge = 1.f - std::min(1.f, std::fabs(forest - 0.5f) * 7.f);           // the forest's margin
                    // in clumps: along the forest's margin, under its canopy, and the odd thicket out in the open
                    float pBush = (0.004f + 0.3f * edge + 0.08f * smooth(0.55f, 0.7f, forest)) * smooth(0.35f, 0.7f, clump) * 1.6f;
                    float pRock = (0.002f + 0.16f * smooth(0.6f, 0.78f, rocky)) * (0.4f + 1.2f * smooth(0.45f, 0.8f, clump));
                    static const float show = std::getenv("CORERAY_SHOWCASE") ? 10.f : 1.f;   // dev: pack them in to look at
                    pBush *= show; pRock *= show * show * 0.4f;
                    float roll = hash2(i, k, 173);
                    bool rock = roll < pRock, bush = !rock && roll < pRock + pBush;
                    if (!rock && !bush) continue;
                    float y = sample(jx, jz);
                    if (wetAt(jx, jz) > -0.6f || y > snow - 30.f) continue;
                    float sx = sample(jx + 2, jz) - sample(jx - 2, jz), sz = sample(jx, jz + 2) - sample(jx, jz - 2);
                    float slope = std::sqrt(sx * sx + sz * sz) / 4.f;
                    if (slope > (rock ? 1.3f : 0.7f)) continue;
                    if (C.roadDistance(jx, jz) < hw + (rock ? 3.5f : 3.f)) continue;
                    bool nearBridge = false;
                    for (const auto& br : bridges) nearBridge |= std::hypot(br.x - jx, br.z - jz) < br.span * 0.5f + 8.f;
                    if (nearBridge) continue;
                    float rot = hash2(i, k, 174) * 2 * PI_F;
                    if (rock) {
                        // mostly small stones, the odd big boulder
                        float u = hash2(i, k, 175);
                        float sc = 0.35f + 2.4f * u * u * u + 0.6f * smooth(0.7f, 0.85f, rocky) * u;
                        int set = VEG_ROCK0 + int(hash2(i, k, 176) * trees::ROCKS) % trees::ROCKS;
                        d.veg[set].push_back(MatrixMultiply(MatrixMultiply(MatrixScale(sc, sc * (0.75f + hash2(i, k, 177) * 0.5f), sc), MatrixRotateY(rot)),
                                                            MatrixTranslate(jx, y - 0.12f * sc - slope * 0.3f * sc, jz)));
                        d.vegPos[set].push_back({jx, y, jz});
                    } else {
                        float sc = 0.7f + hash2(i, k, 178) * 0.65f;
                        int set = VEG_BUSH0 + int(hash2(i, k, 179) * trees::BUSHES) % trees::BUSHES;
                        d.veg[set].push_back(MatrixMultiply(MatrixMultiply(MatrixScale(sc, sc * (0.85f + hash2(i, k, 180) * 0.3f), sc), MatrixRotateY(rot)),
                                                            MatrixTranslate(jx, y - 0.1f, jz)));
                        d.vegPos[set].push_back({jx, y, jz});
                    }
                }
        }
    }
    return d;
}

void Renderer::uploadChunk(ChunkData&& d) {
    auto key = std::make_pair(d.cx, d.cz);
    inFlight_.erase(key);
    if (d.world != (worldId_ & 0x7FFF)) return;  // built for a previous course
    Chunk& c = chunks_[key];
    if (c.mesh.vaoId) UnloadMesh(c.mesh);
    c.cx = d.cx; c.cz = d.cz; c.lod = d.lod; c.version = d.version;
    c.mesh = d.mb.build(true);
    if (c.water.vaoId) UnloadMesh(c.water);
    c.water = Mesh{};
    if (!d.water.idx.empty()) c.water = d.water.build(true);
    c.center = d.center;
    c.radius = d.radius;
    c.hasVeg = d.hasVeg;
    c.fine = d.fine;
    for (int i = 0; i < VEG_SETS; ++i) { c.veg[i] = std::move(d.veg[i]); c.vegPos[i] = std::move(d.vegPos[i]); }
    c.hgrid = std::move(d.hgrid);
    c.rgrid = std::move(d.rgrid);
    c.wgrid = std::move(d.wgrid);
    c.gridQ = d.gridQ;
}

void Renderer::streamWorld(const Vector3& focus, double riderDist, int maxUploads) {
    (void)riderDist;
    streamFocus_ = focus;
    // 1. upload what the workers finished
    std::vector<ChunkData*> ready;
    {
        std::lock_guard<std::mutex> lk(qm_);
        // nearest first
        std::sort(done_.begin(), done_.end(), [&](ChunkData* a, ChunkData* b) {
            return boxDist(a->cx, a->cz, focus) < boxDist(b->cx, b->cz, focus);
        });
        size_t n = maxUploads < 0 ? done_.size() : std::min(done_.size(), size_t(maxUploads));
        ready.assign(done_.begin(), done_.begin() + n);
        done_.erase(done_.begin(), done_.begin() + n);
    }
    for (auto* d : ready) { uploadChunk(std::move(*d)); delete d; }

    // 2. what should exist, at what detail
    const int q = std::clamp(quality_, 0, 2);
    const float view = VIEW[q];
    const int R = int(std::ceil(view / CS)) + 1;
    const int fcx = int(std::floor(focus.x / CS)), fcz = int(std::floor(focus.z / CS));
    struct Need { int cx, cz, lod; float prio; };
    std::vector<Need> need, erodeNeed;
    const terrain::Field& F = course_->field();
    int wanted = 0, have = 0;
    const int ver = courseVersion_ & 0xFFFF;
    for (int dz = -R; dz <= R; ++dz)
        for (int dx = -R; dx <= R; ++dx) {
            int cx = fcx + dx, cz = fcz + dz;
            float dd = boxDist(cx, cz, focus);
            if (dd > view) continue;
            int lod = lodFor(dd);
            auto it = chunks_.find({cx, cz});
            bool ok = it != chunks_.end() && it->second.lod == lod && it->second.version == ver;
            if (lod <= 1) {
                bool ready = F.tileReady(cx, cz);
                if (!ready && !eroding_.count({cx, cz})) erodeNeed.push_back({cx, cz, -1, dd});
                if (ok && !it->second.fine && ready) ok = false;  // eroded surface arrived: rebuild
                if (ready) eroding_.erase({cx, cz});
                if (dd < 1200) { ++wanted; if (ok && it->second.fine) ++have; }
            } else if (dd < 1200) { ++wanted; if (ok) ++have; }
            if (ok) continue;
            auto fl = inFlight_.find({cx, cz});
            if (fl != inFlight_.end() && fl->second == lod) continue;
            bool missing = it == chunks_.end();
            need.push_back({cx, cz, lod, dd + (missing ? 0.f : 900.f)});
        }
    wantedNear_ = wanted;
    haveNear_ = have;
    std::sort(need.begin(), need.end(), [](const Need& a, const Need& b) { return a.prio < b.prio; });
    {
        std::lock_guard<std::mutex> lk(qm_);
        size_t cap = workers_.size() * 3;
        size_t queued = jobs_.size();
        // erosion tiles first (nearest first), but leave room for chunks
        std::sort(erodeNeed.begin(), erodeNeed.end(), [](const Need& a, const Need& b) { return a.prio < b.prio; });
        size_t ecap = std::max<size_t>(2, workers_.size());
        size_t equeued = 0;
        for (auto& j : jobs_) equeued += j.lod < 0;
        for (auto& n : erodeNeed) {
            if (equeued >= ecap) break;
            jobs_.push_back({n.cx, n.cz, -1, ((worldId_ & 0x7FFF) << 16) | ver, course_, n.prio});
            eroding_.insert({n.cx, n.cz});
            ++equeued;
        }
        for (auto& n : need) {
            if (queued >= cap) break;
            if (inFlight_.count({n.cx, n.cz})) continue;  // a different detail is cooking; wait for it
            jobs_.push_back({n.cx, n.cz, n.lod, ((worldId_ & 0x7FFF) << 16) | ver, course_, n.prio});
            inFlight_[{n.cx, n.cz}] = n.lod;
            ++queued;
        }
    }
    qcv_.notify_all();

    // 3. forget what's far behind (and the terrain caches for it, now and then)
    if (GetTime() - lastTrim_ > 8.0) {
        lastTrim_ = GetTime();
        course_->field().trim(focus.x, focus.z, view + 2500.f);
    }
    for (auto it = chunks_.begin(); it != chunks_.end();) {
        if (boxDist(it->first.first, it->first.second, focus) > view + 1200) {
            if (it->second.mesh.vaoId) UnloadMesh(it->second.mesh);
            if (it->second.water.vaoId) UnloadMesh(it->second.water);
            it = chunks_.erase(it);
        } else ++it;
    }
}

float Renderer::loadProgress() const { return wantedNear_ ? float(haveNear_) / float(wantedNear_) : 0.f; }

void Renderer::preload(double dist) {
    if (!course_) return;
    riderDist_ = dist;
    updateRoad(dist);
    auto p = course_->at(dist);
    Vector3 focus = toV(p.pos);
    double t0 = GetTime();
    for (;;) {
        streamWorld(focus, dist, -1);
        if (loadProgress() >= 1.f || GetTime() - t0 > 30) break;
        BeginDrawing();
        // CoreKick's loading veil: warm sand
        int W = GetScreenWidth(), H = GetScreenHeight();
        DrawRectangleGradientV(0, 0, W, H, Color{246, 227, 196, 255}, Color{232, 207, 168, 255});
        const char* msg = "Building a new world...";
        Font& f = ui::font(ui::F::Title);
        Vector2 m = MeasureTextEx(f, msg, 22, -0.2f);
        DrawTextEx(f, msg, {(W - m.x) / 2, H / 2.f - 44}, 22, -0.2f, Color{107, 74, 42, 255});
        DrawRectangleRounded({W / 2.f - 150, H / 2.f, 300, 6}, 1, 6, Color{107, 74, 42, 40});
        DrawRectangleRounded({W / 2.f - 150, H / 2.f, 300 * loadProgress(), 6}, 1, 6, Color{107, 74, 42, 200});
        EndDrawing();
    }
}

// ------------------------------------------------------------------ road, posts, arches, lanterns
void Renderer::updateRoad(double riderDist) {
    const Course& C = *course_;
    const auto& P = C.points();
    const int N = int(P.size());
    if (N < 2) return;
    const float step = C.length() / float(C.loop() ? N : N - 1);
    const int per = 400;
    double lap = C.loop() ? std::fmod(std::fmod(riderDist, C.length()) + C.length(), C.length()) : riderDist;
    int ri = int(lap / step);
    std::vector<int> want;
    for (int i = ri - 1500; i <= ri + 4400; i += per) {
        int k = i;
        if (C.loop()) k = ((k % N) + N) % N;
        else if (k < 0 || k >= N - 1) continue;
        int first = (k / per) * per;
        if (std::find(want.begin(), want.end(), first) == want.end()) want.push_back(first);
    }
    // drop unwanted / stale
    for (auto it = road_.begin(); it != road_.end();) {
        bool keep = std::find(want.begin(), want.end(), it->first) != want.end() && it->version == courseVersion_;
        if (!keep) { if (it->mesh.vaoId) UnloadMesh(it->mesh); it = road_.erase(it); }
        else ++it;
    }
    const float hw = C.roadHalfWidth();
    // river crossings along the road that's about to be built (or already is)
    {
        int sig = int(want.size()) * 7919 + (want.empty() ? 0 : want.front()) + courseVersion_ * 131;
        if (sig != bridgesFor_) {
            bridgesFor_ = sig;
            bridges_.clear();
            const terrain::Field& F = C.field();
            for (int first : want) {
                int end = std::min(N - 1, first + per);
                for (int i = first; i <= end; i += 25) {
                    const auto& p = P[i % N];
                    for (const auto& b : F.bridgesNear(p.pos.x, p.pos.z, 40.f)) {
                        bool dup = false;
                        for (auto& o : bridges_) dup |= std::hypot(o.c.x - b.x, o.c.z - b.z) < 2.f;
                        if (dup) continue;
                        float nd = C.nearestDist(b.x, b.z);
                        if (nd < 0) continue;
                        auto cp = C.at(nd);
                        bridges_.push_back({{b.x, cp.pos.y, b.z}, cp.heading, b.span, b.level, b.width, cp.grade});
                    }
                }
            }
        }
    }
    auto onBridge = [&](Vector3 c) {
        for (auto& b : bridges_) {
            float dx = c.x - b.c.x, dz = c.z - b.c.z;
            float along = dx * std::sin(b.heading) + dz * std::cos(b.heading);
            float lat = dx * std::cos(b.heading) - dz * std::sin(b.heading);
            if (std::fabs(along) < b.span * 0.5f + 0.5f && std::fabs(lat) < 12.f) return true;
        }
        return false;
    };
    for (int first : want) {
        bool have = false;
        for (auto& r : road_) if (r.first == first) have = true;
        if (have) continue;
        MeshBuilder b;
        int end = C.loop() ? std::min(N, first + per) : std::min(N - 1, first + per);
        Vector3 cmin{1e9f, 1e9f, 1e9f}, cmax{-1e9f, -1e9f, -1e9f};
        // six columns across: grass verge, gravel, asphalt edge | asphalt edge, gravel, grass verge.
        // u runs 0..1 across the asphalt (7.2 m) and beyond it on the verges.
        const float W = 2 * hw;
        const float outerM = 3.0f, midM = 0.85f, edgeM = 0.25f;
        const float us[6] = {-outerM / W, -midM / W, -edgeM / W, 1 + edgeM / W, 1 + midM / W, 1 + outerM / W};
        for (int i = first; i <= end; ++i) {
            const auto& p = P[i % N];
            Vector3 r = rightOf(p.heading);
            Vector3 f = Vector3Normalize({std::sin(p.heading), p.grade, std::cos(p.heading)});
            Vector3 c = toV(p.pos);
            c.y += 0.03f;
            float v = (i == N) ? C.length() : p.dist;
            Vector3 pts[6];
            const bool deck = onBridge(c);
            for (int k = 0; k < 6; ++k) {
                float off = (us[k] - 0.5f) * W;  // metres from the centre line (negative = left)
                Vector3 q = Vector3Add(c, Vector3Scale(r, off));
                float ao = std::fabs(off);
                if (deck && ao > hw + edgeM + 0.01f) q.y = c.y - 0.03f;  // level across a bridge
                else if (ao > hw + edgeM + 0.01f) {
                    float ty = C.terrainHeight(q.x, q.z) + 0.035f;
                    if (ao < hw + midM + 0.01f) q.y = std::min(c.y - 0.04f, std::max(ty, c.y - 0.12f));  // gravel just below the tar
                    else q.y = ty;
                }
                pts[k] = q;
            }
            for (int k = 0; k < 6; ++k) {
                Vector3 acrossV = Vector3Subtract(pts[std::min(5, k + 1)], pts[std::max(0, k - 1)]);
                Vector3 nn = Vector3Normalize(Vector3CrossProduct(acrossV, f));
                if (nn.y < 0) nn = Vector3Negate(nn);
                b.add(pts[k], nn, {us[k], v});
            }
            cmin = Vector3Min(cmin, c); cmax = Vector3Max(cmax, c);
        }
        int rows = end - first + 1;
        for (int i = 0; i < rows - 1; ++i)
            for (int k = 0; k < 5; ++k) {
                unsigned short a0 = i * 6 + k, a1 = i * 6 + k + 1, b0 = (i + 1) * 6 + k, b1 = (i + 1) * 6 + k + 1;
                b.tri(a0, a1, b0);
                b.tri(b0, a1, b1);
            }
        RoadChunk rc;
        rc.first = first;
        rc.version = courseVersion_;
        rc.mesh = b.build();
        rc.center = Vector3Scale(Vector3Add(cmin, cmax), 0.5f);
        rc.radius = Vector3Distance(cmin, cmax) * 0.5f + hw + 4.f;
        road_.push_back(rc);
    }
    // km posts near the rider
    signposts_.clear();
    for (int k = int(lap / 1000) - 1; k <= int(lap / 1000) + 2; ++k) {
        double d = k * 1000.0;
        if (d <= 0 || d >= C.length() - 100) continue;
        auto p = C.at(d);
        Vector3 pos = Vector3Add(toV(p.pos), Vector3Scale(rightOf(p.heading), hw + 1.6f));
        pos.y = C.terrainHeight(pos.x, pos.z);
        signposts_.push_back(MatrixMultiply(MatrixRotateY(p.heading), MatrixTranslate(pos.x, pos.y, pos.z)));
    }
    auto p0 = C.at(0);
    archBase_ = MatrixMultiply(MatrixRotateY(p0.heading), MatrixTranslate(p0.pos.x, p0.pos.y, p0.pos.z));
    hasFinish_ = !C.loop();
    if (hasFinish_) {
        auto pe = C.at(C.length());
        finishBase_ = MatrixMultiply(MatrixRotateY(pe.heading), MatrixTranslate(pe.pos.x, pe.pos.y, pe.pos.z));
    }
    // lanterns along the verge, alternating sides
    lanterns_.clear();
    int k0 = int(std::floor((lap - 350) / LANTERN_SPACING)), k1 = int(std::ceil((lap + 1500) / LANTERN_SPACING));
    int perLap = std::max(1, int(C.length() / LANTERN_SPACING));
    for (int k = k0; k <= k1; ++k) {
        int kk = k;
        if (C.loop()) kk = ((k % perLap) + perLap) % perLap;
        double d = kk * double(LANTERN_SPACING) + 20;
        if (d < 0 || d > C.length() - 10) continue;
        auto p = C.at(d);
        if (onBridge(toV(p.pos))) continue;  // no lamp posts standing in the river
        float side = (kk % 2) ? 1.f : -1.f;
        Vector3 pos = Vector3Add(toV(p.pos), Vector3Scale(rightOf(p.heading), side * (hw + 1.3f)));
        pos.y = C.terrainHeight(pos.x, pos.z);
        lanterns_.push_back({pos, float(d), side});
        (void)kk;
    }
}

void Renderer::updateLanterns(const SceneState& s, float dt) {
    (void)dt;
    lights_.clear();
    wxNow_ = s.wx;
    shaftDensity_ = 0.35f + s.wx.fog * 1.6f + s.wx.rain * 0.9f + s.wx.snow * 0.9f + s.wx.hail * 0.5f;
    const Course& C = *course_;
    bool on = s.lanterns && (L_.night > 0.35f || (s.wx.dark > 0.8f && L_.night > 0.15f));
    double lap = C.loop() ? std::fmod(std::fmod(s.dist, C.length()) + C.length(), C.length()) : s.dist;
    if (!on) { lit_.clear(); }
    else {
        for (auto& l : lanterns_) {
            double delta = l.dist - lap;
            if (C.loop()) { if (delta > C.length() / 2) delta -= C.length(); if (delta < -C.length() / 2) delta += C.length(); }
            int key = int(std::lround(l.dist));
            if (delta < 220 && delta > -400 && !lit_.count(key)) lit_[key] = time_;
        }
        std::vector<std::pair<float, Light>> cand;
        for (auto& l : lanterns_) {
            auto it = lit_.find(int(std::lround(l.dist)));
            if (it == lit_.end()) continue;
            float age = time_ - it->second;
            float k = age < 0.9f ? std::max(0.f, (0.55f + 0.45f * std::sin(age * 47.f) * std::sin(age * 13.f)) * age / 0.9f) : 1.f;
            auto p = C.at(l.dist);
            Vector3 lamp = Vector3Add(l.pos, Vector3Add(Vector3Scale(rightOf(p.heading), -l.side * 0.95f), {0, 3.66f, 0}));
            Light L;
            L.pos = Vector3Add(lamp, {0, -0.16f, 0});   // the lamp's emitting face, under the glass
            L.col = Vector3Scale({1.0f, 0.74f, 0.45f}, 17.f * k);   // warm ~3000 K
            // aimed slightly out over the road; the beam is stretched along the road (batwing)
            L.dir = Vector3Normalize(Vector3Add({0, -1, 0}, Vector3Scale(rightOf(p.heading), -l.side * 0.28f)));
            L.axis = fwdOf(p.heading);
            L.elong = 1.9f;
            L.cosIn = 0.84f;
            L.cosOut = 0.22f;
            cand.push_back({Vector3Distance(lamp, cam_.position), L});
        }
        std::sort(cand.begin(), cand.end(), [](auto& a, auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < cand.size() && lights_.size() < 15; ++i) lights_.push_back(cand[i].second);
    }
    // the bike's headlight: a real spot down the road after dusk
    if (L_.night > 0.25f) {
        auto p = C.at(s.dist);
        auto ahead = C.at(s.dist + 12);
        Vector3 lp = Vector3Add(Vector3Add(toV(p.pos), Vector3Scale(rightOf(p.heading), 1.4f)), Vector3Add(Vector3Scale(fwdOf(p.heading), 0.7f), {0, 1.0f, 0}));
        Vector3 aim = Vector3Add(toV(ahead.pos), Vector3Scale(rightOf(ahead.heading), 1.2f));
        Light L;
        L.pos = lp;
        L.dir = Vector3Normalize(Vector3Subtract(aim, lp));
        L.col = Vector3Scale({0.92f, 0.95f, 1.0f}, 55.f * L_.night);
        L.cosIn = 0.975f;
        L.cosOut = 0.90f;
        L.vol = 0.06f;   // a bike light barely shows in the air from behind
        lights_.push_back(L);
    }
}

void Renderer::drawLanterns(bool sp, const Vector3& focus, float radius) {
    if (lanterns_.empty()) return;
    const Color iron{26, 28, 31, 255};
    for (auto& l : lanterns_) {
        if (sp ? (std::fabs(l.pos.x - focus.x) > radius || std::fabs(l.pos.z - focus.z) > radius) : Vector3Distance(l.pos, cam_.position) > 1400.f) continue;
        auto p = course_->at(l.dist);
        Matrix base = MatrixMultiply(MatrixRotateY(p.heading), MatrixTranslate(l.pos.x, l.pos.y, l.pos.z));
        bool near = Vector3Distance(l.pos, cam_.position) < 120.f;
        // fluted base, pole with collars, finial
        drawPart(cyl_, MatrixScale(0.17f, 0.42f, 0.17f), base, iron, 0.45f, 0.5f, sp);
        drawPart(sphere_, MatrixMultiply(MatrixScale(0.15f, 0.07f, 0.15f), MatrixTranslate(0, 0.44f, 0)), base, iron, 0.45f, 0.5f, sp);
        drawPart(cyl_, MatrixMultiply(MatrixScale(0.062f, 3.85f, 0.062f), MatrixTranslate(0, 0.45f, 0)), base, iron, 0.4f, 0.55f, sp);
        if (near && !sp) {
            for (float y : {1.25f, 3.95f}) drawPart(sphere_, MatrixMultiply(MatrixScale(0.09f, 0.04f, 0.09f), MatrixTranslate(0, y, 0)), base, iron, 0.4f, 0.6f, false);
            drawPart(sphere_, MatrixMultiply(MatrixScale(0.07f, 0.07f, 0.07f), MatrixTranslate(0, 4.36f, 0)), base, iron, 0.35f, 0.6f, false);
        }
        // swan-neck arm reaching over the verge (local +X * side is the road side)
        float sx = l.side;
        Vector3 prev{0, 4.12f, 0};
        for (int i = 1; i <= 6; ++i) {
            float t = i / 6.f;
            Vector3 q{sx * 0.95f * t, 4.12f + std::sin(t * PI_F) * 0.22f - t * 0.06f, 0};
            drawLimb(prev, q, 0.03f, base, iron, sp, 0.4f, 0.55f);
            prev = q;
        }
        if (near && !sp) {  // decorative scroll under the arm
            Vector3 pv{0.f, 3.85f, 0};
            for (int i = 1; i <= 6; ++i) {
                float a = i / 6.f * 4.2f;
                Vector3 q{sx * (0.18f + 0.12f * std::sin(a)), 3.98f - 0.1f * std::cos(a) + i * 0.012f, 0};
                drawLimb(pv, q, 0.014f, base, iron, false, 0.4f, 0.55f);
                pv = q;
            }
        }
        auto it = lit_.find(int(std::lround(l.dist)));
        float glow = 0;
        if (it != lit_.end()) {
            float age = time_ - it->second;
            glow = age < 0.9f ? std::max(0.f, (0.55f + 0.45f * std::sin(age * 47.f) * std::sin(age * 13.f)) * age / 0.9f) : 1.f;
        }
        float hx = sx * 0.95f;
        // lantern head: hook, pyramid roof, glass box with corner bars, base plate, drop finial
        drawPart(cyl_, MatrixMultiply(MatrixScale(0.012f, 0.12f, 0.012f), MatrixTranslate(hx, 3.94f, 0)), base, iron, 0.4f, 0.5f, sp);
        drawPart(pyramid_, MatrixMultiply(MatrixMultiply(MatrixScale(0.21f, 0.15f, 0.21f), MatrixRotateY(PI_F / 4)), MatrixTranslate(hx, 3.83f, 0)), base, iron, 0.4f, 0.5f, sp);
        drawPart(cube_, MatrixMultiply(MatrixScale(0.2f, 0.28f, 0.2f), MatrixTranslate(hx, 3.68f, 0)), base,
                 glow > 0 ? Color{255, 214, 150, 255} : Color{70, 74, 82, 255}, 0.08f, 1.0f, sp, 0, glow * 5.f);
        if (!sp && near && glow > 0)   // the lamp itself, hot inside the frosted glass
            drawPart(sphere_, MatrixMultiply(MatrixScale(0.055f, 0.075f, 0.055f), MatrixTranslate(hx, 3.62f, 0)), base,
                     Color{255, 236, 200, 255}, 0.1f, 0.f, false, 0, glow * 30.f);
        if (!sp) {
            for (float cx : {-0.1f, 0.1f})
                for (float cz : {-0.1f, 0.1f})
                    drawPart(cube_, MatrixMultiply(MatrixScale(0.022f, 0.31f, 0.022f), MatrixTranslate(hx + cx, 3.68f, cz)), base, iron, 0.4f, 0.5f, false);
            drawPart(cube_, MatrixMultiply(MatrixScale(0.25f, 0.03f, 0.25f), MatrixTranslate(hx, 3.53f, 0)), base, iron, 0.4f, 0.5f, false);
            drawPart(cone_, MatrixMultiply(MatrixMultiply(MatrixScale(0.05f, 0.1f, 0.05f), MatrixRotateX(PI_F)), MatrixTranslate(hx, 3.52f, 0)), base, iron, 0.4f, 0.5f, false);
        }
    }
}

void Renderer::drawLightShafts() {
    if (lit_.empty()) return;
    const bool vol = volEnabled();   // the volumetric pass draws the cones itself
    BeginBlendMode(BLEND_ADDITIVE);
    rlDisableDepthMask();
    rlDisableBackfaceCulling();
    for (auto& L : lights_) {
        if (vol || L.cosOut < 0 || L.dir.y > -0.9f) continue;  // lanterns only
        float d = Vector3Distance(L.pos, cam_.position);
        if (d > 300.f) continue;
        float ground = course_->terrainHeight(L.pos.x, L.pos.z);
        float h = std::max(1.f, L.pos.y - ground - 0.05f);
        float r = h * 0.78f;
        setV3(volumeSh_, "volColor", Vector3Scale(L.col, 0.013f));
        setF(volumeSh_, "volDensity", shaftDensity_);
        DrawMesh(shaftCone_, volumeMat_, MatrixMultiply(MatrixScale(r, h, r), MatrixTranslate(L.pos.x, L.pos.y - 0.12f, L.pos.z)));
    }
    rlDrawRenderBatchActive();
    // halos around the lamps
    Vector3 f = Vector3Normalize(Vector3Subtract(cam_.target, cam_.position));
    Vector3 rr = Vector3Normalize(Vector3CrossProduct(f, {0, 1, 0}));
    Vector3 uu = Vector3CrossProduct(rr, f);
    BeginShaderMode(particleSh_);
    setF(particleSh_, "intensity", (3.f + shaftDensity_ * 2.f) * (vol ? 0.6f : 1.f));
    rlSetTexture(tex_.soft.id);
    rlBegin(RL_QUADS);
    for (auto& L : lights_) {
        if (L.cosOut < 0 || L.dir.y > -0.9f) continue;
        float d = Vector3Distance(L.pos, cam_.position);
        if (d > 600.f) continue;
        float sz = 0.55f + std::min(1.2f, d * 0.004f);
        float k = std::min(1.f, Vector3Length(L.col) / 15.f);
        rlColor4f(1.f, 0.72f, 0.42f, 0.55f * k);
        Vector3 c = L.pos;
        Vector3 a = Vector3Add(c, Vector3Add(Vector3Scale(rr, -sz), Vector3Scale(uu, -sz)));
        Vector3 b2 = Vector3Add(c, Vector3Add(Vector3Scale(rr, sz), Vector3Scale(uu, -sz)));
        Vector3 c2 = Vector3Add(c, Vector3Add(Vector3Scale(rr, sz), Vector3Scale(uu, sz)));
        Vector3 d2 = Vector3Add(c, Vector3Add(Vector3Scale(rr, -sz), Vector3Scale(uu, sz)));
        rlTexCoord2f(0, 1); rlVertex3f(a.x, a.y, a.z);
        rlTexCoord2f(1, 1); rlVertex3f(b2.x, b2.y, b2.z);
        rlTexCoord2f(1, 0); rlVertex3f(c2.x, c2.y, c2.z);
        rlTexCoord2f(0, 0); rlVertex3f(d2.x, d2.y, d2.z);
    }
    rlEnd();
    rlSetTexture(0);
    rlDrawRenderBatchActive();
    EndShaderMode();
    rlEnableBackfaceCulling();
    rlEnableDepthMask();
    EndBlendMode();
}

// ------------------------------------------------------------------ grass tufts
bool Renderer::groundAt(float x, float z, float& y, float& rd, float* wet) const {
    int cx = int(std::floor(x / CS)), cz = int(std::floor(z / CS));
    auto it = chunks_.find({cx, cz});
    if (it == chunks_.end() || it->second.gridQ == 0 || it->second.version != (courseVersion_ & 0xFFFF)) return false;
    const Chunk& c = it->second;
    int Q = c.gridQ;
    float step = CS / Q;
    float fx = (x - cx * CS) / step, fz = (z - cz * CS) / step;
    int ix = std::clamp(int(fx), 0, Q - 1), iz = std::clamp(int(fz), 0, Q - 1);
    float tx = std::clamp(fx - ix, 0.f, 1.f), tz = std::clamp(fz - iz, 0.f, 1.f);
    auto bil = [&](const std::vector<float>& g) {
        auto G = [&](int a, int b) { return g[size_t(b) * (Q + 1) + a]; };
        return (G(ix, iz) * (1 - tx) + G(ix + 1, iz) * tx) * (1 - tz) + (G(ix, iz + 1) * (1 - tx) + G(ix + 1, iz + 1) * tx) * tz;
    };
    y = bil(c.hgrid);
    rd = bil(c.rgrid);
    if (wet) *wet = c.wgrid.empty() ? -50.f : bil(c.wgrid);
    return true;
}

namespace {
const float GRASS_RADIUS[3] = {28.f, 50.f, 72.f};
const float GRASS_SPACING[3] = {1.2f, 0.9f, 0.72f};   // fewer, fuller clumps
constexpr float GT = 16.f;
}  // namespace

void Renderer::updateGrass() {
    const int q = std::clamp(quality_, 0, 2);
    const float R = GRASS_RADIUS[q], sp = GRASS_SPACING[q];
    Vector3 c = cam_.position;
    // drop far tiles
    for (auto it = grass_.begin(); it != grass_.end();) {
        float dx = it->second.center.x - c.x, dz = it->second.center.z - c.z;
        if (std::sqrt(dx * dx + dz * dz) > R + 40) it = grass_.erase(it); else ++it;
    }
    struct Want { int tx, tz; float d; };
    std::vector<Want> want;
    int t0x = int(std::floor((c.x - R) / GT)), t1x = int(std::floor((c.x + R) / GT));
    int t0z = int(std::floor((c.z - R) / GT)), t1z = int(std::floor((c.z + R) / GT));
    for (int tz = t0z; tz <= t1z; ++tz)
        for (int tx = t0x; tx <= t1x; ++tx) {
            float cx = (tx + 0.5f) * GT, cz = (tz + 0.5f) * GT;
            float d = std::sqrt((cx - c.x) * (cx - c.x) + (cz - c.z) * (cz - c.z));
            if (d > R + GT) continue;
            auto it = grass_.find({tx, tz});
            if (it != grass_.end() && it->second.version == courseVersion_) continue;
            want.push_back({tx, tz, d});
        }
    std::sort(want.begin(), want.end(), [](const Want& a, const Want& b) { return a.d < b.d; });
    const Course& C = *course_;
    const float hw = C.roadHalfWidth(), snowLine = C.maxElevation() + 80.f;
    int built = 0;
    for (auto& w : want) {
        if (built >= 10) break;
        float y0, rd0;
        if (!groundAt((w.tx + 0.5f) * GT, (w.tz + 0.5f) * GT, y0, rd0)) continue;
        GrassTile tile;
        tile.version = courseVersion_;
        tile.center = {(w.tx + 0.5f) * GT, y0, (w.tz + 0.5f) * GT};
        int i0 = int(std::ceil(w.tx * GT / sp)), i1 = int(std::ceil((w.tx + 1) * GT / sp));
        int k0 = int(std::ceil(w.tz * GT / sp)), k1 = int(std::ceil((w.tz + 1) * GT / sp));
        for (int k = k0; k < k1; ++k)
            for (int i = i0; i < i1; ++i) {
                float x = (i + hash2(i, k, 401) * 0.9f - 0.45f) * sp, z = (k + hash2(i, k, 402) * 0.9f - 0.45f) * sp;
                float y, rd, wd;
                if (!groundAt(x, z, y, rd, &wd)) continue;
                if (rd < hw + 0.85f + hash2(i, k, 403) * 0.7f) continue;          // gravel shoulder
                if (wd > -0.25f || y > snowLine) continue;
                float yx, yz, tmp;
                if (!groundAt(x + 0.8f, z, yx, tmp) || !groundAt(x, z + 0.8f, yz, tmp)) continue;
                float slope = std::sqrt((yx - y) * (yx - y) + (yz - y) * (yz - y)) / 0.8f;
                if (slope > 0.75f) continue;
                float forest = fbm(x * 0.0035f, z * 0.0035f, 4, 77);
                float r = hash2(i, k, 404);
                if (forest > 0.56f && r < 0.55f) continue;                        // sparse under the trees
                float meadow = vnoise(x * 0.012f, z * 0.012f, 409) * 0.7f + vnoise(x * 0.05f, z * 0.05f, 410) * 0.3f;
                // grass grows in clumps and patches with bare ground between them
                float clump = vnoise(x * 0.22f, z * 0.22f, 411) * 0.6f + vnoise(x * 0.7f, z * 0.7f, 412) * 0.4f;
                if (clump < 0.33f + 0.12f * (1.f - meadow)) continue;
                float verge = smooth(hw + 0.9f, hw + 3.5f, rd);                       // mown near the road
                float hgt = (0.32f + 0.6f * meadow * meadow + 0.18f * r) * (0.45f + 0.55f * verge) * (0.75f + 0.5f * clump);
                float wid = (1.0f + 0.7f * hash2(i, k, 405)) * (0.8f + hgt * 0.8f);
                Matrix m = MatrixMultiply(MatrixMultiply(MatrixScale(wid, hgt, wid), MatrixRotateY(hash2(i, k, 406) * 6.283f)), MatrixTranslate(x, y - 0.02f, z));
                bool flower = hash2(i, k, 407) < 0.05f * meadow * verge;
                (flower ? tile.flowers : tile.grass).push_back(m);
            }
        grass_[{w.tx, w.tz}] = std::move(tile);
        ++built;
    }
}

void Renderer::drawGrass(const SceneState& s) {
    if (grass_.empty()) return;
    const int q = std::clamp(quality_, 0, 2);
    const float R = GRASS_RADIUS[q];
    Vector3 camF = Vector3Normalize(Vector3Subtract(cam_.target, cam_.position));
    float cosLimit = std::cos((cam_.fovy * 0.5f * 1.9f + 8.f) * DEG2RAD);
    static std::vector<Matrix> g, f;
    g.clear(); f.clear();
    for (auto& [k, t] : grass_) {
        Vector3 v = Vector3Subtract(t.center, cam_.position);
        float d = Vector3Length(v);
        if (d - GT > R) continue;
        if (d > GT * 1.5f && Vector3DotProduct(v, camF) / d < cosLimit - GT / d) continue;
        g.insert(g.end(), t.grass.begin(), t.grass.end());
        f.insert(f.end(), t.flowers.begin(), t.flowers.end());
    }
    Vector2 wind{std::sin(s.wx.windDir), std::cos(s.wx.windDir)};
    setF(grassSh_, "time", time_);
    setV2(grassSh_, "windVec", wind);
    setF(grassSh_, "windAmount", 0.15f + s.wx.wind * 0.9f);
    setF(grassSh_, "gust", s.wx.gust);
    setF(grassSh_, "snowFlat", s.wx.snowCover);
    setV3(grassSh_, "riderPos", riderPos_);
    setF(grassSh_, "fadeStart", R * 0.55f);
    setF(grassSh_, "fadeEnd", R);
    rlDisableBackfaceCulling();
    if (!g.empty()) DrawMeshInstanced(tuft_, grassMat_, g.data(), int(g.size()));
    if (!f.empty()) DrawMeshInstanced(flowerTuft_, grassMat_, f.data(), int(f.size()));
    rlEnableBackfaceCulling();
}

// ------------------------------------------------------------------ world draw
void Renderer::drawWorld(bool sp, int cascade, const Vector3& focus, float radius) {
    // the view frustum (sides, top, bottom): a sphere is visible unless it's wholly outside one
    Vector3 camF = Vector3Normalize(Vector3Subtract(cam_.target, cam_.position));
    Vector3 camR = Vector3Normalize(Vector3CrossProduct(camF, {0, 1, 0}));
    Vector3 camU = Vector3CrossProduct(camR, camF);
    const float tanV = std::tan(cam_.fovy * 0.5f * DEG2RAD) * 1.04f;
    const float tanH = tanV * float(std::max(1, rtW_)) / float(std::max(1, rtH_));
    const float secV = std::sqrt(1.f + tanV * tanV), secH = std::sqrt(1.f + tanH * tanH);
    auto inFrustum = [&](Vector3 c, float r) {
        Vector3 v = Vector3Subtract(c, cam_.position);
        float z = Vector3DotProduct(v, camF);
        if (z < -r) return false;
        float x = Vector3DotProduct(v, camR), y = Vector3DotProduct(v, camU);
        if (std::fabs(x) > z * tanH + r * secH) return false;
        if (std::fabs(y) > z * tanV + r * secV) return false;
        return true;
    };
    auto visible = [&](Vector3 c, float r) {
        if (sp) return std::fabs(c.x - focus.x) < radius + r && std::fabs(c.z - focus.z) < radius + r;
        Vector3 v = Vector3Subtract(c, cam_.position);
        if (Vector3Length(v) - r > 11000.f) return false;
        return inFrustum(c, r);
    };
    tris_ = 0;
    for (auto& [k, c] : chunks_)
        if (c.mesh.vaoId && visible(c.center, c.radius)) {
            DrawMesh(c.mesh, sp ? depthMat_ : terrainMat_, MatrixIdentity());
            tris_ += c.mesh.triangleCount;
        }
    if (!sp && std::getenv("CORERAY_COUNT")) { static int f2 = 0; if (++f2 % 20 == 0) { int n = 0; for (auto& [k, c] : chunks_) if (c.mesh.vaoId && visible(c.center, c.radius)) ++n; std::fprintf(stderr, "[count] terrain chunks %d tris %d\n", n, tris_); } }
    if (!sp) prof::mark(prof::Terrain);
    if (!sp)
        for (auto& c : road_)
            if (visible(c.center, c.radius)) DrawMesh(c.mesh, roadMat_, MatrixIdentity());
    if (!sp) prof::mark(prof::Road);

    // vegetation (instanced), gathered from nearby chunks, culled per tree against the view
    // frustum. Three levels: the full tree near, the simplified tree to a few hundred metres,
    // an impostor (one quad) beyond. Each tree's switch distances are jittered a little so the
    // changes don't sweep along in a line.
    const int q = std::clamp(quality_, 0, 2);
    const float vegDist = VEG_DIST[q];
    const float hiDist = q == 2 ? 90.f : q == 1 ? 70.f : 50.f;
    const float loDist = impAlb_.id ? (q == 2 ? 360.f : q == 1 ? 260.f : 170.f) : 1e9f;
    static const float impDev = std::getenv("CORERAY_IMPDIST") ? float(std::atof(std::getenv("CORERAY_IMPDIST"))) : -1.f;   // dev: impostors from here
    rlDisableBackfaceCulling();
    const float bushHi = q == 2 ? 45.f : q == 1 ? 35.f : 25.f, bushMax = q == 2 ? 420.f : q == 1 ? 300.f : 200.f;
    const float rockMax = q == 2 ? 900.f : q == 1 ? 650.f : 420.f;
    for (int set = 0; set < VEG_SETS; ++set) {
        const bool rocks = set >= VEG_ROCK0, bushes = !rocks && set >= VEG_BUSH0;
        const trees::Built* built = rocks ? nullptr : bushes ? &bushes_[set - VEG_BUSH0] : &trees_[set];
        visHi_.clear();
        visLo_.clear();
        visFar_.clear();
        float maxD = rocks ? rockMax : bushes ? bushMax : vegDist;
        float md2 = maxD * maxD;
        const float th = rocks ? 3.5f : built->height * 1.32f;   // tallest instance of this set
        for (auto& [k, c] : chunks_) {
            if (!c.hasVeg || c.vegPos[set].empty()) continue;
            if (sp) {
                if (std::fabs(c.center.x - focus.x) > radius + CS || std::fabs(c.center.z - focus.z) > radius + CS) continue;
            } else {
                float dx = c.center.x - cam_.position.x, dz = c.center.z - cam_.position.z;
                if (std::sqrt(dx * dx + dz * dz) - CS * 0.71f > maxD) continue;
                if (!visible(c.center, c.radius + th)) continue;
            }
            const auto& pos = c.vegPos[set];
            for (size_t i = 0; i < pos.size(); ++i) {
                const Vector3& p = pos[i];
                if (sp) {
                    if (std::fabs(p.x - focus.x) > radius + 14 || std::fabs(p.z - focus.z) > radius + 14) continue;
                    if (cascade == 1 && !rocks && bushes) continue;   // bushes only cast into the near cascade
                    // near cascade: full detail; far cascade: simplified
                    (rocks || cascade == 0 ? visHi_ : visLo_).push_back(c.veg[set][i]);
                    continue;
                }
                Vector3 v = Vector3Subtract(p, cam_.position);
                float d2 = v.x * v.x + v.z * v.z;
                if (d2 > md2) continue;
                if (!inFrustum({p.x, p.y + th * 0.5f, p.z}, th * 0.6f + (rocks ? 1.f : 0.f))) continue;
                if (rocks) { visHi_.push_back(c.veg[set][i]); continue; }
                float jit = 0.85f + 0.3f * hash2(int(p.x * 4.f), int(p.z * 4.f), 31);
                float d = std::sqrt(d2);
                if (bushes) { (d < bushHi * jit ? visHi_ : visLo_).push_back(c.veg[set][i]); continue; }
                (impDev >= 0 ? (d < impDev ? visHi_ : visFar_) : d < hiDist * jit ? visHi_ : d < loDist * jit ? visLo_ : visFar_).push_back(c.veg[set][i]);
            }
        }
        Material& m = sp ? depthInstMat_ : foliageMat_;
        Shader sh = m.shader;
        setF(sh, "time", time_);
        setF(sh, "windAmount", rocks ? 0.f : bushes ? wind_ * 0.5f : wind_);
        setF(sh, "treeHeight", rocks ? 1.f : built->height);
        if (!sp) setI(sh, "kind", rocks ? 3 : 0);
        if (rocks) {
            if (!visHi_.empty()) DrawMeshInstanced(rocks_[set - VEG_ROCK0], m, visHi_.data(), int(visHi_.size()));
            if (!sp && std::getenv("CORERAY_COUNT")) { static int fr2 = 0; if (++fr2 % 80 == 0) std::fprintf(stderr, "[count] rocks set %d: %zu\n", set, visHi_.size()); }
            continue;
        }
        if (!visHi_.empty()) DrawMeshInstanced(built->hi, m, visHi_.data(), int(visHi_.size()));
        const Mesh& loMesh = sp && cascade == 1 && built->shadow.vaoId ? built->shadow : built->lo;
        if (!visLo_.empty()) DrawMeshInstanced(loMesh, m, visLo_.data(), int(visLo_.size()));
        if (!sp && !bushes) drawImpostors(set, visFar_);
        if (!sp && std::getenv("CORERAY_COUNT")) {
            static int fr = 0;
            if (set == 0) ++fr;
            if (fr % 20 == 0) std::fprintf(stderr, "[count] set %d hi %zu x %d tris  lo %zu x %d tris  imp %zu\n", set, visHi_.size(), built->hi.triangleCount, visLo_.size(), built->lo.triangleCount, visFar_.size());
        }
    }
    rlEnableBackfaceCulling();
    if (!sp) prof::mark(prof::Foliage);

    // props
    const float hw = course_->roadHalfWidth();
    for (auto& m : signposts_) {
        Vector3 p{m.m12, m.m13, m.m14};
        if (sp ? (std::fabs(p.x - focus.x) > radius || std::fabs(p.z - focus.z) > radius) : Vector3Distance(p, cam_.position) > 900.f) continue;
        drawPart(cyl_, MatrixScale(0.06f, 2.2f, 0.06f), m, {200, 200, 200, 255}, 0.4f, 0.5f, sp);
        drawPart(cube_, MatrixMultiply(MatrixScale(0.08f, 0.6f, 0.9f), MatrixTranslate(0, 2.0f, 0)), m, {250, 200, 40, 255}, 0.5f, 0.3f, sp, 2);
    }
    auto arch = [&](const Matrix& base, Color band) {
        Vector3 p{base.m12, base.m13, base.m14};
        bool near = sp ? (std::fabs(p.x - focus.x) < radius + 10 && std::fabs(p.z - focus.z) < radius + 10) : Vector3Distance(p, cam_.position) < 2500.f;
        if (!near) return;
        for (float x : {-(hw + 1.0f), hw + 1.0f})
            drawPart(cube_, MatrixMultiply(MatrixScale(0.5f, 6.5f, 0.5f), MatrixTranslate(x, 3.0f, 0)), base, {30, 30, 36, 255}, 0.5f, 0.3f, sp);
        drawPart(cube_, MatrixMultiply(MatrixScale(2 * hw + 2.5f, 1.2f, 0.3f), MatrixTranslate(0, 6.0f, 0)), base, WHITE, 0.6f, 0.2f, sp, 1);
        drawPart(cube_, MatrixMultiply(MatrixScale(2 * hw + 2.6f, 0.25f, 0.35f), MatrixTranslate(0, 6.7f, 0)), base, band, 0.5f, 0.3f, sp, 0, 0.3f + L_.night * 3.f);
    };
    // stone bridges where rivers pass under the road
    for (auto& b : bridges_) {
        bool near = sp ? (std::fabs(b.c.x - focus.x) < radius + b.span && std::fabs(b.c.z - focus.z) < radius + b.span)
                       : Vector3Distance(b.c, cam_.position) < 2200.f;
        if (!near) continue;
        Matrix base = MatrixMultiply(MatrixMultiply(MatrixRotateX(-std::atan(b.grade)), MatrixRotateY(b.heading)), MatrixTranslate(b.c.x, b.c.y, b.c.z));
        const Color stone{150, 142, 128, 255}, cap{176, 170, 158, 255};
        float drop = std::max(2.f, b.c.y - b.level + 2.2f);
        float wDeck = 2 * hw + 2.6f;
        setF(objectSh_, "waterLine", b.level);
        drawPart(cube_, MatrixMultiply(MatrixScale(wDeck, 0.8f, b.span + 1.f), MatrixTranslate(0, -0.46f, 0)), base, stone, 0.85f, 0.1f, sp, 3);
        for (float x : {-(hw + 1.05f), hw + 1.05f}) {
            drawPart(cube_, MatrixMultiply(MatrixScale(0.42f, 1.0f, b.span + 1.2f), MatrixTranslate(x, 0.45f, 0)), base, stone, 0.9f, 0.08f, sp, 3);
            drawPart(cube_, MatrixMultiply(MatrixScale(0.56f, 0.12f, b.span + 1.4f), MatrixTranslate(x, 1.0f, 0)), base, cap, 0.8f, 0.1f, sp);
        }
        for (float z : {-(b.span * 0.5f + 0.6f), b.span * 0.5f + 0.6f})  // abutments (tops just under the tar)
            drawPart(cube_, MatrixMultiply(MatrixScale(wDeck + 0.4f, drop, 2.2f), MatrixTranslate(0, -0.12f - drop * 0.5f, z)), base, stone, 0.9f, 0.08f, sp, 3);
        if (b.span > 16.f)  // a pier mid-stream
            drawPart(cube_, MatrixMultiply(MatrixScale(wDeck - 0.8f, drop + 1.f, 1.6f), MatrixTranslate(0, -0.6f - drop * 0.5f, 0)), base, stone, 0.9f, 0.08f, sp, 3);
        setF(objectSh_, "waterLine", -1e6f);
    }

    arch(archBase_, {232, 64, 48, 255});
    if (hasFinish_) arch(finishBase_, {74, 222, 128, 255});
}
