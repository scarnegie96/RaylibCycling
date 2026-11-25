#pragma once
// Pure BLE protocol encode/decode (no BLE library dependency, unit-tested).
//  - FTMS (Fitness Machine Service 0x1826): Indoor Bike Data 0x2AD2, Control Point 0x2AD9
//  - CPS  (Cycling Power Service 0x1818):  Cycling Power Measurement 0x2A63
//  - HRS  (Heart Rate Service 0x180D):      Heart Rate Measurement 0x2A37
//  - Wahoo legacy trainer control characteristic (fallback for old Kickr firmware)
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace proto {

using Bytes = std::vector<uint8_t>;

// Full 128-bit lowercase UUIDs (the format SimpleBLE reports on every platform)
std::string uuid16(uint16_t short_uuid);

constexpr uint16_t SVC_FTMS = 0x1826;
constexpr uint16_t SVC_CPS = 0x1818;
constexpr uint16_t SVC_HRS = 0x180D;
constexpr uint16_t CHR_INDOOR_BIKE_DATA = 0x2AD2;
constexpr uint16_t CHR_FTMS_CONTROL_POINT = 0x2AD9;
constexpr uint16_t CHR_FTMS_STATUS = 0x2ADA;
constexpr uint16_t CHR_RESISTANCE_RANGE = 0x2AD6;
constexpr uint16_t CHR_POWER_RANGE = 0x2AD8;
constexpr uint16_t CHR_CP_MEASUREMENT = 0x2A63;
constexpr uint16_t CHR_HR_MEASUREMENT = 0x2A37;
extern const char* WAHOO_TRAINER_CHR;  // a026e005-0a7d-4ab3-97fa-f1500f9feb8b (inside CPS)

// ------------------------------------------------------------------ decode
struct IndoorBikeData {
    std::optional<double> speed_kmh;
    std::optional<double> cadence_rpm;
    std::optional<int> power_w;
    std::optional<int> heart_rate;
    std::optional<uint32_t> total_distance_m;
    std::optional<int> resistance;
};
std::optional<IndoorBikeData> parseIndoorBikeData(const uint8_t* d, size_t n);

struct CyclingPower {
    int power_w = 0;
    std::optional<uint16_t> crank_revs;        // cumulative
    std::optional<uint16_t> crank_event_1024;  // last crank event time, 1/1024 s
    std::optional<uint32_t> wheel_revs;
    std::optional<uint16_t> wheel_event_2048;
};
std::optional<CyclingPower> parseCyclingPower(const uint8_t* d, size_t n);

struct HeartRate {
    int bpm = 0;
    std::vector<double> rr_ms;
};
std::optional<HeartRate> parseHeartRate(const uint8_t* d, size_t n);

// Turns successive CPS crank samples into cadence (handles 16-bit wrap & stale repeats)
class CrankCadence {
public:
    // Returns a cadence when a new crank event arrives. Returns 0 after `stale_s`
    // seconds without a new event.
    std::optional<double> update(uint16_t revs, uint16_t evt_1024, double now_s, double stale_s = 2.5);
private:
    bool have_ = false;
    uint16_t last_revs_ = 0, last_evt_ = 0;
    double last_change_s_ = 0;
};

// FTMS control point response: 0x80, request op, result (1 = success)
struct ControlResponse { uint8_t request_op = 0; uint8_t result = 0; };
std::optional<ControlResponse> parseControlResponse(const uint8_t* d, size_t n);
const char* controlResultName(uint8_t result);

// Machine Status (0x2ADA)
enum class MachineStatus { Reset, StoppedOrPaused, StartedOrResumed, TargetPowerChanged, SimulationChanged, ControlPermissionLost, Other };
std::optional<MachineStatus> parseMachineStatus(const uint8_t* d, size_t n);
// Supported Power / Resistance Range: min, max (sint16), increment (uint16)
struct Range { int16_t min, max; uint16_t inc; };
std::optional<Range> parseRange(const uint8_t* d, size_t n);

// ------------------------------------------------------------------ encode (FTMS)
Bytes ftmsRequestControl();
Bytes ftmsReset();
Bytes ftmsStart();
Bytes ftmsSetTargetPower(int watts);
// Level mode: unitless resistance, 0.1 resolution (uint8 per FTMS 1.0)
Bytes ftmsSetTargetResistance(double level);
Bytes ftmsStop(bool pause);
// grade in percent (e.g. 4.5), crr e.g. 0.004, cw (wind resistance coefficient, kg/m) e.g. 0.51
Bytes ftmsSetSimulation(double grade_pct, double wind_mps = 0.0, double crr = 0.004, double cw = 0.51);

// ------------------------------------------------------------------ encode (Wahoo legacy)
Bytes wahooUnlock();
Bytes wahooSetErg(int watts);
Bytes wahooInitSim(double total_mass_kg, double crr = 0.004, double cw = 0.51);
Bytes wahooSetGrade(double grade_pct);

}  // namespace proto
