// World generator: determinism, physical sanity of rivers/lakes/roads, the network, loops, cache.
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <numeric>

#include "world/WorldMap.h"

using namespace wmap;
static int fails = 0, checks = 0;
#define CHECK(c, ...) do { ++checks; if (!(c)) { ++fails; std::printf("FAIL %s:%d  ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static void suite(uint32_t seed, float size, int climate = 0) {
    Params P;
    P.seed = seed;
    P.size = size;
    P.climate = climate;
    auto M = generate(P);
    std::printf("seed %u (%.0f km): %d rivers, %d lakes, %d salt flats, %d towns, %d roads %.0f km, %d bridges\n", seed, size / 1000, M->stats.rivers, M->stats.lakes,
                M->stats.playas, M->stats.towns, M->stats.roads, M->stats.roadKm, M->stats.bridges);

    // determinism
    auto M2 = generate(P);
    double a = 0, b = 0;
    for (size_t i = 0; i < M->h.size(); i += 97) { a += M->h[i]; b += M2->h[i]; }
    CHECK(a == b && M->roads.size() == M2->roads.size() && M->rivers.size() == M2->rivers.size(), "same seed, same world");

    // heights
    int bad = 0;
    float lo = 1e9f, hi = -1e9f;
    for (float v : M->h) { bad += !std::isfinite(v); lo = std::min(lo, v); hi = std::max(hi, v); }
    CHECK(bad == 0, "%d non-finite heights", bad);
    CHECK(lo > -200 && hi < 5000 && hi - lo > 150, "height range %.0f..%.0f", lo, hi);

    // biomes
    float worst = 0;
    for (const auto& w : M->biome) { float s = 0; for (float v : w) s += v; worst = std::max(worst, std::fabs(s - 1)); }
    CHECK(worst < 1e-3f, "biome weights sum off by %.4f", worst);

    // rivers run downhill, their water sits in a channel below the banks
    int uphill = 0, floating = 0, total = 0;
    for (const River& R : M->rivers) {
        for (size_t j = 1; j < R.pts.size(); ++j) uphill += R.pts[j].level > R.pts[j - 1].level + 1e-3f;
        if (R.dry) continue;
        for (size_t j = 0; j < R.pts.size(); j += 9) {
            ++total;
            Sample s = M->sample(R.pts[j].x, R.pts[j].z);
            if (s.road < 12.f) continue;   // bridges
            floating += !(s.water > s.h);
        }
    }
    CHECK(uphill == 0, "%d river steps uphill", uphill);
    CHECK(floating < total / 20 + 2, "%d of %d river samples without water over the bed", floating, total);

    // lakes: their cells are under the surface
    int dry = 0, cells = 0;
    for (size_t c = 0; c < M->lakeId.size(); ++c) {
        int l = M->lakeId[c];
        if (l < 0 || M->lakes[l].playa) continue;
        ++cells;
        dry += M->h[c] > M->lakes[l].level + 0.01f;
    }
    CHECK(dry == 0, "%d of %d lake cells above their lake", dry, cells);

    // the same scale at any map size: road per square km, towns per square km
    float area = (size / 1000) * (size / 1000);
    if (size > 9000) {
        CHECK(M->stats.roadKm / area > 0.12f && M->stats.roadKm / area < 0.8f, "%.2f km of road per km2", M->stats.roadKm / area);
        CHECK(M->stats.towns / area > 0.04f && M->stats.towns / area < 0.4f, "%.2f towns per km2", M->stats.towns / area);
    }
    if (M->roads.empty()) { CHECK(M->towns.size() < 2, "towns but no roads"); return; }
    // roads: within the map, grades held, never through open water
    // grades: held to the class cap almost everywhere (a road may get steeper rather than leave
    // the land, briefly), never a wall; and the road stays on (or bridges over) the land
    int steep = 0, wall = 0, steps = 0, wet = 0, pts = 0, outside = 0, adrift = 0;
    for (const Road& rd : M->roads) {
        float cap = rd.cls == 0 ? 0.12f : 0.15f;   // the profile caps (tunnels where the land is steeper)
        for (size_t j = 1; j < rd.pts.size(); ++j) {
            float ds = std::hypot(rd.pts[j].x - rd.pts[j - 1].x, rd.pts[j].z - rd.pts[j - 1].z);
            if (ds < 0.5f) continue;
            ++steps;
            float g = std::fabs(rd.pts[j].y - rd.pts[j - 1].y) / ds;
            steep += g > cap + 0.02f;
            wall += g > 0.3f;
        }
        for (const RoadPt& p : rd.pts) {
            if (p.kind != 0) continue;   // tunnels run under the land, bridges over it
            if (std::fabs(p.y - M->height(p.x, p.z)) < 12.f) continue;
            bool deck = false;
            for (const Bridge& br : M->bridges) deck |= std::hypot(br.x - p.x, br.z - p.z) < br.span * 0.5f + 40.f;
            adrift += !deck;
        }
        for (size_t j = 0; j < rd.pts.size(); j += 3) {
            outside += !M->inside(rd.pts[j].x, rd.pts[j].z);
            if (rd.pts[j].kind != 0) continue;
            ++pts;
            Sample s = M->sample(rd.pts[j].x, rd.pts[j].z);
            if (s.water > rd.pts[j].y - 0.5f) ++wet;
            // the road surface is the formation (except on bridge decks)
            bool deck = false;
            for (const Bridge& br : M->bridgesNear(rd.pts[j].x, rd.pts[j].z, 60.f)) deck |= std::hypot(br.x - rd.pts[j].x, br.z - rd.pts[j].z) < br.span * 0.5f + 4.f;
            if (!deck && s.road < 0.5f && std::fabs(s.h - (rd.pts[j].y - 0.3f)) > 0.6f) ++wet;
        }
    }
    CHECK(steep < steps / 50, "%d of %d road steps over the grade cap", steep, steps);
    CHECK(wall == 0, "%d road steps steeper than 30%%", wall);
    CHECK(adrift < steps / 150 + 1, "%d road points more than 12 m off the land away from bridges", adrift);
    CHECK(outside == 0, "%d road points off the map", outside);
    CHECK(wet < pts / 200 + 1, "%d of %d road samples awash or off the formation", wet, pts);
    for (const Bridge& br : M->bridges) CHECK(br.roadY > br.level + 2.f, "bridge deck %.1f over water %.1f", br.roadY, br.level);

    // one connected network
    std::vector<int> uf(M->nodes.size());
    std::iota(uf.begin(), uf.end(), 0);
    std::function<int(int)> f = [&](int x) { return uf[x] == x ? x : uf[x] = f(uf[x]); };
    for (const Road& r : M->roads) uf[f(r.a)] = f(r.b);
    std::map<int, int> comps;
    for (size_t i = 0; i < uf.size(); ++i) comps[f(int(i))]++;
    int biggest = 0;
    for (auto& [k, v] : comps) biggest = std::max(biggest, v);
    CHECK(biggest == int(uf.size()), "network in %zu pieces, the largest only %d of %zu nodes (every road must be reachable)", comps.size(), biggest, uf.size());
    // no dead ends: every node leads on (at least two road ends meet there)
    {
        int dead = 0;
        for (const Node& nd : M->nodes) dead += nd.roads.size() < 2;
        CHECK(dead == 0, "%d dead ends in the network (every road must lead on)", dead);
    }
    // grades where roads meet: every road at a junction meets it at one height
    for (size_t i = 0; i < M->nodes.size(); ++i) {
        float lo = 1e9f, hi = -1e9f;
        for (int r : M->nodes[i].roads) {
            const Road& rd = M->roads[r];
            if (rd.a == int(i)) { lo = std::min(lo, rd.pts.front().y); hi = std::max(hi, rd.pts.front().y); }
            if (rd.b == int(i)) { lo = std::min(lo, rd.pts.back().y); hi = std::max(hi, rd.pts.back().y); }
        }
        CHECK(hi - lo < 0.5f, "junction %zu: roads meet %.1f m apart", i, hi - lo);
    }
    // no stub roads between junctions
    for (const Road& rd : M->roads) CHECK(rd.length > 60.f, "a %.0f m stub road", rd.length);

    // a loop of about the length asked for, closed, on the roads
    float want = size > 15000 ? 30.f : 12.f;
    std::string name;
    auto L = M->loop(want, 7, 0, 0, &name);
    CHECK(L.size() > 10 || M->roads.size() < 3, "no loop");
    if (L.size() > 10) {
        float len = 0;
        float off = 0;
        for (size_t j = 1; j < L.size(); ++j) len += std::hypot(L[j].x - L[j - 1].x, L[j].z - L[j - 1].z);
        for (size_t j = 0; j < L.size(); j += 25) { float px, pz, py; off = std::max(off, M->roadNearest(L[j].x, L[j].z, px, pz, py)); }
        CHECK(std::hypot(L.front().x - L.back().x, L.front().z - L.back().z) < 10.f, "loop doesn't close");
        CHECK(len > 0.5f * want * 1000 && len < 1.6f * want * 1000, "loop %.1f km for %.0f wanted (%s)", len / 1000, want, name.c_str());
        CHECK(off < 6.f, "loop strays %.1f m off the roads", off);
    }

    // cache round trip
    std::string path = "/tmp/world_test_" + std::to_string(seed) + ".bin";
    CHECK(M->save(path), "save");
    Map L2;
    CHECK(L2.load(path), "load");
    CHECK(L2.h == M->h && L2.roads.size() == M->roads.size() && L2.rivers.size() == M->rivers.size() && L2.towns.size() == M->towns.size(), "round trip");
    Sample s1 = M->sample(123.f, -456.f), s2 = L2.sample(123.f, -456.f);
    CHECK(s1.h == s2.h && s1.road == s2.road, "loaded world samples the same");
}

int main() {
    suite(11, 8192.f);
    suite(23, 8192.f);
    suite(4, 20480.f);
    suite(5, 10240.f, 2);   // a small desert map
    std::printf("%d checks, %d failed\n", checks, fails);
    return fails ? 1 : 0;
}
