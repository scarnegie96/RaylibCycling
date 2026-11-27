// Volumetric clouds, fog, light cones and rain shafts (see VolShaders.h for the model).
//
// Start-up: the tiling 3D noise fields (Perlin-Worley shape 128^3, Worley detail 32^3) and
// the 2D weather map are generated on the CPU across threads and packed into textures.
// Per frame: march (reduced resolution, jittered) -> temporal resolve -> depth-aware
// upsample onto the scene. The lamps' halos and the rain / snow particles are drawn after,
// into the fogged scene, so they sit in front of the fog they are in.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "Renderer.h"
#include "RenderUtil.h"
#include "VolShaders.h"

using namespace rutil;

namespace {

// ---------------------------------------------------------------- tiling noise
inline uint32_t hash3(int x, int y, int z, uint32_t seed) {
    uint32_t h = uint32_t(x) * 0x8da6b343u ^ uint32_t(y) * 0xd8163841u ^ uint32_t(z) * 0xcb1ab31fu ^ seed * 0x165667b1u;
    h ^= h >> 15; h *= 0x2c1b3c6du; h ^= h >> 12; h *= 0x297a2d39u; h ^= h >> 15;
    return h;
}
inline float h01(uint32_t h) { return (h & 0xFFFFFF) / float(0x1000000); }
inline int wrap(int a, int p) { return ((a % p) + p) % p; }
inline float fade(float t) { return t * t * t * (t * (t * 6 - 15) + 10); }

// gradient noise that tiles with period p (in lattice cells), roughly in [-1, 1]
float perlin3(float x, float y, float z, int p, uint32_t seed) {
    int xi = int(std::floor(x)), yi = int(std::floor(y)), zi = int(std::floor(z));
    float fx = x - xi, fy = y - yi, fz = z - zi;
    static const float G[12][3] = {{1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1}, {-1, 0, 1},
                                   {1, 0, -1}, {-1, 0, -1}, {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}};
    auto g = [&](int a, int b, int c, float dx, float dy, float dz) {
        const float* v = G[hash3(wrap(a, p), wrap(b, p), wrap(c, p), seed) % 12];
        return v[0] * dx + v[1] * dy + v[2] * dz;
    };
    float u = fade(fx), v = fade(fy), w = fade(fz);
    float x00 = g(xi, yi, zi, fx, fy, fz) + u * (g(xi + 1, yi, zi, fx - 1, fy, fz) - g(xi, yi, zi, fx, fy, fz));
    float x10 = g(xi, yi + 1, zi, fx, fy - 1, fz) + u * (g(xi + 1, yi + 1, zi, fx - 1, fy - 1, fz) - g(xi, yi + 1, zi, fx, fy - 1, fz));
    float x01 = g(xi, yi, zi + 1, fx, fy, fz - 1) + u * (g(xi + 1, yi, zi + 1, fx - 1, fy, fz - 1) - g(xi, yi, zi + 1, fx, fy, fz - 1));
    float x11 = g(xi, yi + 1, zi + 1, fx, fy - 1, fz - 1) + u * (g(xi + 1, yi + 1, zi + 1, fx - 1, fy - 1, fz - 1) - g(xi, yi + 1, zi + 1, fx, fy - 1, fz - 1));
    float y0 = x00 + v * (x10 - x00), y1 = x01 + v * (x11 - x01);
    return y0 + w * (y1 - y0);
}
// u,v,w in [0,1): fbm in [0,1], tiling
float perlinFbm(float u, float v, float w, int base, int oct, uint32_t seed) {
    float s = 0, a = 1, norm = 0;
    int p = base;
    for (int i = 0; i < oct; ++i) {
        s += perlin3(u * p, v * p, w * p, p, seed + i * 101) * a;
        norm += a; a *= 0.5f; p *= 2;
    }
    return std::clamp(s / norm * 0.85f + 0.5f, 0.f, 1.f);
}
// inverted F1 cellular noise (1 at feature points), tiling with `cells` per unit
float worley3(float u, float v, float w, int cells, uint32_t seed) {
    float x = u * cells, y = v * cells, z = w * cells;
    int xi = int(std::floor(x)), yi = int(std::floor(y)), zi = int(std::floor(z));
    float best = 9;
    for (int dz = -1; dz <= 1; ++dz)
        for (int dy = -1; dy <= 1; ++dy)
            for (int dx = -1; dx <= 1; ++dx) {
                int cx = xi + dx, cy = yi + dy, cz = zi + dz;
                uint32_t h = hash3(wrap(cx, cells), wrap(cy, cells), wrap(cz, cells), seed);
                float px = cx + h01(h), py = cy + h01(h * 0x9E3779B9u + 1), pz = cz + h01(h * 0x85EBCA6Bu + 7);
                float d = (px - x) * (px - x) + (py - y) * (py - y) + (pz - z) * (pz - z);
                best = std::min(best, d);
            }
    return 1.f - std::clamp(std::sqrt(best), 0.f, 1.f);
}
float worleyFbm(float u, float v, float w, int cells, uint32_t seed) {
    return worley3(u, v, w, cells, seed) * 0.625f + worley3(u, v, w, cells * 2, seed + 1) * 0.25f + worley3(u, v, w, cells * 4, seed + 2) * 0.125f;
}
float remapf(float x, float a, float b, float c, float d) { return c + (x - a) / std::max(b - a, 1e-5f) * (d - c); }

template <class F>
void parallelFor(int n, F&& fn) {
    unsigned T = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> th;
    for (unsigned t = 0; t < T; ++t)
        th.emplace_back([&, t] { for (int i = int(t); i < n; i += int(T)) fn(i); });
    for (auto& x : th) x.join();
}

// N^3 voxels -> 2D atlas of slices, each (N+2)^2 with a wrapped one-texel gutter for bilinear
Texture2D buildAtlas(int N, int cols, Vector4& dim, void (*voxel)(int N, int x, int y, int z, unsigned char* out)) {
    int rows = (N + cols - 1) / cols, T = N + 2;
    int W = cols * T, H = rows * T;
    std::vector<unsigned char> vox(size_t(N) * N * N * 4);
    parallelFor(N, [&](int z) {
        for (int y = 0; y < N; ++y)
            for (int x = 0; x < N; ++x) voxel(N, x, y, z, &vox[((size_t(z) * N + y) * N + x) * 4]);
    });
    if (std::getenv("CORERAY_VOLDEBUG")) {
        for (int ch = 0; ch < 4; ++ch) {
            std::vector<int> v;
            for (size_t i = 0; i < vox.size() / 4; i += 7) v.push_back(vox[i * 4 + ch]);
            std::sort(v.begin(), v.end());
            std::fprintf(stderr, "[vol] atlas%d ch%d p10 %d p50 %d p90 %d\n", N, ch, v[v.size() / 10], v[v.size() / 2], v[v.size() * 9 / 10]);
        }
    }
    std::vector<unsigned char> px(size_t(W) * H * 4, 0);
    for (int s = 0; s < N; ++s) {
        int ox = (s % cols) * T, oy = (s / cols) * T;
        for (int ly = 0; ly < T; ++ly)
            for (int lx = 0; lx < T; ++lx) {
                int vx = wrap(lx - 1, N), vy = wrap(ly - 1, N);
                std::memcpy(&px[(size_t(oy + ly) * W + ox + lx) * 4], &vox[((size_t(s) * N + vy) * N + vx) * 4], 4);
            }
    }
    Image im{px.data(), W, H, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    Texture2D t = LoadTextureFromImage(im);
    SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(t, TEXTURE_WRAP_CLAMP);
    dim = {float(N), float(cols), float(W), float(H)};
    return t;
}

unsigned char u8(float v) { return (unsigned char)std::clamp(int(v * 255.f + 0.5f), 0, 255); }

void shapeVoxel(int N, int x, int y, int z, unsigned char* o) {
    float u = (x + 0.5f) / N, v = (y + 0.5f) / N, w = (z + 0.5f) / N;
    float p = perlinFbm(u, v, w, 4, 5, 901);
    float wf = worleyFbm(u, v, w, 4, 911);
    // Perlin dilated by Worley: billowy, connected
    float pw = std::clamp(remapf(p, wf - 1.f, 1.f, 0.f, 1.f), 0.f, 1.f);
    o[0] = u8(pw);
    o[1] = u8(worleyFbm(u, v, w, 4, 921));
    o[2] = u8(worleyFbm(u, v, w, 8, 931));
    o[3] = u8(worleyFbm(u, v, w, 16, 941));
}
void detailVoxel(int N, int x, int y, int z, unsigned char* o) {
    float u = (x + 0.5f) / N, v = (y + 0.5f) / N, w = (z + 0.5f) / N;
    o[0] = u8(worleyFbm(u, v, w, 2, 951));
    o[1] = u8(worleyFbm(u, v, w, 4, 961));
    o[2] = u8(worleyFbm(u, v, w, 8, 971));
    o[3] = 255;
}

Texture2D buildWeatherMap(int n, std::vector<unsigned char>& keep) {
    std::vector<float> f[3];
    for (auto& c : f) c.resize(size_t(n) * n);
    parallelFor(n, [&](int y) {
        for (int x = 0; x < n; ++x) {
            float u = (x + 0.5f) / n, v = (y + 0.5f) / n;
            size_t i = size_t(y) * n + x;
            // coverage: broad Perlin fields broken into cumulus clumps by Worley cells
            float p = perlinFbm(u, v, 0.13f, 3, 6, 1001);
            float w = worley3(u, v, 0.21f, 9, 1011) * 0.6f + worley3(u, v, 0.57f, 18, 1012) * 0.4f;
            f[0][i] = remapf(p, 0.25f - w * 0.45f, 1.f, 0.f, 1.f);
            // cloud type and precipitation cells
            f[1][i] = perlinFbm(u, v, 0.71f, 2, 4, 1021);
            f[2][i] = perlinFbm(u, v, 0.33f, 4, 4, 1031) + 0.25f * f[0][i];
        }
    });
    // equalise each channel at full precision (rank -> uniform 0..1), so "coverage 0.3" really
    // means about 30 % of the sky
    std::vector<unsigned char> px(size_t(n) * n * 4, 255);
    for (int ch = 0; ch < 3; ++ch) {
        std::vector<int> order(size_t(n) * n);
        for (size_t i = 0; i < order.size(); ++i) order[i] = int(i);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return f[ch][a] < f[ch][b]; });
        for (size_t r = 0; r < order.size(); ++r) px[size_t(order[r]) * 4 + ch] = u8((r + 0.5f) / order.size());
    }
    keep = px;
    Image im{px.data(), n, n, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    Texture2D t = LoadTextureFromImage(im);
    GenTextureMipmaps(&t);
    SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
    SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
    return t;
}

RenderTexture2D colorRT(int w, int h, int format) {
    RenderTexture2D t{};
    t.id = rlLoadFramebuffer();
    rlEnableFramebuffer(t.id);
    t.texture.id = rlLoadTexture(nullptr, w, h, format, 1);
    t.texture.width = w; t.texture.height = h; t.texture.format = format; t.texture.mipmaps = 1;
    rlFramebufferAttach(t.id, t.texture.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
    if (!rlFramebufferComplete(t.id)) std::fprintf(stderr, "[vol] framebuffer %dx%d incomplete\n", w, h);
    rlDisableFramebuffer();
    SetTextureFilter(t.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(t.texture, TEXTURE_WRAP_CLAMP);
    return t;
}
void freeRT(RenderTexture2D& t) {
    if (!t.id) return;
    if (t.texture.id) rlUnloadTexture(t.texture.id);
    // rlUnloadFramebuffer frees whatever depth is attached: detach the shared scene depth first
    rlEnableFramebuffer(t.id);
    rlFramebufferAttach(t.id, 0, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
    rlDisableFramebuffer();
    rlUnloadFramebuffer(t.id);
    t = RenderTexture2D{};
}

void bindTex(int slot, unsigned id) { rlActiveTextureSlot(slot); rlEnableTexture(id); }
void unbindTex(int slot) { rlActiveTextureSlot(slot); rlDisableTexture(); }

// one full-screen pass with blending off (alpha carries transmittance)
template <class F>
void fullPass(RenderTexture2D& dst, Shader sh, F&& setup, std::initializer_list<int> slots) {
    BeginTextureMode(dst);
    rlDrawRenderBatchActive();
    rlDisableColorBlend();
    BeginShaderMode(sh);
    setup();
    rlActiveTextureSlot(0);
    DrawRectangle(0, 0, dst.texture.width, dst.texture.height, WHITE);
    rlDrawRenderBatchActive();
    for (int s : slots) unbindTex(s);
    rlActiveTextureSlot(0);
    EndShaderMode();
    rlEnableColorBlend();
    EndTextureMode();
}

}  // namespace

// ------------------------------------------------------------------ lifecycle
void Renderer::initVolumetrics() {
    volMarchSh_ = LoadShaderFromMemory(nullptr, volshaders::FS_VOL_MARCH);
    volResolveSh_ = LoadShaderFromMemory(nullptr, volshaders::FS_VOL_RESOLVE);
    volApplySh_ = LoadShaderFromMemory(nullptr, volshaders::FS_VOL_APPLY);
    for (Shader s : {volMarchSh_, volResolveSh_, volApplySh_})
        if (s.id == rlGetShaderIdDefault()) std::fprintf(stderr, "[vol] shader failed to compile\n");
    volShape_ = buildAtlas(128, 16, volShapeDim_, shapeVoxel);
    volDetail_ = buildAtlas(32, 8, volDetailDim_, detailVoxel);
    cloudMap_ = buildWeatherMap(512, cloudPx_);
    cloudPxN_ = 512;
    {   // the same source with the shadow-map main
        std::string src = volshaders::FS_VOL_MARCH;
        src = "#version 330\n#define SHADOW_PASS\n" + src.substr(src.find('\n') + 1);
        volShadowSh_ = LoadShaderFromMemory(nullptr, src.c_str());
        if (volShadowSh_.id == rlGetShaderIdDefault()) std::fprintf(stderr, "[vol] shadow shader failed to compile\n");
    }
}

void Renderer::unloadVolumetrics() {
    destroyVolTargets();
    for (Texture2D* t : {&volShape_, &volDetail_, &cloudMap_})
        if (t->id) { UnloadTexture(*t); *t = Texture2D{}; }
    for (Shader* s : {&volMarchSh_, &volResolveSh_, &volApplySh_, &volShadowSh_})
        if (s->id) { UnloadShader(*s); *s = Shader{}; }
}

void Renderer::createVolTargets() {
    if (!volEnabled()) return;
    int div = quality_ >= 2 ? 2 : 4;
    volW_ = std::max(8, rtW_ / div);
    volH_ = std::max(8, rtH_ / div);
    const int HDR = PIXELFORMAT_UNCOMPRESSED_R16G16B16A16;
    volCur_ = colorRT(volW_, volH_, HDR);
    for (auto& h : volHist_) h = colorRT(volW_, volH_, HDR);
    // the fogged scene shares the scene's depth so particles drawn after still depth-test
    sceneFx_ = colorRT(rtW_, rtH_, HDR);
    rlEnableFramebuffer(sceneFx_.id);
    rlFramebufferAttach(sceneFx_.id, scene_.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
    if (!rlFramebufferComplete(sceneFx_.id)) std::fprintf(stderr, "[vol] fx framebuffer incomplete\n");
    rlDisableFramebuffer();
    const int cs = quality_ >= 2 ? 512 : 256;
    csm_ = colorRT(cs, cs, HDR);
    csmTmp_ = colorRT(cs, cs, HDR);
    volHistValid_ = false;
}

void Renderer::destroyVolTargets() {
    freeRT(volCur_);
    for (auto& h : volHist_) freeRT(h);
    freeRT(sceneFx_);
    freeRT(csm_);
    freeRT(csmTmp_);
    volW_ = volH_ = 0;
    volHistValid_ = false;
}

// ------------------------------------------------------------------ per-frame parameters
void Renderer::updateCloudParams(const SceneState& s, float dt) {
    // The weather decides the sky: how much of it is cloud, what kind, how low, how dark,
    // and how much of it is raining. Coverage is the fraction of the sky covered (the
    // weather map is equalised): clear ~2 %, fair ~30 %, cloudy ~60 %, overcast / rain ~100 %.
    const WeatherFx& w = s.wx;
    CloudParams& c = cloud_;
    float overcast = smooth(0.6f, 1.f, w.cloud);
    c.coverage = std::pow(std::clamp((w.cloud - 0.03f) / 0.92f, 0.f, 1.f), 1.1f);
    // stratus decks when it's grey and settled, cumulus on fair days, towering cumulonimbus in storms
    c.type = std::clamp(0.5f - 0.42f * overcast * (1.f - w.storm) - 0.15f * w.fog + 0.5f * w.storm + 0.2f * w.hail, 0.f, 1.f);
    float ref = refMax_;   // eased (endless roads grow)
    c.base = ref + 1050.f - 420.f * overcast - 200.f * w.fog - 150.f * w.rain;
    c.top = c.base + 650.f + 1500.f * std::min(c.type, 0.6f) + 5200.f * w.storm;
    c.sigma = 0.05f * (1.f + 0.9f * w.dark + 0.5f * w.rain + 0.6f * w.storm);
    c.precip = std::clamp(w.rain + 0.7f * w.snow + 0.5f * w.hail, 0.f, 1.f);
    // rain falls from cells: drizzle from about a third of a grey sky, rain from nearly all of
    // it, a sunshower from a few cumulus with the sun out between them
    c.precipArea = std::clamp(w.rain * 1.25f * (0.35f + 0.65f * w.cloud) + w.snow + w.hail * 0.5f, 0.f, 1.f);
    // rain only falls from cloud; storm rain only from the cores of the cells
    c.precipArea = std::min(c.precipArea, c.coverage * (1.f - 0.45f * std::clamp(w.storm, 0.f, 1.f)));
    float speed = 5.f + w.wind * 16.f + w.gust * 6.f;
    c.wind = Vector2Add(c.wind, {std::sin(w.windDir) * speed * dt, std::cos(w.windDir) * speed * dt});
    if (std::fabs(c.wind.x) > 1e6f || std::fabs(c.wind.y) > 1e6f) c.wind = {0, 0};
    c.scale = 26000.f;
    // is a rain cell over the rider? (eased, so showers arrive and pass rather than switch)
    float target = 1.f;
    if (volEnabled() && course_ && c.precipArea > 0.001f) {
        auto p = course_->at(s.dist);
        float b = precipCellAt(p.pos.x, p.pos.z);
        target = smooth(1.f - c.precipArea - 0.06f, 1.f - c.precipArea + 0.12f, b);
    }
    c.here += (target - c.here) * (1.f - std::exp(-dt / 6.f));
}

float Renderer::precipCellAt(float x, float z) const {
    if (cloudPx_.empty()) return 1.f;
    const int n = cloudPxN_;
    float u = (x + cloud_.wind.x) / cloud_.scale * n - 0.5f, v = (z + cloud_.wind.y) / cloud_.scale * n - 0.5f;
    int i = int(std::floor(u)), k = int(std::floor(v));
    float fu = u - i, fv = v - k;
    auto at = [&](int a, int b) { a = ((a % n) + n) % n; b = ((b % n) + n) % n; return cloudPx_[(size_t(b) * n + a) * 4 + 2] / 255.f; };
    return (at(i, k) * (1 - fu) + at(i + 1, k) * fu) * (1 - fv) + (at(i, k + 1) * (1 - fu) + at(i + 1, k + 1) * fu) * fv;
}

// the cloud shadow map: sunlight through the cloud layer along the light, around the camera
void Renderer::renderCloudShadow() {
    if (!volEnabled() || !csm_.id) return;
    const int res = csm_.texture.width;
    const float size = quality_ >= 2 ? 16000.f : 12000.f, texel = size / res;
    Vector3 L = L_.lightDir;
    L.y = std::max(L.y, 0.05f);
    // centred on where the camera's own light ray meets the cloud base, snapped to texels
    float cx = cam_.position.x + L.x / L.y * (cloud_.base - cam_.position.y);
    float cz = cam_.position.z + L.z / L.y * (cloud_.base - cam_.position.y);
    float x0 = std::floor((cx - size * 0.5f) / texel) * texel, z0 = std::floor((cz - size * 0.5f) / texel) * texel;
    csmRect_ = {x0, z0, 1.f / size, cloud_.base};
    if (std::getenv("CORERAY_VOLDEBUG") && volFrame_ % 10 == 0) std::fprintf(stderr, "[csm] rect %.0f %.0f %.6f %.0f cam %.0f %.0f %.0f L %.2f %.2f %.2f\n", x0, z0, 1.f / size, cloud_.base, cam_.position.x, cam_.position.y, cam_.position.z, L.x, L.y, L.z);
    fullPass(csm_, volShadowSh_, [&] {
        Shader sh = volShadowSh_;
        bindTex(2, volShape_.id); setI(sh, "shapeTex", 2);
        bindTex(3, volDetail_.id); setI(sh, "detailTex", 3);
        bindTex(4, cloudMap_.id); setI(sh, "weatherTex", 4);
        setV2(sh, "res", {float(res), float(res)});
        setV3(sh, "camPos", cam_.position);
        setF(sh, "time", time_);
        setV4(sh, "shapeDim", volShapeDim_);
        setV4(sh, "detailDim", volDetailDim_);
        setV3(sh, "lightDirF", L_.lightDir);
        setF(sh, "coverage", cloud_.coverage);
        setF(sh, "cloudType", cloud_.type);
        setF(sh, "cloudBase", cloud_.base);
        setF(sh, "cloudTop", cloud_.top);
        setF(sh, "cloudSigma", cloud_.sigma);
        setF(sh, "weatherScale", cloud_.scale);
        setV2(sh, "windOffset", cloud_.wind);
        setF(sh, "planetR", 2.5e6f);
        setF(sh, "precipArea", cloud_.precipArea);
        setV4(sh, "csmRect", csmRect_);
    }, {2, 3, 4});
    // soften: cloud edges are wispy and the sun is a disc, so shadows have broad penumbrae
    auto blurPass = [&](RenderTexture2D& src, RenderTexture2D& dst, Vector2 dir) {
        BeginTextureMode(dst);
        BeginShaderMode(blurSh_);
        setV2(blurSh_, "dir", dir);
        DrawTexturePro(src.texture, {0, 0, float(res), -float(res)}, {0, 0, float(res), float(res)}, {0, 0}, 0, WHITE);
        EndShaderMode();
        EndTextureMode();
    };
    // (wider when the sun is low: the light crosses more of each cloud's ragged edge)
    const float t = (1.2f + 1.6f * (1.f - smooth(0.15f, 0.7f, L_.lightDir.y))) / res;
    blurPass(csm_, csmTmp_, {t, 0});
    blurPass(csmTmp_, csm_, {0, t});
    if (std::getenv("CORERAY_VOLDEBUG") && volFrame_ % 10 == 0) {
        Image im = LoadImageFromTexture(csm_.texture);
        ImageFormat(&im, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        const unsigned char* px = (const unsigned char*)im.data;
        int lo = 255, hi = 0; double sum = 0;
        for (int i = 0; i < res * res; ++i) { lo = std::min(lo, int(px[i * 4])); hi = std::max(hi, int(px[i * 4])); sum += px[i * 4]; }
        std::fprintf(stderr, "[csm] T min %d max %d mean %.0f\n", lo, hi, sum / (res * res));
        UnloadImage(im);
    }
}

// ------------------------------------------------------------------ the passes
void Renderer::renderVolumetrics(const SceneState& s) {
    if (!volEnabled() || !volCur_.id) return;
    const Course& C = *course_;
    const WeatherFx& w = s.wx;
    Vector3 fwd = Vector3Normalize(Vector3Subtract(cam_.target, cam_.position));
    Vector3 right = Vector3Normalize(Vector3CrossProduct(fwd, cam_.up));
    Vector3 up = Vector3CrossProduct(right, fwd);
    float tanHalf = std::tan(cam_.fovy * 0.5f * DEG2RAD);
    float aspect = float(rtW_) / float(std::max(1, rtH_));
    Matrix view = MatrixLookAt(cam_.position, cam_.target, cam_.up);
    Matrix proj = MatrixPerspective(cam_.fovy * DEG2RAD, aspect, 0.3, farClip());
    Matrix vp = MatrixMultiply(view, proj);
    Texture2D depthT{scene_.depth.id, rtW_, rtH_, 1, 19};
    const bool hi = quality_ >= 2;
    auto setCam = [&](Shader sh) {
        setV3(sh, "camPos", cam_.position);
        setV3(sh, "camFwd", fwd);
        setV3(sh, "camRight", right);
        setV3(sh, "camUp", up);
        setF(sh, "tanHalf", tanHalf);
        setF(sh, "aspect", aspect);
        setV2(sh, "clipNF", {0.3f, farClip()});
    };

    // ---- march
    fullPass(volCur_, volMarchSh_, [&] {
        Shader sh = volMarchSh_;
        bindTex(1, scene_.depth.id); setI(sh, "depthTex", 1);
        bindTex(2, volShape_.id); setI(sh, "shapeTex", 2);
        bindTex(3, volDetail_.id); setI(sh, "detailTex", 3);
        bindTex(4, cloudMap_.id); setI(sh, "weatherTex", 4);
        bindTex(5, shadow_[0].depth.id); setI(sh, "shadowMap0", 5);
        bindTex(6, shadow_[1].depth.id); setI(sh, "shadowMap1", 6);
        setV2(sh, "res", {float(volW_), float(volH_)});
        setCam(sh);
        setF(sh, "frameIdx", float(volFrame_ % 64));
        setF(sh, "time", time_);
        setV4(sh, "shapeDim", volShapeDim_);
        setV4(sh, "detailDim", volDetailDim_);
        setV3(sh, "sunDir", L_.sunDir);
        setV3(sh, "sunCloudCol", L_.sunRaw);
        setV3(sh, "lightDirF", L_.lightDir);
        setV3(sh, "lightColF", L_.lightColor);
        setV3(sh, "skyZenith", L_.zenith);
        setV3(sh, "skyHorizon", L_.horizon);
        setV3(sh, "groundColor", L_.ground);
        setF(sh, "flash", w.flash);
        setF(sh, "fogGrey", L_.fogGrey);
        setF(sh, "cloudOn", 1.f);
        setF(sh, "coverage", cloud_.coverage);
        setF(sh, "cloudType", cloud_.type);
        setF(sh, "cloudBase", cloud_.base);
        setF(sh, "cloudTop", cloud_.top);
        setF(sh, "cloudSigma", cloud_.sigma);
        setF(sh, "cloudDark", w.dark);
        setF(sh, "weatherScale", cloud_.scale);
        setV2(sh, "windOffset", cloud_.wind);
        setF(sh, "hazeDensity", L_.fog);
        setF(sh, "cloudSteps", hi ? 72.f : 44.f);
        setF(sh, "planetR", 2.5e6f);
        static float dbg = std::getenv("CORERAY_VOLDEBUG") ? 1.f : 0.f;
        setF(sh, "volDebug", dbg);
        if (dbg > 0 && volFrame_ % 10 == 0) std::fprintf(stderr, "[vol] cov %.2f type %.2f base %.0f top %.0f cam %.0f sigma %.3f sun %.2f %.2f %.2f\n", cloud_.coverage, cloud_.type, cloud_.base, cloud_.top, cam_.position.y, cloud_.sigma, L_.sunRaw.x, L_.sunRaw.y, L_.sunRaw.z);
        // fog: a thin haze always (so the sun casts shafts through the trees), thick in fog / mist,
        // a little valley mist at night, spray in rain
        float ground = C.at(s.dist).pos.y;
        setF(sh, "fogDens", 0.00022f + 0.018f * std::pow(w.fog, 2.2f) + L_.night * 0.0007f + w.rain * 0.0004f + w.snow * 0.0006f);
        setF(sh, "fogBase", ground - 4.f);
        setF(sh, "fogScaleH", 30.f + 40.f * (1.f - w.fog) + 30.f * w.rain);
        setF(sh, "fogRange", hi ? 420.f : 280.f);
        setF(sh, "fogSteps", hi ? 40.f : 26.f);
        setF(sh, "rainAmt", w.rain);
        setF(sh, "snowAmt", std::clamp(w.snow + w.hail * 0.4f, 0.f, 1.f));
        setF(sh, "precip", cloud_.precip);
        setV3(sh, "windVel", Vector3Scale({std::sin(w.windDir), 0, std::cos(w.windDir)}, 1.f + w.wind * 6.f));
        setM(sh, "lightVP0", lightVP_[0]);
        setM(sh, "lightVP1", lightVP_[1]);
        Vector3 f = fwd; f.y = 0; f = Vector3Normalize(f);
        setV3(sh, "cascadeCenter0", Vector3Add(cam_.position, Vector3Scale(f, cascadeRange_[0] * 0.55f)));
        setV3(sh, "cascadeCenter1", Vector3Add(cam_.position, Vector3Scale(f, cascadeRange_[1] * 0.55f)));
        setV2(sh, "cascadeRange", {cascadeRange_[0], cascadeRange_[1]});
        setLightUniforms(sh);
        bindTex(7, csm_.texture.id); setI(sh, "cloudShadowTex", 7);
        setV4(sh, "csmRect", csmRect_);
        setF(sh, "precipArea", cloud_.precipArea);
        setV3(sh, "boltPos", bolt_.cloud);
        setF(sh, "boltI", boltLight());
    }, {1, 2, 3, 4, 5, 6, 7});

    // ---- temporal resolve
    RenderTexture2D& hist = volHist_[volHistIdx_];
    RenderTexture2D& out = volHist_[1 - volHistIdx_];
    fullPass(out, volResolveSh_, [&] {
        Shader sh = volResolveSh_;
        bindTex(1, volCur_.texture.id); setI(sh, "curTex", 1);
        bindTex(2, hist.texture.id); setI(sh, "histTex", 2);
        bindTex(3, scene_.depth.id); setI(sh, "depthTex", 3);
        setV2(sh, "res", {float(volW_), float(volH_)});
        setCam(sh);
        setM(sh, "prevVP", prevVP_);
        setF(sh, "histValid", volHistValid_ ? 1.f : 0.f);
        setF(sh, "histWeight", 0.88f);
    }, {1, 2, 3});
    volHistIdx_ = 1 - volHistIdx_;
    volHistValid_ = true;
    prevVP_ = vp;
    ++volFrame_;

    // ---- upsample onto the scene
    fullPass(sceneFx_, volApplySh_, [&] {
        Shader sh = volApplySh_;
        bindTex(1, scene_.texture.id); setI(sh, "sceneTex", 1);
        bindTex(2, out.texture.id); setI(sh, "volTex", 2);
        bindTex(3, scene_.depth.id); setI(sh, "depthTex", 3);
        setV2(sh, "fullRes", {float(rtW_), float(rtH_)});
        setV2(sh, "lowRes", {float(volW_), float(volH_)});
        setV2(sh, "clipNF", {0.3f, farClip()});
    }, {1, 2, 3});
    (void)depthT;
}
