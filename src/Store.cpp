#include "Store.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>

#include "core/Json.h"

#ifdef CORERAY_SQLITE
#include <sqlite3.h>
#endif

namespace fs = std::filesystem;

namespace store {

static std::string home() {
    const char* h = std::getenv("HOME");
#ifdef _WIN32
    if (!h) h = std::getenv("USERPROFILE");
#endif
    return h ? h : ".";
}

std::string dir() {
    fs::path d = fs::path(home()) / ".coreray";
    std::error_code ec;
    fs::create_directories(d / "workouts", ec);
    fs::create_directories(d / "ghosts", ec);
    return d.string();
}

static std::map<std::string, std::string> readKv(const std::string& path) {
    std::map<std::string, std::string> kv;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return kv;
}

void load(Profile& p, Prefs& pr) {
    auto kv = readKv(dir() + "/profile.ini");
    auto num = [&](const char* k, double& v) { auto it = kv.find(k); if (it != kv.end()) try { v = std::stod(it->second); } catch (...) {} };
    auto inum = [&](const char* k, int& v) { double d = v; num(k, d); v = int(std::lround(d)); };
    auto flag = [&](const char* k, bool& v) { auto it = kv.find(k); if (it != kv.end()) v = it->second == "1"; };
    auto str = [&](const char* k, std::string& v) { auto it = kv.find(k); if (it != kv.end()) v = it->second; };
    str("name", p.name);
    num("weightKg", p.weightKg); num("bikeWeightKg", p.bikeWeightKg); inum("age", p.age); flag("male", p.male);
    num("ftp", p.ftp); num("maxHr", p.maxHr); num("restHr", p.restHr); flag("metric", p.metric);
    num("grossEfficiency", p.grossEfficiency); num("cda", p.cda); num("crr", p.crr);
    num("trainerDifficulty", p.trainerDifficulty); num("maxTrainerGrade", p.maxTrainerGrade); num("minTrainerGrade", p.minTrainerGrade);
    flag("virtualShifting", p.virtualShifting); num("wheelCircM", p.wheelCircM); inum("startGear", p.startGear);
    int zm = int(p.hrZoneModel); inum("hrZoneModel", zm); p.hrZoneModel = HrZoneModel(std::clamp(zm, 0, 2));
    num("lthr", p.lthr);
    flag("hrAlerts", p.hrAlerts); inum("hrAlertLo", p.hrAlertLo); inum("hrAlertHi", p.hrAlertHi);
    flag("hrAlertHigh", p.hrAlertHigh); flag("hrAlertLow", p.hrAlertLow); num("hrAlertGraceS", p.hrAlertGraceS); num("hrAlertRepeatS", p.hrAlertRepeatS);
    inum("quality", pr.quality); inum("renderScale", pr.renderScale); inum("timeOfDay", pr.timeOfDay); inum("camera", pr.camera); flag("startIn3d", pr.startIn3d);
    inum("courseSeed", pr.courseSeed); inum("scenery", pr.scenery); inum("mapSeed", pr.mapSeed); inum("mapSizeKm", pr.mapSizeKm); inum("mapClimate", pr.mapClimate); inum("freeWorld", pr.freeWorld);
    str("selRoute", pr.selRoute);
    inum("todMode", pr.todMode); num("todHour", pr.todHour); num("cycleMinutes", pr.cycleMinutes);
    inum("weatherMode", pr.weatherMode); str("weatherKind", pr.weatherKind);
    num("lat", pr.lat); num("lon", pr.lon); str("place", pr.place);
    inum("theme", pr.theme); inum("accent", pr.accent); inum("hudStyle", pr.hudStyle); inum("uiSize", pr.uiSize); inum("hudSize", pr.hudSize);
    flag("wildlife", pr.wildlife); flag("lanterns", pr.lanterns); flag("sound", pr.sound);
    flag("showFps", pr.showFps); flag("showPerf", pr.showPerf);
    num("weeklyGoalHours", p.weeklyGoalHours);
    int fa = int(p.ftpAuto); inum("ftpAuto", fa); p.ftpAuto = FtpAutoMode(std::clamp(fa, 0, 2));
    str("trainerId", pr.trainerId); str("shifterId", pr.shifterId); str("shifterName", pr.shifterName); str("trainerName", pr.trainerName); str("hrId", pr.hrId); str("hrName", pr.hrName);
    flag("simEnabled", pr.simEnabled);
    num("simEffortW", pr.sim.effortW); num("simPowerW", pr.sim.powerW); num("simHrBpm", pr.sim.hrBpm); num("simCadence", pr.sim.cadenceRpm);
    num("simVariability", pr.sim.variability);
    int ps = int(pr.sim.powerSource), pt = int(pr.sim.pattern), hs = int(pr.sim.hrSource), cs = int(pr.sim.cadenceSource);
    inum("simPowerSource", ps); inum("simPattern", pt); inum("simHrSource", hs); inum("simCadenceSource", cs);
    pr.sim.powerSource = sim::PowerSource(std::clamp(ps, 0, 1));
    pr.sim.pattern = sim::Pattern(std::clamp(pt, 0, 3));
    pr.sim.hrSource = sim::HrSource(std::clamp(hs, 0, 2));
    pr.sim.cadenceSource = sim::CadenceSource(std::clamp(cs, 0, 1));
}

void save(const Profile& p, const Prefs& pr) {
    std::ofstream f(dir() + "/profile.ini");
    f << "name=" << p.name << "\nweightKg=" << p.weightKg << "\nbikeWeightKg=" << p.bikeWeightKg << "\nage=" << p.age << "\nmale=" << p.male
      << "\nftp=" << p.ftp << "\nmaxHr=" << p.maxHr << "\nrestHr=" << p.restHr << "\nmetric=" << p.metric
      << "\ngrossEfficiency=" << p.grossEfficiency << "\ncda=" << p.cda << "\ncrr=" << p.crr << "\ntrainerDifficulty=" << p.trainerDifficulty
      << "\nmaxTrainerGrade=" << p.maxTrainerGrade << "\nminTrainerGrade=" << p.minTrainerGrade << "\nvirtualShifting=" << p.virtualShifting
      << "\nwheelCircM=" << p.wheelCircM << "\nstartGear=" << p.startGear << "\nhrZoneModel=" << int(p.hrZoneModel) << "\nlthr=" << p.lthr
      << "\nhrAlerts=" << p.hrAlerts << "\nhrAlertLo=" << p.hrAlertLo << "\nhrAlertHi=" << p.hrAlertHi << "\nhrAlertHigh=" << p.hrAlertHigh
      << "\nhrAlertLow=" << p.hrAlertLow << "\nhrAlertGraceS=" << p.hrAlertGraceS << "\nhrAlertRepeatS=" << p.hrAlertRepeatS
      << "\ntheme=" << pr.theme << "\naccent=" << pr.accent << "\nhudStyle=" << pr.hudStyle << "\nuiSize=" << pr.uiSize << "\nhudSize=" << pr.hudSize
      << "\nquality=" << pr.quality << "\nrenderScale=" << pr.renderScale << "\ntimeOfDay=" << pr.timeOfDay << "\ncamera=" << pr.camera << "\nstartIn3d=" << pr.startIn3d
      << "\ncourseSeed=" << pr.courseSeed << "\nscenery=" << pr.scenery << "\nmapSeed=" << pr.mapSeed << "\nmapSizeKm=" << pr.mapSizeKm << "\nmapClimate=" << pr.mapClimate << "\nfreeWorld=" << pr.freeWorld << "\nshifterId=" << pr.shifterId << "\nshifterName=" << pr.shifterName << "\ntrainerId=" << pr.trainerId << "\ntrainerName=" << pr.trainerName << "\nhrId=" << pr.hrId
      << "\nhrName=" << pr.hrName << "\nsimEnabled=" << pr.simEnabled << "\nsimEffortW=" << pr.sim.effortW << "\nsimPowerW=" << pr.sim.powerW
      << "\nsimHrBpm=" << pr.sim.hrBpm << "\nsimCadence=" << pr.sim.cadenceRpm << "\nsimVariability=" << pr.sim.variability
      << "\nsimPowerSource=" << int(pr.sim.powerSource) << "\nsimPattern=" << int(pr.sim.pattern) << "\nsimHrSource=" << int(pr.sim.hrSource)
      << "\nsimCadenceSource=" << int(pr.sim.cadenceSource) << "\nselRoute=" << pr.selRoute << "\ntodMode=" << pr.todMode
      << "\ntodHour=" << pr.todHour << "\ncycleMinutes=" << pr.cycleMinutes << "\nweatherMode=" << pr.weatherMode
      << "\nweatherKind=" << pr.weatherKind << "\nlat=" << pr.lat << "\nlon=" << pr.lon << "\nplace=" << pr.place
      << "\nwildlife=" << pr.wildlife << "\nlanterns=" << pr.lanterns << "\nsound=" << pr.sound
      << "\nshowFps=" << pr.showFps << "\nshowPerf=" << pr.showPerf
      << "\nweeklyGoalHours=" << p.weeklyGoalHours << "\nftpAuto=" << int(p.ftpAuto) << "\n";
}

// ------------------------------------------------------------------ custom workouts
static std::string safeId(const std::string& name) {
    std::string id;
    for (char c : name) id += std::isalnum((unsigned char)c) ? char(std::tolower((unsigned char)c)) : '-';
    if (id.empty()) id = "workout";
    return id;
}

std::vector<wk::Workout> loadCustomWorkouts() {
    std::vector<wk::Workout> out;
    std::error_code ec;
    for (auto& e : fs::directory_iterator(fs::path(dir()) / "workouts", ec)) {
        if (e.path().extension() != ".txt") continue;
        std::ifstream f(e.path());
        std::string line, body;
        wk::Workout w;
        w.id = "custom-" + e.path().stem().string();
        w.category = "Custom";
        while (std::getline(f, line)) {
            if (line.rfind("# name:", 0) == 0) w.name = line.substr(7);
            else if (line.rfind("# category:", 0) == 0) w.category = line.substr(11);
            else if (line.rfind("# description:", 0) == 0) w.description = line.substr(14);
            else if (line.rfind("#", 0) == 0) continue;
            else body += line + "\n";
        }
        auto trim = [](std::string& s) { while (!s.empty() && s.front() == ' ') s.erase(s.begin()); };
        trim(w.name); trim(w.category); trim(w.description);
        if (w.name.empty()) w.name = e.path().stem().string();
        std::string err;
        if (wk::parseText(body, 250, w.blocks, err)) out.push_back(w);
        else std::fprintf(stderr, "[store] %s: %s\n", e.path().string().c_str(), err.c_str());
    }
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.name < b.name; });
    return out;
}

bool saveCustomWorkout(const wk::Workout& w, std::string& err) {
    if (w.blocks.empty()) { err = "Empty workout"; return false; }
    std::string id = w.id.rfind("custom-", 0) == 0 ? w.id.substr(7) : safeId(w.name);
    std::ofstream f(fs::path(dir()) / "workouts" / (id + ".txt"));
    if (!f) { err = "Could not write workout file"; return false; }
    f << "# name: " << w.name << "\n# category: " << (w.category.empty() ? "Custom" : w.category) << "\n# description: " << w.description
      << "\n" << wk::toText(w.blocks) << "\n";
    return true;
}

void deleteCustomWorkout(const std::string& id) {
    std::error_code ec;
    if (id.rfind("custom-", 0) == 0) fs::remove(fs::path(dir()) / "workouts" / (id.substr(7) + ".txt"), ec);
}

// ------------------------------------------------------------------ ghosts
static fs::path ghostPath(int seed, int laps) {
    return fs::path(dir()) / "ghosts" / ("course" + std::to_string(seed) + "_laps" + std::to_string(laps) + ".bin");
}

std::vector<float> loadGhost(int seed, int laps, double* best) {
    std::ifstream f(ghostPath(seed, laps), std::ios::binary);
    std::vector<float> v;
    float x;
    while (f.read(reinterpret_cast<char*>(&x), sizeof x)) v.push_back(x);
    if (best) *best = v.empty() ? 0 : double(v.size());
    return v;
}

bool saveGhostIfBest(int seed, int laps, const std::vector<float>& d) {
    if (d.empty()) return false;
    auto old = loadGhost(seed, laps);
    if (!old.empty() && old.size() <= d.size()) return false;
    std::ofstream f(ghostPath(seed, laps), std::ios::binary);
    f.write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size() * sizeof(float)));
    return true;
}

// ------------------------------------------------------------------ CoreKick import
std::string coreKickDbPath() {
#ifdef __APPLE__
    return home() + "/Library/Application Support/com.corekick.app/corekick.db";
#elif defined(_WIN32)
    const char* a = std::getenv("APPDATA");
    return std::string(a ? a : ".") + "\\com.corekick.app\\corekick.db";
#else
    return home() + "/.local/share/com.corekick.app/corekick.db";
#endif
}

bool coreKickAvailable() {
#ifdef CORERAY_SQLITE
    std::error_code ec;
    return fs::exists(coreKickDbPath(), ec);
#else
    return false;
#endif
}

static bool blockFromJson(const json::Value& v, wk::Block& b) {
    auto t = v.str("type").value_or("");
    auto I = [&](const char* k) { return int(std::lround(v.num(k).value_or(0))); };
    auto D = [&](const char* k) { return v.num(k).value_or(0); };
    std::optional<int> cad;
    if (auto c = v.num("cadence")) cad = int(*c);
    if (auto c = v.num("on_cadence")) cad = int(*c);
    std::string label = v.str("label").value_or("");
    if (t == "steady") b = wk::Block::steady(I("duration_s"), D("power"), cad, label);
    else if (t == "ramp") b = wk::Block::ramp(I("duration_s"), D("from"), D("to"), label);
    else if (t == "intervals") b = wk::Block::intervals(I("repeats"), I("on_s"), D("on_power"), I("off_s"), D("off_power"), cad);
    else if (t == "free") b = wk::Block::free(I("duration_s"), D("grade_pct"), v.boolean("effort").value_or(false), label);
    else if (t == "set") {
        std::vector<wk::Block> inner;
        if (auto bl = v.get("blocks"))
            for (auto& x : bl->a) { wk::Block ib; if (blockFromJson(x, ib)) inner.push_back(ib); }
        b = wk::Block::set(I("repeats"), inner);
    } else return false;
    return true;
}

bool importCoreKick(Profile& p, std::vector<wk::Workout>& imported, std::string& msg) {
#ifndef CORERAY_SQLITE
    (void)p; (void)imported;
    msg = "Built without SQLite support";
    return false;
#else
    sqlite3* db = nullptr;
    if (sqlite3_open_v2(coreKickDbPath().c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        msg = "Could not open CoreKick database";
        if (db) sqlite3_close(db);
        return false;
    }
    auto query = [&](const char* sql, auto fn) {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &st, nullptr) != SQLITE_OK) return;
        while (sqlite3_step(st) == SQLITE_ROW) {
            const unsigned char* t = sqlite3_column_text(st, 0);
            if (t) fn(std::string(reinterpret_cast<const char*>(t)));
        }
        sqlite3_finalize(st);
    };
    bool gotProfile = false;
    query("SELECT json FROM profile WHERE id = 1", [&](const std::string& j) {
        json::Value v;
        if (!json::parse(j, v)) return;
        gotProfile = true;
        if (auto x = v.str("name")) p.name = *x;
        if (auto x = v.num("weight_kg")) p.weightKg = *x;
        if (auto x = v.num("bike_weight_kg")) p.bikeWeightKg = *x;
        if (auto x = v.num("age")) p.age = int(*x);
        if (auto x = v.str("sex")) p.male = *x != "female";
        if (auto x = v.num("ftp")) p.ftp = *x;
        if (auto x = v.num("max_hr")) p.maxHr = *x;
        if (auto x = v.num("rest_hr")) p.restHr = *x;
        if (auto x = v.str("units")) p.metric = *x != "imperial";
        if (auto x = v.num("gross_efficiency")) p.grossEfficiency = *x;
        if (auto x = v.num("cda")) p.cda = *x;
        if (auto x = v.num("crr")) p.crr = *x;
        if (auto x = v.num("trainer_difficulty")) p.trainerDifficulty = *x;
        if (auto x = v.num("max_trainer_grade")) p.maxTrainerGrade = *x;
        if (auto x = v.num("min_trainer_grade")) p.minTrainerGrade = *x;
        if (auto x = v.boolean("virtual_shifting")) p.virtualShifting = *x;
        if (auto x = v.num("wheel_circumference_m")) p.wheelCircM = *x;
        if (auto x = v.num("start_gear")) p.startGear = int(*x);
        if (auto x = v.str("hr_zone_model")) p.hrZoneModel = *x == "lthr" ? HrZoneModel::Lthr : *x == "reserve" ? HrZoneModel::Reserve : HrZoneModel::MaxHr;
        if (auto x = v.num("lthr")) p.lthr = *x;
        if (auto a = v.get("hr_alerts")) {
            if (auto x = a->boolean("enabled")) p.hrAlerts = *x;
            if (auto x = a->num("low_zone")) p.hrAlertLo = int(*x);
            if (auto x = a->num("high_zone")) p.hrAlertHi = int(*x);
            if (auto x = a->boolean("alert_high")) p.hrAlertHigh = *x;
            if (auto x = a->boolean("alert_low")) p.hrAlertLow = *x;
            if (auto x = a->num("grace_s")) p.hrAlertGraceS = *x;
            if (auto x = a->num("repeat_s")) p.hrAlertRepeatS = *x;
        }
    });
    int n = 0;
    query("SELECT json FROM workouts", [&](const std::string& j) {
        json::Value v;
        if (!json::parse(j, v)) return;
        if (v.boolean("builtin").value_or(false)) return;
        wk::Workout w;
        w.name = v.str("name").value_or("CoreKick workout");
        w.description = v.str("description").value_or("");
        w.category = v.str("category").value_or("Custom");
        w.id = "custom-ck-" + safeId(w.name);
        if (auto bl = v.get("blocks"))
            for (auto& x : bl->a) { wk::Block b; if (blockFromJson(x, b)) w.blocks.push_back(b); }
        std::string err;
        if (!w.blocks.empty() && saveCustomWorkout(w, err)) { imported.push_back(w); ++n; }
    });
    sqlite3_close(db);
    char b[160];
    std::snprintf(b, sizeof b, "%s%d custom workout%s imported from CoreKick", gotProfile ? "Profile and " : "", n, n == 1 ? "" : "s");
    msg = b;
    return gotProfile || n > 0;
#endif
}

std::vector<SavedWorld> loadSavedWorlds() {
    std::vector<SavedWorld> out;
    std::ifstream f(fs::path(dir()) / "saved_worlds.json");
    if (!f) return out;
    std::string txt((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    json::Value v;
    if (!json::parse(txt, v)) return out;
    for (const auto& e : v.arr("worlds")) {
        SavedWorld w;
        w.id = e.strOr("id"); w.name = e.strOr("name"); w.savedAt = e.strOr("saved_at");
        w.seed = uint32_t(e.numOr("seed", 1)); w.sizeKm = int(e.numOr("size_km", 20)); w.climate = int(e.numOr("climate", 0));
        w.cell = float(e.numOr("cell_m", 20)); w.erosion = float(e.numOr("erosion", 1)); w.aridity = float(e.numOr("aridity", 0)); w.roads = e.boolOr("roads", true);
        w.generator = int(e.numOr("generator_version", 0));
        if (auto s = e.get("start"); s && s->type == json::Value::Object) {
            w.hasStart = true;
            w.road = int(s->numOr("road", -1)); w.idx = int(s->numOr("point", 0)); w.fwd = s->boolOr("forwards", true);
            w.startX = float(s->numOr("x", 0)); w.startZ = float(s->numOr("z", 0));
            w.headX = float(s->numOr("heading_x", 0)); w.headZ = float(s->numOr("heading_z", 1));
            w.startLabel = s->strOr("label");
        }
        w.towns = int(e.numOr("places", 0)); w.roadKm = float(e.numOr("road_km", 0));
        if (!w.id.empty()) out.push_back(w);
    }
    return out;
}

bool saveSavedWorlds(const std::vector<SavedWorld>& ws) {
    json::Value root = json::Value::object();
    root["format"] = 1;
    json::Value arr = json::Value::array();
    for (const auto& w : ws) {
        json::Value e = json::Value::object();
        e["id"] = w.id; e["name"] = w.name; e["saved_at"] = w.savedAt;
        e["seed"] = (unsigned long long)w.seed; e["size_km"] = w.sizeKm; e["climate"] = w.climate;
        e["cell_m"] = double(w.cell); e["erosion"] = double(w.erosion); e["aridity"] = double(w.aridity); e["roads"] = w.roads;
        e["generator_version"] = w.generator;
        if (w.hasStart) {
            json::Value s = json::Value::object();
            s["road"] = w.road; s["point"] = w.idx; s["forwards"] = w.fwd;
            s["x"] = double(w.startX); s["z"] = double(w.startZ); s["heading_x"] = double(w.headX); s["heading_z"] = double(w.headZ);
            s["label"] = w.startLabel;
            e["start"] = s;
        }
        e["places"] = w.towns; e["road_km"] = double(w.roadKm);
        arr.push(e);
    }
    root["worlds"] = arr;
    // write a temporary file, then swap it in (no half-written list if we crash mid-write)
    fs::path path = fs::path(dir()) / "saved_worlds.json", tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp);
        if (!f) return false;
        f << json::dump(root);
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

}  // namespace store
