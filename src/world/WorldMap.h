#pragma once
// A whole world, generated up front: 20 x 20 km (by default) of land with climate, biomes,
// erosion, rivers and lakes, towns, and a road network laid over it by an engineer's rules.
//
//   1. Relief: mountain ranges along wandering spines, uplands and basins.
//   2. Climate: temperature (lapse rate, regional gradient) and precipitation carried by a
//      prevailing wind - heavy on the windward slopes, a rain shadow (and desert) behind.
//   3. Biomes as blended weights from temperature x precipitation (Whittaker-style), so every
//      border is a wide, ragged transition. Each biome brings its own landform: rolling
//      farmland, knobby boreal country with kettle lakes, alpine ridges, long steppe swells,
//      savanna with inselbergs, rocky scrub hills, wind-aligned dunes, stepped mesas.
//   4. Droplet erosion scaled by rainfall (deserts barely erode), thermal slumping to each
//      material's angle of repose.
//   5. Hydrology on the whole map: priority-flood, D8 flow, runoff from a Budyko water
//      balance, transmission losses in dry country (rivers shrink crossing a desert), lakes
//      filled only as far as inflow beats evaporation (endorheic lakes, salt flats), dry
//      washes, wetlands where flat, wet ground collects water. Rivers carve their valleys.
//   6. Towns at good sites (flat, by water, not in dunes or marsh), joined by a neighbour
//      graph with loops; each link routed by A* over grade (capped), turning, bridging
//      (short, square crossings only), biome cost and reuse of existing road, so junctions
//      and a road hierarchy emerge. Roads are smoothed, given a grade-limited profile, and
//      the land is cut and filled to them; rivers pass under bridges.
//
// Pure C++ (no raylib), deterministic for a seed.
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace wmap {

enum Biome : int { Meadow, Forest, Boreal, Alpine, Steppe, Savanna, Scrub, Dunes, Badlands, Wetland, BIOMES };
const char* biomeName(int b);

struct Params {
    uint32_t seed = 1;
    float size = 20480.f;      // metres, square, centred on the origin
    float cell = 20.f;         // grid spacing (m)
    float erosion = 1.f;       // droplet budget scale (0 = none)
    bool roads = true;
    float aridity = 0.f;       // -1 wetter .. +1 drier than the seed's own climate
    int climate = 0;           // 0 the seed decides, 1 temperate, 2 desert, 3 cold
};

struct RiverPt { float x, z, level, half, depth, q; };
struct River {
    std::vector<RiverPt> pts;
    int into = -1;             // river it joins (or -1: a lake or the map edge)
    bool dry = false;          // a wash: a channel that only runs after storms
};
struct Lake {
    float level = 0, area = 0, depth = 0;
    float cx = 0, cz = 0;
    bool full = false;         // spills (has an outflow)
    bool playa = false;        // dry salt flat
    bool pond = false;         // marsh pool
};
struct Town { float x = 0, z = 0, y = 0; int rank = 1; std::string name; };
struct RoadPt { float x, z, y; uint8_t kind = 0; };   // kind: 0 on the land, 1 tunnel, 2 bridge
struct Bridge {
    float x, z, roadY, level, span, width, hx, hz;
    int road = -1, i0 = 0, i1 = 0;   // the stretch of road it carries (points i0..i1)
};
struct Tunnel { int road = -1, i0 = 0, i1 = 0; };
struct Road {
    std::vector<RoadPt> pts;
    int cls = 1;               // 0 main, 1 minor
    int a = -1, b = -1;        // graph nodes at the ends
    float length = 0;
    std::vector<std::pair<float, float>> cross;   // generation only: river crossings (distance along, half width)
};
struct Node { float x = 0, z = 0, y = 0; std::vector<int> roads; int town = -1; };

struct Sample {
    float h = 0;
    float water = -1e9f;       // water surface (dry where below h)
    float flowX = 0, flowZ = 0;
    float waterW = 0;          // river width (lakes 1e3)
    float bank = 0, bankLevel = -1e9f;
    float wet = 0;             // damp ground: streams, marsh, washes (0..1)
    float sediment = 0;        // fresh deposits, wash beds, playas (0..1)
    float road = 1e4f, roadY = 0;  // nearest road of the network (not counting tunnels)
    float tunnel = 1e4f;           // nearest tunnel centreline
    int roadCls = -1;
    float moist = 0.5f;        // soil moisture (0 dry .. 1 wet)
    float sand = 0;            // loose sand at the surface (dunes, washes, playas)
    float temp = 10;           // mean annual temperature (C)
    float w[BIOMES] = {};      // biome weights (sum 1)
};

struct GenStats { double ms[10] = {}; int droplets = 0, rivers = 0, lakes = 0, playas = 0, towns = 0, roads = 0, bridges = 0, tunnels = 0; float roadKm = 0, tunnelKm = 0; };

class Map {
public:
    Params params;
    int n = 0;                 // grid points per side
    float cell = 20, x0 = 0, z0 = 0;
    // grids (n x n, row-major z then x)
    std::vector<float> h;      // final carved height
    std::vector<float> temp, precip;   // C, mm/yr
    std::vector<float> moist;  // soil moisture 0..1
    std::vector<float> q;      // discharge (m3/s)
    std::vector<float> sand;   // 0..1
    std::vector<int16_t> lakeId;
    std::vector<uint8_t> wash; // dry channel beds
    // biome weights on a coarse grid (cn x cn, spacing ccell)
    int cn = 0;
    float ccell = 80;
    std::vector<std::array<float, BIOMES>> biome;
    float windX = 1, windZ = 0;
    float snowLine = 2500;     // mean height of the 0 C isotherm

    std::vector<River> rivers;
    std::vector<Lake> lakes;
    std::vector<Town> towns;
    std::vector<Road> roads;
    std::vector<Node> nodes;
    std::vector<Bridge> bridges;
    std::vector<Tunnel> tunnels;
    GenStats stats;

    // ---- queries (thread-safe once built)
    float grid(int i, int k) const { return h[size_t(k) * n + i]; }
    float height(float x, float z) const;              // smooth (bicubic) grid height
    Sample sample(float x, float z) const;             // everything, with fine detail
    void biomeAt(float x, float z, float w[BIOMES]) const;
    float roadNearest(float x, float z, float& px, float& pz, float& py, int* cls = nullptr) const;
    std::vector<Bridge> bridgesNear(float x, float z, float r) const;
    bool inside(float x, float z, float margin = 0) const;
    // A loop on the network about targetKm long, starting near (sx, sz). Points every ~4 m.
    std::vector<RoadPt> loop(float targetKm, uint32_t seed, float sx, float sz, std::string* name = nullptr) const;

    bool save(const std::string& path) const;
    bool load(const std::string& path);
    void index();              // spatial hashes (after generate / load)

    // river pieces and road pieces bucketed by 64 m cell
    struct Ref { int a, b; };
    std::unordered_map<int64_t, std::vector<Ref>> riverCells, roadCells;
    std::unordered_map<int64_t, std::vector<int>> bridgeCells;
};

using Progress = std::function<void(const char* stage, float frac)>;
std::shared_ptr<Map> generate(const Params& p, const Progress& progress = {});

// version of the generator (cache files carry it)
constexpr int VERSION = 8;

}  // namespace wmap
