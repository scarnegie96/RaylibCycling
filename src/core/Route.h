#pragma once
// Routes (port of CoreKick route.rs): elevation profiles, statistics, climb
// detection, a seeded generator, a segment builder, GPX import and in-ride
// sectors spliced into the road ahead.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "Json.h"
#include "Physics.h"

struct RoutePoint { double d = 0, ele = 0; };

enum class RouteSource { Generated, Custom, Gpx, Builtin };
enum class Terrain { Flat, Rolling, Hilly, Mountainous, Alpine };
enum class Finish { Free, Loop, Summit };

const char* terrainName(Terrain t);   // "Flat", "Rolling", ...
const char* terrainId(Terrain t);     // "flat", "rolling", ... (serde)
std::optional<Terrain> terrainFromId(const std::string& s);

struct GenParams {
    double distanceKm = 30, targetGainM = 300, maxGradePct = 8;
    Terrain terrain = Terrain::Rolling;
    Finish finish = Finish::Loop;
    uint64_t seed = 1;
    double startEleM = 120, smoothness = 0.5;
};

struct SegmentSpec { double lengthM = 0, gradePct = 0; };

struct Climb {
    double startD = 0, endD = 0, lengthM = 0, gainM = 0, avgGradePct = 0, maxGradePct = 0, startEle = 0, topEle = 0, score = 0;
    std::string category;
};
const char* climbCategory(double score);
std::vector<Climb> detectClimbs(const std::vector<std::pair<double, double>>& grid);

struct RouteStats {
    double distanceM = 0, gainM = 0, lossM = 0, minEle = 0, maxEle = 0, maxGradePct = 0, minGradePct = 0;
    double climbDensity = 0, climbingShare = 0, difficulty = 0;
    std::string difficultyLabel;
    std::vector<Climb> climbs;
};

struct Route {
    std::string id, name, description, createdAt;
    RouteSource source = RouteSource::Custom;
    std::vector<RoutePoint> points;
    bool isLoop = false;
    std::vector<std::string> tags;
    std::optional<GenParams> genParams;
    std::optional<std::vector<SegmentSpec>> segments;
    bool favourite = false;

    double distance() const { return points.empty() ? 0 : points.back().d; }
    double elevationAt(double d) const;
    double gradeAt(double d, double halfWindow) const;
    std::vector<std::pair<double, double>> uniform(double step) const;
    RouteStats stats() const;
    double estimateTime(double watts, const phys::Params& p) const;

    json::Value toJson() const;
    static std::optional<Route> fromJson(const json::Value& v);
};

double difficultyScore(double gain, double density, double maxGrade);
const char* difficultyLabel(double score);
double suggestedGain(Terrain t, double distanceKm);

Route generateRoute(const GenParams& p, const std::string& name);
Route buildFromSegments(const std::string& name, const std::vector<SegmentSpec>& segs, double startEle, bool smooth);
bool routeFromGpx(const std::string& text, const std::string& nameOverride, Route& out, std::string& err);
Route endlessRoute(Terrain t, uint64_t seed);

// ---------------------------------------------------------------- endless roads, 1 km at a time
// Hilliness of a kilometre: how much it undulates around its trend.
enum class Hilliness { Flat, Rolling, Hilly, Mountain, Alpine };
const char* hillinessName(Hilliness h);
struct EndlessGen {
    uint64_t seed = 1;
    int nextKm = 0;          // index of the next kilometre to generate
    double trendGrade = 0;   // grade the last kilometre ended on (its trend, without undulation)
};
// The next kilometre: rises (or falls) by netM over its 1000 m, undulating as hilliness asks,
// joining the previous kilometre without a kink. Returns 100 x 10 m segments.
std::vector<SegmentSpec> endlessKm(EndlessGen& g, double netM, Hilliness h);
// a fresh endless road: `km` kilometres from startEle
Route endlessStart(EndlessGen& g, double startEle, int km, double netPerKm, Hilliness h);
std::vector<Route> seedRoutes();

// ---------------------------------------------------------------- in-ride sectors
enum class SectorKind { Climb, Descent, Flat, Rollers, Sprint, Custom, Terrain };
struct SectorSpec {
    SectorKind kind = SectorKind::Climb;
    std::string name;
    double lengthM = 1000, gradePct = 6;
    std::vector<SegmentSpec> segments;   // custom
    std::optional<Terrain> terrain;      // terrain sectors
    double inM = 300;                    // how far ahead it starts
    bool atEnd = false;                  // append at the end instead
    bool rejoin = true;                  // return to the original elevation afterwards
    bool timed = true;
    std::string displayName() const;
    std::vector<SegmentSpec> build(uint64_t seed) const;
};
std::pair<double, double> segmentsSummary(const std::vector<SegmentSpec>& segs);  // length, net elevation
std::optional<SegmentSpec> rejoinSegment(double netM);
std::pair<double, double> insertSegments(Route& r, double at, const std::vector<SegmentSpec>& segs);  // inserted length, net ele
