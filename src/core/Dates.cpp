#include "Dates.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>

// Howard Hinnant's civil-date algorithms
long Date::days() const {
    int yy = y - (m <= 2);
    long era = (yy >= 0 ? yy : yy - 399) / 400;
    unsigned yoe = unsigned(yy - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + long(doe) - 719468;
}

Date Date::fromDays(long z) {
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = unsigned(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = long(yoe) + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned dd = doy - (153 * mp + 2) / 5 + 1;
    unsigned mm = mp < 10 ? mp + 3 : mp - 9;
    return Date{int(yy + (mm <= 2)), int(mm), int(dd)};
}

int Date::weekday() const {
    long z = days();  // 1970-01-01 was a Thursday (3)
    return int(((z % 7) + 7 + 3) % 7);
}

std::string Date::str() const {
    char b[16];
    std::snprintf(b, sizeof b, "%04d-%02d-%02d", y, m, d);
    return b;
}

std::optional<Date> Date::parse(const std::string& s) {
    int y, m, d;
    if (std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) return std::nullopt;
    return Date{y, m, d};
}

Date Date::today() {
    std::time_t t = std::time(nullptr);
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    return Date{lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday};
}

namespace dates {

std::optional<std::time_t> parseRfc3339(const std::string& s) {
    int y, mo, d, h = 0, mi = 0;
    double sec = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%lf", &y, &mo, &d, &h, &mi, &sec) < 3) return std::nullopt;
    long days = Date{y, mo, d}.days();
    long long t = days * 86400LL + h * 3600 + mi * 60 + long(sec);
    // offset: Z, +hh:mm, -hh:mm after the time part
    size_t tpos = s.find('T');
    if (tpos != std::string::npos) {
        size_t p = s.find_first_of("Z+-", tpos + 1);
        if (p != std::string::npos && s[p] != 'Z') {
            int oh = 0, om = 0;
            std::sscanf(s.c_str() + p + 1, "%d:%d", &oh, &om);
            long off = oh * 3600 + om * 60;
            t -= s[p] == '+' ? off : -off;
        }
    }
    return std::time_t(t);
}

std::optional<Date> localDate(const std::string& s) {
    auto t = parseRfc3339(s);
    if (!t) return std::nullopt;
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &*t);
#else
    localtime_r(&*t, &lt);
#endif
    return Date{lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday};
}

int utcOffsetSeconds(std::time_t t) {
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
    std::tm gm{};
    gmtime_s(&gm, &t);
    return int(std::difftime(std::mktime(&lt), std::mktime(&gm)));
#else
    localtime_r(&t, &lt);
    return int(lt.tm_gmtoff);
#endif
}

std::string toRfc3339(std::time_t t) {
    std::tm lt{};
#ifdef _WIN32
    localtime_s(&lt, &t);
#else
    localtime_r(&t, &lt);
#endif
    int off = utcOffsetSeconds(t);
    char b[48];
    std::snprintf(b, sizeof b, "%04d-%02d-%02dT%02d:%02d:%02d%c%02d:%02d", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, lt.tm_hour, lt.tm_min,
                  lt.tm_sec, off >= 0 ? '+' : '-', std::abs(off) / 3600, (std::abs(off) / 60) % 60);
    return b;
}

std::string nowRfc3339() { return toRfc3339(std::time(nullptr)); }

std::string newId() {
    static std::mt19937_64 rng(std::random_device{}() ^ uint64_t(std::chrono::steady_clock::now().time_since_epoch().count()));
    uint64_t a = rng(), b = rng();
    a = (a & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    b = (b & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;
    char s[40];
    std::snprintf(s, sizeof s, "%08x-%04x-%04x-%04x-%012llx", unsigned(a >> 32), unsigned((a >> 16) & 0xFFFF), unsigned(a & 0xFFFF),
                  unsigned(b >> 48), (unsigned long long)(b & 0xFFFFFFFFFFFFULL));
    return s;
}

const char* monthName(int m) {
    static const char* n[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};
    return n[(m - 1 + 12) % 12];
}
const char* weekdayShort(int wd) {
    static const char* n[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    return n[(wd % 7 + 7) % 7];
}

}  // namespace dates
