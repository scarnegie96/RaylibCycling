#include "Protocol.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace proto {

const char* WAHOO_TRAINER_CHR = "a026e005-0a7d-4ab3-97fa-f1500f9feb8b";

std::string uuid16(uint16_t s) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "0000%04x-0000-1000-8000-00805f9b34fb", s);
    return buf;
}

namespace {
struct Reader {
    const uint8_t* d;
    size_t n, i = 0;
    bool has(size_t k) const { return i + k <= n; }
    uint8_t u8() { return d[i++]; }
    uint16_t u16() { uint16_t v = uint16_t(d[i] | (d[i + 1] << 8)); i += 2; return v; }
    int16_t s16() { return int16_t(u16()); }
    uint32_t u24() { uint32_t v = d[i] | (d[i + 1] << 8) | (uint32_t(d[i + 2]) << 16); i += 3; return v; }
    uint32_t u32() { uint32_t v = d[i] | (d[i + 1] << 8) | (uint32_t(d[i + 2]) << 16) | (uint32_t(d[i + 3]) << 24); i += 4; return v; }
    void skip(size_t k) { i += k; }
};
void put16(Bytes& b, int v) { b.push_back(uint8_t(v & 0xFF)); b.push_back(uint8_t((v >> 8) & 0xFF)); }
}  // namespace

// FTMS spec 4.9.1 Indoor Bike Data
std::optional<IndoorBikeData> parseIndoorBikeData(const uint8_t* d, size_t n) {
    if (n < 2) return std::nullopt;
    Reader r{d, n};
    uint16_t f = r.u16();
    IndoorBikeData out;
    // bit0 "More Data": when 0, Instantaneous Speed IS present
    if (!(f & 0x0001)) { if (!r.has(2)) return out; out.speed_kmh = r.u16() * 0.01; }
    if (f & 0x0002) { if (!r.has(2)) return out; r.skip(2); }  // average speed
    if (f & 0x0004) { if (!r.has(2)) return out; out.cadence_rpm = r.u16() * 0.5; }
    if (f & 0x0008) { if (!r.has(2)) return out; r.skip(2); }  // average cadence
    if (f & 0x0010) { if (!r.has(3)) return out; out.total_distance_m = r.u24(); }
    if (f & 0x0020) { if (!r.has(2)) return out; out.resistance = r.s16(); }
    if (f & 0x0040) { if (!r.has(2)) return out; out.power_w = r.s16(); }
    if (f & 0x0080) { if (!r.has(2)) return out; r.skip(2); }  // average power
    if (f & 0x0100) { if (!r.has(5)) return out; r.skip(5); }  // expended energy
    if (f & 0x0200) { if (!r.has(1)) return out; out.heart_rate = r.u8(); }
    return out;
}

// CPS spec 3.2 Cycling Power Measurement
std::optional<CyclingPower> parseCyclingPower(const uint8_t* d, size_t n) {
    if (n < 4) return std::nullopt;
    Reader r{d, n};
    uint16_t f = r.u16();
    CyclingPower out;
    out.power_w = r.s16();
    if (f & 0x0001) { if (!r.has(1)) return out; r.skip(1); }  // pedal power balance
    if (f & 0x0004) { if (!r.has(2)) return out; r.skip(2); }  // accumulated torque
    if (f & 0x0010) {
        if (!r.has(6)) return out;
        out.wheel_revs = r.u32();
        out.wheel_event_2048 = r.u16();
    }
    if (f & 0x0020) {
        if (!r.has(4)) return out;
        out.crank_revs = r.u16();
        out.crank_event_1024 = r.u16();
    }
    return out;
}

std::optional<HeartRate> parseHeartRate(const uint8_t* d, size_t n) {
    if (n < 2) return std::nullopt;
    Reader r{d, n};
    uint8_t f = r.u8();
    HeartRate hr;
    if (f & 0x01) { if (!r.has(2)) return std::nullopt; hr.bpm = r.u16(); }
    else hr.bpm = r.u8();
    if (f & 0x08) { if (!r.has(2)) return hr; r.skip(2); }  // energy expended
    if (f & 0x10) {
        while (r.has(2)) hr.rr_ms.push_back(r.u16() * 1000.0 / 1024.0);
    }
    return hr;
}

std::optional<double> CrankCadence::update(uint16_t revs, uint16_t evt, double now, double stale) {
    if (!have_) {
        have_ = true; last_revs_ = revs; last_evt_ = evt; last_change_s_ = now;
        return std::nullopt;
    }
    uint16_t dr = uint16_t(revs - last_revs_);
    uint16_t dt = uint16_t(evt - last_evt_);
    if (dr == 0 || dt == 0) {
        if (now - last_change_s_ > stale) return 0.0;
        return std::nullopt;
    }
    // After a stop longer than `stale` the event clock (1/1024 s, wraps every 64 s) can't
    // tell us how long the gap was, so the first new event only re-arms the baseline:
    // otherwise it reads as a bogus low cadence (e.g. 1 rev over 40 s = 1.5 rpm).
    bool resumed = now - last_change_s_ > stale;
    last_revs_ = revs; last_evt_ = evt; last_change_s_ = now;
    if (resumed) return std::nullopt;
    double rpm = dr * 60.0 * 1024.0 / dt;
    if (rpm > 220.0) return std::nullopt;  // glitch
    return rpm;
}

std::optional<ControlResponse> parseControlResponse(const uint8_t* d, size_t n) {
    if (n < 3 || d[0] != 0x80) return std::nullopt;
    return ControlResponse{d[1], d[2]};
}

const char* controlResultName(uint8_t r) {
    switch (r) {
        case 0x01: return "success";
        case 0x02: return "op code not supported";
        case 0x03: return "invalid parameter";
        case 0x04: return "operation failed";
        case 0x05: return "control not permitted";
        default: return "unknown";
    }
}

Bytes ftmsRequestControl() { return {0x00}; }
Bytes ftmsReset() { return {0x01}; }
Bytes ftmsStart() { return {0x07}; }

Bytes ftmsSetTargetPower(int watts) {
    watts = std::clamp(watts, 0, 2000);
    Bytes b{0x05};
    put16(b, watts);
    return b;
}

Bytes ftmsSetTargetResistance(double level) {
    return {0x04, uint8_t(std::clamp<long>(std::lround(level * 10.0), 0, 255))};
}
Bytes ftmsStop(bool pause) { return {0x08, uint8_t(pause ? 0x02 : 0x01)}; }

std::optional<MachineStatus> parseMachineStatus(const uint8_t* d, size_t n) {
    if (n < 1) return std::nullopt;
    switch (d[0]) {
        case 0x01: return MachineStatus::Reset;
        case 0x02: return MachineStatus::StoppedOrPaused;
        case 0x04: return MachineStatus::StartedOrResumed;
        case 0x08: return MachineStatus::TargetPowerChanged;
        case 0x12: return MachineStatus::SimulationChanged;
        case 0xFF: return MachineStatus::ControlPermissionLost;
        default: return MachineStatus::Other;
    }
}

std::optional<Range> parseRange(const uint8_t* d, size_t n) {
    if (n < 6) return std::nullopt;
    Reader r{d, n};
    Range x;
    x.min = r.s16(); x.max = r.s16(); x.inc = r.u16();
    return x;
}

Bytes ftmsSetSimulation(double grade_pct, double wind_mps, double crr, double cw) {
    Bytes b{0x11};
    put16(b, int(std::lround(std::clamp(wind_mps, -32.0, 32.0) * 1000.0)));
    put16(b, int(std::lround(std::clamp(grade_pct, -40.0, 40.0) * 100.0)));
    b.push_back(uint8_t(std::clamp<long>(std::lround(crr * 10000.0), 0, 255)));
    b.push_back(uint8_t(std::clamp<long>(std::lround(cw * 100.0), 0, 255)));
    return b;
}

// Wahoo legacy trainer protocol (as used by GoldenCheetah / other open-source apps)
Bytes wahooUnlock() { return {0x20, 0xEE, 0xFC}; }
Bytes wahooSetErg(int watts) {
    Bytes b{0x42};
    put16(b, std::clamp(watts, 0, 2000));
    return b;
}
Bytes wahooInitSim(double mass, double crr, double cw) {
    Bytes b{0x43};
    put16(b, int(std::lround(mass * 100.0)));
    put16(b, int(std::lround(crr * 10000.0)));
    put16(b, int(std::lround(cw * 1000.0)));
    return b;
}
Bytes wahooSetResistance(double level01) {
    // 0x40: resistance mode, (1 - level) * 16383 (GoldenCheetah KickrDevice)
    double l = std::clamp(level01, 0.0, 1.0);
    Bytes b{0x40};
    put16(b, int(std::lround((1.0 - l) * 16383.0)));
    return b;
}
Bytes wahooSetGrade(double grade_pct) {
    double g = std::clamp(grade_pct / 100.0, -1.0, 1.0);
    int norm = int(std::min(65535.0, (g + 1.0) * 65536.0 / 2.0));
    Bytes b{0x46};
    put16(b, norm);
    return b;
}

// ------------------------------------------------------------------ Zwift controllers
namespace zwift {
const char* SERVICE = "00000001-19ca-4651-86e5-fa29dcdd09d1";
const char* SERVICE_FC82 = "0000fc82-0000-1000-8000-00805f9b34fb";
const char* ASYNC = "00000002-19ca-4651-86e5-fa29dcdd09d1";
const char* SYNC_RX = "00000003-19ca-4651-86e5-fa29dcdd09d1";
const char* SYNC_TX = "00000004-19ca-4651-86e5-fa29dcdd09d1";

Bytes rideOn() { return {'R', 'i', 'd', 'e', 'O', 'n'}; }

namespace {
// the varint fields of a protobuf message (field -> value); other wire types are skipped
bool varintFields(const uint8_t* d, size_t n, std::vector<std::pair<int, uint64_t>>& out) {
    size_t i = 0;
    auto varint = [&](uint64_t& v) {
        v = 0;
        for (int sh = 0; sh < 64 && i < n; sh += 7) {
            uint8_t b = d[i++];
            v |= uint64_t(b & 0x7F) << sh;
            if (!(b & 0x80)) return true;
        }
        return false;
    };
    while (i < n) {
        uint64_t key;
        if (!varint(key)) return false;
        int field = int(key >> 3), wire = int(key & 7);
        if (wire == 0) { uint64_t v; if (!varint(v)) return false; out.push_back({field, v}); }
        else if (wire == 2) { uint64_t len; if (!varint(len) || i + len > n) return false; i += size_t(len); }
        else if (wire == 5) i += 4;
        else if (wire == 1) i += 8;
        else return false;
    }
    return true;
}
}  // namespace

std::string hex(const uint8_t* d, size_t n, size_t max) {
    std::string o;
    char b[4];
    for (size_t i = 0; i < n && i < max; ++i) { std::snprintf(b, sizeof b, "%02X", d[i]); if (i) o += ' '; o += b; }
    if (n > max) o += " ...";
    return o;
}

std::optional<Buttons> parseButtons(const uint8_t* d, size_t n, bool clickStyle) {
    if (n < 1) return std::nullopt;
    std::vector<std::pair<int, uint64_t>> f;
    if (!varintFields(d + 1, n - 1, f)) return std::nullopt;
    auto get = [&](int k) -> std::optional<uint64_t> { for (auto& [a, v] : f) if (a == k) return v; return std::nullopt; };
    auto pressed = [&](int k) { auto v = get(k); return v && *v == 0; };   // PlayButtonStatus: ON = 0
    Buttons b;
    switch (d[0]) {
        case 0x37:   // Click: 1 = plus, 2 = minus
            b.up = pressed(1);
            b.down = pressed(2);
            return b;
        case 0x07: {  // Play: 1 = which pad (0 right), 2 Y/up, 5 B/down, 7 shift paddle
            bool right = pressed(1);
            bool shiftBtn = pressed(7);
            b.up = pressed(2) || (shiftBtn && right);
            b.down = pressed(5) || (shiftBtn && !right);
            return b;
        }
        case 0x23: {  // Ride: 1 = button map, a bit CLEARED for each held button
            auto m = get(1);
            if (!m) return std::nullopt;
            uint64_t held = ~*m & 0x3FFFF;
            if (clickStyle) {                    // Click v2: one button each side
                b.up = held & (0x2000 | 0x4000 | 0x10000);   // right side
                b.down = held & (0x200 | 0x400 | 0x800);     // left side
            } else {
                b.up = held & (0x200 | 0x2000);    // shift up, left / right
                b.down = held & (0x400 | 0x4000);  // shift down, left / right
            }
            return b;
        }
        default: return std::nullopt;
    }
}

int ShiftEdges::update(const Buttons& b) {
    int r = 0;
    if (b.up && !prev_.up) r += 1;
    if (b.down && !prev_.down) r -= 1;
    prev_ = b;
    return r;
}
}  // namespace zwift

}  // namespace proto
