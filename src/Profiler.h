#pragma once
// Frame profiler for the in-ride performance panel.
//
// GPU time per category comes from GL timestamp queries (GL 3.3 core / ARB_timer_query,
// fetched through GLFW since raylib doesn't expose them): a timestamp at every section
// boundary, read back three frames later so nothing ever stalls. raylib batches immediate
// draws, so every mark flushes the batch first and the work lands in the right section.
// Drivers differ (Apple Silicon's GL reports timestamp support but returns nothing), so it
// picks the best method the driver claims and steps down when one keeps giving nothing:
// timestamps -> elapsed-time queries -> glFinish-synced wall time -> CPU only.
// It costs nothing while the panel is hidden (no queries, no flushes).
#include <cstdint>

namespace prof {

enum Section {
    Update,       // CPU: world streaming, grass, wildlife, particles, lights (before any drawing)
    Shadows,      // sun shadow cascades + cloud shadow map
    Sky,
    Terrain,
    Road,
    Foliage,      // trees, rocks
    Grass,
    Props,        // posts, arches, bridges, lamps, wildlife, rider, birds
    Water,
    Volumetrics,  // clouds, fog, light cones, rain shafts (march + resolve + upsample)
    Particles,    // rain, snow, splashes, lamp glare
    Post,         // bloom, composite, FXAA
    Hud,
    COUNT
};
const char* name(Section s);

void setEnabled(bool on);
bool enabled();
void beginFrame();          // start of the frame (CPU clock + first timestamp)
void mark(Section s);       // everything since the previous mark belongs to `s`
void endFrame();            // after the HUD: closes the frame, harvests old results

struct Stats {
    float ms[COUNT] = {};   // smoothed per section (GPU where available)
    float frameMs = 0;      // smoothed CPU frame time
    float gpuMs = 0;        // smoothed sum of the GPU sections
    bool gpu = false;       // timings come from GPU queries
    const char* method = "";   // how the sections are being timed
};
const Stats& stats();

}  // namespace prof
