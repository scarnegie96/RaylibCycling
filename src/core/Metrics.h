#pragma once
// Training metrics (port of CoreKick metrics.rs): NP, IF, TSS, bests, zones, calories.
#include <array>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace metrics {

extern const int BEST_DURATIONS[14];
extern const double ZONE_BOUNDS[6];
extern const char* ZONE_NAMES[7];

double mean(const std::vector<double>& x);
double normalizedPower(const std::vector<double>& p);
double intensityFactor(double np, double ftp);
double tss(double seconds, double np, double ftp);
std::optional<double> bestAverage(const std::vector<double>& p, size_t secs);
std::vector<std::pair<int, double>> powerBests(const std::vector<double>& p);
int zoneOf(double power, double ftp);  // 0..6
std::array<double, 7> timeInZones(const std::vector<double>& p, double ftp);
double caloriesFromWork(double kj, double grossEfficiency);
double caloriesFromHr(double avgHr, double minutes, double weightKg, int age, bool male);

}  // namespace metrics
