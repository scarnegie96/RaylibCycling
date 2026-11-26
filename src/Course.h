#pragma once
// The 3D road for any Route: a 2D layout (loops become a closed, star-shaped curve;
// point-to-point routes a meandering road that never crosses itself), the route's
// elevation on top, and the terrain height field the road is carved into.
// Works for roads of any length via spatial hashing. Immutable once built: the renderer's
// worker threads read it while the app builds a re-laid copy after an in-ride edit.
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct Route;
namespace terrain { class Field; }
namespace wmap { class Map; struct RoadPt; }

struct V3 { float x = 0, y = 0, z = 0; };

struct CoursePoint {
    V3 pos;            // centreline at road surface
    float heading = 0; // yaw (radians), 0 = +Z
    float dist = 0;    // metres from start
    float grade = 0;   // fraction (0.05 = 5 %)
    uint8_t kind = 0;  // generated worlds: 0 on the land, 1 in a tunnel, 2 on a bridge
};

class Course {
public:
    void build(const Route& route, uint32_t seed);
    // A road of a generated world (wmap::Map): the course follows the given path on its
    // network (a loop) and the land, water and roads all come from the map.
    void buildWorld(const Route& route, std::shared_ptr<const wmap::Map> world, const std::vector<wmap::RoadPt>& path, bool loop = true);
    const wmap::Map* world() const { return world_.get(); }
    // After the route was edited at `fromDist` (sectors spliced in): keep the road behind
    // `fromDist`, re-lay everything after it to the new length / elevations.
    void relay(const Route& route, double fromDist);
    // endless roads: *this is a copy of prev; lay the newly appended road on the end, keep every
    // existing point, and take over prev's terrain field (only the land near the new road is
    // regenerated). Returns the box of land that changed.
    struct Box { float x0, z0, x1, z1; };
    Box extendFrom(const Course& prev, const Route& route);
    // free roaming on a generated map: *this is a copy of prev; the path grew (prev's path is
    // a prefix of it); the terrain field is taken over as it is (the land doesn't change)
    Box extendWorld(const Course& prev, const Route& route, const std::vector<wmap::RoadPt>& path);

    float length() const { return length_; }
    bool loop() const { return loop_; }
    const std::string& name() const { return name_; }
    const std::vector<CoursePoint>& points() const { return pts_; }
    int version() const { return version_; }
    uint32_t seed() const { return seed_; }

    CoursePoint at(double dist) const;  // wraps on loops; extends straight past the ends otherwise
    float gradeAt(double dist) const { return at(dist).grade; }
    int kindAt(double dist) const;     // 0 land, 1 tunnel, 2 bridge (generated worlds)
    float elevationAt(double dist) const { return at(dist).pos.y; }
    float minElevation() const { return minEle_; }
    float maxElevation() const { return maxEle_; }
    float totalAscent() const { return ascent_; }
    double eleOffset() const { return offset_; }  // world y = route elevation - offset

    // final ground height (landform + rivers + erosion; see Terrain.h)
    float terrainHeight(float x, float z, float* roadDist = nullptr) const;
    float roadDistance(float x, float z, float* roadY = nullptr, int* index = nullptr) const;
    // nearest point on the centreline (within ~120 m; returns 1e4 otherwise)
    float roadNearest(float x, float z, float& px, float& pz, float& py) const;
    // The land grows out of the road. Near the road: the road's own height, tilted across it
    // by its gradient (climbs traverse hillsides). Away from it the land relaxes to the
    // course's valley floor (baseHeight) at a natural hillside slope, so a road that climbs
    // ends up on a mountain that rises under it and one that descends sinks into a valley.
    // Strictly local: only road within REACH matters, so extending an endless road never
    // changes land beyond REACH of the new road. coarseDist = distance to the road (from the
    // 80 m road samples); rugged = 0..1 from nearby gradients.
    static constexpr float REACH = 2500.f;
    float regional(float x, float z, float* coarseDist, float* rugged) const;
    float baseHeight(float x, float z) const;   // the valley floor far from any road
    const terrain::Field& field() const;                // lazily built, thread-safe
    float waterLevel() const { return water_; }
    float roadHalfWidth() const { return 3.6f; }
    // distance-from-start of the road point nearest (x,z) within ~120 m, or -1
    float nearestDist(float x, float z) const;

private:
    void layout(const Route& route, size_t keep);
    void finishBuild(const Route& route);
    void index();
    float loopRadius(float theta) const;

    std::vector<CoursePoint> pts_;
    std::vector<float> theta_;                        // loops: polar angle of each point
    std::vector<std::pair<float, float>> bumps_;      // loops: (theta start, scale) from re-lays
    float loopR_ = 1000, la_[4] = {}, lp_[4] = {};
    std::unordered_map<int64_t, std::vector<int>> grid_;    // 64 m cells -> point indices
    std::unordered_map<int64_t, std::vector<int>> coarse_;  // 512 m cells -> every 40th point
    std::unordered_map<int64_t, std::vector<int>> far_;     // 2048 m cells -> every 200th point
    float length_ = 0, minEle_ = 0, maxEle_ = 0, ascent_ = 0, water_ = 0, meanY_ = 0, step_ = 2;
    float baseY_ = 0;   // valley floor: fixed for a course's life (endless roads keep their first one)
    double offset_ = 0;
    bool loop_ = true;
    int version_ = 0;
    uint32_t seed_ = 1;
    std::string name_;
    std::shared_ptr<const wmap::Map> world_;
    // the terrain generator + its caches; never shared between copies (a copy is re-laid)
    struct FieldHolder {
        mutable std::atomic<terrain::Field*> ptr{nullptr};
        mutable std::unique_ptr<terrain::Field> own;
        mutable std::mutex m;
        FieldHolder();
        FieldHolder(const FieldHolder&);
        FieldHolder& operator=(const FieldHolder&) { reset(); return *this; }
        ~FieldHolder();
        void reset();
    } field_;
};

// value-noise helpers shared with the renderer
float hash2(int x, int y, uint32_t seed);
float vnoise(float x, float y, uint32_t seed);
float fbm(float x, float y, int oct, uint32_t seed);
