#pragma once
// Calendar helpers (civil-date arithmetic, local time, RFC 3339) and CoreKick's
// SplitMix64 RNG so seeded routes come out identical to CoreKick's.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>

struct Date {
    int y = 1970, m = 1, d = 1;
    // days since 1970-01-01
    long days() const;
    static Date fromDays(long z);
    Date plus(long n) const { return fromDays(days() + n); }
    int weekday() const;  // 0 = Monday … 6 = Sunday
    Date monday() const { return plus(-weekday()); }
    std::string str() const;  // YYYY-MM-DD
    static std::optional<Date> parse(const std::string& s);
    static Date today();
    bool operator<(const Date& o) const { return days() < o.days(); }
    bool operator<=(const Date& o) const { return days() <= o.days(); }
    bool operator==(const Date& o) const { return days() == o.days(); }
    long operator-(const Date& o) const { return days() - o.days(); }
};

namespace dates {
// "2026-11-20T18:00:00+00:00" → unix seconds
std::optional<std::time_t> parseRfc3339(const std::string& s);
// local calendar date of an RFC 3339 timestamp
std::optional<Date> localDate(const std::string& rfc3339);
std::string nowRfc3339();                    // local time with offset, like chrono's to_rfc3339
std::string toRfc3339(std::time_t t);        // local time with offset
int utcOffsetSeconds(std::time_t t);
std::string newId();                         // random UUID v4
const char* monthName(int m);
const char* weekdayShort(int wd);            // 0 = Monday
}  // namespace dates

class Rng {
public:
    explicit Rng(uint64_t seed) : s_(seed ^ 0x9E3779B97F4A7C15ULL) {}
    uint64_t next() {
        s_ += 0x9E3779B97F4A7C15ULL;
        uint64_t z = s_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    double f64() { return double(next() >> 11) / double(1ULL << 53); }
    double range(double lo, double hi) { return lo + (hi - lo) * f64(); }
    bool chance(double p) { return f64() < p; }
    double normal() {
        double u1 = std::max(f64(), 1e-12), u2 = f64();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * M_PI * u2);
    }

private:
    uint64_t s_;
};
