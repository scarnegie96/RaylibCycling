#include "Ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "raymath.h"

namespace ui {

// CoreKick's light, data-forward palette
const Color BG{243, 245, 248, 255};        // --bg
const Color PANEL{255, 255, 255, 255};     // --card
const Color PANEL2{246, 248, 251, 255};    // --card-2
const Color PANEL_HI{234, 237, 242, 255};  // --card-3
const Color LINE{15, 23, 42, 20};          // --line
const Color TEXT{17, 24, 39, 255};         // --text
const Color DIM{91, 100, 117, 255};        // --muted
const Color FAINT{139, 148, 165, 255};     // --muted-2
const Color ACCENT{15, 157, 88, 255};      // --accent
const Color GOOD{15, 157, 88, 255};
const Color WARN{201, 130, 0, 255};
const Color BAD{220, 61, 67, 255};
const Color HR_RED{229, 70, 107, 255};
const Color SKY{47, 127, 216, 255};
const Color GHOST{114, 89, 232, 255};
const Color INK{31, 36, 48, 255};  // the 3D HUD ink

static Font g_fonts[4];
static Font g_symbols;  // DejaVu Sans: stars, ticks, arrows that Barlow doesn't have
static bool g_haveSymbols = false;
static const int SYMBOLS[] = {0x2605, 0x2606, 0x2713, 0x270E, 0x2191, 0x2193, 0x2192, 0x2190, 0x21BB, 0x25CF, 0x25B6, 0x2600, 0x263E, 0x2601, 0x2602};
static float g_sizes[4] = {92, 30, 17, 13};
static bool g_loaded = false;
static bool g_blocked = false;
static bool g_textActive = false;

static Font loadOne(const char* file, int px) {
    std::string paths[] = {std::string(GetApplicationDirectory()) + "assets/fonts/" + file, std::string("assets/fonts/") + file,
                           std::string(GetApplicationDirectory()) + "../Resources/assets/fonts/" + file};
    for (auto& p : paths)
        if (FileExists(p.c_str())) {
            // include a few extra glyphs used in labels (·, ×, →, ø, °, –)
            std::vector<int> cps;
            for (int c = 32; c < 127; ++c) cps.push_back(c);
            for (int c : {0xB7, 0xD7, 0x2192, 0xF8, 0xB0, 0x2013, 0x2014, 0x2191, 0x2193, 0x201C, 0x201D, 0x2026}) cps.push_back(c);
            Font f = LoadFontEx(p.c_str(), px, cps.data(), int(cps.size()));
            GenTextureMipmaps(&f.texture);
            SetTextureFilter(f.texture, TEXTURE_FILTER_TRILINEAR);
            return f;
        }
    return GetFontDefault();
}

void loadFonts() {
    // Inter: the open sibling of CoreKick's system font
    g_fonts[0] = loadOne("InterDisplay-ExtraBold.ttf", 160);
    g_fonts[1] = loadOne("InterDisplay-Bold.ttf", 96);
    g_fonts[2] = loadOne("Inter-Medium.ttf", 64);
    g_fonts[3] = loadOne("Inter-SemiBold.ttf", 48);
    std::string paths[] = {std::string(GetApplicationDirectory()) + "assets/fonts/DejaVuSans.ttf", "assets/fonts/DejaVuSans.ttf",
                           std::string(GetApplicationDirectory()) + "../Resources/assets/fonts/DejaVuSans.ttf"};
    for (auto& p : paths)
        if (FileExists(p.c_str())) {
            std::vector<int> cps(std::begin(SYMBOLS), std::end(SYMBOLS));
            g_symbols = LoadFontEx(p.c_str(), 64, cps.data(), int(cps.size()));
            GenTextureMipmaps(&g_symbols.texture);
            SetTextureFilter(g_symbols.texture, TEXTURE_FILTER_TRILINEAR);
            g_haveSymbols = true;
            break;
        }
    g_loaded = true;
}
void unloadFonts() {
    if (!g_loaded) return;
    for (auto& f : g_fonts) if (f.texture.id != GetFontDefault().texture.id) UnloadFont(f);
    if (g_haveSymbols) UnloadFont(g_symbols);
    g_haveSymbols = false;
    g_loaded = false;
}
Font& font(F f) { return g_fonts[int(f)]; }
float fontSize(F f) { return g_sizes[int(f)]; }

static bool isSymbol(int cp) {
    for (int s : SYMBOLS) if (s == cp) return true;
    return false;
}
static bool hasSymbols(const std::string& s) {
    for (size_t i = 0; i + 2 < s.size(); ++i) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xE2) {
            int sz;
            if (isSymbol(GetCodepoint(s.c_str() + i, &sz))) return true;
        }
    }
    return false;
}
// walks the string in runs: plain runs use the UI font, symbols use DejaVu
template <class Fn>
static void runs(const std::string& s, Fn fn) {
    size_t i = 0, start = 0;
    while (i < s.size()) {
        int sz = 1;
        int cp = GetCodepoint(s.c_str() + i, &sz);
        if (isSymbol(cp)) {
            if (i > start) fn(s.substr(start, i - start), false);
            fn(s.substr(i, size_t(sz)), true);
            start = i + size_t(sz);
        }
        i += size_t(std::max(1, sz));
    }
    if (start < s.size()) fn(s.substr(start), false);
}
Vector2 measure(const std::string& s, F f, float size) {
    float sz = size > 0 ? size : fontSize(f);
    if (!g_haveSymbols || !hasSymbols(s)) return MeasureTextEx(font(f), s.c_str(), sz, sz * -0.01f);
    Vector2 m{0, sz};
    runs(s, [&](const std::string& r, bool sym) {
        Vector2 k = MeasureTextEx(sym ? g_symbols : font(f), r.c_str(), sym ? sz * 0.9f : sz, sz * -0.01f);
        m.x += k.x + sz * -0.01f;
        m.y = std::max(m.y, k.y);
    });
    return m;
}
void text(const std::string& s, float x, float y, F f, Color c, float size) {
    float sz = size > 0 ? size : fontSize(f);
    if (!g_haveSymbols || !hasSymbols(s)) { DrawTextEx(font(f), s.c_str(), {std::round(x), std::round(y)}, sz, sz * -0.01f, c); return; }
    float cx = x;
    runs(s, [&](const std::string& r, bool sym) {
        float rs = sym ? sz * 0.9f : sz;
        DrawTextEx(sym ? g_symbols : font(f), r.c_str(), {std::round(cx), std::round(y + (sz - rs) * 0.45f)}, rs, sz * -0.01f, c);
        cx += MeasureTextEx(sym ? g_symbols : font(f), r.c_str(), rs, sz * -0.01f).x + sz * -0.01f;
    });
}
void textCentered(const std::string& s, Rectangle r, F f, Color c, float size) {
    Vector2 m = measure(s, f, size);
    text(s, r.x + (r.width - m.x) / 2, r.y + (r.height - m.y) / 2, f, c, size);
}
void textRight(const std::string& s, float right, float y, F f, Color c, float size) { text(s, right - measure(s, f, size).x, y, f, c, size); }

float textWrapped(const std::string& s, Rectangle r, F f, Color c, float size) {
    float sz = size > 0 ? size : fontSize(f);
    float y = r.y, lh = sz * 1.3f;
    std::string line, word;
    auto flush = [&] { if (!line.empty()) { text(line, r.x, y, f, c, sz); y += lh; line.clear(); } };
    for (size_t i = 0; i <= s.size(); ++i) {
        char ch = i < s.size() ? s[i] : ' ';
        if (ch == ' ' || ch == '\n') {
            std::string cand = line.empty() ? word : line + " " + word;
            if (!word.empty() && measure(cand, f, sz).x > r.width) { flush(); line = word; }
            else line = cand;
            word.clear();
            if (ch == '\n') flush();
        } else word += ch;
    }
    flush();
    return y - r.y;
}

void beginFrame() { g_blocked = false; g_textActive = false; }
void setInputBlocked(bool b) { g_blocked = b; }
bool mouseFree() { return !g_blocked; }
bool anyTextActive() { return g_textActive; }

// a CoreKick card: white, hairline border, soft shadow
void panel(Rectangle r, Color c, float round) {
    float rr = std::min(1.f, round * 48.f / std::max(1.f, std::min(r.width, r.height)));
    if (c.a > 200) {
        DrawRectangleRounded({r.x, r.y + 2, r.width, r.height}, rr, 8, Color{15, 23, 42, 8});
        DrawRectangleRounded({r.x - 1, r.y + 1, r.width + 2, r.height + 3}, rr, 8, Color{15, 23, 42, 5});
    }
    DrawRectangleRounded(r, rr, 8, c);
    DrawRectangleRoundedLinesEx(r, rr, 8, 1.f, LINE);
}

bool hovered(Rectangle r) { return !g_blocked && CheckCollisionPointRec(GetMousePosition(), r); }
bool clicked(Rectangle r) { return hovered(r) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT); }

bool button(Rectangle r, const std::string& label, bool primary, bool enabled, float size) {
    bool h = enabled && hovered(r);
    float rr = std::min(1.f, 20.f / std::max(1.f, r.height));
    if (primary) {
        if (enabled) DrawRectangleRounded({r.x, r.y + 3, r.width, r.height}, rr, 8, Color{15, 157, 88, 40});
        DrawRectangleRounded(r, rr, 8, !enabled ? Color{15, 157, 88, 110} : h ? Color{11, 128, 72, 255} : ACCENT);
    } else {
        DrawRectangleRounded(r, rr, 8, h ? PANEL_HI : PANEL);
        DrawRectangleRoundedLinesEx(r, rr, 8, 1.f, Color{15, 23, 42, 38});
    }
    float sz = size > 0 ? size : std::min(fontSize(F::Small) + 1, r.height * 0.42f);
    Color tc = primary ? WHITE : TEXT;
    if (!enabled) tc = primary ? Color{255, 255, 255, 200} : FAINT;
    textCentered(label, r, F::Small, tc, sz);
    if (h) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
    return h && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

bool chipButton(Rectangle r, const std::string& label, bool sel) {
    bool h = hovered(r);
    DrawRectangleRounded(r, 1.f, 10, sel ? Color{15, 157, 88, 30} : h ? Color{15, 23, 42, 8} : Color{0, 0, 0, 0});
    DrawRectangleRoundedLinesEx(r, 1.f, 10, 1.f, sel ? Color{15, 157, 88, 90} : Color{15, 23, 42, 38});
    textCentered(label, r, F::Small, sel ? ACCENT : h ? TEXT : DIM, std::min(13.f, r.height * 0.5f));
    if (h) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
    return h && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

bool toggle(Rectangle r, const std::string& label, bool on) {
    bool h = hovered(r);
    text(label, r.x, r.y + (r.height - fontSize(F::Body)) / 2, F::Body, TEXT);
    Rectangle sw{r.x + r.width - 40, r.y + (r.height - 23) / 2, 40, 23};
    DrawRectangleRounded(sw, 1.f, 12, on ? Color{11, 128, 72, 255} : PANEL_HI);
    DrawCircle(int(on ? sw.x + sw.width - 11.5f : sw.x + 11.5f), int(sw.y + 11.5f), 8.5f, WHITE);
    if (h) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
    return h && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
}

bool stepper(Rectangle r, const std::string& label, const std::string& value, int* delta, float vw) {
    text(label, r.x, r.y + (r.height - fontSize(F::Body)) / 2, F::Body, DIM);
    float bw = r.height;
    Rectangle plus{r.x + r.width - bw, r.y, bw, r.height};
    Rectangle val{plus.x - vw, r.y, vw, r.height};
    Rectangle minus{val.x - bw, r.y, bw, r.height};
    textCentered(value, val, F::Body, TEXT);
    *delta = 0;
    if (button(minus, "-", false, true, 18)) *delta = IsKeyDown(KEY_LEFT_SHIFT) ? -10 : -1;
    if (button(plus, "+", false, true, 18)) *delta = IsKeyDown(KEY_LEFT_SHIFT) ? 10 : 1;
    return *delta != 0;
}

// CoreKick's segmented control: a recessed track, the selection a raised white pill
int segmented(Rectangle r, const std::vector<std::string>& items, int sel) {
    float rr = std::min(1.f, 22.f / std::max(1.f, r.height));
    DrawRectangleRounded(r, rr, 8, Color{236, 239, 244, 255});
    DrawRectangleRoundedLinesEx(r, rr, 8, 1.f, LINE);
    float w = r.width / items.size();
    int out = -1;
    for (size_t i = 0; i < items.size(); ++i) {
        Rectangle c{r.x + i * w + 3, r.y + 3, w - 6, r.height - 6};
        bool h = hovered(c);
        float cr = std::min(1.f, 16.f / std::max(1.f, c.height));
        if (int(i) == sel) {
            DrawRectangleRounded({c.x, c.y + 1, c.width, c.height}, cr, 8, Color{15, 23, 42, 22});
            DrawRectangleRounded(c, cr, 8, WHITE);
        }
        textCentered(items[i], c, F::Small, int(i) == sel || h ? TEXT : DIM, std::min(fontSize(F::Small) + 1, c.height * 0.5f));
        if (h) SetMouseCursor(MOUSE_CURSOR_POINTING_HAND);
        if (h && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) out = int(i);
    }
    return out;
}

void chip(float x, float y, const std::string& label, Color dot, float* outW) {
    Vector2 m = measure(label, F::Small);
    Rectangle r{x, y, m.x + 34, 28};
    DrawRectangleRounded(r, 0.5f, 10, PANEL);
    DrawRectangleRoundedLinesEx(r, 0.5f, 10, 1.f, LINE);
    DrawCircle(int(x + 14), int(y + 14), 4.5f, dot);
    text(label, x + 24, y + (28 - m.y) / 2, F::Small, TEXT);
    if (outW) *outW = r.width;
}

void progressBar(Rectangle r, float f, Color c, Color bg) {
    if (bg.r == 255 && bg.g == 255 && bg.b == 255 && bg.a == 25) bg = PANEL_HI;  // the old dark-theme default
    DrawRectangleRounded(r, 1.f, 6, bg);
    if (f > 0) DrawRectangleRounded({r.x, r.y, r.width * std::clamp(f, 0.f, 1.f), r.height}, 1.f, 6, c);
}

static TextBox* g_focus = nullptr;

bool textBox(Rectangle r, TextBox& tb, bool multiline, const std::string& placeholder) {
    bool h = hovered(r);
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (h) { tb.active = true; g_focus = &tb; }
        else if (g_focus == &tb && !CheckCollisionPointRec(GetMousePosition(), r)) { tb.active = false; g_focus = nullptr; }
    }
    if (g_focus != &tb) tb.active = false;
    if (h) SetMouseCursor(MOUSE_CURSOR_IBEAM);
    DrawRectangleRounded(r, multiline ? 0.04f : 0.3f, 6, PANEL);
    DrawRectangleRoundedLinesEx(r, multiline ? 0.04f : 0.3f, 6, 1.2f, tb.active ? ACCENT : Color{15, 23, 42, 38});
    bool changed = false;
    if (tb.active) {
        g_textActive = true;
        int c;
        while ((c = GetCharPressed()) > 0) {
            if (c < 128) tb.text += char(c);
            else if (c < 0x800) { tb.text += char(0xC0 | (c >> 6)); tb.text += char(0x80 | (c & 0x3F)); }
            else { tb.text += char(0xE0 | (c >> 12)); tb.text += char(0x80 | ((c >> 6) & 0x3F)); tb.text += char(0x80 | (c & 0x3F)); }
            changed = true;
        }
        if ((IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) && !tb.text.empty()) {
            do { tb.text.pop_back(); } while (!tb.text.empty() && (static_cast<unsigned char>(tb.text.back()) & 0xC0) == 0x80);
            changed = true;
        }
        if (multiline && (IsKeyPressed(KEY_ENTER) || IsKeyPressedRepeat(KEY_ENTER))) { tb.text += '\n'; changed = true; }
        bool mod = IsKeyDown(KEY_LEFT_SUPER) || IsKeyDown(KEY_RIGHT_SUPER) || IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        if (mod && IsKeyPressed(KEY_V)) { const char* cb = GetClipboardText(); if (cb) { tb.text += cb; changed = true; } }
        if (mod && IsKeyPressed(KEY_C)) SetClipboardText(tb.text.c_str());
        if (IsKeyPressed(KEY_ESCAPE) || (!multiline && IsKeyPressed(KEY_ENTER))) { tb.active = false; g_focus = nullptr; }
    }
    BeginScissorMode(int(r.x + 4), int(r.y + 2), int(r.width - 8), int(r.height - 4));
    float sz = 17, lh = 22;
    if (tb.text.empty() && !tb.active) text(placeholder, r.x + 10, r.y + 8, F::Body, FAINT, sz);
    float y = r.y + 8;
    size_t st = 0;
    std::vector<std::string> lines;
    for (size_t i = 0; i <= tb.text.size(); ++i)
        if (i == tb.text.size() || tb.text[i] == '\n') { lines.push_back(tb.text.substr(st, i - st)); st = i + 1; }
    float total = lines.size() * lh;
    float maxScroll = std::max(0.f, total - (r.height - 16));
    if (changed) tb.scroll = maxScroll;
    if (h) tb.scroll = std::clamp(tb.scroll - GetMouseWheelMove() * 30, 0.f, maxScroll);
    y -= tb.scroll;
    for (size_t i = 0; i < lines.size(); ++i) {
        text(lines[i], r.x + 10, y, F::Body, TEXT, sz);
        if (tb.active && i + 1 == lines.size() && std::fmod(GetTime(), 1.0) < 0.6) {
            float cx = r.x + 11 + measure(lines[i], F::Body, sz).x;
            DrawRectangle(int(cx), int(y + 2), 2, int(sz), ACCENT);
        }
        y += lh;
    }
    EndScissorMode();
    return changed;
}

// ------------------------------------------------------------------ colours
Color zoneColor(int z) {
    static const Color c[7] = {{148, 156, 172, 255}, {56, 140, 255, 255}, {74, 205, 110, 255}, {250, 204, 21, 255},
                               {255, 128, 40, 255},  {248, 70, 70, 255},  {176, 92, 240, 255}};
    return c[std::clamp(z, 0, 6)];
}
Color powerZoneColor(double w, double ftp) {
    if (ftp <= 0) return zoneColor(0);
    double f = w / ftp;
    static const double b[6] = {0.55, 0.75, 0.90, 1.05, 1.20, 1.50};
    for (int i = 0; i < 6; ++i) if (f <= b[i]) return zoneColor(i);
    return zoneColor(6);
}
Color hrZoneColor(int z, int count) {
    if (z <= 0) return DIM;
    if (count <= 5) {
        static const Color c[5] = {{148, 156, 172, 255}, {56, 140, 255, 255}, {74, 205, 110, 255}, {255, 128, 40, 255}, {248, 70, 70, 255}};
        return c[std::clamp(z - 1, 0, 4)];
    }
    return zoneColor(z - 1);
}
Color gradeColor(double g) {
    double p = g * 100;
    if (p < -4) return {59, 130, 246, 255};
    if (p < -1) return {96, 165, 250, 255};
    if (p < 2) return {74, 205, 110, 255};
    if (p < 4.5) return {250, 204, 21, 255};
    if (p < 7) return {255, 128, 40, 255};
    if (p < 10) return {239, 68, 68, 255};
    return {168, 85, 247, 255};
}

std::string fmtTime(double s) { return fmtDur(s); }
std::string fmtDist(double m, bool metric) {
    char b[32];
    if (metric) std::snprintf(b, sizeof b, m < 1000 ? "%.0f m" : "%.2f km", m < 1000 ? m : m / 1000);
    else std::snprintf(b, sizeof b, "%.2f mi", m / 1609.344);
    return b;
}
std::string fmtSpeed(double mps, bool metric) {
    char b[32];
    std::snprintf(b, sizeof b, "%.1f", metric ? mps * 3.6 : mps * 2.236936);
    return b;
}

// ------------------------------------------------------------------ charts
void seriesChart(Rectangle r, const std::vector<float>& v, Color c, float lo, float hi, bool fill) {
    if (v.size() < 2) return;
    int cols = int(r.width);
    Vector2 prev{-1, -1};
    for (int x = 0; x < cols; ++x) {
        size_t a = size_t(double(x) / cols * v.size()), b = std::max(a + 1, size_t(double(x + 1) / cols * v.size()));
        float s = 0; int n = 0;
        for (size_t i = a; i < b && i < v.size(); ++i) { s += v[i]; ++n; }
        if (!n) continue;
        float val = s / n;
        float y = r.y + r.height * (1 - std::clamp((val - lo) / (hi - lo), 0.f, 1.f));
        if (fill) DrawRectangleRec({r.x + x, y, 1.2f, r.y + r.height - y}, Color{c.r, c.g, c.b, 70});
        if (prev.x >= 0) DrawLineEx(prev, {r.x + x, y}, 1.6f, c);
        prev = {r.x + x, y};
    }
}

void powerChart(Rectangle r, const Samples& s, double ftp, double maxHr, int window, bool labels, bool showTarget) {
    size_t n = s.size();
    size_t start = n > size_t(window) ? n - window : 0;
    float maxP = float(std::max(ftp * 1.5, 200.0));
    for (size_t i = start; i < n; ++i) maxP = std::max(maxP, s.power[i] * 1.08f);
    for (int k = 1; k <= 3; ++k) DrawLineEx({r.x, r.y + r.height * k / 4}, {r.x + r.width, r.y + r.height * k / 4}, 1, Color{15, 23, 42, 10});
    float fy = r.y + r.height * (1 - float(ftp) / maxP);
    for (float x = r.x; x < r.x + r.width; x += 12) DrawLineEx({x, fy}, {std::min(x + 6, r.x + r.width), fy}, 1.2f, Color{15, 23, 42, 54});
    if (labels) text("FTP", r.x + 4, fy - 16, F::Small, FAINT, 12);
    float colW = r.width / float(window);
    float off = float(window - std::min<size_t>(n, window)) * colW;
    for (size_t i = start; i < n; ++i) {
        float x = r.x + off + (i - start) * colW;
        float h = std::min(1.f, s.power[i] / maxP) * r.height;
        Color c = powerZoneColor(s.power[i], ftp);
        c.a = 210;
        DrawRectangleRec({x, r.y + r.height - h, std::max(1.f, colW + 0.5f), h}, c);
    }
    auto line = [&](const std::vector<float>& v, float lo, float hi, Color c, float w) {
        Vector2 prev{-1, -1};
        for (size_t i = start; i < n; ++i) {
            if (v[i] <= 0) { prev = {-1, -1}; continue; }
            float x = r.x + off + (i - start + 0.5f) * colW;
            float y = r.y + r.height * (1 - std::clamp((v[i] - lo) / (hi - lo), 0.f, 1.f));
            if (prev.x >= 0) DrawLineEx(prev, {x, y}, w, c);
            prev = {x, y};
        }
    };
    if (showTarget) line(s.target, 0, maxP, Color{15, 23, 42, 153}, 1.6f);
    line(s.cadence, 0, 160, Color{15, 23, 42, 54}, 1.1f);
    line(s.hr, 50, float(std::max(maxHr, 120.0)), HR_RED, 2.0f);
    if (labels) {
        textRight(std::to_string(int(maxP)) + " W", r.x + r.width - 4, r.y + 2, F::Small, FAINT, 12);
        text("-" + std::to_string(window / 60) + " min", r.x + 4, r.y + r.height - 15, F::Small, FAINT, 12);
    }
}

void routeProfile(Rectangle r, const RouteProfile& route, double riderD, double ghostD, bool metric, bool showClimbs) {
    if (route.length <= 0) return;
    double lo = 1e9, hi = -1e9;
    for (double e : route.ele) { lo = std::min(lo, e); hi = std::max(hi, e); }
    lo -= 5; hi += 5;
    double span = std::max(hi - lo, 30.0);
    int S = int(r.width / 2);
    double L = route.length;
    double lap = std::fmod(std::max(0.0, riderD), L);
    auto yOf = [&](double e) { return r.y + r.height - float((e - lo) / span) * r.height * 0.86f - r.height * 0.06f; };
    if (showClimbs)
        for (auto& c : route.climbs) {
            float x0 = r.x + float(c.startD / L) * r.width, x1 = r.x + float(c.endD / L) * r.width;
            DrawRectangleRec({x0, r.y, x1 - x0, r.height}, Color{15, 23, 42, 9});
            if (c.category != "Uncategorised") text(c.category, x0 + 2, r.y + 1, F::Small, DIM, 12);
        }
    for (int i = 0; i < S; ++i) {
        double d = L * i / S;
        double g = route.gradeAt(d + L / S / 2, std::max(25.0, L / S));
        float y = yOf(route.elevationAt(d));
        Color c = gradeColor(g);
        c.a = d <= lap ? 240 : 120;
        DrawRectangleRec({r.x + i * r.width / S, y, r.width / S + 0.6f, r.y + r.height - y}, c);
    }
    auto marker = [&](double d, Color col, float rad) {
        double ld = std::fmod(std::max(0.0, d), L);
        float x = r.x + float(ld / L) * r.width, y = yOf(route.elevationAt(ld));
        DrawLineEx({x, y}, {x, r.y + r.height}, 1, Color{15, 23, 42, 72});
        DrawCircleV({x, y - rad}, rad + 2, WHITE);
        DrawCircleV({x, y - rad}, rad, col);
    };
    if (ghostD >= 0) marker(ghostD, SKY, 4);
    marker(riderD, TEXT, 5);
    (void)metric;
}

void upcomingProfile(Rectangle r, const RouteProfile& route, double d0, double ahead, bool metric) {
    int S = int(r.width / 3);
    double lo = 1e9, hi = -1e9;
    for (int i = 0; i <= S; ++i) {
        double e = route.elevationAt(d0 + ahead * i / S);
        lo = std::min(lo, e); hi = std::max(hi, e);
    }
    double span = std::max(hi - lo, 25.0);
    for (int i = 0; i < S; ++i) {
        double d = d0 + ahead * i / S;
        float h = float((route.elevationAt(d) - lo) / span) * r.height * 0.78f + r.height * 0.12f;
        DrawRectangleRec({r.x + i * r.width / S, r.y + r.height - h, r.width / S + 0.6f, h}, gradeColor(route.gradeAt(d, 15)));
    }
    int seg = ahead >= 1500 ? 250 : 100;
    for (int k = 0; k < int(ahead / seg); ++k) {
        double a = d0 + k * seg, b = a + seg;
        float g = float((route.elevationAt(b) - route.elevationAt(a)) / seg * 100);
        char buf[16];
        std::snprintf(buf, sizeof buf, "%.1f%%", g);
        float x = r.x + float(k * seg / ahead) * r.width;
        DrawLineEx({x, r.y}, {x, r.y + r.height}, 1, Color{15, 23, 42, 19});
        text(buf, x + 3, r.y + 1, F::Small, TEXT, 12);
    }
    (void)metric;
}

void workoutChart(Rectangle r, const std::vector<wk::Step>& steps, double ftp, double bias, double t, const Samples* s,
                  const std::vector<double>* wt) {
    if (steps.empty()) return;
    double T = steps.back().endS();
    double maxPct = 1.3;
    for (auto& st : steps) maxPct = std::max(maxPct, std::max(st.from, st.to) * bias * 1.1);
    for (auto& st : steps) {
        int seg = (st.erg && st.from != st.to) ? 10 : 1;
        for (int k = 0; k < seg; ++k) {
            double a = st.startS + st.durationS * double(k) / seg, b = st.startS + st.durationS * double(k + 1) / seg;
            double pct = st.erg ? (st.from + (st.to - st.from) * (k + 0.5) / seg) * bias : (st.effort ? 0.95 : 0.6);
            float x0 = r.x + float(a / T) * r.width, x1 = r.x + float(b / T) * r.width;
            float h = float(pct / maxPct) * r.height;
            Color c = st.erg ? powerZoneColor(pct * ftp, ftp) : Color{170, 178, 192, 255};
            c.a = a <= t ? 235 : 120;
            DrawRectangleRec({x0, r.y + r.height - h, std::max(1.f, x1 - x0 - 0.8f), h}, c);
            if (!st.erg) for (float yy = r.y + r.height - h; yy < r.y + r.height; yy += 6) DrawLineEx({x0, yy}, {x1, yy + 3}, 1, Color{15, 23, 42, 36});
        }
    }
    if (s && wt && !s->power.empty()) {
        Vector2 prev{-1, -1};
        size_t n = std::min(s->size(), wt->size());
        size_t step = std::max<size_t>(1, n / std::max<size_t>(1, size_t(r.width)));
        for (size_t i = 0; i < n; i += step) {
            double tt = (*wt)[i];
            if (tt < 0 || tt > T) { prev = {-1, -1}; continue; }
            float x = r.x + float(tt / T) * r.width;
            float y = r.y + r.height - std::min(1.f, float(s->power[i] / (maxPct * ftp))) * r.height;
            if (prev.x >= 0 && x >= prev.x) DrawLineEx(prev, {x, y}, 1.5f, Color{15, 23, 42, 189});
            prev = {x, y};
        }
    }
    if (t >= 0) {
        float cx = r.x + float(std::min(t, T) / T) * r.width;
        DrawLineEx({cx, r.y - 4}, {cx, r.y + r.height}, 2, TEXT);
    }
}

void zoneBars(Rectangle r, const double* secs, int n, const std::vector<std::string>& labels, const std::vector<Color>& colors) {
    double total = 0, mx = 0;
    for (int i = 0; i < n; ++i) { total += secs[i]; mx = std::max(mx, secs[i]); }
    float rowH = r.height / n;
    for (int i = 0; i < n; ++i) {
        float y = r.y + i * rowH;
        text(labels[i], r.x, y + rowH / 2 - 8, F::Small, DIM, 13);
        Rectangle bar{r.x + 120, y + rowH * 0.2f, r.width - 200, rowH * 0.6f};
        DrawRectangleRounded(bar, 0.4f, 4, Color{15, 23, 42, 12});
        if (mx > 0) DrawRectangleRounded({bar.x, bar.y, bar.width * float(secs[i] / mx), bar.height}, 0.4f, 4, colors[i]);
        char b[40];
        std::snprintf(b, sizeof b, "%s  %2.0f%%", fmtDur(secs[i]).c_str(), total > 0 ? secs[i] / total * 100 : 0.0);
        textRight(b, r.x + r.width, y + rowH / 2 - 8, F::Small, TEXT, 13);
    }
}


// ------------------------------------------------------------------ generic charts
void lineChart(Rectangle r, const std::vector<Series>& series, float lo, float hi, const std::vector<std::string>& xLabels, int yTicks,
               const char* yFmt, float zeroLine) {
    if (hi - lo < 1e-6f) hi = lo + 1;
    float lw = 44, bh = xLabels.empty() ? 0 : 20;
    Rectangle p{r.x + lw, r.y + 4, r.width - lw - 6, r.height - bh - 8};
    char b[32];
    for (int i = 0; i <= yTicks; ++i) {
        float v = lo + (hi - lo) * i / std::max(1, yTicks);
        float y = p.y + p.height - (v - lo) / (hi - lo) * p.height;
        DrawLineEx({p.x, y}, {p.x + p.width, y}, 1, Color{15, 23, 42, 16});
        std::snprintf(b, sizeof b, yFmt, v);
        textRight(b, p.x - 6, y - 8, F::Small, FAINT, 12);
    }
    if (!std::isnan(zeroLine) && zeroLine > lo && zeroLine < hi) {
        float y = p.y + p.height - (zeroLine - lo) / (hi - lo) * p.height;
        DrawLineEx({p.x, y}, {p.x + p.width, y}, 1.2f, Color{15, 23, 42, 54});
    }
    for (size_t i = 0; i < xLabels.size(); ++i) {
        if (xLabels[i].empty()) continue;
        float x = p.x + (xLabels.size() > 1 ? p.width * i / (xLabels.size() - 1) : p.width / 2);
        Vector2 m = measure(xLabels[i], F::Small, 12);
        text(xLabels[i], std::clamp(x - m.x / 2, p.x - lw * 0.5f, p.x + p.width - m.x), p.y + p.height + 5, F::Small, FAINT, 12);
    }
    for (auto& s : series) {
        size_t n = s.v.size();
        if (n < 2) continue;
        auto pt = [&](size_t i) { return Vector2{p.x + p.width * i / (n - 1), p.y + p.height - (std::clamp(s.v[i], lo, hi) - lo) / (hi - lo) * p.height}; };
        if (s.fill) {
            float base = p.y + p.height;
            for (size_t i = 0; i + 1 < n; ++i) {
                Vector2 a = pt(i), c = pt(i + 1);
                Color f{s.c.r, s.c.g, s.c.b, 40};
                DrawTriangle(a, {a.x, base}, {c.x, base}, f);
                DrawTriangle(a, {c.x, base}, c, f);
            }
        }
        for (size_t i = 0; i + 1 < n; ++i) DrawLineEx(pt(i), pt(i + 1), s.width, s.c);
    }
}

void barChart(Rectangle r, const std::vector<float>& v, const std::vector<std::string>& labels, Color c, int highlight, const char* valFmt, float goal) {
    if (v.empty()) return;
    float mx = goal > 0 ? goal : 0;
    for (float x : v) mx = std::max(mx, x);
    if (mx <= 0) mx = 1;
    mx *= 1.12f;
    float lh = labels.empty() ? 0 : 18;
    float bw = r.width / v.size();
    float ph = r.height - lh - (valFmt ? 16 : 0);
    float py = r.y + (valFmt ? 16 : 0);
    for (size_t i = 0; i < v.size(); ++i) {
        float h = v[i] / mx * ph;
        Rectangle br{r.x + i * bw + bw * 0.16f, py + ph - h, bw * 0.68f, h};
        Color cc = int(i) == highlight ? c : Color{c.r, c.g, c.b, 150};
        if (h > 0.5f) DrawRectangleRounded(br, 0.25f, 4, cc);
        else DrawRectangleRec({br.x, py + ph - 2, br.width, 2}, Color{15, 23, 42, 22});
        if (valFmt && v[i] > 0) {
            char b[32];
            std::snprintf(b, sizeof b, valFmt, v[i]);
            Vector2 m = measure(b, F::Small, 11);
            if (m.x < bw + 6) text(b, br.x + br.width / 2 - m.x / 2, br.y - 15, F::Small, DIM, 11);
        }
        if (i < labels.size() && !labels[i].empty()) {
            Vector2 m = measure(labels[i], F::Small, 11);
            text(labels[i], r.x + i * bw + bw / 2 - m.x / 2, r.y + r.height - lh + 3, F::Small, FAINT, 11);
        }
    }
    if (goal > 0) {
        float y = py + ph - goal / mx * ph;
        for (float x = r.x; x < r.x + r.width; x += 10) DrawLineEx({x, y}, {std::min(x + 5, r.x + r.width), y}, 1.2f, Color{15, 23, 42, 81});
    }
}

void scrollUpdate(Rectangle r, float& scroll, float contentH) {
    if (hovered(r)) scroll -= GetMouseWheelMove() * 60;
    scroll = std::clamp(scroll, 0.f, std::max(0.f, contentH - r.height));
}

void scrollBar(Rectangle r, float scroll, float contentH) {
    if (contentH <= r.height + 1) return;
    float h = std::max(30.f, r.height * r.height / contentH);
    float y = r.y + (r.height - h) * scroll / (contentH - r.height);
    DrawRectangleRounded({r.x + r.width - 4, y, 3, h}, 1, 4, Color{15, 23, 42, 45});
}

std::string fmtDateShort(int y, int m, int d) {
    static const char* mn[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    (void)y;
    return std::to_string(d) + " " + mn[std::clamp(m, 1, 12) - 1];
}

}  // namespace ui
