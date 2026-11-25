#pragma once
#include <ctime>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "Ble.h"
#include "Course.h"
#include "Db.h"
#include "Renderer.h"
#include "Store.h"
#include "Ui.h"
#include "core/Engine.h"
#include "core/Records.h"
#include "core/Route.h"
#include "core/Sim.h"
#include "core/Weather.h"
#include "core/Workout.h"

class App {
public:
    int run(int argc, char** argv);

private:
    enum class Screen { Home, Ride, Summary };
    enum Page { PHome, PRide, PRoutes, PWorkouts, PEvents, PActivities, PStats, PDevices, PProfile, PAGE_COUNT };

    // ---- lifecycle / flow (App.cpp)
    void loadData();
    void refreshHistory();
    void showRoute(const Route& r, bool force = false);
    void startRide();
    void startEvent(const Event& e);
    void endRide(bool save);
    void afterSave(RideRecord& rec);
    void reassessFtp(const std::optional<std::string>& trigger, bool allowAuto);
    void applyFtp(double ftp, const std::string& source);
    void frameUpdate(float dt);
    void updateWorld(float dt, double dist);
    SceneState scene();
    void handleRideKeys(float dt);
    void handleEvents();
    void hrAlerts();
    void toast(const std::string& s, Color c = ui::TEXT);
    void drawToasts();
    void handleDroppedFiles();
    TrainerInput currentInput(float dt);
    bool simActive() const;
    const wk::Workout* findWorkout(const std::string& id) const;
    std::vector<const wk::Workout*> allWorkouts() const;
    const Route* findRoute(const std::string& id) const;
    const RouteProfile& profileFor(const Route& r);
    void async(std::function<void()> work);          // run on a worker thread
    void onMain(std::function<void()> fn);           // queue for the main thread (thread-safe)
    void stravaUpload(const std::string& rideId, bool quiet);
    void fetchLiveWeather();
    void cycleWeather();
    void cycleTime();

    // ---- home + navigation (HomeUi.cpp)
    void drawShell();
    void drawSidebar(Rectangle r);
    void drawHomePage(Rectangle r);
    void drawRidePage(Rectangle r);

    // ---- routes + workouts (LibraryUi.cpp)
    void drawRoutesPage(Rectangle r);
    void drawRouteDetail(Rectangle r, const Route& rt);
    void drawRouteGenerator(Rectangle r);
    void drawRouteBuilder(Rectangle r);
    void drawWorkoutsPage(Rectangle r);
    void drawWorkoutEditor(Rectangle r);

    // ---- events (EventsUi.cpp)
    void drawEventsPage(Rectangle r);
    void drawEventEditor(Rectangle r);

    // ---- history (HistoryUi.cpp)
    void drawActivitiesPage(Rectangle r);
    void drawStatsPage(Rectangle r);
    void drawRideDetail(Rectangle r, RideRecord& rec, bool fresh);

    // ---- settings (SettingsUi.cpp)
    void drawDevicesPage(Rectangle r);
    void drawSimPanel(Rectangle r, bool compact);
    void drawProfilePage(Rectangle r);

    // ---- ride (RideUi.cpp)
    void drawRideHud();
    void drawDashboard2d();
    void drawStepPanel(Rectangle r);
    void drawSectorBanner(float x, float& y, float w);
    void drawEditDrawer();
    void drawRoadAhead(float x, float& y, float w);
    void drawPauseMenu();
    void drawHelp();

    // ---- summary (SummaryUi.cpp)
    void drawSummary();

    // state ---------------------------------------------------------------
    Profile prof_;
    Prefs prefs_;
    Db db_;
    BleManager ble_;
    Renderer renderer_;
    std::shared_ptr<Course> course_;
    std::string worldRouteKey_;
    Route worldRoute_;
    std::unique_ptr<sim::SimTrainer> sim_;
    std::vector<wk::Workout> builtin_, custom_;
    std::vector<Route> routes_;
    std::vector<Event> events_;
    std::vector<RideSummary> rides_;
    AccountStats stats_;
    std::optional<FtpAssessment> assessment_;
    std::map<std::string, RouteProfile> profileCache_;

    Screen screen_ = Screen::Home;
    int page_ = PHome;

    // ride setup
    RideKind setupKind_ = RideKind::Route;
    std::string selRoute_;
    int setupLaps_ = 1;
    bool raceGhost_ = true;
    bool sceneryEndless_ = true;
    std::string selWorkout_ = "builtin-sweet-spot-2x20";
    bool workoutResistance_ = false;
    wk::TestKind selTest_ = wk::TestKind::Ramp;
    FreeMode setupFreeMode_ = FreeMode::Gradient;
    Goal setupGoal_;
    std::optional<std::string> pendingEvent_;

    // routes page
    int routeView_ = 0;  // 0 library, 1 generator, 2 builder
    float routeScroll_ = 0;
    int routeFilter_ = 0;
    GenParams gen_;
    std::optional<Route> genPreview_;
    ui::TextBox genName_, builderName_, builderText_;
    std::vector<SegmentSpec> builderSegs_;
    std::string builderErr_;
    bool builderLoop_ = false;
    std::string routeDeleteArm_;

    // workouts page
    int wkCategory_ = 0;
    int wkSort_ = 0;
    std::string wkSelected_ = "builtin-sweet-spot-2x20";
    float wkScroll_ = 0;
    bool editorOpen_ = false;
    std::string editingId_;
    ui::TextBox edName_, edDesc_, edText_;
    std::string edErr_;

    // events page
    bool eventEditor_ = false;
    Event evDraft_;
    ui::TextBox evName_, evTime_;
    float evScroll_ = 0;
    int calMonthOffset_ = 0;
    std::string eventDeleteArm_;

    // activities / stats
    std::string selRide_;
    std::optional<RideRecord> detail_;
    float actScroll_ = 0;
    ui::TextBox renameBox_, notesBox_;
    bool deleteArm_ = false;
    bool curveWkg_ = false;
    int fitnessRange_ = 1;  // 0 6 weeks, 1 3 months, 2 1 year

    // profile page
    ui::TextBox stravaId_, stravaSecret_, placeBox_;
    std::vector<weather::Place> placeResults_;
    double ftpOnOpen_ = 0;
    int profileTab_ = 0;

    // ride
    std::unique_ptr<RideEngine> eng_;
    std::string rideStartedAt_;
    std::time_t rideStart_ = 0;
    int enginePlanVersion_ = 0;
    size_t routeSectorsSeen_ = 0;
    bool view3d_ = true;
    bool hudHidden_ = false;
    bool drawerOpen_ = false;
    int drawerTab_ = 0;
    bool testPanel_ = false;
    bool helpOpen_ = false;
    bool pauseMenu_ = false;
    bool hrAlertsOn_ = false;
    ui::TextBox insertText_;
    std::string insertErr_;
    SectorSpec sector_;
    CamMode cam_ = CamMode::Chase;
    double crank_ = 0, animTime_ = 0, lean_ = 0, stand_ = 0, tuck_ = 0, standTimer_ = 0;
    double lastAlertT_ = -1e9;
    int lastHrState_ = 0;

    // world: sky + weather
    weather::Sim wx_{1};
    double wxElapsed_ = 0;
    std::optional<weather::Live> live_;
    double liveFetchedAt_ = -1e9;
    bool liveFetching_ = false;
    double cycleHours_ = 0;   // day/night cycle clock
    SkyState sky_;
    WeatherFx fx_;
    double homeDist_ = 0;
    float thunderCooldown_ = 0;

    // summary
    std::optional<RideRecord> summaryRec_;
    std::string savedDir_;
    bool newPb_ = false, ftpApplied_ = false;

    // async
    std::mutex mainQm_;
    std::vector<std::function<void()>> mainQ_;
    std::vector<std::thread> threads_;
    std::map<std::string, std::string> uploadState_;  // ride id -> "uploading" / "processing" / error

    struct Toast { std::string text; double t; Color c; };
    std::deque<Toast> toasts_;
    std::string importMsg_;
};
