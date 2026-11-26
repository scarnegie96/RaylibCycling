#pragma once
// Shared helpers for the world generator (noise, grids, threads).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace wmap::detail {

constexpr float PI = 3.14159265358979f;

inline uint32_t hashu(uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
inline uint32_t hash3(int a, int b, uint32_t s) { return hashu(uint32_t(a) * 0x9E3779B1u ^ hashu(uint32_t(b) * 0x85EBCA77u ^ s)); }
inline float hashf(int a, int b, uint32_t s) { return (hash3(a, b, s) >> 8) * (1.f / 16777216.f); }

// 2D gradient noise in -1..1
inline float gnoise(float x, float y, uint32_t s) {
    int xi = int(std::floor(x)), yi = int(std::floor(y));
    float fx = x - xi, fy = y - yi;
    auto grad = [&](int i, int j, float dx, float dy) {
        float a = hashf(i, j, s) * 2 * PI;
        return std::cos(a) * dx + std::sin(a) * dy;
    };
    float u = fx * fx * fx * (fx * (fx * 6 - 15) + 10), v = fy * fy * fy * (fy * (fy * 6 - 15) + 10);
    float n00 = grad(xi, yi, fx, fy), n10 = grad(xi + 1, yi, fx - 1, fy);
    float n01 = grad(xi, yi + 1, fx, fy - 1), n11 = grad(xi + 1, yi + 1, fx - 1, fy - 1);
    float a = n00 + (n10 - n00) * u, b = n01 + (n11 - n01) * u;
    return (a + (b - a) * v) * 1.41f;
}
// fractal sum in about -1..1
inline float fbm(float x, float y, int oct, uint32_t s, float lac = 2.03f, float gain = 0.5f) {
    float sum = 0, amp = 1, norm = 0;
    for (int i = 0; i < oct; ++i) {
        sum += gnoise(x, y, s + uint32_t(i) * 101u) * amp;
        norm += amp;
        float nx = x * 0.8f - y * 0.6f, ny = x * 0.6f + y * 0.8f;
        x = nx * lac; y = ny * lac;
        amp *= gain;
    }
    return sum / norm;
}
// ridged multifractal in 0..1 (sharp crests)
inline float ridged(float x, float y, int oct, uint32_t s) {
    float sum = 0, amp = 0.5f, w = 1, norm = 0;
    for (int i = 0; i < oct; ++i) {
        float r = 1.f - std::fabs(gnoise(x, y, s + uint32_t(i) * 131u));
        r *= r;
        r *= w;
        w = std::clamp(r * 1.8f, 0.f, 1.f);
        sum += r * amp;
        norm += amp;
        float nx = x * 0.8f - y * 0.6f, ny = x * 0.6f + y * 0.8f;
        x = nx * 2.1f; y = ny * 2.1f;
        amp *= 0.5f;
    }
    return sum / norm;
}
inline float sstep(float a, float b, float x) {
    float t = std::clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3 - 2 * t);
}
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float smin(float a, float b, float k) {
    float h = std::max(k - std::fabs(a - b), 0.f) / k;
    return std::min(a, b) - h * h * k * 0.25f;
}

template <class F>
void parallelFor(int n, F&& fn) {
    unsigned T = std::max(1u, std::min(16u, std::thread::hardware_concurrency()));
    if (n < 64 || T == 1) { for (int i = 0; i < n; ++i) fn(i); return; }
    std::vector<std::thread> th;
    for (unsigned t = 0; t < T; ++t) th.emplace_back([&, t] { for (int i = int(t); i < n; i += int(T)) fn(i); });
    for (auto& x : th) x.join();
}

inline int64_t ckey(int a, int b) { return (int64_t(a) << 32) ^ int64_t(uint32_t(b)); }
constexpr float HASH = 64.f;   // spatial hash cell (m)

// distance from p to segment ab, with the parameter t
inline float segDist(float px, float pz, float ax, float az, float bx, float bz, float& t) {
    float ex = bx - ax, ez = bz - az;
    t = std::clamp(((px - ax) * ex + (pz - az) * ez) / std::max(1e-9f, ex * ex + ez * ez), 0.f, 1.f);
    float dx = ax + ex * t - px, dz = az + ez * t - pz;
    return std::sqrt(dx * dx + dz * dz);
}

}  // namespace wmap::detail
