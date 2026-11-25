// Development tool: writes a shaded relief map (PPM) of the terrain around a generated route.
//   terrain_dump out.ppm [terrain 0..4] [seed] [size_m] [res_m] [erode 0/1] [cx] [cz]
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "Course.h"
#include "Terrain.h"
#include "core/Route.h"

int main(int argc, char** argv) {
    const char* out = argc > 1 ? argv[1] : "relief.ppm";
    int ter = argc > 2 ? std::atoi(argv[2]) : 2;
    int seed = argc > 3 ? std::atoi(argv[3]) : 3;
    float size = argc > 4 ? float(std::atof(argv[4])) : 4000.f;
    float res = argc > 5 ? float(std::atof(argv[5])) : 8.f;
    bool erode = argc > 6 ? std::atoi(argv[6]) != 0 : false;
    GenParams gp;
    gp.terrain = Terrain(ter);
    gp.distanceKm = 40;
    gp.targetGainM = suggestedGain(gp.terrain, gp.distanceKm);
    gp.seed = uint64_t(seed);
    gp.finish = Finish::Loop;
    Route r = generateRoute(gp, "dump");
    Course c;
    c.build(r, uint32_t(seed));
    float cx = argc > 7 ? float(std::atof(argv[7])) : c.points()[0].pos.x;
    float cz = argc > 8 ? float(std::atof(argv[8])) : c.points()[0].pos.z;
    const auto& F = c.field();
    F.setErosion(erode, std::getenv("Q") ? std::atoi(std::getenv("Q")) : 1);
    int N = int(size / res);
    float x0 = cx - size / 2, z0 = cz - size / 2;
    auto t0 = std::chrono::steady_clock::now();
    if (erode) {
        int a0 = int(std::floor(x0 / terrain::TILE)), a1 = int(std::floor((x0 + size) / terrain::TILE));
        int b0 = int(std::floor(z0 / terrain::TILE)), b1 = int(std::floor((z0 + size) / terrain::TILE));
        std::vector<std::thread> th;
        unsigned nt = std::max(1u, std::thread::hardware_concurrency());
        std::vector<std::pair<int, int>> jobs;
        for (int b = b0; b <= b1; ++b) for (int a = a0; a <= a1; ++a) jobs.push_back({a, b});
        std::atomic<size_t> next{0};
        for (unsigned t = 0; t < nt; ++t)
            th.emplace_back([&] { for (size_t j; (j = next++) < jobs.size();) F.prepareTile(jobs[j].first, jobs[j].second); });
        for (auto& t : th) t.join();
    }
    auto t1 = std::chrono::steady_clock::now();
    terrain::writeRelief(F, x0, z0, size, res, out);
    int nans = 0;
    for (int k = 0; k < 100; ++k) for (int i = 0; i < 100; ++i) nans += std::isnan(F.height(x0 + i * size / 100, z0 + k * size / 100));
    std::printf("nan samples: %d / 10000\n", nans);
    auto t2 = std::chrono::steady_clock::now();
    float mn = 0, mx = 0;
    auto st = F.stats();
    std::printf("range %.0f..%.0f m, erosion %.0f ms wall (%d windows, %.0f ms cpu), sampling %.0f ms (%.2f us/sample), %d nodes %d cells, %zu bridges near start\n",
                mn, mx, std::chrono::duration<double, std::milli>(t1 - t0).count(), st.windows, st.erodeMs,
                std::chrono::duration<double, std::milli>(t2 - t1).count(),
                std::chrono::duration<double, std::micro>(t2 - t1).count() / (double(N) * N), st.nodes, st.cells,
                F.bridgesNear(cx, cz, size).size());
}
