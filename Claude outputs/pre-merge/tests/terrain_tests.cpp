// Terrain field: determinism, order independence of erosion tiles, seamless tile blending.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <thread>

#include "Course.h"
#include "Terrain.h"
#include "core/Route.h"

static int fails = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++fails; std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); } } while (0)

static Route makeRoute(int seed) {
    GenParams gp;
    gp.terrain = Terrain::Hilly;
    gp.distanceKm = 20;
    gp.targetGainM = suggestedGain(gp.terrain, gp.distanceKm);
    gp.seed = uint64_t(seed);
    return generateRoute(gp, "t");
}

int main() {
    Route r = makeRoute(5);
    Course a, b;
    a.build(r, 5);
    b.build(r, 5);
    const auto& FA = a.field();
    const auto& FB = b.field();
    float x0 = a.points()[0].pos.x, z0 = a.points()[0].pos.z;
    int tx = int(std::floor(x0 / terrain::TILE)), tz = int(std::floor(z0 / terrain::TILE));

    // macro field: identical across instances and query order
    float maxd = 0;
    for (int k = 0; k < 40; ++k)
        for (int i = 0; i < 40; ++i) {
            float x = x0 - 800 + i * 41.3f, z = z0 - 800 + k * 39.7f;
            FB.macro(z, x);  // touch B's caches in a different order first
            maxd = std::max(maxd, std::fabs(FA.macro(x, z).h - FB.macro(x, z).h));
        }
    CHECK(maxd == 0.f);

    // erosion: A builds tiles left-to-right on two threads, B right-to-left on one
    {
        std::thread t1([&] { FA.prepareTile(tx, tz); });
        std::thread t2([&] { FA.prepareTile(tx + 1, tz); });
        t1.join(); t2.join();
        FB.prepareTile(tx + 1, tz);
        FB.prepareTile(tx, tz);
    }
    CHECK(FA.tileReady(tx, tz) && FB.tileReady(tx + 1, tz));
    maxd = 0;
    float maxDisp = 0;
    for (int k = 0; k < 64; ++k)
        for (int i = 0; i < 128; ++i) {
            float x = tx * terrain::TILE + i * 8.f + 1.3f, z = tz * terrain::TILE + k * 8.f + 2.1f;
            float ha = FA.height(x, z), hb = FB.height(x, z);
            maxd = std::max(maxd, std::fabs(ha - hb));
            maxDisp = std::max(maxDisp, std::fabs(ha - FA.macro(x, z).h));
        }
    CHECK(maxd == 0.f);
    CHECK(maxDisp > 0.05f);  // erosion did something

    // no seams: the eroded surface is continuous across the tile boundary
    float worst = 0;
    float bx = (tx + 1) * terrain::TILE;
    for (int k = 0; k < 200; ++k) {
        float z = tz * terrain::TILE + k * 2.5f;
        float l = FA.height(bx - 0.002f, z), rr = FA.height(bx + 0.002f, z);
        worst = std::max(worst, std::fabs(l - rr));
    }
    std::printf("seam step max %.4f m, erosion max %.2f m\n", worst, maxDisp);
    CHECK(worst < 0.01f);

    // the road surface is never disturbed by rivers or erosion
    float worstRoad = 0;
    for (size_t i = 0; i < a.points().size(); i += 7) {
        const auto& p = a.points()[i].pos;
        if (std::fabs(p.x - x0) > 900 || std::fabs(p.z - z0) > 900) continue;
        if (!FA.bridgesNear(p.x, p.z, 40.f).empty()) continue;  // rivers pass under bridges
        float dev = std::fabs(FA.height(p.x, p.z) - (p.y - 0.3f));
        if (dev > 0.4f) std::printf("  road dev %.2f at %zu (%.0f,%.0f) macro %.2f road %.2f\n", dev, i, p.x, p.z, FA.macro(p.x, p.z).h, p.y);
        worstRoad = std::max(worstRoad, dev);
    }
    std::printf("road deviation max %.3f m\n", worstRoad);
    CHECK(worstRoad < 0.4f);

    std::printf("terrain tests: %d/%d passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
