#pragma once
// Terrain generation for any road, forever, in any order.
//
// Three layers, each a pure function of world position (plus the course), so a tile built
// now always agrees with one built an hour ago or on another thread:
//
//  1. Landform: the route's own elevations spread over the land (Course::regional), tilted
//     across the road by its gradient so climbs traverse hillsides, plus warped ridged relief
//     that grows into mountains away from the road and with the route's steepness.
//  2. Hydrology: a jittered drainage lattice (180 m). Every node drains to its steepest
//     neighbour; catchment is counted upstream (bounded, so it's still local). Edges become
//     meandering river polylines that carve channels and valleys sized by catchment; pits
//     become lakes. Rivers stay clear of the road and cross it at right angles (bridges).
//  3. Particle erosion (after SimpleHydrology, weigert): 512 m tiles with an apron are
//     eroded by droplets whose spawn points are seeded from world cells, then blended with
//     a partition of unity across the apron, so neighbouring tiles agree without ever
//     needing to be simulated together. Adds gullies, fans, scree and stream discharge.
#include <cstdint>
#include <memory>
#include <vector>

class Course;

namespace terrain {

constexpr float TILE = 512.f;  // erosion tiles line up with the renderer's chunks
constexpr float CELL = 4.f;    // erosion resolution (m)

struct Sample {
    float h = 0;                 // ground height
    float road = 1e4f;           // distance from the road centreline
    float water = -1e9f;         // water surface (rivers, lakes); dry where below h
    float flowX = 0, flowZ = 0;  // surface current (m/s)
    float waterW = 0;            // width of the river here (lakes: 1e3)
    float wet = 0;               // streams and damp ground from erosion discharge (0..1)
    float sediment = 0;          // fresh deposits: gravel bars, fans (0..1)
    float scour = 0;             // eroded, bare ground (0..1)
    float bank = 0;              // river / lake shore (0..1)
    float bankLevel = -1e9f;     // water surface of the nearby river (banks stay above it)
};

// a river passing under the road
struct Bridge {
    float x = 0, z = 0;          // on the road centreline
    float roadY = 0, level = 0;  // deck height, water surface
    float span = 0;              // total length along the road (m)
    float width = 0;             // river width
};

struct Stats { int windows = 0, nodes = 0, cells = 0; double erodeMs = 0; };

class Field {
public:
    explicit Field(const Course& c);
    ~Field();
    Field(const Field&) = delete;
    Field& operator=(const Field&) = delete;

    float height(float x, float z, float* roadDist = nullptr) const;
    Sample sample(float x, float z) const;    // eroded where the erosion tiles exist
    Sample macro(float x, float z) const;     // landform + rivers + lakes only

    // run (or wait for another thread's) erosion for everything tile (tx,tz) depends on
    void prepareTile(int tx, int tz) const;
    bool tileReady(int tx, int tz) const;
    std::vector<Bridge> bridgesNear(float x, float z, float radius) const;
    void trim(float x, float z, float keepRadius) const;  // forget caches far away
    void setErosion(bool on, int quality) const;           // quality 0..2: droplet budget
    Stats stats() const;

    struct Impl;
private:
    std::unique_ptr<Impl> d_;
};

// development: shaded relief map (PPM) - water blue, road dark
void writeRelief(const Field& F, float x0, float z0, float size, float res, const char* path);

}  // namespace terrain
