#pragma once
// Weather & sky (ports of CoreKick's world/weather.ts, world/sky.ts and core weather.rs):
// fifteen named conditions blended into a continuous state, a deterministic
// "changing weather" timeline per ride, live weather from Open-Meteo, and
// low-precision sun/moon astronomy with the moon's real phase.
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace weather {

enum class Kind { Clear, Fair, Windy, Cloudy, Overcast, Mist, Fog, Drizzle, Sunshower, Rain, Storm, Hail, Sleet, Snow, Blizzard, COUNT };
const char* kindId(Kind k);
const char* kindLabel(Kind k);
std::optional<Kind> kindFromId(const std::string& s);

struct State {
    double cloud = 0, dark = 0, rain = 0, snow = 0, fog = 0, wind = 0, storm = 0, hail = 0, gust = 0;
};
State stateOf(Kind k);
void blend(State& cur, const State& target, double dt, double seconds = 45);
State adjustForAltitude(State s, double snowiness);

struct Segment { double start; Kind kind; };
std::vector<Segment> timeline(uint32_t seed, double hours = 6, bool alpine = false);
Kind kindAt(const std::vector<Segment>& segs, double t);

struct Live {
    Kind kind = Kind::Fair;
    double temperatureC = 0, windKmh = 0, cloudCover = 0, precipitationMm = 0;
    bool isDay = true;
    std::string place;
    int code = 0;
    struct Hour { double inHours = 1; Kind kind = Kind::Fair; double tempC = 0, windKmh = 0, precipProb = 0; };
    std::vector<Hour> hourly;   // the next three hours
};
// air temperature for a spell (seasonal mean, day/night swing damped by cloud, rain, height)
double temperature(double seasonalMeanC, double sunElevationDeg, Kind k, double elevationM);
// mean temperature for the time of year at a latitude (a rough climatology: London ~5 C in late Nov)
double seasonalMean(double latDeg, int dayOfYear);
const char* kindFromWmo(int code, double windKmh, double cloudCover);
State stateFromLive(const Live& l);
std::string forecastUrl(double lat, double lon);
std::string geocodeUrl(const std::string& query);
std::optional<Live> parseCurrent(const std::string& json, const std::string& place);
struct Place { std::string name; double lat = 0, lon = 0; };
std::vector<Place> parsePlaces(const std::string& json);

enum class Mode { Auto, Live, Fixed };

double gustPulse(double t);

class Sim {
public:
    Mode mode = Mode::Auto;
    Kind fixed = Kind::Clear;
    std::optional<Live> live;
    State state;
    double wet = 0, snowCover = 0, flash = 0, rainbow = 0, windDir = 0, gust = 0;

    explicit Sim(uint32_t seed = 1, bool alpine = false);
    Kind currentKind(double elapsed) const;
    Kind kindIn(double elapsed, double aheadS) const;   // forecast: the spell aheadS from now
    State targetFor(double elapsed) const;
    struct Events { bool lightning = false; double thunderIn = 0, strength = 0; std::optional<Kind> changed; };
    Events update(double dt, double elapsed, double snowiness, bool sunUp, bool instant = false);

private:
    std::vector<Segment> segs_;
    uint64_t r_;
    double rnd();
    double nextFlash_ = 8, flash2_ = -1, wasRaining_ = 999;
    std::optional<Kind> kind_;
};

}  // namespace weather

namespace sky {
struct Body { double elevation = 0, azimuth = 0; };  // degrees above horizon; radians clockwise from north
struct MoonInfo { Body pos; double illumination = 0, age = 0; bool waxing = true; };
Body sunPosition(std::time_t t, double lat, double lon);
MoonInfo moonPosition(std::time_t t, double lat, double lon);
MoonInfo moonPhase(std::time_t t);
double todFromElevation(double elev);  // palette 0 afternoon .. 1 night
Body stylisedSun(double tod, double baseAzimuth, double paletteElevation);
Body stylisedMoon(double tod, double baseAzimuth);
void skyVector(const Body& b, double out[3]);  // x east, y up, z north
}  // namespace sky
