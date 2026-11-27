#pragma once
// Local persistence (~/.coreray): profile, app preferences, custom workouts, ghosts,
// and a one-shot import of the rider profile + custom workouts from CoreKick's database.
#include <string>
#include <vector>

#include "core/Engine.h"
#include "core/Sim.h"
#include "core/Workout.h"

struct Prefs {
    int quality = 2;        // 0 battery, 1 balanced, 2 high
    int renderScale = 0;    // 3D resolution: 0 auto (75 % on Retina), 1 100 %, 2 85 %, 3 75 %, 4 50 %
    int timeOfDay = 1;      // renderer preset
    int camera = 0;
    bool startIn3d = true;
    int courseSeed = 7;
    int scenery = 0;        // 0: land grown around each road, 1: a generated 20 km map (wmap)
    int mapSeed = 1;        // which generated map
    int mapSizeKm = 20;     // its size (10, 20, 30 or 40 km square)
    int mapClimate = 0;     // 0 the seed decides, 1 temperate, 2 desert, 3 cold
    int freeWorld = 0;      // free rides: 0 the endless road, 1 the generated map (start where you pick)
    std::string selRoute;
    std::string hudLayout;  // moved / resized ride HUD widgets (hud::Layout::save)
    // world
    int todMode = 1;           // 0 real time at your location, 1 fixed time, 2 day/night cycle
    double todHour = 13.0;     // fixed time / cycle start (local hours)
    double cycleMinutes = 30;  // real minutes per in-game day
    int weatherMode = 0;       // 0 changing (auto), 1 live at your location, 2 fixed
    std::string weatherKind = "fair";
    double lat = 51.48, lon = -0.0;
    std::string place = "Greenwich, England";
    bool wildlife = true, lanterns = true, sound = true;
    // appearance
    int theme = 0;        // ui::Theme: 0 light, 1 dark, 2 midnight, 3 sand
    int accent = 0;       // ui::ACCENTS index
    int hudStyle = 0;     // ui::HudStyle: 0 match theme, 1 light glass, 2 dark glass, 3 day / night
    int uiSize = 110;     // % on top of the screen-size auto scale (menus)
    int hudSize = 130;    // % for the ride HUD
    bool showFps = false, showPerf = false;   // in-ride FPS counter (F) and frame-time panel (P)
    bool miniMap = true;    // generated worlds: the circular minimap in rides (N; click it to zoom)
    int miniMapZoom = 1;    // 0..3: 600 m, 1.2 km, 2.5 km, 5 km from the centre to the rim
    bool miniMapNorthUp = false;   // false: heading-up (Shift+N)
    std::string trainerId, trainerName, hrId, hrName, shifterId, shifterName;
    bool simEnabled = false;
    sim::Settings sim;
};

namespace store {

std::string dir();
void load(Profile& p, Prefs& prefs);
void save(const Profile& p, const Prefs& prefs);

// custom workouts are kept in the text format, one file each
std::vector<wk::Workout> loadCustomWorkouts();
bool saveCustomWorkout(const wk::Workout& w, std::string& err);
void deleteCustomWorkout(const std::string& id);

// ghost = distance at each second of the best completed ride of this course & lap count
std::vector<float> loadGhost(int courseSeed, int laps, double* bestTimeS = nullptr);
bool saveGhostIfBest(int courseSeed, int laps, const std::vector<float>& distances);  // true if new PB

// Generated worlds the rider has kept (~/.coreray/saved_worlds.json). A world is fully
// determined by its generator inputs + the generator version; the start is a road point,
// with its map position and heading so it can be found again if the generator changes.
struct SavedWorld {
    std::string id, name, savedAt;
    uint32_t seed = 1;
    int sizeKm = 20, climate = 0;
    float cell = 20, erosion = 1, aridity = 0;   // generator inputs that are fixed in the app today
    bool roads = true;
    int generator = 0;                           // wmap::VERSION it was made with
    bool hasStart = false;
    int road = -1, idx = 0;
    bool fwd = true;
    float startX = 0, startZ = 0, headX = 0, headZ = 1;
    int towns = 0;
    float roadKm = 0;
    std::string startLabel;                      // e.g. "Minor road near Highfield"
};
std::vector<SavedWorld> loadSavedWorlds();
bool saveSavedWorlds(const std::vector<SavedWorld>& w);

// CoreKick (~/Library/Application Support/com.corekick.app/corekick.db)
std::string coreKickDbPath();
bool coreKickAvailable();
// Imports profile fields and custom workouts; returns a human-readable summary
bool importCoreKick(Profile& p, std::vector<wk::Workout>& imported, std::string& message);

}  // namespace store

// the 3D resolution setting as a fraction (0 = auto)
inline float renderScaleOf(int setting) {
    static const float v[] = {0.f, 1.f, 0.85f, 0.75f, 0.5f};
    return setting >= 0 && setting < 5 ? v[setting] : 0.f;
}
