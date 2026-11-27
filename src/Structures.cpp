// Generated worlds: the structures a road needs where the land won't carry it - tunnels (a
// lined tube cut straight into the hillside, lamps along the roof) and bridges (stone arches on piers,
// following the road's own curve and grade). Built from the map's road points near the camera.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

#include "RenderUtil.h"
#include "Renderer.h"
#include "rlgl.h"
#include "world/WorldMap.h"

using namespace rutil;

namespace {
constexpr float HW = 3.6f;                 // road half width
constexpr float TW = HW + 1.6f;            // tunnel half width (walls)
constexpr float TWALL = 4.2f, TCROWN = 6.6f;   // wall height, crown height

Vector3 V(const wmap::RoadPt& p) { return {p.x, p.y, p.z}; }
// where the hillside face stands at a tunnel end: the cutting's formation stops 0.6 of the way
// from the last open point to the first tunnel point (Map::sample), so the tube ends there
Vector3 portalFace(const std::vector<wmap::RoadPt>& P, int i0, int i1, bool start) {
    int n = int(P.size());
    if (start) return i0 > 0 ? Vector3Lerp(V(P[i0 - 1]), V(P[i0]), 0.6f) : V(P[i0]);
    return i1 + 1 < n ? Vector3Lerp(V(P[i1]), V(P[i1 + 1]), 0.4f) : V(P[i1]);
}
float headingAt(const std::vector<wmap::RoadPt>& P, int j) {
    int a = std::max(0, j - 1), b = std::min(int(P.size()) - 1, j + 1);
    return std::atan2(P[b].x - P[a].x, P[b].z - P[a].z);
}
// a triangle facing the given way
void triFacing(MeshBuilder& b, unsigned short i0, unsigned short i1, unsigned short i2, Vector3 want) {
    auto P = [&](unsigned short i) { return Vector3{b.v[i * 3], b.v[i * 3 + 1], b.v[i * 3 + 2]}; };
    Vector3 nrm = Vector3CrossProduct(Vector3Subtract(P(i1), P(i0)), Vector3Subtract(P(i2), P(i0)));
    if (Vector3DotProduct(nrm, want) < 0) b.tri(i0, i2, i1);
    else b.tri(i0, i1, i2);
}
// a strip of quads between two rows of points (same count), facing `out` at each row point
void strip(MeshBuilder& b, const std::vector<Vector3>& lo, const std::vector<Vector3>& hi, const std::vector<Vector3>& out, float vScale) {
    if (lo.size() < 2) return;
    unsigned short base = (unsigned short)(b.v.size() / 3);
    for (size_t j = 0; j < lo.size(); ++j) {
        b.add(lo[j], out[j], {0, j * vScale});
        b.add(hi[j], out[j], {1, j * vScale});
    }
    for (size_t j = 0; j + 1 < lo.size(); ++j) {
        unsigned short a0 = base + j * 2, a1 = a0 + 1, b0 = a0 + 2, b1 = a0 + 3;
        triFacing(b, a0, b0, a1, out[j]);
        triFacing(b, a1, b0, b1, out[j]);
    }
}

// ---- a tunnel: a lined tube (vertical walls, an elliptical vault) facing inwards
Mesh tunnelMesh(const std::vector<wmap::RoadPt>& P, int i0, int i1) {
    MeshBuilder b;
    const int K = 12;   // profile points: wall foot, wall top, vault..., wall top, wall foot
    float px[K], py[K], nx[K], ny[K];
    px[0] = -TW; py[0] = -0.4f; nx[0] = 1; ny[0] = 0;
    px[1] = -TW; py[1] = TWALL; nx[1] = 1; ny[1] = 0;
    for (int q = 0; q < K - 4; ++q) {
        float th = 3.14159265f * (1.f - float(q + 1) / (K - 3));
        px[q + 2] = TW * std::cos(th);
        py[q + 2] = TWALL + (TCROWN - TWALL) * std::sin(th);
        float gx = -std::cos(th) / TW, gy = -std::sin(th) / (TCROWN - TWALL);
        float gl = std::hypot(gx, gy);
        nx[q + 2] = gx / gl; ny[q + 2] = gy / gl;
    }
    px[K - 2] = TW; py[K - 2] = TWALL; nx[K - 2] = -1; ny[K - 2] = 0;
    px[K - 1] = TW; py[K - 1] = -0.4f; nx[K - 1] = -1; ny[K - 1] = 0;
    // rows: the hillside face (pushed 0.4 m out so the cut edge laps onto the lining), the
    // tunnel's own points, the far face
    struct Row { Vector3 p; float h; };
    std::vector<Row> R;
    auto face = [&](bool start) {
        int j = start ? i0 : i1;
        float h = headingAt(P, j);
        Vector3 f = portalFace(P, i0, i1, start);
        Vector3 out = Vector3Scale(Vector3{std::sin(h), 0, std::cos(h)}, start ? -0.4f : 0.4f);
        return Row{Vector3Add(f, out), h};
    };
    R.push_back(face(true));
    for (int j = i0; j <= i1; ++j) R.push_back({V(P[j]), headingAt(P, j)});
    R.push_back(face(false));
    float along = 0;
    for (size_t j = 0; j < R.size(); ++j) {
        if (j) along += std::hypot(R[j].p.x - R[j - 1].p.x, R[j].p.z - R[j - 1].p.z);
        Vector3 r = rightOf(R[j].h);
        for (int q = 0; q < K; ++q) {
            Vector3 p = Vector3Add(R[j].p, Vector3Add(Vector3Scale(r, px[q]), {0, py[q], 0}));
            Vector3 n = Vector3Add(Vector3Scale(r, nx[q]), {0, ny[q], 0});
            b.add(p, n, {float(q) / (K - 1), along / 4.f});
        }
    }
    int rows = int(R.size());
    for (int j = 0; j + 1 < rows; ++j)
        for (int q = 0; q + 1 < K; ++q) {
            unsigned short a0 = j * K + q, a1 = a0 + 1, b0 = a0 + K, b1 = b0 + 1;
            Vector3 in{b.n[a0 * 3], b.n[a0 * 3 + 1], b.n[a0 * 3 + 2]};
            triFacing(b, a0, b0, a1, in);
            triFacing(b, a1, b0, b1, in);
        }
    return b.build();
}

// ---- a bridge: deck slab, parapets, stone arches springing from piers and abutments
Mesh bridgeMesh(const wmap::Map& M, const wmap::Bridge& br) {
    const auto& P = M.roads[br.road].pts;
    const int a = std::max(0, br.i0), z = std::min(int(P.size()) - 1, br.i1);
    if (z - a < 1) return Mesh{};
    MeshBuilder b;
    const float W = HW + 1.25f;          // deck half width
    const float slab = 1.0f;             // deck thickness
    const int n = z - a + 1;
    std::vector<float> s(n, 0.f), ground(n);
    std::vector<Vector3> c(n), r(n);
    for (int j = 0; j < n; ++j) {
        const auto& p = P[a + j];
        if (j) s[j] = s[j - 1] + std::hypot(p.x - P[a + j - 1].x, p.z - P[a + j - 1].z);
        c[j] = {p.x, p.y - 0.06f, p.z};
        r[j] = rightOf(headingAt(P, a + j));
        ground[j] = std::min(M.height(p.x, p.z), br.width > 0 ? br.level : 1e9f);
    }
    const float span = s.back();
    // supports: the abutments at the ends, piers so that no arch is wider than ~22 m
    int arches = std::max(1, int(std::ceil(span / 22.f)));
    std::vector<float> sup;
    for (int k = 0; k <= arches; ++k) sup.push_back(span * k / arches);
    auto archUnder = [&](float sj, float deckBottom, float gnd) {
        // the soffit of the arch the point sits in: springing low at the supports, the crown
        // just under the deck
        int k = std::min(arches - 1, int(sj / (span / arches)));
        float s0 = sup[k], s1 = sup[k + 1], t = (sj - s0) / std::max(1e-3f, s1 - s0);
        float clear = deckBottom - (gnd + 0.6f);
        float rise = std::clamp(std::min(0.42f * (s1 - s0), clear * 0.8f), 0.6f, 14.f);
        float spring = deckBottom - rise;
        return spring + rise * std::sqrt(std::max(0.f, 1.f - (2 * t - 1) * (2 * t - 1)));
    };
    std::vector<Vector3> topL(n), topR(n), botL(n), botR(n), archL(n), archR(n), outL(n), outR(n), down(n, {0, -1, 0}), upv(n, {0, 1, 0});
    std::vector<Vector3> parInL(n), parInR(n), parTopL(n), parTopR(n), parOutTopL(n), parOutTopR(n), inL(n), inR(n);
    for (int j = 0; j < n; ++j) {
        Vector3 L = Vector3Add(c[j], Vector3Scale(r[j], -W)), R = Vector3Add(c[j], Vector3Scale(r[j], W));
        topL[j] = L; topR[j] = R;
        botL[j] = {L.x, L.y - slab, L.z};
        botR[j] = {R.x, R.y - slab, R.z};
        float under = archUnder(s[j], c[j].y - slab, ground[j]);
        archL[j] = {L.x, under, L.z};
        archR[j] = {R.x, under, R.z};
        outL[j] = Vector3Negate(r[j]);
        outR[j] = r[j];
        inL[j] = r[j];
        inR[j] = Vector3Negate(r[j]);
        // parapets: 1.05 m walls on the deck edges
        Vector3 Li = Vector3Add(c[j], Vector3Scale(r[j], -(W - 0.45f))), Ri = Vector3Add(c[j], Vector3Scale(r[j], W - 0.45f));
        parInL[j] = Li; parInR[j] = Ri;
        parTopL[j] = {Li.x, Li.y + 1.05f, Li.z};
        parTopR[j] = {Ri.x, Ri.y + 1.05f, Ri.z};
        parOutTopL[j] = {L.x, L.y + 1.05f, L.z};
        parOutTopR[j] = {R.x, R.y + 1.05f, R.z};
    }
    // spandrel walls (deck edge down to the arch) on both faces, then the arch soffits
    strip(b, archL, topL, outL, 0.25f);
    strip(b, archR, topR, outR, 0.25f);
    strip(b, archL, archR, down, 0.25f);
    // the parapets: outer faces continue the spandrels up, inner faces and copings
    strip(b, topL, parOutTopL, outL, 0.25f);
    strip(b, topR, parOutTopR, outR, 0.25f);
    strip(b, parInL, parTopL, inL, 0.25f);
    strip(b, parInR, parTopR, inR, 0.25f);
    strip(b, parTopL, parOutTopL, upv, 0.25f);
    strip(b, parTopR, parOutTopR, upv, 0.25f);
    // the deck surface under the tarmac ribbon (in case the ribbon is narrower)
    strip(b, topL, topR, upv, 0.25f);
    // piers and abutments: tapered stone columns from the springing down into the ground
    for (size_t k = 0; k < sup.size(); ++k) {
        float sk = sup[k];
        int j = 0;
        while (j + 1 < n && s[j + 1] < sk) ++j;
        int j1 = std::min(n - 1, j + 1);
        float t = std::clamp((sk - s[j]) / std::max(1e-3f, s[j1] - s[j]), 0.f, 1.f);
        Vector3 cc = Vector3Lerp(c[j], c[j1], t);
        Vector3 rr = Vector3Normalize(Vector3Lerp(r[j], r[j1], t));
        Vector3 ff{rr.z, 0, -rr.x};   // along the road (r = right, so forward is r rotated)
        float gnd = std::min(M.height(cc.x, cc.z), br.width > 0 ? br.level - 1.f : 1e9f);
        bool end = k == 0 || k + 1 == sup.size();
        float top = cc.y - slab;
        if (!end) top = archUnder(sk + 0.01f, cc.y - slab, gnd);   // pier tops carry the springing
        float bottom = gnd - 2.5f;
        if (top - bottom < 0.5f) continue;
        float halfAlong = end ? 2.2f : std::clamp(0.06f * (top - bottom) + 1.0f, 1.1f, 2.6f);
        float halfAcross = W + (end ? 0.6f : 0.3f);
        float taper = end ? 0.f : std::min(0.025f * (top - bottom), 1.2f);   // wider at the foot
        Vector3 corners[8];
        for (int q = 0; q < 8; ++q) {
            float sa = (q & 1) ? 1.f : -1.f, sc = (q & 2) ? 1.f : -1.f;
            bool low = q & 4;
            float ha = halfAlong + (low ? taper : 0.f), hc = halfAcross + (low ? taper : 0.f);
            Vector3 p = Vector3Add(cc, Vector3Add(Vector3Scale(ff, sa * ha), Vector3Scale(rr, sc * hc)));
            p.y = low ? bottom : top;
            corners[q] = p;
        }
        auto quad = [&](int i0, int i1, int i2, int i3, Vector3 out) {
            unsigned short base = (unsigned short)(b.v.size() / 3);
            b.add(corners[i0], out, {0, 0}); b.add(corners[i1], out, {1, 0});
            b.add(corners[i2], out, {1, 1}); b.add(corners[i3], out, {0, 1});
            triFacing(b, base, base + 1, base + 2, out);
            triFacing(b, base, base + 2, base + 3, out);
        };
        quad(0, 1, 5, 4, Vector3Negate(rr));   // (q&2)=0: the -r face
        quad(2, 3, 7, 6, rr);
        quad(0, 2, 6, 4, Vector3Negate(ff));
        quad(1, 3, 7, 5, ff);
        // cutwater cap / pier cap
        quad(0, 1, 3, 2, {0, 1, 0});
    }
    return b.build();
}
}  // namespace

void Renderer::clearStructures() {
    for (auto& [k, st] : structs_) if (st.mesh.vaoId) UnloadMesh(st.mesh);
    structs_.clear();
    portals_.clear();
    tunnelLamps_.clear();
    tunnelLampsU_.clear();
    structMap_ = nullptr;
    tunnelRect_.z = 0;
    tunnelFor_ = nullptr;
    tunnelCentre_ = {1e9f, 1e9f};
}

// The tunnel light map: a 1 km square round the camera at 2 m, rebuilt when the camera has
// moved 200 m from its centre. Per texel within the tube (+ a margin past the walls): metres
// in from the nearer mouth (the hillside face), 1, the road's height. The shaders compare a
// fragment's height with the road's, so roads and land above a tunnel stay in daylight.
void Renderer::updateTunnelMap(const wmap::Map* M) {
    constexpr int N = 512;
    constexpr float CELL = 2.f, SIZE = N * CELL, MARGIN = TW + 2.5f;
    if (!M || M->tunnels.empty()) { tunnelRect_.z = 0; return; }
    Vector2 c{cam_.position.x, cam_.position.z};
    if (M == tunnelFor_ && std::hypot(c.x - tunnelCentre_.x, c.y - tunnelCentre_.y) < 200.f) return;
    tunnelFor_ = M;
    c = {std::round(c.x / 64.f) * 64.f, std::round(c.y / 64.f) * 64.f};
    tunnelCentre_ = c;
    const float x0 = c.x - SIZE * 0.5f, z0 = c.y - SIZE * 0.5f;
    std::vector<float> data(size_t(N) * N * 4, 0.f), best(size_t(N) * N, 1e9f);
    bool any = false;
    for (const auto& T : M->tunnels) {
        const auto& P = M->roads[T.road].pts;
        // the tube's centre line, face to face (as tunnelMesh builds it)
        std::vector<Vector3> R;
        R.push_back(portalFace(P, T.i0, T.i1, true));
        for (int j = T.i0; j <= T.i1; ++j) R.push_back(V(P[j]));
        R.push_back(portalFace(P, T.i0, T.i1, false));
        std::vector<float> s(R.size(), 0.f);
        float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
        for (size_t j = 0; j < R.size(); ++j) {
            if (j) s[j] = s[j - 1] + std::hypot(R[j].x - R[j - 1].x, R[j].z - R[j - 1].z);
            lo[0] = std::min(lo[0], R[j].x); hi[0] = std::max(hi[0], R[j].x);
            lo[1] = std::min(lo[1], R[j].z); hi[1] = std::max(hi[1], R[j].z);
        }
        if (hi[0] + MARGIN < x0 || lo[0] - MARGIN > x0 + SIZE || hi[1] + MARGIN < z0 || lo[1] - MARGIN > z0 + SIZE) continue;
        const float L = s.back();
        for (size_t j = 0; j + 1 < R.size(); ++j) {
            const Vector3 a = R[j], b = R[j + 1];
            int ix0 = std::max(0, int((std::min(a.x, b.x) - MARGIN - x0) / CELL));
            int ix1 = std::min(N - 1, int((std::max(a.x, b.x) + MARGIN - x0) / CELL));
            int iz0 = std::max(0, int((std::min(a.z, b.z) - MARGIN - z0) / CELL));
            int iz1 = std::min(N - 1, int((std::max(a.z, b.z) + MARGIN - z0) / CELL));
            float ex = b.x - a.x, ez = b.z - a.z, el2 = std::max(1e-6f, ex * ex + ez * ez);
            for (int iz = iz0; iz <= iz1; ++iz)
                for (int ix = ix0; ix <= ix1; ++ix) {
                    float px = x0 + (ix + 0.5f) * CELL, pz = z0 + (iz + 0.5f) * CELL;
                    float t = std::clamp(((px - a.x) * ex + (pz - a.z) * ez) / el2, 0.f, 1.f);
                    float d = std::hypot(px - (a.x + ex * t), pz - (a.z + ez * t));
                    size_t k = size_t(iz) * N + ix;
                    if (d > MARGIN || d >= best[k]) continue;
                    best[k] = d;
                    float sa = s[j] + (s[j + 1] - s[j]) * t;
                    data[k * 4 + 0] = std::min(sa, L - sa);       // metres in from the nearer mouth
                    data[k * 4 + 1] = 1.f;
                    data[k * 4 + 2] = a.y + (b.y - a.y) * t;      // the road under the vault
                    any = true;
                }
        }
    }
    if (!any) { tunnelRect_.z = 0; return; }
    if (!tunnelTex_) {
        tunnelTex_ = rlLoadTexture(data.data(), N, N, PIXELFORMAT_UNCOMPRESSED_R32G32B32A32, 1);
        rlTextureParameters(tunnelTex_, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_NEAREST);
        rlTextureParameters(tunnelTex_, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_NEAREST);
        rlTextureParameters(tunnelTex_, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_CLAMP);
        rlTextureParameters(tunnelTex_, RL_TEXTURE_WRAP_T, RL_TEXTURE_WRAP_CLAMP);
    } else {
        rlUpdateTexture(tunnelTex_, 0, 0, N, N, PIXELFORMAT_UNCOMPRESSED_R32G32B32A32, data.data());
    }
    tunnelRect_ = {x0, z0, 1.f / SIZE, float(N)};
}

void Renderer::updateStructures() {
    const wmap::Map* M = course_ ? course_->world() : nullptr;
    if (M != structMap_) { clearStructures(); structMap_ = M; }
    portals_.clear();
    tunnelLamps_.clear();
    tunnelLampsU_.clear();
    updateTunnelMap(M);
    if (!M) return;
    const Vector3 c = cam_.position;
    auto bounds = [&](const std::vector<wmap::RoadPt>& P, int i0, int i1, Vector3& ctr, float& rad) {
        Vector3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};
        for (int j = std::max(0, i0); j <= std::min(int(P.size()) - 1, i1); ++j) { lo = Vector3Min(lo, V(P[j])); hi = Vector3Max(hi, V(P[j])); }
        ctr = Vector3Scale(Vector3Add(lo, hi), 0.5f);
        rad = Vector3Distance(lo, hi) * 0.5f + 30.f;
    };
    int builds = 0;
    auto keep = [&](int kind, int idx, const std::vector<wmap::RoadPt>& P, int i0, int i1, auto make) {
        Vector3 ctr;
        float rad;
        bounds(P, i0, i1, ctr, rad);
        float d = Vector3Distance(ctr, c) - rad;
        auto key = std::make_pair(kind, idx);
        auto it = structs_.find(key);
        if (d > 2600.f * viewScale_) {
            if (it != structs_.end()) { if (it->second.mesh.vaoId) UnloadMesh(it->second.mesh); structs_.erase(it); }
            return false;
        }
        if (it == structs_.end() && builds < 4) {
            Structure st;
            st.mesh = make();
            st.center = ctr;
            st.radius = rad;
            structs_[key] = st;
            ++builds;
        }
        return d < 1500.f * viewScale_;
    };
    camTunnel_ = InTunnel{};
    static const bool noTunnelCull = std::getenv("CORERAY_NOTUNNELCULL") != nullptr;   // dev: A/B
    for (size_t t = 0; t < M->tunnels.size(); ++t) {
        const auto& T = M->tunnels[t];
        const auto& P = M->roads[T.road].pts;
        bool nearT = keep(1, int(t), P, T.i0, T.i1, [&] { return tunnelMesh(P, T.i0, T.i1); });
        if (!nearT) continue;
        // portals face out of the hill; the heading points into the tunnel
        Vector3 fa = portalFace(P, T.i0, T.i1, true), fb = portalFace(P, T.i0, T.i1, false);
        portals_.push_back({fa, headingAt(P, T.i0)});
        portals_.push_back({fb, headingAt(P, T.i1) + PI_F});
        // is the camera in this tube (within the walls, under the crown, between the faces)?
        if (!noTunnelCull && !camTunnel_.in) {
            float best = 1e9f, roadY = 0, along = 0, total = 0;
            for (int j = std::max(0, T.i0); j < std::min(int(P.size()) - 1, T.i1); ++j) {
                float ax = P[j].x, az = P[j].z, bx = P[j + 1].x, bz = P[j + 1].z;
                float dx = bx - ax, dz = bz - az, l2 = dx * dx + dz * dz, len = std::sqrt(l2);
                float u = l2 > 1e-6f ? std::clamp(((c.x - ax) * dx + (c.z - az) * dz) / l2, 0.f, 1.f) : 0.f;
                float d = std::hypot(c.x - (ax + u * dx), c.z - (az + u * dz));
                if (d < best) { best = d; roadY = P[j].y + u * (P[j + 1].y - P[j].y); along = total + u * len; }
                total += len;
            }
            // the faces sit a few metres in from the ends; keep clear of them (the mouth itself
            // sees out widely anyway)
            float da = Vector3Distance(c, fa), db = Vector3Distance(c, fb);
            if (best < TW - 0.3f && c.y > roadY - 0.5f && c.y < roadY + TCROWN && along > 2.f && along < total - 2.f && da > 6.f && db > 6.f) {
                camTunnel_.in = true;
                camTunnel_.open[0] = Vector3Add(fa, {0, TCROWN * 0.5f, 0});
                camTunnel_.open[1] = Vector3Add(fb, {0, TCROWN * 0.5f, 0});
            }
        }
        for (int j = T.i0 + 2; j < T.i1 - 1; j += 5) {   // a lamp every 25 m along the crown
            Vector3 p = V(P[j]);
            if (Vector3Distance(p, c) > 500.f) continue;
            p.y += TCROWN - 0.25f;
            tunnelLamps_.push_back(p);
        }
    }
    // the nearest 24 light the scene (just under the fittings)
    std::vector<std::pair<float, Vector3>> lamps;
    for (const auto& l : tunnelLamps_) lamps.push_back({Vector3Distance(l, c), Vector3Add(l, {0, -0.15f, 0})});
    std::sort(lamps.begin(), lamps.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 0; i < lamps.size() && i < 24; ++i) tunnelLampsU_.push_back(lamps[i].second);
    for (size_t i = 0; i < M->bridges.size(); ++i) {
        const auto& B = M->bridges[i];
        if (B.road < 0) continue;
        keep(0, int(i), M->roads[B.road].pts, B.i0, B.i1, [&] { return bridgeMesh(*M, B); });
    }
}

void Renderer::drawStructures(bool sp, const Vector3& focus, float radius) {
    if (!structMap_) return;
    const Color stone{152, 144, 130, 255};
    for (auto& [k, st] : structs_) {
        if (!st.mesh.vaoId) continue;
        bool near = sp ? (std::fabs(st.center.x - focus.x) < radius + st.radius && std::fabs(st.center.z - focus.z) < radius + st.radius)
                       : Vector3Distance(st.center, cam_.position) - st.radius < 2400.f;
        if (!near) continue;
        // concrete lining: the shaders' tunnel map keeps the daylight out, its lamps light it
        if (k.first == 1) drawPart(st.mesh, MatrixIdentity(), MatrixIdentity(), {150, 149, 144, 255}, 0.8f, 0.12f, sp);
        else drawPart(st.mesh, MatrixIdentity(), MatrixIdentity(), stone, 0.9f, 0.08f, sp, 3);
    }
    // no portal geometry: the terrain shader cuts the hillside to the tube's arch (World.cpp)
    if (!sp)
        for (const auto& l : tunnelLamps_)
            drawPart(cube_, MatrixMultiply(MatrixScale(0.35f, 0.1f, 1.4f), MatrixTranslate(l.x, l.y, l.z)), MatrixIdentity(), {255, 214, 150, 255}, 0.4f, 0.2f, false, 0, 6.f);
}

// From inside a tube the land only shows through the two openings: a sphere is kept if it
// overlaps the cone from the camera through either opening (a disc ~8 m across the arch).
// Conservative: a curved tunnel hides even more.
bool Renderer::seenThroughTunnel(Vector3 c, float r) const {
    if (!camTunnel_.in) return true;
    const Vector3 cam = cam_.position;
    Vector3 v = Vector3Subtract(c, cam);
    float dv = Vector3Length(v);
    if (dv <= r) return true;
    for (const Vector3& o : camTunnel_.open) {
        Vector3 w = Vector3Subtract(o, cam);
        float dw = Vector3Length(w);
        const float ro = 8.f;   // the arch's circumscribed radius, with a margin
        if (dw <= ro) return true;
        float aOpen = std::asin(std::min(1.f, ro / dw)), aSphere = std::asin(std::min(1.f, r / dv));
        float ang = std::acos(std::clamp(Vector3DotProduct(v, w) / (dv * dw), -1.f, 1.f));
        if (ang <= aOpen + aSphere) return true;
    }
    return false;
}
