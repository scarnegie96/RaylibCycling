// Body: proportions, mesh quality, skin weights, FK / IK, scaling.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>

#include "body/Body.h"

using namespace body;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; std::printf("FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void bounds(const std::vector<V3>& v, V3& lo, V3& hi) {
    lo = {1e9f, 1e9f, 1e9f}; hi = {-1e9f, -1e9f, -1e9f};
    for (auto& p : v) { lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)}; hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)}; }
}

int main() {
    auto t0 = std::chrono::steady_clock::now();
    Body B = build();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    const Mesh& M = B.mesh;
    std::printf("body: %zu verts, %zu tris, %zu primitives, built in %.0f ms\n", M.pos.size(), M.idx.size() / 3, B.prims.size(), ms);

    // ---- proportions
    V3 lo, hi;
    bounds(M.pos, lo, hi);
    CHECK(std::fabs(hi.y - 1.78f) < 0.04f, "stature %.3f", hi.y);
    CHECK(std::fabs(lo.y) < 0.012f, "soles at %.3f", lo.y);
    CHECK(std::fabs(lo.x + hi.x) < 0.01f, "symmetric in x (%.3f %.3f)", lo.x, hi.x);
    const Skeleton& S = B.skeleton;
    float shoulderW = len(S.bones[UpperArmL].head - S.bones[UpperArmR].head);
    CHECK(shoulderW > 0.32f && shoulderW < 0.40f, "shoulder joints %.3f m apart", shoulderW);
    float leg = S.bones[ThighL].length + S.bones[ShinL].length;
    CHECK(leg > 0.82f && leg < 0.92f, "leg %.3f", leg);
    float arm = S.bones[UpperArmL].length + S.bones[ForearmL].length;
    CHECK(arm > 0.55f && arm < 0.63f, "arm %.3f", arm);
    // half the stature is near the hips; the head is ~1/7.5 of the height
    CHECK(std::fabs(S.bones[ThighL].head.y / 1.78f - 0.53f) < 0.02f, "hip height %.3f", S.bones[ThighL].head.y);
    CHECK(S.bones[Head].length > 0.2f && S.bones[Head].length < 0.26f, "head %.3f", S.bones[Head].length);
    // the surface wraps every bone: each bone's middle is inside the body
    for (int b = Pelvis; b < BONES; ++b) {
        V3 mid = (S.bones[b].head + S.bones[b].tail) * 0.5f;
        CHECK(B.sdf(mid) < 0, "%s middle outside the body (%.3f)", boneName(b), B.sdf(mid));
    }
    // limbs don't fuse: the gap between the thighs and between hand and hip is open
    CHECK(B.sdf({0, 0.45f, 0.0f}) > 0, "knees fused");
    CHECK(B.sdf({0, 0.2f, 0.0f}) > 0, "shins fused");

    // ---- mesh quality
    CHECK(M.pos.size() > 8000 && M.pos.size() < 65000, "vertex count %zu", M.pos.size());
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    for (size_t i = 0; i < M.idx.size(); i += 3)
        for (int e = 0; e < 3; ++e) {
            uint32_t a = M.idx[i + e], b = M.idx[i + (e + 1) % 3];
            edges[{std::min(a, b), std::max(a, b)}]++;
        }
    int open = 0, nonManifold = 0;
    for (auto& [k, n] : edges) { open += n == 1; nonManifold += n > 2; }
    CHECK(open == 0, "%d open edges", open);
    CHECK(nonManifold < int(edges.size() / 500), "%d non-manifold edges", nonManifold);
    // vertices sit on the surface
    float worst = 0;
    for (size_t i = 0; i < M.pos.size(); i += 7) worst = std::max(worst, std::fabs(B.sdf(M.pos[i])));
    CHECK(worst < 0.002f, "vertex off the surface by %.4f m", worst);
    // outward winding
    int inward = 0;
    for (size_t i = 0; i < M.idx.size(); i += 3) {
        V3 a = M.pos[M.idx[i]], b = M.pos[M.idx[i + 1]], c = M.pos[M.idx[i + 2]];
        V3 n = cross(b - a, c - a);
        if (dot(n, M.nrm[M.idx[i]] + M.nrm[M.idx[i + 1]] + M.nrm[M.idx[i + 2]]) < 0) ++inward;
    }
    CHECK(inward == 0, "%d inward triangles", inward);

    // ---- weights
    int badW = 0;
    int perBone[BONES] = {};
    for (size_t i = 0; i < M.pos.size(); ++i) {
        float s = 0;
        for (int k = 0; k < 4; ++k) s += M.weights[i][k];
        badW += std::fabs(s - 1) > 1e-3f;
        perBone[M.bones[i][0]]++;
    }
    CHECK(badW == 0, "%d vertices with weights not summing to 1", badW);
    for (int b = Pelvis; b < BONES; ++b) if (b != ClavicleL && b != ClavicleR) CHECK(perBone[b] > 20, "%s drives only %d vertices", boneName(b), perBone[b]);
    // left hand vertices never follow the right side
    int cross = 0;
    for (size_t i = 0; i < M.pos.size(); ++i)
        for (int k = 0; k < 4; ++k)
            if (M.weights[i][k] > 0 && boneSide(M.bones[i][k]) * (M.pos[i].x > 0.05f ? 1 : M.pos[i].x < -0.05f ? -1 : 0) < 0) ++cross;
    CHECK(cross == 0, "%d vertices weighted to the other side", cross);

    // ---- FK: the rest pose reproduces the bind mesh
    Pose P;
    Globals G = forward(S, P);
    std::vector<V3> pos, nrm;
    skin(B, G, pos, nrm);
    float err = 0;
    for (size_t i = 0; i < pos.size(); ++i) err = std::max(err, len(pos[i] - M.pos[i]));
    CHECK(err < 1e-4f, "rest pose moves vertices by %.6f", err);
    for (int b = 0; b < BONES; ++b) CHECK(len(G.pos[b] - S.bones[b].head) < 1e-4f && len(G.tail(S, b) - S.bones[b].tail) < 1e-4f, "%s rest FK", boneName(b));
    // a positive x flex swings the thigh forwards and bends the knee... backwards when negative
    P.local[ThighL] = flex(0.5f);
    G = forward(S, P);
    CHECK(G.tail(S, ThighL).z > S.bones[ThighL].tail.z + 0.15f, "hip flexion moves the knee forwards");
    P = Pose{};
    P.local[ShinL] = flex(-0.8f);
    G = forward(S, P);
    CHECK(G.tail(S, ShinL).z < S.bones[ShinL].tail.z - 0.2f, "knee flexion moves the ankle back");
    // bone lengths survive any pose
    P = Pose{};
    anim::walk(B, P, 0.37f);
    G = forward(S, P);
    for (int b = 0; b < BONES; ++b) CHECK(std::fabs(len(G.tail(S, b) - G.pos[b]) - S.bones[b].length) < 1e-4f, "%s length in walk", boneName(b));

    // ---- IK
    P = Pose{};
    V3 target = S.bones[ShinL].tail + V3{0, 0.25f, 0.2f};
    bool ok = twoBoneIK(S, P, ThighL, ShinL, target, target + V3{0, 0, 1}, 1.f);
    G = forward(S, P);
    CHECK(ok && len(G.tail(S, ShinL) - target) < 1e-3f, "leg IK reaches (%.4f)", len(G.tail(S, ShinL) - target));
    CHECK(G.pos[ShinL].z > S.bones[ThighL].head.z + 0.1f, "knee bends forwards");
    P = Pose{};
    V3 hand = S.bones[UpperArmL].head + V3{0.05f, -0.2f, 0.35f};
    ok = twoBoneIK(S, P, UpperArmL, ForearmL, hand, S.bones[UpperArmL].head + V3{0.4f, -0.3f, -0.3f}, -1.f);
    G = forward(S, P);
    CHECK(ok && len(G.tail(S, ForearmL) - hand) < 1e-3f, "arm IK reaches (%.4f)", len(G.tail(S, ForearmL) - hand));
    // cycling: feet stay on the pedals all the way round
    float worstPedal = 0;
    for (int i = 0; i < 16; ++i) {
        anim::Bike bk;
        bk.crank = i * 6.2832f / 16;
        bk.seat = {0, 0.915f, -0.193f};
        bk.hoodL = {0.21f, 0.955f, 0.64f}; bk.hoodR = {-0.21f, 0.955f, 0.64f};
        auto ped = [&](int side) { float a = bk.crank + (side ? 3.14159f : 0.f); return V3{side == 0 ? -0.095f : 0.095f, 0.275f - std::cos(a) * 0.17f, -std::sin(a) * 0.17f}; };
        bk.pedalR = ped(0); bk.pedalL = ped(1);
        P = Pose{};
        anim::cycle(B, P, bk);
        G = forward(S, P);
        worstPedal = std::max({worstPedal, len(G.tail(S, FootL) - (bk.pedalL + V3{0, 0.02f * 1.78f, 0})), len(G.tail(S, FootR) - (bk.pedalR + V3{0, 0.02f * 1.78f, 0}))});
        float handErr = std::max(len(G.tail(S, ForearmL) - (bk.hoodL + V3{0, 0.0356f, -0.0712f})), len(G.tail(S, ForearmR) - (bk.hoodR + V3{0, 0.0356f, -0.0712f})));
        CHECK(handErr < 0.03f, "hands off the hoods by %.3f at crank %d", handErr, i);
    }
    CHECK(worstPedal < 0.01f, "feet off the pedals by %.3f m", worstPedal);

    // ---- scaling
    Params small;
    small.height = 1.6f;
    small.girth = 0.9f;
    Body Bs = build(small);
    bounds(Bs.mesh.pos, lo, hi);
    CHECK(std::fabs(hi.y - 1.6f) < 0.04f, "scaled stature %.3f", hi.y);
    Params tall;
    tall.height = 1.95f;
    tall.legs = 1.06f;
    tall.muscle = 1.4f;
    Body Bt = build(tall);
    bounds(Bt.mesh.pos, lo, hi);
    CHECK(std::fabs(hi.y - 1.95f) < 0.05f, "tall stature %.3f", hi.y);
    CHECK(Bt.skeleton.bones[ThighL].head.y / 1.95f > B.skeleton.bones[ThighL].head.y / 1.78f, "longer legs sit the hips higher");
    CHECK(Bt.mesh.pos.size() < 65000, "tall mesh %zu verts", Bt.mesh.pos.size());

    std::printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
