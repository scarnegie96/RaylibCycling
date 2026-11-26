#include "MapPreview.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "RenderUtil.h"
#include "Ui.h"
#include "raymath.h"
#include "rlgl.h"
#include "world/WorldMap.h"

using rutil::MeshBuilder;

namespace {
// stylised map palette per biome (Meadow, Forest, Boreal, Alpine, Steppe, Savanna, Scrub, Dunes, Badlands, Wetland)
const float PAL[wmap::BIOMES][3] = {
    {132, 182, 86}, {62, 122, 66}, {78, 116, 94}, {184, 182, 176}, {200, 190, 122},
    {208, 180, 98}, {164, 152, 96}, {236, 210, 154}, {204, 124, 82}, {96, 146, 122},
};
constexpr int G = 181;                // terrain grid per side (G*G + skirt < 65536 vertices)
constexpr float BASE = -0.035f;       // bottom of the block (model units)
constexpr float LIFT_CASE = 0.0020f, LIFT_FILL = 0.0028f;
const Color MAIN_FILL{255, 210, 92, 255}, MAIN_CASE{70, 62, 50, 255};
const Color MINOR_FILL{252, 252, 250, 255}, MINOR_CASE{78, 82, 92, 255};
const Color BRIDGE_CASE{214, 112, 52, 255}, TUNNEL_FILL{120, 124, 132, 255};
}  // namespace

struct MapPreview::Cpu {
    float L = 1, cx = 0, cz = 0, href = 0, ex = 2;
    std::vector<float> hgrid;   // model-space top height (land or water), G x G
    MeshBuilder terrain;
    std::vector<MeshBuilder> roads;
    struct TownM { Vector3 p; std::string name; int rank; };
    std::vector<TownM> towns;
    float modelY(float h) const { return (h - href) / L * ex; }
    float heightN(float nx, float nz) const {
        float fx = std::clamp((nx + 0.5f) * (G - 1), 0.f, float(G - 1)), fz = std::clamp((nz + 0.5f) * (G - 1), 0.f, float(G - 1));
        int i = std::min(int(fx), G - 2), k = std::min(int(fz), G - 2);
        float u = fx - i, v = fz - k;
        auto H = [&](int a, int b) { return hgrid[size_t(b) * G + a]; };
        return (H(i, k) * (1 - u) + H(i + 1, k) * u) * (1 - v) + (H(i, k + 1) * (1 - u) + H(i + 1, k + 1) * u) * v;
    }
};

static std::shared_ptr<MapPreview::Cpu> buildCpu(std::shared_ptr<const wmap::Map> mp) {
    const wmap::Map& M = *mp;
    auto C = std::make_shared<MapPreview::Cpu>();
    C->L = (M.n - 1) * M.cell;
    C->cx = M.x0 + 0.5f * C->L;
    C->cz = M.z0 + 0.5f * C->L;
    // ---- sample the land
    std::vector<wmap::Sample> sm(size_t(G) * G);
    float hmin = 1e9f, hmax = -1e9f;
    for (int k = 0; k < G; ++k)
        for (int i = 0; i < G; ++i) {
            float x = M.x0 + C->L * i / (G - 1), z = M.z0 + C->L * k / (G - 1);
            auto& s = sm[size_t(k) * G + i];
            s = M.sample(x, z);
            float top = std::max(s.h, s.water);
            hmin = std::min(hmin, top);
            hmax = std::max(hmax, top);
        }
    C->href = hmin;
    // vertical exaggeration: flat maps get more, alpine ones less (relief ~0.06..0.16 of the width)
    float rel = std::max(1.f, hmax - hmin) / C->L;
    C->ex = std::clamp(0.12f / rel, 1.2f, 3.2f);
    C->hgrid.resize(size_t(G) * G);
    for (size_t q = 0; q < sm.size(); ++q) C->hgrid[q] = C->modelY(std::max(sm[q].h, sm[q].water));
    // ---- terrain top: baked hillshade (light from the north-west, high)
    const Vector3 light = Vector3Normalize({-0.55f, 0.75f, -0.45f});
    const float step = 1.f / (G - 1);
    auto Hg = [&](int i, int k) { return C->hgrid[size_t(std::clamp(k, 0, G - 1)) * G + std::clamp(i, 0, G - 1)]; };
    MeshBuilder& T = C->terrain;
    for (int k = 0; k < G; ++k)
        for (int i = 0; i < G; ++i) {
            const auto& s = sm[size_t(k) * G + i];
            float y = Hg(i, k);
            Vector3 n = Vector3Normalize({-(Hg(i + 1, k) - Hg(i - 1, k)) / (2 * step), 1.f, -(Hg(i, k + 1) - Hg(i, k - 1)) / (2 * step)});
            float r = 0, g = 0, b = 0;
            for (int q = 0; q < wmap::BIOMES; ++q) { r += PAL[q][0] * s.w[q]; g += PAL[q][1] * s.w[q]; b += PAL[q][2] * s.w[q]; }
            r += (232 - r) * s.sand * 0.6f; g += (208 - g) * s.sand * 0.6f; b += (156 - b) * s.sand * 0.6f;
            if (s.wet > 0.3f) { r *= 0.92f; g *= 0.97f; }
            float snow = std::clamp((-0.5f - s.temp) / 2.f, 0.f, 1.f);
            r += (246 - r) * snow; g += (248 - g) * snow; b += (252 - b) * snow;
            float sh = 0.5f + 0.62f * std::max(0.f, Vector3DotProduct(n, light));
            float lift = 0.94f + 0.12f * std::clamp((y - 0.f) / 0.15f, 0.f, 1.f);   // higher ground a touch brighter
            r *= sh * lift; g *= sh * lift; b *= sh * lift;
            if (s.water > s.h + 0.05f) {   // water: deeper is darker, a little sheen
                float depth = std::clamp((s.water - s.h) / 12.f, 0.f, 1.f);
                r = 104 - 40 * depth; g = 168 - 46 * depth; b = 222 - 30 * depth;
            }
            Color c{(unsigned char)std::clamp(int(r), 0, 255), (unsigned char)std::clamp(int(g), 0, 255), (unsigned char)std::clamp(int(b), 0, 255), 255};
            T.add({-0.5f + i * step, y, -0.5f + k * step}, n, {0, 0}, c);
        }
    for (int k = 0; k < G - 1; ++k)
        for (int i = 0; i < G - 1; ++i) {
            unsigned short a = (unsigned short)(k * G + i);
            T.tri(a, a + G, a + 1);
            T.tri(a + 1, a + G, a + G + 1);
        }
    // ---- skirt: the block's sides, soil darkening towards the base
    auto side = [&](int i0, int k0, int di, int dk, Vector3 n) {
        unsigned short base = (unsigned short)(T.v.size() / 3);
        for (int s = 0; s < G; ++s) {
            int i = i0 + di * s, k = k0 + dk * s;
            float x = -0.5f + i * step, z = -0.5f + k * step, y = Hg(i, k);
            T.add({x, y, z}, n, {0, 0}, Color{150, 128, 104, 255});
            T.add({x, BASE, z}, n, {0, 0}, Color{96, 82, 70, 255});
        }
        for (int s = 0; s < G - 1; ++s) {
            unsigned short a = base + s * 2;
            T.tri(a, a + 1, a + 2);
            T.tri(a + 2, a + 1, a + 3);
        }
    };
    side(0, 0, 1, 0, {0, 0, -1});
    side(0, G - 1, 1, 0, {0, 0, 1});
    side(0, 0, 0, 1, {-1, 0, 0});
    side(G - 1, 0, 0, 1, {1, 0, 0});
    // ---- roads: ribbons (dark casing under a coloured fill), decimated to ~1/700 of the width
    const float keep = C->L / 700.f;
    MeshBuilder* cur = nullptr;
    auto ribbon = [&](const std::vector<Vector3>& P, const std::vector<int>& dash, float halfW, float lift, Color col) {
        if (P.size() < 2) return;
        if (!cur || cur->v.size() / 3 + P.size() * 2 > 60000) { C->roads.emplace_back(); cur = &C->roads.back(); }
        unsigned short base = (unsigned short)(cur->v.size() / 3);
        for (size_t j = 0; j < P.size(); ++j) {
            Vector3 a = P[j == 0 ? 0 : j - 1], b = P[j + 1 < P.size() ? j + 1 : j];
            Vector2 d{b.x - a.x, b.z - a.z};
            float l = std::max(1e-6f, std::sqrt(d.x * d.x + d.y * d.y));
            Vector2 nrm{-d.y / l, d.x / l};
            cur->add({P[j].x + nrm.x * halfW, P[j].y + lift, P[j].z + nrm.y * halfW}, {0, 1, 0}, {0, 0}, col);
            cur->add({P[j].x - nrm.x * halfW, P[j].y + lift, P[j].z - nrm.y * halfW}, {0, 1, 0}, {0, 0}, col);
        }
        for (size_t j = 0; j + 1 < P.size(); ++j) {
            if (!dash.empty() && !dash[j]) continue;
            unsigned short a = base + j * 2;
            cur->tri(a, a + 2, a + 1);
            cur->tri(a + 1, a + 2, a + 3);
        }
    };
    // rivers: blue ribbons (width from the channel, kept visible), under the roads
    for (const auto& rv : M.rivers) {
        if (rv.dry || rv.pts.size() < 2) continue;
        std::vector<Vector3> P;
        float acc = keep, hw = 0;
        for (size_t j = 0; j < rv.pts.size(); ++j) {
            const auto& p = rv.pts[j];
            if (j > 0) acc += std::hypot(p.x - rv.pts[j - 1].x, p.z - rv.pts[j - 1].z);
            if (acc < keep && j + 1 != rv.pts.size()) continue;
            acc = 0;
            float nx = (p.x - C->cx) / C->L, nz = (p.z - C->cz) / C->L;
            P.push_back({nx, std::max(C->modelY(p.level), C->heightN(nx, nz)), nz});
            hw = std::max(hw, p.half);
        }
        float w = std::clamp(hw / C->L * 1.6f, 0.0008f, 0.0032f);
        ribbon(P, {}, w, 0.0012f, Color{92, 156, 214, 255});
    }
    // minor roads first, main roads drawn over them
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& R : M.roads) {
            bool main = R.cls == 0;
            if ((pass == 0) == main) continue;
            // split the road into runs of the same kind (land / tunnel / bridge)
            std::vector<Vector3> P;
            std::vector<int> dash;
            int runKind = -1;
            float acc = keep;
            auto flush = [&](int kind) {
                if (P.size() >= 2) {
                    float hw = main ? 0.0021f : 0.0014f;
                    if (kind == 1) {   // tunnel: a dashed grey line over the hill
                        ribbon(P, dash, hw * 0.8f, LIFT_FILL, TUNNEL_FILL);
                    } else {
                        ribbon(P, {}, hw + 0.0010f, LIFT_CASE, kind == 2 ? BRIDGE_CASE : main ? MAIN_CASE : MINOR_CASE);
                        ribbon(P, {}, hw, LIFT_FILL, main ? MAIN_FILL : MINOR_FILL);
                    }
                }
                P.clear();
                dash.clear();
            };
            for (size_t j = 0; j < R.pts.size(); ++j) {
                const auto& p = R.pts[j];
                if (int(p.kind) != runKind) {
                    if (runKind >= 0) {
                        // close the run on this point so runs join up
                        float nx = (p.x - C->cx) / C->L, nz = (p.z - C->cz) / C->L;
                        float y = runKind == 2 ? std::max(C->modelY(p.y), C->heightN(nx, nz)) : C->heightN(nx, nz);
                        P.push_back({nx, y, nz});
                        dash.push_back(int(P.size() / 3) % 2);
                        flush(runKind);
                    }
                    runKind = p.kind;
                    acc = keep;
                }
                if (j > 0) acc += std::hypot(p.x - R.pts[j - 1].x, p.z - R.pts[j - 1].z);
                if (acc < keep && j + 1 != R.pts.size()) continue;
                acc = 0;
                float nx = (p.x - C->cx) / C->L, nz = (p.z - C->cz) / C->L;
                float y = p.kind == 2 ? std::max(C->modelY(p.y), C->heightN(nx, nz)) : C->heightN(nx, nz);
                P.push_back({nx, y, nz});
                dash.push_back(int(P.size() / 3) % 2);
            }
            flush(runKind);
        }
    for (const auto& t : M.towns) {
        float nx = (t.x - C->cx) / C->L, nz = (t.z - C->cz) / C->L;
        C->towns.push_back({{nx, C->heightN(nx, nz), nz}, t.name, t.rank});
    }
    return C;
}

MapPreview::~MapPreview() {
    if (job_.valid()) job_.wait();   // GPU objects are freed by release(), while the window is still open
}

void MapPreview::release() {
    if (job_.valid()) job_.wait();
    unloadGpu();
    if (rt_.id) UnloadRenderTexture(rt_);
    rt_ = RenderTexture2D{};
    rtW_ = rtH_ = 0;
    if (matLoaded_) UnloadMaterial(mat_);   // default shader / texture are left alone by raylib
    matLoaded_ = false;
    map_.reset();
    cpu_.reset();
}

void MapPreview::unloadGpu() {
    for (auto& m : meshes_) UnloadMesh(m);
    meshes_.clear();
    uploaded_ = false;
}

void MapPreview::setMap(std::shared_ptr<const wmap::Map> m) {
    if (m == map_) return;
    if (job_.valid()) job_.wait();
    unloadGpu();
    cpu_.reset();
    hover_ = Spawn{};
    map_ = m;
    if (m) job_ = std::async(std::launch::async, buildCpu, m);
}

void MapPreview::resetView() { yaw_ = 0.65f; pitch_ = 0.9f; dist_ = 2.0f; tx_ = tz_ = 0; }

void MapPreview::upload() {
    meshes_.push_back(cpu_->terrain.build(true));
    for (auto& r : cpu_->roads) meshes_.push_back(r.build(true));
    cpu_->terrain = MeshBuilder{};   // CPU copies no longer needed
    cpu_->roads.clear();
    if (!matLoaded_) { mat_ = LoadMaterialDefault(); matLoaded_ = true; }
    uploaded_ = true;
}

float MapPreview::heightN(float nx, float nz) const { return cpu_ ? cpu_->heightN(nx, nz) : 0.f; }

Vector3 MapPreview::roadPoint(int road, int idx) const {
    const auto& p = map_->roads[road].pts[std::clamp(idx, 0, int(map_->roads[road].pts.size()) - 1)];
    float nx = (p.x - cpu_->cx) / cpu_->L, nz = (p.z - cpu_->cz) / cpu_->L;
    float y = p.kind == 2 ? std::max(cpu_->modelY(p.y), heightN(nx, nz)) : heightN(nx, nz);
    return {nx, y + LIFT_FILL, nz};
}

bool MapPreview::pick(Vector2 mouse, Rectangle r, Spawn& out) const {
    if (!ready()) return false;
    Ray ray = GetScreenToWorldRayEx({(mouse.x - r.x) / r.width * rtW_, (mouse.y - r.y) / r.height * rtH_}, cam_, rtW_, rtH_);
    // march the height field, then refine
    float t0 = 0, hitT = -1;
    for (float t = 0.01f; t < 8.f; t += 0.0025f) {
        Vector3 p = Vector3Add(ray.position, Vector3Scale(ray.direction, t));
        if (std::fabs(p.x) > 0.5f || std::fabs(p.z) > 0.5f) { t0 = t; continue; }
        if (p.y <= heightN(p.x, p.z)) {
            float a = t0, b = t;
            for (int k = 0; k < 14; ++k) {
                float m = 0.5f * (a + b);
                Vector3 q = Vector3Add(ray.position, Vector3Scale(ray.direction, m));
                (q.y <= heightN(q.x, q.z) ? b : a) = m;
            }
            hitT = b;
            break;
        }
        t0 = t;
        if (p.y < BASE) break;
    }
    if (hitT < 0) return false;
    Vector3 hp = Vector3Add(ray.position, Vector3Scale(ray.direction, hitT));
    float wx = hp.x * cpu_->L + cpu_->cx, wz = hp.z * cpu_->L + cpu_->cz;
    // snap to the nearest road point (not inside a tunnel) within ~18 px
    float mpp = hitT * 2.f * std::tan(cam_.fovy * 0.5f * DEG2RAD) / std::max(1.f, r.height) * cpu_->L;
    float tol = 18.f * mpp, best = tol * tol;
    int br = -1, bi = 0;
    for (int ri = 0; ri < int(map_->roads.size()); ++ri) {
        const auto& P = map_->roads[ri].pts;
        for (int i = 0; i < int(P.size()); ++i) {
            if (P[i].kind == 1) continue;
            float dx = P[i].x - wx, dz = P[i].z - wz, d2 = dx * dx + dz * dz;
            if (d2 < best) { best = d2; br = ri; bi = i; }
        }
    }
    if (br < 0) return false;
    out.set = true;
    out.road = br;
    out.idx = bi;
    out.fwd = bi < int(map_->roads[br].pts.size()) / 2;   // the longer way along this road
    return true;
}

bool MapPreview::draw(Rectangle r, Spawn& spawn, float dt) {
    using namespace ui;
    anim_ += dt;
    bool changed = false;
    if (job_.valid() && job_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        cpu_ = job_.get();
        upload();
    }
    // ---- render target at the panel's pixel size (2x supersampled on non-Retina screens)
    float dpi = GetWindowScaleDPI().x;
    float ss = dpi >= 1.5f ? 1.f : 2.f;
    const float us = ui::scale();   // the UI's own zoom: the panel is r * us points on screen
    int w = std::max(16, int(r.width * us * dpi * ss)), h = std::max(16, int(r.height * us * dpi * ss));
    if (w != rtW_ || h != rtH_) {
        if (rt_.id) UnloadRenderTexture(rt_);
        rt_ = LoadRenderTexture(w, h);
        SetTextureFilter(rt_.texture, TEXTURE_FILTER_BILINEAR);
        rtW_ = w; rtH_ = h;
    }
    // ---- input
    Vector2 mouse = ui::mouseLogical();
    bool over = hovered(r);
    if (ready()) {
        float wheel = over ? GetMouseWheelMove() : 0.f;
        if (wheel != 0) {
            float nd = std::clamp(dist_ * std::pow(0.87f, wheel), 0.26f, 2.6f);
            if (hover_.set && wheel > 0) {   // zoom towards the road under the cursor
                Vector3 hp = roadPoint(hover_.road, hover_.idx);
                float f = 1.f - nd / dist_;
                tx_ += (hp.x - tx_) * f;
                tz_ += (hp.z - tz_) * f;
            }
            dist_ = nd;
            interacted_ = true;
        }
        if (over && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { dragging_ = true; dragMoved_ = false; dragStart_ = mouse; }
        if (over && (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) || IsMouseButtonPressed(MOUSE_BUTTON_MIDDLE))) rdragging_ = true;
        Vector2 md = GetMouseDelta();
        if (dragging_ && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
            if (Vector2Distance(mouse, dragStart_) > 4) dragMoved_ = true;
            if (dragMoved_) {
                interacted_ = true;
                yaw_ -= md.x * 0.006f;
                pitch_ = std::clamp(pitch_ + md.y * 0.005f, 0.2f, 1.45f);
            }
        }
        if (rdragging_ && (IsMouseButtonDown(MOUSE_BUTTON_RIGHT) || IsMouseButtonDown(MOUSE_BUTTON_MIDDLE))) {
            float k = cdist_ * 0.0017f;
            Vector2 fw{-std::sin(cyaw_), -std::cos(cyaw_)}, rt{std::cos(cyaw_), -std::sin(cyaw_)};
            tx_ = std::clamp(tx_ - rt.x * md.x * k + fw.x * md.y * k, -0.5f, 0.5f);
            tz_ = std::clamp(tz_ - rt.y * md.x * k + fw.y * md.y * k, -0.5f, 0.5f);
        } else rdragging_ = false;
        if (dragging_ && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
            if (!dragMoved_ && over && hover_.set) {
                if (spawn.set && spawn.road == hover_.road && std::abs(spawn.idx - hover_.idx) <= 2) spawn.fwd = !spawn.fwd;   // click the pin again: turn round
                else spawn = hover_;
                changed = true;
            }
            dragging_ = false;
        }
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) dragging_ = false;
    }
    // smoothed camera
    float a = std::min(1.f, dt * 12.f);
    cyaw_ += (yaw_ - cyaw_) * a; cpitch_ += (pitch_ - cpitch_) * a; cdist_ += (dist_ - cdist_) * a;
    ctx_ += (tx_ - ctx_) * a; ctz_ += (tz_ - ctz_) * a;
    float ty = ready() ? heightN(ctx_, ctz_) * 0.6f : 0.f;
    cam_.target = {ctx_, ty, ctz_};
    cam_.position = {ctx_ + cdist_ * std::cos(cpitch_) * std::sin(cyaw_), ty + cdist_ * std::sin(cpitch_), ctz_ + cdist_ * std::cos(cpitch_) * std::cos(cyaw_)};
    cam_.up = {0, 1, 0};
    cam_.fovy = 34;
    cam_.projection = CAMERA_PERSPECTIVE;
    // hover (after the camera has moved this frame)
    hover_ = Spawn{};
    if (ready() && over && !dragMoved_ && !rdragging_) pick(mouse, r, hover_);

    // ---- render
    BeginTextureMode(rt_);
    ClearBackground({246, 248, 251, 255});
    DrawRectangleGradientV(0, 0, rtW_, rtH_, {232, 239, 247, 255}, {249, 250, 252, 255});
    if (ready()) {
        BeginMode3D(cam_);
        rlDisableBackfaceCulling();
        // soft contact shadow under the block
        for (int k = 0; k < 4; ++k) {
            float s = 1.02f + k * 0.035f;
            DrawPlane({0.015f * k, BASE - 0.001f - k * 0.0002f, 0.02f * k}, {s, s}, Color{20, 30, 50, (unsigned char)(26 - k * 5)});
        }
        for (auto& m : meshes_) DrawMesh(m, mat_, MatrixIdentity());
        // towns: little clusters of houses
        float sc = std::clamp(cdist_ / 2.0f, 0.3f, 1.3f);
        for (const auto& t : cpu_->towns) {
            int n = t.rank >= 2 ? 6 : 3;
            for (int q = 0; q < n; ++q) {
                float ang = q * 2.4f + t.p.x * 40.f, rad = (q == 0 ? 0.f : 0.004f + 0.0016f * q) * sc;
                float hx = t.p.x + std::cos(ang) * rad, hz = t.p.z + std::sin(ang) * rad;
                float hy = heightN(hx, hz);
                float s = (t.rank >= 2 ? 0.0034f : 0.0026f) * sc;
                DrawCube({hx, hy + s * 0.5f, hz}, s, s, s, Color{250, 247, 240, 255});
                DrawCube({hx, hy + s * 1.12f, hz}, s * 1.1f, s * 0.3f, s * 1.1f, Color{200, 86, 64, 255});
            }
        }
        // hover marker and the spawn pin with its direction of travel
        auto arrow = [&](const Spawn& s, Color c, float size) {
            const auto& P = map_->roads[s.road].pts;
            int n = int(P.size());
            int j = std::clamp(s.idx + (s.fwd ? 6 : -6), 0, n - 1);
            Vector3 p = roadPoint(s.road, s.idx), q = roadPoint(s.road, j);
            Vector3 d = Vector3Subtract(q, p);
            d.y = 0;
            if (Vector3Length(d) < 1e-6f) return;
            d = Vector3Normalize(d);
            Vector3 b0 = Vector3Add(Vector3Add(p, {0, 0.004f * size, 0}), Vector3Scale(d, 0.012f * size));
            Vector3 b1 = Vector3Add(b0, Vector3Scale(d, 0.04f * size));
            DrawCylinderEx(b0, b1, 0.0042f * size, 0.0042f * size, 10, c);
            DrawCylinderEx(b1, Vector3Add(b1, Vector3Scale(d, 0.024f * size)), 0.012f * size, 0.f, 16, c);
        };
        if (hover_.set) {
            Vector3 p = roadPoint(hover_.road, hover_.idx);
            DrawSphere(p, 0.0055f * sc, WHITE);
            DrawCylinder(p, 0.012f * sc, 0.012f * sc, 0.0008f, 24, Color{255, 255, 255, 120});
        }
        if (spawn.set && map_ && spawn.road >= 0 && spawn.road < int(map_->roads.size())) {
            Vector3 p = roadPoint(spawn.road, spawn.idx);
            arrow(spawn, ui::ACCENT, sc);
            float pulse = 0.5f + 0.5f * std::sin(anim_ * 3.f);
            DrawCylinder(p, (0.012f + 0.006f * pulse) * sc, (0.012f + 0.006f * pulse) * sc, 0.0009f, 28, Color{15, 157, 88, (unsigned char)(110 - 70 * pulse)});
        }
        rlEnableBackfaceCulling();
        EndMode3D();
    }
    EndTextureMode();
    ui::reapplyScale();

    // ---- composite with rounded corners
    DrawTexturePro(rt_.texture, {0, 0, float(rtW_), -float(rtH_)}, r, {0, 0}, 0, WHITE);
    // round the corners: paint the card colour over each corner outside a quarter circle
    const float rad = 14;
    const Vector2 cs[4] = {{r.x + rad, r.y + rad}, {r.x + r.width - rad, r.y + rad}, {r.x + r.width - rad, r.y + r.height - rad}, {r.x + rad, r.y + r.height - rad}};
    const float a0[4] = {180, 270, 0, 90};
    for (int k = 0; k < 4; ++k) DrawRing(cs[k], rad, rad * 1.5f, a0[k], a0[k] + 90, 12, PANEL);
    DrawRectangleRoundedLinesEx(r, rad * 2 / std::min(r.width, r.height), 16, 1, Color{15, 23, 42, 22});

    if (!ready()) {   // building the model
        Vector2 c{r.x + r.width / 2, r.y + r.height / 2 - 14};
        DrawRing(c, 15, 19, anim_ * 360.f, anim_ * 360.f + 260.f, 32, ACCENT);
        textCentered("Building the 3D model...", {r.x, c.y + 30, r.width, 24}, F::Small, DIM);
        return changed;
    }
    // where the start pin will be drawn (labels keep clear of it)
    bool pinOn = false;
    Vector2 pinBase{}, pinHead{};
    if (spawn.set && spawn.road >= 0 && spawn.road < int(map_->roads.size())) {
        Vector3 p = roadPoint(spawn.road, spawn.idx);
        Vector3 toP = Vector3Subtract(p, cam_.position), fwd = Vector3Subtract(cam_.target, cam_.position);
        if (Vector3DotProduct(toP, fwd) > 0) {
            Vector2 sp = GetWorldToScreenEx(p, cam_, rtW_, rtH_);
            pinBase = {r.x + sp.x / rtW_ * r.width, r.y + sp.y / rtH_ * r.height};
            pinHead = {pinBase.x, pinBase.y - 34 + 2.f * std::sin(anim_ * 3.f)};
            pinOn = true;
        }
    }
    const Rectangle pinBox{pinHead.x - 15, pinHead.y - 15, 30, pinBase.y - pinHead.y + 18};
    // ---- town labels (projected)
    ui::beginScissor({r.x, r.y, r.width, r.height});
    for (const auto& t : cpu_->towns) {
        if (t.rank < 2 && cdist_ > 1.25f) continue;
        Vector3 lp = Vector3Add(t.p, {0, 0.018f, 0});
        Vector3 toP = Vector3Subtract(lp, cam_.position), fwd = Vector3Subtract(cam_.target, cam_.position);
        if (Vector3DotProduct(toP, fwd) <= 0) continue;
        Vector2 sp = GetWorldToScreenEx(lp, cam_, rtW_, rtH_);
        float sx = r.x + sp.x / rtW_ * r.width, sy = r.y + sp.y / rtH_ * r.height;
        if (sx < r.x || sx > r.x + r.width || sy < r.y || sy > r.y + r.height) continue;
        float fs = t.rank >= 2 ? 14.f : 12.f;
        Vector2 m = measure(t.name, F::Small, fs);
        Rectangle pill{sx - m.x / 2 - 8, sy - m.y - 8, m.x + 16, m.y + 6};
        if (pinOn && CheckCollisionRecs(pill, pinBox)) pill.x = pinBox.x + pinBox.width + 4;   // step aside for the pin
        DrawRectangleRounded({pill.x, pill.y + 1.5f, pill.width, pill.height}, 1, 8, Color{15, 23, 42, 28});
        DrawRectangleRounded(pill, 1, 8, withAlpha(PANEL, 235));
        text(t.name, pill.x + 8, pill.y + 3, F::Small, t.rank >= 2 ? TEXT : DIM, fs);
    }
    // the start pin, in screen space so it stays crisp and on top of the labels
    if (pinOn) {
        {
            Vector2 base = pinBase, head = pinHead;
            DrawEllipse(int(base.x), int(base.y), 7, 3, Color{15, 23, 42, 60});
            DrawLineEx(base, head, 3.f, Color{30, 36, 48, 255});
            DrawCircleV({head.x, head.y + 1.5f}, 13, Color{15, 23, 42, 40});
            DrawCircleV(head, 12.5f, WHITE);
            DrawCircleV(head, 10, ACCENT);
            DrawCircleV(head, 4, WHITE);
        }
    }
    // hover hint at the cursor
    if (hover_.set && !dragging_) {
        bool again = spawn.set && spawn.road == hover_.road && std::abs(spawn.idx - hover_.idx) <= 2;
        const char* hint = again ? "Click to turn around" : "Click to start here";
        Vector2 m = measure(hint, F::Small, 13);
        Rectangle tip{mouse.x + 14, mouse.y + 12, m.x + 18, m.y + 10};
        DrawRectangleRounded(tip, 1, 8, Color{17, 24, 39, 225});
        text(hint, tip.x + 9, tip.y + 5, F::Small, WHITE, 13);
    }
    EndScissorMode();
    return changed;
}
