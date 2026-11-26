#include "body/Body.h"

#include <algorithm>
#include <cstdio>
#include <thread>
#include <unordered_map>

namespace body {

// ================================================================ quaternions
Quat qmul(Quat a, Quat b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Quat qconj(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }
Quat qnorm(Quat q) {
    float l = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 0 ? Quat{q.x / l, q.y / l, q.z / l, q.w / l} : Quat{};
}
V3 qrot(Quat q, V3 v) {
    V3 u{q.x, q.y, q.z};
    V3 t = cross(u, v) * 2.f;
    return v + t * q.w + cross(u, t);
}
Quat qaxis(V3 a, float ang) {
    a = norm(a);
    float s = std::sin(ang * 0.5f);
    return {a.x * s, a.y * s, a.z * s, std::cos(ang * 0.5f)};
}
Quat qbetween(V3 f, V3 t) {
    f = norm(f); t = norm(t);
    float d = dot(f, t);
    if (d < -0.999999f) {
        V3 ax = cross({1, 0, 0}, f);
        if (len(ax) < 1e-4f) ax = cross({0, 1, 0}, f);
        return qaxis(ax, 3.14159265f);
    }
    V3 c = cross(f, t);
    return qnorm({c.x, c.y, c.z, 1.f + d});
}
Quat qbasis(V3 x, V3 y, V3 z) {
    float m00 = x.x, m01 = y.x, m02 = z.x, m10 = x.y, m11 = y.y, m12 = z.y, m20 = x.z, m21 = y.z, m22 = z.z;
    float tr = m00 + m11 + m22;
    Quat q;
    if (tr > 0) {
        float s = std::sqrt(tr + 1.f) * 2;
        q = {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s};
    } else if (m00 > m11 && m00 > m22) {
        float s = std::sqrt(1.f + m00 - m11 - m22) * 2;
        q = {0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
    } else if (m11 > m22) {
        float s = std::sqrt(1.f + m11 - m00 - m22) * 2;
        q = {(m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s};
    } else {
        float s = std::sqrt(1.f + m22 - m00 - m11) * 2;
        q = {(m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s};
    }
    return qnorm(q);
}
Quat qslerp(Quat a, Quat b, float t) {
    float d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0) { b = {-b.x, -b.y, -b.z, -b.w}; d = -d; }
    if (d > 0.9995f) return qnorm({a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t});
    float th = std::acos(d), s = std::sin(th);
    float wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
    return {a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb, a.w * wa + b.w * wb};
}

// ================================================================ bones
namespace {
const char* NAMES[BONES] = {"root", "pelvis", "spine", "chest", "neck", "head",
                            "clavicle.L", "upper_arm.L", "forearm.L", "hand.L", "fingers.L", "thumb.L",
                            "clavicle.R", "upper_arm.R", "forearm.R", "hand.R", "fingers.R", "thumb.R",
                            "thigh.L", "shin.L", "foot.L", "toes.L", "thigh.R", "shin.R", "foot.R", "toes.R"};
const int PARENT[BONES] = {-1, Root, Pelvis, Spine, Chest, Neck,
                           Chest, ClavicleL, UpperArmL, ForearmL, HandL, HandL,
                           Chest, ClavicleR, UpperArmR, ForearmR, HandR, HandR,
                           Pelvis, ThighL, ShinL, FootL, Pelvis, ThighR, ShinR, FootR};
float smin(float a, float b, float k) {
    if (k <= 0) return std::min(a, b);
    float h = std::max(k - std::fabs(a - b), 0.f) / k;
    return std::min(a, b) - h * h * k * 0.25f;
}
float sstep(float a, float b, float x) { float t = std::clamp((x - a) / (b - a), 0.f, 1.f); return t * t * (3 - 2 * t); }
const float PI = 3.14159265f;
}  // namespace

const char* boneName(int b) { return b >= 0 && b < BONES ? NAMES[b] : "?"; }
int boneParent(int b) { return PARENT[b]; }
int boneSide(int b) {
    if ((b >= ClavicleL && b <= ThumbL) || (b >= ThighL && b <= ToesL)) return 1;
    if ((b >= ClavicleR && b <= ThumbR) || (b >= ThighR && b <= ToesR)) return -1;
    return 0;
}
int boneMirror(int b) {
    if (b >= ClavicleL && b <= ThumbL) return b + 6;
    if (b >= ClavicleR && b <= ThumbR) return b - 6;
    if (b >= ThighL && b <= ToesL) return b + 4;
    if (b >= ThighR && b <= ToesR) return b - 4;
    return b;
}

// ================================================================ field
float Body::primDist(const Prim& pr, V3 p) const {
    if (pr.type == Prim::Ellipsoid) {
        V3 d = p - pr.a;
        V3 q{dot(d, pr.ax) / pr.r.x, dot(d, pr.ay) / pr.r.y, dot(d, pr.az) / pr.r.z};
        V3 q2{q.x / pr.r.x, q.y / pr.r.y, q.z / pr.r.z};
        float k0 = len(q), k1 = len(q2);
        return k1 > 1e-9f ? k0 * (k0 - 1.f) / k1 : -std::min({pr.r.x, pr.r.y, pr.r.z});
    }
    // round cone (Inigo Quilez)
    V3 ba = pr.b - pr.a;
    float l2 = dot(ba, ba), rr = pr.ra - pr.rb, a2 = l2 - rr * rr, il2 = 1.f / l2;
    V3 pa = p - pr.a;
    float y = dot(pa, ba), z = y - l2;
    V3 xv = pa * l2 - ba * y;
    float x2 = dot(xv, xv), y2 = y * y * l2, z2 = z * z * l2;
    float k = (rr > 0 ? 1.f : rr < 0 ? -1.f : 0.f) * rr * rr * x2;
    if ((z > 0 ? 1.f : -1.f) * a2 * z2 > k) return std::sqrt(x2 + z2) * il2 - pr.rb;
    if ((y > 0 ? 1.f : -1.f) * a2 * y2 < k) return std::sqrt(x2 + y2) * il2 - pr.ra;
    return (std::sqrt(x2 * a2 * il2) + y * rr) * il2 - pr.ra;
}

float Body::sdf(V3 p) const {
    // each group is smoothly unioned within itself; every limb blends into the torso but
    // never into another limb (no webbing between the thighs or arm to leg)
    float g[5] = {1e9f, 1e9f, 1e9f, 1e9f, 1e9f};
    for (const Prim& pr : prims) {
        float& acc = g[pr.group];
        float lb = len(p - pr.bc) - pr.br;
        if (lb > acc + pr.k) continue;                 // can't change the union
        acc = smin(acc, primDist(pr, p), pr.k);
    }
    float d = g[0];
    for (int i = 1; i < 5; ++i) d = std::min(d, smin(g[0], g[i], groupK[i]));
    return d;
}

// ================================================================ build
namespace {

void finishPrim(Prim& p) {
    if (p.type == Prim::Ellipsoid) { p.bc = p.a; p.br = std::max({p.r.x, p.r.y, p.r.z}); }
    else { p.bc = (p.a + p.b) * 0.5f; p.br = len(p.b - p.a) * 0.5f + std::max(p.ra, p.rb); }
}

// rest frame for a bone: y along it, z towards `ref` (front, or up for bones that point forward)
Quat frameFor(V3 head, V3 tail) {
    V3 y = norm(tail - head);
    V3 ref = std::fabs(y.z) > 0.8f ? V3{0, 1, 0} : V3{0, 0, 1};
    V3 x = norm(cross(y, ref));
    V3 z = cross(x, y);
    return qbasis(x, y, z);
}

struct Builder {
    Body& B;
    float H, g, m;
    explicit Builder(Body& b) : B(b), H(b.params.height), g(b.params.girth), m(b.params.muscle) {}
    // ellipsoid in a bone's frame: centre = head + along·y + side·x + fwd·z (fractions of H), radii (fractions of H)
    void ell(int bone, int group, float k, V3 centre, V3 r, Quat frame, float mus = 0.f) {
        Prim p;
        p.type = Prim::Ellipsoid;
        p.bone = bone; p.group = group; p.k = k * H;
        p.a = centre;
        float s = g * (1.f + mus * (m - 1.f));
        p.r = r * (H * s);
        p.ax = qrot(frame, {1, 0, 0}); p.ay = qrot(frame, {0, 1, 0}); p.az = qrot(frame, {0, 0, 1});
        finishPrim(p);
        B.prims.push_back(p);
    }
    void cone(int bone, int group, float k, V3 a, V3 b, float ra, float rb) {
        Prim p;
        p.type = Prim::RoundCone;
        p.bone = bone; p.group = group; p.k = k * H;
        p.a = a; p.b = b; p.ra = ra * H * g; p.rb = rb * H * g;
        finishPrim(p);
        B.prims.push_back(p);
    }
    V3 at(int bone, float along, float side = 0, float fwd = 0) const {   // a point in a bone's frame (fractions of H)
        const BoneRest& r = B.skeleton.bones[bone];
        return r.head + qrot(r.rot, V3{side * H, along * r.length, fwd * H});
    }
    Quat fr(int bone) const { return B.skeleton.bones[bone].rot; }
};

void buildSkeleton(Body& B) {
    const Params& P = B.params;
    const float H = P.height;
    auto& bn = B.skeleton.bones;
    // vertical landmarks as fractions of the height (adult male, after Drillis & Contini)
    const float ankleY = 0.039f, legLen = 0.491f * P.legs;
    const float hipY = ankleY + legLen;
    // the torso and head fill what the legs leave, keeping the stature
    auto Y = [&](float f) { return (hipY + (f - 0.53f) * (1.f - hipY) / (1.f - 0.53f)) * H; };
    const float kneeY = ankleY + legLen * (0.246f / 0.491f);
    auto set = [&](int b, V3 h, V3 t) { bn[b].head = h; bn[b].tail = t; };
    set(Root, {0, 0, 0}, {0, 0.1f * H, 0});
    set(Pelvis, {0, Y(0.535f), -0.004f * H}, {0, Y(0.60f), -0.006f * H});
    set(Spine, bn[Pelvis].tail, {0, Y(0.68f), -0.008f * H});
    set(Chest, bn[Spine].tail, {0, Y(0.805f), -0.006f * H});
    set(Neck, bn[Chest].tail, {0, Y(0.87f), 0.004f * H});
    set(Head, bn[Neck].tail, {0, Y(1.f), 0.0f});
    // arms in an A-pose: 50 degrees out from hanging
    const float armA = 50.f * PI / 180.f;
    for (int s = 0; s < 2; ++s) {
        float sx = s == 0 ? 1.f : -1.f;
        int o = s == 0 ? 0 : 6;
        V3 sternum{sx * 0.012f * H, Y(0.81f), 0.012f * H};
        V3 shoulder{sx * 0.109f * H * P.shoulders, Y(0.808f), -0.006f * H};
        V3 dir = norm(V3{sx * std::sin(armA), -std::cos(armA), 0.f});
        float ua = 0.186f * H * P.arms, fa = 0.146f * H * P.arms, palm = 0.058f * H * P.arms, fing = 0.05f * H * P.arms;
        V3 elbow = shoulder + dir * ua, wrist = elbow + dir * fa, knuck = wrist + dir * palm;
        set(ClavicleL + o, sternum, shoulder);
        set(UpperArmL + o, shoulder, elbow);
        set(ForearmL + o, elbow, wrist);
        set(HandL + o, wrist, knuck);
        set(FingersL + o, knuck, knuck + dir * fing);
        // thumb: from the base of the palm, forwards and along
        V3 tb = wrist + dir * (0.012f * H) + V3{0, 0, 0.012f * H};
        set(ThumbL + o, tb, tb + norm(dir + V3{0, 0, 1.1f}) * (0.04f * H * P.arms));
    }
    for (int s = 0; s < 2; ++s) {
        float sx = s == 0 ? 1.f : -1.f;
        int o = s == 0 ? 0 : 4;
        V3 hip{sx * 0.051f * H * P.hips, hipY * H, 0};
        V3 knee{sx * 0.054f * H * P.hips, kneeY * H, 0.004f * H};
        V3 ankle{sx * 0.058f * H * P.hips, ankleY * H, -0.004f * H};
        V3 ball = ankle + V3{sx * 0.006f * H, -0.027f * H, 0.083f * H};
        V3 tip = ball + V3{sx * 0.002f * H, -0.002f * H, 0.036f * H};
        set(ThighL + o, hip, knee);
        set(ShinL + o, knee, ankle);
        set(FootL + o, ankle, ball);
        set(ToesL + o, ball, tip);
    }
    for (int b = 0; b < BONES; ++b) {
        BoneRest& r = bn[b];
        r.parent = PARENT[b];
        r.length = len(r.tail - r.head);
        r.rot = frameFor(r.head, r.tail);
    }
    for (int b = 0; b < BONES; ++b) {
        BoneRest& r = bn[b];
        if (r.parent < 0) { r.localRot = r.rot; r.localOffset = r.head; continue; }
        const BoneRest& p = bn[r.parent];
        r.localRot = qmul(qconj(p.rot), r.rot);
        r.localOffset = qrot(qconj(p.rot), r.head - p.head);
    }
}

void buildShape(Body& B) {
    Builder b(B);
    const float H = B.params.height;
    auto& bn = B.skeleton.bones;
    const Quat I{};
    const float m = B.params.muscle;
    // ---------------- torso + head (group 0)
    float kT = 0.038f;
    V3 pel = bn[Pelvis].head;
    b.ell(Pelvis, 0, kT, pel + V3{0, 0.006f * H, -0.004f * H}, {0.084f * B.params.hips, 0.058f, 0.06f}, I);
    for (float sx : {1.f, -1.f}) {   // glutes
        b.ell(Pelvis, 0, kT, pel + V3{sx * 0.043f * H * B.params.hips, -0.016f * H, -0.034f * H}, {0.044f, 0.05f, 0.036f}, qaxis({1, 0, 0}, -0.25f), 0.4f);
    }
    V3 sp = bn[Spine].head;
    b.ell(Spine, 0, kT, sp + V3{0, 0.03f * H, 0.0f * H}, {0.076f, 0.068f, 0.05f}, I);           // abdomen + waist
    b.ell(Spine, 0, 0.02f, sp + V3{0, 0.025f * H, 0.024f * H}, {0.048f, 0.055f, 0.018f}, I, 0.5f);      // abs
    V3 ch = bn[Chest].head;
    const float sh = B.params.shoulders;
    b.ell(Chest, 0, kT, ch + V3{0, 0.05f * H, -0.004f * H}, {0.092f * sh, 0.09f, 0.06f}, I);    // ribcage
    for (float sx : {1.f, -1.f}) {
        b.ell(Chest, 0, 0.026f, ch + V3{sx * 0.046f * H, 0.08f * H, 0.03f * H}, {0.056f, 0.03f, 0.018f}, qaxis({0, 0, 1}, sx * 0.12f), 0.7f);  // pecs: broad, flat plates
        b.ell(Chest, 0, 0.03f, ch + V3{sx * 0.063f * H * sh, 0.05f * H, -0.022f * H}, {0.036f, 0.08f, 0.036f}, qaxis({0, 0, 1}, sx * -0.22f), 0.7f);  // lats
        b.ell(Chest, 0, 0.03f, ch + V3{sx * 0.035f * H, 0.085f * H, -0.035f * H}, {0.04f, 0.045f, 0.025f}, I, 0.4f);   // upper back
    }
    // neck + traps
    b.cone(Neck, 0, 0.024f, bn[Neck].head + V3{0, -0.01f * H, -0.006f * H}, bn[Neck].tail + V3{0, -0.006f * H, -0.004f * H}, 0.038f, 0.033f);
    for (int s = 0; s < 2; ++s) {
        float sx = s == 0 ? 1.f : -1.f;
        int cl = s == 0 ? ClavicleL : ClavicleR;
        b.cone(cl, 0, 0.028f, bn[Neck].head + V3{sx * 0.022f * H, 0.012f * H, -0.012f * H}, bn[cl].tail + V3{-sx * 0.012f * H, 0.012f * H, -0.004f * H},
               0.026f * (0.85f + 0.15f * m), 0.018f);   // trapezius onto the shoulder
        b.cone(cl, 0, 0.02f, bn[cl].head + V3{0, 0.004f * H, 0}, bn[cl].tail + V3{0, 0.006f * H, 0.006f * H}, 0.011f, 0.012f);   // collarbone
    }
    // head: cranium, face, jaw, nose, ears
    V3 hd = bn[Head].head;
    float hl = bn[Head].length;
    b.ell(Head, 0, 0.012f, hd + V3{0, 0.62f * hl, -0.01f * H}, {0.044f, 0.05f, 0.054f}, I);          // cranium
    b.ell(Head, 0, 0.016f, hd + V3{0, 0.42f * hl, 0.018f * H}, {0.04f, 0.048f, 0.038f}, I);          // face
    b.ell(Head, 0, 0.012f, hd + V3{0, 0.16f * hl, 0.022f * H}, {0.032f, 0.022f, 0.03f}, qaxis({1, 0, 0}, 0.3f));   // jaw + chin
    b.cone(Head, 0, 0.004f, hd + V3{0, 0.48f * hl, 0.052f * H}, hd + V3{0, 0.38f * hl, 0.058f * H}, 0.005f, 0.006f);  // nose
    for (float sx : {1.f, -1.f}) {
        b.ell(Head, 0, 0.004f, hd + V3{sx * 0.044f * H, 0.47f * hl, -0.004f * H}, {0.006f, 0.016f, 0.011f}, I);      // ears
        b.ell(Head, 0, 0.008f, hd + V3{sx * 0.024f * H, 0.53f * hl, 0.044f * H}, {0.013f, 0.007f, 0.006f}, I);     // brow
    }
    // ---------------- arms (groups 1, 2)
    for (int s = 0; s < 2; ++s) {
        int o = s == 0 ? 0 : 6, grp = 1 + s;
        float sx = s == 0 ? 1.f : -1.f;
        int ua = UpperArmL + o, fa = ForearmL + o, ha = HandL + o, fi = FingersL + o, th = ThumbL + o;
        float kA = 0.016f;
        // deltoid capping the shoulder
        b.ell(ua, grp, kA, b.at(ua, 0.14f, sx * 0.003f, 0.0f), {0.038f, 0.056f, 0.04f}, b.fr(ua), 0.6f);
        b.cone(ua, grp, kA, bn[ua].head, bn[ua].tail, 0.029f, 0.021f);
        b.ell(ua, grp, kA, b.at(ua, 0.52f, 0, 0.011f), {0.023f, 0.062f, 0.022f}, b.fr(ua), 1.f);     // biceps
        b.ell(ua, grp, kA, b.at(ua, 0.42f, 0, -0.012f), {0.021f, 0.07f, 0.02f}, b.fr(ua), 0.8f);   // triceps
        b.cone(fa, grp, kA, bn[fa].head, bn[fa].tail, 0.022f, 0.0145f);
        b.ell(fa, grp, kA, b.at(fa, 0.26f, 0, 0.004f), {0.024f, 0.06f, 0.02f}, b.fr(fa), 0.7f);    // forearm muscles
        // hand: palm (flat, breadth front-back in the A-pose), finger block, thumb
        b.ell(ha, grp, 0.008f, b.at(ha, 0.5f), {0.011f, 0.032f, 0.025f}, b.fr(ha));
        b.ell(fi, grp, 0.006f, b.at(fi, 0.48f, 0, -0.002f), {0.008f, 0.03f, 0.022f}, b.fr(fi));
        b.cone(th, grp, 0.006f, bn[th].head, bn[th].tail, 0.009f, 0.0065f);
    }
    // ---------------- legs (groups 3, 4)
    for (int s = 0; s < 2; ++s) {
        int o = s == 0 ? 0 : 4, grp = 3 + s;
        float sx = s == 0 ? 1.f : -1.f;
        int t = ThighL + o, sh2 = ShinL + o, f = FootL + o, to = ToesL + o;
        float kL = 0.02f;
        b.cone(t, grp, kL, bn[t].head + V3{0, 0.01f * H, 0}, bn[t].tail, 0.046f, 0.03f);
        b.ell(t, grp, kL, b.at(t, 0.5f, 0.004f * sx, 0.016f), {0.034f, 0.11f, 0.03f}, b.fr(t), 0.9f);         // quads
        b.ell(t, grp, kL, b.at(t, 0.78f, sx * 0.018f, 0.01f), {0.022f, 0.045f, 0.022f}, b.fr(t), 1.f);        // vastus medialis (teardrop)
        b.ell(t, grp, kL, b.at(t, 0.45f, 0, -0.016f), {0.03f, 0.1f, 0.028f}, b.fr(t), 0.7f);                // hamstrings
        b.ell(t, grp, kL, b.at(t, 0.25f, sx * -0.02f, 0), {0.026f, 0.08f, 0.028f}, b.fr(t), 0.4f);          // adductors
        b.ell(sh2, grp, 0.012f, bn[sh2].head + V3{0, 0, 0.012f * H}, {0.018f, 0.02f, 0.012f}, I);              // kneecap
        b.cone(sh2, grp, kL, bn[sh2].head, bn[sh2].tail + V3{0, 0.008f * H, 0}, 0.03f, 0.018f);
        b.ell(sh2, grp, kL, b.at(sh2, 0.3f, sx * 0.003f, -0.016f), {0.027f, 0.065f, 0.026f}, b.fr(sh2), 1.f);  // calf
        b.ell(sh2, grp, 0.008f, bn[sh2].tail + V3{sx * 0.012f * H, 0.004f * H, 0}, {0.008f, 0.01f, 0.008f}, I);   // ankle bone
        // foot: heel, arch, ball, toes
        V3 an = bn[f].head;
        b.ell(f, grp, 0.012f, an + V3{0, -0.016f * H, -0.012f * H}, {0.02f, 0.02f, 0.026f}, I);
        b.ell(f, grp, 0.016f, b.at(f, 0.55f, 0, -0.006f), {0.026f, 0.05f, 0.017f}, b.fr(f));
        b.ell(to, grp, 0.01f, b.at(to, 0.45f, 0, -0.002f), {0.026f, 0.024f, 0.01f}, b.fr(to));
    }
    B.groupK[1] = B.groupK[2] = 0.022f * H;   // arms into the shoulders
    B.groupK[3] = B.groupK[4] = 0.03f * H;    // legs into the hips
}

// ---- surface nets
struct Grid {
    V3 o;
    float c;
    int nx, ny, nz;
    std::vector<float> d;
    float& at(int i, int j, int k) { return d[(size_t(k) * (ny + 1) + j) * (nx + 1) + i]; }
};

template <class F>
void parallelRows(int n, F&& fn) {
    unsigned T = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> th;
    for (unsigned t = 0; t < T; ++t) th.emplace_back([&, t] { for (int i = int(t); i < n; i += int(T)) fn(i); });
    for (auto& x : th) x.join();
}

V3 gradient(const Body& B, V3 p, float e) {
    return V3{B.sdf(p + V3{e, 0, 0}) - B.sdf(p - V3{e, 0, 0}), B.sdf(p + V3{0, e, 0}) - B.sdf(p - V3{0, e, 0}),
              B.sdf(p + V3{0, 0, e}) - B.sdf(p - V3{0, 0, e})} * (0.5f / e);
}

bool polygonise(Body& B, float cell) {
    // bounds of everything
    V3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
    for (const Prim& p : B.prims) {
        lo = {std::min(lo.x, p.bc.x - p.br), std::min(lo.y, p.bc.y - p.br), std::min(lo.z, p.bc.z - p.br)};
        hi = {std::max(hi.x, p.bc.x + p.br), std::max(hi.y, p.bc.y + p.br), std::max(hi.z, p.bc.z + p.br)};
    }
    float pad = 0.05f * B.params.height;
    lo = lo - V3{pad, pad, pad};
    hi = hi + V3{pad, pad, pad};
    Grid G;
    G.o = lo;
    G.c = cell;
    G.nx = int(std::ceil((hi.x - lo.x) / cell));
    G.ny = int(std::ceil((hi.y - lo.y) / cell));
    G.nz = int(std::ceil((hi.z - lo.z) / cell));
    G.d.assign(size_t(G.nx + 1) * (G.ny + 1) * (G.nz + 1), 1e9f);
    parallelRows(G.nz + 1, [&](int k) {
        for (int j = 0; j <= G.ny; ++j)
            for (int i = 0; i <= G.nx; ++i) G.at(i, j, k) = B.sdf(G.o + V3{i * cell, j * cell, k * cell});
    });
    // one vertex per cell the surface passes through
    const int corner[8][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}, {0, 0, 1}, {1, 0, 1}, {0, 1, 1}, {1, 1, 1}};
    const int edges[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    std::unordered_map<int64_t, uint32_t> vid;
    auto key = [&](int i, int j, int k) { return (int64_t(k) * (G.ny + 1) + j) * (G.nx + 1) + i; };
    std::vector<V3> verts;
    std::vector<int64_t> cellOf;
    for (int k = 0; k < G.nz; ++k)
        for (int j = 0; j < G.ny; ++j)
            for (int i = 0; i < G.nx; ++i) {
                float v[8];
                int inside = 0;
                for (int c = 0; c < 8; ++c) { v[c] = G.at(i + corner[c][0], j + corner[c][1], k + corner[c][2]); inside += v[c] < 0; }
                if (inside == 0 || inside == 8) continue;
                V3 sum{};
                int n = 0;
                for (auto& e : edges) {
                    float a = v[e[0]], b2 = v[e[1]];
                    if ((a < 0) == (b2 < 0)) continue;
                    float t = a / (a - b2);
                    V3 pa{float(corner[e[0]][0]), float(corner[e[0]][1]), float(corner[e[0]][2])};
                    V3 pb{float(corner[e[1]][0]), float(corner[e[1]][1]), float(corner[e[1]][2])};
                    sum = sum + lerp(pa, pb, t);
                    ++n;
                }
                vid[key(i, j, k)] = uint32_t(verts.size());
                verts.push_back(G.o + (V3{float(i), float(j), float(k)} + sum * (1.f / n)) * cell);
                cellOf.push_back(key(i, j, k));
            }
    if (int(verts.size()) > B.params.maxVerts) return false;
    // project each vertex onto the true surface; normals from the gradient
    Mesh& M = B.mesh;
    M = Mesh{};
    M.pos.resize(verts.size());
    M.nrm.resize(verts.size());
    const float e = cell * 0.25f;
    parallelRows(int(verts.size()), [&](int vi) {
        V3 p = verts[vi];
        for (int it = 0; it < 4; ++it) {
            V3 gr = gradient(B, p, e);
            float g2 = dot(gr, gr);
            if (g2 < 1e-8f) break;
            float d = B.sdf(p);
            V3 step = gr * (d / g2);
            float sl = len(step);
            if (sl > cell * 0.75f) step = step * (cell * 0.75f / sl);   // stay near the cell
            p = p - step;
            if (std::fabs(d) < cell * 0.01f) break;
        }
        M.pos[vi] = p;
        M.nrm[vi] = norm(gradient(B, p, e));
    });
    // quads across every edge with a sign change
    auto emit = [&](uint32_t a, uint32_t b2, uint32_t c, uint32_t d) {
        // split along the shorter diagonal, wind outwards
        auto tri = [&](uint32_t p0, uint32_t p1, uint32_t p2) {
            V3 n = cross(M.pos[p1] - M.pos[p0], M.pos[p2] - M.pos[p0]);
            V3 avg = M.nrm[p0] + M.nrm[p1] + M.nrm[p2];
            if (dot(n, avg) < 0) std::swap(p1, p2);
            M.idx.insert(M.idx.end(), {p0, p1, p2});
        };
        if (len(M.pos[a] - M.pos[c]) < len(M.pos[b2] - M.pos[d])) { tri(a, b2, c); tri(a, c, d); }
        else { tri(a, b2, d); tri(b2, c, d); }
    };
    auto cellV = [&](int i, int j, int k, uint32_t& out) {
        auto it = vid.find(key(i, j, k));
        if (it == vid.end()) return false;
        out = it->second;
        return true;
    };
    for (int k = 1; k < G.nz; ++k)
        for (int j = 1; j < G.ny; ++j)
            for (int i = 1; i < G.nx; ++i) {
                bool s0 = G.at(i, j, k) < 0;
                uint32_t a, b2, c, d;
                if (s0 != (G.at(i + 1, j, k) < 0) && cellV(i, j - 1, k - 1, a) && cellV(i, j, k - 1, b2) && cellV(i, j, k, c) && cellV(i, j - 1, k, d)) emit(a, b2, c, d);
                if (s0 != (G.at(i, j + 1, k) < 0) && cellV(i - 1, j, k - 1, a) && cellV(i, j, k - 1, b2) && cellV(i, j, k, c) && cellV(i - 1, j, k, d)) emit(a, b2, c, d);
                if (s0 != (G.at(i, j, k + 1) < 0) && cellV(i - 1, j - 1, k, a) && cellV(i, j - 1, k, b2) && cellV(i, j, k, c) && cellV(i - 1, j, k, d)) emit(a, b2, c, d);
            }
    return true;
}

// ---- skin weights: a vertex follows the bones whose shapes it lies on
void computeWeights(Body& B) {
    Mesh& M = B.mesh;
    const size_t n = M.pos.size();
    M.bones.assign(n, {0, 0, 0, 0});
    M.weights.assign(n, {1, 0, 0, 0});
    const float sigma = 0.011f * B.params.height;
    parallelRows(int(n), [&](int vi) {
        float best[BONES];
        for (float& x : best) x = 1e9f;
        for (const Prim& pr : B.prims) {
            float lb = len(M.pos[vi] - pr.bc) - pr.br;
            if (lb > best[pr.bone] || lb > 0.15f * B.params.height) continue;
            best[pr.bone] = std::min(best[pr.bone], std::max(0.f, B.primDist(pr, M.pos[vi])));
        }
        int nb = 0;
        for (int b = 1; b < BONES; ++b) if (best[b] < best[nb]) nb = b;
        // candidates: the nearest bone, its parent and its children
        float w[BONES] = {};
        float sum = 0;
        for (int b = 0; b < BONES; ++b) {
            bool cand = b == nb || b == PARENT[nb] || PARENT[b] == nb;
            if (!cand || best[b] > 1e8f) continue;
            // a vertex clearly on one side never follows the other side's bones
            float vx = M.pos[vi].x;
            if (boneSide(b) * vx < -0.02f * B.params.height) continue;
            float dd = (best[b] - best[nb]) / sigma;
            if (dd > 4.f) continue;
            w[b] = std::exp(-dd * dd * 0.7f - dd * 0.6f);
            sum += w[b];
        }
        // top four
        std::array<std::pair<float, int>, BONES> order;
        for (int b = 0; b < BONES; ++b) order[b] = {w[b], b};
        std::partial_sort(order.begin(), order.begin() + 4, order.end(), [](auto& x, auto& y) { return x.first > y.first; });
        float s4 = 0;
        for (int i = 0; i < 4; ++i) { if (order[i].first < 0.01f * order[0].first) order[i].first = 0; s4 += order[i].first; }
        if (s4 <= 0) { M.bones[vi] = {uint8_t(nb), 0, 0, 0}; M.weights[vi] = {1, 0, 0, 0}; return; }
        for (int i = 0; i < 4; ++i) {
            M.bones[vi][i] = uint8_t(order[i].second);
            M.weights[vi][i] = order[i].first / s4;
        }
        (void)sum;
    });
}

}  // namespace

Body build(const Params& p) {
    Body B;
    B.params = p;
    buildSkeleton(B);
    buildShape(B);
    float cell = p.cell;
    for (int tries = 0; tries < 6; ++tries) {
        if (polygonise(B, cell)) break;
        cell *= 1.18f;   // too many vertices for a 16-bit mesh: coarser
    }
    computeWeights(B);
    return B;
}

// ================================================================ posing
Globals forward(const Skeleton& s, const Pose& p) {
    Globals g;
    for (int b = 0; b < BONES; ++b) {   // bones are ordered parents first
        const BoneRest& r = s.bones[b];
        if (r.parent < 0) {
            g.rot[b] = qnorm(qmul(r.rot, p.local[b]));
            g.pos[b] = r.head + p.root;
        } else {
            g.rot[b] = qnorm(qmul(qmul(g.rot[r.parent], r.localRot), p.local[b]));
            g.pos[b] = g.pos[r.parent] + qrot(g.rot[r.parent], r.localOffset);
        }
    }
    return g;
}

void skin(const Body& B, const Globals& g, std::vector<V3>& pos, std::vector<V3>& nrm) {
    const Mesh& M = B.mesh;
    const size_t n = M.pos.size();
    pos.resize(n);
    nrm.resize(n);
    // per bone: rest -> posed rotation, and the posed head
    Quat dq[BONES];
    for (int b = 0; b < BONES; ++b) dq[b] = qmul(g.rot[b], qconj(B.skeleton.bones[b].rot));
    for (size_t i = 0; i < n; ++i) {
        V3 p{}, nn{};
        for (int k = 0; k < 4; ++k) {
            float w = M.weights[i][k];
            if (w <= 0) continue;
            int b = M.bones[i][k];
            p = p + (g.pos[b] + qrot(dq[b], M.pos[i] - B.skeleton.bones[b].head)) * w;
            nn = nn + qrot(dq[b], M.nrm[i]) * w;
        }
        pos[i] = p;
        nrm[i] = norm(nn);
    }
}

void setGlobalRot(const Skeleton& s, Pose& p, int bone, Quat global) {
    const BoneRest& r = s.bones[bone];
    if (r.parent < 0) { p.local[bone] = qnorm(qmul(qconj(r.rot), global)); return; }
    Globals g = forward(s, p);
    Quat base = qmul(g.rot[r.parent], r.localRot);
    p.local[bone] = qnorm(qmul(qconj(base), global));
}

void aim(const Skeleton& s, Pose& p, int bone, V3 dir, V3 zRef) {
    Globals g = forward(s, p);
    V3 y = norm(dir);
    Quat q;
    if (len(zRef) > 1e-6f) {
        V3 z = zRef - y * dot(zRef, y);
        if (len(z) < 1e-5f) z = qrot(g.rot[bone], {0, 0, 1});
        z = norm(z);
        V3 x = cross(y, z);
        q = qbasis(x, y, z);
    } else {
        V3 cur = qrot(g.rot[bone], {0, 1, 0});
        q = qmul(qbetween(cur, y), g.rot[bone]);
    }
    setGlobalRot(s, p, bone, q);
}

bool twoBoneIK(const Skeleton& s, Pose& p, int upper, int lower, V3 target, V3 pole, float zToPole) {
    Globals g = forward(s, p);
    V3 A = g.pos[upper];
    float la = s.bones[upper].length, lb = s.bones[lower].length;
    V3 toT = target - A;
    float d = len(toT);
    bool reach = d <= la + lb - 1e-4f && d >= std::fabs(la - lb) + 1e-4f;
    d = std::clamp(d, std::fabs(la - lb) + 1e-4f, la + lb - 1e-4f);
    V3 dir = norm(toT);
    V3 pd = (pole - A) - dir * dot(pole - A, dir);
    if (len(pd) < 1e-5f) pd = qrot(g.rot[upper], {0, 0, zToPole});
    pd = norm(pd);
    float a = (la * la - lb * lb + d * d) / (2 * d);
    float h = std::sqrt(std::max(0.f, la * la - a * a));
    V3 E = A + dir * a + pd * h;
    aim(s, p, upper, E - A, pd * zToPole);
    g = forward(s, p);
    V3 end = A + dir * d;
    aim(s, p, lower, end - g.pos[lower], pd * zToPole);
    return reach;
}

Quat flex(float x, float y, float z) {
    return qnorm(qmul(qmul(qaxis({0, 1, 0}, y), qaxis({1, 0, 0}, x)), qaxis({0, 0, 1}, z)));
}

// ================================================================ animation
namespace anim {

void rest(Pose& p) { p = Pose{}; }

namespace {
// let both arms hang: upper arm down (a little out and forwards), elbows soft, hands relaxed
void hangArms(const Body& B, Pose& p, float swingL, float swingR, float elbowL, float elbowR, float out = 0.1f) {
    const Skeleton& s = B.skeleton;
    for (int side = 0; side < 2; ++side) {
        int o = side == 0 ? 0 : 6;
        float sx = side == 0 ? 1.f : -1.f, sw = side == 0 ? swingL : swingR, el = side == 0 ? elbowL : elbowR;
        V3 down = norm(V3{sx * out, -1.f, 0.02f});
        V3 dir = qrot(qaxis({1, 0, 0}, -sw), down);                     // swing forwards (+) / back (-)
        aim(s, p, UpperArmL + o, dir, V3{sx * -0.3f, 0, 1});            // inner elbow facing forwards
        V3 fdir = qrot(qaxis({1, 0, 0}, -(sw + el)), down);
        aim(s, p, ForearmL + o, fdir, V3{sx * -0.6f, 0, 1});
        // palm towards the thigh, fingers softly curled
        aim(s, p, HandL + o, qrot(qaxis({1, 0, 0}, -0.1f), fdir), V3{0, 0, 1});
        p.local[FingersL + o] = qaxis({0, 0, 1}, -sx * 0.35f);
        p.local[ThumbL + o] = qaxis({0, 0, 1}, -sx * 0.2f);
    }
}
float footLift(const Body& B) { return B.skeleton.bones[FootL].head.y; }
}  // namespace

void idle(const Body& B, Pose& p, float t) {
    rest(p);
    const float H = B.params.height;
    float breath = std::sin(t * 2 * PI / 4.2f);
    float sway = std::sin(t * 2 * PI / 7.f);
    p.root = {sway * 0.008f * H, -0.004f * H + breath * 0.0008f * H, 0};
    p.local[Pelvis] = flex(0.03f, 0, sway * 0.025f);
    p.local[Spine] = flex(-0.02f + breath * 0.01f, 0, -sway * 0.02f);
    p.local[Chest] = flex(-0.03f - breath * 0.02f, 0, -sway * 0.01f);
    p.local[Neck] = flex(0.04f, std::sin(t * 0.5f) * 0.12f, 0);
    p.local[Head] = flex(-0.02f, std::sin(t * 0.5f + 0.4f) * 0.1f, 0);
    hangArms(B, p, 0.04f, 0.03f, 0.18f, 0.2f);
    // feet planted: legs reach for the rest ankle positions
    const Skeleton& s = B.skeleton;
    for (int side = 0; side < 2; ++side) {
        int o = side == 0 ? 0 : 4;
        V3 ank = s.bones[FootL + o].head;
        twoBoneIK(s, p, ThighL + o, ShinL + o, ank, ank + V3{0, 0.4f * H, 0.6f * H}, 1.f);
        aim(s, p, FootL + o, s.bones[FootL + o].tail - s.bones[FootL + o].head, V3{0, 1, 0});
    }
}

void walk(const Body& B, Pose& p, float t, float speed) {
    rest(p);
    const Skeleton& s = B.skeleton;
    const float H = B.params.height;
    const float stride = 0.42f * H * std::clamp(0.6f + speed * 0.3f, 0.7f, 1.1f);   // one step
    const float freq = speed / (2 * stride);                                          // full cycles / s
    const float ph = t * freq;
    const float ankleY = footLift(B);
    // pelvis: lowest at double support, highest mid-stance; yaw and roll with the stride
    float c2 = std::cos(ph * 4 * PI);
    p.root = {std::sin(ph * 2 * PI) * 0.012f * H, -0.012f * H - c2 * 0.009f * H, 0};
    p.local[Pelvis] = flex(0.04f, std::sin(ph * 2 * PI) * 0.09f, std::sin(ph * 2 * PI) * 0.035f);
    p.local[Spine] = flex(0.01f, -std::sin(ph * 2 * PI) * 0.06f, -std::sin(ph * 2 * PI) * 0.02f);
    p.local[Chest] = flex(-0.02f, -std::sin(ph * 2 * PI) * 0.07f, 0);
    p.local[Neck] = flex(0.03f, std::sin(ph * 2 * PI) * 0.05f, 0);
    for (int side = 0; side < 2; ++side) {
        int o = side == 0 ? 0 : 4;
        float u = std::fmod(ph + (side == 0 ? 0.f : 0.5f), 1.f);
        if (u < 0) u += 1;
        const float stance = 0.6f;
        V3 rest0 = s.bones[FootL + o].head;
        float z, lift = 0, pitch = 0;
        if (u < stance) {
            float k = u / stance;                         // heel strike in front -> toe off behind
            z = stride * (0.5f - k);
            pitch = k < 0.12f ? 0.25f * (1 - k / 0.12f) : k > 0.75f ? -0.55f * sstep(0.75f, 1.f, k) : 0.f;
            if (k > 0.75f) lift = 0.03f * H * sstep(0.75f, 1.f, k);   // heel rises
        } else {
            float k = (u - stance) / (1 - stance);
            z = stride * (-0.5f + sstep(0.f, 1.f, k));
            lift = std::sin(k * PI) * 0.055f * H;
            pitch = -0.55f * (1 - sstep(0.f, 0.35f, k)) + 0.25f * sstep(0.6f, 1.f, k);
        }
        V3 ankle{rest0.x * 0.85f, ankleY + lift, rest0.z + z};
        twoBoneIK(s, p, ThighL + o, ShinL + o, ankle, ankle + V3{0, 0.45f * H, 0.6f * H}, 1.f);
        V3 fd = norm(s.bones[FootL + o].tail - s.bones[FootL + o].head);
        aim(s, p, FootL + o, qrot(qaxis({1, 0, 0}, -pitch), fd), V3{0, 1, 0});
        p.local[ToesL + o] = flex(pitch < -0.1f ? -pitch * 0.8f : 0.f);   // toes stay on the ground at push-off
    }
    // arms swing against the legs; the forward arm bends more
    float sw = std::sin(ph * 2 * PI) * 0.32f * std::clamp(speed / 1.4f, 0.5f, 1.4f);
    hangArms(B, p, -sw, sw, 0.25f + std::max(0.f, -sw) * 0.6f, 0.25f + std::max(0.f, sw) * 0.6f, 0.12f);
    // keep the head level and looking ahead
    Globals g = forward(s, p);
    V3 fwd = qrot(g.rot[Head], {0, 0, 1});
    float pitchH = std::atan2(fwd.y, fwd.z);
    p.local[Head] = qmul(p.local[Head], qaxis({1, 0, 0}, pitchH));
}

static void cycleOnce(const Body& B, Pose& p, const Bike& bike, float drop, float& excess) {
    rest(p);
    const Skeleton& s = B.skeleton;
    const float H = B.params.height;
    const float st = std::clamp(bike.stand, 0.f, 1.f), tk = std::clamp(bike.tuck, 0.f, 1.f) * (1 - st);
    const float rock = std::sin(bike.crank * 2.f) * (0.3f + bike.effort) * (1.f + st * 2.f);
    // pelvis: hip centre above the sit bones (out of the saddle: up and forwards over the cranks)
    V3 hipRest = s.bones[Pelvis].head;
    V3 seatHip = bike.seat + V3{0, 0.045f * H, 0.01f * H};
    V3 standHip = lerp(bike.seat, (bike.pedalL + bike.pedalR) * 0.5f, 0.45f) + V3{0, 0.16f * H, 0};
    V3 hip = lerp(seatHip, standHip, st) + V3{rock * 0.006f * H, -drop, 0};
    p.root = hip - hipRest;
    // torso lean: find the forward bend that brings the shoulders within a comfortable reach
    // of the hands (elbows a little bent), spread over pelvis, lumbar and chest
    V3 hands = (bike.hoodL + bike.hoodR) * 0.5f;
    float reach = (s.bones[UpperArmL].length + s.bones[ForearmL].length) * (0.86f - 0.04f * tk);
    float lean = 0.8f;
    for (int it = 0; it < 14; ++it) {
        float a = lean;
        p.local[Pelvis] = flex(a * 0.42f, 0, rock * 0.02f);
        p.local[Spine] = flex(a * 0.28f, -rock * 0.02f, 0);
        p.local[Chest] = flex(a * 0.3f, -rock * 0.03f, -rock * 0.02f);
        Globals g = forward(s, p);
        V3 shoulders = (g.pos[UpperArmL] + g.pos[UpperArmR]) * 0.5f;
        float err = len(hands - shoulders) - reach;
        lean = std::clamp(lean + err / (0.3f * H), 0.15f, 1.45f);
    }
    // neck and head look up the road
    p.local[Neck] = flex(-lean * 0.35f);
    {
        Globals g = forward(s, p);
        V3 fwd = qrot(g.rot[Head], {0, 0, 1});
        p.local[Head] = qmul(qaxis({1, 0, 0}, std::atan2(fwd.y, fwd.z) - 0.08f), p.local[Head]);
    }
    // arms to the hoods: elbows out and down a little
    for (int side = 0; side < 2; ++side) {
        int o = side == 0 ? 0 : 6;
        float sx = side == 0 ? 1.f : -1.f;
        V3 grip = side == 0 ? bike.hoodL : bike.hoodR;
        V3 wrist = grip + V3{0, 0.02f * H, -0.04f * H};
        Globals g = forward(s, p);
        V3 sh = g.pos[UpperArmL + o];
        twoBoneIK(s, p, UpperArmL + o, ForearmL + o, wrist, sh + V3{sx * 0.16f * H, -0.3f * H, -0.12f * H}, -1.f);
        // hand forwards along the hood, palm in, fingers wrapped round it
        aim(s, p, HandL + o, norm(V3{0, -0.25f, 1}), V3{sx * -1.f, 0.2f, 0});
        p.local[FingersL + o] = qaxis({0, 0, 1}, -sx * 1.25f);
        p.local[ThumbL + o] = qaxis({0, 0, 1}, -sx * 0.6f);
    }
    // legs: ball of the foot on the pedal, ankle dropping through the back of the stroke
    for (int side = 0; side < 2; ++side) {
        int o = side == 0 ? 0 : 4;
        float sx = side == 0 ? 1.f : -1.f;
        V3 ped = side == 0 ? bike.pedalL : bike.pedalR;
        float a = bike.crank + (side == 0 ? PI : 0.f);
        float theta = 0.3f + 0.18f * std::sin(a + 0.6f);
        V3 fdir{0, -std::sin(theta), std::cos(theta)};
        float fl = s.bones[FootL + o].length;
        V3 ball = ped + V3{0, 0.02f * H, 0};
        V3 ankle = ball - fdir * fl;
        Globals g = forward(s, p);
        V3 hj = g.pos[ThighL + o];
        float maxReach = (s.bones[ThighL + o].length + s.bones[ShinL + o].length) * 0.995f;
        excess = std::max(excess, len(ankle - hj) - maxReach);
        twoBoneIK(s, p, ThighL + o, ShinL + o, ankle, hj + V3{sx * 0.08f * H, 0.1f * H, 0.6f * H}, 1.f);
        g = forward(s, p);
        aim(s, p, FootL + o, ball - g.pos[FootL + o], V3{0, 1, 0});
        p.local[ToesL + o] = flex(0.1f);
    }
}

void cycle(const Body& B, Pose& p, const Bike& bike) {
    // a saddle set a touch high for this body: the hips rock down to reach the bottom of the stroke
    float drop = 0;
    for (int it = 0; it < 3; ++it) {
        float excess = -1e9f;
        cycleOnce(B, p, bike, drop, excess);
        if (excess <= 0) break;
        drop += excess * 1.05f;
    }
}

}  // namespace anim
}  // namespace body
