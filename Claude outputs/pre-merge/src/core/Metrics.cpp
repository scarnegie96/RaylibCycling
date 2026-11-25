#include "Metrics.h"

#include <algorithm>
#include <cmath>

namespace metrics {

const int BEST_DURATIONS[14] = {1, 5, 15, 30, 60, 120, 180, 300, 480, 600, 1200, 1800, 3600, 5400};
const double ZONE_BOUNDS[6] = {0.55, 0.75, 0.90, 1.05, 1.20, 1.50};
const char* ZONE_NAMES[7] = {"Active Recovery", "Endurance", "Tempo", "Threshold", "VO2 Max", "Anaerobic", "Neuromuscular"};

double mean(const std::vector<double>& x) {
    if (x.empty()) return 0;
    double s = 0;
    for (double v : x) s += v;
    return s / x.size();
}

double normalizedPower(const std::vector<double>& p) {
    const size_t W = 30;
    if (p.empty()) return 0;
    if (p.size() < W) return mean(p);
    double sum = 0;
    for (size_t i = 0; i < W; ++i) sum += p[i];
    double acc = std::pow(sum / W, 4);
    size_t n = 1;
    for (size_t i = W; i < p.size(); ++i) {
        sum += p[i] - p[i - W];
        acc += std::pow(std::max(0.0, sum / W), 4);
        ++n;
    }
    return std::pow(acc / n, 0.25);
}

double intensityFactor(double np, double ftp) { return ftp > 0 ? np / ftp : 0; }

double tss(double s, double np, double ftp) {
    if (ftp <= 0) return 0;
    double iff = np / ftp;
    return s * np * iff / (ftp * 3600.0) * 100.0;
}

std::optional<double> bestAverage(const std::vector<double>& p, size_t secs) {
    if (secs == 0 || p.size() < secs) return std::nullopt;
    double sum = 0;
    for (size_t i = 0; i < secs; ++i) sum += p[i];
    double best = sum;
    for (size_t i = secs; i < p.size(); ++i) {
        sum += p[i] - p[i - secs];
        best = std::max(best, sum);
    }
    return best / secs;
}

std::vector<std::pair<int, double>> powerBests(const std::vector<double>& p) {
    std::vector<std::pair<int, double>> out;
    for (int d : BEST_DURATIONS)
        if (auto w = bestAverage(p, d)) out.push_back({d, std::round(*w * 10) / 10});
    return out;
}

int zoneOf(double power, double ftp) {
    if (ftp <= 0) return 0;
    double f = power / ftp;
    for (int i = 0; i < 6; ++i)
        if (f <= ZONE_BOUNDS[i]) return i;
    return 6;
}

std::array<double, 7> timeInZones(const std::vector<double>& p, double ftp) {
    std::array<double, 7> z{};
    for (double v : p) z[zoneOf(v, ftp)] += 1;
    return z;
}

double caloriesFromWork(double kj, double ge) { return kj / (4.184 * std::clamp(ge, 0.15, 0.30)); }

double caloriesFromHr(double hr, double minutes, double w, int age, bool male) {
    double perMin = male ? (-55.0969 + 0.6309 * hr + 0.1988 * w + 0.2017 * age) / 4.184
                         : (-20.4022 + 0.4472 * hr - 0.1263 * w + 0.074 * age) / 4.184;
    return std::max(0.0, perMin * minutes);
}

}  // namespace metrics
