#include "Rider.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

#include "raymath.h"
#include "rlgl.h"

namespace {

constexpr float PI_F = 3.14159265358979f;
constexpr float CRANK = 0.17f;
const Vector3 BB{0, 0.275f, 0};

// region codes for the kit shader (vertex colour .r)
enum Region { Jersey = 0, Accent = 1, Shorts = 2, Skin = 3, Shoe = 4, Sock = 5, Glove = 6, Sole = 7 };

struct Ring { Vector3 c; float rx, rz; };                       // ellipse in the plane across the path
struct Weight { int b0, b1; float w; };                          // w = weight of b0
using WeightFn = std::function<Weight(const Vector3& p)>;
using RegionFn = std::function<int(const Vector3& p, float u)>;

struct Builder {
    std::vector<float> pos, nrm, uv;
    std::vector<unsigned char> col, b0, b1;
    std::vector<float> w;
    std::vector<unsigned short> idx;
    void loft(const std::vector<Ring>& R, int sides, const WeightFn& wf, const RegionFn& rf) {
        const int n = int(R.size());
        unsigned short base = (unsigned short)(pos.size() / 3);
        for (int i = 0; i < n; ++i) {
            Vector3 t = Vector3Normalize(Vector3Subtract(R[std::min(n - 1, i + 1)].c, R[std::max(0, i - 1)].c));
            Vector3 X{1, 0, 0};
            Vector3 right = Vector3Normalize(Vector3Subtract(X, Vector3Scale(t, Vector3DotProduct(X, t))));
            Vector3 up = Vector3CrossProduct(t, right);
            float ds = std::max(1e-4f, Vector3Distance(R[std::min(n - 1, i + 1)].c, R[std::max(0, i - 1)].c));
            float drx = (R[std::min(n - 1, i + 1)].rx - R[std::max(0, i - 1)].rx) / ds;
            float drz = (R[std::min(n - 1, i + 1)].rz - R[std::max(0, i - 1)].rz) / ds;
            for (int j = 0; j <= sides; ++j) {
                float a = 2 * PI_F * j / sides;
                float ca = std::cos(a), sa = std::sin(a);
                Vector3 p = Vector3Add(R[i].c, Vector3Add(Vector3Scale(right, ca * R[i].rx), Vector3Scale(up, sa * R[i].rz)));
                Vector3 nn = Vector3Normalize(Vector3Add(Vector3Scale(right, ca / std::max(1e-3f, R[i].rx)), Vector3Scale(up, sa / std::max(1e-3f, R[i].rz))));
                nn = Vector3Normalize(Vector3Subtract(nn, Vector3Scale(t, ca * ca * drx + sa * sa * drz)));
                Weight wt = wf(p);
                pos.insert(pos.end(), {p.x, p.y, p.z});
                nrm.insert(nrm.end(), {nn.x, nn.y, nn.z});
                uv.insert(uv.end(), {float(j) / sides, float(i) / (n - 1)});
                col.insert(col.end(), {(unsigned char)rf(p, float(j) / sides), 255, 255, 255});
                b0.push_back((unsigned char)wt.b0);
                b1.push_back((unsigned char)wt.b1);
                w.push_back(wt.w);
            }
        }
        for (int i = 0; i < n - 1; ++i)
            for (int j = 0; j < sides; ++j) {
                unsigned short a = base + i * (sides + 1) + j, a1 = a + 1, c = base + (i + 1) * (sides + 1) + j, c1 = c + 1;
                idx.insert(idx.end(), {a, a1, c, a1, c1, c});
            }
    }
};

float sstep(float a, float b, float x) { float t = std::clamp((x - a) / (b - a), 0.f, 1.f); return t * t * (3 - 2 * t); }


struct Basis { Vector3 x, y, z; };
Basis basisOf(Vector3 head, Vector3 tail) {
    Vector3 y = Vector3Normalize(Vector3Subtract(tail, head));
    Vector3 X{1, 0, 0};
    Vector3 x = Vector3Subtract(X, Vector3Scale(y, Vector3DotProduct(X, y)));
    if (Vector3Length(x) < 1e-4f) x = {0, 0, 1};
    x = Vector3Normalize(x);
    Vector3 z = Vector3CrossProduct(x, y);
    return {x, y, z};
}

Vector3 ik(Vector3 root, Vector3 end, float l1, float l2, Vector3 pole) {
    Vector3 d = Vector3Subtract(end, root);
    float dist = std::clamp(Vector3Length(d), 0.05f, l1 + l2 - 0.002f);
    Vector3 dir = Vector3Normalize(d);
    float a = (l1 * l1 - l2 * l2 + dist * dist) / (2 * dist);
    float h = std::sqrt(std::max(0.f, l1 * l1 - a * a));
    Vector3 p = Vector3Normalize(Vector3Subtract(pole, Vector3Scale(dir, Vector3DotProduct(pole, dir))));
    return Vector3Add(Vector3Add(root, Vector3Scale(dir, a)), Vector3Scale(p, h));
}

}  // namespace

Vector3 RiderRig::pedal(double crank, int side) {
    double a = crank + (side ? PI_F : 0.0);
    float x = side == 0 ? -0.095f : 0.095f;
    // forward pedalling: top -> front -> bottom -> back
    return {x, BB.y - float(std::cos(a)) * CRANK, BB.z - float(std::sin(a)) * CRANK};
}

void RiderRig::build() {
    if (built_) return;
    // ---------------- bind skeleton (standing, metres; +x = rider's left, +z forward)
    auto setB = [&](Bone b, Vector3 h, Vector3 t) { bindHead_[b] = h; bindTail_[b] = t; };
    setB(Pelvis, {0, 0.96f, 0}, {0, 1.10f, -0.01f});
    setB(Spine, {0, 1.10f, -0.01f}, {0, 1.30f, -0.01f});
    setB(Chest, {0, 1.30f, -0.01f}, {0, 1.47f, 0});
    setB(Neck, {0, 1.47f, 0}, {0, 1.60f, 0.02f});
    setB(Head, {0, 1.60f, 0.02f}, {0, 1.78f, 0});
    for (int s = 0; s < 2; ++s) {
        float x = s == 0 ? 0.09f : -0.09f;
        Bone th = s == 0 ? ThighL : ThighR, sh = s == 0 ? ShinL : ShinR, ft = s == 0 ? FootL : FootR;
        setB(th, {x, 0.93f, 0}, {x, 0.50f, 0.02f});
        setB(sh, {x, 0.50f, 0.02f}, {x, 0.085f, 0});
        setB(ft, {x, 0.085f, 0}, {x, 0.02f, 0.17f});
        float ax = s == 0 ? 0.185f : -0.185f;
        Bone up = s == 0 ? UpperL : UpperR, fo = s == 0 ? ForeL : ForeR, ha = s == 0 ? HandL : HandR;
        setB(up, {ax, 1.43f, 0}, {ax * 1.03f, 1.13f, 0});
        setB(fo, {ax * 1.03f, 1.13f, 0}, {ax * 1.03f, 0.87f, 0.02f});
        setB(ha, {ax * 1.03f, 0.87f, 0.02f}, {ax * 1.03f, 0.70f, 0.03f});
    }

    Builder B;
    // ---------------- torso (pelvis .. neck), bib shorts below the waist, jersey above
    {
        struct S { float y, rx, rz, z; };
        const S sec[] = {{0.835f, 0.05f, 0.04f, -0.02f}, {0.86f, 0.13f, 0.09f, -0.02f}, {0.90f, 0.165f, 0.115f, -0.025f}, {0.96f, 0.172f, 0.12f, -0.025f},
                         {1.02f, 0.158f, 0.105f, -0.012f}, {1.10f, 0.146f, 0.095f, 0.f}, {1.18f, 0.154f, 0.1f, 0.004f}, {1.26f, 0.168f, 0.112f, 0.008f},
                         {1.33f, 0.178f, 0.118f, 0.008f}, {1.39f, 0.186f, 0.108f, 0.f}, {1.435f, 0.175f, 0.09f, -0.01f}, {1.465f, 0.1f, 0.065f, -0.005f},
                         {1.49f, 0.058f, 0.058f, 0.005f}, {1.55f, 0.052f, 0.054f, 0.012f}, {1.585f, 0.03f, 0.03f, 0.015f}};
        std::vector<Ring> R;
        for (auto& s : sec) R.push_back({{0, s.y, s.z}, s.rx, s.rz});
        B.loft(R, 28,
               [](const Vector3& p) {
                   if (p.y < 1.02f) return Weight{Pelvis, Spine, 1.f - sstep(0.98f, 1.10f, p.y)};
                   if (p.y < 1.24f) return Weight{Spine, Pelvis, sstep(0.98f, 1.10f, p.y)};
                   if (p.y < 1.44f) return Weight{Chest, Spine, sstep(1.24f, 1.34f, p.y)};
                   return Weight{Neck, Chest, sstep(1.44f, 1.5f, p.y)};
               },
               [](const Vector3& p, float u) {
                   if (p.y < 1.0f) return int(Shorts);
                   if (p.y > 1.47f) return int(Skin);
                   if (std::fabs(u - 0.25f) < 0.05f || std::fabs(u - 0.75f) < 0.05f) return int(Accent);  // side panels
                   if (p.y > 1.31f && p.y < 1.36f) return int(Accent);                                     // chest band
                   return int(Jersey);
               });
    }
    // ---------------- head (jaw, face, crown) - the helmet sits on top
    {
        std::vector<Ring> R = {{{0, 1.545f, 0.012f}, 0.05f, 0.05f}, {{0, 1.575f, 0.03f}, 0.062f, 0.072f}, {{0, 1.615f, 0.03f}, 0.07f, 0.088f},
                               {{0, 1.66f, 0.02f}, 0.077f, 0.096f}, {{0, 1.705f, 0.01f}, 0.075f, 0.094f}, {{0, 1.745f, 0.0f}, 0.058f, 0.072f},
                               {{0, 1.775f, -0.005f}, 0.02f, 0.025f}};
        B.loft(R, 20, [](const Vector3& p) { return Weight{Head, Neck, sstep(1.55f, 1.6f, p.y)}; }, [](const Vector3&, float) { return int(Skin); });
        // nose
        std::vector<Ring> N = {{{0, 1.655f, 0.105f}, 0.012f, 0.012f}, {{0, 1.64f, 0.12f}, 0.014f, 0.012f}, {{0, 1.63f, 0.126f}, 0.004f, 0.004f}};
        B.loft(N, 8, [](const Vector3&) { return Weight{Head, Head, 1}; }, [](const Vector3&, float) { return int(Skin); });
    }
    // ---------------- legs: shorts to mid-thigh, skin, socks; shoes
    for (int s = 0; s < 2; ++s) {
        float x = s == 0 ? 0.09f : -0.09f;
        Bone th = s == 0 ? ThighL : ThighR, sh = s == 0 ? ShinL : ShinR, ft = s == 0 ? FootL : FootR;
        struct S { float y, rx, rz, z; };
        const S sec[] = {{0.985f, 0.08f, 0.08f, 0.f}, {0.95f, 0.088f, 0.092f, 0.f}, {0.88f, 0.084f, 0.088f, 0.005f}, {0.80f, 0.076f, 0.08f, 0.01f},
                         {0.70f, 0.066f, 0.07f, 0.014f}, {0.61f, 0.057f, 0.06f, 0.017f}, {0.55f, 0.05f, 0.053f, 0.02f}, {0.50f, 0.048f, 0.05f, 0.022f},
                         {0.45f, 0.05f, 0.054f, 0.012f}, {0.38f, 0.056f, 0.06f, 0.f}, {0.30f, 0.05f, 0.052f, -0.004f}, {0.22f, 0.04f, 0.04f, 0.f},
                         {0.15f, 0.033f, 0.035f, 0.f}, {0.10f, 0.032f, 0.034f, 0.f}, {0.07f, 0.02f, 0.02f, 0.f}};
        std::vector<Ring> R;
        for (auto& q : sec) R.push_back({{x, q.y, q.z}, q.rx, q.rz});
        B.loft(R, 16, [=](const Vector3& p) { return Weight{th, sh, sstep(0.47f, 0.54f, p.y)}; },
               [](const Vector3& p, float) { return p.y > 0.62f ? int(Shorts) : p.y < 0.17f ? int(Sock) : int(Skin); });
        // shoe from heel to toe
        std::vector<Ring> F = {{{x, 0.06f, -0.085f}, 0.01f, 0.01f}, {{x, 0.065f, -0.07f}, 0.04f, 0.045f}, {{x, 0.07f, -0.01f}, 0.046f, 0.052f},
                               {{x, 0.055f, 0.07f}, 0.05f, 0.04f}, {{x, 0.045f, 0.14f}, 0.044f, 0.03f}, {{x, 0.04f, 0.185f}, 0.026f, 0.018f},
                               {{x, 0.038f, 0.198f}, 0.005f, 0.005f}};
        B.loft(F, 14, [=](const Vector3& p) { return Weight{ft, sh, p.z < -0.04f ? 0.75f : 1.f}; },
               [](const Vector3& p, float u) { return (std::fabs(u - 0.75f) < 0.17f && p.z > -0.06f) ? int(Sole) : int(Shoe); });
    }
    // ---------------- arms: short sleeves, skin, gloves
    for (int s = 0; s < 2; ++s) {
        float ax = s == 0 ? 0.185f : -0.185f;
        Bone up = s == 0 ? UpperL : UpperR, fo = s == 0 ? ForeL : ForeR, ha = s == 0 ? HandL : HandR;
        struct S { float y, r1, r2, z; };
        const S sec[] = {{1.47f, 0.03f, 0.03f, 0.f}, {1.45f, 0.058f, 0.058f, 0.f}, {1.39f, 0.055f, 0.055f, 0.f}, {1.30f, 0.048f, 0.05f, 0.f},
                         {1.22f, 0.045f, 0.046f, 0.f}, {1.16f, 0.039f, 0.042f, 0.f}, {1.12f, 0.037f, 0.04f, 0.002f}, {1.05f, 0.041f, 0.039f, 0.008f},
                         {0.97f, 0.035f, 0.031f, 0.014f}, {0.89f, 0.027f, 0.022f, 0.02f}, {0.875f, 0.024f, 0.02f, 0.02f}};
        std::vector<Ring> R;
        for (auto& q : sec) R.push_back({{ax * (1.f + (1.43f - std::min(1.43f, q.y)) * 0.1f), q.y, q.z}, q.r1, q.r2});
        B.loft(R, 14,
               [=](const Vector3& p) {
                   if (p.y > 1.42f) return Weight{up, Chest, 1.f - 0.5f * sstep(1.42f, 1.48f, p.y)};
                   return Weight{up, fo, sstep(1.09f, 1.16f, p.y)};
               },
               [](const Vector3& p, float) { return p.y > 1.24f ? int(Jersey) : p.y < 0.9f ? int(Glove) : int(Skin); });
        // gloved hand: a flattened mitten with a thumb bump
        std::vector<Ring> H = {{{ax * 1.03f, 0.885f, 0.02f}, 0.026f, 0.022f}, {{ax * 1.03f, 0.84f, 0.025f}, 0.042f, 0.02f}, {{ax * 1.03f, 0.78f, 0.03f}, 0.044f, 0.018f},
                               {{ax * 1.03f, 0.72f, 0.03f}, 0.036f, 0.014f}, {{ax * 1.03f, 0.695f, 0.03f}, 0.01f, 0.008f}};
        B.loft(H, 10, [=](const Vector3&) { return Weight{ha, ha, 1}; }, [](const Vector3&, float) { return int(Glove); });
    }

    // ---------------- mesh (dynamic: positions + normals are rewritten every frame)
    const int nv = int(B.pos.size() / 3);
    bindPos_ = B.pos;
    bindNrm_ = B.nrm;
    outPos_ = B.pos;
    outNrm_ = B.nrm;
    b0_ = B.b0;
    b1_ = B.b1;
    w_ = B.w;
    Mesh m{};
    m.vertexCount = nv;
    m.triangleCount = int(B.idx.size() / 3);
    m.vertices = (float*)MemAlloc(sizeof(float) * B.pos.size());
    std::memcpy(m.vertices, B.pos.data(), sizeof(float) * B.pos.size());
    m.normals = (float*)MemAlloc(sizeof(float) * B.nrm.size());
    std::memcpy(m.normals, B.nrm.data(), sizeof(float) * B.nrm.size());
    m.texcoords = (float*)MemAlloc(sizeof(float) * B.uv.size());
    std::memcpy(m.texcoords, B.uv.data(), sizeof(float) * B.uv.size());
    m.colors = (unsigned char*)MemAlloc(B.col.size());
    std::memcpy(m.colors, B.col.data(), B.col.size());
    m.indices = (unsigned short*)MemAlloc(sizeof(unsigned short) * B.idx.size());
    std::memcpy(m.indices, B.idx.data(), sizeof(unsigned short) * B.idx.size());
    UploadMesh(&m, true);
    mesh_ = m;
    for (auto& s : skin_) s = MatrixIdentity();
    built_ = true;
}

void RiderRig::unload() {
    if (built_ && mesh_.vaoId) UnloadMesh(mesh_);
    mesh_ = Mesh{};
    built_ = false;
}

void RiderRig::pose(const RiderPose& p) {
    if (!built_) return;
    Vector3 head[COUNT], tail[COUNT];
    const float st = p.stand, tk = p.tuck * (1 - p.stand);
    const float rock = std::sin(float(p.crank) * 2.f) * 0.012f * (0.3f + p.effort) * (1 + st * 2);
    // ---- pelvis on the saddle (or up and forward when standing), shoulders towards the bars
    Vector3 hip = Vector3Lerp({0, 0.995f, -0.175f}, {0, 1.13f, 0.05f}, st);
    hip.x = rock * 0.6f;
    Vector3 neckBase = Vector3Lerp({rock, 1.38f, 0.22f}, {rock * 2, 1.45f, 0.33f}, st);
    neckBase = Vector3Lerp(neckBase, {rock, 1.23f, 0.33f}, tk);
    Vector3 lumbar = Vector3Add(Vector3Lerp(hip, neckBase, 0.26f), {0, 0.045f, -0.01f});
    Vector3 chest = Vector3Add(Vector3Lerp(hip, neckBase, 0.64f), {0, 0.055f, 0});
    head[Pelvis] = hip; tail[Pelvis] = lumbar;
    head[Spine] = lumbar; tail[Spine] = chest;
    head[Chest] = chest; tail[Chest] = neckBase;
    Vector3 skull = Vector3Add(neckBase, Vector3Lerp({0, 0.09f, 0.10f}, {0, 0.07f, 0.12f}, tk));
    head[Neck] = neckBase; tail[Neck] = skull;
    head[Head] = skull;
    tail[Head] = Vector3Add(skull, {std::sin(p.look) * 0.04f, 0.16f, -0.05f + 0.03f * tk});
    // ---- legs: ball of the foot on the pedal, ankle angle varying through the stroke
    for (int s = 0; s < 2; ++s) {
        Bone th = s == 0 ? ThighR : ThighL, sh = s == 0 ? ShinR : ShinL, ft = s == 0 ? FootR : FootL;
        float sx = s == 0 ? -1.f : 1.f;
        double a = p.crank + (s ? PI_F : 0.0);
        Vector3 ped = pedal(p.crank, s);
        Vector3 ball = Vector3Add(ped, {sx * 0.005f, 0.035f, 0});
        float theta = 0.32f + 0.18f * float(std::sin(a + 0.6));  // toe drops through the back of the stroke
        Vector3 fdir{0, -std::sin(theta), std::cos(theta)};
        Vector3 ankle = Vector3Subtract(ball, Vector3Scale(fdir, 0.135f));
        ankle.y += 0.045f;
        Vector3 hipJ = Vector3Add(hip, {sx * 0.09f, -0.03f, 0});
        Vector3 knee = ik(hipJ, ankle, 0.4305f, 0.4155f, {sx * 0.12f, 0.15f, 1.f});
        // if the ankle is out of reach the IK straightens; put the ankle where the shin ends
        Vector3 kd = Vector3Normalize(Vector3Subtract(ankle, knee));
        ankle = Vector3Add(knee, Vector3Scale(kd, 0.4155f));
        head[th] = hipJ; tail[th] = knee;
        head[sh] = knee; tail[sh] = ankle;
        head[ft] = ankle; tail[ft] = Vector3Add(ankle, Vector3Scale(Vector3Normalize(Vector3Add(fdir, {0, 0.2f, 0})), 0.18f));
    }
    // ---- arms: hands on the hoods (or the drops)
    for (int s = 0; s < 2; ++s) {
        Bone up = s == 0 ? UpperR : UpperL, fo = s == 0 ? ForeR : ForeL, ha = s == 0 ? HandR : HandL;
        float sx = s == 0 ? -1.f : 1.f;
        Vector3 sh = Vector3Add(neckBase, {sx * 0.18f, -0.045f, -0.02f});
        Vector3 grip = Vector3Lerp({sx * 0.21f, 0.955f, 0.64f}, {sx * 0.21f, 0.80f, 0.575f}, tk);
        Vector3 wrist = Vector3Add(grip, Vector3Lerp({0, 0.045f, -0.06f}, {0, 0.06f, -0.04f}, tk));
        Vector3 elbow = ik(sh, wrist, 0.30f, 0.262f, {sx * 0.7f, -0.55f, -0.35f});
        Vector3 wd = Vector3Normalize(Vector3Subtract(wrist, elbow));
        wrist = Vector3Add(elbow, Vector3Scale(wd, 0.262f));
        head[up] = sh; tail[up] = elbow;
        head[fo] = elbow; tail[fo] = wrist;
        head[ha] = wrist; tail[ha] = Vector3Add(grip, Vector3Lerp({0, -0.01f, 0.07f}, {0, -0.06f, 0.05f}, tk));
    }
    // ---- skin matrices: bind frame -> posed frame (with stretch along the bone)
    struct Xf { float r[9]; Vector3 hb, hc; };
    Xf X[COUNT];
    for (int b = 0; b < COUNT; ++b) {
        Basis fb = basisOf(bindHead_[b], bindTail_[b]), fc = basisOf(head[b], tail[b]);
        float lb = Vector3Distance(bindHead_[b], bindTail_[b]), lc = Vector3Distance(head[b], tail[b]);
        float sy = std::clamp(lc / std::max(1e-4f, lb), 0.7f, 1.4f);
        // R = Fc * diag(1, sy, 1) * Fb^T
        Vector3 cx = fc.x, cy = Vector3Scale(fc.y, sy), cz = fc.z;
        const Vector3 bx = fb.x, by = fb.y, bz = fb.z;
        float* r = X[b].r;
        auto rowCol = [&](int i, int j) {
            float ci[3] = {cx.x, cx.y, cx.z}, cyi[3] = {cy.x, cy.y, cy.z}, czi[3] = {cz.x, cz.y, cz.z};
            float bj[3] = {bx.x, bx.y, bx.z}, byj[3] = {by.x, by.y, by.z}, bzj[3] = {bz.x, bz.y, bz.z};
            return ci[i] * bj[j] + cyi[i] * byj[j] + czi[i] * bzj[j];
        };
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) r[i * 3 + j] = rowCol(i, j);
        X[b].hb = bindHead_[b];
        X[b].hc = head[b];
        Vector3 t{head[b].x - (r[0] * bindHead_[b].x + r[1] * bindHead_[b].y + r[2] * bindHead_[b].z),
                  head[b].y - (r[3] * bindHead_[b].x + r[4] * bindHead_[b].y + r[5] * bindHead_[b].z),
                  head[b].z - (r[6] * bindHead_[b].x + r[7] * bindHead_[b].y + r[8] * bindHead_[b].z)};
        Matrix m{};
        m.m0 = r[0]; m.m4 = r[1]; m.m8 = r[2]; m.m12 = t.x;
        m.m1 = r[3]; m.m5 = r[4]; m.m9 = r[5]; m.m13 = t.y;
        m.m2 = r[6]; m.m6 = r[7]; m.m10 = r[8]; m.m14 = t.z;
        m.m15 = 1;
        skin_[b] = m;
    }
    // ---- skin
    const int nv = int(bindPos_.size() / 3);
    for (int i = 0; i < nv; ++i) {
        const float* bp = &bindPos_[i * 3];
        const float* bn = &bindNrm_[i * 3];
        float px = 0, py = 0, pz = 0, nx = 0, ny = 0, nz = 0;
        auto acc = [&](int b, float wt) {
            if (wt <= 0) return;
            const Xf& x = X[b];
            float dx = bp[0] - x.hb.x, dy = bp[1] - x.hb.y, dz = bp[2] - x.hb.z;
            const float* r = x.r;
            px += wt * (r[0] * dx + r[1] * dy + r[2] * dz + x.hc.x);
            py += wt * (r[3] * dx + r[4] * dy + r[5] * dz + x.hc.y);
            pz += wt * (r[6] * dx + r[7] * dy + r[8] * dz + x.hc.z);
            nx += wt * (r[0] * bn[0] + r[1] * bn[1] + r[2] * bn[2]);
            ny += wt * (r[3] * bn[0] + r[4] * bn[1] + r[5] * bn[2]);
            nz += wt * (r[6] * bn[0] + r[7] * bn[1] + r[8] * bn[2]);
        };
        acc(b0_[i], w_[i]);
        acc(b1_[i], 1.f - w_[i]);
        float nl = std::sqrt(nx * nx + ny * ny + nz * nz) + 1e-6f;
        outPos_[i * 3] = px; outPos_[i * 3 + 1] = py; outPos_[i * 3 + 2] = pz;
        outNrm_[i * 3] = nx / nl; outNrm_[i * 3 + 1] = ny / nl; outNrm_[i * 3 + 2] = nz / nl;
    }
    UpdateMeshBuffer(mesh_, 0, outPos_.data(), int(outPos_.size() * sizeof(float)), 0);
    UpdateMeshBuffer(mesh_, 2, outNrm_.data(), int(outNrm_.size() * sizeof(float)), 0);
}
