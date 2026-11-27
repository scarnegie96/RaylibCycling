#include <cstring>
#include <chrono>
// Frame orchestration: shaders, lighting (sun / moon / weather), camera, rider, shadows, post.
// World streaming lives in World.cpp, weather particles + wildlife in Fx.cpp.
#include "Renderer.h"
#include "world/WorldMap.h"
#include "Terrain.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <string>

#include "RenderUtil.h"
#pragma GCC diagnostic ignored "-Wunused-variable"   // (the shader header defines every source)
#include "Shaders.h"
#include "Profiler.h"

using namespace rutil;

const char* camModeName(CamMode m) {
    switch (m) {
        case CamMode::Chase: return "Chase";
        case CamMode::Cinematic: return "Cinematic";
        case CamMode::Side: return "Side";
        case CamMode::Rider: return "Rider";
        case CamMode::Drone: return "Drone";
        case CamMode::Close: return "Close chase";
        case CamMode::FirstPerson: return "Eyes";
        default: return "";
    }
}

// ------------------------------------------------------------------ mesh helpers
namespace {


Mesh genTorusX(float R, float r, int seg, int sides) {
    MeshBuilder b;
    for (int i = 0; i <= seg; ++i) {
        float a = 2 * PI_F * i / seg;
        Vector3 dir{0, std::cos(a), std::sin(a)};
        for (int j = 0; j <= sides; ++j) {
            float t = 2 * PI_F * j / sides;
            Vector3 nn{std::sin(t), dir.y * std::cos(t), dir.z * std::cos(t)};
            b.add({nn.x * r, dir.y * R + nn.y * r, dir.z * R + nn.z * r}, nn, {float(i) / seg, float(j) / sides});
        }
    }
    for (int i = 0; i < seg; ++i)
        for (int j = 0; j < sides; ++j) {
            unsigned short a = i * (sides + 1) + j, bb = (i + 1) * (sides + 1) + j;
            b.tri(a, bb, a + 1);
            b.tri(a + 1, bb, bb + 1);
        }
    return b.build();
}

// Surface of revolution about the X axis. `prof` is a closed (r, x) polygon traversed
// counter-clockwise in the (r, x) plane (outward normal = (dx, -dr)); flat-shaded per edge.
void latheX(MeshBuilder& b, const std::vector<Vector2>& prof, int seg) {
    size_t np = prof.size();
    for (size_t e = 0; e < np; ++e) {
        Vector2 p0 = prof[e], p1 = prof[(e + 1) % np];
        float dr = p1.x - p0.x, dx = p1.y - p0.y, len = std::sqrt(dr * dr + dx * dx);
        if (len < 1e-7f) continue;
        float nr = dx / len, nx = -dr / len;
        unsigned short base = (unsigned short)(b.v.size() / 3);
        for (int i = 0; i <= seg; ++i) {
            float a = 2 * PI_F * i / seg, c = std::cos(a), sn = std::sin(a);
            for (Vector2 q : {p0, p1}) b.add({q.y, q.x * c, q.x * sn}, {nx, nr * c, nr * sn}, {float(i) / seg, 0});
        }
        for (int i = 0; i < seg; ++i) {
            unsigned short a0 = base + i * 2, b0 = base + (i + 1) * 2;
            b.tri(a0, b0, a0 + 1);
            b.tri(a0 + 1, b0, b0 + 1);
        }
    }
}
// closed annulus (flat ring) from r0 to r1 between x0 and x1
void ringX(MeshBuilder& b, float r0, float r1, float x0, float x1, int seg) {
    latheX(b, {{r0, x0}, {r1, x0}, {r1, x1}, {r0, x1}}, seg);
}
// open tube from a to b (no caps), `sides` facets
void tube(MeshBuilder& b, Vector3 a, Vector3 c, float r, int sides = 6) {
    Vector3 d = Vector3Subtract(c, a);
    if (Vector3Length(d) < 1e-6f) return;
    d = Vector3Normalize(d);
    Vector3 ref = std::fabs(d.y) < 0.9f ? Vector3{0, 1, 0} : Vector3{1, 0, 0};
    Vector3 u = Vector3Normalize(Vector3CrossProduct(ref, d)), v = Vector3CrossProduct(d, u);  // u x v = d
    unsigned short base = (unsigned short)(b.v.size() / 3);
    for (int i = 0; i <= sides; ++i) {
        float t = 2 * PI_F * i / sides;
        Vector3 n = Vector3Add(Vector3Scale(u, std::cos(t)), Vector3Scale(v, std::sin(t)));
        b.add(Vector3Add(a, Vector3Scale(n, r)), n, {0, 0});
        b.add(Vector3Add(c, Vector3Scale(n, r)), n, {1, 0});
    }
    for (int i = 0; i < sides; ++i) {
        unsigned short a0 = base + i * 2, a1 = base + (i + 1) * 2;
        b.tri(a0, a1, a0 + 1);
        b.tri(a1, a1 + 1, a0 + 1);
    }
}

// Hub + laced spokes in wheel-local space (axle along X). Front: 20 radial;
// rear: 24 two-cross (12 per side, alternately leading / trailing).
Mesh genSpokedHub(bool rear) {
    MeshBuilder b;
    // hub shell with flanges
    float fl = rear ? 0.027f : 0.024f;
    latheX(b, {{0.008f, -0.050f}, {0.012f, -0.050f}, {0.012f, -0.036f}, {0.024f, -0.034f}, {0.024f, -0.030f}, {0.016f, -0.028f},
               {0.016f, 0.028f}, {0.024f, 0.030f}, {0.024f, 0.034f}, {0.012f, 0.036f}, {0.012f, 0.050f}, {0.008f, 0.050f}}, 16);
    const float rimR = 0.268f;  // spoke nipples sit in the rim bed
    int n = rear ? 24 : 20;
    for (int k = 0; k < n; ++k) {
        float side = (k % 2) ? 1.f : -1.f;
        float aRim = 2 * PI_F * k / n;
        float aHub = aRim;
        if (rear) aHub += ((k / 2) % 2 ? 1.f : -1.f) * 2 * PI_F / 12 * 2 * 0.5f;  // 2-cross
        Vector3 h{side * fl, 0.022f * std::cos(aHub), 0.022f * std::sin(aHub)};
        Vector3 r{side * 0.003f, rimR * std::cos(aRim), rimR * std::sin(aRim)};
        tube(b, h, r, 0.0011f, 4);
    }
    return b.build();
}

Shader loadShader(const char* vs, const std::string& fs, const char* label) {
    Shader s = LoadShaderFromMemory(vs, fs.c_str());
    if (s.id == rlGetShaderIdDefault()) std::fprintf(stderr, "[render] shader '%s' failed to compile\n", label);
    return s;
}

RenderTexture2D loadRT(int w, int h, int format, bool depth) {
    RenderTexture2D t{};
    t.id = rlLoadFramebuffer();
    rlEnableFramebuffer(t.id);
    t.texture.id = rlLoadTexture(nullptr, w, h, format, 1);
    t.texture.width = w; t.texture.height = h; t.texture.format = format; t.texture.mipmaps = 1;
    rlFramebufferAttach(t.id, t.texture.id, RL_ATTACHMENT_COLOR_CHANNEL0, RL_ATTACHMENT_TEXTURE2D, 0);
    if (depth) {
        t.depth.id = rlLoadTextureDepth(w, h, true);
        t.depth.width = w; t.depth.height = h;
        rlFramebufferAttach(t.id, t.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_RENDERBUFFER, 0);
    }
    if (!rlFramebufferComplete(t.id)) std::fprintf(stderr, "[render] framebuffer %dx%d incomplete\n", w, h);
    rlDisableFramebuffer();
    SetTextureFilter(t.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureWrap(t.texture, TEXTURE_WRAP_CLAMP);
    return t;
}

RenderTexture2D loadShadowRT(int res) {
    RenderTexture2D t{};
    t.id = rlLoadFramebuffer();
    t.texture.width = res; t.texture.height = res;
    rlEnableFramebuffer(t.id);
    t.depth.id = rlLoadTextureDepth(res, res, false);
    t.depth.width = res; t.depth.height = res; t.depth.format = 19; t.depth.mipmaps = 1;
    rlFramebufferAttach(t.id, t.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
    if (!rlFramebufferComplete(t.id)) std::fprintf(stderr, "[render] shadow framebuffer incomplete\n");
    rlDisableFramebuffer();
    return t;
}

void unloadRT(RenderTexture2D& t) {
    if (t.id == 0) return;
    if (t.texture.id) rlUnloadTexture(t.texture.id);
    // rlUnloadFramebuffer frees the attached depth (texture or renderbuffer) itself; deleting it
    // here too would free it twice, and a renderbuffer id passed to the texture delete frees
    // some unrelated texture that happens to share the number
    rlUnloadFramebuffer(t.id);
    t = RenderTexture2D{};
}

void blit(const RenderTexture2D& src, const RenderTexture2D& dst) {
    DrawTexturePro(src.texture, {0, 0, float(src.texture.width), -float(src.texture.height)},
                   {0, 0, float(dst.texture.width), float(dst.texture.height)}, {0, 0}, 0, WHITE);
}

Vector3 lerp3(Vector3 a, Vector3 b, float t) { return Vector3Lerp(a, b, std::clamp(t, 0.f, 1.f)); }

}  // namespace

// ------------------------------------------------------------------ lifecycle
Renderer::Renderer() = default;
Renderer::~Renderer() { stopWorkers(); }

bool Renderer::init(int quality) {
    quality_ = quality;
    if (const char* dc = std::getenv("CORERAY_DEVCAM"))
        std::sscanf(dc, "%f,%f,%f,%f,%f,%f", &devCam_[0], &devCam_[1], &devCam_[2], &devCam_[3], &devCam_[4], &devCam_[5]);
    shadowRes_ = quality >= 2 ? 4096 : quality == 1 ? 2048 : 1024;
    std::string head = shaders::FS_HEADER, common = shaders::FS_COMMON;
    terrainSh_ = loadShader(shaders::VS_TERRAIN, head + common + shaders::FS_TERRAIN, "terrain");
    roadSh_ = loadShader(shaders::VS_STATIC, head + common + shaders::FS_ROAD, "road");
    objectSh_ = loadShader(shaders::VS_STATIC, head + common + shaders::FS_OBJECT, "object");
    waterSh_ = loadShader(shaders::VS_STATIC, head + common + shaders::FS_WATER, "water");
    skySh_ = loadShader(shaders::VS_STATIC, head + common + shaders::FS_SKY, "sky");
    foliageSh_ = loadShader(shaders::VS_INSTANCED, head + common + shaders::FS_FOLIAGE, "foliage");
    foliageSh_.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocationAttrib(foliageSh_, "instanceTransform");
    depthSh_ = loadShader(shaders::VS_DEPTH, shaders::FS_DEPTH, "depth");
    depthInstSh_ = loadShader(shaders::VS_DEPTH_INSTANCED, shaders::FS_DEPTH_ALPHA, "depth-inst");
    depthInstSh_.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocationAttrib(depthInstSh_, "instanceTransform");
    std::string ph = shaders::FS_POST_HEADER;
    brightSh_ = loadShader(nullptr, ph + shaders::FS_BRIGHT, "bright");
    blurSh_ = loadShader(nullptr, ph + shaders::FS_BLUR, "blur");
    compositeSh_ = loadShader(nullptr, ph + shaders::FS_HASH + shaders::FS_COMPOSITE, "composite");
    fxaaSh_ = loadShader(nullptr, ph + shaders::FS_FXAA, "fxaa");
    refrCopySh_ = loadShader(nullptr, ph + shaders::FS_REFR_COPY, "refraction-copy");

    grassSh_ = loadShader(shaders::VS_GRASS, head + common + shaders::FS_GRASS, "grass");
    grassSh_.locs[SHADER_LOC_MATRIX_MODEL] = GetShaderLocationAttrib(grassSh_, "instanceTransform");
    animalSh_ = loadShader(shaders::VS_ANIMAL, head + common + shaders::FS_ANIMAL, "animal");
    volumeSh_ = loadShader(shaders::VS_STATIC, head + common + shaders::FS_VOLUME, "volume");
    particleSh_ = loadShader(nullptr, shaders::FS_PARTICLE, "particle");
    kitSh_ = loadShader(shaders::VS_STATIC, head + common + shaders::FS_KIT, "kit");

    tex_ = tex::generate(quality);
    volDisabled_ = std::getenv("CORERAY_NOVOL") != nullptr;
    initVolumetrics();
    terrainMat_ = makeMat(terrainSh_); roadMat_ = makeMat(roadSh_); objectMat_ = makeMat(objectSh_);
    waterMat_ = makeMat(waterSh_); skyMat_ = makeMat(skySh_); foliageMat_ = makeMat(foliageSh_);
    depthMat_ = makeMat(depthSh_); depthInstMat_ = makeMat(depthInstSh_);
    grassMat_ = makeMat(grassSh_); animalMat_ = makeMat(animalSh_); volumeMat_ = makeMat(volumeSh_); kitMat_ = makeMat(kitSh_);
    terrainMat_.maps[MATERIAL_MAP_ALBEDO].texture = tex_.grass;
    grassMat_.maps[MATERIAL_MAP_ALBEDO].texture = tex_.grass;
    roadMat_.maps[MATERIAL_MAP_ALBEDO].texture = tex_.asphalt;
    roadMat_.maps[MATERIAL_MAP_METALNESS].texture = tex_.gravel;
    roadMat_.maps[MATERIAL_MAP_NORMAL].texture = tex_.grass;
    terrainMat_.maps[MATERIAL_MAP_METALNESS].texture = tex_.rock;
    terrainMat_.maps[MATERIAL_MAP_NORMAL].texture = tex_.gravel;
    terrainSh_.locs[SHADER_LOC_MAP_ROUGHNESS] = GetShaderLocation(terrainSh_, "texSand");
    terrainSh_.locs[SHADER_LOC_MAP_OCCLUSION] = GetShaderLocation(terrainSh_, "texSoil");
    terrainMat_.shader = terrainSh_;
    terrainMat_.maps[MATERIAL_MAP_ROUGHNESS].texture = tex_.sand;
    terrainMat_.maps[MATERIAL_MAP_OCCLUSION].texture = tex_.soil;
    terrainSh_.locs[SHADER_LOC_MAP_EMISSION] = GetShaderLocation(terrainSh_, "texGranite");
    terrainSh_.locs[SHADER_LOC_MAP_HEIGHT] = GetShaderLocation(terrainSh_, "texScree");
    terrainMat_.shader = terrainSh_;
    terrainMat_.maps[MATERIAL_MAP_EMISSION].texture = tex_.granite;
    terrainMat_.maps[MATERIAL_MAP_HEIGHT].texture = tex_.scree;
    buildMeshes();
    for (int i = 0; i < 2; ++i) shadow_[i] = loadShadowRT(shadowRes_);
    startWorkers();
    ready_ = true;
    return true;
}

void Renderer::buildMeshes() {
    cyl_ = GenMeshCylinder(1, 1, 12);
    sphere_ = GenMeshSphere(1, 14, 18);
    cube_ = GenMeshCube(1, 1, 1);
    cone_ = GenMeshCone(1, 1, 12);
    // 700x25c tyre: 25 mm section on a 622 mm bead seat (outer radius WHEEL_FREE_R)
    wheelTire_ = genTorusX(WHEEL_FREE_R - 0.0125f, 0.0125f, 72, 10);
    {   // 45 mm deep carbon rim, 21 mm wide at the bead, tapering to the spoke bed
        MeshBuilder b;
        latheX(b, {{0.262f, -0.0060f}, {0.290f, -0.0108f}, {0.3105f, -0.0105f}, {0.3105f, 0.0105f}, {0.290f, 0.0108f}, {0.262f, 0.0060f}}, 72);
        wheelRim_ = b.build();
    }
    spokesFront_ = genSpokedHub(false);
    spokesRear_ = genSpokedHub(true);
    {   // 160 mm disc rotor: braking ring + 6 arms to the centre-lock carrier
        MeshBuilder b;
        ringX(b, 0.064f, 0.080f, -0.0009f, 0.0009f, 48);
        ringX(b, 0.018f, 0.030f, -0.0015f, 0.0015f, 16);
        for (int k = 0; k < 6; ++k) {
            float a0 = 2 * PI_F * k / 6, a1 = a0 + 0.45f;
            tube(b, {0, 0.028f * std::cos(a0), 0.028f * std::sin(a0)}, {0, 0.066f * std::cos(a1), 0.066f * std::sin(a1)}, 0.0035f, 4);
        }
        rotor_ = b.build();
    }
    {   // 52/36 chainset: rings (pitch radius 12.7 mm x T / 2pi), teeth, 4-arm spider
        MeshBuilder b;
        auto ring = [&](int teeth, float x0, float x1) {
            float pr = 0.0127f * teeth / (2 * PI_F);
            ringX(b, pr - 0.014f, pr + 0.002f, x0, x1, teeth * 2);
            for (int t = 0; t < teeth; ++t) {  // simple tooth tips
                float a = 2 * PI_F * t / teeth, ca = std::cos(a), sa = std::sin(a);
                tube(b, {(x0 + x1) * 0.5f, (pr + 0.001f) * ca, (pr + 0.001f) * sa}, {(x0 + x1) * 0.5f, (pr + 0.0045f) * ca, (pr + 0.0045f) * sa}, 0.0016f, 3);
            }
        };
        ring(52, -0.0040f, -0.0020f);
        ring(36, 0.0035f, 0.0055f);
        for (int k = 0; k < 4; ++k) {
            float a = 2 * PI_F * k / 4 + 0.4f;
            tube(b, {-0.002f, 0.012f * std::cos(a), 0.012f * std::sin(a)}, {-0.002f, 0.092f * std::cos(a), 0.092f * std::sin(a)}, 0.0055f, 5);
        }
        ringX(b, 0.0f, 0.016f, -0.006f, 0.006f, 16);
        chainring_ = b.build();
    }
    {   // 11-speed 11-30 cassette, 3.8 mm pitch (smallest cog outboard = -x)
        MeshBuilder b;
        const int cogs[11] = {30, 27, 24, 21, 19, 17, 15, 14, 13, 12, 11};
        for (int i = 0; i < 11; ++i) {
            float pr = 0.0127f * cogs[i] / (2 * PI_F), x = -0.0038f * i;
            ringX(b, 0.017f, pr + 0.003f, x - 0.0009f, x + 0.0009f, std::max(24, cogs[i] * 2));
        }
        cassette_ = b.build();
    }
    skyDome_ = GenMeshSphere(1, 24, 32);
    water_ = GenMeshPlane(30000, 30000, 8, 8);
    // a new set of trees, bushes and rocks every launch (ez-tree port, fresh seeds)
    trees::setSeed(std::getenv("CORERAY_TREESEED") ? uint32_t(std::atoi(std::getenv("CORERAY_TREESEED"))) : uint32_t(std::time(nullptr)));
    for (int t = 0; t < trees::TYPES; ++t) trees_[t] = trees::build(t, quality_);
    for (int b = 0; b < trees::BUSHES; ++b) bushes_[b] = trees::buildBush(b);
    for (int r = 0; r < trees::ROCKS; ++r) rocks_[r] = trees::buildRock(r);
    leafAtlas_ = trees::leafAtlas(quality_);
    foliageMat_.maps[MATERIAL_MAP_ALBEDO].texture = leafAtlas_;
    depthInstMat_.maps[MATERIAL_MAP_ALBEDO].texture = leafAtlas_;
    if (!std::getenv("CORERAY_NOIMP")) bakeImpostors();
    // grass tufts, cartoon style: a rounded clump of wide, pointed, curving blades (unit
    // height), the middle ones tallest. Normals point out from a centre below the clump so it
    // shades as one soft volume. Vertex colour = per-blade tint; uv.y = 0 root .. 1 tip;
    // uv.x > 1.5 marks flower heads.
    for (int variant = 0; variant < 2; ++variant) {
        MeshBuilder b;
        uint32_t seed = 101 + variant;
        int blades = variant == 0 ? 18 : 12;
        for (int k = 0; k < blades; ++k) {
            float a = 2 * PI_F * (k + hash2(k, 1, seed) * 0.9f) / blades;
            float inner = hash2(k, 2, seed);                       // 0 centre .. 1 rim
            float r0 = 0.02f + 0.16f * inner;
            float lean = 0.12f + 0.38f * inner + 0.1f * hash2(k, 3, seed);
            float hgt = (1.0f - 0.45f * inner) * (0.8f + 0.2f * hash2(k, 4, seed));
            float w = 0.035f + 0.025f * hash2(k, 5, seed);
            float tint = 0.44f + 0.12f * hash2(k, 6, seed);
            float twist = (hash2(k, 7, seed) - 0.5f) * 0.6f;
            Vector3 dir{std::cos(a), 0, std::sin(a)};
            const int segs = 3;
            unsigned short prevL = 0, prevR = 0;
            for (int sgm = 0; sgm <= segs; ++sgm) {
                float t = float(sgm) / segs;
                float y = hgt * (t - 0.12f * t * t * inner);       // the rim blades droop a little
                float out = r0 + lean * t * t;
                Vector3 c{dir.x * out, y, dir.z * out};
                float ang = a + twist * t;
                Vector3 side{-std::sin(ang), 0, std::cos(ang)};
                float ww = w * (1.f - std::pow(t, 1.6f));        // pointed tip
                Vector3 nn = Vector3Normalize({c.x * 1.6f, c.y + 0.35f, c.z * 1.6f});   // clump-volume normal
                Color col{(unsigned char)(tint * 255), (unsigned char)(tint * 255), (unsigned char)(tint * 255), 255};
                unsigned short L = b.add(Vector3Subtract(c, Vector3Scale(side, ww)), nn, {0, t}, col);
                unsigned short R = b.add(Vector3Add(c, Vector3Scale(side, ww)), nn, {1, t}, col);
                if (sgm > 0) { b.tri(prevL, L, prevR); b.tri(prevR, L, R); }
                prevL = L; prevR = R;
            }
        }
        if (variant == 1) {
            // three flower heads on thin stems: small star-shaped discs
            for (int f = 0; f < 3; ++f) {
                float a = f * 2.1f + 0.4f, out = 0.08f + 0.06f * f;
                float y = 0.75f + 0.25f * hash2(f, 9, seed);
                Vector3 c{std::cos(a) * out, y, std::sin(a) * out};
                float kind = 2.f + float((f + int(seed)) % 4);
                Color col{110, 140, 60, 255};
                unsigned short ctr = b.add(c, {0, 1, 0}, {kind, 1}, col);
                const int petals = 10;
                std::vector<unsigned short> ring;
                for (int pp = 0; pp <= petals; ++pp) {
                    float pa = 2 * PI_F * pp / petals;
                    float pr = (pp % 2 ? 0.035f : 0.05f);
                    ring.push_back(b.add({c.x + std::cos(pa) * pr, c.y + 0.005f, c.z + std::sin(pa) * pr}, {0, 1, 0}, {kind, 1}, col));
                }
                for (int pp = 0; pp < petals; ++pp) b.tri(ctr, ring[pp + 1], ring[pp]);
                // stem
                Vector3 s0{c.x * 0.3f, 0, c.z * 0.3f};
                Vector3 sd = Vector3Normalize(Vector3Subtract(c, s0));
                Vector3 ss = Vector3Normalize(Vector3CrossProduct(sd, {0, 0, 1}));
                unsigned short a0 = b.add(Vector3Subtract(s0, Vector3Scale(ss, 0.004f)), {0, 0, 1}, {0, 0}, col);
                unsigned short a1 = b.add(Vector3Add(s0, Vector3Scale(ss, 0.004f)), {0, 0, 1}, {1, 0}, col);
                unsigned short a2 = b.add(Vector3Subtract(c, Vector3Scale(ss, 0.003f)), {0, 0, 1}, {0, 1}, col);
                unsigned short a3 = b.add(Vector3Add(c, Vector3Scale(ss, 0.003f)), {0, 0, 1}, {1, 1}, col);
                b.tri(a0, a2, a1); b.tri(a1, a2, a3);
            }
        }
        (variant == 0 ? tuft_ : flowerTuft_) = b.build(true);
    }
    // light shaft: an open cone, apex at the lamp, uv.y = 0 at the lamp .. 1 at the ground
    {
        MeshBuilder b;
        const int sl = 24;
        for (int i = 0; i <= sl; ++i) {
            float a = 2 * PI_F * i / sl;
            Vector3 d{std::cos(a), 0, std::sin(a)};
            Vector3 nn = Vector3Normalize({d.x, 0.55f, d.z});
            b.add({d.x * 0.08f, 0, d.z * 0.08f}, nn, {float(i) / sl, 0});
            b.add({d.x, -1, d.z}, nn, {float(i) / sl, 1});
        }
        for (int i = 0; i < sl; ++i) {
            unsigned short a = i * 2, c = i * 2 + 1, a2 = (i + 1) * 2, c2 = (i + 1) * 2 + 1;
            b.tri(a, c, a2); b.tri(a2, c, c2);
        }
        shaftCone_ = b.build();
    }
    pyramid_ = GenMeshCone(1, 1, 4);
    rig_.build();
    ghostRig_.build();
    if (!greyReady_ && !greyFuture_.valid()) {
        greyFuture_ = std::async(std::launch::async, [] {
            body::Params p;
            p.cell = 0.009f;   // ~35k verts: plenty at riding distance
            return body::build(p);
        });
    }
    buildAnimalMeshes();
}

void Renderer::shutdown() {
    if (!ready_) return;
    stopWorkers();
    destroyTargets();
    for (auto& r : shadow_) unloadRT(r);
    if (tunnelTex_) { rlUnloadTexture(tunnelTex_); tunnelTex_ = 0; }
    if (sunJob_.valid()) sunJob_.wait();
    if (sunTex_) { rlUnloadTexture(sunTex_); sunTex_ = 0; }
    for (auto& [k, c] : chunks_) { if (c.mesh.vaoId) UnloadMesh(c.mesh); if (c.water.vaoId) UnloadMesh(c.water); if (c.roads.vaoId) UnloadMesh(c.roads); }
    chunks_.clear();
    for (auto& r : road_) if (r.mesh.vaoId) UnloadMesh(r.mesh);
    road_.clear();
    clearStructures();
    camInit_ = false;
    for (Mesh* m : {&water_, &skyDome_, &cyl_, &sphere_, &cube_, &cone_, &wheelTire_, &wheelRim_, &spokesFront_, &spokesRear_, &rotor_, &chainring_, &cassette_,
                    &tuft_, &flowerTuft_, &shaftCone_, &pyramid_})
        if (m->vaoId) { UnloadMesh(*m); *m = Mesh{}; }
    for (auto& am : animals_)
        for (Mesh* m : {&am.body, &am.head, &am.upper, &am.lower, &am.tail, &am.ear})
            if (m->vaoId) { UnloadMesh(*m); *m = Mesh{}; }
    grass_.clear();
    rig_.unload();
    ghostRig_.unload();
    if (greyFuture_.valid()) greyFuture_.wait();
    if (greyReady_) { UnloadMesh(greyMesh_); greyMesh_ = Mesh{}; greyReady_ = false; }
    tex::unload(tex_);
    unloadVolumetrics();
    unloadImpostors();
    for (auto& t : trees_) { if (t.hi.vaoId) UnloadMesh(t.hi); if (t.lo.vaoId) UnloadMesh(t.lo); if (t.shadow.vaoId) UnloadMesh(t.shadow); t = trees::Built{}; }
    for (auto& t : bushes_) { if (t.hi.vaoId) UnloadMesh(t.hi); if (t.lo.vaoId) UnloadMesh(t.lo); t = trees::Built{}; }
    for (auto& r : rocks_) if (r.vaoId) { UnloadMesh(r); r = Mesh{}; }
    if (leafAtlas_.id) { UnloadTexture(leafAtlas_); leafAtlas_ = Texture2D{}; }
    for (Shader s : {terrainSh_, roadSh_, objectSh_, foliageSh_, waterSh_, skySh_, depthSh_, depthInstSh_, brightSh_, blurSh_, compositeSh_, fxaaSh_,
                     grassSh_, animalSh_, volumeSh_, particleSh_, kitSh_, refrCopySh_})
        UnloadShader(s);
    ready_ = false;
}

void Renderer::setQuality(int q) {
    if (q == quality_) return;
    quality_ = q;
    int res = q >= 2 ? 4096 : q == 1 ? 2048 : 1024;
    if (res != shadowRes_) {
        for (auto& r : shadow_) unloadRT(r);
        shadowRes_ = res;
        for (auto& r : shadow_) r = loadShadowRT(shadowRes_);
    }
    for (auto& [k, c] : chunks_) c.lod = -1;  // rebuild at the new resolution (old meshes stay until replaced)
    if (rtW_) { destroyTargets(); createTargets(); }  // volumetric resolution follows quality
    if (course_) course_->field().setErosion(true, q);
    grass_.clear();
}

void Renderer::setCourse(std::shared_ptr<const Course> c) {
    refInit_ = false;
    {
        std::lock_guard<std::mutex> lk(qm_);
        jobs_.clear();
        inFlight_.clear();
        eroding_.clear();
    }
    for (auto& [k, ch] : chunks_) { if (ch.mesh.vaoId) UnloadMesh(ch.mesh); if (ch.water.vaoId) UnloadMesh(ch.water); if (ch.roads.vaoId) UnloadMesh(ch.roads); }
    chunks_.clear();
    for (auto& r : road_) if (r.mesh.vaoId) UnloadMesh(r.mesh);
    road_.clear();
    clearStructures();
    herds_.clear();
    birds_.clear();
    grass_.clear();
    splashes_.clear();
    lit_.clear();
    lanterns_.clear();
    course_ = std::move(c);
    courseVersion_ = course_ ? course_->version() : 0;
    if (course_) course_->field().setErosion(true, quality_);
    ++worldId_;
    camInit_ = false;
}

void Renderer::updateCourse(std::shared_ptr<const Course> c) {
    course_ = std::move(c);
    course_->field().setErosion(true, quality_);
    courseVersion_ = course_->version();
    {
        std::lock_guard<std::mutex> lk(qm_);
        eroding_.clear();
    }
    for (auto& r : road_) r.version = -1;  // rebuilt next frame
    for (auto& [k, g] : grass_) g.version = -1;
    lit_.clear();
}

void Renderer::extendCourse(std::shared_ptr<const Course> c, float x0, float z0, float x1, float z1) {
    course_ = std::move(c);
    course_->field().setErosion(true, quality_);
    int oldFull = courseVersion_, oldVer = courseVersion_ & 0xFFFF;
    courseVersion_ = course_->version();
    const int ver = courseVersion_ & 0xFFFF;
    const float CS = terrain::TILE;
    for (auto& [k, ch] : chunks_) {
        if (ch.version != oldVer) continue;
        float cx0 = ch.cx * CS, cz0 = ch.cz * CS;
        bool touched = cx0 + CS > x0 && cx0 < x1 && cz0 + CS > z0 && cz0 < z1;
        if (!touched) ch.version = ver;  // the land here hasn't changed
    }
    {
        std::lock_guard<std::mutex> lk(qm_);
        eroding_.clear();
    }
    for (auto& r : road_) r.version = -1;  // the road pieces are cheap: rebuild them
    for (auto& [k, g] : grass_)
        if (g.version == oldFull) g.version = courseVersion_;
    bridgesFor_ = -1;
    lit_.clear();
}

// ------------------------------------------------------------------ lighting
void Renderer::computeLighting(const SceneState& s) {
    const SkyState& k = s.sky;
    const WeatherFx& w = s.wx;
    // palette keys along CoreKick's tod axis (0 day .. 1 night)
    struct P { float t; Vector3 sun, zen, hor, gnd; float amb, exp, fog; };
    static const P keys[] = {
        {0.00f, {3.1f, 2.95f, 2.7f}, {0.14f, 0.30f, 0.68f}, {0.60f, 0.72f, 0.88f}, {0.24f, 0.22f, 0.17f}, 0.55f, 0.78f, 1.f / 5200.f},
        {0.42f, {3.3f, 2.55f, 1.8f}, {0.15f, 0.27f, 0.58f}, {0.86f, 0.74f, 0.60f}, {0.25f, 0.20f, 0.14f}, 0.50f, 0.90f, 1.f / 4300.f},
        {0.66f, {3.2f, 1.55f, 0.65f}, {0.13f, 0.17f, 0.40f}, {0.98f, 0.55f, 0.32f}, {0.22f, 0.15f, 0.10f}, 0.45f, 1.00f, 1.f / 3600.f},
        {0.83f, {0.5f, 0.28f, 0.25f}, {0.05f, 0.06f, 0.16f}, {0.30f, 0.20f, 0.30f}, {0.08f, 0.06f, 0.06f}, 0.55f, 1.45f, 1.f / 3000.f},
        {1.00f, {0.0f, 0.0f, 0.0f}, {0.008f, 0.012f, 0.030f}, {0.022f, 0.028f, 0.050f}, {0.012f, 0.013f, 0.018f}, 0.5f, 1.9f, 1.f / 2600.f},
    };
    float t = std::clamp(k.tod, 0.f, 1.f);
    int i = 0;
    while (i < 3 && t > keys[i + 1].t) ++i;
    float f = (t - keys[i].t) / (keys[i + 1].t - keys[i].t);
    const P &a = keys[i], &b = keys[i + 1];
    auto mix3 = [&](Vector3 x, Vector3 y) { return lerp3(x, y, f); };
    Vector3 sunCol = mix3(a.sun, b.sun);
    L_.zenith = mix3(a.zen, b.zen);
    L_.horizon = mix3(a.hor, b.hor);
    L_.ground = mix3(a.gnd, b.gnd);
    L_.ambient = a.amb + (b.amb - a.amb) * f;
    L_.exposure = a.exp + (b.exp - a.exp) * f;
    L_.fog = a.fog + (b.fog - a.fog) * f;
    L_.night = smooth(0.7f, 0.95f, t);
    L_.sunDir = k.sunDir;
    L_.moonDir = k.moonDir;
    L_.moonIllum = k.moonIllum;
    L_.moonWaxing = k.moonWaxing ? 1.f : 0.f;

    // weather: clouds swallow the sun, rain darkens and greys the sky, fog thickens.
    // With volumetric clouds the clouds themselves shadow the ground (moving patches), so the
    // blanket dimming only covers the thickening of a full overcast.
    const bool vol = volEnabled();
    L_.sunRaw = Vector3Scale(sunCol, smooth(-6.f, 1.f, k.sunElevation));
    float overcast = std::clamp(w.cloud * 0.6f + w.dark * 0.7f, 0.f, 1.f);
    // (with volumetric clouds the cloud shadow map does the dimming, cloud by cloud)
    sunCol = Vector3Scale(sunCol, vol ? 1.f - 0.1f * w.dark : 1.f - 0.82f * smooth(0.45f, 1.f, w.cloud) - 0.1f * w.dark);
    auto grey = [&](Vector3 c, float amt, float dim) {
        float l = (c.x + c.y + c.z) / 3.f;
        return Vector3Scale(lerp3(c, {l * 0.95f, l * 0.98f, l * 1.04f}, amt), dim);
    };
    L_.zenith = grey(L_.zenith, overcast * 0.85f, 1.f - 0.5f * w.dark);
    L_.horizon = grey(L_.horizon, overcast * 0.75f, 1.f - 0.4f * w.dark);
    L_.ambient *= 1.f + 0.7f * overcast;
    // the volumetric pass carries the near fog itself; the analytic fog keeps the distance
    L_.fog *= vol ? 1.f + w.fog * 3.f + w.rain * 0.9f + w.snow * 1.8f : 1.f + w.fog * 7.f + w.rain * 1.5f + w.snow * 2.5f;
    L_.fog /= viewScale_;   // further views see further
    L_.fogGrey = std::clamp(w.fog * 0.6f + overcast * 0.4f, 0.f, 0.9f);
    L_.clouds = std::clamp(0.2f + w.cloud * 0.8f, 0.f, 1.f);
    L_.stars = L_.night * (1.f - smooth(0.4f, 0.85f, w.cloud)) * (1.f - w.fog);
    L_.sunVisible = (1.f - smooth(0.55f, 0.95f, w.cloud)) * smooth(-2.f, 2.f, k.sunElevation);

    // the directional light: sun by day, moon by night
    Vector3 sd = k.sunDir;
    if (sd.y < 0.06f) { sd.y = 0.06f; sd = Vector3Normalize(sd); }
    Vector3 md = k.moonElevation > 3 ? k.moonDir : Vector3Normalize({0.3f, 0.8f, 0.2f});
    if (md.y < 0.08f) { md.y = 0.08f; md = Vector3Normalize(md); }
    float moonAmt = smooth(-1.f, -7.f, k.sunElevation);
    float moonLight = (k.moonElevation > 0 ? 0.25f + 0.75f * k.moonIllum : 0.15f) * (1.f - 0.8f * w.cloud);
    Vector3 moonCol = Vector3Scale({0.10f, 0.13f, 0.22f}, moonLight);
    L_.lightDir = Vector3Normalize(lerp3(sd, md, moonAmt));
    L_.lightColor = lerp3(sunCol, moonCol, moonAmt);
}

// ------------------------------------------------------------------ targets
void Renderer::createTargets() {
    rtW_ = targetW();
    rtH_ = targetH();
    const int HDR = PIXELFORMAT_UNCOMPRESSED_R16G16B16A16;
    // the scene's depth is a texture so the water pass can read it (via the refraction copy)
    scene_ = loadRT(rtW_, rtH_, HDR, false);
    rlEnableFramebuffer(scene_.id);
    scene_.depth.id = rlLoadTextureDepth(rtW_, rtH_, false);
    scene_.depth.width = rtW_; scene_.depth.height = rtH_;
    rlFramebufferAttach(scene_.id, scene_.depth.id, RL_ATTACHMENT_DEPTH, RL_ATTACHMENT_TEXTURE2D, 0);
    if (!rlFramebufferComplete(scene_.id)) std::fprintf(stderr, "[render] scene framebuffer incomplete\n");
    rlDisableFramebuffer();
    bright_ = loadRT(rtW_ / 2, rtH_ / 2, HDR, false);
    blurA_ = loadRT(rtW_ / 2, rtH_ / 2, HDR, false);
    blurB_ = loadRT(rtW_ / 2, rtH_ / 2, HDR, false);
    qA_ = loadRT(rtW_ / 8, rtH_ / 8, HDR, false);
    qB_ = loadRT(rtW_ / 8, rtH_ / 8, HDR, false);
    ldr_ = loadRT(rtW_, rtH_, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8, false);
    SetTextureFilter(ldr_.texture, TEXTURE_FILTER_BILINEAR);   // upscaled to the window when the render scale is below 100 %
    // refraction copy: scene colour + linear depth (32-bit float so thin water stays precise)
    refr_ = loadRT(rtW_, rtH_, PIXELFORMAT_UNCOMPRESSED_R32G32B32A32, false);
    SetTextureFilter(refr_.texture, TEXTURE_FILTER_POINT);
    createVolTargets();
}

float Renderer::renderScale() const {
    if (renderScale_ > 0) return std::clamp(renderScale_, 0.3f, 1.f);
    if (const char* e = std::getenv("CORERAY_RENDERSCALE")) return std::clamp(float(std::atof(e)), 0.3f, 1.f);
    // auto: Retina screens have pixels to spare; the sharpened upscale is hard to tell apart
    return GetWindowScaleDPI().x > 1.25f ? 0.75f : 1.f;
}
int Renderer::targetW() const { return std::max(16, int(std::lround(GetRenderWidth() * renderScale()))); }
int Renderer::targetH() const { return std::max(16, int(std::lround(GetRenderHeight() * renderScale()))); }

void Renderer::destroyTargets() {
    destroyVolTargets();
    for (RenderTexture2D* r : {&scene_, &bright_, &blurA_, &blurB_, &qA_, &qB_, &ldr_, &refr_}) unloadRT(*r);
    rtW_ = rtH_ = 0;
}

// ------------------------------------------------------------------ camera
void Renderer::updateCamera(const SceneState& s, float dt) {
    if (s.freeCam) {   // explore mode: the camera is where it's told to be
        cam_.position = s.camPos;
        cam_.target = s.camTarget;
        cam_.up = {0, 1, 0};
        cam_.fovy = s.fov;
        cam_.projection = CAMERA_PERSPECTIVE;
        camInit_ = false;
        return;
    }
    // CoreKick's cameras (world.ts updateCamera): positions along the road relative to the
    // rider, smoothed *relative to the rider* so the camera never lags along the road.
    const Course& C = *course_;
    const double d = s.dist;
    auto rp = C.at(d);
    const float LANE = 1.4f;  // the rider's line, right of the centre line
    Vector3 anchor = Vector3Add(toV(rp.pos), Vector3Scale(rightOf(rp.heading), LANE));
    auto P = [&](double dd, float lat, float up) {
        auto q = C.at(dd);
        Vector3 v = Vector3Add(toV(q.pos), Vector3Scale(rightOf(q.heading), lat));
        v.y += up;
        return v;
    };
    CamMode mode = s.cam;
    int shot = -1;
    if (mode == CamMode::Cinematic) {
        cineTimer_ += dt;
        if (cineTimer_ > 16.f) { cineTimer_ = 0; cineShot_ = (cineShot_ + 1) % 6; camInit_ = false; }
        shot = cineShot_;
    }
    float climbing = smooth(3.f, 9.f, rp.grade * 100.f);
    Vector3 pos, tgt;
    float fovBase = 58.f;
    bool eyes = false;
    if (mode == CamMode::Chase || shot == 0) {
        pos = P(d - 7.2, LANE * 0.7f, 2.45f + climbing * 0.6f);
        tgt = P(d + 13, LANE * 0.6f, 1.1f);
    } else if (mode == CamMode::Side || shot == 1) {
        pos = P(d + 1, -7.6f, 2.1f);                  // on the far verge
        tgt = P(d + 2.5, LANE, 1.0f);
    } else if (mode == CamMode::Drone || shot == 2) {
        pos = P(d - 70, -28.f, 58.f);
        tgt = P(d + 90, 0, 0);
    } else if (mode == CamMode::Rider || shot == 5) {
        pos = P(d + 1.6, LANE - 3.3f, 1.35f);          // close three-quarter view of the rider
        tgt = P(d + 0.1, LANE, 0.95f);
    } else if (shot == 3) {
        float t = cineTimer_ / 16.f * 18.f;            // low, ahead, looking back as the rider comes through
        pos = P(d + 16 - t, -2.8f, 0.7f);
        tgt = P(d, LANE, 1.0f);
    } else if (shot == 4) {
        float a = cineTimer_ * 0.12f;                  // high trailing crane, slowly orbiting
        pos = P(d - 30, std::sin(a) * 30.f, 22.f);
        tgt = P(d + 30, 0, 0);
    } else if (mode == CamMode::Close && devCam_[0] < -1e8f) {
        pos = P(d - 3.0, LANE + 0.2f, 1.45f);
        tgt = P(d + 2.5, LANE, 1.05f);
        fovBase = 62.f;
    } else if (mode == CamMode::Close && devCam_[0] > -1e8f) {
        pos = P(d + devCam_[0], devCam_[1], devCam_[2]);  // dev: CORERAY_DEVCAM="along,lat,up,talong,tlat,tup"
        tgt = P(d + devCam_[3], devCam_[4], devCam_[5]);
    } else {  // eyes
        pos = Vector3Add(anchor, Vector3Add(Vector3Scale(fwdOf(rp.heading), 0.33f), {0, 1.66f, 0}));
        tgt = P(d + 30, LANE, 1.2f);
        fovBase = 72.f;
        eyes = true;
    }
    // in a tunnel the land overhead isn't the floor
    const bool inTunnel = C.world() && (C.kindAt(d) == 1 || C.kindAt(d - 12) == 1 || C.kindAt(d + 12) == 1);
    if (!inTunnel) pos.y = std::max(pos.y, C.terrainHeight(pos.x, pos.z) + 1.3f);
    Vector3 off = Vector3Subtract(pos, anchor), lookOff = Vector3Subtract(tgt, anchor);
    if (!camInit_ || eyes) {
        camPosSmooth_ = off;
        camTargetSmooth_ = lookOff;
        camInit_ = true;
    } else {
        camPosSmooth_ = Vector3Lerp(camPosSmooth_, off, 1.f - std::exp(-dt * (mode == CamMode::Drone || shot == 2 ? 1.2f : 2.6f)));
        camTargetSmooth_ = Vector3Lerp(camTargetSmooth_, lookOff, 1.f - std::exp(-dt * 3.5f));
    }
    // a touch wider at speed, for a sense of pace
    float fovT = fovBase + std::clamp((float(s.speed) - 7.f) * 0.45f, 0.f, 9.f) * (mode == CamMode::Drone ? 0.3f : 1.f);
    cam_.fovy = cam_.fovy > 1 ? cam_.fovy + (fovT - cam_.fovy) * (1.f - std::exp(-dt * 1.5f)) : fovT;
    cam_.position = Vector3Add(anchor, camPosSmooth_);
    float g2 = C.terrainHeight(cam_.position.x, cam_.position.z) + 1.2f;
    if (!eyes && !inTunnel && cam_.position.y < g2) cam_.position.y = g2;
    cam_.target = Vector3Add(anchor, camTargetSmooth_);
    cam_.up = {0, 1, 0};
    cam_.projection = CAMERA_PERSPECTIVE;
}

// ------------------------------------------------------------------ uniforms
void Renderer::setCommonUniforms(Shader sh) {
    setV3(sh, "camPos", cam_.position);
    setV3(sh, "sunDir", L_.lightDir);
    setV3(sh, "sunColor", L_.lightColor);
    setV3(sh, "skyZenith", L_.zenith);
    setV3(sh, "skyHorizon", L_.horizon);
    setV3(sh, "groundColor", L_.ground);
    setF(sh, "ambient", L_.ambient);
    setF(sh, "fogDensity", L_.fog);
    setF(sh, "time", time_);
    setI(sh, "shadowMap0", 10);
    setI(sh, "shadowMap1", 11);
    setM(sh, "lightVP0", lightVP_[0]);
    setM(sh, "lightVP1", lightVP_[1]);
    setV2(sh, "shadowTexel", {1.f / shadowRes_, 1.f / shadowRes_});
    setV2(sh, "cascadeRange", {cascadeRange_[0], cascadeRange_[1]});
    Vector3 f = Vector3Subtract(cam_.target, cam_.position);
    f.y = 0;
    f = Vector3Normalize(f);
    setV3(sh, "cascadeCenter0", Vector3Add(cam_.position, Vector3Scale(f, cascadeRange_[0] * 0.55f)));
    setV3(sh, "cascadeCenter1", Vector3Add(cam_.position, Vector3Scale(f, cascadeRange_[1] * 0.55f)));
    setI(sh, "shadowsOn", 1);
    setF(sh, "waterLevel", course_->waterLevel());
    if (const wmap::Map* W = course_->world()) {   // a generated world: its own climate
        setF(sh, "snowLine", W->snowLine);
        setF(sh, "landBase", W->snowLine - 1900.f);
        setI(sh, "worldMode", 1);
    } else {
        setF(sh, "snowLine", refMax_ + 120.f);
        setF(sh, "landBase", refMin_);
        setI(sh, "worldMode", 0);
    }
    setF(sh, "cloudCover", L_.clouds);
    setF(sh, "night", L_.night);
    setF(sh, "fogGrey", L_.fogGrey);
    setI(sh, "cloudMap", 13);
    setF(sh, "cloudShadowAmt", volEnabled() && csm_.id ? 1.f : 0.f);
    setF(sh, "cloudCov", cloud_.coverage);
    setV4(sh, "csmRect", csmRect_);
    setF(sh, "volClouds", volEnabled() ? 1.f : 0.f);
    setLightUniforms(sh);
    // ridges between the sun and the land
    setI(sh, "sunTerrain", 9);
    setV4(sh, "sunTerrainRect", sunTex_ ? sunRect_ : Vector4{0, 0, 0, 0});
    // tunnels: daylight only from the mouths, the ceiling lamps
    setI(sh, "tunnelMap", 15);
    setV4(sh, "tunnelRect", tunnelTex_ ? tunnelRect_ : Vector4{0, 0, 0, 0});
    int nt = int(std::min<size_t>(24, tunnelLampsU_.size()));
    setI(sh, "numTunnelLamps", tunnelRect_.z > 0 ? nt : 0);
    setV3(sh, "tunnelLampCol", Vector3Scale({1.0f, 0.80f, 0.56f}, 8.f));   // warm white
    int lt = GetShaderLocation(sh, "tunnelLamp");
    if (nt > 0 && lt >= 0) SetShaderValueV(sh, lt, tunnelLampsU_.data(), SHADER_UNIFORM_VEC3, nt);
}

void Renderer::setLightUniforms(Shader sh) {
    int n = int(std::min<size_t>(16, lights_.size()));
    setI(sh, "numLights", n);
    for (int i = 0; i < n; ++i) {
        char nm[32];
        std::snprintf(nm, sizeof nm, "lightPos[%d]", i);
        setV3(sh, nm, lights_[i].pos);
        std::snprintf(nm, sizeof nm, "lightCol[%d]", i);
        setV3(sh, nm, lights_[i].col);
        std::snprintf(nm, sizeof nm, "lightDir[%d]", i);
        setV3(sh, nm, lights_[i].dir);
        std::snprintf(nm, sizeof nm, "lightCone[%d]", i);
        setV2(sh, nm, {lights_[i].cosIn, lights_[i].cosOut});
        std::snprintf(nm, sizeof nm, "lightShape[%d]", i);
        setV4(sh, nm, {lights_[i].axis.x, lights_[i].axis.y, lights_[i].axis.z, lights_[i].elong});
        std::snprintf(nm, sizeof nm, "lightVol[%d]", i);
        setF(sh, nm, lights_[i].vol);
    }
}

// ------------------------------------------------------------------ rider
void Renderer::drawPart(const Mesh& m, Matrix local, const Matrix& base, Color c, float rough, float spec, bool shadowPass, int pattern, float emissive) {
    Matrix world = MatrixMultiply(local, base);
    if (shadowPass) { DrawMesh(m, depthMat_, world); return; }
    objectMat_.maps[MATERIAL_MAP_DIFFUSE].color = c;
    setF(objectSh_, "roughness", rough);
    setF(objectSh_, "specular", spec);
    setI(objectSh_, "pattern", pattern);
    setF(objectSh_, "emissive", emissive);
    DrawMesh(m, objectMat_, world);
}

void Renderer::drawLimb(Vector3 a, Vector3 b, float r, const Matrix& base, Color c, bool shadowPass, float rough, float spec) {
    drawPart(cyl_, alignY(a, b, r, r), base, c, rough, spec, shadowPass);
    drawPart(sphere_, MatrixMultiply(MatrixScale(r, r, r), MatrixTranslate(b.x, b.y, b.z)), base, c, rough, spec, shadowPass);
}

Matrix Renderer::riderBase(const SceneState& s) const {
    auto rp = course_->at(s.dist);
    Vector3 pos = Vector3Add(toV(rp.pos), Vector3Scale(rightOf(rp.heading), 1.4f));
    return MatrixMultiply(MatrixMultiply(MatrixMultiply(MatrixRotateX(-std::atan(rp.grade)), MatrixRotateZ(s.lean + std::sin(float(s.crankAngle)) * 0.09f * s.stand)),
                                         MatrixRotateY(rp.heading)),
                          MatrixTranslate(pos.x, pos.y, pos.z));
}

void Renderer::drawRider(const Matrix& base, const SceneState& s, bool ghost, bool sp) {
    const Color frame = ghost ? Color{90, 150, 255, 255} : Color{28, 30, 36, 255};
    const Color accent = ghost ? Color{120, 190, 255, 255} : Color{235, 70, 50, 255};
    const Color tire{18, 18, 18, 255};
    const Color metal{170, 172, 178, 255};
    const float em = ghost ? 0.35f : 0.f;

    // Geometry of a 56 cm endurance road bike (metres, bike-local: +z forward, +x = rider's
    // left, y up, ground at 0). Rider contact points (saddle, hoods, pedals at BB +-170 mm)
    // are shared with RiderRig / the greybox body and must not move.
    const float AY = WHEEL_ROLL_R;                       // axle height = loaded rolling radius
    Vector3 rearAxle{0, AY, -0.408f}, frontAxle{0, AY, 0.598f}, bb{0, 0.275f, 0};
    Vector3 seatTop{0, 0.80f, -0.148f}, headTop{0, 0.87f, 0.462f}, headBot{0, 0.715f, 0.510f};
    Vector3 saddle{0, 0.93f, -0.19f};
    Vector3 stemEnd{0, 0.905f, 0.572f};
    const Color bar{22, 22, 26, 255};
    const Color rubber{30, 30, 34, 255};
    const Color chain{95, 95, 100, 255};

    // ---- frame (tapered / ovalised tubes)
    drawPart(cyl_, alignY(bb, headBot, 0.019f, 0.026f), base, accent, 0.35f, 0.6f, sp, 0, em);   // down tube (aero oval)
    drawPart(cyl_, alignY(bb, seatTop, 0.017f, 0.019f), base, frame, 0.35f, 0.6f, sp, 0, em);    // seat tube
    drawLimb(seatTop, headTop, 0.0145f, base, frame, sp, 0.35f, 0.6f);                         // top tube (slight slope)
    drawPart(cyl_, alignY(headBot, headTop, 0.021f, 0.021f), base, frame, 0.35f, 0.6f, sp, 0, em);  // head tube
    drawPart(sphere_, MatrixMultiply(MatrixScale(0.034f, 0.03f, 0.04f), MatrixTranslate(bb.x, bb.y, bb.z)), base, frame, 0.35f, 0.6f, sp);  // BB shell
    for (float x : {-1.f, 1.f}) {
        // chainstays and dropped seatstays to the rear dropout
        drawLimb({x * 0.030f, bb.y + 0.005f, bb.z - 0.03f}, {x * 0.064f, rearAxle.y, rearAxle.z + 0.01f}, 0.0105f, base, frame, sp, 0.35f, 0.6f);
        drawLimb({x * 0.016f, 0.735f, -0.128f}, {x * 0.064f, rearAxle.y + 0.012f, rearAxle.z + 0.004f}, 0.0085f, base, frame, sp, 0.35f, 0.6f);
        // fork: straight-tapered blades with 45 mm rake to the front axle
        Vector3 crown{x * 0.032f, headBot.y - 0.025f, headBot.z + 0.008f};
        Vector3 mid{x * 0.052f, 0.48f, 0.565f};
        drawPart(cyl_, alignY(crown, mid, 0.012f, 0.017f), base, frame, 0.35f, 0.6f, sp, 0, em);
        drawPart(cyl_, alignY(mid, {x * 0.054f, frontAxle.y, frontAxle.z}, 0.0095f, 0.012f), base, frame, 0.35f, 0.6f, sp, 0, em);
    }
    drawPart(cube_, MatrixMultiply(MatrixScale(0.075f, 0.03f, 0.045f), MatrixTranslate(0, headBot.y - 0.02f, headBot.z + 0.006f)), base, frame, 0.35f, 0.6f, sp);  // fork crown
    // seatpost, saddle (rails + shell + nose)
    drawLimb(seatTop, {0, saddle.y - 0.03f, saddle.z + 0.012f}, 0.0135f, base, bar, sp, 0.4f, 0.5f);
    drawPart(sphere_, MatrixMultiply(MatrixScale(0.068f, 0.018f, 0.095f), MatrixTranslate(saddle.x, saddle.y - 0.008f, saddle.z - 0.035f)), base, {15, 15, 15, 255}, 0.6f, 0.3f, sp);
    drawPart(sphere_, ellipsoid({0, saddle.y - 0.008f, saddle.z - 0.02f}, {0, saddle.y - 0.012f, saddle.z + 0.125f}, 0.022f, 0.016f), base, {15, 15, 15, 255}, 0.6f, 0.3f, sp);
    // cockpit: spacers, stem, bar tops, ramps to the hoods, drops; hoods + levers
    drawLimb(headTop, {0, headTop.y + 0.02f, headTop.z - 0.006f}, 0.017f, base, bar, sp);
    drawPart(cyl_, alignY({0, headTop.y + 0.012f, headTop.z - 0.004f}, stemEnd, 0.016f, 0.019f), base, bar, 0.4f, 0.5f, sp);
    drawPart(cyl_, alignY({-0.205f, stemEnd.y, stemEnd.z}, {0.205f, stemEnd.y, stemEnd.z}, 0.0125f, 0.0125f), base, bar, 0.7f, 0.2f, sp);
    for (float x : {-0.205f, 0.205f}) {
        float sx = x > 0 ? 1.f : -1.f;
        Vector3 bend{x + sx * 0.008f, stemEnd.y + 0.004f, stemEnd.z + 0.045f};
        Vector3 hood{x + sx * 0.008f, 0.925f, 0.625f};
        drawLimb({x, stemEnd.y, stemEnd.z}, bend, 0.0125f, base, bar, sp, 0.7f, 0.2f);
        drawLimb(bend, hood, 0.0125f, base, bar, sp, 0.7f, 0.2f);
        drawLimb(hood, {x + sx * 0.012f, 0.86f, 0.655f}, 0.0125f, base, bar, sp, 0.7f, 0.2f);       // drop curve
        drawLimb({x + sx * 0.012f, 0.86f, 0.655f}, {x + sx * 0.014f, 0.80f, 0.62f}, 0.0125f, base, bar, sp, 0.7f, 0.2f);
        drawLimb({x + sx * 0.014f, 0.80f, 0.62f}, {x + sx * 0.014f, 0.785f, 0.535f}, 0.0125f, base, bar, sp, 0.7f, 0.2f);  // drops
        drawPart(sphere_, ellipsoid({hood.x, hood.y - 0.005f, hood.z - 0.02f}, {hood.x, hood.y + 0.012f, hood.z + 0.04f}, 0.017f, 0.021f), base, rubber, 0.8f, 0.2f, sp);  // hood body
        drawLimb({hood.x, hood.y - 0.005f, hood.z + 0.035f}, {hood.x, 0.83f, 0.672f}, 0.0065f, base, metal, sp, 0.3f, 0.8f);  // brake/shift lever
    }
    // lights: front white, rear red - they glow at night
    float glow = L_.night;
    drawPart(cube_, MatrixMultiply(MatrixScale(0.05f, 0.035f, 0.05f), MatrixTranslate(0, stemEnd.y - 0.04f, stemEnd.z + 0.05f)), base, {240, 240, 230, 255}, 0.3f, 0.5f, sp, 0, 2 + glow * 30);
    drawPart(cube_, MatrixMultiply(MatrixScale(0.03f, 0.05f, 0.02f), MatrixTranslate(0, 0.76f, -0.245f)), base, {230, 20, 20, 255}, 0.3f, 0.5f, sp, 0, 1 + glow * 25);
    // bottle cage + bottle on the down tube
    {
        Vector3 dd = Vector3Normalize(Vector3Subtract(headBot, bb));
        Vector3 up{0, dd.z, -dd.y};
        Vector3 b0 = Vector3Add(Vector3Add(bb, Vector3Scale(dd, 0.16f)), Vector3Scale(up, 0.05f));
        Vector3 b1 = Vector3Add(b0, Vector3Scale(dd, 0.19f));
        drawPart(cyl_, alignY(b0, b1, 0.034f, 0.034f), base, ghost ? accent : Color{230, 232, 236, 255}, 0.5f, 0.4f, sp, 0, em);
        drawLimb(b1, Vector3Add(b1, Vector3Scale(dd, 0.025f)), 0.014f, base, {20, 20, 24, 255}, sp);
    }

    // ---- wheels: tyre, deep rim, hub + laced spokes, disc rotor (left side); all spin together.
    // wheelAngle = distance / WHEEL_ROLL_R, so the contact patch is stationary on the road.
    for (int w = 0; w < 2; ++w) {
        Vector3 axle = w ? frontAxle : rearAxle;
        Matrix spin = MatrixMultiply(MatrixRotateX(float(s.wheelAngle)), MatrixTranslate(axle.x, axle.y, axle.z));
        drawPart(wheelTire_, spin, base, tire, 0.85f, 0.08f, sp);
        drawPart(wheelRim_, spin, base, ghost ? accent : Color{30, 30, 34, 255}, 0.3f, 0.7f, sp, 0, em);
        if (!sp) drawPart(w ? spokesFront_ : spokesRear_, spin, base, metal, 0.3f, 0.8f, false);
        drawPart(rotor_, MatrixMultiply(MatrixMultiply(MatrixTranslate(0.058f, 0, 0), MatrixRotateX(float(s.wheelAngle))), MatrixTranslate(axle.x, axle.y, axle.z)),
                 base, metal, 0.25f, 0.9f, sp);
        // brake caliper on the left fork blade / chainstay, just above-behind the rotor
        float cz = w ? -0.055f : 0.06f;
        drawPart(cube_, MatrixMultiply(MatrixScale(0.022f, 0.05f, 0.06f), MatrixTranslate(0.058f, axle.y + 0.055f, axle.z + cz)), base, {40, 40, 44, 255}, 0.4f, 0.6f, sp);
        drawPart(cyl_, MatrixMultiply(alignY({-0.068f, 0, 0}, {0.068f, 0, 0}, 0.006f, 0.006f), MatrixTranslate(axle.x, axle.y, axle.z)), base, metal, 0.3f, 0.8f, sp);  // thru-axle
    }

    // ---- drivetrain (right side, -x): cassette, chainset at the crank angle, chain, derailleurs
    drawPart(cassette_, MatrixMultiply(MatrixMultiply(MatrixTranslate(-0.028f, 0, 0), MatrixRotateX(float(s.wheelAngle))), MatrixTranslate(rearAxle.x, rearAxle.y, rearAxle.z)),
             base, metal, 0.3f, 0.85f, sp);
    drawPart(chainring_, MatrixMultiply(MatrixMultiply(MatrixTranslate(-0.068f, 0, 0), MatrixRotateX(float(s.crankAngle))), MatrixTranslate(bb.x, bb.y, bb.z)),
             base, {50, 50, 56, 255}, 0.3f, 0.8f, sp);
    if (!sp) {
        // chain on the big ring (pitch r 105 mm) and a mid cog (17T, r 34 mm)
        const float rc = 0.0127f * 52 / (2 * PI_F), rk = 0.0127f * 17 / (2 * PI_F), cx = -0.07f;
        Vector3 cTop{cx, bb.y + rc, bb.z}, kTop{cx + 0.022f, rearAxle.y + rk, rearAxle.z};
        Vector3 pUp{cx + 0.014f, rearAxle.y - 0.05f, rearAxle.z + 0.004f}, pLo{cx + 0.006f, rearAxle.y - 0.118f, rearAxle.z + 0.03f};
        Vector3 cBot{cx, bb.y - rc, bb.z};
        drawLimb(cTop, kTop, 0.0042f, base, chain, false, 0.4f, 0.9f);
        drawLimb(cBot, pLo, 0.0042f, base, chain, false, 0.4f, 0.9f);
        drawLimb(pLo, pUp, 0.0042f, base, chain, false, 0.4f, 0.9f);
        drawLimb(pUp, {cx + 0.022f, rearAxle.y - rk, rearAxle.z - 0.01f}, 0.0042f, base, chain, false, 0.4f, 0.9f);
        // rear derailleur: body off the hanger + cage with two jockey wheels
        drawPart(cube_, MatrixMultiply(MatrixScale(0.016f, 0.045f, 0.03f), MatrixTranslate(cx + 0.006f, rearAxle.y - 0.03f, rearAxle.z - 0.012f)), base, {35, 35, 40, 255}, 0.4f, 0.6f, false);
        drawPart(cyl_, alignY(pUp, pLo, 0.006f, 0.016f), base, {35, 35, 40, 255}, 0.4f, 0.6f, false);
        for (Vector3 pj : {pUp, pLo})
            drawPart(sphere_, MatrixMultiply(MatrixScale(0.006f, 0.0125f, 0.0125f), MatrixTranslate(pj.x - 0.004f, pj.y, pj.z)), base, metal, 0.3f, 0.8f, false);
        // front derailleur cage on the seat tube, just above the big ring
        Vector3 st = Vector3Lerp(bb, seatTop, 0.24f);
        drawPart(cube_, MatrixMultiply(MatrixScale(0.012f, 0.022f, 0.06f), MatrixTranslate(-0.072f, st.y + 0.008f, st.z + 0.03f)), base, {35, 35, 40, 255}, 0.4f, 0.6f, false);
    }
    // cranks (170 mm, flattened arms) + road pedals at the real crank angle (forward pedalling)
    drawPart(cyl_, alignY({-0.072f, bb.y, bb.z}, {0.072f, bb.y, bb.z}, 0.012f, 0.012f), base, metal, 0.3f, 0.8f, sp);  // spindle
    for (int side = 0; side < 2; ++side) {
        Vector3 ped = RiderRig::pedal(s.crankAngle, side);
        float x = ped.x, sx = side ? 1.f : -1.f;
        Vector3 root{sx * 0.074f, bb.y, bb.z}, tip{x * 0.86f, ped.y, ped.z};
        drawPart(cyl_, alignY(root, tip, 0.008f, 0.016f), base, {45, 45, 50, 255}, 0.3f, 0.8f, sp);
        drawLimb(tip, ped, 0.006f, base, metal, sp, 0.3f, 0.8f);
        drawPart(cube_, MatrixMultiply(MatrixScale(0.068f, 0.012f, 0.075f), MatrixTranslate(ped.x + sx * 0.03f, ped.y - 0.002f, ped.z)), base, {30, 30, 34, 255}, 0.5f, 0.4f, sp);
    }
    // ---- the rider: skinned human posed on the bike
    RiderRig& rig = ghost ? ghostRig_ : rig_;
    if (!ghost && greyOn_ && greyReady_) { drawGrey(base, sp); return; }
    if (sp) DrawMesh(rig.mesh(), depthMat_, base);
    else {
        auto lin = [](Color c) { return Vector3{c.r / 255.f, c.g / 255.f, c.b / 255.f}; };
        setV3(kitSh_, "kitJersey", lin(ghost ? Color{80, 150, 255, 255} : Color{214, 40, 36, 255}));
        setV3(kitSh_, "kitAccent", lin(ghost ? Color{160, 210, 255, 255} : Color{245, 245, 245, 255}));
        setV3(kitSh_, "kitShorts", lin(Color{20, 20, 24, 255}));
        setV3(kitSh_, "kitSkin", lin(Color{222, 172, 140, 255}));
        setV3(kitSh_, "kitShoe", lin(Color{240, 240, 240, 255}));
        setF(kitSh_, "ghost", ghost ? 1.f : 0.f);
        DrawMesh(rig.mesh(), kitMat_, base);
    }
    // helmet + glasses ride on the head bone (bind-space placement, then the bone's pose)
    Matrix headM = rig.boneMatrix(RiderRig::Head);
    Matrix helm = MatrixMultiply(MatrixMultiply(MatrixMultiply(MatrixScale(0.11f, 0.088f, 0.152f), MatrixRotateX(-0.12f)), MatrixTranslate(0, 1.722f, -0.008f)), headM);
    drawPart(sphere_, helm, base, ghost ? accent : Color{248, 248, 248, 255}, 0.3f, 0.6f, sp, 0, em);
    if (!sp) {
        Matrix vent = MatrixMultiply(MatrixMultiply(MatrixScale(0.112f, 0.03f, 0.13f), MatrixTranslate(0, 1.77f, -0.02f)), headM);
        drawPart(sphere_, vent, base, ghost ? accent : Color{214, 40, 36, 255}, 0.3f, 0.6f, false, 0, em);
        Matrix glasses = MatrixMultiply(MatrixMultiply(MatrixScale(0.088f, 0.024f, 0.03f), MatrixTranslate(0, 1.66f, 0.088f)), headM);
        drawPart(sphere_, glasses, base, {10, 10, 14, 255}, 0.05f, 1.2f, false);
    }
}

// ------------------------------------------------------------------ greybox rider
void Renderer::poseGrey(const SceneState& s) {
    if (!greyReady_) {
        if (!greyFuture_.valid() || greyFuture_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        grey_ = greyFuture_.get();
        const body::Mesh& M = grey_.mesh;
        greyMesh_ = Mesh{};
        greyMesh_.vertexCount = int(M.pos.size());
        greyMesh_.triangleCount = int(M.idx.size() / 3);
        greyMesh_.vertices = (float*)MemAlloc(greyMesh_.vertexCount * 3 * sizeof(float));
        greyMesh_.normals = (float*)MemAlloc(greyMesh_.vertexCount * 3 * sizeof(float));
        greyMesh_.texcoords = (float*)MemAlloc(greyMesh_.vertexCount * 2 * sizeof(float));
        greyMesh_.indices = (unsigned short*)MemAlloc(M.idx.size() * sizeof(unsigned short));
        std::memcpy(greyMesh_.vertices, M.pos.data(), M.pos.size() * sizeof(body::V3));
        std::memcpy(greyMesh_.normals, M.nrm.data(), M.nrm.size() * sizeof(body::V3));
        for (size_t i = 0; i < M.idx.size(); ++i) greyMesh_.indices[i] = (unsigned short)M.idx[i];
        UploadMesh(&greyMesh_, true);
        greyReady_ = true;
    }
    // the same bike geometry the kitted rider and drawRider use
    body::anim::Bike bk;
    bk.crank = float(s.crankAngle);
    bk.seat = {0, 0.93f, -0.19f};
    bk.hoodL = {0.21f, 0.94f, 0.625f};
    bk.hoodR = {-0.21f, 0.94f, 0.625f};
    Vector3 pr = RiderRig::pedal(s.crankAngle, 0), pl = RiderRig::pedal(s.crankAngle, 1);
    bk.pedalR = {pr.x, pr.y, pr.z};
    bk.pedalL = {pl.x, pl.y, pl.z};
    bk.stand = s.stand;
    bk.tuck = s.tuck;
    bk.effort = s.effort;
    body::Pose pose;
    body::anim::cycle(grey_, pose, bk);
    greyG_ = body::forward(grey_.skeleton, pose);
    body::skin(grey_, greyG_, greyPos_, greyNrm_);
    std::memcpy(greyMesh_.vertices, greyPos_.data(), greyPos_.size() * sizeof(body::V3));
    std::memcpy(greyMesh_.normals, greyNrm_.data(), greyNrm_.size() * sizeof(body::V3));
    UpdateMeshBuffer(greyMesh_, 0, greyMesh_.vertices, greyMesh_.vertexCount * 12, 0);
    UpdateMeshBuffer(greyMesh_, 2, greyMesh_.normals, greyMesh_.vertexCount * 12, 0);
}

void Renderer::drawGrey(const Matrix& base, bool sp) {
    drawPart(greyMesh_, MatrixIdentity(), base, {132, 133, 138, 255}, 0.75f, 0.15f, sp);
    // the helmet stays: it rides on the head bone
    const body::BoneRest& hr = grey_.skeleton.bones[body::Head];
    body::Quat dq = body::qmul(greyG_.rot[body::Head], body::qconj(hr.rot));
    body::V3 ax;
    float ang = 2 * std::acos(std::clamp(dq.w, -1.f, 1.f));
    float sn = std::sqrt(std::max(0.f, 1 - dq.w * dq.w));
    ax = sn > 1e-5f ? body::V3{dq.x / sn, dq.y / sn, dq.z / sn} : body::V3{1, 0, 0};
    Matrix headM = MatrixMultiply(MatrixMultiply(MatrixTranslate(-hr.head.x, -hr.head.y, -hr.head.z), MatrixRotate({ax.x, ax.y, ax.z}, ang)),
                                  MatrixTranslate(greyG_.pos[body::Head].x, greyG_.pos[body::Head].y, greyG_.pos[body::Head].z));
    float H = grey_.params.height, top = hr.head.y + hr.length;
    Matrix helm = MatrixMultiply(MatrixMultiply(MatrixMultiply(MatrixScale(0.065f * H, 0.05f * H, 0.085f * H), MatrixRotateX(-0.12f)),
                                                MatrixTranslate(0, top - 0.055f * H, -0.012f * H)), headM);
    drawPart(sphere_, helm, base, {248, 248, 248, 255}, 0.3f, 0.6f, sp);
}

// ------------------------------------------------------------------ shadows
void Renderer::renderShadowMaps(const SceneState& s) {
    Vector3 f = Vector3Subtract(cam_.target, cam_.position);
    f.y = 0;
    f = Vector3Normalize(f);
    for (int c = 0; c < 2; ++c) {
        float range = cascadeRange_[c];
        Vector3 focus = Vector3Add(cam_.position, Vector3Scale(f, range * 0.55f));
        focus.y = course_->at(s.dist).pos.y;
        Vector3 Ld = L_.lightDir;
        Vector3 fwd = Vector3Negate(Ld);
        Vector3 r = Vector3Normalize(Vector3CrossProduct(fwd, {0, 1, 0}));
        Vector3 u = Vector3CrossProduct(r, fwd);
        float texel = 2 * range / shadowRes_;
        float cr = Vector3DotProduct(focus, r), cu = Vector3DotProduct(focus, u);
        focus = Vector3Add(focus, Vector3Add(Vector3Scale(r, std::floor(cr / texel) * texel - cr), Vector3Scale(u, std::floor(cu / texel) * texel - cu)));
        Camera3D lc{};
        lc.position = Vector3Add(focus, Vector3Scale(Ld, 900.f));
        lc.target = focus;
        lc.up = {0, 1, 0};
        lc.fovy = range * 2.f;
        lc.projection = CAMERA_ORTHOGRAPHIC;
        BeginTextureMode(shadow_[c]);
        ClearBackground(WHITE);
        rlClearScreenBuffers();
        rlSetClipPlanes(10.0, 2200.0);
        BeginMode3D(lc);
        lightVP_[c] = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
        drawWorld(true, c, focus, range);
        if (c == 0) {
            drawLanterns(true, focus, range);
            if (s.wildlife) drawWildlife(true, focus, range);
        }
        if (!s.hideRider) drawRider(riderBase(s), s, false, true);
        EndMode3D();
        EndTextureMode();
    }
}

// ------------------------------------------------------------------ post
void Renderer::postProcess(const SceneState& s) {
    const RenderTexture2D& src = volEnabled() && sceneFx_.id ? sceneFx_ : scene_;
    BeginTextureMode(bright_);
    BeginShaderMode(brightSh_);
    setV2(brightSh_, "texel", {1.f / rtW_, 1.f / rtH_});
    setF(brightSh_, "threshold", 1.6f);
    blit(src, bright_);
    EndShaderMode();
    EndTextureMode();
    auto blurPass = [&](RenderTexture2D& src, RenderTexture2D& dst, Vector2 dir) {
        BeginTextureMode(dst);
        BeginShaderMode(blurSh_);
        setV2(blurSh_, "dir", dir);
        blit(src, dst);
        EndShaderMode();
        EndTextureMode();
    };
    float hw = 1.f / bright_.texture.width, hh = 1.f / bright_.texture.height;
    blurPass(bright_, blurB_, {hw, 0});
    blurPass(blurB_, blurA_, {0, hh});
    blurPass(blurA_, blurB_, {hw * 2, 0});
    blurPass(blurB_, blurA_, {0, hh * 2});
    BeginTextureMode(qA_);
    blit(blurA_, qA_);
    EndTextureMode();
    float qw = 1.f / qA_.texture.width, qh = 1.f / qA_.texture.height;
    blurPass(qA_, qB_, {qw, 0});
    blurPass(qB_, qA_, {0, qh});
    blurPass(qA_, qB_, {qw * 2, 0});
    blurPass(qB_, qA_, {0, qh * 2});

    Vector3 sunWorld = Vector3Add(cam_.position, Vector3Scale(L_.sunDir, 3000.f));
    Vector2 sp = GetWorldToScreenEx(sunWorld, cam_, rtW_, rtH_);
    Vector3 camF = Vector3Normalize(Vector3Subtract(cam_.target, cam_.position));
    float facing = Vector3DotProduct(camF, L_.sunDir);
    Vector2 suv{sp.x / rtW_, 1.f - sp.y / rtH_};
    float onScreen = 1.f - smooth(0.6f, 1.1f, std::max(std::fabs(suv.x - 0.5f), std::fabs(suv.y - 0.5f)) * 2.f * 0.5f + 0.25f);
    float sunVis = facing > 0 ? smooth(0.0f, 0.5f, facing) * onScreen * L_.sunVisible : 0.f;

    BeginTextureMode(ldr_);
    BeginShaderMode(compositeSh_);
    SetShaderValueTexture(compositeSh_, GetShaderLocation(compositeSh_, "bloomA"), blurA_.texture);
    SetShaderValueTexture(compositeSh_, GetShaderLocation(compositeSh_, "bloomB"), qA_.texture);
    setV2(compositeSh_, "sunUV", suv);
    setF(compositeSh_, "sunVisible", sunVis);
    setF(compositeSh_, "exposure", L_.exposure);
    setF(compositeSh_, "bloomStrength", 0.35f + L_.night * 0.25f);
    setF(compositeSh_, "shaftStrength", 0.45f + smooth(0.3f, 0.66f, s.sky.tod) * 0.5f);
    setF(compositeSh_, "saturation", 1.08f - s.wx.rain * 0.18f - s.wx.fog * 0.2f - L_.night * 0.25f);
    setF(compositeSh_, "rainAmt", s.wx.rain);
    setF(compositeSh_, "snowAmt", std::clamp(s.wx.snow + s.wx.hail * 0.4f, 0.f, 1.f));
    bool lensCam = s.cam == CamMode::Chase || s.cam == CamMode::Close || s.cam == CamMode::FirstPerson;
    setF(compositeSh_, "lensDrops", lensCam ? 1.f : 0.f);
    setF(compositeSh_, "ptime", float(s.time));
    {
        Vector3 cf = Vector3Normalize(Vector3Subtract(cam_.target, cam_.position));
        Vector3 cr = Vector3Normalize(Vector3CrossProduct(cf, {0, 1, 0}));
        Vector3 wv{std::sin(s.wx.windDir), 0, std::cos(s.wx.windDir)};
        setF(compositeSh_, "windX", Vector3DotProduct(wv, cr) * (0.3f + s.wx.wind * 1.4f));
    }
    setF(compositeSh_, "aspect", float(rtW_) / float(std::max(1, rtH_)));
    blit(src, ldr_);
    EndShaderMode();
    EndTextureMode();

    BeginShaderMode(fxaaSh_);
    setV2(fxaaSh_, "texel", {1.f / rtW_, 1.f / rtH_});
    // below full resolution the upscale gets a light contrast-adaptive sharpen
    setF(fxaaSh_, "sharpen", std::clamp((1.f - float(rtW_) / std::max(1, GetRenderWidth())) * 1.0f, 0.f, 0.35f));   // gentle: more brings back texture grain
    DrawTexturePro(ldr_.texture, {0, 0, float(rtW_), -float(rtH_)}, {0, 0, float(GetScreenWidth()), float(GetScreenHeight())}, {0, 0}, 0, WHITE);
    EndShaderMode();
}

// ------------------------------------------------------------------ water
void Renderer::drawWater(const SceneState& s) {
    static bool off = std::getenv("CORERAY_NOWATER") != nullptr;
    if (off) return;
    bool any = false;
    for (auto& [k, c] : chunks_) any |= c.water.vaoId != 0;
    if (!any) return;
    // copy the opaque scene (colour + linear depth) so the water can look through itself
    EndMode3D();
    EndTextureMode();
    BeginTextureMode(refr_);
    rlDrawRenderBatchActive();
    rlDisableColorBlend();  // alpha carries depth: write it as is
    BeginShaderMode(refrCopySh_);
    SetShaderValueTexture(refrCopySh_, GetShaderLocation(refrCopySh_, "sceneDepth"), Texture2D{scene_.depth.id, rtW_, rtH_, 1, 19});
    setV2(refrCopySh_, "clipNF", {0.3f, farClip()});
    blit(scene_, refr_);
    EndShaderMode();
    rlDrawRenderBatchActive();
    rlEnableColorBlend();
    EndTextureMode();
    BeginTextureMode(scene_);
    BeginMode3D(cam_);
    rlActiveTextureSlot(12);
    rlEnableTexture(refr_.texture.id);
    rlActiveTextureSlot(0);
    setI(waterSh_, "refrColor", 12);
    setV2(waterSh_, "screenSize", {float(rtW_), float(rtH_)});
    setV2(waterSh_, "clipNF", {0.3f, farClip()});
    setV2(waterSh_, "windVec", {std::sin(s.wx.windDir) * (0.2f + s.wx.wind), std::cos(s.wx.windDir) * (0.2f + s.wx.wind)});
    setF(waterSh_, "turbidity", std::clamp(0.06f + s.wx.rain * 0.45f + s.wx.wet * 0.15f, 0.f, 1.f));
    Vector3 camF = Vector3Normalize(Vector3Subtract(cam_.target, cam_.position));
    float cosLimit = std::cos((cam_.fovy * 0.5f * 1.9f + 8.f) * DEG2RAD);
    rlDrawRenderBatchActive();
    rlDisableDepthTest();  // the water shader does its own occlusion (see FS_WATER)
    rlDisableBackfaceCulling();
    for (auto& [k, c] : chunks_) {
        if (!c.water.vaoId) continue;
        Vector3 v = Vector3Subtract(c.center, cam_.position);
        float d = Vector3Length(v);
        float rad = c.radius + 220.f;  // lakes and river ribbons can reach past the chunk
        if (d > rad && Vector3DotProduct(v, camF) / d < cosLimit - rad / d) continue;
        DrawMesh(c.water, waterMat_, MatrixIdentity());
    }
    rlDrawRenderBatchActive();
    rlEnableDepthTest();
    rlEnableBackfaceCulling();
    rlActiveTextureSlot(12);
    rlDisableTexture();
    rlActiveTextureSlot(0);
}

// ------------------------------------------------------------------ frame
void Renderer::render(const SceneState& sIn, float dt) {
    if (!course_) return;
    // the weather sets the clouds; the rain the rider gets is what falls from the cloud overhead
    SceneState s = sIn;
    if (volEnabled() && s.wx.storm > 0.f) {
        // thunderstorms are towering cells with gaps between them, not a grey lid: ~80 % cover,
        // clearer air under and between the cells, so you can watch them light up inside
        float st = std::clamp(s.wx.storm, 0.f, 1.f);
        s.wx.cloud += (std::min(s.wx.cloud, 0.8f) - s.wx.cloud) * st;
        s.wx.fog *= 1.f - 0.7f * st;
        s.wx.dark *= 1.f - 0.35f * st;
    }
    {
        float tmin = course_->minElevation(), tmax = course_->maxElevation();
        if (!refInit_) { refMin_ = tmin; refMax_ = tmax; refInit_ = true; }
        float a = 1.f - std::exp(-dt / 20.f);
        refMin_ += (tmin - refMin_) * a;
        refMax_ += (tmax - refMax_) * a;
    }
    updateCloudParams(s, dt);
    if (volEnabled()) {
        float f = 0.12f + 0.88f * cloud_.here;
        s.wx.rain *= f; s.wx.snow *= f; s.wx.hail *= f;
        // the ground gets wet from the rain that falls here, and dries between showers
        if (wetLocal_ < 0) wetLocal_ = s.wx.wet;
        wetLocal_ = std::clamp(s.wx.rain > 0.05f ? wetLocal_ + dt * s.wx.rain / 35.f : wetLocal_ - dt / 160.f, 0.f, 1.f);
        s.wx.wet = wetLocal_;
        // what the rider hears: the rain here, and showers nearby, by side
        precipSound_.rain = s.wx.rain;
        precipSound_.hail = s.wx.hail;
        precipSound_.farL = precipSound_.farR = 0;
        if (cloud_.precipArea > 0.001f) {
            Vector3 f3 = Vector3Normalize(Vector3Subtract(cam_.target, cam_.position));
            Vector3 r3 = Vector3Normalize(Vector3CrossProduct(f3, {0, 1, 0}));
            auto p = course_->at(s.dist);
            float L = 0, R = 0;
            for (int k = 0; k < 12; ++k) {
                float a = k * 6.2832f / 12.f;
                Vector3 dir{std::sin(a), 0, std::cos(a)};
                float side = Vector3DotProduct(dir, r3);
                for (float rad : {700.f, 1600.f}) {
                    float c = precipCellAt(p.pos.x + dir.x * rad, p.pos.z + dir.z * rad);
                    float m = smooth(1.f - cloud_.precipArea - 0.06f, 1.f - cloud_.precipArea + 0.12f, c) * (rad < 1000.f ? 1.f : 0.55f);
                    L += m * (0.5f - 0.5f * side); R += m * (0.5f + 0.5f * side);
                }
            }
            float k = sIn.wx.rain * (1.f - 0.75f * cloud_.here) / 9.f;
            precipSound_.farL = std::min(1.f, L * k);
            precipSound_.farR = std::min(1.f, R * k);
        }
        // a strike lights the land in proportion to how close it is
        if (bolt_.t >= 0) {
            float d = Vector3Distance(bolt_.cloud, cam_.position);
            s.wx.flash *= 0.06f + 0.4f * (1.f - smooth(800.f, 4000.f, d));   // the glowing cloud does the rest
        }
    } else {
        precipSound_ = {s.wx.rain, s.wx.hail, 0, 0};
    }
    if (rtW_ != targetW() || rtH_ != targetH()) {
        destroyTargets();
        createTargets();
    }
    time_ = float(s.time);
    riderDist_ = s.dist;
    computeLighting(s);
    updateCamera(s, dt);
    updateRoad(s.dist);
    updateStructures();
    updateSunTerrain();
    streamWorld(cam_.position, s.dist, 3);
    updateLanterns(s, dt);
    if (s.wildlife) updateWildlife(s, dt);
    riderPos_ = Vector3Add(toV(course_->at(s.dist).pos), Vector3Scale(rightOf(course_->at(s.dist).heading), 1.4f));
    updateGrass();
    updateParticles(s, dt);
    if (bolt_.t >= 0) { bolt_.t += dt; if (bolt_.t > 0.8f) bolt_.t = -1; }
    cascadeRange_[0] = 40.f;
    cascadeRange_[1] = quality_ >= 1 ? 380.f : 250.f;

    rig_.pose({s.crankAngle, s.stand, s.tuck, s.effort, 0.f});
    if (greyOn_) poseGrey(s);
    if (s.pacerDist >= 0) ghostRig_.pose({s.time * 9.0, 0.f, 0.f, 0.5f, 0.f});

    prof::mark(prof::Update);
    renderShadowMaps(s);
    renderCloudShadow();
    prof::mark(prof::Shadows);

    BeginTextureMode(scene_);
    ClearBackground(BLACK);
    rlSetClipPlanes(0.3, farClip());
    BeginMode3D(cam_);
    rlActiveTextureSlot(10);
    rlEnableTexture(shadow_[0].depth.id);
    rlActiveTextureSlot(11);
    rlEnableTexture(shadow_[1].depth.id);
    rlActiveTextureSlot(13);
    rlEnableTexture(csm_.id ? csm_.texture.id : cloudMap_.id);
    rlActiveTextureSlot(14);
    rlEnableTexture(tex_.moor.id);   // terrain: moorland (material slots 0-11 are taken)
    if (tunnelTex_) { rlActiveTextureSlot(15); rlEnableTexture(tunnelTex_); }   // tunnel light map
    if (sunTex_) { rlActiveTextureSlot(9); rlEnableTexture(sunTex_); }         // terrain sun shadows
    rlActiveTextureSlot(0);
    setI(terrainSh_, "texMoor", 14);

    for (Shader sh : {terrainSh_, roadSh_, objectSh_, foliageSh_, waterSh_, skySh_, grassSh_, animalSh_, volumeSh_, kitSh_, impSh_}) {
        setCommonUniforms(sh);
        setF(sh, "wetness", s.wx.wet);
        setF(sh, "snowCover", s.wx.snowCover);
        setF(sh, "flash", s.wx.flash);
    }
    setI(terrainSh_, "tunnelSkip", 1);   // the land is never seen from inside a tube
    wind_ = 0.5f + s.wx.wind * 1.6f + s.wx.gust * 0.8f;
    static int tdebug = std::getenv("CORERAY_TDEBUG") ? std::atoi(std::getenv("CORERAY_TDEBUG")) : 0;
    setI(terrainSh_, "debugMode", tdebug);
    setV3(skySh_, "realSunDir", L_.sunDir);
    setV3(skySh_, "moonDir", L_.moonDir);
    setF(skySh_, "moonIllum", L_.moonIllum);
    setF(skySh_, "moonWaxing", L_.moonWaxing);
    setF(skySh_, "starAmt", L_.stars);
    setF(skySh_, "rainbow", s.wx.rainbow);
    setF(skySh_, "cloudDark", s.wx.dark);
    setF(skySh_, "sunDisk", L_.sunVisible);

    rlDisableBackfaceCulling();
    rlDisableDepthMask();
    DrawMesh(skyDome_, skyMat_, MatrixMultiply(MatrixScale(6000, 6000, 6000), MatrixTranslate(cam_.position.x, cam_.position.y, cam_.position.z)));
    rlEnableDepthMask();
    rlEnableBackfaceCulling();
    prof::mark(prof::Sky);

    drawWorld(false, 0, cam_.position, 0);   // marks Terrain, Road, Foliage, Props
    drawGrass(s);
    prof::mark(prof::Grass);
    drawLanterns(false, cam_.position, 0);
    if (s.wildlife) drawWildlife(false, cam_.position, 0);

    if (s.cam != CamMode::FirstPerson && !s.hideRider) drawRider(riderBase(s), s, false, false);
    if (s.pacerDist >= 0) {
        auto pp = course_->at(s.pacerDist);
        Vector3 ppos = Vector3Add(toV(pp.pos), Vector3Scale(rightOf(pp.heading), -0.2f));
        Matrix pb = MatrixMultiply(MatrixMultiply(MatrixRotateX(-std::atan(pp.grade)), MatrixRotateY(pp.heading)), MatrixTranslate(ppos.x, ppos.y, ppos.z));
        SceneState ps = s;
        ps.crankAngle = s.time * 9.0;
        ps.wheelAngle = std::fmod(s.pacerDist / WHEEL_ROLL_R, 2 * PI);
        drawRider(pb, ps, true, false);
    }
    if (s.wildlife) drawBirds(s);
    drawBolt();
    prof::mark(prof::Props);
    drawWater(s);
    prof::mark(prof::Water);

    rlActiveTextureSlot(10);
    rlDisableTexture();
    rlActiveTextureSlot(11);
    rlDisableTexture();
    rlActiveTextureSlot(13);
    rlDisableTexture();
    rlActiveTextureSlot(14);
    rlDisableTexture();
    rlActiveTextureSlot(15);
    rlDisableTexture();
    rlActiveTextureSlot(9);
    rlDisableTexture();
    rlActiveTextureSlot(0);
    if (volEnabled() && sceneFx_.id) {
        // fog, light cones, rain shafts and clouds over the opaque scene + water, then the
        // sprites (lamp glare, rain, snow) into the fogged copy, depth-tested against the scene
        EndMode3D();
        EndTextureMode();
        renderVolumetrics(s);
        prof::mark(prof::Volumetrics);
        BeginTextureMode(sceneFx_);
        BeginMode3D(cam_);
    }
    drawLightShafts();
    drawParticles(s);
    prof::mark(prof::Particles);
    EndMode3D();
    EndTextureMode();

    postProcess(s);
    prof::mark(prof::Post);
}
