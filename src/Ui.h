#pragma once
// Immediate-mode UI toolkit + 2D charts for CoreRay.
#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "core/Engine.h"
#include "raylib.h"

namespace ui {

// The palette is set by applyTheme(); everything reads these.
extern Color BG, PANEL, PANEL2, PANEL_HI, LINE, TEXT, DIM, FAINT, ACCENT, GOOD, WARN, BAD, HR_RED, SKY, GHOST, INK;
extern Color HOVER, SIDEBAR;
// the 3D ride HUD: frosted glass cards with ink on them
extern Color HUD_INK, HUD_MUTED, HUD_GLASS, HUD_INV;

// ---- themes
enum class Theme { Light, Dark, Midnight, Sand };
enum class HudStyle { MatchTheme, Light, Dark, DayNight };
extern const char* const THEME_NAMES[4];
extern const char* const ACCENT_NAMES[6];
extern const Color ACCENTS[6];
extern const char* const HUD_STYLE_NAMES[4];
// theme + accent; hudDark picks the ride HUD's glass
void applyTheme(int theme, int accent, bool hudDark);
bool themeIsDark(int theme);
bool themeIsDark();
bool hudIsDark();
// ink over the current background at alpha a (dark ink on light themes, light ink on dark)
Color ink(unsigned char a);
Color hudInk(unsigned char a);
Color shadow(unsigned char a);  // always dark
Color accentA(unsigned char a);
Color accentDeep();             // pressed / hover accent
Color withAlpha(Color c, unsigned char a);
Color hudPick(Color onLight, Color onDark);  // a tint that reads on the current HUD glass

// ---- scaling. The UI is laid out in logical pixels; pushScale() draws it bigger. Inside,
// screenW()/screenH() are the logical screen and the mouse is in logical pixels too.
void pushScale(float s);
void popScale();
float scale();
// for input handled outside the scaled drawing (frameUpdate): last frame's scale, logical mouse / screen
float lastScale();
Vector2 mouseLogical();
void reapplyScale();  // after EndTextureMode / EndMode3D, which reset the modelview
float screenW();
float screenH();
void beginScissor(Rectangle r);  // logical rect
// A movable / resizable widget: draw it in its own (default-layout) coordinates and it lands at
// offset + scale * p. The mouse (mouseLogical, hovered, clicked) and scissor rects follow, so
// buttons inside keep working. Not nestable beyond a few levels; always pair with popWidget().
void pushWidget(Vector2 offset, float scale);
void popWidget();
// suggested scale for a window: 1 at 1440x900 points, up to 2.2 on big screens
float autoScale(float minLogicalW, float minLogicalH, float user);

void loadFonts();
void unloadFonts();
enum class F { Big, Title, Body, Small };
Font& font(F f);
float fontSize(F f);

Vector2 measure(const std::string& s, F f, float size = 0);
void text(const std::string& s, float x, float y, F f, Color c, float size = 0);
void textCentered(const std::string& s, Rectangle r, F f, Color c, float size = 0);
void textRight(const std::string& s, float right, float y, F f, Color c, float size = 0);
// word-wrapped text, returns height used
float textWrapped(const std::string& s, Rectangle r, F f, Color c, float size = 0);

// input routing: when a modal/drawer is open, widgets below it shouldn't react
void beginFrame();
void setInputBlocked(bool b);
bool mouseFree();

void panel(Rectangle r, Color c = PANEL, float round = 0.18f);
bool hovered(Rectangle r);
bool clicked(Rectangle r);
bool button(Rectangle r, const std::string& label, bool primary = false, bool enabled = true, float size = 0);
bool chipButton(Rectangle r, const std::string& label, bool selected);
bool toggle(Rectangle r, const std::string& label, bool on);
bool stepper(Rectangle r, const std::string& label, const std::string& value, int* delta, float valueW = 96);
// returns index clicked or -1
int segmented(Rectangle r, const std::vector<std::string>& items, int selected);
void chip(float x, float y, const std::string& label, Color dot, float* outWidth = nullptr);
void progressBar(Rectangle r, float f, Color c, Color bg = Color{255, 255, 255, 25});

struct TextBox {
    std::string text;
    bool active = false;
    float scroll = 0;
};
// returns true when text changed
bool textBox(Rectangle r, TextBox& tb, bool multiline, const std::string& placeholder = "");
bool anyTextActive();

// zones / colours
Color powerZoneColor(double watts, double ftp);
Color zoneColor(int zone0);  // 0..6
Color hrZoneColor(int zone1, int count);
Color gradeColor(double gradeFrac);

// charts
void powerChart(Rectangle r, const Samples& s, double ftp, double maxHr, int windowSec, bool labels = true, bool showTarget = true);
void seriesChart(Rectangle r, const std::vector<float>& v, Color c, float lo, float hi, bool fill);
void routeProfile(Rectangle r, const RouteProfile& route, double riderD, double ghostD, bool metric, bool showClimbs = true);
void upcomingProfile(Rectangle r, const RouteProfile& route, double d0, double ahead, bool metric);
void workoutChart(Rectangle r, const std::vector<wk::Step>& steps, double ftp, double bias, double t, const Samples* s,
                  const std::vector<double>* workoutT = nullptr);
void zoneBars(Rectangle r, const double* secs, int n, const std::vector<std::string>& labels, const std::vector<Color>& colors);

// generic charts
struct Series { std::vector<float> v; Color c; float width = 2.f; bool fill = false; };
// y range lo..hi, optional x labels spread evenly, yTicks horizontal grid lines labelled with yFmt
void lineChart(Rectangle r, const std::vector<Series>& series, float lo, float hi, const std::vector<std::string>& xLabels, int yTicks,
               const char* yFmt = "%.0f", float zeroLine = NAN);
void barChart(Rectangle r, const std::vector<float>& v, const std::vector<std::string>& labels, Color c, int highlight = -1,
              const char* valFmt = nullptr, float goal = -1);
// scrolling list helper: updates scroll from the mouse wheel when hovered and clamps it
void scrollUpdate(Rectangle r, float& scroll, float contentH);
void scrollBar(Rectangle r, float scroll, float contentH);
std::string fmtDateShort(int y, int m, int d);

std::string fmtDist(double m, bool metric);
std::string fmtSpeed(double mps, bool metric);
std::string fmtTime(double s);

}  // namespace ui
