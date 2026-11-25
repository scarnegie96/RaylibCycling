#include "Textures.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <thread>
#include <vector>

namespace tex {
namespace {

struct Rng {
    uint32_t s;
    explicit Rng(uint32_t seed) : s(seed * 747796405u + 2891336453u) {}
    float f() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return (s & 0xFFFFFF) / float(0x1000000); }
    float r(float a, float b) { return a + (b - a) * f(); }
};

float hashi(int x, int y, uint32_t seed) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xFFFFFF) / float(0x1000000);
}
// value noise that tiles with the given period (in lattice cells)
float tnoise(float x, float y, int period, uint32_t seed) {
    int xi = int(std::floor(x)), yi = int(std::floor(y));
    float fx = x - xi, fy = y - yi;
    float ux = fx * fx * (3 - 2 * fx), uy = fy * fy * (3 - 2 * fy);
    auto H = [&](int a, int b) { return hashi(((a % period) + period) % period, ((b % period) + period) % period, seed); };
    float a = H(xi, yi), b = H(xi + 1, yi), c = H(xi, yi + 1), d = H(xi + 1, yi + 1);
    return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
}
float tfbm(float u, float v, int base, int oct, uint32_t seed) {  // u,v in 0..1
    float s = 0, amp = 0.5f, norm = 0;
    int p = base;
    for (int i = 0; i < oct; ++i) {
        s += tnoise(u * p, v * p, p, seed + i * 31) * amp;
        norm += amp;
        amp *= 0.5f;
        p *= 2;
    }
    return s / norm;
}

// ---- gradient + cellular noise (tiling), domain warping: the TerraForged toolbox in 2D
float grad2(int x, int y, int period, uint32_t seed, float dx, float dy) {
    float a = hashi(((x % period) + period) % period, ((y % period) + period) % period, seed) * 6.2831853f;
    return std::cos(a) * dx + std::sin(a) * dy;
}
float gnoise(float x, float y, int period, uint32_t seed) {   // ~[-0.7, 0.7]
    int xi = int(std::floor(x)), yi = int(std::floor(y));
    float fx = x - xi, fy = y - yi;
    float ux = fx * fx * fx * (fx * (fx * 6 - 15) + 10), uy = fy * fy * fy * (fy * (fy * 6 - 15) + 10);
    float a = grad2(xi, yi, period, seed, fx, fy), b = grad2(xi + 1, yi, period, seed, fx - 1, fy);
    float c = grad2(xi, yi + 1, period, seed, fx, fy - 1), d = grad2(xi + 1, yi + 1, period, seed, fx - 1, fy - 1);
    return a + (b - a) * ux + (c - a) * uy + (a - b - c + d) * ux * uy;
}
float gfbm(float u, float v, int base, int oct, uint32_t seed, float gain = 0.5f) {  // 0..1
    float s = 0, amp = 1, norm = 0;
    int p = base;
    for (int i = 0; i < oct; ++i) { s += gnoise(u * p, v * p, p, seed + i * 17) * amp; norm += amp; amp *= gain; p *= 2; }
    return std::clamp(s / norm * 1.1f + 0.5f, 0.f, 1.f);
}
// domain-warped fbm: organic, flowing variation instead of round blobs
float warped(float u, float v, int base, int oct, uint32_t seed, float amt) {
    float wu = gfbm(u, v, base, 3, seed + 101) - 0.5f, wv = gfbm(u, v, base, 3, seed + 202) - 0.5f;
    return gfbm(u + wu * amt, v + wv * amt, base, oct, seed);
}
struct Cell { float f1, f2; int id; float cx, cy; };  // distances to nearest two sites, nearest site
Cell cellular(float u, float v, int cells, uint32_t seed, float jitter = 0.9f) {
    float x = u * cells, y = v * cells;
    int xi = int(std::floor(x)), yi = int(std::floor(y));
    Cell c{9, 9, 0, 0, 0};
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx) {
            int cx = xi + dx, cy = yi + dy;
            int wx = ((cx % cells) + cells) % cells, wy = ((cy % cells) + cells) % cells;
            float px = cx + 0.5f + (hashi(wx, wy, seed) - 0.5f) * jitter, py = cy + 0.5f + (hashi(wx, wy, seed + 7) - 0.5f) * jitter;
            float d = std::sqrt((px - x) * (px - x) + (py - y) * (py - y));
            if (d < c.f1) { c.f2 = c.f1; c.f1 = d; c.id = wy * cells + wx; c.cx = px - x; c.cy = py - y; }
            else if (d < c.f2) c.f2 = d;
        }
    return c;
}
template <class F>
void rows(int n, F&& fn) {
    unsigned T = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> th;
    for (unsigned t = 0; t < T; ++t) th.emplace_back([&, t] { for (int y = int(t); y < n; y += int(T)) fn(y); });
    for (auto& x : th) x.join();
}

struct Img {
    int n;
    std::vector<float> r, g, b, h;
    explicit Img(int size) : n(size), r(size * size), g(size * size), b(size * size), h(size * size) {}
    int idx(int x, int y) const { return ((y % n + n) % n) * n + ((x % n + n) % n); }
    Texture2D upload(bool mips) const {
        std::vector<unsigned char> px(size_t(n) * n * 4);
        for (int i = 0; i < n * n; ++i) {
            px[i * 4 + 0] = (unsigned char)std::clamp(r[i] * 255.f, 0.f, 255.f);
            px[i * 4 + 1] = (unsigned char)std::clamp(g[i] * 255.f, 0.f, 255.f);
            px[i * 4 + 2] = (unsigned char)std::clamp(b[i] * 255.f, 0.f, 255.f);
            px[i * 4 + 3] = (unsigned char)std::clamp(h[i] * 255.f, 0.f, 255.f);
        }
        Image im{px.data(), n, n, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        Texture2D t = LoadTextureFromImage(im);
        if (mips) {
            GenTextureMipmaps(&t);
            SetTextureFilter(t, TEXTURE_FILTER_ANISOTROPIC_16X);
        } else SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
        SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
        return t;
    }
};

// ---------------------------------------------------------------- grass: thousands of blades seen from above
Texture2D makeGrass(int n) {
    Img im(n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            float u = float(x) / n, v = float(y) / n;
            float m = tfbm(u, v, 8, 4, 11);
            float soil = tfbm(u, v, 32, 3, 12);
            int i = y * n + x;
            // dark earthy thatch underneath
            im.r[i] = 0.10f + 0.06f * soil; im.g[i] = 0.12f + 0.08f * m; im.b[i] = 0.04f + 0.02f * soil;
            im.h[i] = 0.08f * soil;
        }
    Rng rng(7);
    int blades = n * n / 5;
    for (int k = 0; k < blades; ++k) {
        float x0 = rng.f() * n, y0 = rng.f() * n;
        float ang = rng.f() * 6.2832f;
        float len = rng.r(0.012f, 0.032f) * n;
        float tall = rng.r(0.5f, 1.f);
        float w = rng.r(0.6f, 1.4f) * n / 1024.f + 0.5f;
        // hue: mostly fresh green, some yellow-green, a few dry straw blades
        float hue = rng.f();
        float cr, cg, cb;
        if (hue < 0.62f) { cr = rng.r(0.10f, 0.20f); cg = rng.r(0.30f, 0.44f); cb = rng.r(0.05f, 0.10f); }
        else if (hue < 0.9f) { cr = rng.r(0.22f, 0.32f); cg = rng.r(0.38f, 0.48f); cb = rng.r(0.07f, 0.12f); }
        else { cr = rng.r(0.42f, 0.55f); cg = rng.r(0.40f, 0.50f); cb = rng.r(0.18f, 0.25f); }
        float dx = std::cos(ang), dy = std::sin(ang);
        int steps = int(len * 1.5f) + 1;
        for (int s = 0; s <= steps; ++s) {
            float t = float(s) / steps;
            float bend = t * t * 0.25f * len;  // slight curl
            float px = x0 + dx * len * t - dy * bend, py = y0 + dy * len * t + dx * bend;
            float hh = tall * (0.25f + 0.75f * t);
            float lit = 0.55f + 0.65f * t;   // blades get lighter towards the tip
            float ww = w * (1.f - 0.7f * t);
            int r = int(std::ceil(ww));
            for (int oy = -r; oy <= r; ++oy)
                for (int ox = -r; ox <= r; ++ox) {
                    float d = std::sqrt(float(ox * ox + oy * oy));
                    if (d > ww + 0.5f) continue;
                    int i = im.idx(int(px) + ox, int(py) + oy);
                    float hv = hh * (1.f - 0.3f * d / (ww + 0.5f));
                    if (hv <= im.h[i]) continue;  // nearer the eye wins
                    im.h[i] = hv;
                    im.r[i] = cr * lit; im.g[i] = cg * lit; im.b[i] = cb * lit;
                }
        }
    }
    return im.upload(true);
}

// ---------------------------------------------------------------- asphalt: bitumen with aggregate
Texture2D makeAsphalt(int n) {
    Img im(n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            float u = float(x) / n, v = float(y) / n;
            float m = tfbm(u, v, 16, 4, 21), f = tnoise(u * 256, v * 256, 256, 22);
            float g = 0.085f + 0.035f * m + 0.02f * f;
            int i = y * n + x;
            im.r[i] = g; im.g[i] = g; im.b[i] = g * 1.04f;
            im.h[i] = 0.18f + 0.1f * f;
        }
    Rng rng(3);
    int stones = n * n / 22;
    for (int k = 0; k < stones; ++k) {
        float cx = rng.f() * n, cy = rng.f() * n;
        float rad = rng.r(0.7f, 3.2f) * n / 1024.f + 0.4f;
        float ex = rng.r(0.7f, 1.3f), rot = rng.f() * 6.28f;
        float pick = rng.f();
        float c = pick < 0.08f ? rng.r(0.45f, 0.62f) : pick < 0.4f ? rng.r(0.24f, 0.36f) : rng.r(0.14f, 0.24f);
        float tint = rng.r(-0.02f, 0.02f);
        int r = int(std::ceil(rad * 1.4f)) + 1;
        for (int oy = -r; oy <= r; ++oy)
            for (int ox = -r; ox <= r; ++ox) {
                float lx = ox * std::cos(rot) + oy * std::sin(rot), ly = (-ox * std::sin(rot) + oy * std::cos(rot)) * ex;
                float d = std::sqrt(lx * lx + ly * ly) / rad;
                if (d > 1) continue;
                int i = im.idx(int(cx) + ox, int(cy) + oy);
                float dome = std::sqrt(1 - d * d);
                float hv = 0.35f + 0.6f * dome;
                if (hv <= im.h[i]) continue;
                im.h[i] = hv;
                float sh = 0.8f + 0.25f * dome;
                im.r[i] = (c + tint) * sh; im.g[i] = c * sh; im.b[i] = (c - tint) * sh * 1.02f;
            }
    }
    return im.upload(true);
}

// ---------------------------------------------------------------- gravel: rounded pebbles on dusty ground
Texture2D makeGravel(int n) {
    Img im(n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            float u = float(x) / n, v = float(y) / n;
            float m = tfbm(u, v, 8, 4, 31);
            int i = y * n + x;
            im.r[i] = 0.30f + 0.08f * m; im.g[i] = 0.26f + 0.07f * m; im.b[i] = 0.20f + 0.05f * m;
            im.h[i] = 0.1f;
        }
    Rng rng(5);
    int stones = n * n / 26;
    for (int k = 0; k < stones; ++k) {
        float cx = rng.f() * n, cy = rng.f() * n;
        float rad = rng.r(2.5f, 9.f) * n / 512.f;
        float ex = rng.r(0.65f, 1.f), rot = rng.f() * 6.28f;
        float pick = rng.f();
        float cr, cg, cb;
        if (pick < 0.45f) { float g = rng.r(0.38f, 0.6f); cr = g; cg = g * 0.98f; cb = g * 0.95f; }      // grey limestone
        else if (pick < 0.8f) { cr = rng.r(0.5f, 0.66f); cg = rng.r(0.44f, 0.56f); cb = rng.r(0.32f, 0.42f); }  // beige
        else { cr = rng.r(0.35f, 0.48f); cg = rng.r(0.26f, 0.34f); cb = rng.r(0.18f, 0.24f); }             // brown
        int r = int(std::ceil(rad)) + 1;
        for (int oy = -r; oy <= r; ++oy)
            for (int ox = -r; ox <= r; ++ox) {
                float lx = ox * std::cos(rot) + oy * std::sin(rot), ly = (-ox * std::sin(rot) + oy * std::cos(rot)) / ex;
                float d = std::sqrt(lx * lx + ly * ly) / rad;
                if (d > 1) continue;
                int i = im.idx(int(cx) + ox, int(cy) + oy);
                float dome = std::sqrt(1 - d * d);
                float hv = 0.3f + 0.7f * dome * (rad / (9.f * n / 512.f) * 0.5f + 0.5f);
                if (hv <= im.h[i]) continue;
                im.h[i] = hv;
                // light from the top-left, a little ambient occlusion at the rim
                float lit = 0.62f + 0.38f * dome + 0.15f * (-lx - ly) / (rad * 1.4f);
                im.r[i] = cr * lit; im.g[i] = cg * lit; im.b[i] = cb * lit;
            }
    }
    return im.upload(true);
}

// ---------------------------------------------------------------- rock: bedded sandstone / limestone
// Strata (bands that wobble), joints that break them into blocks, weathered edges, lichen.
Texture2D makeRock(int n) {
    Img im(n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            float u = float(x) / n, v = float(y) / n;
            float warp = tfbm(u, v, 4, 4, 51);
            float bed = v * 9.f + warp * 2.2f;                      // 9 beds per tile, warped
            float band = bed - std::floor(bed);
            int bedId = int(std::floor(bed));
            float bedTone = hashi(bedId, 0, 52);
            // vertical joints, offset per bed
            float ju = u * (4 + (bedId & 1) * 2) + hashi(bedId, 1, 53) * 3.f + tfbm(u, v, 16, 2, 54) * 0.25f;
            float joint = std::fabs(ju - std::round(ju));
            float grain = tfbm(u, v, 64, 3, 55), fine = tfbm(u, v, 128, 2, 56), blotch = tfbm(u, v, 8, 4, 57);
            float crack = std::min(band, 1.f - band);              // bedding planes
            float edge = std::min(crack * 9.f, 1.f) * std::min(joint * 22.f, 1.f);
            int i = y * n + x;
            float g = 0.42f + 0.16f * bedTone + 0.12f * (grain - 0.5f) + 0.06f * (fine - 0.5f);
            float r = g * (1.04f + 0.08f * bedTone), gg = g * (0.99f + 0.02f * bedTone), b = g * (0.92f - 0.05f * bedTone);
            // weathering: darker, iron-stained hollows
            float stain = std::clamp((blotch - 0.55f) * 3.f, 0.f, 1.f);
            r = r * (1 - 0.25f * stain) + 0.05f * stain; gg *= 1 - 0.3f * stain; b *= 1 - 0.35f * stain;
            // lichen patches (grey-green and orange)
            float lich = std::clamp((tfbm(u, v, 32, 3, 58) - 0.66f) * 6.f, 0.f, 1.f) * (0.5f + 0.5f * edge);
            float lichO = std::clamp((tfbm(u, v, 32, 3, 59) - 0.74f) * 6.f, 0.f, 1.f);
            r = r * (1 - lich) + 0.48f * lich; gg = gg * (1 - lich) + 0.52f * lich; b = b * (1 - lich) + 0.40f * lich;
            r = r * (1 - lichO * 0.7f) + 0.62f * lichO * 0.7f; gg = gg * (1 - lichO * 0.7f) + 0.40f * lichO * 0.7f; b = b * (1 - lichO * 0.7f) + 0.18f * lichO * 0.7f;
            float ao = 0.45f + 0.55f * edge;
            im.r[i] = r * ao; im.g[i] = gg * ao; im.b[i] = b * ao;
            im.h[i] = std::clamp(0.15f + 0.6f * edge * (0.75f + 0.25f * bedTone) + 0.2f * grain, 0.f, 1.f);
        }
    return im.upload(true);
}

// ---------------------------------------------------------------- sand / silt: river and lake sediment
Texture2D makeSand(int n) {
    Img im(n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            float u = float(x) / n, v = float(y) / n;
            float rip = std::sin((v * 14.f + tfbm(u, v, 4, 3, 61) * 3.f) * 6.2831853f) * 0.5f + 0.5f;  // current ripples
            float grain = hashi(x, y, 62), g2 = tfbm(u, v, 128, 2, 63), m = tfbm(u, v, 8, 4, 64);
            int i = y * n + x;
            float base = 0.52f + 0.1f * (m - 0.5f) + 0.08f * (grain - 0.5f) + 0.05f * (g2 - 0.5f);
            float silt = std::clamp((m - 0.55f) * 3.f, 0.f, 1.f);  // muddier patches
            im.r[i] = base * (1.02f - 0.25f * silt); im.g[i] = base * (0.93f - 0.22f * silt); im.b[i] = base * (0.74f - 0.15f * silt);
            im.h[i] = 0.35f + 0.35f * rip * (1 - silt) + 0.1f * grain;
        }
    // a scatter of small pebbles and shell grit
    Rng rng(7);
    int stones = n * n / 400;
    for (int k = 0; k < stones; ++k) {
        float cx = rng.f() * n, cy = rng.f() * n, rad = rng.r(1.2f, 4.f) * n / 512.f;
        float g = rng.r(0.3f, 0.62f);
        int r = int(std::ceil(rad)) + 1;
        for (int oy = -r; oy <= r; ++oy)
            for (int ox = -r; ox <= r; ++ox) {
                float d = std::sqrt(float(ox * ox + oy * oy)) / rad;
                if (d > 1) continue;
                int i = im.idx(int(cx) + ox, int(cy) + oy);
                float dome = std::sqrt(1 - d * d);
                im.r[i] = g * (0.7f + 0.3f * dome); im.g[i] = g * 0.95f * (0.7f + 0.3f * dome); im.b[i] = g * 0.88f * (0.7f + 0.3f * dome);
                im.h[i] = std::max(im.h[i], 0.6f + 0.4f * dome);
            }
    }
    return im.upload(true);
}

// ---------------------------------------------------------------- soil: bare, eroded earth
Texture2D makeSoil(int n) {
    Img im(n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            float u = float(x) / n, v = float(y) / n;
            float m = tfbm(u, v, 8, 5, 71), c = tfbm(u, v, 32, 3, 72), f = hashi(x, y, 73);
            // desiccation cracks / rills
            float w = tfbm(u, v, 16, 3, 74);
            float rill = std::clamp(1.f - std::fabs(w - 0.5f) * 24.f, 0.f, 1.f);
            int i = y * n + x;
            float base = 0.30f + 0.10f * (m - 0.5f) + 0.05f * (f - 0.5f);
            float red = std::clamp((c - 0.5f) * 2.f, 0.f, 1.f);
            im.r[i] = base * (1.25f + 0.15f * red); im.g[i] = base * (0.98f - 0.05f * red); im.b[i] = base * 0.70f;
            float ao = 1.f - 0.5f * rill;
            im.r[i] *= ao; im.g[i] *= ao; im.b[i] *= ao;
            im.h[i] = 0.5f + 0.25f * (m - 0.5f) - 0.35f * rill + 0.1f * f;
        }
    Rng rng(9);
    int stones = n * n / 160;
    for (int k = 0; k < stones; ++k) {
        float cx = rng.f() * n, cy = rng.f() * n, rad = rng.r(1.5f, 6.f) * n / 512.f;
        float g = rng.r(0.32f, 0.55f);
        int r = int(std::ceil(rad)) + 1;
        for (int oy = -r; oy <= r; ++oy)
            for (int ox = -r; ox <= r; ++ox) {
                float d = std::sqrt(float(ox * ox + oy * oy)) / rad;
                if (d > 1) continue;
                int i = im.idx(int(cx) + ox, int(cy) + oy);
                float dome = std::sqrt(1 - d * d);
                float lit = 0.6f + 0.4f * dome;
                im.r[i] = g * lit; im.g[i] = g * 0.96f * lit; im.b[i] = g * 0.9f * lit;
                im.h[i] = std::max(im.h[i], 0.55f + 0.45f * dome);
            }
    }
    return im.upload(true);
}


// ---------------------------------------------------------------- granite: crystalline, jointed into blocks
// Two scales of warped Voronoi joints break it into blocks; the surface is speckled with
// feldspar, quartz and biotite crystals; patina and lichen weather it.
Texture2D makeGranite(int n) {
    Img im(n);
    const int crystals = std::max(32, n / 6);
    rows(n, [&](int y) {
        for (int x = 0; x < n; ++x) {
            float u = float(x) / n, v = float(y) / n;
            float wu = (gfbm(u, v, 4, 3, 301) - 0.5f) * 0.09f, wv = (gfbm(u, v, 4, 3, 302) - 0.5f) * 0.09f;
            Cell a = cellular(u + wu, v + wv, 5, 311, 0.85f);
            Cell b = cellular(u + wu * 1.5f, v + wv * 1.5f, 13, 312, 0.9f);
            float joint = std::clamp((a.f2 - a.f1) * 7.f, 0.f, 1.f);
            float joint2 = std::clamp((b.f2 - b.f1) * 9.f, 0.f, 1.f);
            float tone = hashi(a.id, 0, 321);
            Cell k = cellular(u, v, crystals, 331, 1.f);
            float m = hashi(k.id, 1, 332);
            float cr, cg, cb;
            if (m < 0.30f) { cr = 0.70f; cg = 0.54f; cb = 0.47f; }        // pink K-feldspar
            else if (m < 0.55f) { cr = 0.80f; cg = 0.79f; cb = 0.76f; }   // quartz
            else if (m < 0.68f) { cr = 0.14f; cg = 0.13f; cb = 0.13f; }   // biotite
            else { cr = 0.60f; cg = 0.60f; cb = 0.57f; }                  // plagioclase
            float facet = 0.85f + 0.15f * std::clamp(1.f - k.f1 * 1.4f, 0.f, 1.f);
            float base = 0.5f + 0.12f * (tone - 0.5f);
            float r = (cr * 0.55f + base * 0.45f) * facet, g = (cg * 0.55f + base * 0.97f * 0.45f) * facet, bb = (cb * 0.55f + base * 0.93f * 0.45f) * facet;
            // warm iron patina in hollows, cool grey on exposed faces
            float pat = warped(u, v, 3, 4, 341, 0.7f);
            float stain = std::clamp((pat - 0.55f) * 3.f, 0.f, 1.f);
            r = r * (1 - 0.3f * stain) + 0.32f * 0.3f * stain; g = g * (1 - 0.35f * stain) + 0.2f * 0.35f * stain; bb = bb * (1 - 0.4f * stain) + 0.12f * 0.4f * stain;
            float lich = std::clamp((warped(u, v, 8, 3, 351, 0.5f) - 0.64f) * 5.f, 0.f, 1.f) * joint;
            r = r * (1 - lich) + 0.56f * lich; g = g * (1 - lich) + 0.6f * lich; bb = bb * (1 - lich) + 0.47f * lich;
            float ao = (0.35f + 0.65f * joint) * (0.75f + 0.25f * joint2);
            int i = y * n + x;
            im.r[i] = r * ao; im.g[i] = g * ao; im.b[i] = bb * ao;
            float dome = std::clamp(1.f - a.f1 * 0.7f, 0.f, 1.f);
            im.h[i] = std::clamp(0.12f + 0.55f * joint * (0.6f + 0.4f * dome) * (0.7f + 0.3f * joint2) + 0.08f * facet, 0.f, 1.f);
        }
    });
    return im.upload(true);
}

// ---------------------------------------------------------------- scree: angular broken rock
// Faceted stones (each Voronoi cell lit as a few flat faces) over a bed of smaller chips and
// dark fines in the gaps.
Texture2D makeScree(int n) {
    Img im(n);
    rows(n, [&](int y) {
        for (int x = 0; x < n; ++x) {
            float u = float(x) / n, v = float(y) / n;
            float wu = (gfbm(u, v, 8, 2, 401) - 0.5f) * 0.03f, wv = (gfbm(u, v, 8, 2, 402) - 0.5f) * 0.03f;
            float r = 0.15f, g = 0.12f, b = 0.10f, h = 0.05f;   // fines between the stones
            for (int layer = 1; layer >= 0; --layer) {           // small chips first, big stones on top
                int cells = layer ? 41 : 17;
                Cell c = cellular(u + wu, v + wv, cells, 411 + layer, 0.95f);
                float gap = c.f2 - c.f1;
                float edge = std::clamp((gap - 0.04f) * 14.f, 0.f, 1.f);
                if (edge <= 0.f) continue;
                float tone = hashi(c.id, 3, 421 + layer), kind = hashi(c.id, 5, 431 + layer);
                float sr, sg, sb;
                if (kind < 0.5f) { sr = sg = sb = 0.42f + 0.2f * tone; sb *= 0.97f; }         // grey
                else if (kind < 0.8f) { sr = 0.30f + 0.1f * tone; sg = sr * 0.98f; sb = sr * 1.04f; }  // dark slate
                else { sr = 0.52f + 0.1f * tone; sg = sr * 0.86f; sb = sr * 0.68f; }           // ochre
                // facets: quantise the direction to the stone's centre into a few flat faces
                float ang = std::atan2(c.cy, c.cx) + hashi(c.id, 9, 441) * 6.2832f;
                float faces = 4.f + std::floor(hashi(c.id, 11, 451) * 3.f);
                float fa = std::floor(ang / 6.2832f * faces) / faces * 6.2832f;
                float lit = 0.72f + 0.3f * std::cos(fa - 2.3f) * std::clamp(c.f1 * 2.5f, 0.f, 1.f);
                float hh = (layer ? 0.35f : 0.6f) + 0.35f * std::clamp(gap * 3.f, 0.f, 1.f);
                if (hh * edge <= h) continue;
                r = sr * lit * (0.55f + 0.45f * edge); g = sg * lit * (0.55f + 0.45f * edge); b = sb * lit * (0.55f + 0.45f * edge);
                h = hh * edge;
            }
            int i = y * n + x;
            im.r[i] = r; im.g[i] = g; im.b[i] = b; im.h[i] = h;
        }
    });
    return im.upload(true);
}

// ---------------------------------------------------------------- moorland: heather, moss, bracken, bilberry
// Clumps of each plant, chosen by large warped patch fields, over dark peat.
Texture2D makeMoor(int n) {
    Img im(n);
    rows(n, [&](int y) {
        for (int x = 0; x < n; ++x) {
            float u = float(x) / n, v = float(y) / n;
            float heather = warped(u, v, 2, 4, 501, 0.9f), moss = warped(u, v, 2, 4, 502, 0.9f), bracken = warped(u, v, 3, 4, 503, 0.8f);
            float r = 0.11f, g = 0.085f, b = 0.065f, h = 0.05f;   // peat
            for (int layer = 0; layer < 2; ++layer) {
                Cell c = cellular(u, v, layer ? 150 : 70, 511 + layer, 0.95f);
                float rad = layer ? 0.62f : 0.72f;
                float dome = std::clamp(1.f - c.f1 / rad, 0.f, 1.f);
                if (dome <= 0.f) continue;
                float pick = hashi(c.id, 1, 521 + layer), t = hashi(c.id, 2, 531 + layer);
                float wh = heather * 1.2f, wm = moss, wb = bracken * 0.9f, wl = 0.35f;
                float sum = wh + wm + wb + wl, p = pick * sum;
                float cr, cg, cb, ch;
                if (p < wh) { cr = 0.30f + 0.12f * t; cg = 0.19f + 0.04f * t; cb = 0.24f + 0.12f * t; ch = 0.8f; }         // heather, purple-brown
                else if (p < wh + wm) { cr = 0.27f + 0.1f * t; cg = 0.32f + 0.1f * t; cb = 0.11f + 0.03f * t; ch = 0.4f; } // moss
                else if (p < wh + wm + wb) { cr = 0.47f + 0.1f * t; cg = 0.27f + 0.05f * t; cb = 0.11f; ch = 0.9f; }      // dead bracken
                else { cr = 0.17f + 0.05f * t; cg = 0.29f + 0.06f * t; cb = 0.11f; ch = 0.6f; }                           // bilberry
                float fine = 0.8f + 0.4f * gnoise(u * 512.f, v * 512.f, 512, 541 + layer);
                float lit = (0.5f + 0.6f * dome) * fine;
                float hh = ch * (0.35f + 0.65f * dome) * (layer ? 0.7f : 1.f);
                if (hh <= h) continue;
                r = cr * lit; g = cg * lit; b = cb * lit; h = hh;
            }
            int i = y * n + x;
            im.r[i] = r; im.g[i] = g; im.b[i] = b; im.h[i] = h;
        }
    });
    return im.upload(true);
}

Texture2D sprite(int w, int h, float (*fn)(float u, float v)) {
    std::vector<unsigned char> px(size_t(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float a = std::clamp(fn((x + 0.5f) / w, (y + 0.5f) / h), 0.f, 1.f);
            size_t i = (size_t(y) * w + x) * 4;
            px[i] = px[i + 1] = px[i + 2] = 255;
            px[i + 3] = (unsigned char)(a * 255);
        }
    Image im{px.data(), w, h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    Texture2D t = LoadTextureFromImage(im);
    GenTextureMipmaps(&t);
    SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
    SetTextureWrap(t, TEXTURE_WRAP_CLAMP);
    return t;
}

}  // namespace

Set generate(int quality) {
    Set s;
    int big = quality >= 2 ? 1024 : quality == 1 ? 1024 : 512;
    s.grass = makeGrass(big);
    s.asphalt = makeAsphalt(big);
    s.gravel = makeGravel(quality >= 1 ? 512 : 256);
    s.rock = makeRock(big);
    s.sand = makeSand(quality >= 1 ? 512 : 256);
    s.soil = makeSoil(quality >= 1 ? 512 : 256);
    s.granite = makeGranite(quality >= 1 ? 1024 : 512);
    s.scree = makeScree(quality >= 1 ? 1024 : 512);
    s.moor = makeMoor(quality >= 1 ? 1024 : 512);
    s.soft = sprite(64, 64, [](float u, float v) {
        float d = std::sqrt((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f)) * 2;
        return std::exp(-d * d * 4.5f) * (1 - std::clamp((d - 0.9f) * 10.f, 0.f, 1.f));
    });
    s.streak = sprite(16, 128, [](float u, float v) {
        float x = (u - 0.5f) * 2;
        float body = std::exp(-x * x * 6.f);
        float along = std::pow(std::sin(3.14159f * v), 0.7f) * (0.35f + 0.65f * v);  // brighter at the leading end
        return body * along;
    });
    s.ring = sprite(64, 64, [](float u, float v) {
        float d = std::sqrt((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f)) * 2;
        float ring = std::exp(-std::pow((d - 0.78f) / 0.09f, 2.f));
        return ring + 0.15f * std::exp(-d * d * 8.f);
    });
    return s;
}

void unload(Set& s) {
    for (Texture2D* t : {&s.grass, &s.asphalt, &s.gravel, &s.rock, &s.sand, &s.soil, &s.granite, &s.scree, &s.moor, &s.soft, &s.streak, &s.ring})
        if (t->id) { UnloadTexture(*t); *t = Texture2D{}; }
}

}  // namespace tex
