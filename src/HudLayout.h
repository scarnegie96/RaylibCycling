#pragma once
// Where the ride HUD's widgets go when the rider has moved or resized them (pause menu >
// Modify UI). A widget the rider hasn't touched keeps its built-in place. A moved one is pinned
// by one of its points (left / centre / right, top / middle / bottom: whichever is nearest the
// screen edge it was left closest to) to a fraction of the screen, so layouts survive window
// resizes and widgets whose width follows their content grow away from the edge.
#include <string>

#include "raylib.h"

namespace hud {

enum Id { Effort, Gear, Ride, Climb, Weather, Step, Controls, Flow, RoadAhead, Profile, MiniMap, COUNT };
const char* name(int id);

struct Place {
    bool set = false;
    float px = 0, py = 0;     // the anchor point, as a fraction of the screen
    float hx = 0, hy = 0;     // which point of the widget: 0 left/top, 0.5 centre, 1 right/bottom
    float scale = 1;          // 1 = built-in size
};

struct Layout {
    Place place[COUNT];
    // where the widget lands this frame (top-left, scale), given its built-in rect
    void resolve(int id, Rectangle def, float W, float H, Vector2& topLeft, float& s) const;
    // pin a widget whose scaled rect is now `at` (anchor chosen from where it sits)
    void pin(int id, Rectangle at, float scale, float W, float H);
    void reset(int id) { place[id] = Place{}; }
    void resetAll() { for (auto& p : place) p = Place{}; }
    bool any() const { for (auto& p : place) if (p.set) return true; return false; }
    std::string save() const;              // "id:px,py,hx,hy,scale;..."
    void load(const std::string& s);
};

constexpr float MIN_SCALE = 0.6f, MAX_SCALE = 2.0f;

}  // namespace hud
