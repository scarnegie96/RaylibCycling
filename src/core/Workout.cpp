#include "Workout.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "Metrics.h"

namespace wk {

// ------------------------------------------------------------------ blocks
Block Block::steady(int d, double p, std::optional<int> cad, std::string label) {
    Block b; b.type = Steady; b.durationS = d; b.power = p; b.cadence = cad; b.label = std::move(label); return b;
}
Block Block::ramp(int d, double a, double c, std::string label) {
    Block b; b.type = Ramp; b.durationS = d; b.from = a; b.to = c; b.label = std::move(label); return b;
}
Block Block::intervals(int n, int on, double onp, int off, double offp, std::optional<int> cad) {
    Block b; b.type = Intervals; b.repeats = n; b.onS = on; b.onPower = onp; b.offS = off; b.offPower = offp; b.cadence = cad; return b;
}
Block Block::free(int d, double grade, bool effort, std::string label) {
    Block b; b.type = Free; b.durationS = d; b.gradePct = grade; b.effort = effort; b.label = std::move(label); return b;
}
Block Block::set(int n, std::vector<Block> v) {
    Block b; b.type = Set; b.repeats = n; b.blocks = std::move(v); return b;
}

std::optional<double> Step::fractionAt(double t) const {
    if (!erg) return std::nullopt;
    double f = durationS > 0 ? std::clamp(t / durationS, 0.0, 1.0) : 0.0;
    return from + (to - from) * f;
}

static const char* zoneLabel(double f) {
    if (f <= 0.55) return "Recovery";
    if (f <= 0.75) return "Endurance";
    if (f <= 0.90) return "Tempo";
    if (f <= 1.05) return "Threshold";
    if (f <= 1.20) return "VO2 Max";
    return "Anaerobic";
}

static void pushStep(std::vector<Step>& out, int& t, const std::string& prefix, int dur, bool erg, double from, double to,
                     double grade, std::string label, bool effort, std::optional<int> cad) {
    if (dur <= 0) return;
    Step s;
    s.startS = t; s.durationS = dur; s.erg = erg; s.from = from; s.to = to; s.gradePct = grade;
    s.label = prefix.empty() ? label : prefix + " \xC2\xB7 " + label;
    s.effort = effort; s.cadence = cad;
    out.push_back(s);
    t += dur;
}

static void expand(const std::vector<Block>& blocks, const std::string& prefix, std::vector<Step>& out, int& t) {
    for (auto& b : blocks) {
        switch (b.type) {
            case Block::Steady:
                pushStep(out, t, prefix, b.durationS, true, b.power, b.power, 0, b.label.empty() ? zoneLabel(b.power) : b.label,
                         b.power > 1.05, b.cadence);
                break;
            case Block::Ramp:
                pushStep(out, t, prefix, b.durationS, true, b.from, b.to, 0,
                         b.label.empty() ? (b.to >= b.from ? "Ramp up" : "Ramp down") : b.label, false, {});
                break;
            case Block::Intervals:
                for (int i = 0; i < b.repeats; ++i) {
                    pushStep(out, t, prefix, b.onS, true, b.onPower, b.onPower, 0,
                             "Interval " + std::to_string(i + 1) + "/" + std::to_string(b.repeats), true, b.cadence);
                    pushStep(out, t, prefix, b.offS, true, b.offPower, b.offPower, 0, "Recover", false, {});
                }
                break;
            case Block::Free:
                pushStep(out, t, prefix, b.durationS, false, 0, 0, b.gradePct,
                         b.label.empty() ? (b.effort ? "All-out effort" : "Free ride") : b.label, b.effort, {});
                break;
            case Block::Set:
                for (int k = 0; k < b.repeats; ++k) {
                    std::string s = "Set " + std::to_string(k + 1) + "/" + std::to_string(b.repeats);
                    expand(b.blocks, prefix.empty() ? s : prefix + " \xC2\xB7 " + s, out, t);
                }
                break;
        }
    }
}

std::vector<Step> blocksToSteps(const std::vector<Block>& blocks) {
    std::vector<Step> out;
    int t = 0;
    expand(blocks, "", out, t);
    return out;
}

void reflow(std::vector<Step>& steps, size_t from) {
    for (size_t i = std::max<size_t>(from, 1); i < steps.size(); ++i) steps[i].startS = steps[i - 1].endS();
}

Estimate estimateSteps(const std::vector<Step>& steps, double ftp) {
    std::vector<double> p;
    for (auto& s : steps)
        for (int k = 0; k < s.durationS; ++k) p.push_back(s.fractionAt(k + 0.5).value_or(s.effort ? 0.95 : 0.6) * ftp);
    Estimate e;
    e.np = metrics::normalizedPower(p);
    e.durationS = int(p.size());
    e.avgPower = metrics::mean(p);
    e.intensityFactor = metrics::intensityFactor(e.np, ftp);
    e.tss = metrics::tss(double(p.size()), e.np, ftp);
    double sum = 0;
    for (double v : p) sum += v;
    e.workKj = sum / 1000.0;
    e.zoneS = metrics::timeInZones(p, ftp);
    return e;
}

std::vector<Step> Workout::steps() const { return blocksToSteps(blocks); }
int Workout::durationS() const { auto s = steps(); return s.empty() ? 0 : s.back().endS(); }
Estimate Workout::estimate(double ftp) const { return estimateSteps(steps(), ftp); }

std::optional<std::pair<size_t, double>> stepAt(const std::vector<Step>& steps, double t) {
    if (t < 0) return std::nullopt;
    auto it = std::partition_point(steps.begin(), steps.end(), [&](const Step& s) { return double(s.endS()) <= t; });
    if (it == steps.end()) return std::nullopt;
    return std::make_pair(size_t(it - steps.begin()), t - it->startS);
}

// ------------------------------------------------------------------ text format
static std::u32string toU32(const std::string& s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        char32_t cp;
        int n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; n = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; n = 3; }
        else { cp = c & 0x07; n = 4; }
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (s[i + k] & 0x3F);
        out.push_back(cp);
        i += n;
    }
    return out;
}
static std::string toU8(const std::u32string& s) {
    std::string out;
    for (char32_t c : s) {
        if (c < 0x80) out.push_back(char(c));
        else if (c < 0x800) { out.push_back(char(0xC0 | (c >> 6))); out.push_back(char(0x80 | (c & 0x3F))); }
        else { out.push_back(char(0xE0 | (c >> 12))); out.push_back(char(0x80 | ((c >> 6) & 0x3F))); out.push_back(char(0x80 | (c & 0x3F))); }
    }
    return out;
}

namespace {
struct Spec {
    bool power = true;
    double from = 0, to = 0;
    std::optional<int> cadence;
    bool effort = false;
    double grade = 0;
};

struct ParseError { std::string msg; };

struct Parser {
    std::u32string s;
    size_t i = 0;
    double ftp;

    char32_t peek(size_t k = 0) const { return i + k < s.size() ? s[i + k] : 0; }
    void ws() { while (peek() == ' ' || peek() == '\t') ++i; }
    void wsAll() { while (peek() && (peek() == ' ' || peek() == '\t' || peek() == '\n' || peek() == '\r')) ++i; }
    bool sep() {
        bool any = false;
        for (;;) {
            ws();
            char32_t c = peek();
            if (c == ',' || c == ';' || c == '\n' || c == '\r') { ++i; any = true; } else return any;
        }
    }
    [[noreturn]] void fail(const std::string& msg) {
        std::u32string ctx = s.substr(i, 18);
        std::string c = toU8(ctx);
        while (!c.empty() && std::isspace((unsigned char)c.back())) c.pop_back();
        while (!c.empty() && std::isspace((unsigned char)c.front())) c.erase(c.begin());
        throw ParseError{c.empty() ? msg + " (at end)" : msg + " near \xE2\x80\x9C" + c + "\xE2\x80\x9D"};
    }
    std::string word() {
        size_t st = i;
        while (peek() && ((peek() >= 'a' && peek() <= 'z') || (peek() >= 'A' && peek() <= 'Z'))) ++i;
        std::string w = toU8(s.substr(st, i - st));
        for (auto& ch : w) ch = char(std::tolower((unsigned char)ch));
        return w;
    }
    std::optional<double> number() {
        size_t st = i;
        while (peek() && ((peek() >= '0' && peek() <= '9') || peek() == '.')) ++i;
        if (st == i) return std::nullopt;
        try { return std::stod(toU8(s.substr(st, i - st))); } catch (...) { return std::nullopt; }
    }

    std::vector<Block> plan(bool nested) {
        std::vector<Block> out;
        for (;;) {
            wsAll();
            if (!peek() || (nested && peek() == ')')) break;
            auto v = item();
            out.insert(out.end(), v.begin(), v.end());
            if (!sep()) {
                wsAll();
                if (!peek() || (nested && peek() == ')')) break;
                fail("Expected a comma or new line");
            }
        }
        return out;
    }
    std::optional<int> tryRepeat() {
        size_t save = i;
        if (auto n = number()) {
            ws();
            if (peek() == 'x' || peek() == 'X' || peek() == U'×') {
                ++i;
                if (*n >= 1 && std::floor(*n) == *n) return int(*n);
            }
        }
        i = save;
        return std::nullopt;
    }
    static Block toBlock(int d, const Spec& s) {
        if (!s.power) return Block::free(d, s.grade, s.effort);
        if (std::fabs(s.from - s.to) < 1e-9) return Block::steady(d, s.from, s.cadence);
        return Block::ramp(d, s.from, s.to);
    }
    std::vector<Block> item() {
        if (auto n = tryRepeat()) {
            int reps = std::min(*n, 100);
            ws();
            if (peek() == '(') {
                ++i;
                auto inner = plan(true);
                wsAll();
                if (peek() != ')') fail("Missing ')'");
                ++i;
                if (inner.empty()) throw ParseError{"Empty repeat group"};
                return {Block::set(reps, inner)};
            }
            auto [d1, s1] = segment();
            ws();
            if (peek() == '/') {
                ++i;
                ws();
                auto [d2, s2] = segment();
                if (s1.power && s2.power && std::fabs(s1.from - s1.to) < 1e-9 && std::fabs(s2.from - s2.to) < 1e-9)
                    return {Block::intervals(reps, d1, s1.from, d2, s2.from, s1.cadence)};
                return {Block::set(reps, {toBlock(d1, s1), toBlock(d2, s2)})};
            }
            return {Block::set(reps, {toBlock(d1, s1)})};
        }
        auto [d, sp] = segment();
        return {toBlock(d, sp)};
    }
    std::pair<int, Spec> segment() {
        int d = duration();
        ws();
        return {d, spec()};
    }
    int duration() {
        auto a = number();
        if (!a) fail("Expected a duration like 30s, 5m or 1:30");
        if (peek() == ':') {
            ++i;
            auto b = number();
            if (!b) fail("Expected seconds after ':'");
            if (peek() == ':') {
                ++i;
                auto c = number();
                if (!c) fail("Expected seconds");
                return int(std::lround(*a * 3600 + *b * 60 + *c));
            }
            return int(std::lround(*a * 60 + *b));
        }
        ws();
        std::string u = word();
        double secs;
        if (u == "s" || u == "sec" || u == "secs" || u == "second" || u == "seconds") secs = *a;
        else if (u == "m" || u == "min" || u == "mins" || u == "minute" || u == "minutes") secs = *a * 60;
        else if (u == "h" || u == "hr" || u == "hrs" || u == "hour" || u == "hours") secs = *a * 3600;
        else fail("Add a unit to the duration (s, m or h)");
        if (secs < 1) throw ParseError{"Durations must be at least 1 second"};
        return int(std::lround(secs));
    }
    double power() {
        auto n = number();
        if (!n) fail("Expected a power like 95% or 250w");
        ws();
        if (peek() == '%') { ++i; return *n / 100.0; }
        if (peek() == 'w' || peek() == 'W') { ++i; return *n / std::max(ftp, 50.0); }
        return *n <= 3.0 ? *n : *n / 100.0;
    }
    Spec spec() {
        if (peek() == '@') { ++i; ws(); }
        size_t save = i;
        std::string w = word();
        if (w == "free" || w == "freeride" || w == "effort" || w == "max" || w == "allout") {
            Spec s;
            s.power = false;
            s.effort = !(w == "free" || w == "freeride");
            ws();
            s.grade = s.effort ? 1.0 : 0.0;
            if (peek() == '@') {
                ++i;
                ws();
                auto g = number();
                if (!g) fail("Expected a gradient");
                s.grade = *g;
                if (peek() == '%') ++i;
            }
            return s;
        }
        if (w == "at") ws();
        else if (!w.empty()) { i = save; fail("Expected a power (e.g. 90%), 'free' or 'effort'"); }
        Spec s;
        s.from = power();
        ws();
        s.to = s.from;
        size_t save2 = i;
        bool ramp = false;
        if (peek() == '-' || peek() == U'→') { ++i; ramp = true; }
        else if (peek() == '.' && peek(1) == '.') { i += 2; ramp = true; }
        else if (peek() == 't') { if (word() == "to") ramp = true; else i = save2; }
        if (ramp) { ws(); s.to = power(); }
        ws();
        size_t save3 = i;
        if (auto c = number()) {
            ws();
            if (word() == "rpm") s.cadence = int(std::lround(*c)); else i = save3;
        }
        if (s.from < 0 || s.from > 4 || s.to < 0 || s.to > 4) throw ParseError{"Power must be between 0 and 400% FTP"};
        return s;
    }
};
}  // namespace

bool parseText(const std::string& src, double ftp, std::vector<Block>& out, std::string& err) {
    Parser p{toU32(src), 0, ftp};
    try {
        auto b = p.plan(false);
        p.wsAll();
        if (p.i < p.s.size()) p.fail("Unexpected text");
        if (b.empty()) { err = "Nothing to add"; return false; }
        out = std::move(b);
        return true;
    } catch (ParseError& e) {
        err = e.msg;
        return false;
    }
}

static std::string fmtDur(int s) {
    char b[32];
    if (s % 3600 == 0 && s >= 3600) std::snprintf(b, sizeof b, "%dh", s / 3600);
    else if (s % 60 == 0) std::snprintf(b, sizeof b, "%dm", s / 60);
    else if (s > 60) std::snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
    else std::snprintf(b, sizeof b, "%ds", s);
    return b;
}
static std::string pct(double f) { return std::to_string(int(std::lround(f * 100))) + "%"; }
static std::string g(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%g", v);
    return b;
}

std::string toText(const std::vector<Block>& blocks) {
    std::string out;
    for (size_t k = 0; k < blocks.size(); ++k) {
        const Block& b = blocks[k];
        std::string line;
        switch (b.type) {
            case Block::Steady:
                line = fmtDur(b.durationS) + " @" + pct(b.power) + (b.cadence ? " " + std::to_string(*b.cadence) + "rpm" : "");
                break;
            case Block::Ramp: line = fmtDur(b.durationS) + " " + pct(b.from) + "-" + pct(b.to); break;
            case Block::Intervals:
                line = std::to_string(b.repeats) + "x " + fmtDur(b.onS) + " @" + pct(b.onPower) +
                       (b.cadence ? " " + std::to_string(*b.cadence) + "rpm" : "") + " / " + fmtDur(std::max(1, b.offS)) + " @" +
                       pct(b.offPower);
                break;
            case Block::Free: line = fmtDur(b.durationS) + (b.effort ? " effort @" : " free @") + g(b.gradePct) + "%"; break;
            case Block::Set: {
                std::string inner = toText(b.blocks);
                for (size_t p = 0; (p = inner.find('\n', p)) != std::string::npos;) inner.replace(p, 1, ", ");
                line = std::to_string(b.repeats) + "x (" + inner + ")";
                break;
            }
        }
        if (k) out += "\n";
        out += line;
    }
    return out;
}

// ------------------------------------------------------------------ ZWO / ERG / MRC
namespace {
struct Tag { std::string name; std::vector<std::pair<std::string, std::string>> attrs; size_t end = 0; bool closing = false; };

std::string lower(std::string s) { for (auto& c : s) c = char(std::tolower((unsigned char)c)); return s; }

bool nextTag(const std::string& x, size_t from, Tag& t, size_t& tagStart) {
    size_t lt = x.find('<', from);
    while (lt != std::string::npos && (x.compare(lt, 4, "<!--") == 0 || x.compare(lt, 2, "<?") == 0)) {
        size_t e = x.compare(lt, 4, "<!--") == 0 ? x.find("-->", lt) : x.find("?>", lt);
        if (e == std::string::npos) return false;
        lt = x.find('<', e);
    }
    if (lt == std::string::npos) return false;
    size_t gt = x.find('>', lt);
    if (gt == std::string::npos) return false;
    tagStart = lt;
    std::string body = x.substr(lt + 1, gt - lt - 1);
    t = Tag{};
    t.end = gt + 1;
    size_t p = 0;
    if (!body.empty() && body[0] == '/') { t.closing = true; p = 1; }
    size_t ns = p;
    while (p < body.size() && !std::isspace((unsigned char)body[p]) && body[p] != '/') ++p;
    t.name = lower(body.substr(ns, p - ns));
    while (p < body.size()) {
        while (p < body.size() && (std::isspace((unsigned char)body[p]) || body[p] == '/')) ++p;
        size_t ks = p;
        while (p < body.size() && body[p] != '=' && !std::isspace((unsigned char)body[p])) ++p;
        std::string key = body.substr(ks, p - ks);
        while (p < body.size() && body[p] != '=') { if (!std::isspace((unsigned char)body[p])) break; ++p; }
        if (p >= body.size() || body[p] != '=') { if (key.empty()) break; continue; }
        ++p;
        while (p < body.size() && std::isspace((unsigned char)body[p])) ++p;
        if (p >= body.size()) break;
        char q = body[p];
        std::string val;
        if (q == '"' || q == '\'') {
            size_t e = body.find(q, p + 1);
            if (e == std::string::npos) e = body.size();
            val = body.substr(p + 1, e - p - 1);
            p = e + 1;
        } else {
            size_t vs = p;
            while (p < body.size() && !std::isspace((unsigned char)body[p])) ++p;
            val = body.substr(vs, p - vs);
        }
        if (!key.empty()) t.attrs.push_back({lower(key), val});
    }
    return true;
}

std::optional<double> attr(const Tag& t, std::initializer_list<const char*> names) {
    for (const char* n : names)
        for (auto& [k, v] : t.attrs)
            if (k == lower(n)) { try { return std::stod(v); } catch (...) {} }
    return std::nullopt;
}

std::string childText(const std::string& x, const std::string& tag) {
    std::string lx = lower(x);
    size_t a = lx.find("<" + tag + ">");
    if (a == std::string::npos) return "";
    size_t b = lx.find("</" + tag + ">", a);
    if (b == std::string::npos) return "";
    std::string s = x.substr(a + tag.size() + 2, b - a - tag.size() - 2);
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    return s;
}

std::string xmlEsc(const std::string& s) {
    std::string o;
    for (char c : s) {
        if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '"') o += "&quot;"; else o += c;
    }
    return o;
}
}  // namespace

bool fromZwo(const std::string& text, Workout& out, std::string& err) {
    std::string lx = lower(text);
    size_t ws = lx.find("<workout>");
    if (ws == std::string::npos) { err = "No <workout> section found"; return false; }
    size_t we = lx.find("</workout>", ws);
    if (we == std::string::npos) we = text.size();
    std::vector<Block> blocks;
    size_t pos = ws + 9, st;
    Tag t;
    while (pos < we && nextTag(text, pos, t, st) && st < we) {
        pos = t.end;
        if (t.closing) continue;
        int dur = int(std::lround(attr(t, {"Duration"}).value_or(0)));
        std::optional<int> cad;
        if (auto c = attr(t, {"Cadence"})) cad = int(std::lround(*c));
        if (t.name == "warmup" || t.name == "cooldown" || t.name == "ramp") {
            double lo = attr(t, {"PowerLow"}).value_or(0.5), hi = attr(t, {"PowerHigh"}).value_or(lo);
            blocks.push_back(Block::ramp(dur, lo, hi, t.name == "warmup" ? "Warm-up" : t.name == "cooldown" ? "Cool-down" : ""));
        } else if (t.name == "steadystate" || t.name == "solidstate") {
            double p = 0.6;
            if (auto v = attr(t, {"Power"})) p = *v;
            else if (auto lo = attr(t, {"PowerLow"}), hi = attr(t, {"PowerHigh"}); lo && hi) p = (*lo + *hi) / 2;
            blocks.push_back(Block::steady(dur, p, cad));
        } else if (t.name == "intervalst") {
            blocks.push_back(Block::intervals(std::max(1, int(std::lround(attr(t, {"Repeat"}).value_or(1)))),
                                              int(std::lround(attr(t, {"OnDuration"}).value_or(60))),
                                              attr(t, {"OnPower", "PowerOnHigh", "PowerOnLow"}).value_or(1.0),
                                              int(std::lround(attr(t, {"OffDuration"}).value_or(60))),
                                              attr(t, {"OffPower", "PowerOffLow", "PowerOffHigh"}).value_or(0.5), cad));
        } else if (t.name == "freeride") {
            blocks.push_back(Block::free(dur, 0, false));
        } else if (t.name == "maxeffort") {
            blocks.push_back(Block::free(dur, 1, true, "Max effort"));
        }
    }
    if (blocks.empty()) { err = "The workout has no supported blocks"; return false; }
    out = Workout{};
    out.name = childText(text, "name");
    if (out.name.empty()) out.name = "Imported workout";
    out.description = childText(text, "description");
    out.category = childText(text, "category");
    if (out.category.empty()) out.category = "Imported";
    out.blocks = std::move(blocks);
    return true;
}

static void zwoBlocks(const std::vector<Block>& blocks, std::string& out) {
    char b[256];
    for (auto& x : blocks) {
        switch (x.type) {
            case Block::Steady:
                std::snprintf(b, sizeof b, "    <SteadyState Duration=\"%d\" Power=\"%.3f\"%s/>\n", x.durationS, x.power,
                              x.cadence ? (" Cadence=\"" + std::to_string(*x.cadence) + "\"").c_str() : "");
                out += b;
                break;
            case Block::Ramp:
                std::snprintf(b, sizeof b, "    <Ramp Duration=\"%d\" PowerLow=\"%.3f\" PowerHigh=\"%.3f\"/>\n", x.durationS, x.from, x.to);
                out += b;
                break;
            case Block::Intervals:
                std::snprintf(b, sizeof b,
                              "    <IntervalsT Repeat=\"%d\" OnDuration=\"%d\" OffDuration=\"%d\" OnPower=\"%.3f\" OffPower=\"%.3f\"%s/>\n",
                              x.repeats, x.onS, x.offS, x.onPower, x.offPower,
                              x.cadence ? (" Cadence=\"" + std::to_string(*x.cadence) + "\"").c_str() : "");
                out += b;
                break;
            case Block::Free:
                std::snprintf(b, sizeof b, x.effort ? "    <MaxEffort Duration=\"%d\"/>\n" : "    <FreeRide Duration=\"%d\" FlatRoad=\"1\"/>\n",
                              x.durationS);
                out += b;
                break;
            case Block::Set:
                for (int k = 0; k < x.repeats; ++k) zwoBlocks(x.blocks, out);
                break;
        }
    }
}

std::string toZwo(const Workout& w) {
    std::string out = "<workout_file>\n  <author>CoreRay</author>\n";
    out += "  <name>" + xmlEsc(w.name) + "</name>\n";
    out += "  <description>" + xmlEsc(w.description) + "</description>\n";
    out += "  <sportType>bike</sportType>\n";
    out += "  <category>" + xmlEsc(w.category) + "</category>\n  <workout>\n";
    zwoBlocks(w.blocks, out);
    out += "  </workout>\n</workout_file>\n";
    return out;
}

bool fromErgMrc(const std::string& text, double ftp, Workout& out, std::string& err) {
    std::string name, desc;
    std::optional<double> fileFtp;
    bool percent = false, inData = false;
    std::vector<std::pair<double, double>> pts;
    std::istringstream in(text);
    std::string raw;
    while (std::getline(in, raw)) {
        std::string line = raw;
        while (!line.empty() && std::isspace((unsigned char)line.back())) line.pop_back();
        while (!line.empty() && std::isspace((unsigned char)line.front())) line.erase(line.begin());
        if (line.empty() || line[0] == ';') continue;
        std::string up = line;
        for (auto& c : up) c = char(std::toupper((unsigned char)c));
        if (up.rfind("[COURSE DATA]", 0) == 0) { inData = true; continue; }
        if (up.rfind("[END COURSE DATA]", 0) == 0) { inData = false; continue; }
        if (up[0] == '[') continue;
        if (!inData) {
            auto eq = line.find('=');
            if (eq != std::string::npos) {
                std::string k = up.substr(0, eq), v = line.substr(eq + 1);
                while (!k.empty() && std::isspace((unsigned char)k.back())) k.pop_back();
                while (!v.empty() && std::isspace((unsigned char)v.front())) v.erase(v.begin());
                if (k == "FILE NAME") name = v;
                else if (k == "DESCRIPTION") desc = v;
                else if (k == "FTP") { try { fileFtp = std::stod(v); } catch (...) {} }
            } else if (up.find("PERCENT") != std::string::npos) percent = true;
            continue;
        }
        std::istringstream ls(line);
        double t, v;
        if (ls >> t >> v) pts.push_back({t * 60, v});
    }
    if (pts.size() < 2) { err = "No [COURSE DATA] found - is this an .erg or .mrc file?"; return false; }
    double base = std::max(fileFtp.value_or(ftp), 50.0);
    auto frac = [&](double v) { return percent ? v / 100.0 : v / base; };
    std::vector<Block> merged;
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        double d = std::round(pts[i + 1].first - pts[i].first);
        if (d < 1) continue;
        double a = frac(pts[i].second), b = frac(pts[i + 1].second);
        if (std::fabs(a - b) < 1e-6) {
            if (!merged.empty() && merged.back().type == Block::Steady && std::fabs(merged.back().power - a) < 1e-6)
                merged.back().durationS += int(d);
            else merged.push_back(Block::steady(int(d), a));
        } else merged.push_back(Block::ramp(int(d), a, b));
    }
    out = Workout{};
    out.name = name.empty() ? "Imported workout" : name;
    out.description = desc;
    out.category = "Imported";
    out.blocks = std::move(merged);
    return true;
}

bool importFile(const std::string& fileName, const std::string& text, double ftp, Workout& out, std::string& err) {
    std::string l = lower(fileName);
    auto ends = [&](const char* e) { std::string s = e; return l.size() >= s.size() && l.compare(l.size() - s.size(), s.size(), s) == 0; };
    size_t first = text.find_first_not_of(" \t\r\n");
    std::string up = text;
    for (auto& c : up) c = char(std::toupper((unsigned char)c));
    if (ends(".zwo") || (first != std::string::npos && text[first] == '<')) return fromZwo(text, out, err);
    if (ends(".erg") || ends(".mrc") || up.find("[COURSE DATA]") != std::string::npos) return fromErgMrc(text, ftp, out, err);
    std::vector<Block> b;
    if (!parseText(text, ftp, b, err)) return false;
    out = Workout{};
    std::string base = fileName.substr(fileName.find_last_of("/\\") == std::string::npos ? 0 : fileName.find_last_of("/\\") + 1);
    if (base.size() > 4 && lower(base.substr(base.size() - 4)) == ".txt") base.resize(base.size() - 4);
    out.name = base.empty() ? "Imported" : base;
    out.category = "Imported";
    out.blocks = std::move(b);
    return true;
}

// ------------------------------------------------------------------ built-in library
static Workout W(const char* id, const char* name, const char* cat, const char* desc, std::vector<Block> b) {
    Workout w;
    w.id = std::string("builtin-") + id; w.name = name; w.category = cat; w.description = desc; w.blocks = std::move(b); w.builtin = true;
    return w;
}
static Block steady(int d, double p) { return Block::steady(d, p); }
static Block steadyC(int d, double p, int c, const char* l) { return Block::steady(d, p, c, l); }
static Block ramp(int d, double a, double b) { return Block::ramp(d, a, b); }
static Block reps(int n, int on, double onp, int off, double offp) { return Block::intervals(n, on, onp, off, offp); }
static Block set(int n, std::vector<Block> b) { return Block::set(n, std::move(b)); }
static Block warmup() { return Block::ramp(600, 0.45, 0.75, "Warm-up"); }
static Block cooldown() { return Block::ramp(300, 0.6, 0.4, "Cool-down"); }

std::vector<std::string> categories() {
    return {"Recovery", "Endurance", "Tempo", "Sweet Spot", "Threshold", "VO2 Max", "Anaerobic", "Mixed"};
}

std::vector<Workout> builtinWorkouts() {
    return {
        W("recovery-spin", "Recovery Spin", "Recovery", "Easy, high-cadence spin to flush the legs.",
          {ramp(300, 0.4, 0.5), Block::steady(1500, 0.5, 95, "Easy spin"), ramp(300, 0.5, 0.4)}),
        W("recovery-45", "Active Recovery 45", "Recovery",
          "45 minutes barely above coffee-ride pace, with a few cadence lifts to keep the legs supple.",
          {ramp(300, 0.4, 0.5), set(4, {steady(450, 0.5), steadyC(90, 0.52, 105, "Spin-up")}), ramp(240, 0.5, 0.4)}),
        W("endurance-60", "Endurance 60", "Endurance", "An hour of steady Zone 2 aerobic work.", {warmup(), steady(2700, 0.68), cooldown()}),
        W("endurance-90", "Endurance 90", "Endurance", "90 minutes of Zone 2 with gentle variation to stay engaged.",
          {warmup(), set(4, {steady(780, 0.68), steady(240, 0.74)}), steady(420, 0.65), cooldown()}),
        W("long-ride-2h", "Long Ride 2h", "Endurance", "Two hours of aerobic base. Fuel it like an outdoor ride.",
          {warmup(), set(3, {steady(1600, 0.68), steady(300, 0.75)}), steady(600, 0.65), cooldown()}),
        W("endurance-bursts", "Endurance + Bursts", "Endurance",
          "Zone 2 hour with a 10-second burst every 5 minutes to recruit fast-twitch fibres.",
          {warmup(), set(9, {steady(290, 0.68), Block::steady(10, 1.6, 100, "Burst")}), cooldown()}),
        W("cadence-pyramid", "Cadence Pyramid", "Endurance", "Zone 2 power, climbing cadence targets 85 -> 120 rpm and back. Smoothness drill.",
          {warmup(),
           set(2, {steadyC(180, 0.65, 85, "85 rpm"), steadyC(180, 0.65, 95, "95 rpm"), steadyC(120, 0.65, 105, "105 rpm"),
                   steadyC(60, 0.65, 115, "115 rpm"), steadyC(60, 0.62, 120, "120 rpm"), steadyC(120, 0.65, 105, "105 rpm"),
                   steadyC(180, 0.65, 90, "90 rpm")}),
           cooldown()}),
        W("tempo-3x12", "Tempo 3x12", "Tempo", "Three 12-minute tempo blocks to build muscular endurance.",
          {warmup(), reps(3, 720, 0.85, 240, 0.55), cooldown()}),
        W("tempo-climb", "Low-Cadence Tempo", "Tempo", "Two 20-minute tempo efforts at 65-70 rpm - strength-endurance for long climbs.",
          {warmup(), steadyC(1200, 0.83, 68, "Big-gear tempo"), steady(300, 0.55), steadyC(1200, 0.83, 68, "Big-gear tempo"), cooldown()}),
        W("tempo-surges", "Tempo with Surges", "Tempo", "Tempo blocks broken by 30 s surges - like holding a wheel in a group.",
          {warmup(), set(3, {set(4, {steady(150, 0.82), Block::steady(30, 1.2, {}, "Surge")}), steady(240, 0.55)}), cooldown()}),
        W("sweet-spot-2x20", "Sweet Spot 2x20", "Sweet Spot", "The classic: two 20-minute blocks at 88-93 % FTP.",
          {warmup(), reps(2, 1200, 0.9, 300, 0.5), cooldown()}),
        W("sweet-spot-3x15", "Sweet Spot 3x15", "Sweet Spot", "Three 15-minute sweet-spot intervals with 5-minute recoveries.",
          {warmup(), reps(3, 900, 0.9, 300, 0.5), cooldown()}),
        W("sweet-spot-4x12", "Sweet Spot 4x12", "Sweet Spot", "Four 12-minute blocks, each one a touch harder: 88 -> 94 %.",
          {warmup(), steady(720, 0.88), steady(240, 0.5), steady(720, 0.9), steady(240, 0.5), steady(720, 0.92), steady(240, 0.5),
           steady(720, 0.94), cooldown()}),
        W("hour-of-power", "Hour of Power", "Sweet Spot", "A continuous 40 minutes at 85-90 % FTP. Mental toughness included.",
          {warmup(), ramp(2400, 0.85, 0.9), cooldown()}),
        W("threshold-3x10", "Threshold 3x10", "Threshold", "Three 10-minute efforts right at FTP.",
          {warmup(), reps(3, 600, 1.0, 300, 0.5), cooldown()}),
        W("threshold-2x20", "Threshold 2x20", "Threshold", "Two 20-minute efforts at 95-100 % FTP. The benchmark FTP builder.",
          {warmup(), reps(2, 1200, 0.97, 360, 0.5), cooldown()}),
        W("over-unders", "Over-Unders", "Threshold", "Alternate 2 min at 95 % and 1 min at 110 % to teach lactate clearance.",
          {warmup(), Block::intervals(3, 120, 0.95, 60, 1.1), steady(300, 0.5), Block::intervals(3, 120, 0.95, 60, 1.1), cooldown()}),
        W("under-over-sets", "Under-Over Sets", "Threshold", "3 sets of (4 x 2 min @ 95 % / 1 min @ 105 %) with 5-minute recoveries.",
          {warmup(), set(3, {set(4, {steady(120, 0.95), steady(60, 1.05)}), steady(300, 0.5)}), cooldown()}),
        W("threshold-ladder", "Threshold Ladder", "Threshold", "4-6-8-6-4 minutes at threshold with short recoveries.",
          {warmup(), steady(240, 1.0), steady(120, 0.5), steady(360, 1.0), steady(180, 0.5), steady(480, 0.98), steady(240, 0.5),
           steady(360, 1.0), steady(180, 0.5), steady(240, 1.02), cooldown()}),
        W("vo2-5x3", "VO2 Max 5x3", "VO2 Max", "Five 3-minute efforts at 120 % FTP with equal recovery.",
          {warmup(), steady(180, 0.95), steady(180, 0.5), reps(5, 180, 1.2, 180, 0.5), cooldown()}),
        W("norwegian-4x4", "Norwegian 4x4", "VO2 Max", "4 x 4 minutes at 112-115 % with 3-minute recoveries. Simple, brutal, effective.",
          {warmup(), reps(4, 240, 1.13, 180, 0.55), cooldown()}),
        W("micro-30-30", "30/30s", "VO2 Max", "Two sets of 10 x 30 s on / 30 s off at 130 %.",
          {warmup(), reps(10, 30, 1.3, 30, 0.5), steady(300, 0.5), reps(10, 30, 1.3, 30, 0.5), cooldown()}),
        W("ronnestad-30-15", "R\xC3\xB8nnestad 30/15s", "VO2 Max", "3 sets of 13 x 30 s @ 120 % / 15 s @ 50 % - the research-backed VO2 session.",
          {warmup(), set(3, {reps(13, 30, 1.2, 15, 0.5), steady(180, 0.5)}), cooldown()}),
        W("40-20s", "40/20s", "VO2 Max", "3 sets of 8 x 40 s hard / 20 s easy.",
          {warmup(), set(3, {reps(8, 40, 1.2, 20, 0.5), steady(300, 0.5)}), cooldown()}),
        W("tabata", "Tabata Blast", "Anaerobic", "8 x 20 s all-out (170 %) / 10 s rest. Short and brutal.",
          {warmup(), steady(120, 0.5), reps(8, 20, 1.7, 10, 0.4), steady(300, 0.5), cooldown()}),
        W("anaerobic-6x1", "Anaerobic 6x1", "Anaerobic", "Six 1-minute efforts at 140 % with full recovery.",
          {warmup(), reps(6, 60, 1.4, 240, 0.5), cooldown()}),
        W("sprint-primer", "Sprint Primer", "Anaerobic", "Six seated/standing 15 s sprints with full recovery.",
          {warmup(), reps(6, 15, 2.0, 285, 0.5), cooldown()}),
        W("sprint-power", "Sprint Power", "Anaerobic", "8 x 10-second max sprints in resistance mode - you choose the gear.",
          {warmup(), set(8, {Block::free(10, 2.0, true, "Sprint!"), steady(170, 0.5)}), cooldown()}),
        W("pyramid", "Pyramid", "Mixed", "1-2-3-4-3-2-1 minute steps climbing to VO2 and back.",
          {warmup(), steady(60, 0.9), steady(60, 0.5), steady(120, 1.0), steady(60, 0.5), steady(180, 1.05), steady(90, 0.5),
           steady(240, 1.1), steady(120, 0.5), steady(180, 1.05), steady(90, 0.5), steady(120, 1.0), steady(60, 0.5), steady(60, 0.9),
           cooldown()}),
        W("race-sim", "Race Simulation", "Mixed", "Unpredictable surges, a 5-minute break attempt and a sprint finish.",
          {warmup(), steady(300, 0.75), set(4, {steady(20, 1.5), steady(100, 0.8)}), steady(300, 1.05), steady(180, 0.7),
           set(6, {steady(30, 1.3), steady(60, 0.85)}), steady(240, 0.95), Block::free(20, 1.0, true, "Sprint finish"), cooldown()}),
        W("openers", "Pre-Race Openers", "Mixed", "Short day-before session: Zone 2 with 3 x 1 min at 110 % and 3 short sprints.",
          {warmup(), steady(300, 0.65), reps(3, 60, 1.1, 120, 0.55), reps(3, 10, 1.8, 110, 0.55), steady(300, 0.6), cooldown()}),
    };
}

// ------------------------------------------------------------------ FTP tests
std::vector<TestInfo> testCatalogue() {
    return {
        {TestKind::Ramp, "Ramp Test", "Short and sharp. ERG mode raises the target every minute until you can't hold it.",
         "5 min warm-up, then the target climbs ~6.7 % of your current FTP every minute. Keep cadence above 70 rpm; the test ends "
         "automatically when power or cadence collapses. FTP = 75 % of your best 1-minute power.",
         20},
        {TestKind::Ftp20, "20-Minute FTP Test", "The gold standard. Pace a 20-minute all-out effort in resistance mode.",
         "Warm-up, a 5-minute blow-out effort, recovery, then 20 minutes as hard as you can sustain on a 1 % virtual gradient - you "
         "control power with your gears. FTP = 95 % of the 20-minute average.",
         60},
        {TestKind::Ftp8, "2 x 8-Minute Test", "Two shorter maximal efforts - easier to pace than 20 minutes.",
         "Warm-up, then two 8-minute maximal efforts separated by 10 minutes of easy spinning. FTP = 90 % of the average of both efforts.", 45},
    };
}

static double roundTo(double x, double m) { return std::round(x / m) * m; }

std::pair<double, double> rampParams(double ftp) {
    ftp = std::max(ftp, 80.0);
    return {std::max(roundTo(ftp * 0.45, 10), 60.0), std::max(roundTo(ftp / 15.0, 5), 10.0)};
}

Workout testProtocol(TestKind k, double ftp) {
    ftp = std::max(ftp, 80.0);
    std::vector<Block> b;
    switch (k) {
        case TestKind::Ramp: {
            auto [start, step] = rampParams(ftp);
            b.push_back(Block::steady(180, start * 0.8 / ftp, {}, "Warm-up"));
            b.push_back(Block::steady(120, start / ftp, {}, "Warm-up"));
            int n = std::clamp(int(std::ceil((ftp * 2.2 - start) / step)), 15, 60);
            for (int i = 0; i < n; ++i) b.push_back(Block::steady(60, (start + step * i) / ftp, {}, "Ramp " + std::to_string(i + 1)));
            b.push_back(Block::free(300, 0, false, "Cool-down"));
            break;
        }
        case TestKind::Ftp20:
            b = {Block::ramp(600, 0.45, 0.75, "Warm-up"), Block::intervals(3, 60, 1.0, 60, 0.5, 100), Block::steady(180, 0.5, {}, "Easy"),
                 Block::free(300, 1, false, "Blow-out effort"), Block::steady(600, 0.5, {}, "Recover"),
                 Block::free(1200, 1, true, "20-minute test"), Block::free(600, 0, false, "Cool-down")};
            break;
        case TestKind::Ftp8:
            b = {Block::ramp(600, 0.45, 0.75, "Warm-up"), Block::intervals(3, 60, 1.0, 60, 0.5, 100), Block::steady(240, 0.5, {}, "Easy"),
                 Block::free(480, 1, true, "Effort 1 of 2"), Block::steady(600, 0.5, {}, "Recover"),
                 Block::free(480, 1, true, "Effort 2 of 2"), Block::free(600, 0, false, "Cool-down")};
            break;
    }
    Workout w;
    for (auto& t : testCatalogue())
        if (t.kind == k) { w.name = t.name; w.description = t.how; }
    w.id = k == TestKind::Ramp ? "test-ramp" : k == TestKind::Ftp20 ? "test-ftp20" : "test-ftp8";
    w.category = "Test";
    w.blocks = std::move(b);
    w.builtin = true;
    return w;
}

std::optional<TestResult> evaluateTest(TestKind k, const std::vector<Step>& steps, const std::vector<double>& t, const std::vector<double>& power) {
    if (power.empty()) return std::nullopt;
    double maxP = 0;
    for (double p : power) maxP = std::max(maxP, p);
    char buf[160];
    if (k == TestKind::Ramp) {
        std::vector<double> rp;
        for (size_t i = 0; i < t.size() && i < power.size(); ++i)
            for (auto& s : steps)
                if (s.label.rfind("Ramp", 0) == 0 && t[i] >= s.startS && t[i] < s.endS()) { rp.push_back(power[i]); break; }
        auto best = metrics::bestAverage(rp, 60);
        if (!best) return std::nullopt;
        TestResult r; r.kind = k;
        r.ftp = std::round(*best * 0.75);
        r.measuredW = std::round(*best);
        r.factor = 0.75;
        std::snprintf(buf, sizeof buf, "Best 1-minute power %.0f W over %zu ramp minutes", *best, rp.size() / 60);
        r.detail = buf;
        r.maxPower = maxP;
        r.valid = rp.size() >= 120;
        return r;
    }
    std::vector<const Step*> windows;
    for (auto& s : steps)
        if (s.effort && !s.erg) windows.push_back(&s);
    if (windows.empty()) return std::nullopt;
    std::vector<double> avgs;
    bool complete = true;
    for (auto* s : windows) {
        std::vector<double> v;
        for (size_t i = 0; i < t.size() && i < power.size(); ++i)
            if (t[i] >= s->startS && t[i] < s->endS()) v.push_back(power[i]);
        if (double(v.size()) < s->durationS * 0.9) complete = false;
        if (!v.empty()) avgs.push_back(metrics::mean(v));
    }
    if (avgs.empty()) return std::nullopt;
    double measured = metrics::mean(avgs);
    double factor = k == TestKind::Ftp20 ? 0.95 : 0.90;
    TestResult r; r.kind = k;
    r.ftp = std::round(measured * factor);
    r.measuredW = std::round(measured);
    r.factor = factor;
    if (k == TestKind::Ftp20) std::snprintf(buf, sizeof buf, "20-minute average %.0f W", measured), r.detail = buf;
    else {
        r.detail = "8-minute averages ";
        for (size_t i = 0; i < avgs.size(); ++i) {
            std::snprintf(buf, sizeof buf, "%s%.0f W", i ? " / " : "", avgs[i]);
            r.detail += buf;
        }
    }
    r.maxPower = maxP;
    r.valid = complete;
    return r;
}

}  // namespace wk
