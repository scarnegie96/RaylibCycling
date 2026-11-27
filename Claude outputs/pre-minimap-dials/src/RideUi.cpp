// Ride screen: 3D HUD, 2D dashboard, workout step panel, edit drawer, pause menu, help.
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "App.h"
#include "Audio.h"
#include "Profiler.h"
#include "Renderer.h"
#include "Store.h"
#include "raylib.h"

using namespace ui;

static std::string fmt(const char* f, double v) { char b[64]; std::snprintf(b, sizeof b, f, v); return b; }

static void metricTile(Rectangle r, const char* label, const std::string& value, const char* unit, Color accent, float size = 60,
                       const std::string& badge = "", Color badgeC = WHITE) {
    panel(r);
    DrawRectangleRounded({r.x + 9, r.y + 12, 3, r.height - 24}, 1, 4, accent);
    text(label, r.x + 20, r.y + 9, F::Small, DIM, 13);
    Vector2 m = measure(value, F::Big, size);
    text(value, r.x + 18, r.y + r.height - m.y - 2, F::Big, TEXT, size);
    if (unit && *unit) text(unit, r.x + 24 + m.x, r.y + r.height - 28, F::Small, DIM, 15);
    if (!badge.empty()) {
        Rectangle zr{r.x + r.width - 46, r.y + 9, 36, 20};
        DrawRectangleRounded(zr, 0.5f, 6, badgeC);
        textCentered(badge, zr, F::Small, BLACK, 13);
    }
}

static std::string modeLabel(const RideEngine& e, char* b, size_t n) {
    switch (e.mode()) {
        case ControlMode::Erg:
            if (e.setup().kind == RideKind::Free && e.freeMode() == FreeMode::HeartRate && !e.structuredActive())
                std::snprintf(b, n, "HR %.0f bpm \xE2\x86\x92 %.0f W", e.hrTargetValue(), e.targetW().value_or(0));
            else std::snprintf(b, n, "ERG %.0f W", e.targetW().value_or(0));
            break;
        case ControlMode::Level: std::snprintf(b, n, "LEVEL %.0f%%", e.freeLevel()); break;
        default:
            if (e.shiftingActive()) std::snprintf(b, n, "GEAR %d/24  SIM %.1f%%", e.gear(), e.trainerGradePct());
            else std::snprintf(b, n, "SIM %.1f%%", e.trainerGradePct());
    }
    return b;
}

void App::drawStepPanel(Rectangle br) {
    const RideEngine& e = *eng_;
    char b[200];
    panel(br);
    auto cs = e.currentStep();
    if (!cs) {
        textCentered(e.workoutComplete() ? "Workout complete" : "Get ready", br, F::Title, GOOD, 28);
        return;
    }
    const auto& st = e.steps()[cs->first];
    double rem = st.durationS - cs->second;
    text(st.label, br.x + 18, br.y + 10, F::Body, TEXT, 19);
    textRight(fmtTime(rem), br.x + br.width - 18, br.y + 4, F::Title, rem <= 3.5 ? WARN : TEXT, 34);
    if (st.erg) {
        double tgt = e.targetW().value_or(0);
        text(e.ergDisabled() ? "TARGET (resistance mode)" : "TARGET", br.x + 18, br.y + 42, F::Small, DIM, 12);
        text(fmt("%.0f W", tgt), br.x + 18, br.y + 56, F::Title, powerZoneColor(tgt, prof_.ftp), 32);
        double diff = e.power3s() - tgt;
        Color cc = std::fabs(diff) < tgt * 0.05 ? GOOD : std::fabs(diff) < tgt * 0.12 ? WARN : BAD;
        progressBar({br.x + 18, br.y + br.height - 9, br.width - 36, 4}, float(std::clamp(e.power3s() / std::max(1.0, tgt * 1.5), 0.0, 1.0)), cc);
    } else {
        text(st.effort ? "ALL-OUT EFFORT - YOU CONTROL POWER" : "RIDE BY FEEL", br.x + 18, br.y + 46, F::Small, st.effort ? ACCENT : DIM, 13);
        if (st.effort && e.setup().test) {
            double avg = e.splitAvgPower();
            (void)avg;
        }
    }
    if (st.cadence) {
        Color cc = std::fabs(e.cadence() - *st.cadence) < 5 ? GOOD : WARN;
        text(fmt("%.0f rpm target", *st.cadence), br.x + 140, br.y + 64, F::Small, cc, 15);
    }
    if (cs->first + 1 < e.steps().size()) {
        const auto& nx = e.steps()[cs->first + 1];
        if (nx.erg) std::snprintf(b, sizeof b, "Next: %s  %.0f W  %s", nx.label.c_str(), nx.from * prof_.ftp * e.ergBiasValue(), fmtTime(nx.durationS).c_str());
        else std::snprintf(b, sizeof b, "Next: %s  %s", nx.label.c_str(), fmtTime(nx.durationS).c_str());
        textRight(b, br.x + br.width - 18, br.y + 66, F::Small, DIM, 14);
    }
    if (std::fabs(e.ergBiasValue() - 1) > 0.001) textRight(fmt("Intensity %.0f%%", e.ergBiasValue() * 100), br.x + br.width - 18, br.y + 46, F::Small, WARN, 13);
}

// ------------------------------------------------------------------ 3D HUD (CoreKick's "Valley" HUD: light, glassy, quiet)
namespace {
void glass(Rectangle r, float alpha = 1.f) {
    float rr = std::min(1.f, 36.f / std::max(1.f, std::min(r.width, r.height)));
    DrawRectangleRounded({r.x, r.y + 6, r.width, r.height + 2}, rr, 10, Color{31, 36, 48, (unsigned char)(14 * alpha)});
    DrawRectangleRounded({r.x - 2, r.y + 2, r.width + 4, r.height + 6}, rr, 10, Color{31, 36, 48, (unsigned char)(7 * alpha)});
    DrawRectangleRounded(r, rr, 10, withAlpha(HUD_GLASS, (unsigned char)((hudIsDark() ? 178 : 190) * alpha)));
    DrawRectangleRoundedLinesEx(r, rr, 10, 1.f, hudIsDark() ? Color{255, 255, 255, (unsigned char)(26 * alpha)} : Color{255, 255, 255, (unsigned char)(200 * alpha)});
}
// a value with a small unit after it, baseline-aligned; returns the width used
float valUnit(const std::string& v, const char* unit, float x, float y, float size, Color c = HUD_INK) {
    text(v, x, y, F::Title, c, size);
    float w = measure(v, F::Title, size).x;
    if (unit && *unit) {
        text(unit, x + w + 3, y + size * 0.42f, F::Small, HUD_MUTED, 11);
        w += 3 + measure(unit, F::Small, 11).x;
    }
    return w;
}
float rowWidth(const std::vector<std::pair<std::string, const char*>>& items, float size) {
    float w = 0;
    for (auto& [v, u] : items) w += measure(v, F::Title, size).x + (u && *u ? 3 + measure(u, F::Small, 11).x : 0) + 16;
    return w - 16;
}
bool hudButton(Rectangle r, const std::string& label, int kind = 0) {  // 0 plain, 1 strong, 2 danger
    bool h = hovered(r);
    float rr = std::min(1.f, 24.f / r.height);
    DrawRectangleRounded({r.x, r.y + 3, r.width, r.height}, rr, 8, Color{31, 36, 48, 12});
    Color bg = kind == 1 ? HUD_INK : withAlpha(HUD_GLASS, (unsigned char)(h ? 225 : 175));
    DrawRectangleRounded(r, rr, 8, bg);
    DrawRectangleRoundedLinesEx(r, rr, 8, 1.f, kind == 1 ? HUD_INK : hudIsDark() ? Color{255, 255, 255, 30} : Color{255, 255, 255, 200});
    textCentered(label, r, F::Small, kind == 1 ? HUD_INV : kind == 2 ? hudPick(Color{220, 61, 67, 255}, Color{255, 110, 110, 255}) : HUD_INK, 12);
    if (h) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
    return h && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}
float hudButtonW(const std::string& label) { return measure(label, F::Small, 12).x + 26; }

// a small weather glyph drawn from shapes (sun / moon, cloud, rain, snow, bolt, fog, wind), size s px
void weatherIcon(weather::Kind k, Vector2 c, float s, bool night) {
    using weather::Kind;
    const Color sun{255, 190, 50, 255}, moon{214, 220, 236, 255}, lightCloud{206, 212, 222, 255}, darkCloud{128, 137, 152, 255};
    const Color rain{66, 140, 222, 255}, snow{150, 170, 200, 255}, bolt{255, 196, 40, 255};
    auto orb = [&](Vector2 p, float r) {
        if (night) { DrawCircleV(p, r, moon); DrawCircleV({p.x + r * 0.45f, p.y - r * 0.3f}, r * 0.8f, Color{255, 255, 255, 200}); return; }
        for (int i = 0; i < 8; ++i) {
            float a = i * PI / 4;
            DrawLineEx({p.x + std::cos(a) * r * 1.3f, p.y + std::sin(a) * r * 1.3f}, {p.x + std::cos(a) * r * 1.65f, p.y + std::sin(a) * r * 1.65f}, std::max(1.5f, s * 0.05f), sun);
        }
        DrawCircleV(p, r, sun);
    };
    auto cloud = [&](Vector2 p, float w, Color col) {
        DrawCircleV({p.x - w * 0.28f, p.y + w * 0.05f}, w * 0.22f, col);
        DrawCircleV({p.x + w * 0.02f, p.y - w * 0.1f}, w * 0.3f, col);
        DrawCircleV({p.x + w * 0.3f, p.y + w * 0.06f}, w * 0.2f, col);
        DrawRectangleRounded({p.x - w * 0.5f, p.y + w * 0.02f, w, w * 0.25f}, 1, 8, col);
    };
    auto drops = [&](Vector2 p, int n, bool dots, Color col) {
        for (int i = 0; i < n; ++i) {
            float x = p.x - s * 0.22f + i * s * 0.22f;
            if (dots) DrawCircleV({x, p.y + s * 0.08f}, s * 0.05f, col);
            else DrawLineEx({x + s * 0.04f, p.y}, {x - s * 0.04f, p.y + s * 0.16f}, std::max(1.5f, s * 0.06f), col);
        }
    };
    auto lines = [&](Vector2 p, int n, Color col) {
        for (int i = 0; i < n; ++i) DrawLineEx({p.x - s * 0.36f + (i % 2) * s * 0.08f, p.y + i * s * 0.14f}, {p.x + s * 0.3f + (i % 2) * s * 0.08f, p.y + i * s * 0.14f}, std::max(1.5f, s * 0.06f), col);
    };
    const Vector2 cc{c.x + s * 0.02f, c.y + s * 0.0f}, below{c.x, c.y + s * 0.22f};
    switch (k) {
        case Kind::Clear: orb(c, s * 0.26f); break;
        case Kind::Fair: orb({c.x - s * 0.14f, c.y - s * 0.12f}, s * 0.2f); cloud({c.x + s * 0.08f, c.y + s * 0.08f}, s * 0.62f, lightCloud); break;
        case Kind::Windy: orb({c.x - s * 0.18f, c.y - s * 0.2f}, s * 0.15f); lines({c.x + s * 0.04f, c.y + s * 0.02f}, 3, darkCloud); break;
        case Kind::Cloudy: orb({c.x - s * 0.18f, c.y - s * 0.16f}, s * 0.17f); cloud(cc, s * 0.8f, lightCloud); break;
        case Kind::Overcast: cloud({c.x - s * 0.1f, c.y - s * 0.1f}, s * 0.6f, darkCloud); cloud({c.x + s * 0.06f, c.y + s * 0.04f}, s * 0.75f, lightCloud); break;
        case Kind::Mist: case Kind::Fog: if (k == Kind::Fog) cloud({c.x, c.y - s * 0.12f}, s * 0.7f, lightCloud); lines({c.x, c.y + (k == Kind::Fog ? s * 0.12f : -s * 0.12f)}, 3, darkCloud); break;
        case Kind::Drizzle: cloud({c.x, c.y - s * 0.1f}, s * 0.8f, lightCloud); drops(below, 3, true, rain); break;
        case Kind::Sunshower: orb({c.x - s * 0.18f, c.y - s * 0.2f}, s * 0.16f); cloud({c.x + s * 0.06f, c.y - s * 0.04f}, s * 0.7f, lightCloud); drops({below.x + s * 0.06f, below.y + s * 0.04f}, 2, false, rain); break;
        case Kind::Rain: cloud({c.x, c.y - s * 0.1f}, s * 0.82f, darkCloud); drops(below, 3, false, rain); break;
        case Kind::Storm:
            cloud({c.x, c.y - s * 0.12f}, s * 0.82f, darkCloud);
            DrawTriangle({c.x + s * 0.02f, c.y + s * 0.08f}, {c.x - s * 0.12f, c.y + s * 0.26f}, {c.x - s * 0.01f, c.y + s * 0.24f}, bolt);
            DrawTriangle({c.x - s * 0.01f, c.y + s * 0.2f}, {c.x - s * 0.06f, c.y + s * 0.42f}, {c.x + s * 0.1f, c.y + s * 0.2f}, bolt);
            break;
        case Kind::Hail: cloud({c.x, c.y - s * 0.1f}, s * 0.82f, darkCloud); for (int i = 0; i < 3; ++i) { DrawCircleV({c.x - s * 0.2f + i * s * 0.2f, below.y + s * 0.08f}, s * 0.07f, WHITE); DrawCircleLines(int(c.x - s * 0.2f + i * s * 0.2f), int(below.y + s * 0.08f), s * 0.07f, snow); } break;
        case Kind::Sleet: cloud({c.x, c.y - s * 0.1f}, s * 0.8f, darkCloud); drops(below, 2, false, rain); DrawCircleV({c.x + s * 0.24f, below.y + s * 0.1f}, s * 0.05f, snow); break;
        case Kind::Snow: case Kind::Blizzard:
            cloud({c.x, c.y - s * 0.1f}, s * 0.8f, lightCloud);
            for (int i = 0; i < 3; ++i) {
                Vector2 f{c.x - s * 0.22f + i * s * 0.22f, below.y + s * 0.1f};
                for (int q = 0; q < 3; ++q) { float a = q * PI / 3; DrawLineEx({f.x - std::cos(a) * s * 0.07f, f.y - std::sin(a) * s * 0.07f}, {f.x + std::cos(a) * s * 0.07f, f.y + std::sin(a) * s * 0.07f}, 1.5f, snow); }
            }
            if (k == Kind::Blizzard) lines({c.x, c.y + s * 0.38f}, 1, darkCloud);
            break;
        default: cloud(cc, s * 0.8f, lightCloud);
    }
}
const char* compass8(double rad) {
    static const char* n[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return n[int(std::lround(rad / (PI / 4))) & 7];
}
}  // namespace

void App::updateFlow(float dt) {
    // flow: fills while the effort is steady (or on target), like CoreKick's
    const RideEngine& e = *eng_;
    double p = e.power3s();
    if (!e.started() || e.paused()) return;
    double k = std::min(1.0, dt / 10.0);
    flowMean_ += (p - flowMean_) * k;
    flowVar_ += ((p - flowMean_) * (p - flowMean_) - flowVar_) * k;
    double cv = std::sqrt(std::max(0.0, flowVar_)) / std::max(60.0, flowMean_);
    double target = p > 0.35 * prof_.ftp ? std::clamp(1.0 - (cv - 0.05) / 0.22, 0.0, 1.0) : 0.0;
    if (auto t = e.targetW()) target = std::max(target, std::clamp(1.0 - std::fabs(p - *t) / std::max(20.0, *t * 0.15), 0.0, 1.0));
    flow_ += (target - flow_) * std::min(1.0, dt / 3.0);
    if (flow_ > 0.6) flowStreak_ += dt; else flowStreak_ = 0;
    flowBest_ = std::max(flowBest_, flowStreak_);
}

void App::drawRideHud() {
    const RideEngine& e = *eng_;
    const float W = ui::screenW(), H = ui::screenH();
    const float pad = 22;
    const bool metric = prof_.metric;

    // ---- top-left: effort
    float effortRight = pad, effortBottom = pad;
    {
        double p3 = e.power3s();
        std::string pw = std::to_string(int(std::lround(p3)));
        float pwW = measure(pw, F::Big, 54).x;
        std::string tgt;
        if (auto t = e.targetW()) tgt = "\xE2\x86\x92 " + std::to_string(int(std::lround(*t)));
        std::string hr = e.heartRate() ? std::to_string(int(std::lround(*e.heartRate()))) : "\xE2\x80\x93";
        std::vector<std::pair<std::string, const char*>> row = {{hr, "bpm"}, {std::to_string(int(std::lround(e.cadence()))), "rpm"},
                                                                  {fmt("%.1f", p3 / prof_.weightKg), "W/kg"}};
        float rw = rowWidth(row, 17) + 13;
        std::string step;
        if (auto cs = e.currentStep()) {
            const auto& st = e.steps()[cs->first];
            step = st.label + "  \xC2\xB7  " + fmtTime(st.durationS - cs->second);
        } else if (e.setup().kind == RideKind::Free && !e.structuredActive()) {
            if (e.freeMode() == FreeMode::Power) step = fmt("Holding %.0f W", e.freeWattsValue());
            else if (e.freeMode() == FreeMode::HeartRate) step = fmt("Heart rate target %.0f bpm", e.hrTargetValue());
            else if (e.freeMode() == FreeMode::Level) step = fmt("Level %.0f%%", e.freeLevel());
        }
        if (e.goal().type != Goal::None) step += (step.empty() ? "" : "   ") + std::string("Goal: ") + (e.goalDone() ? "done!" : e.goalRemaining());
        float cw = std::max({220.f, pwW + 60 + (tgt.empty() ? 0 : measure(tgt, F::Small, 15).x + 30), rw + 32, measure(step, F::Small, 12).x + 32});
        Rectangle r{pad, pad, cw, step.empty() ? 104.f : 126.f};
        effortRight = r.x + r.width;
        effortBottom = r.y + r.height;
        glass(r);
        text(pw, r.x + 16, r.y + 10, F::Big, HUD_INK, 54);
        text("W", r.x + 16 + pwW + 6, r.y + 36, F::Small, HUD_MUTED, 16);
        if (!tgt.empty()) {
            double t = *e.targetW();
            bool on = std::fabs(p3 - t) / std::max(1.0, t) < 0.08;
            Vector2 m = measure(tgt, F::Small, 14);
            Rectangle pill{r.x + 16 + pwW + 32, r.y + 32, m.x + 16, 24};
            DrawRectangleRounded(pill, 1, 8, on ? Color{255, 190, 90, 90} : hudInk(20));
            textCentered(tgt, pill, F::Small, on ? hudPick(Color{138, 75, 0, 255}, Color{255, 200, 120, 255}) : HUD_MUTED, 14);
        }
        float x = r.x + 16, y = r.y + 72;
        if (e.heartRate()) DrawCircle(int(x + 4), int(y + 10), 4, hrZoneColor(e.hrZone(), int(prof_.hrZones().size())));
        else DrawCircle(int(x + 4), int(y + 10), 4, hudInk(40));
        x += 13;
        for (auto& [v, u] : row) x += valUnit(v, u, x, y, 17) + 16;
        if (!step.empty()) text(step, r.x + 16, r.y + 100, F::Small, HUD_MUTED, 12);
    }

    // ---- virtual gear (under the effort card): gear number, a tick per gear, shift buttons
    if (e.virtualShifting()) {
        const bool live = e.shiftingActive();
        Rectangle r{pad, effortBottom + 12, 236, 74};
        glass(r);
        text("GEAR", r.x + 16, r.y + 10, F::Small, HUD_MUTED, 10);
        std::string g = std::to_string(e.gear());
        text(g, r.x + 16, r.y + 22, F::Big, live ? HUD_INK : HUD_MUTED, 34);
        float gw = measure(g, F::Big, 34).x;
        text("/ 24", r.x + 20 + gw, r.y + 38, F::Small, HUD_MUTED, 12);
        // 24 ticks, rising like a cassette seen side-on; the current gear lit
        float tx = r.x + 80, ty = r.y + 58;
        for (int i = 0; i < phys::GEAR_COUNT; ++i) {
            float hgt = 6 + 16.f * i / (phys::GEAR_COUNT - 1);
            Color c = i == e.gear() - 1 ? ui::ACCENT : i < e.gear() - 1 ? hudInk(90) : hudInk(35);
            DrawRectangleRounded({tx + i * 3.4f, ty - hgt, 2.2f, hgt}, 1, 4, c);
        }
        Link sl = ble_.shifter();
        std::string sub = !live ? "gears rest in ERG" : sl.state == LinkState::Connected ? sl.name : "[ ] or Zwift shifter";
        text(sub, r.x + 80, r.y + 12, F::Small, HUD_MUTED, 11);
        if (sl.state == LinkState::Connected) DrawCircle(int(r.x + 76 + measure(sub, F::Small, 11).x + 10), int(r.y + 18), 3, ui::ACCENT);
        if (hudButton({r.x + r.width - 72, r.y + 32, 28, 28}, "-")) eng_->shift(-1);
        if (hudButton({r.x + r.width - 40, r.y + 32, 28, 28}, "+")) eng_->shift(1);
    }

    // ---- top-right: the ride. Live speed and grade beside the time, then the ride's totals
    // grouped in one row (distance, climbed, average power, average W/kg), then where you are.
    float rideBottom, rideLeft;
    {
        std::string tm = fmtTime(e.elapsedS());
        double g = e.gradePct();
        Color gc = gradeColor(g / 100);
        Color gradeInk{(unsigned char)(gc.r * 0.8f), (unsigned char)(gc.g * 0.75f), (unsigned char)(gc.b * 0.8f), 255};
        std::vector<std::pair<std::string, const char*>> live = {{fmt("%.1f", e.speedMps() * (metric ? 3.6 : 2.23694)), metric ? "km/h" : "mph"}, {fmt("%.1f", g), "%"}};
        struct Cell { const char* label; std::string v; const char* unit; };
        double avgP = e.avgPower();
        std::vector<Cell> tot = {{"DISTANCE", fmt("%.1f", e.distance() / (metric ? 1000.0 : 1609.344)), metric ? "km" : "mi"},
                                 {"CLIMBED", fmt("%.0f", metric ? e.gain() : e.gain() * 3.28084), metric ? "m" : "ft"},
                                 {"AVG POWER", fmt("%.0f", avgP), "W"},
                                 {"AVG W/KG", fmt("%.1f", avgP / std::max(30.0, prof_.weightKg)), ""},
                                 {"CALORIES", fmt("%.0f", e.calories()), "kcal"}};   // work / gross efficiency (Profile)
        std::string sub = fmt(metric ? "Elevation %.0f m" : "Elevation %.0f ft", metric ? e.ele() : e.ele() * 3.28084);
        if (!endlessRide_ && !e.endless()) {
            sub += "  \xC2\xB7  " + fmtDist(std::max(0.0, e.profileData().length * e.laps() - e.distance()), metric) + " to go";
            if (auto gap = e.ghostGapS()) sub += std::string("  \xC2\xB7  ghost ") + (*gap >= 0 ? "-" : "+") + fmtTime(std::fabs(*gap));
        }
        if (e.paused()) sub = "Paused  \xC2\xB7  " + sub;
        // column widths: each total gets the room its label or value needs
        std::vector<float> colW;
        float totW = 0;
        for (auto& c : tot) {
            float vw = measure(c.v, F::Title, 18).x + (*c.unit ? 3 + measure(c.unit, F::Small, 11).x : 0);
            colW.push_back(std::max(vw, measure(c.label, F::Small, 10).x) + 4);
            totW += colW.back();
        }
        const float gapX = 18;
        totW += gapX * (tot.size() - 1);
        float topW = measure(tm, F::Big, 34).x + 24 + rowWidth(live, 18);
        float cw = std::max({totW, topW, measure(sub, F::Small, 12).x}) + 32;
        Rectangle r{W - pad - cw, pad, cw, 128};
        glass(r);
        // time (left) and live speed / grade (right)
        text(tm, r.x + 16, r.y + 8, F::Big, HUD_INK, 34);
        float x = r.x + r.width - 16 - rowWidth(live, 18);
        for (size_t i = 0; i < live.size(); ++i) x += valUnit(live[i].first, live[i].second, x, r.y + 16, 18, i == 1 ? gradeInk : HUD_INK) + 16;
        DrawLineEx({r.x + 16, r.y + 54}, {r.x + r.width - 16, r.y + 54}, 1, hudInk(22));
        // the ride's totals
        x = r.x + 16;
        for (size_t i = 0; i < tot.size(); ++i) {
            text(tot[i].label, x, r.y + 62, F::Small, HUD_MUTED, 10);
            valUnit(tot[i].v, tot[i].unit, x, r.y + 76, 18);
            x += colW[i] + gapX;
        }
        text(sub, r.x + 16, r.y + 104, F::Small, HUD_MUTED, 12);
        rideBottom = r.y + r.height;
        rideLeft = r.x;
    }

    // ---- climb (under the ride card)
    {
        int ci = e.currentClimb(), ni = e.nextClimb();
        if (ci >= 0 || (ni >= 0 && !endlessRide_)) {
            const Climb& c = e.profileData().climbs[ci >= 0 ? ci : ni];
            double ld = e.lapDistance();
            std::string cat = c.category == "Uncategorised" ? "Climb" : c.category;
            std::string main, rest;
            if (ci >= 0) {
                main = fmtDist(c.endD - ld, metric);
                rest = fmt(" to the top  \xC2\xB7  %.0f m", std::max(0.0, c.topEle - e.ele())) + fmt("  \xC2\xB7  avg %.1f%%", c.avgGradePct);
            } else {
                double to = c.startD - ld;
                if (to < 0) to += e.profileData().length;
                main = "in " + fmtDist(to, metric);
                rest = fmt("  \xC2\xB7  %.1f km", c.lengthM / 1000) + fmt(" at %.1f%%", c.avgGradePct);
            }
            float cw = measure(cat, F::Small, 11).x + 16 + 12 + measure(main, F::Title, 16).x + measure(rest, F::Small, 12).x + 28;
            Rectangle r{W - pad - cw, rideBottom + 12, cw, 40};
            glass(r);
            Rectangle badge{r.x + 12, r.y + 9, measure(cat, F::Small, 11).x + 16, 22};
            DrawRectangleRounded(badge, 0.4f, 6, Color{240, 122, 26, 255});
            textCentered(cat, badge, F::Small, WHITE, 11);
            float x = badge.x + badge.width + 10;
            text(main, x, r.y + 10, F::Title, HUD_INK, 16);
            text(rest, x + measure(main, F::Title, 16).x, r.y + 13, F::Small, HUD_MUTED, 12);
            rideBottom = r.y + r.height;
        }
    }

    float rightBottom = rideBottom;   // the foot of the right-hand stack (the minimap goes under it)
    // ---- weather: its own card, left of the ride card (or under the right-hand stack if there's no room)
    {
        const float ww = 292, wh = weatherCardH();
        Rectangle wr{rideLeft - 14 - ww, pad, ww, wh};
        if (wr.x < effortRight + 16) { wr = {W - pad - ww, rideBottom + 12, ww, wh}; rightBottom = wr.y + wr.height; }
        drawWeatherCard(wr);
    }

    // ---- top centre: workout step, road-change banner
    {
        float bw = std::min(560.f, W - 2 * 330);
        float cy = pad;
        if (e.structured() && bw > 300) {
            drawStepPanel({(W - bw) / 2, cy, bw, 92});
            cy += 100;
        }
        if (bw > 300) drawSectorBanner((W - bw) / 2, cy, bw);
    }

    // ---- bottom row: flow | the road | controls
    const float by = H - pad;
    // controls (right): wrap into rows, bottom-aligned
    struct Btn { std::string label; int kind; int id; };
    std::vector<Btn> btns;
    btns.push_back({"Edit", drawerOpen_ ? 1 : 0, 1});
    btns.push_back({"Lap", 0, 2});
    if (simActive()) btns.push_back({"Test", testPanel_ ? 1 : 0, 3});
    btns.push_back({e.paused() ? "Resume" : "Pause", 0, 4});
    btns.push_back({"End", 2, 5});
    btns.push_back({camModeName(cam_), 0, 6});
    {
        static const char* tod[] = {"Real time", "Fixed time", "Day cycle"};
        btns.push_back({tod[std::clamp(prefs_.todMode, 0, 2)], 0, 7});
    }
    btns.push_back({prefs_.weatherMode == 0 ? "Changing" : prefs_.weatherMode == 1 ? "Live weather" : weather::kindLabel(*weather::kindFromId(prefs_.weatherKind)), 0, 8});
    btns.push_back({prefs_.sound ? "Sound on" : "Sound off", 0, 9});
    btns.push_back({"?", 0, 10});
    btns.push_back({"2D", 1, 11});
    btns.push_back({"", 0, 12});   // devices (cog icon)
    const float maxCW = std::min(440.f, W * 0.34f), bh = 32, gap = 6;
    std::vector<std::vector<int>> rows(1);
    float rwAcc = 0;
    auto bw = [&](const Btn& b) { return b.id == 12 ? bh : hudButtonW(b.label); };
    for (int i = 0; i < int(btns.size()); ++i) {
        float w = bw(btns[i]);
        if (rwAcc > 0 && rwAcc + gap + w > maxCW) { rows.emplace_back(); rwAcc = 0; }
        rows.back().push_back(i);
        rwAcc += (rwAcc > 0 ? gap : 0) + w;
    }
    float ctrlTop = by - rows.size() * bh - (rows.size() - 1) * gap;
    float ctrlLeft = W - pad;
    for (size_t ri = 0; ri < rows.size(); ++ri) {
        float total = 0;
        for (int i : rows[ri]) total += bw(btns[i]) + gap;
        float x = W - pad - (total - gap), y = ctrlTop + ri * (bh + gap);
        ctrlLeft = std::min(ctrlLeft, x);
        for (int i : rows[ri]) {
            float w = bw(btns[i]);
            bool hit = hudButton({x, y, w, bh}, btns[i].label, btns[i].kind);
            if (btns[i].id == 12) {   // the cog, drawn from the SVG (rasterised to assets/ui/settings-cog-64.png)
                Link tl = ble_.trainer();
                bool trouble = tl.state == LinkState::Reconnecting || (!simActive() && tl.state != LinkState::Connected);
                if (cogTex_.id) DrawTexturePro(cogTex_, {0, 0, float(cogTex_.width), float(cogTex_.height)}, {x + 7, y + 7, w - 14, bh - 14}, {0, 0}, 0, HUD_INK);
                else DrawCircleLines(int(x + w / 2), int(y + bh / 2), 7, HUD_INK);
                if (trouble) DrawCircle(int(x + w - 6), int(y + 6), 4, Color{220, 61, 67, 255});   // a device needs attention
                if (hovered({x, y, w, bh})) {
                    const char* tip = "Devices";
                    Vector2 m = measure(tip, F::Small, 12);
                    Rectangle tp{x + w - m.x - 16, y - 30, m.x + 16, 24};
                    DrawRectangleRounded(tp, 1, 8, Color{17, 24, 39, 225});
                    text(tip, tp.x + 8, tp.y + 5, F::Small, WHITE, 12);
                }
            }
            if (hit) {
                switch (btns[i].id) {
                    case 1: drawerOpen_ = !drawerOpen_; break;
                    case 2: eng_->lap(); break;
                    case 3: testPanel_ = !testPanel_; break;
                    case 4: if (eng_->paused()) { eng_->resume(); pauseMenu_ = false; } else eng_->pause(); break;
                    case 5: pauseMenu_ = true; eng_->pause(); break;
                    case 6: cam_ = CamMode((int(cam_) + 1) % int(CamMode::COUNT)); break;
                    case 7: cycleTime(); break;
                    case 8: cycleWeather(); break;
                    case 9: prefs_.sound = !prefs_.sound; sfx::setMuted(!prefs_.sound); store::save(prof_, prefs_); break;
                    case 10: helpOpen_ = !helpOpen_; break;
                    case 11: view3d_ = false; break;
                    case 12: devicesModal_ = true; devicesOpenedAt_ = GetTime(); break;
                }
            }
            x += w + gap;
        }
    }
    // ---- the minimap (generated worlds): right-hand side, between the stack and the controls
    if (prefs_.miniMap && map_ && course_ && course_->world() == map_.get()) {
        miniMap_.setMap(map_);   // bakes once per world, off-thread
        float top = rightBottom + 16, bottom = ctrlTop - 16;
        float rad = std::min(std::clamp(H * 0.15f, 92.f, 132.f), (bottom - top) / 2 - 6);
        if (rad >= 64) {
            Vector2 c{W - pad - rad - 6, bottom - rad - 6};
            auto p = course_->at(e.distance());
            drawMiniMap(c, rad, p.pos.x, p.pos.z, p.heading, e.distance());
        }
    }
    // flow (left)
    float flowRight;
    {
        Rectangle r{pad, by - 72, 168, 72};
        glass(r);
        Vector2 c{r.x + 36, r.y + 36};
        DrawRing(c, 14.5f, 19.5f, 0, 360, 48, hudInk(22));
        float v = float(std::clamp(flow_, 0.0, 1.0));
        if (v > 0.01f) DrawRing(c, 14.5f, 19.5f, -90, -90 + 360 * v, 48, Color{255, 170, 70, 255});
        DrawCircleV(c, 4 + v * 5, Color{255, 211, 107, (unsigned char)(90 + 150 * v)});
        text("FLOW", r.x + 66, r.y + 13, F::Small, hudPick(Color{138, 90, 0, 255}, Color{255, 205, 120, 255}), 11);
        text(flowStreak_ >= 5 ? fmtTime(flowStreak_) : "\xE2\x80\x94", r.x + 66, r.y + 27, F::Title, HUD_INK, 22);
        text("best " + fmtTime(flowBest_), r.x + 66, r.y + 52, F::Small, HUD_MUTED, 11);
        flowRight = r.x + r.width;
    }
    // the road ahead: hilliness for the kilometres not yet built (endless rides)
    if (endlessRide_) {
        Rectangle r{pad, by - 72 - 12 - 112, 330, 112};
        glass(r);
        text("ROAD AHEAD", r.x + 16, r.y + 12, F::Small, hudPick(Color{15, 128, 72, 255}, Color{100, 225, 160, 255}), 11);
        textRight("from km " + std::to_string(endless_.nextKm + 1), r.x + r.width - 16, r.y + 12, F::Small, HUD_MUTED, 11);
        std::vector<std::string> hs;
        for (int i = 0; i < 5; ++i) hs.push_back(hillinessName(Hilliness(i)));
        int sel = segmented({r.x + 12, r.y + 32, r.width - 24, 30}, hs, int(hillNext_));
        if (sel >= 0) { hillNext_ = Hilliness(sel); toast(std::string("Road ahead: ") + hillinessName(hillNext_)); }
        float y = r.y + 72;
        Rectangle minus{r.x + 12, y, 30, 28}, plus{r.x + r.width - 42, y, 30, 28};
        if (hudButton(minus, "-")) netNext_ = std::max(-120, netNext_ - 10);
        if (hudButton(plus, "+")) netNext_ = std::min(150, netNext_ + 10);
        std::string nv = netNext_ == 0 ? "Level" : fmt(metric ? "%+.0f m" : "%+.0f ft", metric ? netNext_ : netNext_ * 3.28084);
        std::string nu = netNext_ == 0 ? "  per km" : fmt(" per km  \xC2\xB7  avg %+.1f%%", netNext_ / 10.0);
        float tw = measure(nv, F::Title, 18).x + measure(nu, F::Small, 12).x;
        float tx = r.x + (r.width - tw) / 2;
        text(nv, tx, y + 3, F::Title, netNext_ > 0 ? hudPick(Color{194, 94, 11, 255}, Color{255, 160, 80, 255}) : netNext_ < 0 ? hudPick(Color{47, 127, 216, 255}, Color{120, 180, 255, 255}) : HUD_INK, 18);
        text(nu, tx + measure(nv, F::Title, 18).x, y + 8, F::Small, HUD_MUTED, 12);
    }
    // the road (centre strip): the ridden part warm, you a dark line, climbs underlined
    {
        float sx = flowRight + 14, sw = std::min(640.f, ctrlLeft - 14 - sx);
        if (sw > 160) {
            Rectangle r{sx + std::max(0.f, (ctrlLeft - 14 - sx - sw) / 2), by - 72, sw, 72};
            glass(r);
            Rectangle in{r.x + 12, r.y + 9, r.width - 24, r.height - 18};
            const RouteProfile& rp = e.profileData();
            double d = e.lapDistance();
            // A moving window: you start at the left, ride to the middle, then the road scrolls
            // under you so there is always as much ahead as behind. A fixed route shorter than
            // the window is shown whole; at the end of the last lap you ride on to the right edge.
            constexpr double SPAN = 6000;
            const bool grows = e.endless();                                  // endless road / roaming: built as you go
            const bool wraps = !grows && e.lapNumber() < e.laps();           // more laps: ahead continues from the start
            double a0 = std::max(0.0, d - SPAN / 2), a1 = a0 + SPAN;
            if (!grows && !wraps && rp.length > 0) { a0 = std::max(0.0, std::min(a0, rp.length - SPAN)); a1 = std::min(a0 + SPAN, rp.length); }
            const double built = grows ? rp.length : 1e18;                   // nothing drawn past what's laid out
            auto eleAt = [&](double dd) {
                if (wraps && rp.length > 0 && dd > rp.length) dd = std::fmod(dd, rp.length);
                return rp.elevationAt(std::min(dd, grows ? rp.length : dd));
            };
            if (a1 - a0 > 1) {
                double mn = 1e9, mx = -1e9;
                const int N = int(in.width / 2);
                std::vector<float> ev(N + 1);
                for (int i = 0; i <= N; ++i) {
                    double dd = a0 + (a1 - a0) * i / N;
                    ev[i] = float(eleAt(dd));
                    if (dd > built) continue;
                    mn = std::min(mn, double(ev[i])); mx = std::max(mx, double(ev[i]));
                }
                if (mx - mn < 20) mx = mn + 20;
                float xr = in.x + float((d - a0) / (a1 - a0)) * in.width;
                for (int i = 0; i < N; ++i) {
                    if (a0 + (a1 - a0) * i / N > built) break;
                    float x0 = in.x + in.width * i / N, x1 = in.x + in.width * (i + 1) / N;
                    float y0 = in.y + in.height - 4 - float((ev[i] - mn) / (mx - mn)) * (in.height - 10);
                    float y1 = in.y + in.height - 4 - float((ev[i + 1] - mn) / (mx - mn)) * (in.height - 10);
                    Color c = x1 <= xr ? Color{255, 190, 100, 200} : hudInk(36);
                    DrawTriangle({x0, y0}, {x0, in.y + in.height}, {x1, y1}, c);
                    DrawTriangle({x1, y1}, {x0, in.y + in.height}, {x1, in.y + in.height}, c);
                }
                for (int lapOff = 0; lapOff <= (wraps ? 1 : 0); ++lapOff)
                for (const auto& c : rp.climbs) {
                    double cs = c.startD + lapOff * rp.length, ce = c.endD + lapOff * rp.length;
                    if (ce < a0 || cs > a1) continue;
                    float x0 = in.x + float((std::max(cs, a0) - a0) / (a1 - a0)) * in.width;
                    float x1 = in.x + float((std::min(ce, a1) - a0) / (a1 - a0)) * in.width;
                    DrawRectangleRec({x0, in.y + in.height - 2, x1 - x0, 2}, Color{240, 122, 26, 255});
                }
                if (auto gd = e.ghostDistance()) {
                    double g = std::fmod(*gd, std::max(1.0, rp.length));
                    if (g >= a0 && g <= a1) {
                        float gx = in.x + float((g - a0) / (a1 - a0)) * in.width;
                        for (float yy = in.y; yy < in.y + in.height; yy += 6) DrawLineEx({gx, yy}, {gx, yy + 3}, 1.5f, Color{142, 124, 240, 255});
                    }
                }
                DrawLineEx({xr, in.y}, {xr, in.y + in.height}, 2, HUD_INK);
                if (endlessRide_) {  // where the road is still to be shaped
                    text("built to km " + std::to_string(int(rp.length / 1000)), in.x + in.width - measure("built to km 00", F::Small, 10).x, in.y - 2, F::Small, HUD_MUTED, 10);
                }
            }
        }
    }

    if (!e.started()) {
        bool ok = ble_.trainer().state == LinkState::Connected || simActive();
        std::string msg = ok ? "Start pedalling to begin" : "No trainer  \xC2\xB7  Esc for the menu";
        Vector2 m = measure(msg, F::Title, 22);
        Rectangle r{(W - m.x - 60) / 2, H * 0.36f, m.x + 60, 52};
        glass(r);
        textCentered(msg, r, F::Title, HUD_INK, 22);
    }
    if (e.paused() && !pauseMenu_) {
        DrawRectangle(0, 0, int(W), int(H), Color{31, 36, 48, 30});
        Vector2 m = measure("Paused", F::Big, 42);
        text("Paused", (W - m.x) / 2 + 1, (H - m.y) / 2 + 3, F::Big, Color{31, 36, 48, 90}, 42);
        text("Paused", (W - m.x) / 2, (H - m.y) / 2, F::Big, WHITE, 42);
    }
}

// ------------------------------------------------------------------ 2D dashboard (CoreKick style)
void App::drawDashboard2d() {
    const RideEngine& e = *eng_;
    const float W = ui::screenW(), H = ui::screenH();
    const bool metric = prof_.metric;
    char b[200];
    DrawRectangle(0, 0, int(W), int(H), BG);
    float pad = 20;
    // header
    text(e.setup().name, pad, 14, F::Title, TEXT, 28);
    std::string ml = modeLabel(e, b, sizeof b);
    textRight(ml, W - pad, 20, F::Body, e.mode() == ControlMode::Erg ? ACCENT : TEXT);
    // metric row
    struct M { const char* l; std::string v; const char* u; Color c; };
    double p3 = e.power3s();
    std::vector<M> ms = {
        {"POWER 3s", std::to_string(int(std::lround(p3))), "W", powerZoneColor(p3, prof_.ftp)},
        {"HEART RATE", e.heartRate() ? std::to_string(int(*e.heartRate())) : "--", "bpm", hrZoneColor(e.hrZone(), int(prof_.hrZones().size()))},
        {"CADENCE", std::to_string(int(std::lround(e.cadence()))), "rpm", DIM},
        {"SPEED", fmtSpeed(e.speedMps(), metric), metric ? "km/h" : "mph", SKY},
        {"TIME", fmtTime(e.elapsedS()), "", TEXT},
        {"DISTANCE", fmtDist(e.distance(), metric), "", TEXT},
        {"GRADE", fmt("%+.1f", e.gradePct()), "%", gradeColor(e.gradePct() / 100)},
    };
    float mw = (W - 2 * pad - 6 * 12) / 7, my = 60, mh = 110;
    for (size_t i = 0; i < ms.size(); ++i) metricTile({pad + i * (mw + 12), my, mw, mh}, ms[i].l, ms[i].v, ms[i].u, ms[i].c, i < 4 ? 64 : 44);
    float y = my + mh + 14;
    // second row: W/kg, avg/np, kJ/kcal, TSS, ghost, lap
    std::snprintf(b, sizeof b, "%.2f W/kg   \xC2\xB7   avg %.0f W   \xC2\xB7   NP %.0f W   \xC2\xB7   %.0f kJ  \xC2\xB7  %.0f kcal   \xC2\xB7   TSS %.0f   \xC2\xB7   +%.0f m",
                  p3 / prof_.weightKg, e.avgPower(), e.np(), e.workKj(), e.calories(), e.tss(), e.gain());
    text(b, pad, y, F::Body, DIM, 18);
    if (auto gap = e.ghostGapS()) {
        std::snprintf(b, sizeof b, "Ghost %s%s", *gap >= 0 ? "+" : "-", fmtTime(std::fabs(*gap)).c_str());
        textRight(b, W - pad, y, F::Body, *gap >= 0 ? GOOD : BAD, 18);
    }
    y += 34;
    float rest = H - y - pad;
    // step panel / route
    if (e.structured()) {
        drawStepPanel({pad, y, W * 0.42f, 92});
        Rectangle wc{pad + W * 0.42f + 16, y, W - 2 * pad - W * 0.42f - 16, 92};
        panel(wc);
        workoutChart({wc.x + 12, wc.y + 10, wc.width - 24, wc.height - 20}, e.steps(), prof_.ftp, e.ergBiasValue(), e.workoutT(), &e.samples(),
                     &e.workoutTSamples());
        y += 104;
        rest -= 104;
    }
    float h1 = rest * 0.42f;
    Rectangle rp{pad, y, W - 2 * pad, h1};
    panel(rp);
    text("ROUTE", rp.x + 14, rp.y + 8, F::Small, DIM, 13);
    std::snprintf(b, sizeof b, "%s / %s   lap %d", fmtDist(e.lapDistance(), metric).c_str(), fmtDist(e.profileData().length, metric).c_str(), e.lapNumber());
    textRight(b, rp.x + rp.width - 14, rp.y + 8, F::Small, DIM, 13);
    routeProfile({rp.x + 14, rp.y + 28, rp.width - 28, rp.height - 38}, e.profileData(), e.lapDistance(),
                 e.ghostDistance() ? std::fmod(*e.ghostDistance(), e.profileData().length) : -1, metric);
    y += h1 + 12;
    float h2 = rest - h1 - 12;
    Rectangle up{pad, y, W * 0.36f, h2};
    panel(up);
    text("NEXT 2 KM", up.x + 14, up.y + 8, F::Small, DIM, 13);
    int ci = e.currentClimb();
    if (ci >= 0) {
        const Climb& c = e.profileData().climbs[ci];
        std::snprintf(b, sizeof b, "%s to the top", fmtDist(c.endD - e.lapDistance(), metric).c_str());
        textRight(b, up.x + up.width - 14, up.y + 8, F::Small, WARN, 13);
    }
    upcomingProfile({up.x + 14, up.y + 28, up.width - 28, up.height - 38}, e.profileData(), e.lapDistance(), 2000, metric);
    Rectangle pc{up.x + up.width + 12, y, W - 2 * pad - up.width - 12, h2};
    panel(pc);
    text("POWER  \xC2\xB7  HEART RATE  \xC2\xB7  CADENCE   (5 min)", pc.x + 14, pc.y + 8, F::Small, DIM, 13);
    powerChart({pc.x + 14, pc.y + 28, pc.width - 28, pc.height - 38}, e.samples(), prof_.ftp, prof_.maxHr, 300);
    if (!e.started()) {
        Rectangle r{W / 2 - 240, H * 0.45f, 480, 64};
        panel(r, PANEL);
        textCentered("Start pedalling to begin", r, F::Title, TEXT, 28);
    }
}

// ------------------------------------------------------------------ edit drawer
void App::drawEditDrawer() {
    RideEngine& e = *eng_;
    const float W = ui::screenW(), H = ui::screenH();
    Rectangle r{W - 460, 0, 460, H};
    DrawRectangleRec(r, PANEL);
    DrawLineEx({r.x, 0}, {r.x, H}, 1, LINE);
    float x = r.x + 20, y = 18, w = r.width - 40;
    text("Edit ride", x, y, F::Title, TEXT, 28);
    if (button({r.x + r.width - 56, y, 36, 32}, "x")) drawerOpen_ = false;
    y += 48;
    static const std::vector<std::string> tabs = {"Intervals", "Ride", "Road ahead", "Insert"};
    int t = segmented({x, y, w, 36}, tabs, drawerTab_);
    if (t >= 0) drawerTab_ = t;
    y += 52;
    char b[160];
    auto btn = [&](const char* label, bool enabled = true) {
        bool c = button({x, y, w, 40}, label, false, enabled, 17);
        y += 48;
        return c;
    };
    auto pair = [&](const char* a, const char* bb, bool en, int& which) {
        which = 0;
        if (button({x, y, w / 2 - 5, 40}, a, false, en, 17)) which = 1;
        if (button({x + w / 2 + 5, y, w / 2 - 5, 40}, bb, false, en, 17)) which = 2;
        y += 48;
    };
    int k;
    if (drawerTab_ == 0) {
        bool on = e.structuredActive() && e.currentStep().has_value();
        if (!e.structured()) {
            textWrapped("This ride has no intervals. Use Insert to add a set - ERG takes over for it, then hands back.", {x, y, w, 60}, F::Body, DIM, 16);
            y += 70;
        } else if (auto cs = e.currentStep()) {
            text("Current: " + e.steps()[cs->first].label, x, y, F::Body, TEXT, 17);
            y += 32;
        }
        pair("Step -1 min", "Step +1 min", on, k);
        if (k == 1) e.extendStep(-60); else if (k == 2) e.extendStep(60);
        pair("Step -30 s", "Step +30 s", on, k);
        if (k == 1) e.extendStep(-30); else if (k == 2) e.extendStep(30);
        pair("Target -5%", "Target +5%", on, k);
        if (k == 1) e.adjustStep(-5); else if (k == 2) e.adjustStep(5);
        std::snprintf(b, sizeof b, "Intensity %.0f%%", e.ergBiasValue() * 100);
        text(b, x, y + 4, F::Body, DIM, 16);
        y += 30;
        pair("Intensity -1%", "Intensity +1%", e.structured(), k);
        if (k == 1) e.ergBias(-1); else if (k == 2) e.ergBias(1);
        if (btn("One more rep", on)) e.oneMoreRep();
        if (btn("Skip this step  (Right arrow)", on)) e.skipStep();
        if (btn("Skip to cool-down", on)) e.skipToCooldown();
        if (btn(e.ergDisabled() ? "Switch back to ERG" : "Switch to resistance mode", e.structured())) e.toggleErg();
        if (btn("End workout here", on)) e.endWorkout();
    } else if (drawerTab_ == 1) {
        if (e.setup().kind == RideKind::Free) {
            text("Free-ride mode", x, y, F::Small, DIM);
            y += 20;
            int m = segmented({x, y, w, 36}, {"Gradient", "Power", "Level", "HR"}, int(e.freeMode()));
            if (m >= 0) e.setFreeMode(FreeMode(m));
            y += 48;
            pair("Target -", "Target +", true, k);
            if (k) e.nudge(k == 1 ? -1 : 1);
        }
        text("Goal", x, y, F::Small, DIM);
        y += 20;
        int g = segmented({x, y, w, 34}, {"None", "Time", "Dist", "kJ", "kcal", "Climb"}, int(e.goal().type));
        if (g >= 0) {
            static const double defaults[] = {0, 3600, 30000, 600, 600, 500};
            Goal ng; ng.type = Goal::Type(g); ng.value = defaults[g];
            e.setGoal(ng);
        }
        y += 44;
        if (e.goal().type != Goal::None) {
            static const double steps[] = {0, 300, 5000, 50, 50, 50};
            pair("Goal -", "Goal +", true, k);
            if (k) { Goal ng = e.goal(); ng.value = std::max(steps[ng.type], ng.value + (k == 1 ? -1 : 1) * steps[ng.type]); e.setGoal(ng); }
        }
        std::snprintf(b, sizeof b, "Trainer difficulty %.0f%%", e.difficultyValue() * 100);
        text(b, x, y + 4, F::Body, DIM, 16);
        y += 30;
        pair("Difficulty -10%", "Difficulty +10%", true, k);
        if (k) e.difficulty(k == 1 ? -10 : 10);
        if (e.setup().kind == RideKind::Route) {
            std::snprintf(b, sizeof b, "Laps: %d", e.laps());
            text(b, x, y + 4, F::Body, DIM, 16);
            y += 30;
            pair("Remove lap", "Add lap", true, k);
            if (k) e.addLaps(k == 1 ? -1 : 1);
        }
        if (btn("Record a lap  (L)")) e.lap();
        if (prof_.virtualShifting) {
            std::snprintf(b, sizeof b, "Gear %d / 24  (ratio %.2f)", e.gear(), e.gearRatio());
            text(b, x, y + 4, F::Body, DIM, 16);
            y += 30;
            pair("Shift down  [", "Shift up  ]", true, k);
            if (k) e.shift(k == 1 ? -1 : 1);
        }
    } else if (drawerTab_ == 2) {
        drawRoadAhead(x, y, w);
    } else {
        textWrapped("Add intervals to this ride in the text format, e.g.  5x 1m @120% / 1m @50%  or  3x (30s @150%, 30s @50%).", {x, y, w, 60},
                    F::Small, DIM, 15);
        y += 56;
        textBox({x, y, w, 120}, insertText_, true, "5x 1m @120% / 1m @50%");
        y += 130;
        std::vector<wk::Block> blocks;
        bool ok = !insertText_.text.empty() && wk::parseText(insertText_.text, prof_.ftp, blocks, insertErr_);
        if (ok) {
            auto st = wk::blocksToSteps(blocks);
            workoutChart({x, y, w, 70}, st, prof_.ftp, 1.0, -1, nullptr);
            y += 80;
            text(fmtTime(st.back().endS()) + "  \xC2\xB7  " + std::to_string(st.size()) + " steps", x, y, F::Small, TEXT);
            y += 26;
        } else if (!insertText_.text.empty()) {
            textWrapped(insertErr_, {x, y, w, 40}, F::Small, BAD, 14);
            y += 40;
        }
        if (button({x, y, w / 2 - 5, 42}, "Insert next", true, ok)) { e.insertBlocks(blocks, false); insertText_.text.clear(); }
        if (button({x + w / 2 + 5, y, w / 2 - 5, 42}, "Add at end", false, ok)) { e.insertBlocks(blocks, true); insertText_.text.clear(); }
        y += 56;
        text("Quick sets", x, y, F::Small, DIM);
        y += 22;
        const char* quick[][2] = {{"5 x 30/30 @130%", "5x 30s @130% / 30s @50%"}, {"3 x 3 min VO2", "3x 3m @118% / 3m @50%"},
                                  {"2 x 8 min threshold", "2x 8m @100% / 4m @55%"}, {"10 min sweet spot", "10m @90%"}, {"Sprint x 3", "3x (10s effort @2%, 2m @50%)"}};
        for (auto& q : quick) {
            if (button({x, y, w, 36}, q[0], false, true, 16)) {
                std::vector<wk::Block> qb;
                std::string err;
                if (wk::parseText(q[1], prof_.ftp, qb, err)) e.insertBlocks(qb, false);
            }
            y += 42;
        }
    }
    // block clicks to the HUD beneath
    if (CheckCollisionPointRec(ui::mouseLogical(), r)) ui::setInputBlocked(true);
}

void App::drawPauseMenu() {
    const float W = ui::screenW(), H = ui::screenH();
    DrawRectangle(0, 0, int(W), int(H), Color{8, 12, 20, 70});
    // on a generated map the world can be kept: seed, size, climate (everything that makes it) and where this ride began
    const bool genWorld = map_ && eng_->route().id.rfind("map-", 0) == 0;
    const float extra = genWorld ? 62.f : 0.f;
    Rectangle r{W / 2 - 220, H / 2 - 190 - extra / 2, 440, 380 + extra};
    panel(r, PANEL);
    textCentered("Paused", {r.x, r.y + 12, r.width, 50}, F::Title, TEXT);
    textCentered(fmtTime(eng_->elapsedS()) + "  \xC2\xB7  " + fmtDist(eng_->distance(), prof_.metric), {r.x, r.y + 58, r.width, 24}, F::Body, DIM);
    float y = r.y + 100;
    if (button({r.x + 40, y, r.width - 80, 50}, "Resume", true)) { pauseMenu_ = false; eng_->resume(); }
    y += 62;
    if (button({r.x + 40, y, r.width - 80, 50}, view3d_ ? "Switch to 2D dashboard" : "Switch to 3D world")) view3d_ = !view3d_;
    y += 62;
    if (genWorld) {
        const int km = int(std::lround(map_->params.size / 1024.f));
        const store::SavedWorld* sw = savedWorldFor(map_->params.seed, km, map_->params.climate);
        if (sw) button({r.x + 40, y, r.width - 80, 50}, "Seed " + std::to_string(map_->params.seed) + " is in your saved list", false, false, 16);
        else if (button({r.x + 40, y, r.width - 80, 50}, "Add seed to saved list") && saveWorld(*map_, rideMapStart_))
            toast("Saved: Free ride > Saved worlds starts you right here", GOOD);
        y += 62;
    }
    if (button({r.x + 40, y, r.width - 80, 50}, "End ride & save")) { pauseMenu_ = false; endRide(true); }
    y += 62;
    if (button({r.x + 40, y, r.width - 80, 50}, "Discard ride")) { pauseMenu_ = false; endRide(false); }
}

void App::drawHelp() {
    const float W = ui::screenW(), H = ui::screenH();
    Rectangle r{W / 2 - 340, H / 2 - 318, 680, 636};
    panel(r, PANEL);
    text("Keys", r.x + 24, r.y + 16, F::Title, TEXT);
    const char* rows[][2] = {{"Space", "Pause / resume"}, {"L", "Record a lap"}, {"E", "Edit drawer (intervals, goal, laps, insert sets)"},
                             {"Up / Down", "Nudge the target; endless road: climb per km ahead"}, {"G", "Endless road: hilliness ahead"}, {"Right", "Skip to the next workout step"},
                             {"[ ]  PgUp / PgDn", "Virtual gears (Zwift Cog)"}, {"H", "Heart-rate alerts on / off"}, {"V", "2D dashboard / 3D world"}, {"B", "Greybox body / kitted rider"},
                             {"R", "Change the road ahead: add a climb, descent, sprint..."}, {"Backspace", "Undo the last road change"},
                             {"C", "Camera: chase, cinematic, side, rider, drone, close, eyes"}, {"T / W / M", "Time of day / weather / sound"}, {"D", "Simulator test data"},
                             {"Shift+Up/Down", "Simulator power +-10 W"}, {"Alt+Up/Down", "Simulator heart rate +-2 bpm"}, {"Tab", "Hide the HUD"},
                             {"F / P", "FPS counter / frame-time breakdown"}, {"N / Shift+N", "Minimap zoom / heading-up or north-up"}, {"Esc", "Menu"}};
    float y = r.y + 70;
    for (auto& rw : rows) {
        text(rw[0], r.x + 30, y, F::Body, ACCENT, 17);
        text(rw[1], r.x + 220, y, F::Body, TEXT, 17);
        y += 27;
    }
}

// ------------------------------------------------------------------ minimap (generated worlds)
void App::drawMiniMap(Vector2 centre, float radius, float x, float z, float heading, double dist) {
    static const float RANGES[4] = {600, 1200, 2500, 5000};
    const int zi = std::clamp(prefs_.miniMapZoom, 0, 3);
    MiniMap::View v;
    v.centre = centre;
    v.radius = radius;
    v.rangeM = RANGES[zi];
    v.x = x; v.z = z;
    v.heading = heading;
    v.northUp = prefs_.miniMapNorthUp;
    // the road ahead, as far as the rim (only what is laid out: a roam grows as you choose)
    if (course_ && dist >= 0) {
        const double step = std::max(10.0, v.rangeM / 90.0);
        const double until = course_->loop() ? dist + v.rangeM * 2.5 : std::min(dist + v.rangeM * 2.5, double(course_->length()));
        for (double d = dist; d <= until; d += step) {
            auto q = course_->at(d);
            v.ahead.push_back({q.pos.x, q.pos.z});
        }
    }
    if (miniMap_.draw(v, GetFrameTime())) {   // click: next zoom
        prefs_.miniMapZoom = (zi + 1) % 4;
        store::save(prof_, prefs_);
    }
}

// ------------------------------------------------------------------ performance (F / P)
void App::drawPerf() {
    const float W = ui::screenW(), H = ui::screenH();
    static float fpsS = 60, msS = 16.7f;
    float dt = GetFrameTime();
    if (dt > 0) { msS += (dt * 1000.f - msS) * 0.08f; fpsS = 1000.f / std::max(msS, 0.01f); }
    const prof::Stats& st = prof::stats();
    const bool detail = prefs_.showPerf;
    const float w = detail ? 250.f : 120.f;
    const float rowH = 20.f;
    const float h = detail ? 64.f + rowH * prof::COUNT + 30.f : 46.f;
    float right = W - 16.f;
    if (testPanel_ && simActive()) right = W - 400 - 12.f;   // stay clear of the test-data panel
    Rectangle r{right - w, std::round(H * 0.5f - h * 0.5f), w, h};
    panel(r, withAlpha(PANEL, 236), 0.08f);
    char b[64];
    Color fc = fpsS >= 55 ? GOOD : fpsS >= 30 ? WARN : BAD;
    std::snprintf(b, sizeof b, "%.0f", fpsS);
    text(b, r.x + 14, r.y + 8, F::Title, fc, 24);
    float fw = measure(b, F::Title, 24).x;
    std::snprintf(b, sizeof b, "fps  %.1f ms", msS);
    text(b, r.x + 20 + fw, r.y + 16, F::Small, DIM, 13);
    if (!detail) return;
    // per category: GPU time (timestamp queries) where available
    std::snprintf(b, sizeof b, "Total  %.1f ms   3D %dx%d (%.0f%%)", st.gpuMs, renderer_.renderWidth(), renderer_.renderHeight(), 100.f * renderer_.renderWidth() / std::max(1, GetRenderWidth()));
    text(b, r.x + 14, r.y + 42, F::Small, DIM, 13);
    const float scale = std::max(msS, 4.f);   // bars relative to the frame
    static const Color cols[prof::COUNT] = {
        {148, 163, 184, 255}, {100, 116, 139, 255}, {56, 189, 248, 255}, {132, 204, 22, 255}, {71, 85, 105, 255},
        {22, 163, 74, 255}, {163, 230, 53, 255}, {234, 179, 8, 255}, {14, 165, 233, 255}, {167, 139, 250, 255},
        {244, 114, 182, 255}, {251, 146, 60, 255}, {239, 68, 68, 255}};
    float y = r.y + 64;
    for (int i = 0; i < prof::COUNT; ++i) {
        float ms = st.ms[i];
        text(prof::name(prof::Section(i)), r.x + 14, y + 2, F::Small, TEXT, 13);
        std::snprintf(b, sizeof b, "%.2f", ms);
        textRight(b, r.x + w - 14, y + 2, F::Small, ms > 4.f ? BAD : ms > 2.f ? WARN : DIM, 13);
        float bw = std::clamp(ms / scale, 0.f, 1.f) * (w - 28);
        DrawRectangleRec({r.x + 14, y + 16, w - 28, 2}, ink(14));
        if (bw > 0.5f) DrawRectangleRec({r.x + 14, y + 16, bw, 2}, cols[i]);
        y += rowH;
    }
    text(std::string("Timing: ") + st.method, r.x + 14, y + 8, F::Small, FAINT, 11);
}

// ------------------------------------------------------------------ sectors (road ahead)
void App::drawSectorBanner(float x, float& y, float w) {
    const RideEngine& e = *eng_;
    char b[200];
    for (auto& s : e.sectors()) {
        if (s.done && (!s.result || e.distance() > s.endTotal + 400)) continue;
        Rectangle r{x, y, w, 36};
        panel(r);
        double d = e.distance();
        if (d < s.startTotal) {
            std::snprintf(b, sizeof b, "%s in %s", s.name.c_str(), fmtDist(s.startTotal - d, prof_.metric).c_str());
            text(b, r.x + 14, r.y + 9, F::Small, SKY, 14);
        } else if (!s.done) {
            double prog = (d - s.startTotal) / std::max(1.0, s.endTotal - s.startTotal);
            std::snprintf(b, sizeof b, "%s  \xC2\xB7  %s to go%s", s.name.c_str(), fmtDist(s.endTotal - d, prof_.metric).c_str(), s.timed ? "  \xC2\xB7  timed" : "");
            text(b, r.x + 14, r.y + 9, F::Small, WARN, 14);
            progressBar({r.x + r.width - 150, r.y + 15, 136, 6}, float(prog), WARN);
        } else if (s.result) {
            std::snprintf(b, sizeof b, "%s done  \xC2\xB7  %s  \xC2\xB7  %.0f W", s.name.c_str(), fmtTime(s.result->durationS).c_str(), s.result->avgPower);
            text(b, r.x + 14, r.y + 9, F::Small, GOOD, 14);
        }
        y += 44;
    }
}

void App::drawRoadAhead(float x, float& y, float w) {
    RideEngine& e = *eng_;
    char b[200];
    int d;
    textWrapped("Splice something into the road in front of you. The 3D road is re-laid ahead and the trainer follows the new gradient.", {x, y, w, 50}, F::Small, DIM, 14);
    y += 46;
    static const char* kinds[] = {"Climb", "Descent", "Flat", "Rollers", "Sprint", "Terrain"};
    static const SectorKind kk[] = {SectorKind::Climb, SectorKind::Descent, SectorKind::Flat, SectorKind::Rollers, SectorKind::Sprint, SectorKind::Terrain};
    float bw = (w - 2 * 8) / 3;
    for (int i = 0; i < 6; ++i)
        if (chipButton({x + (i % 3) * (bw + 8), y + (i / 3) * 38, bw, 32}, kinds[i], sector_.kind == kk[i])) {
            sector_.kind = kk[i];
            if (kk[i] == SectorKind::Climb) { sector_.gradePct = std::max(2.0, std::fabs(sector_.gradePct)); }
            if (kk[i] == SectorKind::Descent) { sector_.gradePct = -std::max(2.0, std::fabs(sector_.gradePct)); }
            if (kk[i] == SectorKind::Sprint) sector_.lengthM = 300;
            if (kk[i] == SectorKind::Terrain && !sector_.terrain) sector_.terrain = Terrain::Hilly;
        }
    y += 84;
    double step = sector_.lengthM >= 2000 ? 500 : sector_.lengthM >= 600 ? 200 : 100;
    if (stepper({x, y, w, 34}, "Length", fmtDist(sector_.lengthM, prof_.metric), &d, 120)) sector_.lengthM = std::clamp(sector_.lengthM + d * step, 100.0, 30000.0);
    y += 42;
    if (sector_.kind == SectorKind::Climb || sector_.kind == SectorKind::Descent) {
        std::snprintf(b, sizeof b, "%+.1f%%", sector_.gradePct);
        if (stepper({x, y, w, 34}, "Gradient", b, &d, 120)) {
            double g = std::fabs(sector_.gradePct) + d * 0.5;
            g = std::clamp(g, 1.0, 20.0);
            sector_.gradePct = sector_.kind == SectorKind::Descent ? -g : g;
        }
        y += 42;
    } else if (sector_.kind == SectorKind::Terrain) {
        int t = segmented({x, y, w, 32}, {"Rolling", "Hilly", "Mountain", "Alpine"}, std::max(0, int(sector_.terrain.value_or(Terrain::Hilly)) - 1));
        if (t >= 0) sector_.terrain = Terrain(t + 1);
        y += 42;
    }
    if (stepper({x, y, w, 34}, "Starts in", fmtDist(sector_.inM, prof_.metric), &d, 120)) sector_.inM = std::clamp(sector_.inM + d * 100.0, 100.0, 5000.0);
    y += 42;
    if (toggle({x, y, w, 32}, "Return to the original height afterwards", sector_.rejoin)) sector_.rejoin = !sector_.rejoin;
    y += 40;
    if (toggle({x, y, w, 32}, "Time it (shows as a split)", sector_.timed)) sector_.timed = !sector_.timed;
    y += 44;
    auto segs = sector_.build(1);
    auto [len, net] = segmentsSummary(segs);
    std::snprintf(b, sizeof b, "%s  \xC2\xB7  %s  \xC2\xB7  %+.0f m", sector_.displayName().c_str(), fmtDist(len, prof_.metric).c_str(), net);
    text(b, x, y, F::Small, TEXT, 14);
    y += 26;
    if (button({x, y, w, 44}, "Add to the road", true)) {
        e.addSector(sector_);
        toast(sector_.displayName() + " added ahead", SKY);
    }
    y += 54;
    if (button({x, y, w, 38}, "Undo last change  (Backspace)", false, e.canUndoSector(), 15)) { e.undoSector(); toast("Road change undone"); }
    y += 52;
    // quick picks
    text("Quick picks", x, y, F::Small, DIM);
    y += 22;
    struct Q { const char* l; SectorKind k; double len, g; };
    Q qs[] = {{"1 km @ 8% kicker", SectorKind::Climb, 1000, 8}, {"5 km @ 5% climb", SectorKind::Climb, 5000, 5}, {"300 m sprint", SectorKind::Sprint, 300, 0},
              {"2 km rollers", SectorKind::Rollers, 2000, 0}};
    for (auto& q : qs) {
        if (button({x, y, w, 34}, q.l, false, true, 14)) {
            SectorSpec sp;
            sp.kind = q.k; sp.lengthM = q.len; sp.gradePct = q.g; sp.inM = 300;
            e.addSector(sp);
            toast(sp.displayName() + " added ahead", SKY);
        }
        y += 40;
    }
    // upcoming profile with the change
    if (y + 90 < ui::screenH()) {
        text("NEXT 3 KM", x, y + 6, F::Small, DIM, 12);
        upcomingProfile({x, y + 24, w, 70}, e.profileData(), e.lapDistance(), 3000, prof_.metric);
    }
}

// ------------------------------------------------------------------ weather card: now + forecast
float App::weatherCardH() const { return prefs_.weatherMode == 2 ? 84.f : 128.f; }   // as tall as the ride card

void App::drawWeatherCard(Rectangle r) {
    using weather::Kind;
    const bool metric = prof_.metric;
    const bool night = sky_.tod > 0.8f;
    glass(r);
    Kind now = wx_.currentKind(wxElapsed_);
    auto tempStr = [&](double c) { return fmt("%.0f\xC2\xB0", metric ? c : c * 9 / 5 + 32); };
    auto tempFor = [&](Kind k) { return weather::temperature(wxSeasonC_, wxSunEle_, k, wxEleForTemp_); };
    // now: icon, temperature, condition; wind and the road underneath
    weatherIcon(now, {r.x + 34, r.y + 34}, 44, night);
    std::string t = tempStr(wxTempC_);
    text(t, r.x + 66, r.y + 10, F::Big, HUD_INK, 30);
    float tx = r.x + 66 + measure(t, F::Big, 30).x + 10;
    text(weather::kindLabel(now), tx, r.y + 12, F::Body, HUD_INK, 16);
    const char* modeTag = prefs_.weatherMode == 0 ? "Changing" : prefs_.weatherMode == 1 ? "Live" : "Fixed";
    textRight(modeTag, r.x + r.width - 14, r.y + 14, F::Small, HUD_MUTED, 11);
    std::string road = wx_.snowCover > 0.3 ? "snow on the road" : wx_.wet > 0.4 ? "wet road" : wx_.wet > 0.15 ? "damp road" : "dry road";
    std::string wind = fmt(metric ? "Wind %.0f km/h " : "Wind %.0f mph ", metric ? wxWindKmh_ : wxWindKmh_ * 0.621371) + compass8(wx_.windDir);
    text(wind + "  \xC2\xB7  " + road, tx, r.y + 34, F::Small, HUD_MUTED, 12);
    if (prefs_.weatherMode == 1 && live_) textRight(live_->place, r.x + r.width - 14, r.y + 30, F::Small, HUD_MUTED, 11);
    if (prefs_.weatherMode == 2) return;   // fixed: nothing will change
    // forecast: the next spells (changing weather) or the next hours (live)
    DrawLineEx({r.x + 14, r.y + 62}, {r.x + r.width - 14, r.y + 62}, 1, hudInk(22));
    struct Slot { std::string when; Kind k; double temp; };
    std::vector<Slot> slots;
    if (prefs_.weatherMode == 1 && live_ && !live_->hourly.empty()) {
        for (const auto& h : live_->hourly) slots.push_back({fmt("+%.0f h", h.inHours), h.kind, h.tempC});
    } else {
        const double ahead[3] = {15 * 60, 30 * 60, 60 * 60};
        const char* lbl[3] = {"in 15 min", "in 30 min", "in 1 h"};
        for (int i = 0; i < 3; ++i) { Kind k = wx_.kindIn(wxElapsed_, ahead[i]); slots.push_back({lbl[i], k, tempFor(k)}); }
    }
    float colW = (r.width - 28) / float(slots.size());
    for (size_t i = 0; i < slots.size(); ++i) {
        float cx = r.x + 14 + colW * (i + 0.5f);
        // two lines per slot: when, then icon + temperature
        textCentered(slots[i].when, {cx - colW / 2, r.y + 68, colW, 14}, F::Small, HUD_MUTED, 11);
        std::string ts = tempStr(slots[i].temp);
        float tw = measure(ts, F::Small, 15).x, gw = 26 + 6 + tw;
        weatherIcon(slots[i].k, {cx - gw / 2 + 13, r.y + 101}, 26, night);
        text(ts, cx - gw / 2 + 32, r.y + 92, F::Small, HUD_INK, 15);
        if (i + 1 < slots.size()) DrawLineEx({r.x + 14 + colW * (i + 1), r.y + 70}, {r.x + 14 + colW * (i + 1), r.y + 116}, 1, hudInk(16));
    }
}
