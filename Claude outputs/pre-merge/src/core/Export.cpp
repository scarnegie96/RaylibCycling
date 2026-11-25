#include "Export.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>

namespace fs = std::filesystem;

namespace exporter {

static const uint16_t CRC_TABLE[16] = {0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
                                       0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400};

uint16_t crc16(const uint8_t* d, size_t n) {
    uint16_t crc = 0;
    for (size_t i = 0; i < n; ++i) {
        uint8_t b = d[i];
        uint16_t tmp = CRC_TABLE[crc & 0xF];
        crc = (crc >> 4) & 0x0FFF;
        crc = crc ^ tmp ^ CRC_TABLE[b & 0xF];
        tmp = CRC_TABLE[crc & 0xF];
        crc = (crc >> 4) & 0x0FFF;
        crc = crc ^ tmp ^ CRC_TABLE[(b >> 4) & 0xF];
    }
    return crc;
}

namespace {
constexpr int64_t FIT_EPOCH_OFFSET = 631065600;
constexpr uint16_t PROFILE_VERSION = 2132;
enum : uint8_t { ENUM = 0x00, UINT8 = 0x02, UINT16 = 0x84, UINT32 = 0x86, UINT32Z = 0x8C };

struct V {
    uint8_t base, size;
    uint32_t value;
    static V e(uint8_t v) { return {ENUM, 1, v}; }
    static V u8(std::optional<uint8_t> v) { return {UINT8, 1, v ? *v : 0xFFu}; }
    static V u16(std::optional<uint16_t> v) { return {UINT16, 2, v ? *v : 0xFFFFu}; }
    static V u32(std::optional<uint32_t> v) { return {UINT32, 4, v ? *v : 0xFFFFFFFFu}; }
    static V u32z(uint32_t v) { return {UINT32Z, 4, v}; }
};
using Fields = std::vector<std::pair<uint8_t, V>>;

struct Writer {
    std::vector<uint8_t> data;
    std::vector<std::pair<uint16_t, std::vector<uint32_t>>> defs;  // global, signature
    void msg(uint16_t global, const Fields& f) {
        std::vector<uint32_t> sig;
        for (auto& [n, v] : f) sig.push_back(uint32_t(n) | (uint32_t(v.size) << 8) | (uint32_t(v.base) << 16));
        int local = -1;
        for (size_t i = 0; i < defs.size(); ++i)
            if (defs[i].first == global && defs[i].second == sig) { local = int(i); break; }
        if (local < 0) {
            size_t slot = defs.size() < 16 ? defs.size() : size_t(global % 16);
            if (slot == defs.size()) defs.push_back({global, sig}); else defs[slot] = {global, sig};
            local = int(slot);
            data.push_back(uint8_t(0x40 | local));
            data.push_back(0);
            data.push_back(0);
            data.push_back(uint8_t(global & 0xFF));
            data.push_back(uint8_t(global >> 8));
            data.push_back(uint8_t(f.size()));
            for (auto& [n, v] : f) { data.push_back(n); data.push_back(v.size); data.push_back(v.base); }
        }
        data.push_back(uint8_t(local));
        for (auto& [n, v] : f)
            for (int k = 0; k < v.size; ++k) data.push_back(uint8_t((v.value >> (8 * k)) & 0xFF));
    }
    std::vector<uint8_t> finish() {
        std::vector<uint8_t> out{14, 0x20, uint8_t(PROFILE_VERSION & 0xFF), uint8_t(PROFILE_VERSION >> 8)};
        uint32_t n = uint32_t(data.size());
        for (int k = 0; k < 4; ++k) out.push_back(uint8_t((n >> (8 * k)) & 0xFF));
        for (char c : std::string(".FIT")) out.push_back(uint8_t(c));
        uint16_t h = crc16(out.data(), out.size());
        out.push_back(uint8_t(h & 0xFF));
        out.push_back(uint8_t(h >> 8));
        out.insert(out.end(), data.begin(), data.end());
        uint16_t c = crc16(out.data(), out.size());
        out.push_back(uint8_t(c & 0xFF));
        out.push_back(uint8_t(c >> 8));
        return out;
    }
};

std::optional<uint8_t> u8o(double x) {
    if (std::isfinite(x) && x > 0) return uint8_t(std::clamp(std::round(x), 0.0, 254.0));
    return std::nullopt;
}
std::optional<uint16_t> u16s(double x, double sc) {
    if (std::isfinite(x) && x >= 0) return uint16_t(std::clamp(std::round(x * sc), 0.0, 65534.0));
    return std::nullopt;
}
std::optional<uint32_t> u32s(double x, double sc) {
    if (std::isfinite(x) && x >= 0) return uint32_t(std::clamp(std::round(x * sc), 0.0, 4294967294.0));
    return std::nullopt;
}
std::optional<uint8_t> optHr(const std::optional<double>& h) { return h ? u8o(*h) : std::nullopt; }

constexpr uint16_t FILE_ID = 0, SESSION = 18, LAP = 19, RECORD = 20, EVENT = 21, DEVICE_INFO = 23, ACTIVITY = 34;
}  // namespace

std::vector<uint8_t> toFit(const RideSummary& s, const Samples& sm, std::time_t startUtc, int utcOffsetS) {
    auto fitTs = [](int64_t unixT) { return uint32_t(std::max<int64_t>(0, unixT - FIT_EPOCH_OFFSET)); };
    uint32_t t0 = fitTs(startUtc);
    size_t n = sm.size();
    uint32_t tEnd = t0 + uint32_t(std::max<size_t>(n, 1));
    Writer w;
    w.msg(FILE_ID, {{0, V::e(4)}, {1, V::u16(255)}, {2, V::u16(1)}, {3, V::u32z(0x0C0E4B1D)}, {4, V::u32(t0)}});
    w.msg(DEVICE_INFO, {{253, V::u32(t0)}, {0, V::u8(0)}, {2, V::u16(255)}, {4, V::u16(1)}, {5, V::u16(100)}});
    w.msg(EVENT, {{253, V::u32(t0)}, {0, V::e(0)}, {1, V::e(0)}});
    for (size_t i = 0; i < n; ++i) {
        double hr = sm.hr[i];
        w.msg(RECORD, {{253, V::u32(t0 + uint32_t(i))},
                       {2, V::u16(u16s(sm.ele[i] + 500.0, 5.0))},
                       {3, V::u8(hr > 0 ? u8o(hr) : std::nullopt)},
                       {4, V::u8(uint8_t(std::clamp(std::round(double(sm.cadence[i])), 0.0, 254.0)))},
                       {5, V::u32(u32s(sm.distance[i], 100.0))},
                       {6, V::u16(u16s(sm.speed[i], 1000.0))},
                       {7, V::u16(u16s(sm.power[i], 1.0))}});
    }
    w.msg(EVENT, {{253, V::u32(tEnd)}, {0, V::e(0)}, {1, V::e(4)}});

    std::vector<Split> laps;
    for (auto& sp : s.splits) if (sp.kind == "lap") laps.push_back(sp);
    if (laps.empty()) {
        Split l;
        l.kind = "lap"; l.name = "Lap 1"; l.index = 1; l.durationS = std::max(s.elapsedS, double(n)); l.distanceM = s.distanceM;
        l.elevationGainM = s.elevationGainM; l.avgPower = s.avgPower; l.maxPower = s.maxPower; l.np = s.np; l.avgHr = s.avgHr;
        l.avgCadence = s.avgCadence; l.avgSpeedKmh = s.avgSpeedMps * 3.6;
        laps.push_back(l);
    }
    for (auto& sp : laps) {
        uint32_t st = t0 + uint32_t(std::lround(sp.startS)), end = st + uint32_t(std::lround(sp.durationS));
        w.msg(LAP, {{253, V::u32(end)}, {0, V::e(9)}, {1, V::e(1)}, {2, V::u32(st)},
                    {7, V::u32(u32s(sp.durationS, 1000))}, {8, V::u32(u32s(sp.durationS, 1000))}, {9, V::u32(u32s(sp.distanceM, 100))},
                    {13, V::u16(u16s(sp.avgSpeedKmh / 3.6, 1000))}, {15, V::u8(optHr(sp.avgHr))}, {17, V::u8(u8o(sp.avgCadence))},
                    {19, V::u16(u16s(sp.avgPower, 1))}, {20, V::u16(u16s(sp.maxPower, 1))}, {21, V::u16(u16s(sp.elevationGainM, 1))},
                    {25, V::e(2)}, {39, V::e(58)}, {33, V::u16(u16s(sp.np, 1))}, {41, V::u32(u32s(sp.avgPower * sp.durationS, 1))}});
    }
    w.msg(SESSION, {{253, V::u32(tEnd)}, {0, V::e(8)}, {1, V::e(1)}, {2, V::u32(t0)}, {5, V::e(2)}, {6, V::e(58)},
                    {7, V::u32(u32s(s.elapsedS, 1000))}, {8, V::u32(u32s(s.elapsedS, 1000))}, {9, V::u32(u32s(s.distanceM, 100))},
                    {11, V::u16(u16s(s.caloriesKcal, 1))}, {14, V::u16(u16s(s.avgSpeedMps, 1000))}, {15, V::u16(u16s(s.maxSpeedMps, 1000))},
                    {16, V::u8(optHr(s.avgHr))}, {17, V::u8(optHr(s.maxHr))}, {18, V::u8(u8o(s.avgCadence))}, {19, V::u8(u8o(s.maxCadence))},
                    {20, V::u16(u16s(s.avgPower, 1))}, {21, V::u16(u16s(s.maxPower, 1))}, {22, V::u16(u16s(s.elevationGainM, 1))},
                    {25, V::u16(0)}, {26, V::u16(uint16_t(laps.size()))}, {34, V::u16(u16s(s.np, 1))}, {35, V::u16(u16s(s.tss, 10))},
                    {36, V::u16(u16s(s.intensityFactor, 1000))}, {45, V::u16(u16s(s.ftp, 1))}, {48, V::u32(u32s(s.workKj * 1000, 1))}});
    w.msg(ACTIVITY, {{253, V::u32(tEnd)}, {0, V::u32(u32s(s.elapsedS, 1000))}, {1, V::u16(1)}, {2, V::e(0)}, {3, V::e(26)}, {4, V::e(1)},
                     {5, V::u32(uint32_t(std::max<int64_t>(0, int64_t(tEnd) + utcOffsetS)))}});
    return w.finish();
}

bool validateFit(const std::vector<uint8_t>& b, std::string& err, size_t* records) {
    if (b.size() < 16 || std::memcmp(&b[8], ".FIT", 4) != 0) { err = "not a FIT file"; return false; }
    size_t hs = b[0];
    size_t ds = b[4] | (b[5] << 8) | (b[6] << 16) | (size_t(b[7]) << 24);
    if (b.size() != hs + ds + 2) { err = "size mismatch"; return false; }
    if (crc16(b.data(), hs + ds) != uint16_t(b[hs + ds] | (b[hs + ds + 1] << 8))) { err = "file CRC mismatch"; return false; }
    if (hs == 14 && crc16(b.data(), 12) != uint16_t(b[12] | (b[13] << 8))) { err = "header CRC mismatch"; return false; }
    // walk messages
    struct Def { uint16_t global; size_t len; };
    Def defs[16] = {};
    size_t p = hs, end = hs + ds, recs = 0;
    while (p < end) {
        uint8_t h = b[p++];
        if (h & 0x80) { err = "compressed headers unsupported"; return false; }
        int local = h & 0x0F;
        if (h & 0x40) {
            if (p + 5 > end) { err = "truncated def"; return false; }
            uint16_t g = uint16_t(b[p + 2] | (b[p + 3] << 8));
            int nf = b[p + 4];
            p += 5;
            size_t len = 0;
            for (int i = 0; i < nf; ++i) { len += b[p + 1]; p += 3; }
            defs[local] = {g, len};
        } else {
            if (p + defs[local].len > end) { err = "truncated data"; return false; }
            if (defs[local].global == RECORD) ++recs;
            p += defs[local].len;
        }
    }
    if (records) *records = recs;
    return true;
}

static std::string isoTime(std::time_t t) {
    char buf[32];
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return buf;
}

std::string toTcx(const RideSummary& s, const Samples& sm, std::time_t start, const std::string& notes) {
    std::string x = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                    "<TrainingCenterDatabase xmlns=\"http://www.garmin.com/xmlschemas/TrainingCenterDatabase/v2\" "
                    "xmlns:ns3=\"http://www.garmin.com/xmlschemas/ActivityExtension/v2\">\n <Activities>\n  <Activity Sport=\"Biking\">\n";
    char b[512];
    x += "   <Id>" + isoTime(start) + "</Id>\n   <Lap StartTime=\"" + isoTime(start) + "\">\n";
    std::snprintf(b, sizeof b,
                  "    <TotalTimeSeconds>%.0f</TotalTimeSeconds>\n    <DistanceMeters>%.1f</DistanceMeters>\n    <Calories>%.0f</Calories>\n"
                  "    <Intensity>Active</Intensity>\n    <TriggerMethod>Manual</TriggerMethod>\n    <Track>\n",
                  s.elapsedS, s.distanceM, s.caloriesKcal);
    x += b;
    for (size_t i = 0; i < sm.size(); ++i) {
        std::string hr = sm.hr[i] > 0 ? "<HeartRateBpm><Value>" + std::to_string(int(std::lround(sm.hr[i]))) + "</Value></HeartRateBpm>" : "";
        std::snprintf(b, sizeof b,
                      "     <Trackpoint><Time>%s</Time><AltitudeMeters>%.1f</AltitudeMeters><DistanceMeters>%.1f</DistanceMeters>%s"
                      "<Cadence>%d</Cadence><Extensions><ns3:TPX><ns3:Speed>%.2f</ns3:Speed><ns3:Watts>%d</ns3:Watts></ns3:TPX></Extensions></Trackpoint>\n",
                      isoTime(start + std::time_t(i)).c_str(), sm.ele[i], sm.distance[i], hr.c_str(), int(std::lround(sm.cadence[i])),
                      sm.speed[i], int(std::lround(sm.power[i])));
        x += b;
    }
    x += "    </Track>\n   </Lap>\n   <Notes>" + notes + "</Notes>\n  </Activity>\n </Activities>\n</TrainingCenterDatabase>\n";
    return x;
}

std::string toCsv(const Samples& sm) {
    std::string out = "time_s,power_w,heart_rate,cadence_rpm,speed_kmh,distance_m,altitude_m,grade_pct,target_w\n";
    char b[200];
    for (size_t i = 0; i < sm.size(); ++i) {
        std::snprintf(b, sizeof b, "%zu,%.0f,%.0f,%.0f,%.2f,%.1f,%.1f,%.2f,%.0f\n", i, sm.power[i], sm.hr[i], sm.cadence[i], sm.speed[i] * 3.6,
                      sm.distance[i], sm.ele[i], sm.grade[i], sm.target[i]);
        out += b;
    }
    return out;
}

std::string ridesDir() {
    const char* home = std::getenv("HOME");
    fs::path p = fs::path(home ? home : ".") / "CoreRay Rides";
    std::error_code ec;
    fs::create_directories(p, ec);
    return p.string();
}

std::string saveRide(const RideSummary& s, const Samples& sm, std::time_t start, const std::string& name, std::string* baseOut) {
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &start);
#else
    localtime_r(&start, &lt);
#endif
    char stamp[40];
    std::strftime(stamp, sizeof stamp, "CoreRay_%Y-%m-%d_%H%M", &lt);
    std::tm gm{};
#ifdef _WIN32
    gmtime_s(&gm, &start);
#else
    gmtime_r(&start, &gm);
#endif
    int offset = int(std::difftime(std::mktime(&lt), std::mktime(&gm)));
    fs::path dir = ridesDir();
    std::string base = stamp;
    {
        auto fit = toFit(s, sm, start, offset);
        std::ofstream f(dir / (base + ".fit"), std::ios::binary);
        f.write(reinterpret_cast<const char*>(fit.data()), std::streamsize(fit.size()));
    }
    std::ofstream(dir / (base + ".tcx")) << toTcx(s, sm, start, "CoreRay - " + name);
    std::ofstream(dir / (base + ".csv")) << toCsv(sm);
    if (baseOut) *baseOut = base;
    return dir.string();
}

}  // namespace exporter
