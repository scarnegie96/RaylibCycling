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
    int timeOfDay = 1;      // renderer preset
    int camera = 0;
    bool startIn3d = true;
    int courseSeed = 7;
    std::string selRoute;
    // world
    int todMode = 1;           // 0 real time at your location, 1 fixed time, 2 day/night cycle
    double todHour = 13.0;     // fixed time / cycle start (local hours)
    double cycleMinutes = 30;  // real minutes per in-game day
    int weatherMode = 0;       // 0 changing (auto), 1 live at your location, 2 fixed
    std::string weatherKind = "fair";
    double lat = 51.48, lon = -0.0;
    std::string place = "Greenwich, England";
    bool wildlife = true, lanterns = true, sound = true;
    bool showFps = false, showPerf = false;   // in-ride FPS counter (F) and frame-time panel (P)
    std::string trainerId, trainerName, hrId, hrName;
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

// CoreKick (~/Library/Application Support/com.corekick.app/corekick.db)
std::string coreKickDbPath();
bool coreKickAvailable();
// Imports profile fields and custom workouts; returns a human-readable summary
bool importCoreKick(Profile& p, std::vector<wk::Workout>& imported, std::string& message);

}  // namespace store
