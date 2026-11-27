#include "HudLayout.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace hud {

const char* name(int id) {
    static const char* N[COUNT] = {"Effort", "Gear", "Ride", "Climb", "Weather", "Workout step", "Controls", "Flow", "Road ahead", "Elevation", "Minimap"};
    return id >= 0 && id < COUNT ? N[id] : "?";
}

void Layout::resolve(int id, Rectangle def, float W, float H, Vector2& tl, float& s) const {
    const Place& p = place[id];
    if (!p.set) { tl = {def.x, def.y}; s = 1; return; }
    s = std::clamp(p.scale, MIN_SCALE, MAX_SCALE);
    float w = def.width * s, h = def.height * s;
    tl = {p.px * W - p.hx * w, p.py * H - p.hy * h};
    // keep it on the screen (a window made smaller, or content grown wider)
    tl.x = std::clamp(tl.x, 0.f, std::max(0.f, W - w));
    tl.y = std::clamp(tl.y, 0.f, std::max(0.f, H - h));
}

void Layout::pin(int id, Rectangle at, float scale, float W, float H) {
    Place& p = place[id];
    p.set = true;
    p.scale = std::clamp(scale, MIN_SCALE, MAX_SCALE);
    // anchor on the side of the widget nearest a screen edge (thirds: left / centre / right)
    float cx = (at.x + at.width / 2) / W, cy = (at.y + at.height / 2) / H;
    p.hx = cx < 1.f / 3 ? 0.f : cx > 2.f / 3 ? 1.f : 0.5f;
    p.hy = cy < 1.f / 3 ? 0.f : cy > 2.f / 3 ? 1.f : 0.5f;
    p.px = (at.x + p.hx * at.width) / W;
    p.py = (at.y + p.hy * at.height) / H;
}

std::string Layout::save() const {
    std::string out;
    char b[96];
    for (int i = 0; i < COUNT; ++i) {
        const Place& p = place[i];
        if (!p.set) continue;
        std::snprintf(b, sizeof b, "%d:%.4f,%.4f,%.1f,%.1f,%.3f;", i, p.px, p.py, p.hx, p.hy, p.scale);
        out += b;
    }
    return out;
}

void Layout::load(const std::string& s) {
    resetAll();
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ';')) {
        int id;
        Place p;
        if (std::sscanf(item.c_str(), "%d:%f,%f,%f,%f,%f", &id, &p.px, &p.py, &p.hx, &p.hy, &p.scale) != 6) continue;
        if (id < 0 || id >= COUNT) continue;
        p.set = true;
        p.px = std::clamp(p.px, 0.f, 1.f);
        p.py = std::clamp(p.py, 0.f, 1.f);
        p.scale = std::clamp(p.scale, MIN_SCALE, MAX_SCALE);
        place[id] = p;
    }
}

}  // namespace hud
