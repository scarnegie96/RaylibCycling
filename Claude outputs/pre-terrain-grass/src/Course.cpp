#include "Course.h"

#include <algorithm>
#include <cmath>

#include "Terrain.h"
#include "core/Route.h"

static constexpr float PI = 3.14159265358979f;

float hash2(int x, int y, uint32_t seed) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (h & 0xFFFFFF) / float(0x1000000);
}

float vnoise(float x, float y, uint32_t seed) {
    int xi = int(std::floor(x)), yi = int(std::floor(y));
    float fx = x - xi, fy = y - yi;
    float ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
    float a = hash2(xi, yi, seed), b = hash2(xi + 1, yi, seed);
    float c = hash2(xi, yi + 1, seed), d = hash2(xi + 1, yi + 1, seed);
    return (a + (b - a) * ux) + ((c + (d - c) * ux) - (a + (b - a) * ux)) * uy;
}

float fbm(float x, float y, int oct, uint32_t seed) {
    float sum = 0, amp = 0.5f, norm = 0;
    for (int i = 0; i < oct; ++i) {
        sum += vnoise(x, y, seed + i * 17) * amp;
        norm += amp;
        float nx = x * 1.6f - y * 1.2f, ny = x * 1.2f + y * 1.6f;
        x = nx; y = ny;
        amp *= 0.5f;
    }
    return sum / norm;
}

static float smoothstepf(float a, float b, float x) {
    float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}

static inline int64_t cellKey(int cx, int cz) { return (int64_t(cx) << 32) | int64_t(uint32_t(cz)); }

// ------------------------------------------------------------------ layout
float Course::loopRadius(float th) const {
    float r = 1;
    for (int k = 0; k < 4; ++k) {
        static const float harm[4] = {2, 3, 5, 7};
        r += la_[k] * std::sin(harm[k] * th + lp_[k]);
    }
    r *= loopR_;
    for (auto& [t0, K] : bumps_) {
        if (th <= t0) continue;
        float u = (th - t0) / (2 * PI - t0);
        float b = std::sin(PI * std::clamp(u, 0.f, 1.f));
        r *= 1 + (K - 1) * b * b;
    }
    return r;
}

void Course::build(const Route& route, uint32_t seed) {
    seed_ = seed * 2654435761u + 12345u;
    name_ = route.name;
    loop_ = route.isLoop;
    bumps_.clear();
    version_ = 0;
    // loop shape harmonics
    uint32_t s = seed_;
    auto U = [&]() { s = s * 1664525u + 1013904223u; return (s >> 8) / float(1 << 24); };
    static const float lo[4] = {0.06f, 0.03f, 0.01f, 0.004f}, span[4] = {0.07f, 0.04f, 0.02f, 0.01f};
    for (int k = 0; k < 4; ++k) { la_[k] = lo[k] + U() * span[k]; lp_[k] = U() * 2 * PI; }
    // elevation offset: keep the lowest point of the route 40 m above "sea level"
    double mn = 1e18;
    for (auto& p : route.points) mn = std::min(mn, p.ele);
    offset_ = route.points.empty() ? 0 : mn - 40;
    layout(route, 0);
    finishBuild(route);
}

void Course::relay(const Route& route, double fromDist) {
    if (pts_.empty()) { build(route, seed_); return; }
    double from = fromDist;
    if (loop_) { from = std::fmod(from, double(length_)); if (from < 0) from += length_; }
    size_t keep = size_t(std::clamp(from / step_ + 30, 2.0, double(pts_.size() - 1)));  // keep +60 m so the road under the rider never moves
    layout(route, keep);
    finishBuild(route);
    ++version_;
}

Course::Box Course::extendFrom(const Course& prev, const Route& route) {
    size_t keep = pts_.size() > 12 ? pts_.size() - 12 : pts_.size();
    layout(route, keep);
    finishBuild(route);
    ++version_;
    Box b{1e9f, 1e9f, -1e9f, -1e9f};
    for (size_t i = keep > 0 ? keep - 1 : 0; i < pts_.size(); ++i) {
        b.x0 = std::min(b.x0, pts_[i].pos.x); b.x1 = std::max(b.x1, pts_[i].pos.x);
        b.z0 = std::min(b.z0, pts_[i].pos.z); b.z1 = std::max(b.z1, pts_[i].pos.z);
    }
    const float reach = 1700.f;  // how far the road's heights shape the land (Course::regional)
    b.x0 -= reach; b.z0 -= reach; b.x1 += reach; b.z1 += reach;
    // take over the terrain field
    terrain::Field* f = nullptr;
    {
        std::lock_guard<std::mutex> lk(prev.field_.m);
        if (prev.field_.own) {
            f = prev.field_.own.get();
            std::lock_guard<std::mutex> lk2(field_.m);
            field_.own = std::move(prev.field_.own);
            field_.ptr.store(f);
            prev.field_.ptr.store(nullptr);
        }
    }
    if (f) f->rebind(*this, b.x0, b.z0, b.x1, b.z1);
    return b;
}

void Course::layout(const Route& route, size_t keep) {
    const double L = std::max(50.0, route.distance());
    const int N = std::max(20, int(std::lround(L / 2.0)));
    const float step = float(L / N);
    std::vector<CoursePoint> old;
    std::vector<float> oldTheta;
    old.swap(pts_);
    oldTheta.swap(theta_);
    keep = std::min(keep, std::min(old.size(), size_t(N)));
    const size_t total = loop_ ? size_t(N) : size_t(N) + 1;
    pts_.assign(total, {});
    for (size_t i = 0; i < keep; ++i) pts_[i] = old[i];

    if (loop_) {
        theta_.assign(total, 0);
        for (size_t i = 0; i < keep; ++i) theta_[i] = oldTheta[i];
        float th0 = keep ? theta_[keep - 1] : 0.f;
        double sKeep = keep ? (keep - 1) * double(step) : 0;
        auto arc = [&](float a, float b, int M) {
            double acc = 0;
            float px = 0, pz = 0;
            for (int i = 0; i <= M; ++i) {
                float t = a + (b - a) * i / M, r = loopRadius(t);
                float x = r * std::cos(t), z = r * std::sin(t);
                if (i) acc += std::hypot(x - px, z - pz);
                px = x; pz = z;
            }
            return acc;
        };
        if (keep == 0) {
            loopR_ = 1;
            loopR_ = float(L / arc(0, 2 * PI, 20000));
        } else {
            // add a smooth bulge (or squeeze) over the rest of the loop so it closes at the new length
            double target = L - sKeep;
            bumps_.push_back({th0, 1.f});
            float lo = 0.15f, hi = 12.f;
            for (int it = 0; it < 40; ++it) {
                float mid = 0.5f * (lo + hi);
                bumps_.back().second = mid;
                if (arc(th0, 2 * PI, 6000) < target) lo = mid; else hi = mid;
            }
            bumps_.back().second = 0.5f * (lo + hi);
        }
        // dense polyline from th0 to 2pi, then resample evenly
        const int M = std::max(20000, int((N - keep) * 4));
        std::vector<float> xs(M + 1), zs(M + 1), ts(M + 1);
        std::vector<double> acc(M + 1, 0);
        for (int i = 0; i <= M; ++i) {
            float t = th0 + (2 * PI - th0) * i / M, r = loopRadius(t);
            xs[i] = r * std::cos(t); zs[i] = r * std::sin(t); ts[i] = t;
            if (i) acc[i] = acc[i - 1] + std::hypot(xs[i] - xs[i - 1], zs[i] - zs[i - 1]);
        }
        size_t first = keep ? keep : 0;
        size_t count = size_t(N) - first;              // points first..N-1, then wraps to point 0
        double piece = acc[M];
        double baseIdx = keep ? double(keep - 1) : 0;  // arc 0 corresponds to index keep-1 (or 0)
        double span = double(N) - baseIdx;             // index units from base to the wrap
        int j = 0;
        for (size_t k = 0; k < count; ++k) {
            size_t i = first + k;
            double sTarget = (double(i) - baseIdx) / span * piece;
            while (j < M - 1 && acc[j + 1] < sTarget) ++j;
            float f = float((sTarget - acc[j]) / std::max(1e-9, acc[j + 1] - acc[j]));
            pts_[i].pos = {xs[j] + (xs[j + 1] - xs[j]) * f, 0, zs[j] + (zs[j + 1] - zs[j]) * f};
            theta_[i] = ts[j] + (ts[j + 1] - ts[j]) * f;
        }
    } else {
        theta_.clear();
        const float base = hash2(7, 11, seed_) * 2 * PI;
        auto heading = [&](double s) {
            float a = 1.2f * (2 * fbm(float(s / 2600.0), 0.37f, 3, seed_ + 5) - 1) + 0.4f * (2 * vnoise(float(s / 650.0), 4.1f, seed_ + 9) - 1);
            return base + std::clamp(a * 1.4f, -1.35f, 1.35f);
        };
        size_t i0 = keep ? keep - 1 : 0;
        if (!keep) pts_[0].pos = {0, 0, 0};
        for (size_t i = i0; i + 1 < total; ++i) {
            float h = heading((i + 0.5) * step);
            pts_[i + 1].pos = {pts_[i].pos.x + std::sin(h) * step, 0, pts_[i].pos.z + std::cos(h) * step};
        }
    }

    // elevations (smoothed), distances
    const size_t n = pts_.size();
    std::vector<float> e(n);
    for (size_t i = 0; i < n; ++i) e[i] = float(route.elevationAt(std::min(L, i * double(step))) - offset_);
    if (loop_ && n > 200) {  // make the loop's start and finish heights meet
        float gap = float(route.elevationAt(0) - route.elevationAt(L));
        int taper = std::min<int>(int(n / 4), 150);
        for (int k = 0; k < taper; ++k) e[n - taper + k] += gap * smoothstepf(0, 1, float(k + 1) / taper);
    }
    auto idx = [&](long i) -> size_t {
        if (loop_) return size_t(((i % long(n)) + long(n)) % long(n));
        return size_t(std::clamp<long>(i, 0, long(n) - 1));
    };
    for (int pass = 0; pass < 2; ++pass) {  // +-10 m box filter, twice
        std::vector<float> s(n);
        for (size_t i = 0; i < n; ++i) {
            float a = 0;
            for (long k = -5; k <= 5; ++k) a += e[idx(long(i) + k)];
            s[i] = a / 11;
        }
        e.swap(s);
    }
    for (size_t i = 0; i < n; ++i) {
        if (i >= keep) pts_[i].pos.y = e[i];
        pts_[i].dist = float(i * double(step));
    }
    for (size_t i = 0; i < n; ++i) {
        const auto& a = pts_[idx(long(i) - 5)].pos;
        const auto& b = pts_[idx(long(i) + 5)].pos;
        float span = float(idx(long(i) + 5) - idx(long(i) - 5));
        if (loop_) span = 10;
        pts_[i].grade = span > 0 ? (b.y - a.y) / (span * step) : 0;
        const auto& c = pts_[i].pos;
        const auto& nx = pts_[idx(long(i) + 1)].pos;
        const auto& pv = pts_[idx(long(i) - 1)].pos;
        pts_[i].heading = (i + 1 < n || loop_) ? std::atan2(nx.x - c.x, nx.z - c.z) : std::atan2(c.x - pv.x, c.z - pv.z);
    }
    step_ = step;
    length_ = float(L);
}

void Course::finishBuild(const Route& route) {
    name_ = route.name;
    minEle_ = 1e9f; maxEle_ = -1e9f; ascent_ = 0;
    double sum = 0;
    for (size_t i = 0; i < pts_.size(); ++i) {
        float y = pts_[i].pos.y;
        minEle_ = std::min(minEle_, y); maxEle_ = std::max(maxEle_, y);
        sum += y;
        if (i) ascent_ += std::max(0.f, y - pts_[i - 1].pos.y);
    }
    meanY_ = float(sum / std::max<size_t>(1, pts_.size()));
    water_ = minEle_ - 1000.f;  // no global sea: rivers and lakes come from the terrain field
    index();
    field_.reset();
}

void Course::index() {
    grid_.clear();
    coarse_.clear();
    far_.clear();
    for (int i = 0; i < int(pts_.size()); ++i) {
        const V3& p = pts_[i].pos;
        grid_[cellKey(int(std::floor(p.x / 64.f)), int(std::floor(p.z / 64.f)))].push_back(i);
        if (i % 40 == 0) coarse_[cellKey(int(std::floor(p.x / 512.f)), int(std::floor(p.z / 512.f)))].push_back(i);
        if (i % 200 == 0) far_[cellKey(int(std::floor(p.x / 2048.f)), int(std::floor(p.z / 2048.f)))].push_back(i);
    }
}

// ------------------------------------------------------------------ queries
CoursePoint Course::at(double dist) const {
    if (pts_.empty()) return {};
    const size_t n = pts_.size();
    if (!loop_ && (dist < 0 || dist > length_)) {
        const CoursePoint& e = dist < 0 ? pts_.front() : pts_.back();
        double over = dist < 0 ? dist : dist - length_;
        CoursePoint c = e;
        c.pos.x += std::sin(e.heading) * float(over);
        c.pos.z += std::cos(e.heading) * float(over);
        c.pos.y += e.grade * float(over);
        c.dist = float(dist);
        return c;
    }
    double d = dist;
    if (loop_) { d = std::fmod(d, double(length_)); if (d < 0) d += length_; }
    double fi = d / step_;
    size_t i = std::min(size_t(fi), n - 1);
    size_t k = loop_ ? (i + 1) % n : std::min(i + 1, n - 1);
    float f = float(fi - double(i));
    const auto& a = pts_[i]; const auto& b = pts_[k];
    CoursePoint c;
    c.pos = {a.pos.x + (b.pos.x - a.pos.x) * f, a.pos.y + (b.pos.y - a.pos.y) * f, a.pos.z + (b.pos.z - a.pos.z) * f};
    float dh = b.heading - a.heading;
    while (dh > PI) dh -= 2 * PI;
    while (dh < -PI) dh += 2 * PI;
    c.heading = a.heading + dh * f;
    c.grade = a.grade + (b.grade - a.grade) * f;
    c.dist = float(d);
    return c;
}

float Course::roadDistance(float x, float z, float* roadY, int* index) const {
    int gx = int(std::floor(x / 64.f)), gz = int(std::floor(z / 64.f));
    float best = 1e9f, by = 0;
    int bi = -1;
    const int N = int(pts_.size());
    for (int dz = -2; dz <= 2; ++dz)
        for (int dx = -2; dx <= 2; ++dx) {
            auto it = grid_.find(cellKey(gx + dx, gz + dz));
            if (it == grid_.end()) continue;
            for (int i : it->second) {
                int j = i + 1;
                if (j >= N) { if (!loop_) continue; j = 0; }
                const V3& a = pts_[i].pos; const V3& b = pts_[j].pos;
                float ex = b.x - a.x, ez = b.z - a.z;
                float t = ((x - a.x) * ex + (z - a.z) * ez) / std::max(1e-6f, ex * ex + ez * ez);
                t = std::clamp(t, 0.f, 1.f);
                float px = a.x + ex * t - x, pz = a.z + ez * t - z;
                float d2 = px * px + pz * pz;
                if (d2 < best) { best = d2; by = a.y + (b.y - a.y) * t; bi = i; }
            }
        }
    if (roadY) *roadY = by;
    if (index) *index = bi;
    return best > 1e8f ? 1e4f : std::sqrt(best);
}

float Course::nearestDist(float x, float z) const {
    int i;
    float d = roadDistance(x, z, nullptr, &i);
    if (i < 0 || d > 150) return -1;
    return pts_[i].dist;
}

float Course::roadNearest(float x, float z, float& px, float& pz, float& py) const {
    int gx = int(std::floor(x / 64.f)), gz = int(std::floor(z / 64.f));
    float best = 1e18f;
    const int N = int(pts_.size());
    for (int dz = -2; dz <= 2; ++dz)
        for (int dx = -2; dx <= 2; ++dx) {
            auto it = grid_.find(cellKey(gx + dx, gz + dz));
            if (it == grid_.end()) continue;
            for (int i : it->second) {
                int j = i + 1;
                if (j >= N) { if (!loop_) continue; j = 0; }
                const V3& a = pts_[i].pos; const V3& b = pts_[j].pos;
                float ex = b.x - a.x, ez = b.z - a.z;
                float t = std::clamp(((x - a.x) * ex + (z - a.z) * ez) / std::max(1e-6f, ex * ex + ez * ez), 0.f, 1.f);
                float qx = a.x + ex * t, qz = a.z + ez * t;
                float d2 = (qx - x) * (qx - x) + (qz - z) * (qz - z);
                if (d2 < best) { best = d2; px = qx; pz = qz; py = a.y + (b.y - a.y) * t; }
            }
        }
    return best > 1e17f ? 1e4f : std::sqrt(best);
}

float Course::regional(float x, float z, float* coarseDist, float* rugged) const {
    const float R = 1500.f;
    int gx = int(std::floor(x / 512.f)), gz = int(std::floor(z / 512.f));
    double sw = 0, sh = 0, sg = 0;
    float dmin2 = R * R;
    for (int dz = -3; dz <= 3; ++dz)
        for (int dx = -3; dx <= 3; ++dx) {
            auto it = coarse_.find(cellKey(gx + dx, gz + dz));
            if (it == coarse_.end()) continue;
            for (int i : it->second) {
                const CoursePoint& cp = pts_[i];
                const V3& p = cp.pos;
                float ox = x - p.x, oz = z - p.z;
                float d2 = ox * ox + oz * oz;
                if (d2 >= R * R) continue;
                dmin2 = std::min(dmin2, d2);
                double w = 1.0 / (double(d2) + 400.0);
                w *= w;
                w *= 1.0 - smoothstepf(900.f, R, std::sqrt(d2));
                // the road section tilts the land across it: steeper roads climb steeper hillsides
                float g = std::fabs(cp.grade);
                float lat = ox * std::cos(cp.heading) - oz * std::sin(cp.heading);  // + = right of the road
                float side = vnoise(cp.dist / 2200.f, 3.7f, seed_ + 41) > 0.5f ? 1.f : -1.f;
                float tilt = (0.04f + std::min(0.30f, g * 2.6f)) * side;
                float latc = 260.f * std::tanh(lat / 260.f);
                sw += w; sh += w * (p.y + tilt * latc); sg += w * g;
            }
        }
    // beyond that the land eases smoothly towards the farther road (a sparse index out to
    // ~5 km), then towards the course's mean height
    {
        int fx = int(std::floor(x / 2048.f)), fz = int(std::floor(z / 2048.f));
        for (int dz = -2; dz <= 2; ++dz)
            for (int dx = -2; dx <= 2; ++dx) {
                auto it = far_.find(cellKey(fx + dx, fz + dz));
                if (it == far_.end()) continue;
                for (int i : it->second) {
                    const V3& p = pts_[i].pos;
                    double d2 = double(x - p.x) * (x - p.x) + double(z - p.z) * (z - p.z);
                    double w = 1.0 / (d2 + 250000.0);
                    w *= w * 0.04 * (1.0 - smoothstepf(2500.f, 5000.f, float(std::sqrt(d2))));
                    sw += w; sh += w * p.y; sg += w * std::fabs(pts_[i].grade);
                }
            }
    }
    double wf = 1.0 / (6000.0 * 6000.0 * 6000.0 * 6000.0) * 0.05;
    sw += wf; sh += wf * meanY_; sg += wf * 0.02;
    if (coarseDist) *coarseDist = std::sqrt(dmin2);
    if (rugged) *rugged = std::clamp(float(sg / sw) * 11.f, 0.f, 1.f);
    return float(sh / sw);
}

const terrain::Field& Course::field() const {
    terrain::Field* f = field_.ptr.load(std::memory_order_acquire);
    if (f) return *f;
    std::lock_guard<std::mutex> lk(field_.m);
    f = field_.ptr.load(std::memory_order_relaxed);
    if (!f) {
        field_.own = std::make_unique<terrain::Field>(*this);
        f = field_.own.get();
        field_.ptr.store(f, std::memory_order_release);
    }
    return *f;
}

Course::FieldHolder::FieldHolder() = default;
Course::FieldHolder::FieldHolder(const FieldHolder&) {}
Course::FieldHolder::~FieldHolder() = default;
void Course::FieldHolder::reset() {
    std::lock_guard<std::mutex> lk(m);
    ptr.store(nullptr);
    own.reset();
}

float Course::terrainHeight(float x, float z, float* roadDistOut) const {
    return field().height(x, z, roadDistOut);
}
