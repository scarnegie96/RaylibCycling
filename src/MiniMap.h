#pragma once
// The in-ride minimap for generated worlds (wmap): a stylised topographic map of the whole
// world baked once into a texture off-thread (the menu preview's biome palette and road
// colours, a soft hillshade, contour lines with bolder index contours, lakes and rivers,
// towns), shown in a circular HUD widget around the rider, heading-up or north-up.
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "raylib.h"

namespace wmap { class Map; }

class MiniMap {
public:
    MiniMap() = default;
    ~MiniMap();
    MiniMap(const MiniMap&) = delete;
    MiniMap& operator=(const MiniMap&) = delete;

    // Bake this map (no-op when it is already the current one). nullptr clears.
    void setMap(std::shared_ptr<const wmap::Map> m);
    bool ready() const { return tex_.id != 0 && map_; }
    bool building() const { return map_ && !tex_.id; }
    void release();   // free the texture (call before the window closes)

    struct View {
        Vector2 centre{};        // screen (logical UI) coords
        float radius = 110;      // px
        float rangeM = 1200;     // metres from the centre to the rim
        float x = 0, z = 0;      // rider, world metres
        float heading = 0;       // rider yaw (0 = +z)
        bool northUp = false;
        std::vector<Vector2> ahead;   // the road ahead, world x/z, from the rider on
        bool canZoomIn = true, canZoomOut = true;   // greys out the + / - dial buttons at the ends
    };
    enum class Hit { None, Map, ZoomIn, ZoomOut };
    // Draws the widget (with + / - dial buttons tucked under its lower rim); returns what was clicked.
    Hit draw(const View& v, float dt);
    // how far the widget reaches beyond `radius` (the dial buttons), for hit tests around it
    static float reach(float radius) { return radius * 0.06f + 30.f; }

    struct Bake {   // the CPU image, built off-thread (MiniMapBake.cpp)
        int R = 0;
        float L = 1, x0 = 0, z0 = 0;   // it spans [x0, x0 + L] in x and z
        std::vector<unsigned char> rgba;
    };
    // test / tooling hook: bake synchronously and write the image as a PNG
    static bool bakeToPng(const wmap::Map& m, const std::string& path, int maxRes = 4096);

private:
    std::shared_ptr<const wmap::Map> map_;
    std::future<std::shared_ptr<Bake>> job_;
    Texture2D tex_{};
    float L_ = 1, x0_ = 0, z0_ = 0;   // the texture spans [x0, x0 + L] in x and z
    float anim_ = 0;
    struct Town { float x, z; int rank; std::string name; };
    std::vector<Town> towns_;
};
