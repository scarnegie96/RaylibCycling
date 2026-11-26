#include "EzTree.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

namespace eztree {
namespace {

constexpr float PI = 3.14159265358979f;

// ---- ez-tree's RNG (a multiply-with-carry pair), reproducing JavaScript's 32-bit semantics
struct RNG {
    int32_t m_w, m_z;
    explicit RNG(uint32_t seed) {
        m_w = int32_t(uint32_t(int64_t(123456789) + int64_t(seed)));
        m_z = int32_t(uint32_t(int64_t(987654321) - int64_t(seed)));
    }
    float random(float max = 1, float min = 0) {
        int64_t tz = 36969LL * (m_z & 65535) + (m_z >> 16);
        m_z = int32_t(uint32_t(tz));
        int64_t tw = 18000LL * (m_w & 65535) + (m_w >> 16);
        m_w = int32_t(uint32_t(tw));
        int32_t hi = int32_t(uint32_t(m_z) << 16);
        uint32_t r = uint32_t(int64_t(hi) + (m_w & 65535));
        double res = double(r) / 4294967296.0;
        return float((max - min) * res + min);
    }
};

// ---- three.js maths
struct Q { float x = 0, y = 0, z = 0, w = 1; };
struct E { float x = 0, y = 0, z = 0; };   // order XYZ

Q mul(const Q& a, const Q& b) {
    return {a.x * b.w + a.w * b.x + a.y * b.z - a.z * b.y,
            a.y * b.w + a.w * b.y + a.z * b.x - a.x * b.z,
            a.z * b.w + a.w * b.z + a.x * b.y - a.y * b.x,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Q fromEuler(const E& e) {
    float c1 = std::cos(e.x / 2), c2 = std::cos(e.y / 2), c3 = std::cos(e.z / 2);
    float s1 = std::sin(e.x / 2), s2 = std::sin(e.y / 2), s3 = std::sin(e.z / 2);
    return {s1 * c2 * c3 + c1 * s2 * s3, c1 * s2 * c3 - s1 * c2 * s3, c1 * c2 * s3 + s1 * s2 * c3, c1 * c2 * c3 - s1 * s2 * s3};
}
E toEuler(const Q& q) {
    float x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
    float xx = q.x * x2, xy = q.x * y2, xz = q.x * z2, yy = q.y * y2, yz = q.y * z2, zz = q.z * z2;
    float wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;
    float m11 = 1 - (yy + zz), m12 = xy - wz, m13 = xz + wy;
    float m22 = 1 - (xx + zz), m23 = yz - wx;
    float m32 = yz + wx, m33 = 1 - (xx + yy);
    E e;
    e.y = std::asin(std::clamp(m13, -1.f, 1.f));
    if (std::fabs(m13) < 0.9999999f) { e.x = std::atan2(-m23, m33); e.z = std::atan2(-m12, m11); }
    else { e.x = std::atan2(m32, m22); e.z = 0; }
    return e;
}
Q axisAngle(V3 a, float ang) { float s = std::sin(ang / 2); return {a.x * s, a.y * s, a.z * s, std::cos(ang / 2)}; }
V3 rotate(const Q& q, V3 v) {
    // three.js applyQuaternion
    float tx = 2 * (q.y * v.z - q.z * v.y), ty = 2 * (q.z * v.x - q.x * v.z), tz = 2 * (q.x * v.y - q.y * v.x);
    return {v.x + q.w * tx + q.y * tz - q.z * ty, v.y + q.w * ty + q.z * tx - q.x * tz, v.z + q.w * tz + q.x * ty - q.y * tx};
}
// three.js: qa.slerp(qb, t)
Q slerp(Q a, const Q& b, float t) {
    if (t == 0) return a;
    if (t == 1) return b;
    float x = a.x, y = a.y, z = a.z, w = a.w;
    float cosH = w * b.w + x * b.x + y * b.y + z * b.z;
    Q r;
    if (cosH < 0) { r = {-b.x, -b.y, -b.z, -b.w}; cosH = -cosH; } else r = b;
    if (cosH >= 1.0f) return {x, y, z, w};
    float sqrSin = 1.f - cosH * cosH;
    if (sqrSin <= 1.1920929e-7f) {
        float s = 1 - t;
        Q o{s * x + t * r.x, s * y + t * r.y, s * z + t * r.z, s * w + t * r.w};
        float l = std::sqrt(o.x * o.x + o.y * o.y + o.z * o.z + o.w * o.w);
        return {o.x / l, o.y / l, o.z / l, o.w / l};
    }
    float sinH = std::sqrt(sqrSin), half = std::atan2(sinH, cosH);
    float ra = std::sin((1 - t) * half) / sinH, rb = std::sin(t * half) / sinH;
    return {x * ra + r.x * rb, y * ra + r.y * rb, z * ra + r.z * rb, w * ra + r.w * rb};
}
V3 add(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 scl(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
V3 lerp(V3 a, V3 b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; }
float len(V3 a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
V3 norm(V3 a) { float l = len(a); return l > 0 ? scl(a, 1.f / l) : a; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

struct Branch { V3 origin; E orientation; float length, radius; int level, sectionCount, segmentCount; };

struct Grower {
    const Options& o;
    RNG rng;
    Skeleton sk;
    std::deque<Branch> queue;
    explicit Grower(const Options& op) : o(op), rng(op.seed) {}

    std::vector<int> shuffled(int count) {
        std::vector<int> a(count);
        for (int k = 0; k < count; ++k) a[k] = k;
        for (int k = count - 1; k > 0; --k) {
            int r = int(std::floor(rng.random() * (k + 1)));
            std::swap(a[k], a[r]);
        }
        return a;
    }
    void recordLeaf(V3 origin, const E& orientation) {
        float size = o.leafSize * (1 + rng.random(o.leafSizeVariance, -o.leafSizeVariance));
        Q q = fromEuler(orientation);
        sk.leaves.push_back({origin, q.x, q.y, q.z, q.w, size});
    }
    struct Sec { V3 origin; E orientation; float radius; };

    void growBranch(const Branch& b) {
        E so = b.orientation;
        V3 sp = b.origin;
        // (ez-tree compares the type against 'Deciduous', which never matches its lower-case
        // 'deciduous', so the section length is never divided: presets are tuned for that)
        float sectionLength = b.length / b.sectionCount;
        std::vector<Sec> sections;
        BranchSk out;
        out.segmentCount = b.segmentCount;
        out.baseRadius = b.radius;
        out.level = b.level;
        float along = 0;
        for (int i = 0; i <= b.sectionCount; ++i) {
            float r = b.radius;
            if (i == b.sectionCount && b.level == o.levels) r = 0.001f;
            else if (!o.evergreen) r *= 1 - o.taper[b.level] * (float(i) / b.sectionCount);
            else r *= 1 - float(i) / b.sectionCount;
            sections.push_back({sp, so, r});
            Q qs = fromEuler(so);
            out.sections.push_back({sp, qs.x, qs.y, qs.z, qs.w, r, along});
            sp = add(sp, rotate(qs, {0, sectionLength, 0}));
            along += sectionLength;
            float g = std::max(1.f, 1.f / std::sqrt(r)) * o.gnarliness[b.level];
            so.x += rng.random(g, -g);
            so.z += rng.random(g, -g);
            Q qSection = fromEuler(so);
            qSection = mul(qSection, axisAngle({0, 1, 0}, o.twist[b.level]));
            V3 up = rotate(qSection, {0, 1, 0});
            V3 target = norm(o.forceDir);
            V3 axis = cross(up, target);
            float sinFull = len(axis);
            if (sinFull > 1e-6f) {
                axis = scl(axis, 1.f / sinFull);
                float full = std::atan2(sinFull, dot(up, target));
                float step = o.forceStrength / r;
                float cl = std::max(-full, std::min(full, step));
                qSection = mul(axisAngle(axis, cl), qSection);
            }
            so = toEuler(qSection);
        }
        sk.branches.push_back(std::move(out));
        if (!o.evergreen) {
            const Sec& last = sections.back();
            if (b.level < o.levels)
                queue.push_back({last.origin, last.orientation, o.length[b.level + 1], last.radius, b.level + 1, b.sectionCount, b.segmentCount});
            else
                recordLeaf(last.origin, last.orientation);
        }
        if (b.level == o.levels) leaves(sections);
        else if (b.level < o.levels) children(int(o.children[b.level]), b.level + 1, sections);
    }

    void interp(const std::vector<Sec>& s, float t, V3& origin, E& orient, float* radius) {
        int n = int(s.size());
        int idx = int(std::floor(t * (n - 1)));
        const Sec& a = s[idx];
        const Sec& b = idx == n - 1 ? a : s[idx + 1];
        float alpha = (t - float(idx) / (n - 1)) / (1.f / (n - 1));
        origin = lerp(a.origin, b.origin, alpha);
        if (radius) *radius = (1 - alpha) * a.radius + alpha * b.radius;
        orient = toEuler(slerp(fromEuler(b.orientation), fromEuler(a.orientation), alpha));
    }

    void children(int count, int level, const std::vector<Sec>& s) {
        float radialOffset = rng.random();
        float startMin = o.start[level];
        float heightStep = (1.f - startMin) / count;
        auto slots = shuffled(count);
        for (int i = 0; i < count; ++i) {
            float st = startMin + (i + rng.random()) * heightStep;
            V3 origin; E parent; float pr;
            interp(s, st, origin, parent, &pr);
            float radius = o.radius[level] * pr;
            float jitter = rng.random(0.5f, -0.5f);
            float radial = 2.f * PI * (radialOffset + (slots[i] + jitter) / count);
            Q q1 = axisAngle({1, 0, 0}, o.angle[level] / (180.f / PI));
            Q q2 = axisAngle({0, 1, 0}, radial);
            Q q3 = fromEuler(parent);
            E orient = toEuler(mul(q3, mul(q2, q1)));
            float length = o.length[level] * (o.evergreen ? 1.f - st : 1.f);
            queue.push_back({origin, orient, length, radius, level, int(o.sections[level]), int(o.segments[level])});
        }
    }

    void leaves(const std::vector<Sec>& s) {
        float radialOffset = rng.random();
        int count = o.leafCount;
        float startMin = o.leafStart;
        float heightStep = (1.f - startMin) / count;
        auto slots = shuffled(count);
        for (int i = 0; i < count; ++i) {
            float st = startMin + (i + rng.random()) * heightStep;
            V3 origin; E parent;
            interp(s, st, origin, parent, nullptr);
            float jitter = rng.random(0.5f, -0.5f);
            float radial = 2.f * PI * (radialOffset + (slots[i] + jitter) / count);
            Q q1 = axisAngle({1, 0, 0}, o.leafAngle / (180.f / PI));
            Q q2 = axisAngle({0, 1, 0}, radial);
            Q q3 = fromEuler(parent);
            recordLeaf(origin, toEuler(mul(q3, mul(q2, q1))));
        }
    }
};

const Options PRESETS[] = {
#include "EzTreePresets.inc"
};

}  // namespace

const Options* preset(const char* name) {
    for (const Options& p : PRESETS)
        if (std::strcmp(p.name, name) == 0) return &p;
    return nullptr;
}

Skeleton grow(const Options& o) {
    Grower g(o);
    E zero;
    g.queue.push_back({{0, 0, 0}, zero, o.length[0], o.radius[0], 0, int(o.sections[0]), int(o.segments[0])});
    while (!g.queue.empty()) {
        Branch b = g.queue.front();
        g.queue.pop_front();
        g.growBranch(b);
    }
    return std::move(g.sk);
}

Geometry mesh(const Options& o, const Skeleton& sk, const Detail& d) {
    Geometry g;
    const int stride = std::max(1, d.sectionStride);
    for (const BranchSk& br : sk.branches) {
        const int segments = std::max(3, int(std::lround(br.segmentCount * d.segmentFactor)));
        const float wrapsX = std::max(1.f, std::round(br.baseRadius * o.barkScaleX));
        std::vector<const Section*> sampled;
        for (size_t i = 0; i < br.sections.size(); i += stride) sampled.push_back(&br.sections[i]);
        if ((br.sections.size() - 1) % stride != 0) sampled.push_back(&br.sections.back());
        const uint32_t base = uint32_t(g.branchVerts.size());
        for (size_t k = 0; k < sampled.size(); ++k) {
            const Section& s = *sampled[k];
            Q q{s.qx, s.qy, s.qz, s.qw};
            for (int j = 0; j <= segments; ++j) {   // (the last duplicates the first, for UV continuity)
                float a = 2.f * PI * float(j % segments) / segments;
                V3 dir = rotate(q, {std::cos(a), 0, std::sin(a)});
                Vertex v;
                v.p = add(s.origin, scl(dir, s.radius));
                v.n = norm(dir);
                v.u = float(j) / segments * wrapsX;
                v.v = s.along;
                v.branchLevel = br.level;
                g.branchVerts.push_back(v);
            }
        }
        const uint32_t N = segments + 1;
        for (size_t i = 0; i + 1 < sampled.size(); ++i)
            for (int j = 0; j < segments; ++j) {
                uint32_t v1 = base + uint32_t(i) * N + j, v2 = v1 + 1, v3 = v1 + N, v4 = v2 + N;
                g.branchIdx.insert(g.branchIdx.end(), {v1, v3, v2, v2, v3, v4});
            }
    }
    const bool dbl = d.billboard < 0 ? o.doubleBillboard : d.billboard == 1;
    for (size_t li = 0; li < sk.leaves.size(); li += std::max(1, d.leafStride)) {
        const LeafSk& L = sk.leaves[li];
        Q q{L.qx, L.qy, L.qz, L.qw};
        float W = L.size * d.leafScale, H = W;
        auto quad = [&](float rot) {
            uint32_t i = uint32_t(g.leafVerts.size());
            Q qr = mul(q, axisAngle({0, 1, 0}, rot));   // (rotate about y first, then the leaf's orientation)
            V3 corner[4] = {{-W / 2, H, 0}, {-W / 2, 0, 0}, {W / 2, 0, 0}, {W / 2, H, 0}};
            float uv[4][2] = {{0, 1}, {0, 0}, {1, 0}, {1, 1}};
            V3 n = rotate(q, {0, 0, 1});
            for (int c = 0; c < 4; ++c) {
                Vertex v;
                v.p = add(L.origin, rotate(qr, corner[c]));
                v.n = norm(add(n, sub(v.p, L.origin)));   // ez-tree's rounded normals
                v.u = uv[c][0]; v.v = uv[c][1];
                v.branchLevel = -1;
                g.leafVerts.push_back(v);
                g.leafOrigins.push_back(L.origin);
            }
            g.leafIdx.insert(g.leafIdx.end(), {i, i + 1, i + 2, i, i + 2, i + 3});
        };
        quad(0);
        if (dbl) quad(PI / 2);
    }
    return g;
}

}  // namespace eztree
