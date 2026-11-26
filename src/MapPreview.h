#pragma once
// A miniature 3D model of a generated map (wmap) for choosing where a ride starts:
// a shaded terrain block with water, main / minor roads, bridges, tunnels and towns,
// orbit (drag), zoom (wheel) and pan (right-drag). Hovering snaps to the nearest road;
// clicking places the spawn pin there. Heavy sampling runs on a worker thread.
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "raylib.h"

namespace wmap { class Map; }

class MapPreview {
public:
    struct Spawn {
        bool set = false;
        int road = -1, idx = 0;   // point on map->roads[road].pts
        bool fwd = true;          // ride towards the road's end (b) or its start (a)
    };

    MapPreview() = default;
    ~MapPreview();
    MapPreview(const MapPreview&) = delete;
    MapPreview& operator=(const MapPreview&) = delete;

    // Show this map (rebuilds when the map changes). nullptr clears.
    void setMap(std::shared_ptr<const wmap::Map> m);
    bool building() const { return map_ && !uploaded_; }
    bool ready() const { return map_ && uploaded_; }
    void resetView();
    void release();   // free GPU objects (call before the window closes)

    // Draws the viewport into r (screen coords) and handles its input.
    // Returns true when the spawn changed (a click on a road).
    bool draw(Rectangle r, Spawn& spawn, float dt);
    bool hasHover() const { return hover_.set; }
    Spawn hover() const { return hover_; }
    bool interacted() const { return interacted_; }   // the rider has rotated / zoomed the map

    struct Cpu;   // built off-thread
private:
    void upload();
    void unloadGpu();
    float heightN(float nx, float nz) const;   // model-space height at model-space x/z
    Vector3 roadPoint(int road, int idx) const; // model space, on top of the drawn ribbon
    bool pick(Vector2 mouse, Rectangle r, Spawn& out) const;

    std::shared_ptr<const wmap::Map> map_;
    std::future<std::shared_ptr<Cpu>> job_;
    std::shared_ptr<Cpu> cpu_;
    bool uploaded_ = false;
    std::vector<Mesh> meshes_;   // terrain block + road ribbons
    Material mat_{};
    bool matLoaded_ = false;
    RenderTexture2D rt_{};
    int rtW_ = 0, rtH_ = 0;

    // camera (model space: the map spans -0.5..0.5 in x and z)
    float yaw_ = 0.65f, pitch_ = 0.9f, dist_ = 2.0f, tx_ = 0, tz_ = 0;
    float cyaw_ = 0.65f, cpitch_ = 0.9f, cdist_ = 2.0f, ctx_ = 0, ctz_ = 0;   // smoothed
    bool dragging_ = false, rdragging_ = false, dragMoved_ = false;
    Vector2 dragStart_{};
    Spawn hover_;
    Camera3D cam_{};
    float camMetresPerPx_ = 10;
    float anim_ = 0;
    bool interacted_ = false;
};
