#include "Route.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "Dates.h"

const char* terrainName(Terrain t) {
    static const char* n[] = {"Flat", "Rolling", "Hilly", "Mountainous", "Alpine"};
    return n[int(t)];
}
const char* terrainId(Terrain t) {
    static const char* n[] = {"flat", "rolling", "hilly", "mountainous", "alpine"};
    return n[int(t)];
}
std::optional<Terrain> terrainFromId(const std::string& s) {
    for (int i = 0; i < 5; ++i) if (s == terrainId(Terrain(i))) return Terrain(i);
    return std::nullopt;
}

// ------------------------------------------------------------------ profile queries
double Route::elevationAt(double d) const {
    size_t n = points.size();
    if (n == 0) return 0;
    if (n == 1 || d <= points[0].d) return points[0].ele;
    if (d >= points[n - 1].d) return points[n - 1].ele;
    auto it = std::partition_point(points.begin(), points.end(), [&](const RoutePoint& p) { return p.d <= d; });
    const RoutePoint& b = *it;
    const RoutePoint& a = *(it - 1);
    double f = b.d > a.d ? (d - a.d) / (b.d - a.d) : 0;
    return a.ele + f * (b.ele - a.ele);
}

double Route::gradeAt(double d, double half) const {
    double total = distance();
    if (total <= 0) return 0;
    double h = std::max(half, 1.0);
    double a = std::max(0.0, d - h), b = std::min(total, d + h);
    if (b - a < 1) return 0;
    return (elevationAt(b) - elevationAt(a)) / (b - a);
}

std::vector<std::pair<double, double>> Route::uniform(double step) const {
    double total = distance();
    size_t n = size_t(std::ceil(total / step));
    std::vector<std::pair<double, double>> v;
    for (size_t i = 0; i <= n; ++i) {
        double d = std::min(i * step, total);
        if (!v.empty() && std::fabs(v.back().first - d) < 1e-9) continue;
        v.push_back({d, elevationAt(d)});
    }
    return v;
}

const char* climbCategory(double s) {
    if (s >= 80000) return "HC";
    if (s >= 64000) return "Cat 1";
    if (s >= 32000) return "Cat 2";
    if (s >= 16000) return "Cat 3";
    if (s >= 8000) return "Cat 4";
    return "Uncategorised";
}

static std::optional<Climb> makeClimb(const std::vector<std::pair<double, double>>& g, size_t s, size_t p) {
    auto slope = [&](size_t i) { double dd = g[i + 1].first - g[i].first; return dd > 0 ? (g[i + 1].second - g[i].second) / dd : 0.0; };
    while (s < p && slope(s) < 0.01) ++s;
    while (p > s && slope(p - 1) < 0.01) --p;
    if (p <= s) return std::nullopt;
    double len = g[p].first - g[s].first, gain = g[p].second - g[s].second;
    if (len < 300 || gain < 15) return std::nullopt;
    double avg = gain / len * 100;
    if (avg < 2) return std::nullopt;
    double mg = 0;
    for (size_t i = s; i < p; ++i) {
        size_t j = std::min(i + 4, p);
        double dd = g[j].first - g[i].first;
        if (dd >= 50) mg = std::max(mg, (g[j].second - g[i].second) / dd * 100);
    }
    double score = len * avg;
    return Climb{g[s].first, g[p].first, len, gain, avg, std::max(mg, avg), g[s].second, g[p].second, score, climbCategory(score)};
}

std::vector<Climb> detectClimbs(const std::vector<std::pair<double, double>>& g) {
    std::vector<Climb> out;
    if (g.size() < 3) return out;
    size_t start = 0, peak = 0;
    for (size_t i = 1; i < g.size(); ++i) {
        double e = g[i].second;
        if (e <= g[start].second && peak == start) { start = peak = i; continue; }
        if (e >= g[peak].second) peak = i;
        else {
            double drop = g[peak].second - e, gain = g[peak].second - g[start].second;
            if (drop > std::max(10.0, 0.25 * gain)) {
                if (auto c = makeClimb(g, start, peak)) out.push_back(*c);
                start = peak = i;
            }
        }
        if (e < g[start].second) start = peak = i;
    }
    if (auto c = makeClimb(g, start, peak)) out.push_back(*c);
    return out;
}

double difficultyScore(double gain, double density, double maxGrade) {
    double s = 1.6 * std::log2(1.0 + std::max(0.0, gain) / 200.0) + density / 10.0 + std::max(0.0, maxGrade - 6.0) / 3.0;
    return std::round(std::clamp(s, 0.0, 10.0) * 10) / 10;
}

const char* difficultyLabel(double s) {
    if (s < 2.5) return "Easy";
    if (s < 4.5) return "Moderate";
    if (s < 6.5) return "Hard";
    if (s < 8.5) return "Very hard";
    return "Extreme";
}

RouteStats Route::stats() const {
    RouteStats st;
    double total = distance();
    st.distanceM = total;
    st.minEle = std::numeric_limits<double>::infinity();
    st.maxEle = -std::numeric_limits<double>::infinity();
    for (size_t i = 1; i < points.size(); ++i) {
        double de = points[i].ele - points[i - 1].ele;
        if (de > 0) st.gainM += de; else st.lossM -= de;
    }
    for (auto& p : points) { st.minEle = std::min(st.minEle, p.ele); st.maxEle = std::max(st.maxEle, p.ele); }
    if (points.empty()) st.minEle = st.maxEle = 0;
    auto grid = uniform(25.0);
    double climbing = 0;
    for (size_t i = 0; i < grid.size(); ++i) {
        size_t j = std::min(i + 4, grid.size() - 1);
        if (j == i) continue;
        double dd = grid[j].first - grid[i].first;
        if (dd < 50) continue;
        double g = (grid[j].second - grid[i].second) / dd * 100;
        st.maxGradePct = std::max(st.maxGradePct, g);
        st.minGradePct = std::min(st.minGradePct, g);
    }
    for (size_t i = 1; i < grid.size(); ++i) {
        double dd = grid[i].first - grid[i - 1].first;
        if (dd > 0 && (grid[i].second - grid[i - 1].second) / dd >= 0.03) climbing += dd;
    }
    st.climbs = detectClimbs(grid);
    double km = std::max(total / 1000.0, 0.001);
    st.climbDensity = st.gainM / km;
    st.climbingShare = total > 0 ? climbing / total : 0;
    st.difficulty = difficultyScore(st.gainM, st.climbDensity, st.maxGradePct);
    st.difficultyLabel = difficultyLabel(st.difficulty);
    return st;
}

double Route::estimateTime(double watts, const phys::Params& p) const {
    auto grid = uniform(50.0);
    double t = 0;
    for (size_t i = 1; i < grid.size(); ++i) {
        double dd = grid[i].first - grid[i - 1].first;
        if (dd <= 0) continue;
        double g = (grid[i].second - grid[i - 1].second) / dd;
        double v = std::clamp(phys::steadySpeed(watts, g, grid[i - 1].second, p), 1.0, 18.0);
        t += dd / v;
    }
    return t;
}

// ------------------------------------------------------------------ generator
static std::vector<double> movingAverage(const std::vector<double>& x, size_t w) {
    if (w <= 1 || x.size() < 2) return x;
    size_t half = w / 2, n = x.size();
    std::vector<double> pre(n + 1, 0.0), out(n);
    for (size_t i = 0; i < n; ++i) pre[i + 1] = pre[i] + x[i];
    for (size_t i = 0; i < n; ++i) {
        size_t a = i >= half ? i - half : 0, b = std::min(i + half + 1, n);
        out[i] = (pre[b] - pre[a]) / double(b - a);
    }
    return out;
}

static std::vector<RoutePoint> pointsFromGrades(const std::vector<double>& g, double step, double startEle) {
    std::vector<RoutePoint> pts;
    double ele = startEle;
    pts.push_back({0, ele});
    for (size_t i = 0; i < g.size(); ++i) { ele += g[i] * step; pts.push_back({double(i + 1) * step, ele}); }
    double mn = 1e18;
    for (auto& p : pts) mn = std::min(mn, p.ele);
    if (mn < 0) for (auto& p : pts) p.ele -= mn;
    for (auto& p : pts) p.ele = std::round(p.ele * 100) / 100;
    return pts;
}

struct TerrainSpec { double segLo, segHi, gLo, gHi, jitter, flatP; };
static TerrainSpec terrainSpec(Terrain t) {
    switch (t) {
        case Terrain::Flat: return {800, 4000, 0.5, 2.0, 0.4, 0.5};
        case Terrain::Rolling: return {250, 1200, 1.5, 5.0, 0.8, 0.2};
        case Terrain::Hilly: return {500, 2500, 3.0, 8.0, 1.2, 0.15};
        case Terrain::Mountainous: return {1500, 7000, 4.5, 9.0, 1.5, 0.1};
        default: return {4000, 14000, 6.0, 10.0, 1.5, 0.05};
    }
}

double suggestedGain(Terrain t, double km) {
    static const double perKm[] = {3, 10, 18, 28, 40};
    return std::round(perKm[int(t)] * km / 10.0) * 10.0;
}

static double scaleComponent(std::vector<double>& g, double step, double target, double limit, bool positive) {
    auto sel = [&](double x) { return positive ? x > 0 : x < 0; };
    auto total = [&] { double s = 0; for (double x : g) if (sel(x)) s += std::fabs(x); return s * step; };
    if (target <= 0) { for (double& x : g) if (sel(x)) x = 0; return 0; }
    for (int it = 0; it < 12; ++it) {
        double have = total();
        if (have < 1e-6) break;
        double f = target / have;
        if (std::fabs(f - 1) < 0.005) break;
        for (double& x : g) if (sel(x)) x = std::clamp(x * f, -limit, limit);
    }
    return total();
}

Route generateRoute(const GenParams& p, const std::string& name) {
    const double step = 20;
    double total = std::clamp(p.distanceKm * 1000, 1000.0, 400000.0);
    size_t n = size_t(std::max(10.0, std::round(total / step)));
    TerrainSpec spec = terrainSpec(p.terrain);
    Rng rng(p.seed);
    double maxG = std::clamp(p.maxGradePct, 1.0, 25.0) / 100;
    std::vector<double> raw(n, 0.0);
    size_t summitLen = p.finish == Finish::Summit ? size_t(std::clamp(total * 0.25, 1500.0, std::max(spec.segHi, 4000.0)) / step) : 0;
    size_t body = n > summitLen ? n - summitLen : 0;
    double typicalUp = std::min(spec.gHi / 100, maxG) * 0.85;
    double neededShare = std::max(0.0, p.targetGainM) / std::max(total * typicalUp, 1.0);
    double bias = std::clamp(neededShare / 0.45, 0.6, 2.5);
    double flatP = neededShare > 0.5 ? spec.flatP * 0.3 : spec.flatP;
    size_t i = 0;
    bool up = rng.chance(0.6);
    while (i < body) {
        bool flat = rng.chance(flatP);
        double mult = flat ? 1.0 : up ? bias : 1.0 / bias;
        double lenM = std::max(std::min(rng.range(spec.segLo, spec.segHi) * mult, total * 0.35), step * 5);
        size_t len = std::max<size_t>(1, size_t(lenM / step));
        double base;
        if (flat) base = rng.normal() * 0.3;
        else {
            double mag = rng.range(spec.gLo, spec.gHi);
            base = up ? mag : -mag * rng.range(0.9, 1.25);
            up = !up;
        }
        double chunk = base;
        for (size_t k = 0; k < len; ++k) {
            if (i >= body) break;
            if (k % 10 == 0) chunk = base + rng.normal() * spec.jitter * (flat ? 0.5 : 1.0);
            raw[i++] = chunk / 100;
        }
    }
    for (size_t k = body; k < n; ++k) {
        double prog = double(k - body) / std::max<size_t>(summitLen, 1);
        double base = spec.gHi * (0.85 + 0.3 * prog);
        raw[k] = std::max(0.01, (base + rng.normal() * spec.jitter * 0.6) / 100);
    }
    size_t w = 3 + size_t(std::clamp(p.smoothness, 0.0, 1.0) * 12);
    auto g = movingAverage(raw, w);
    double targetGain = std::max(0.0, p.targetGainM);
    auto lossFor = [&](double gain) { return p.finish == Finish::Loop ? gain : p.finish == Finish::Free ? gain * 0.85 : gain * 0.45; };
    double gain = scaleComponent(g, step, targetGain, maxG, true);
    scaleComponent(g, step, lossFor(gain), maxG + 0.03, false);
    g = movingAverage(g, 3);
    for (double& x : g) x = std::clamp(x, -(maxG + 0.03), maxG);
    gain = scaleComponent(g, step, targetGain, maxG, true);
    scaleComponent(g, step, lossFor(gain), maxG + 0.03, false);
    if (p.finish == Finish::Loop) {
        double net = 0;
        for (double x : g) net += x;
        net *= step;
        double per = net / (double(n) * step);
        for (double& x : g) x -= per;
    }
    Route r;
    r.id = dates::newId();
    r.name = name;
    r.source = RouteSource::Generated;
    r.createdAt = dates::nowRfc3339();
    r.points = pointsFromGrades(g, step, p.startEleM);
    r.isLoop = p.finish == Finish::Loop;
    r.tags = {terrainId(p.terrain)};
    if (p.finish == Finish::Summit) r.tags.push_back("summit finish");
    if (r.isLoop) r.tags.push_back("loop");
    r.genParams = p;
    return r;
}

Route buildFromSegments(const std::string& name, const std::vector<SegmentSpec>& segs, double startEle, bool smooth) {
    const double step = 10;
    std::vector<double> g;
    for (auto& s : segs) {
        size_t n = size_t(std::round(std::max(0.0, s.lengthM) / step));
        g.insert(g.end(), n, std::clamp(s.gradePct, -25.0, 25.0) / 100);
    }
    if (g.empty()) g.push_back(0);
    if (smooth) g = movingAverage(g, 7);
    Route r;
    r.id = dates::newId();
    r.name = name;
    r.source = RouteSource::Custom;
    r.createdAt = dates::nowRfc3339();
    r.points = pointsFromGrades(g, step, startEle);
    r.isLoop = std::fabs(r.points.front().ele - r.points.back().ele) < 1;
    r.tags = {"custom"};
    r.segments = segs;
    return r;
}

static double haversine(double la1, double lo1, double la2, double lo2) {
    const double R = 6371000, rad = M_PI / 180;
    double p1 = la1 * rad, p2 = la2 * rad, dp = (la2 - la1) * rad, dl = (lo2 - lo1) * rad;
    double a = std::pow(std::sin(dp / 2), 2) + std::cos(p1) * std::cos(p2) * std::pow(std::sin(dl / 2), 2);
    return 2 * R * std::asin(std::sqrt(a));
}

static std::optional<std::string> xmlAttr(const std::string& tag, const char* name) {
    std::string key = std::string(name) + "=";
    size_t p = 0;
    while ((p = tag.find(key, p)) != std::string::npos) {
        if (p > 0 && !std::isspace((unsigned char)tag[p - 1])) { p += key.size(); continue; }
        size_t q = p + key.size();
        if (q >= tag.size()) return std::nullopt;
        char quote = tag[q];
        size_t e = tag.find(quote, q + 1);
        if (e == std::string::npos) return std::nullopt;
        return tag.substr(q + 1, e - q - 1);
    }
    return std::nullopt;
}

bool routeFromGpx(const std::string& text, const std::string& nameOverride, Route& out, std::string& err) {
    std::string name;
    struct P { double lat, lon; std::optional<double> ele; };
    std::vector<P> pts;
    size_t pos = 0;
    while ((pos = text.find('<', pos)) != std::string::npos) {
        size_t gt = text.find('>', pos);
        if (gt == std::string::npos) break;
        std::string tag = text.substr(pos + 1, gt - pos - 1);
        // strip namespace prefixes like <gpx:trkpt>
        std::string tname = tag.substr(0, tag.find_first_of(" \t\r\n/"));
        size_t colon = tname.find(':');
        if (colon != std::string::npos) tname = tname.substr(colon + 1);
        if (tname == "name" && name.empty()) {
            size_t e = text.find('<', gt);
            if (e != std::string::npos) {
                std::string t = text.substr(gt + 1, e - gt - 1);
                while (!t.empty() && std::isspace((unsigned char)t.back())) t.pop_back();
                while (!t.empty() && std::isspace((unsigned char)t.front())) t.erase(t.begin());
                if (t.rfind("<![CDATA[", 0) == 0) t = t.substr(9, t.size() > 12 ? t.size() - 12 : 0);
                name = t;
            }
        } else if (tname == "trkpt" || tname == "rtept") {
            auto la = xmlAttr(tag, "lat"), lo = xmlAttr(tag, "lon");
            if (la && lo) {
                P p{std::atof(la->c_str()), std::atof(lo->c_str()), std::nullopt};
                bool selfClosing = !tag.empty() && tag.back() == '/';
                if (!selfClosing) {
                    size_t end = text.find(tname == "trkpt" ? "trkpt>" : "rtept>", gt);
                    size_t es = text.find("ele>", gt);
                    if (es != std::string::npos && (end == std::string::npos || es < end)) p.ele = std::atof(text.c_str() + es + 4);
                }
                pts.push_back(p);
            }
        }
        pos = gt + 1;
    }
    if (pts.size() < 2) { err = "GPX file contains fewer than two track/route points"; return false; }
    bool hasEle = false;
    for (auto& p : pts) if (p.ele) hasEle = true;
    std::vector<double> dist{0}, ele{pts[0].ele.value_or(0)};
    for (size_t i = 1; i < pts.size(); ++i) {
        double dd = haversine(pts[i - 1].lat, pts[i - 1].lon, pts[i].lat, pts[i].lon);
        if (dd < 0.5) continue;
        dist.push_back(dist.back() + dd);
        ele.push_back(pts[i].ele.value_or(ele.back()));
    }
    double total = dist.back();
    if (total < 200) { err = "GPX track is shorter than 200 m"; return false; }
    const double step = 10;
    size_t n = size_t(std::floor(total / step));
    std::vector<double> res;
    size_t j = 0;
    for (size_t i = 0; i <= n; ++i) {
        double d = i * step;
        while (j + 1 < dist.size() && dist[j + 1] < d) ++j;
        double e = ele[j];
        if (j + 1 < dist.size()) {
            double f = std::clamp((d - dist[j]) / std::max(dist[j + 1] - dist[j], 1e-9), 0.0, 1.0);
            e = ele[j] + f * (ele[j + 1] - ele[j]);
        }
        res.push_back(e);
    }
    auto sm = movingAverage(res, 7);
    Route r;
    for (size_t i = 0; i < sm.size(); ++i) r.points.push_back({i * step, std::round(sm[i] * 100) / 100});
    if (total - r.points.back().d > 1) r.points.push_back({total, ele.back()});
    r.isLoop = haversine(pts.front().lat, pts.front().lon, pts.back().lat, pts.back().lon) < 250;
    r.tags = {"gpx"};
    if (!hasEle) r.tags.push_back("no elevation");
    if (r.isLoop) r.tags.push_back("loop");
    r.id = dates::newId();
    r.name = !nameOverride.empty() ? nameOverride : !name.empty() ? name : "Imported route";
    r.description = hasEle ? "" : "GPX had no elevation data - ridden flat.";
    r.source = RouteSource::Gpx;
    r.createdAt = dates::nowRfc3339();
    out = std::move(r);
    return true;
}

Route endlessRoute(Terrain t, uint64_t seed) {
    double km = 150;
    static const double maxg[] = {2, 6, 9, 11, 11};
    GenParams p;
    p.distanceKm = km; p.targetGainM = suggestedGain(t, km); p.maxGradePct = maxg[int(t)]; p.terrain = t;
    p.finish = Finish::Loop; p.seed = seed; p.startEleM = 200; p.smoothness = 0.6;
    std::string tn = terrainId(t);
    Route r = generateRoute(p, "Endless " + tn);
    r.description = "Procedurally generated road that never ends.";
    return r;
}

std::vector<Route> seedRoutes() {
    auto mk = [](const char* name, const char* desc, double km, double gain, double maxg, Terrain t, Finish f, uint64_t seed, double start) {
        GenParams p;
        p.distanceKm = km; p.targetGainM = gain; p.maxGradePct = maxg; p.terrain = t; p.finish = f; p.seed = seed; p.startEleM = start; p.smoothness = 0.55;
        Route r = generateRoute(p, name);
        r.description = desc;
        r.source = RouteSource::Builtin;
        return r;
    };
    return {
        mk("Coastal Flats", "Fast, flat seafront loop. Perfect for recovery or tempo.", 24, 60, 2.5, Terrain::Flat, Finish::Loop, 11, 8),
        mk("Rolling Downs", "Punchy rollers that reward momentum.", 32, 380, 7, Terrain::Rolling, Finish::Loop, 23, 90),
        mk("Valley Classic", "A hilly one-day-classic profile with a few sharp kickers.", 58, 900, 11, Terrain::Hilly, Finish::Free, 37, 140),
        mk("Col de Kickr", "A long Alpine-style summit finish. Pace it.", 21, 1050, 10.5, Terrain::Alpine, Finish::Summit, 51, 620),
        mk("Gran Fondo Sierra", "Big day out: three major climbs and fast descents.", 96, 2300, 12, Terrain::Mountainous, Finish::Loop, 77, 400),
        mk("Lunchtime Hill Reps", "Short loop with one solid climb - ideal for laps.", 8, 140, 8, Terrain::Hilly, Finish::Loop, 5, 60),
    };
}

// ------------------------------------------------------------------ sectors
std::string SectorSpec::displayName() const {
    std::string n = name;
    while (!n.empty() && std::isspace((unsigned char)n.back())) n.pop_back();
    if (!n.empty()) return n;
    auto km = [](double m) { char b[32]; if (m >= 1000) std::snprintf(b, sizeof b, "%.1f km", m / 1000); else std::snprintf(b, sizeof b, "%.0f m", m); return std::string(b); };
    char b[96];
    switch (kind) {
        case SectorKind::Climb: std::snprintf(b, sizeof b, "Climb %s @ %.1f%%", km(lengthM).c_str(), std::fabs(gradePct)); return b;
        case SectorKind::Descent: std::snprintf(b, sizeof b, "Descent %s @ %.1f%%", km(lengthM).c_str(), std::fabs(gradePct)); return b;
        case SectorKind::Flat: return "Flat " + km(lengthM);
        case SectorKind::Rollers: return "Rollers " + km(lengthM);
        case SectorKind::Sprint: return "Sprint " + km(lengthM);
        case SectorKind::Custom: return "Custom sector";
        case SectorKind::Terrain: return std::string(terrainName(terrain.value_or(Terrain::Hilly))) + " " + km(lengthM);
    }
    return "Sector";
}

std::vector<SegmentSpec> SectorSpec::build(uint64_t seed) const {
    double len = std::clamp(lengthM, 50.0, 50000.0), g = std::clamp(gradePct, -20.0, 20.0);
    switch (kind) {
        case SectorKind::Climb: return {{len, std::max(std::fabs(g), 0.5)}};
        case SectorKind::Descent: return {{len, -std::max(std::fabs(g), 0.5)}};
        case SectorKind::Flat: return {{len, std::clamp(g, -1.0, 1.0)}};
        case SectorKind::Sprint: return {{std::min(len, 1000.0), 0.0}};
        case SectorKind::Rollers: {
            size_t n = std::max<size_t>(2, size_t(std::round(len / 400)) & ~size_t(1));
            double amp = std::fabs(g) < 0.5 ? 4.0 : std::fabs(g);
            std::vector<SegmentSpec> v;
            for (size_t i = 0; i < n; ++i) v.push_back({len / n, i % 2 == 0 ? amp : -amp});
            return v;
        }
        case SectorKind::Custom: {
            std::vector<SegmentSpec> v;
            for (auto& s : segments) if (s.lengthM > 0) v.push_back(s);
            return v;
        }
        case SectorKind::Terrain: {
            Terrain t = terrain.value_or(Terrain::Hilly);
            GenParams p;
            p.distanceKm = std::max(len / 1000, 1.0);
            p.targetGainM = std::max(suggestedGain(t, len / 1000), 10.0);
            p.maxGradePct = t == Terrain::Flat ? 2.5 : t == Terrain::Rolling ? 7 : t == Terrain::Hilly ? 10 : 12;
            p.terrain = t; p.finish = Finish::Loop; p.seed = seed; p.startEleM = 0; p.smoothness = 0.5;
            Route r = generateRoute(p, "chunk");
            std::vector<SegmentSpec> v;
            for (size_t i = 1; i < r.points.size(); ++i) {
                double dd = r.points[i].d - r.points[i - 1].d;
                v.push_back({dd, (r.points[i].ele - r.points[i - 1].ele) / std::max(dd, 1e-6) * 100});
            }
            return v;
        }
    }
    return {};
}

std::pair<double, double> segmentsSummary(const std::vector<SegmentSpec>& segs) {
    double l = 0, e = 0;
    for (auto& s : segs) { l += s.lengthM; e += s.lengthM * s.gradePct / 100; }
    return {l, e};
}

std::optional<SegmentSpec> rejoinSegment(double net) {
    if (std::fabs(net) < 2) return std::nullopt;
    double len = std::max(std::fabs(net) / 0.05, 200.0);
    return SegmentSpec{len, -net / len * 100};
}

std::pair<double, double> insertSegments(Route& r, double at, const std::vector<SegmentSpec>& segs) {
    const double step = 10;
    double total = r.distance();
    at = std::clamp(at, 0.0, total);
    double e0 = r.elevationAt(at);
    std::vector<double> grades;
    for (auto& s : segs) {
        size_t n = std::max<size_t>(1, size_t(std::round(s.lengthM / step)));
        grades.insert(grades.end(), n, std::clamp(s.gradePct, -25.0, 25.0) / 100);
    }
    if (grades.empty()) return {0, 0};
    grades = movingAverage(grades, 5);
    std::vector<RoutePoint> pts;
    for (auto& p : r.points) if (p.d < at) pts.push_back(p);
    pts.push_back({at, e0});
    double d = at, e = e0;
    for (double g : grades) { d += step; e += g * step; pts.push_back({d, std::round(e * 100) / 100}); }
    double ins = d - at, de = e - e0;
    for (auto& p : r.points) if (p.d > at) pts.push_back({p.d + ins, p.ele + de});
    double mn = 1e18;
    for (auto& p : pts) mn = std::min(mn, p.ele);
    if (mn < 0) for (auto& p : pts) p.ele -= mn;
    r.points = std::move(pts);
    return {ins, de};
}

// ------------------------------------------------------------------ JSON (CoreKick serde layout)
static const char* sourceId(RouteSource s) {
    static const char* n[] = {"generated", "custom", "gpx", "builtin"};
    return n[int(s)];
}
static const char* finishId(Finish f) {
    static const char* n[] = {"free", "loop", "summit"};
    return n[int(f)];
}

json::Value Route::toJson() const {
    json::Value v = json::Value::object();
    v["id"] = id; v["name"] = name; v["description"] = description; v["source"] = sourceId(source); v["created_at"] = createdAt;
    json::Value pts = json::Value::array();
    for (auto& p : points) { json::Value o = json::Value::object(); o["d"] = p.d; o["ele"] = p.ele; pts.push(o); }
    v["points"] = pts;
    v["is_loop"] = isLoop;
    json::Value t = json::Value::array();
    for (auto& s : tags) t.push(s);
    v["tags"] = t;
    if (genParams) {
        json::Value g = json::Value::object();
        g["distance_km"] = genParams->distanceKm; g["target_gain_m"] = genParams->targetGainM; g["max_grade_pct"] = genParams->maxGradePct;
        g["terrain"] = terrainId(genParams->terrain); g["finish"] = finishId(genParams->finish); g["seed"] = (unsigned long long)genParams->seed;
        g["start_ele_m"] = genParams->startEleM; g["smoothness"] = genParams->smoothness;
        v["gen_params"] = g;
    } else v["gen_params"] = nullptr;
    if (segments) {
        json::Value a = json::Value::array();
        for (auto& s : *segments) { json::Value o = json::Value::object(); o["length_m"] = s.lengthM; o["grade_pct"] = s.gradePct; a.push(o); }
        v["segments"] = a;
    } else v["segments"] = nullptr;
    v["favourite"] = favourite;
    return v;
}

std::optional<Route> Route::fromJson(const json::Value& v) {
    if (v.type != json::Value::Object) return std::nullopt;
    Route r;
    r.id = v.strOr("id");
    r.name = v.strOr("name", "Route");
    r.description = v.strOr("description");
    r.createdAt = v.strOr("created_at");
    std::string s = v.strOr("source", "custom");
    r.source = s == "generated" ? RouteSource::Generated : s == "gpx" ? RouteSource::Gpx : s == "builtin" ? RouteSource::Builtin : RouteSource::Custom;
    for (auto& p : v.arr("points")) r.points.push_back({p.numOr("d", 0), p.numOr("ele", 0)});
    if (r.points.size() < 2) return std::nullopt;
    r.isLoop = v.boolOr("is_loop", false);
    for (auto& t : v.arr("tags")) if (t.type == json::Value::String) r.tags.push_back(t.s);
    if (auto g = v.get("gen_params"); g && g->type == json::Value::Object) {
        GenParams p;
        p.distanceKm = g->numOr("distance_km", 30); p.targetGainM = g->numOr("target_gain_m", 300); p.maxGradePct = g->numOr("max_grade_pct", 8);
        p.terrain = terrainFromId(g->strOr("terrain", "rolling")).value_or(Terrain::Rolling);
        std::string f = g->strOr("finish", "loop");
        p.finish = f == "free" ? Finish::Free : f == "summit" ? Finish::Summit : Finish::Loop;
        p.seed = uint64_t(g->numOr("seed", 1)); p.startEleM = g->numOr("start_ele_m", 120); p.smoothness = g->numOr("smoothness", 0.5);
        r.genParams = p;
    }
    if (auto sg = v.get("segments"); sg && sg->type == json::Value::Array) {
        std::vector<SegmentSpec> segs;
        for (auto& x : sg->a) segs.push_back({x.numOr("length_m", 0), x.numOr("grade_pct", 0)});
        r.segments = segs;
    }
    r.favourite = v.boolOr("favourite", false);
    return r;
}
