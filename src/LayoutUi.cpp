// Modify UI: move and resize the ride HUD's widgets (pause menu > Modify UI).
// The ride stays paused. Drag a card to move it (edges snap to the screen margins, the centre
// lines and the other cards; hold Alt to place freely), drag its corner handle or scroll over it
// to resize, arrows nudge the selected card (Shift: 10 px). Done (or Esc) goes back to the pause
// menu. The layout is saved to prefs (hudLayout) as each change ends.
#include <algorithm>
#include <cmath>

#include "App.h"
#include "Ui.h"
#include "raymath.h"

using namespace ui;

namespace {
constexpr float PAD = 22, SNAP = 8, HANDLE = 8;
Rectangle grow(Rectangle r, float d) { return {r.x - d, r.y - d, r.width + 2 * d, r.height + 2 * d}; }
}  // namespace

void App::beginWidget(int id, Rectangle def) {
    Vector2 tl;
    float s;
    hudLayout_.resolve(id, def, ui::screenW(), ui::screenH(), tl, s);
    WidgetFrame& w = wid_[id];
    w.on = true;
    w.def = def;
    w.at = {tl.x, tl.y, def.width * s, def.height * s};
    w.scale = s;
    ui::pushWidget({tl.x - s * def.x, tl.y - s * def.y}, s);
}

void App::endWidget() { ui::popWidget(); }

void App::closeLayoutEditor() {
    layoutEdit_ = false;
    edSel_ = edHover_ = -1;
    edDrag_ = 0;
    prefs_.hudLayout = hudLayout_.save();
    store::save(prof_, prefs_);
    pauseMenu_ = true;   // back where it was opened from; the ride is still paused
}

void App::drawLayoutEditor() {
    const float W = ui::screenW(), H = ui::screenH();
    const Vector2 m = ui::mouseLogical();
    auto save = [&] { prefs_.hudLayout = hudLayout_.save(); store::save(prof_, prefs_); };
    auto handleAt = [&](int id) { const Rectangle& a = wid_[id].at; return Vector2{a.x + a.width + 4, a.y + a.height + 4}; };

    // ---- the toolbar: centre of the screen by default (where the rider is); drag its title to
    // move it out of the way. Hidden while a card is being dragged.
    Rectangle tb{W / 2 - 290 + edBar_.x, H * 0.5f - 66 + edBar_.y, 580, 132};
    tb.x = std::clamp(tb.x, 0.f, W - tb.width);
    tb.y = std::clamp(tb.y, 0.f, H - tb.height);
    const bool showBar = edDrag_ != 1 && edDrag_ != 2;
    const bool overBar = showBar && CheckCollisionPointRec(m, tb);
    const Rectangle grip{tb.x, tb.y, tb.width, 44};   // the title strip
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && CheckCollisionPointRec(m, grip) && !edDrag_) { edDrag_ = 3; edGrab_ = m; edAt0_ = tb; }
    if (edDrag_ == 3) {
        edBar_.x += m.x - edGrab_.x;
        edBar_.y += m.y - edGrab_.y;
        edGrab_ = m;
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) edDrag_ = 0;
    }

    // ---- hover: the top-most card under the mouse (or the selected card's handle)
    edHover_ = -1;
    if (!overBar && !edDrag_) {
        for (int id = hud::COUNT - 1; id >= 0; --id)
            if (wid_[id].on && CheckCollisionPointRec(m, grow(wid_[id].at, 4))) { edHover_ = id; break; }
    }
    auto overHandle = [&](int id) { return id >= 0 && wid_[id].on && Vector2Distance(m, handleAt(id)) <= HANDLE + 4; };

    // ---- start a drag
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !overBar) {
        int id = overHandle(edSel_) ? edSel_ : overHandle(edHover_) ? edHover_ : -1;
        if (id >= 0) { edSel_ = id; edDrag_ = 2; }
        else if (edHover_ >= 0) { edSel_ = edHover_; edDrag_ = 1; }
        else edSel_ = -1;
        if (edDrag_) { edGrab_ = m; edAt0_ = wid_[edSel_].at; edS0_ = wid_[edSel_].scale; }
    }
    edGuideX_ = edGuideY_ = -1;
    if ((edDrag_ == 1 || edDrag_ == 2) && edSel_ >= 0 && wid_[edSel_].on) {
        const WidgetFrame& w = wid_[edSel_];
        if (edDrag_ == 1) {   // move, snapping one of the card's edges / centre to a target
            Rectangle r{edAt0_.x + m.x - edGrab_.x, edAt0_.y + m.y - edGrab_.y, edAt0_.width, edAt0_.height};
            if (!(IsKeyDown(KEY_LEFT_ALT) || IsKeyDown(KEY_RIGHT_ALT))) {
                std::vector<float> tx{PAD, W / 2, W - PAD}, ty{PAD, H / 2, H - PAD};
                for (int id = 0; id < hud::COUNT; ++id) {
                    if (id == edSel_ || !wid_[id].on) continue;
                    const Rectangle& o = wid_[id].at;
                    tx.insert(tx.end(), {o.x, o.x + o.width / 2, o.x + o.width});
                    ty.insert(ty.end(), {o.y, o.y + o.height / 2, o.y + o.height});
                }
                auto snap = [&](float p, float len, const std::vector<float>& ts, float& guide) {
                    float best = SNAP, shift = 0;
                    for (float t : ts)
                        for (float f : {0.f, 0.5f, 1.f}) {
                            float d = t - (p + f * len);
                            if (std::fabs(d) < best) { best = std::fabs(d); shift = d; guide = t; }
                        }
                    return p + shift;
                };
                r.x = snap(r.x, r.width, tx, edGuideX_);
                r.y = snap(r.y, r.height, ty, edGuideY_);
            }
            r.x = std::clamp(r.x, 0.f, std::max(0.f, W - r.width));
            r.y = std::clamp(r.y, 0.f, std::max(0.f, H - r.height));
            hudLayout_.pin(edSel_, r, edS0_, W, H);
        } else {   // resize from the top-left corner, by how far the handle has been pulled
            Vector2 tl{edAt0_.x, edAt0_.y};
            float d0 = std::max(10.f, Vector2Distance(edGrab_, tl)), d1 = Vector2Distance(m, tl);
            float s = std::clamp(edS0_ * d1 / d0, hud::MIN_SCALE, hud::MAX_SCALE);
            if (std::fabs(s - 1.f) < 0.04f) s = 1.f;   // a gentle click back to the built-in size
            hudLayout_.pin(edSel_, {tl.x, tl.y, w.def.width * s, w.def.height * s}, s, W, H);
        }
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) { edDrag_ = 0; save(); }
    }
    // wheel over a card: resize about its centre
    if (!edDrag_ && edHover_ >= 0) {
        float wheel = GetMouseWheelMove();
        if (wheel != 0) {
            const WidgetFrame& w = wid_[edHover_];
            float s = std::clamp(w.scale * std::pow(1.06f, wheel), hud::MIN_SCALE, hud::MAX_SCALE);
            if (std::fabs(s - 1.f) < 0.03f) s = 1.f;
            Vector2 c{w.at.x + w.at.width / 2, w.at.y + w.at.height / 2};
            float nw = w.def.width * s, nh = w.def.height * s;
            hudLayout_.pin(edHover_, {c.x - nw / 2, c.y - nh / 2, nw, nh}, s, W, H);
            edSel_ = edHover_;
            save();
        }
    }
    // arrows nudge the selected card
    if (edSel_ >= 0 && wid_[edSel_].on && !edDrag_) {
        float st = (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) ? 10.f : 1.f;
        Vector2 d{};
        if (IsKeyPressed(KEY_LEFT) || IsKeyPressedRepeat(KEY_LEFT)) d.x -= st;
        if (IsKeyPressed(KEY_RIGHT) || IsKeyPressedRepeat(KEY_RIGHT)) d.x += st;
        if (IsKeyPressed(KEY_UP) || IsKeyPressedRepeat(KEY_UP)) d.y -= st;
        if (IsKeyPressed(KEY_DOWN) || IsKeyPressedRepeat(KEY_DOWN)) d.y += st;
        if (d.x != 0 || d.y != 0) {
            Rectangle r = wid_[edSel_].at;
            r.x = std::clamp(r.x + d.x, 0.f, std::max(0.f, W - r.width));
            r.y = std::clamp(r.y + d.y, 0.f, std::max(0.f, H - r.height));
            hudLayout_.pin(edSel_, r, wid_[edSel_].scale, W, H);
            save();
        }
    }

    // ---- outlines, names, handles
    for (float x : {edGuideX_}) if (x >= 0) DrawLineEx({x, 0}, {x, H}, 1.5f, withAlpha(ACCENT, 200));
    for (float y : {edGuideY_}) if (y >= 0) DrawLineEx({0, y}, {W, y}, 1.5f, withAlpha(ACCENT, 200));
    for (int id = 0; id < hud::COUNT; ++id) {
        const WidgetFrame& w = wid_[id];
        if (!w.on) continue;
        const bool sel = id == edSel_, hov = id == edHover_;
        Rectangle o = grow(w.at, 4);
        float rr = std::min(1.f, 28.f / std::max(1.f, std::min(o.width, o.height)));
        if (sel || hov) DrawRectangleRounded(o, rr, 10, withAlpha(ACCENT, sel ? 26 : 14));
        DrawRectangleRoundedLinesEx(o, rr, 10, sel ? 2.5f : 1.5f, sel ? ACCENT : hov ? Color{255, 255, 255, 235} : Color{255, 255, 255, 140});
        if (!sel && !hov) continue;
        // name tag (with the size when it isn't the built-in one)
        std::string tag = hud::name(id);
        if (std::fabs(w.scale - 1.f) > 0.005f) tag += TextFormat("  %d%%", int(std::lround(w.scale * 100)));
        Vector2 tm = measure(tag, F::Small, 11);
        Rectangle pill{o.x, o.y - tm.y - 8, tm.x + 14, tm.y + 6};
        if (pill.y < 2) pill.y = o.y + o.height + 4;
        DrawRectangleRounded(pill, 1, 8, sel ? ACCENT : Color{17, 24, 39, 215});
        text(tag, pill.x + 7, pill.y + 3, F::Small, WHITE, 11);
        {   // resize handle at the bottom-right corner
            Vector2 h = handleAt(id);
            bool hh = Vector2Distance(m, h) <= HANDLE + 4 || (sel && edDrag_ == 2);
            DrawCircleV(h, HANDLE + 1.5f, Color{15, 23, 42, 60});
            DrawCircleV(h, HANDLE, WHITE);
            DrawCircleV(h, HANDLE - 3, hh ? ACCENT : withAlpha(ACCENT, 170));
        }
    }
    if (overHandle(edSel_) || overHandle(edHover_) || edDrag_ == 2) SetMouseCursor(MOUSE_CURSOR_RESIZE_NWSE);
    else if (edDrag_ == 1 || edDrag_ == 3 || edHover_ >= 0 || CheckCollisionPointRec(m, grip)) SetMouseCursor(MOUSE_CURSOR_RESIZE_ALL);
    else SetMouseCursor(MOUSE_CURSOR_DEFAULT);

    // ---- toolbar
    if (!showBar) return;
    panel(tb, PANEL);
    for (int k = 0; k < 3; ++k) DrawCircleV({tb.x + tb.width / 2 - 8 + k * 8.f, tb.y + 9}, 2, Color{120, 124, 132, 160});   // grip dots
    text("Modify UI", tb.x + 22, tb.y + 16, F::Title, TEXT, 22);
    textRight("Ride paused", tb.x + tb.width - 22, tb.y + 22, F::Small, DIM, 13);
    text(edSel_ >= 0 ? std::string("Selected: ") + hud::name(edSel_) + "  \xC2\xB7  arrows nudge (Shift x10), Alt drags without snapping"
                     : "Drag a card to move it  \xC2\xB7  drag its corner or scroll over it to resize",
         tb.x + 22, tb.y + 50, F::Small, DIM, 13);
    float bx = tb.x + 22, by = tb.y + 78, bh = 38;
    if (button({bx, by, 150, bh}, "Reset card", false, edSel_ >= 0 && hudLayout_.place[edSel_].set)) { hudLayout_.reset(edSel_); save(); }
    if (button({bx + 160, by, 150, bh}, "Reset all", false, hudLayout_.any())) { hudLayout_.resetAll(); save(); }
    if (button({tb.x + tb.width - 22 - 140, by, 140, bh}, "Done", true)) { SetMouseCursor(MOUSE_CURSOR_DEFAULT); closeLayoutEditor(); }
}
