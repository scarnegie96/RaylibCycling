#include "Weather.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "Json.h"

namespace weather {

static const char* IDS[] = {"clear", "fair", "windy", "cloudy", "overcast", "mist", "fog", "drizzle", "sunshower", "rain", "storm", "hail", "sleet", "snow", "blizzard"};
static const char* LABELS[] = {"Clear", "Fair", "Windy", "Cloudy", "Overcast", "Mist", "Fog", "Drizzle", "Sunshower", "Rain", "Thunderstorm", "Hail", "Sleet", "Snow", "Blizzard"};

const char* kindId(Kind k) { return IDS[int(k)]; }
const char* kindLabel(Kind k) { return LABELS[int(k)]; }
std::optional<Kind> kindFromId(const std::string& s) {
    for (int i = 0; i < int(Kind::COUNT); ++i) if (s == IDS[i]) return Kind(i);
    return std::nullopt;
}

static State st(double cloud, double dark, double rain, double snow, double fog, double wind, double storm = 0, double hail = 0, double gust = -1) {
    return State{cloud, dark, rain, snow, fog, wind, storm, hail, gust < 0 ? wind * 0.4 : gust};
}

State stateOf(Kind k) {
    switch (k) {
        case Kind::Clear: return st(0.05, 0, 0, 0, 0, 0.15);
        case Kind::Fair: return st(0.35, 0.05, 0, 0, 0, 0.3);
        case Kind::Windy: return st(0.42, 0.1, 0, 0, 0, 1, 0, 0, 1);
        case Kind::Cloudy: return st(0.65, 0.25, 0, 0, 0.08, 0.4);
        case Kind::Overcast: return st(0.95, 0.45, 0, 0, 0.2, 0.35);
        case Kind::Mist: return st(0.7, 0.2, 0, 0, 0.85, 0.05, 0, 0, 0);
        case Kind::Fog: return st(0.85, 0.2, 0, 0, 1, 0.02, 0, 0, 0);
        case Kind::Drizzle: return st(0.85, 0.45, 0.3, 0, 0.3, 0.3);
        case Kind::Sunshower: return st(0.32, 0.12, 0.45, 0, 0.05, 0.25);
        case Kind::Rain: return st(1, 0.65, 0.75, 0, 0.4, 0.5);
        case Kind::Storm: return st(1, 0.9, 1, 0, 0.45, 0.85, 1, 0, 0.9);
        case Kind::Hail: return st(1, 0.85, 0.45, 0, 0.3, 0.7, 0.45, 1, 0.8);
        case Kind::Sleet: return st(1, 0.6, 0.5, 0.35, 0.35, 0.45);
        case Kind::Snow: return st(0.95, 0.35, 0, 0.6, 0.4, 0.25);
        case Kind::Blizzard: return st(1, 0.55, 0, 1, 0.75, 0.9, 0, 0, 0.9);
        default: return State{};
    }
}

void blend(State& c, const State& t, double dt, double seconds) {
    double k = 1 - std::exp(-dt / seconds);
    c.cloud += (t.cloud - c.cloud) * k; c.dark += (t.dark - c.dark) * k; c.rain += (t.rain - c.rain) * k;
    c.snow += (t.snow - c.snow) * k; c.fog += (t.fog - c.fog) * k; c.wind += (t.wind - c.wind) * k;
    c.storm += (t.storm - c.storm) * k; c.hail += (t.hail - c.hail) * k; c.gust += (t.gust - c.gust) * k;
}

State adjustForAltitude(State s, double snowiness) {
    if (snowiness <= 0) return s;
    double moved = s.rain * snowiness;
    s.rain -= moved;
    s.snow = std::min(1.0, s.snow + moved * 0.8);
    s.storm *= 1 - snowiness;
    return s;
}

// mulberry32-style generator (stands in for CoreKick's noise.ts rng)
static double mulberry(uint32_t& a) {
    a += 0x6D2B79F5u;
    uint32_t t = a;
    t = (t ^ (t >> 15)) * (t | 1u);
    t ^= t + (t ^ (t >> 7)) * (t | 61u);
    return double((t ^ (t >> 14))) / 4294967296.0;
}

std::vector<Segment> timeline(uint32_t seed, double hours, bool alpine) {
    struct Opt { Kind k; double w; };
    auto next = [](Kind k) -> std::vector<Opt> {
        switch (k) {
            case Kind::Clear: return {{Kind::Clear, 3}, {Kind::Fair, 5}, {Kind::Cloudy, 1}, {Kind::Windy, 0.6}, {Kind::Mist, 0.5}};
            case Kind::Fair: return {{Kind::Clear, 3}, {Kind::Fair, 2}, {Kind::Cloudy, 4}, {Kind::Windy, 1}, {Kind::Sunshower, 0.7}, {Kind::Drizzle, 0.5}};
            case Kind::Windy: return {{Kind::Fair, 2}, {Kind::Cloudy, 2}, {Kind::Clear, 1}};
            case Kind::Cloudy: return {{Kind::Fair, 3}, {Kind::Overcast, 3}, {Kind::Drizzle, 2}, {Kind::Rain, 1}, {Kind::Windy, 0.8}, {Kind::Mist, 0.6}};
            case Kind::Overcast: return {{Kind::Cloudy, 3}, {Kind::Drizzle, 3}, {Kind::Rain, 2.5}, {Kind::Storm, 0.8}, {Kind::Mist, 0.8}, {Kind::Fog, 0.4}, {Kind::Sleet, 0.3}};
            case Kind::Mist: return {{Kind::Fair, 2}, {Kind::Cloudy, 3}, {Kind::Overcast, 1}, {Kind::Fog, 1}};
            case Kind::Fog: return {{Kind::Mist, 3}, {Kind::Cloudy, 2}, {Kind::Fair, 1}};
            case Kind::Drizzle: return {{Kind::Cloudy, 3}, {Kind::Rain, 2}, {Kind::Fair, 1.5}, {Kind::Sunshower, 0.8}};
            case Kind::Sunshower: return {{Kind::Fair, 3}, {Kind::Clear, 1}, {Kind::Drizzle, 1}};
            case Kind::Rain: return {{Kind::Drizzle, 2}, {Kind::Cloudy, 2.5}, {Kind::Fair, 2}, {Kind::Storm, 0.8}, {Kind::Sleet, 0.3}};
            case Kind::Storm: return {{Kind::Rain, 3}, {Kind::Cloudy, 2}, {Kind::Hail, 0.6}};
            case Kind::Hail: return {{Kind::Storm, 1}, {Kind::Rain, 2}, {Kind::Cloudy, 1}};
            case Kind::Sleet: return {{Kind::Rain, 2}, {Kind::Drizzle, 2}, {Kind::Snow, 1}, {Kind::Overcast, 1}};
            case Kind::Snow: return {{Kind::Overcast, 2}, {Kind::Snow, 2}, {Kind::Blizzard, 0.6}, {Kind::Sleet, 0.5}};
            default: return {{Kind::Snow, 3}};
        }
    };
    uint32_t a = seed * 7717u + 3u;
    std::vector<Segment> segs;
    Kind kind = mulberry(a) < 0.55 ? Kind::Clear : mulberry(a) < 0.6 ? Kind::Fair : Kind::Cloudy;
    double t = 0;
    while (t < hours * 3600) {
        segs.push_back({t, kind});
        t += 480 + mulberry(a) * 840;
        auto opts = next(kind);
        double total = 0;
        for (auto& o : opts) { if (alpine && (o.k == Kind::Rain || o.k == Kind::Drizzle)) o.w *= 0.6; total += o.w; }
        double pick = mulberry(a) * total;
        for (auto& o : opts) { pick -= o.w; if (pick <= 0) { kind = o.k; break; } }
    }
    return segs;
}

Kind kindAt(const std::vector<Segment>& segs, double t) {
    Kind k = segs.empty() ? Kind::Clear : segs[0].kind;
    for (auto& s : segs) { if (s.start <= t) k = s.kind; else break; }
    return k;
}

const char* kindFromWmo(int code, double wind, double cloud) {
    if (code >= 0 && code <= 2 && wind >= 38) return "windy";
    switch (code) {
        case 0: return "clear";
        case 1: return "fair";
        case 2: return "cloudy";
        case 3: return "overcast";
        case 45: return "mist";
        case 48: return "fog";
        case 56: case 57: case 66: case 67: return "sleet";
        case 80: if (cloud < 55) return "sunshower"; return "drizzle";
        case 51: case 53: case 55: case 61: return "drizzle";
        case 63: case 65: case 81: case 82: return "rain";
        case 71: case 73: case 77: case 85: return wind > 40 ? "blizzard" : "snow";
        case 75: case 86: return "blizzard";
        case 95: return "storm";
        case 96: case 99: return "hail";
        default: return "cloudy";
    }
}

State stateFromLive(const Live& l) {
    State b = stateOf(l.kind);
    b.cloud = std::max(b.cloud * 0.5, std::min(1.0, l.cloudCover / 100));
    b.wind = std::min(1.0, l.windKmh / 45);
    b.gust = std::max(b.gust, std::min(1.0, (l.windKmh - 15) / 35));
    return b;
}

std::string forecastUrl(double lat, double lon) {
    char b[300];
    std::snprintf(b, sizeof b,
                  "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code,cloud_cover,"
                  "precipitation,wind_speed_10m,is_day&wind_speed_unit=kmh",
                  lat, lon);
    return b;
}

std::string geocodeUrl(const std::string& query) {
    std::string q;
    std::string t = query;
    while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
    while (!t.empty() && std::isspace((unsigned char)t.front())) t.erase(t.begin());
    for (unsigned char c : t) {
        if (c == ' ') q += '+';
        else if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == ',') q += char(c);
        else { char b[4]; std::snprintf(b, sizeof b, "%%%02X", c); q += b; }
    }
    return "https://geocoding-api.open-meteo.com/v1/search?name=" + q + "&count=6&language=en&format=json";
}

std::optional<Live> parseCurrent(const std::string& text, const std::string& place) {
    json::Value v;
    if (!json::parse(text, v)) return std::nullopt;
    auto c = v.get("current");
    if (!c) return std::nullopt;
    Live l;
    l.code = int(c->numOr("weather_code", 3));
    l.temperatureC = c->numOr("temperature_2m", 0);
    l.cloudCover = c->numOr("cloud_cover", 0);
    l.precipitationMm = c->numOr("precipitation", 0);
    l.windKmh = c->numOr("wind_speed_10m", 0);
    l.isDay = c->numOr("is_day", 1) == 1;
    l.kind = kindFromId(kindFromWmo(l.code, l.windKmh, l.cloudCover)).value_or(Kind::Cloudy);
    l.place = place;
    return l;
}

std::vector<Place> parsePlaces(const std::string& text) {
    std::vector<Place> out;
    json::Value v;
    if (!json::parse(text, v)) return out;
    for (auto& g : v.arr("results")) {
        Place p;
        p.name = g.strOr("name");
        for (auto* k : {"admin1", "country"}) {
            auto extra = g.str(k);
            if (extra && p.name.find(*extra) == std::string::npos) p.name += ", " + *extra;
        }
        p.lat = g.numOr("latitude", 0);
        p.lon = g.numOr("longitude", 0);
        out.push_back(p);
    }
    return out;
}

double gustPulse(double t) {
    double g = 0.5 + 0.5 * std::sin(t * 0.83) * std::sin(t * 0.31 + 1.3) + 0.25 * std::sin(t * 2.1 + std::sin(t * 0.5));
    double x = std::clamp((g - 0.55) / 0.4, 0.0, 1.0);
    return x * x * (3 - 2 * x);
}

Sim::Sim(uint32_t seed, bool alpine) {
    segs_ = timeline(seed, 8, alpine);
    r_ = uint64_t(seed) * 31 + 7;
    windDir = rnd() * M_PI * 2;
    state = targetFor(0);
    wet = state.rain > 0.2 ? 0.8 : 0;
    snowCover = state.snow > 0.2 ? 0.9 : 0;
}

double Sim::rnd() { uint32_t a = uint32_t(r_); double v = mulberry(a); r_ = a; return v; }

Kind Sim::currentKind(double elapsed) const {
    if (mode == Mode::Live) return live ? live->kind : Kind::Fair;
    if (mode == Mode::Auto) return kindAt(segs_, elapsed);
    return fixed;
}

State Sim::targetFor(double elapsed) const {
    if (mode == Mode::Live && live) return stateFromLive(*live);
    return stateOf(currentKind(elapsed));
}

Sim::Events Sim::update(double dt, double elapsed, double snowiness, bool sunUp, bool instant) {
    Events ev;
    Kind k = currentKind(elapsed);
    if (!kind_ || *kind_ != k) {
        if (kind_) ev.changed = k;
        kind_ = k;
    }
    State target = adjustForAltitude(targetFor(elapsed), snowiness);
    blend(state, target, instant ? 1e6 : dt, 40);
    const State& s = state;
    wet = std::clamp(s.rain > 0.05 ? wet + dt * s.rain / 35 : wet - dt / 160, 0.0, 1.0);
    double settle = s.snow * (1 - s.rain * 0.8) + s.hail * 0.5;
    snowCover = std::clamp(settle > 0.06 ? snowCover + dt * settle / 70 : snowCover - dt / (s.rain > 0.2 ? 240 : 900), 0.0, 1.0);
    if (s.rain > 0.3) wasRaining_ = 0; else wasRaining_ += dt;
    bool afterShower = wasRaining_ > 4 && wasRaining_ < 120 && s.rain < 0.12 && s.cloud < 0.85;
    bool sunshower = s.rain > 0.15 && s.cloud < 0.5;
    bool bow = (afterShower || sunshower) && sunUp;
    rainbow = std::clamp(rainbow + (bow ? dt / 12 : -dt / 20), 0.0, 1.0);
    flash *= std::exp(-dt * 7);
    if (flash2_ >= 0) {
        flash2_ -= dt;
        if (flash2_ < 0) flash = std::max(flash, 0.8);
    }
    if (s.storm > 0.35) {
        nextFlash_ -= dt * s.storm;
        if (nextFlash_ < 0) {
            double strength = 0.6 + rnd() * 0.4;
            flash = strength;
            flash2_ = rnd() < 0.6 ? 0.12 + rnd() * 0.1 : -1;
            nextFlash_ = 5 + rnd() * 16;
            ev.lightning = true;
            ev.thunderIn = 0.5 + rnd() * 3.5;
            ev.strength = strength;
        }
    }
    windDir += std::sin(elapsed / 400) * 0.0004 * dt * 60;
    gust = s.gust * gustPulse(elapsed);
    return ev;
}

}  // namespace weather

namespace sky {

static const double RAD = M_PI / 180;

static double julian(std::time_t t) { return double(t) / 86400.0 + 2440587.5; }
static double sidereal(double jd, double lon) {
    double d = jd - 2451545.0;
    double v = std::fmod(280.46061837 + 360.98564736629 * d + lon, 360.0);
    if (v < 0) v += 360;
    return v * RAD;
}
static Body horizontal(double ra, double dec, double lst, double lat) {
    double H = lst - ra, phi = lat * RAD;
    double alt = std::asin(std::sin(phi) * std::sin(dec) + std::cos(phi) * std::cos(dec) * std::cos(H));
    double az = std::atan2(-std::sin(H) * std::cos(dec), std::cos(phi) * std::sin(dec) - std::sin(phi) * std::cos(dec) * std::cos(H));
    return Body{alt / RAD, std::fmod(az + 2 * M_PI, 2 * M_PI)};
}

Body sunPosition(std::time_t t, double lat, double lon) {
    double jd = julian(t), n = jd - 2451545.0;
    double L = (280.46 + 0.9856474 * n) * RAD, g = (357.528 + 0.9856003 * n) * RAD;
    double lambda = L + (1.915 * std::sin(g) + 0.02 * std::sin(2 * g)) * RAD;
    double eps = (23.439 - 0.0000004 * n) * RAD;
    double ra = std::atan2(std::cos(eps) * std::sin(lambda), std::cos(lambda));
    double dec = std::asin(std::sin(eps) * std::sin(lambda));
    return horizontal(ra, dec, sidereal(jd, lon), lat);
}

MoonInfo moonPhase(std::time_t t) {
    const double SYN = 29.530588853;
    const double NEW_MOON = 10962.7598;  // 2000-01-06 18:14 UTC in days since epoch
    double days = double(t) / 86400.0 - NEW_MOON;
    double age = std::fmod(std::fmod(days, SYN) + SYN, SYN);
    double ang = age / SYN * 2 * M_PI;
    MoonInfo m;
    m.age = age;
    m.illumination = (1 - std::cos(ang)) / 2;
    m.waxing = age < SYN / 2;
    return m;
}

MoonInfo moonPosition(std::time_t t, double lat, double lon) {
    double jd = julian(t), d = jd - 2451545.0;
    double L = (218.316 + 13.176396 * d) * RAD, M = (134.963 + 13.064993 * d) * RAD, F = (93.272 + 13.22935 * d) * RAD;
    double lng = L + 6.289 * RAD * std::sin(M), latm = 5.128 * RAD * std::sin(F);
    double eps = 23.4397 * RAD;
    double ra = std::atan2(std::sin(lng) * std::cos(eps) - std::tan(latm) * std::sin(eps), std::cos(lng));
    double dec = std::asin(std::sin(latm) * std::cos(eps) + std::cos(latm) * std::sin(eps) * std::sin(lng));
    MoonInfo m = moonPhase(t);
    m.pos = horizontal(ra, dec, sidereal(jd, lon), lat);
    return m;
}

double todFromElevation(double e) {
    if (e >= 35) return 0.02;
    if (e >= 16) return 0.02 + (35 - e) / 19 * 0.4;
    if (e >= 4) return 0.42 + (16 - e) / 12 * 0.24;
    if (e >= -3) return 0.66 + (4 - e) / 7 * 0.17;
    if (e >= -12) return 0.83 + (-3 - e) / 9 * 0.17;
    return 1;
}

Body stylisedSun(double tod, double base, double palEl) { return Body{palEl, base + 0.95 - tod * 0.9}; }
Body stylisedMoon(double tod, double base) {
    double night = std::clamp((tod - 0.62) / 0.3, 0.0, 1.0);
    return Body{-8 + night * 40, base + M_PI - 0.7 + night * 0.3};
}

void skyVector(const Body& b, double out[3]) {
    double e = b.elevation * RAD;
    out[0] = std::sin(b.azimuth) * std::cos(e);
    out[1] = std::sin(e);
    out[2] = std::cos(b.azimuth) * std::cos(e);
}

}  // namespace sky
