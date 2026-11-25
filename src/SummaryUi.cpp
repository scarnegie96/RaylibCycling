// Post-ride summary: the ride detail view over the 3D world, plus "Done".
#include "App.h"
#include "raylib.h"

using namespace ui;

void App::drawSummary() {
    if (!summaryRec_) { screen_ = Screen::Home; return; }
    const float W = float(GetScreenWidth()), H = float(GetScreenHeight());
    DrawRectangle(0, 0, int(W), int(H), Color{15, 23, 42, 60});
    Rectangle r{W * 0.03f, H * 0.03f, W * 0.94f, H * 0.94f};
    panel(r, PANEL);
    textRight("Ride saved", r.x + r.width - 250, r.y + 26, F::Body, GOOD, 18);
    if (button({r.x + r.width - 222, r.y + 16, 200, 46}, "Done", true) || (IsKeyPressed(KEY_ENTER) && !anyTextActive())) {
        screen_ = Screen::Home;
        page_ = PHome;
        summaryRec_.reset();
        if (const Route* rt = findRoute(selRoute_)) showRoute(*rt);
        return;
    }
    drawRideDetail({r.x + 26, r.y + 20, r.width - 52, r.height - 34}, *summaryRec_, true);
}
