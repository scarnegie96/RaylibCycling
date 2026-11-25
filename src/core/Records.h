#pragma once
// Ride records, events and account statistics (ports of CoreKick model.rs,
// stats.rs, ftp_estimate.rs and events.rs). JSON layouts match CoreKick's serde
// output so rides, routes and events can move between the two apps.
#include <array>
#include <optional>
#include <string>
#include <vector>

#include "Dates.h"
#include "Json.h"
#include "Workout.h"

enum class RideKind { Free, Route, Workout, Test };
const char* rideKindId(RideKind k);
const char* rideKindName(RideKind k);
RideKind rideKindFromId(const std::string& s);
const char* testKindId(wk::TestKind k);
std::optional<wk::TestKind> testKindFromId(const std::string& s);

struct Samples {
    std::vector<uint32_t> t;
    std::vector<float> power, cadence, hr, speed, distance, ele, grade, target;
    size_t size() const { return power.size(); }
    std::vector<double> powerD() const { return std::vector<double>(power.begin(), power.end()); }
    json::Value toJson() const;
    static Samples fromJson(const json::Value& v);
};

struct Split {
    int index = 0;
    std::string kind, name;  // "lap" | "route_lap" | "sector"
    double startS = 0, durationS = 0, distanceM = 0, elevationGainM = 0;
    double avgPower = 0, maxPower = 0, np = 0, avgCadence = 0, avgSpeedKmh = 0, avgGradePct = 0;
    std::optional<double> avgHr;
    static std::optional<Split> fromSamples(const Samples& s, size_t a, size_t b, const std::string& kind, const std::string& name, int index);
    json::Value toJson() const;
    static Split fromJson(const json::Value& v);
};

json::Value testResultJson(const wk::TestResult& r);
std::optional<wk::TestResult> testResultFromJson(const json::Value& v);

struct RideSummary {
    std::string id, name, startedAt, endedAt;
    RideKind kind = RideKind::Free;
    std::optional<std::string> routeId, routeName, workoutId, workoutName, eventId;
    std::optional<wk::TestKind> test;
    int laps = 1;
    bool completed = false;
    std::optional<double> routeTimeS;
    bool personalBest = false;
    double elapsedS = 0, movingS = 0, distanceM = 0, elevationGainM = 0, avgSpeedMps = 0, maxSpeedMps = 0;
    double avgPower = 0, maxPower = 0, np = 0, intensityFactor = 0, tss = 0, variabilityIndex = 0;
    double workKj = 0, caloriesKcal = 0;
    std::optional<double> caloriesHrKcal, avgHr, maxHr;
    double avgCadence = 0, maxCadence = 0, ftp = 0, weightKg = 0;
    std::vector<std::pair<int, double>> powerBests;
    std::array<double, 7> zoneS{};
    std::optional<wk::TestResult> testResult;
    std::string notes;
    std::vector<Split> splits;
    bool routeModified = false;
    std::vector<double> hrZoneS;
    std::optional<unsigned long long> stravaActivityId;
    bool simulated = false;
    json::Value toJson() const;
    static RideSummary fromJson(const json::Value& v);
};

struct RideRecord {
    RideSummary summary;
    Samples samples;
};

struct GhostTrace {
    std::string rideId, startedAt;
    double totalS = 0;
    std::vector<float> t, d;
    static GhostTrace fromRecord(const RideRecord& r);
    std::optional<double> timeAtDistance(double dist) const;
    double distanceAtTime(double t) const;
    bool empty() const { return d.empty(); }
};

// ---------------------------------------------------------------- events
enum class Freq { Once, Daily, Weekly, EveryNDays };
struct Recurrence {
    Freq freq = Freq::Weekly;
    std::string startDate, time = "18:00";
    std::optional<std::string> endDate;
    std::vector<int> weekdays;  // 0 = Monday
    int interval = 1;
};
struct EventTarget {
    enum Type { Route, Workout, Test } type = Route;
    std::string routeId;
    int laps = 1;
    std::string workoutId;
    std::optional<std::string> workoutRouteId;
    wk::TestKind test = wk::TestKind::Ramp;
};
struct Event {
    std::string id, name, description, createdAt, color = "#1DB870";
    EventTarget target;
    Recurrence recurrence;
    bool active = true;
    json::Value toJson() const;
    static std::optional<Event> fromJson(const json::Value& v);
};
struct Occurrence { std::string eventId, eventName, color; Date date; std::string time; };
bool occursOn(const Event& e, const Date& d);
std::vector<Date> occurrences(const Event& e, const Date& from, const Date& to);
std::vector<Occurrence> upcoming(const std::vector<Event>& events, const Date& from, int days);
std::string recurrenceLabel(const Recurrence& r);

// ---------------------------------------------------------------- stats
struct FtpEntry { std::string date; double ftp = 0; std::string source; };

struct Totals {
    int rides = 0;
    double distanceM = 0, movingS = 0, elevationM = 0, workKj = 0, caloriesKcal = 0, tss = 0;
    void add(const RideSummary& r);
};
struct WeekBucket { Date weekStart; Totals totals; };
struct FitnessPoint { Date date; double tss = 0, ctl = 0, atl = 0, tsb = 0; };
struct RecordRef { std::string rideId, rideName; Date date; double value = 0; };
struct Records {
    std::optional<RecordRef> longestDistance, longestTime, mostElevation, mostWork, highestNp, highestTss;
};
struct CurvePoint { int durationS = 0; double watts = 0, wkg = 0; std::string rideId; Date date; };
struct AccountStats {
    Totals allTime, thisWeek, lastWeek, thisMonth, thisYear;
    std::vector<WeekBucket> weekly;
    std::vector<FitnessPoint> fitness;
    std::vector<CurvePoint> powerCurveAll, powerCurve90d;
    Records records;
    std::vector<FtpEntry> ftpHistory;
    std::array<double, 7> zoneS28d{};
    int weekStreak = 0;
};
AccountStats computeStats(const std::vector<RideSummary>& rides, double weightKg, std::vector<FtpEntry> ftpHistory, const Date& today);

// ---------------------------------------------------------------- eFTP
struct EffortPoint { int durationS = 0; double watts = 0; std::string rideId, rideName, date; };
struct FtpEstimate {
    double estimate = 0;
    std::string method;  // critical_power | best_20min
    std::optional<double> cp, wPrimeKj, r2;
    std::vector<EffortPoint> points;
    std::string confidence;  // high | medium | low
    int ridesConsidered = 0;
    bool hardRideInWindow = false;
};
enum class Suggestion { Increase, Decrease, Hold, NotEnoughData };
struct FtpAssessment {
    std::string date;
    double currentFtp = 0;
    std::optional<FtpEstimate> estimate;
    std::optional<double> suggestedFtp;
    double changeW = 0, changePct = 0;
    Suggestion suggestion = Suggestion::NotEnoughData;
    std::string reason;
    std::optional<std::string> triggerRideId;
    bool autoApplied = false, dismissed = false;
    json::Value toJson() const;
    static std::optional<FtpAssessment> fromJson(const json::Value& v);
};
bool isValidFtpRide(const RideSummary& r);
std::optional<std::tuple<double, double, double>> fitCp(const std::vector<std::pair<double, double>>& pts);
std::optional<FtpEstimate> estimateFtp(const std::vector<RideSummary>& rides, const Date& today);
FtpAssessment assessFtp(double currentFtp, std::optional<FtpEstimate> est, const Date& date, std::optional<std::string> trigger);
