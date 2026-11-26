#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "Course.h"
#include "Textures.h"
#include "Trees.h"
#include "Rider.h"
#include "body/Body.h"
#include <future>
#include "raylib.h"

// CoreKick's five (chase, cinematic, side, rider, drone) plus a close chase and the rider's eyes
enum class CamMode { Chase, Cinematic, Side, Rider, Drone, Close, FirstPerson, COUNT };
const char* camModeName(CamMode m);

// Where the sun and moon are, and how "night" it is (0 day .. 1 full night, CoreKick's palette scale).
struct SkyState {
    Vector3 sunDir{0.3f, 0.75f, 0.6f};
    float sunElevation = 45;  // degrees
    Vector3 moonDir{-0.3f, 0.5f, -0.8f};
    float moonElevation = -10, moonIllum = 0.5f;
    bool moonWaxing = true;
    float tod = 0;
};

// Continuous weather state (weather::State + Sim extras), all 0..1 except windDir (radians).
struct WeatherFx {
    float cloud = 0.3f, dark = 0, rain = 0, snow = 0, fog = 0, wind = 0.3f, storm = 0, hail = 0, gust = 0;
    float wet = 0, snowCover = 0, flash = 0, rainbow = 0, windDir = 0.6f;
};

struct SceneState {
    double dist = 0;         // rider distance along course (m)
    double speed = 0;        // m/s
    double crankAngle = 0;   // radians
    double wheelAngle = 0;   // radians
    double time = 0;         // seconds (animation)
    float effort = 0;        // 0..1 for body rocking
    CamMode cam = CamMode::Chase;
    double pacerDist = -1;   // <0 = no pacer
    float lean = 0;
    float stand = 0;
    float tuck = 0;
    SkyState sky;
    WeatherFx wx;
    bool wildlife = true;
    bool lanterns = true;
};

// vegetation instance sets: tree types, then bushes, then rocks
constexpr int VEG_BUSH0 = trees::TYPES, VEG_ROCK0 = trees::TYPES + trees::BUSHES, VEG_SETS = trees::TYPES + trees::BUSHES + trees::ROCKS;

class Renderer {
public:
    Renderer();
    ~Renderer();
    bool init(int quality);
    void shutdown();
    void setQuality(int q);
    // 3D resolution as a fraction of the window's pixels (0 = auto: 75 % on high-DPI screens)
    // the rider: greybox human (true) or the kitted RiderRig
    void setGreyboxRider(bool on) { greyOn_ = on; }
    bool greyboxRider() const { return greyOn_; }
    void setRenderScale(float s) { renderScale_ = s; }
    float renderScale() const;
    int renderWidth() const { return rtW_; }
    int renderHeight() const { return rtH_; }
    void setCourse(std::shared_ptr<const Course> c);         // new road; streams in the new world
    void updateCourse(std::shared_ptr<const Course> c);      // same world, road ahead re-laid
    // endless road grew: only chunks inside the box (x0,z0,x1,z1) are rebuilt
    void extendCourse(std::shared_ptr<const Course> c, float x0, float z0, float x1, float z1);
    void preload(double dist);                               // block until the land around `dist` is built
    float loadProgress() const;
    void render(const SceneState& s, float dt);
    Camera3D camera() const { return cam_; }
    int triangles() const { return tris_; }
    // a lightning strike: a flickering ball of light inside a storm cloud (and sometimes a bolt
    // to the ground under it). Returns where it is, for the thunder.
    struct Strike { float dist = 3000, pan = 0; };
    Strike lightning(float azimuth);
    // what the rider hears: rain / hail falling here, showers off to either side
    struct PrecipSound { float rain = 0, hail = 0, farL = 0, farR = 0; };
    PrecipSound precipSound() const { return precipSound_; }

private:
    // ---- world streaming (World.cpp)
    struct ChunkData;  // CPU side, built by workers
    struct Chunk {
        int cx = 0, cz = 0, lod = -1, version = -1;
        Mesh mesh{}, water{};
        bool fine = false;  // built with the particle-eroded surface
        Vector3 center{};
        float radius = 0;
        bool hasVeg = false;
        std::vector<Matrix> veg[VEG_SETS];   // tree types..., bushes..., rocks...
        std::vector<Vector3> vegPos[VEG_SETS];
        std::vector<float> hgrid, rgrid, wgrid;  // full-detail chunks keep height, road distance, water depth (grass)
        int gridQ = 0;
    };
    struct GrassTile { std::vector<Matrix> grass, flowers; Vector3 center{}; int version = -1; };
    struct Light { Vector3 pos, col, dir; float cosIn = -1, cosOut = -1; Vector3 axis{0, 0, 1}; float elong = 1; float vol = 1; };  // axis/elong: beam stretched along the road
    struct Splash { Vector3 p; float t, size; };
    struct AnimalMeshes { Mesh body{}, head{}, upper{}, lower{}, tail{}, ear{}; float hipY = 0, shoulderY = 0, hipZ = 0, shoulderZ = 0, hipX = 0, upperLen = 0, lowerLen = 0; Vector3 neckPivot{}, tailRoot{}; };
    struct Job { int cx, cz, lod, version; std::shared_ptr<const Course> course; float prio; };
    struct RoadChunk { int first = 0, version = -1; Mesh mesh{}; Vector3 center{}; float radius = 0; };
    struct Lantern { Vector3 pos; float dist; float side; };
    struct BridgeVis { Vector3 c; float heading, span, level, width, grade; };
    std::vector<BridgeVis> bridges_;
    int bridgesFor_ = -1;  // road chunk set the list was built for

    void startWorkers();
    void stopWorkers();
    void workerLoop();
    static ChunkData buildChunk(const Job& j, int quality);
    void streamWorld(const Vector3& focus, double riderDist, int maxUploads);
    void uploadChunk(ChunkData&& d);
    void updateRoad(double riderDist);
    void buildMeshes();
    void drawWorld(bool shadowPass, int cascade, const Vector3& focus, float radius);
    // far trees as impostors (Impostors.cpp)
    static constexpr int IMP_VIEWS = 8;
    struct ImpInfo { float size = 10, cy = 5; } imp_[trees::TYPES];
    Shader impSh_{};
    Material impMat_{};
    Texture2D impAlb_{}, impNrm_{};
    Mesh impQuad_{};
    std::vector<Matrix> visFar_;
    void bakeImpostors();
    void unloadImpostors();
    void drawImpostors(int set, const std::vector<Matrix>& inst);
    void drawLanterns(bool shadowPass, const Vector3& focus, float radius);
    void drawLightShafts();
    void updateLanterns(const SceneState& s, float dt);
    void updateGrass();
    void drawGrass(const SceneState& s);
    bool groundAt(float x, float z, float& y, float& rd, float* wet = nullptr) const;  // from full-detail chunk grids

    // ---- effects & wildlife (Fx.cpp)
    struct Particle { Vector3 p; float seed; };
    struct Animal {
        int kind;            // 0 sheep, 1 cow, 2 deer, 3 horse
        Vector3 pos;
        float heading, phase, speed = 0, graze = 0, fleeT = -1, scale = 1;
        Color coat;
        Color coatB{230, 222, 210, 255};
        float patch = 0, seed = 0, gait = 0;
    };
    struct Herd { int seg; std::vector<Animal> animals; };
    struct Bird { Vector3 c; float radius, height, speed, phase, size; bool raptor; };
    void updateWildlife(const SceneState& s, float dt);
    void drawWildlife(bool shadowPass, const Vector3& focus, float radius);
    void drawAnimal(const Animal& a, bool shadowPass);
    void buildAnimalMeshes();
    void drawAnimalPart(const Mesh& m, const Matrix& local, const Matrix& base, const Animal& a, bool shadowPass);
    void drawBirds(const SceneState& s);
    void updateParticles(const SceneState& s, float dt);
    void drawParticles(const SceneState& s);
    void drawBolt();

    // ---- volumetric clouds / fog / light cones / rain shafts (Volumetrics.cpp)
    struct CloudParams {
        float coverage = 0.3f, type = 0.5f, base = 1500, top = 2500, sigma = 0.05f, precip = 0, scale = 26000;
        float precipArea = 0;   // fraction of the sky raining / snowing (the weather's rain cells)
        float here = 1;         // 0..1: how much of that falls on the rider right now (cell overhead?)
        Vector2 wind{};
    };
    void renderCloudShadow();
    float precipCellAt(float x, float z) const;   // CPU copy of the weather map's rain cells
    void initVolumetrics();
    void unloadVolumetrics();
    void createVolTargets();
    void destroyVolTargets();
    void updateCloudParams(const SceneState& s, float dt);
    void renderVolumetrics(const SceneState& s);
    bool volEnabled() const { return quality_ >= 1 && volShape_.id != 0 && !volDisabled_; }
    void setLightUniforms(Shader sh);
    Shader volMarchSh_{}, volResolveSh_{}, volApplySh_{}, volShadowSh_{};
    RenderTexture2D csm_{}, csmTmp_{};   // cloud shadow map (+ blur scratch)
    Vector4 csmRect_{0, 0, 1.f / 16000.f, 1500};
    std::vector<unsigned char> cloudPx_;   // weather map texels (r coverage, g type, b rain cells)
    int cloudPxN_ = 0;
    Texture2D volShape_{}, volDetail_{}, cloudMap_{};
    Vector4 volShapeDim_{}, volDetailDim_{};
    RenderTexture2D volCur_{}, volHist_[2]{}, sceneFx_{};
    int volW_ = 0, volH_ = 0, volHistIdx_ = 0, volFrame_ = 0;
    bool volHistValid_ = false, volDisabled_ = false;
    Matrix prevVP_{};
    CloudParams cloud_;
    // the course's height range, eased: endless roads grow it a kilometre at a time and the cloud
    // base, snow line and terrain altitude zones shouldn't jump when they do
    float refMin_ = 0, refMax_ = 0;
    bool refInit_ = false;

    // ---- frame (Renderer.cpp)
    void createTargets();
    void destroyTargets();
    void updateCamera(const SceneState& s, float dt);
    void computeLighting(const SceneState& s);
    void setCommonUniforms(Shader sh);
    void drawRider(const Matrix& base, const SceneState& s, bool ghost, bool shadowPass);
    void drawPart(const Mesh& m, Matrix local, const Matrix& base, Color c, float rough, float spec, bool shadowPass, int pattern = 0, float emissive = 0);
    void drawLimb(Vector3 a, Vector3 b, float r, const Matrix& base, Color c, bool shadowPass, float rough = 0.7f, float spec = 0.2f);
    void renderShadowMaps(const SceneState& s);
    void postProcess(const SceneState& s);
    Matrix riderBase(const SceneState& s) const;

    std::shared_ptr<const Course> course_;
    int quality_ = 2;
    bool ready_ = false;

    struct Lighting {
        Vector3 lightDir, lightColor, sunDir, zenith, horizon, ground, moonDir, sunRaw;  // sunRaw: sunlight above the clouds
        float ambient, fog, exposure, clouds, night, moonIllum, stars, sunVisible, moonWaxing, fogGrey;
    } L_{};

    // shaders
    Shader terrainSh_{}, roadSh_{}, objectSh_{}, foliageSh_{}, waterSh_{}, skySh_{};
    Shader depthSh_{}, depthInstSh_{};
    Shader grassSh_{}, animalSh_{}, volumeSh_{}, particleSh_{}, kitSh_{};
    Material kitMat_{};
    RiderRig rig_, ghostRig_;
    // greybox human (src/body): built off-thread at start, skinned on the CPU each frame
    std::future<body::Body> greyFuture_;
    body::Body grey_;
    bool greyReady_ = false, greyOn_ = true;
    Mesh greyMesh_{};
    body::Globals greyG_;
    std::vector<body::V3> greyPos_, greyNrm_;
    void poseGrey(const SceneState& s);
    void drawGrey(const Matrix& base, bool shadowPass);
    Material grassMat_{}, animalMat_{}, volumeMat_{};
    tex::Set tex_;
    Mesh tuft_{}, flowerTuft_{}, shaftCone_{}, pyramid_{};
    AnimalMeshes animals_[4];
    std::map<std::pair<int, int>, GrassTile> grass_;
    std::vector<Splash> splashes_;
    Vector3 riderPos_{};
    float splashAcc_ = 0, shaftDensity_ = 0.4f;
    WeatherFx wxNow_{};
    Shader brightSh_{}, blurSh_{}, compositeSh_{}, fxaaSh_{};
    Material terrainMat_{}, roadMat_{}, objectMat_{}, foliageMat_{}, waterMat_{}, skyMat_{}, depthMat_{}, depthInstMat_{};

    // world
    std::map<std::pair<int, int>, Chunk> chunks_;
    std::vector<RoadChunk> road_;
    std::vector<Lantern> lanterns_;
    std::unordered_map<int, float> lit_;   // lantern index -> time it lit
    std::vector<Light> lights_;  // lanterns + headlight this frame
    int courseVersion_ = 0, worldId_ = 0;
    Mesh water_{}, skyDome_{};
    Mesh cyl_{}, sphere_{}, cube_{}, cone_{}, wheelTire_{}, wheelRim_{};
    Mesh rocks_[trees::ROCKS]{};
    trees::Built trees_[trees::TYPES];
    trees::Built bushes_[trees::BUSHES];
    Texture2D leafAtlas_{};
    std::vector<Matrix> visHi_, visLo_;
    std::vector<Matrix> signposts_;
    Matrix archBase_{}, finishBase_{};
    bool hasFinish_ = false;
    double riderDist_ = 0;
    float wind_ = 1;
    Vector3 streamFocus_{};

    // workers
    std::vector<std::thread> workers_;
    std::mutex qm_;
    std::condition_variable qcv_;
    std::deque<Job> jobs_;
    std::vector<ChunkData*> done_;
    std::map<std::pair<int, int>, int> inFlight_;  // chunk -> lod being built
    std::set<std::pair<int, int>> eroding_;        // erosion tiles queued / running
    bool quit_ = false;
    std::atomic<int> active_{0};
    int wantedNear_ = 1, haveNear_ = 0;

    // fx
    std::vector<Particle> rain_, snow_;
    Vector3 partOrigin_{};
    std::map<int, Herd> herds_;
    std::vector<Bird> birds_;
    struct Bolt { std::vector<Vector3> pts; float t = -1; Vector3 cloud{}; float strength = 0; bool ground = false; } bolt_;
    float boltLight() const;   // flicker of the current strike (0 when none)
    PrecipSound precipSound_;
    float wetLocal_ = -1;      // wet ground from the rain that has actually fallen here

    // render targets
    int rtW_ = 0, rtH_ = 0, shadowRes_ = 2048;
    float renderScale_ = 0;
    int targetW() const;
    int targetH() const;
    RenderTexture2D scene_{}, bright_{}, blurA_{}, blurB_{}, qA_{}, qB_{}, ldr_{};
    RenderTexture2D refr_{};
    Shader refrCopySh_{};
    double lastTrim_ = 0;
    float devCam_[6] = {-1e9f, 0, 0, 0, 0, 0};  // copy of the opaque scene (colour + depth texture) for water
    void drawWater(const SceneState& s);
    RenderTexture2D shadow_[2]{};
    Matrix lightVP_[2]{};
    float cascadeRange_[2] = {45.f, 420.f};

    Camera3D cam_{};
    Vector3 camPosSmooth_{}, camTargetSmooth_{};
    bool camInit_ = false;
    float time_ = 0;
    int cineShot_ = 0;
    float cineTimer_ = 0;
    double cineAnchor_ = -1;
    int tris_ = 0;
};
