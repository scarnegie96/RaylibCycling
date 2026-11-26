// body_viewer: inspect the rigged greybox body.
//   1-4 mode (bind / idle / walk / cycle)   K skeleton   W wireframe   H weight heatmap (bone under cursor: [ ])
//   Up/Down height   Left/Right girth   M muscle   L legs   mouse drag orbit, wheel zoom
//   --mode bind|idle|walk|cycle  --screenshot out.png  --frames N  --time T  --yaw deg  --skeleton  --heat BONE
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "body/Body.h"
#include "raylib.h"
#include "rlgl.h"

#include "raymath.h"
using body::V3; using body::Body; using body::Params; using body::Pose; using body::Globals;
using body::BONES; using body::boneName; using body::boneSide; using body::build; using body::forward; using body::skin;
namespace anim = body::anim;

static const char* VS = R"(#version 330
in vec3 vertexPosition; in vec3 vertexNormal; in vec4 vertexColor;
uniform mat4 mvp;
out vec3 n; out vec3 wp; out vec4 col;
void main(){ n = vertexNormal; wp = vertexPosition; col = vertexColor; gl_Position = mvp*vec4(vertexPosition,1.0); }
)";
static const char* FS = R"(#version 330
in vec3 n; in vec3 wp; in vec4 col;
uniform vec3 eye; uniform float heat;
out vec4 o;
void main(){
  vec3 N = normalize(n), L = normalize(vec3(0.5,0.8,0.45)), V = normalize(eye-wp);
  vec3 base = mix(vec3(0.62,0.62,0.64), col.rgb, heat);
  float d = max(dot(N,L),0.0), fill = max(dot(N,normalize(vec3(-0.6,0.2,-0.5))),0.0)*0.25;
  float rim = pow(1.0-max(dot(N,V),0.0),3.0)*0.25;
  float sky = 0.25+0.15*N.y;
  vec3 H = normalize(L+V); float sp = pow(max(dot(N,H),0.0),40.0)*0.15;
  o = vec4(base*(d*0.85+fill+sky)+vec3(rim+sp),1.0);
}
)";

struct GpuBody {
    ::Mesh mesh{};
    bool up = false;
    void upload(const body::Mesh& M) {
        if (up) UnloadMesh(mesh);
        mesh = ::Mesh{};
        mesh.vertexCount = int(M.pos.size());
        mesh.triangleCount = int(M.idx.size() / 3);
        mesh.vertices = (float*)MemAlloc(mesh.vertexCount * 3 * sizeof(float));
        mesh.normals = (float*)MemAlloc(mesh.vertexCount * 3 * sizeof(float));
        mesh.colors = (unsigned char*)MemAlloc(mesh.vertexCount * 4);
        mesh.indices = (unsigned short*)MemAlloc(M.idx.size() * sizeof(unsigned short));
        for (size_t i = 0; i < M.idx.size(); ++i) mesh.indices[i] = (unsigned short)M.idx[i];
        std::memset(mesh.colors, 255, mesh.vertexCount * 4);
        UploadMesh(&mesh, true);
        up = true;
    }
    void update(const std::vector<V3>& p, const std::vector<V3>& n) {
        std::memcpy(mesh.vertices, p.data(), p.size() * sizeof(V3));
        std::memcpy(mesh.normals, n.data(), n.size() * sizeof(V3));
        UpdateMeshBuffer(mesh, 0, mesh.vertices, mesh.vertexCount * 12, 0);
        UpdateMeshBuffer(mesh, 2, mesh.normals, mesh.vertexCount * 12, 0);
    }
    void colours(const body::Mesh& M, int bone) {
        for (size_t i = 0; i < M.pos.size(); ++i) {
            float w = 0;
            for (int k = 0; k < 4; ++k) if (M.bones[i][k] == bone) w = M.weights[i][k];
            // dominant bone hue when no bone is picked, else a blue->red ramp
            Color c;
            if (bone < 0) c = ColorFromHSV(float((M.bones[i][0] * 47) % 360), 0.55f, 0.9f);
            else c = Color{(unsigned char)(255 * w), (unsigned char)(80 * (1 - std::fabs(w * 2 - 1))), (unsigned char)(255 * (1 - w)), 255};
            std::memcpy(mesh.colors + i * 4, &c, 4);
        }
        UpdateMeshBuffer(mesh, 3, mesh.colors, mesh.vertexCount * 4, 0);
    }
};

static Vector3 rv(V3 v) { return {v.x, v.y, v.z}; }

int main(int argc, char** argv) {
    std::string mode = "idle", shot;
    int frames = -1, heatBone = -2;
    float fixedT = -1, yaw = 35, pitch = 10, dist = 3.2f;
    bool showSkel = false, wire = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&] { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--mode") mode = next();
        else if (a == "--screenshot") shot = next();
        else if (a == "--frames") frames = std::stoi(next());
        else if (a == "--time") fixedT = std::stof(next());
        else if (a == "--yaw") yaw = std::stof(next());
        else if (a == "--pitch") pitch = std::stof(next());
        else if (a == "--dist") dist = std::stof(next());
        else if (a == "--skeleton") showSkel = true;
        else if (a == "--wire") wire = true;
        else if (a == "--heat") {
            std::string b = next();
            heatBone = -1;
            for (int k = 0; k < BONES; ++k) if (b == boneName(k)) heatBone = k;
        }
    }
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI);
    InitWindow(1100, 800, "CoreRay body viewer");
    SetTargetFPS(60);
    Shader sh = LoadShaderFromMemory(VS, FS);
    int locEye = GetShaderLocation(sh, "eye"), locHeat = GetShaderLocation(sh, "heat");
    Material mat = LoadMaterialDefault();
    mat.shader = sh;

    Params P;
    Body B = build(P);
    GpuBody gb;
    gb.upload(B.mesh);
    if (heatBone > -2) gb.colours(B.mesh, heatBone);
    std::vector<V3> pos, nrm;
    float t = 0;
    int frame = 0;
    auto rebuild = [&] { B = build(P); gb.upload(B.mesh); if (heatBone > -2) gb.colours(B.mesh, heatBone); };

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        t = fixedT >= 0 ? fixedT : t + dt;
        if (IsKeyPressed(KEY_ONE)) mode = "bind";
        if (IsKeyPressed(KEY_TWO)) mode = "idle";
        if (IsKeyPressed(KEY_THREE)) mode = "walk";
        if (IsKeyPressed(KEY_FOUR)) mode = "cycle";
        if (IsKeyPressed(KEY_K)) showSkel = !showSkel;
        if (IsKeyPressed(KEY_W)) wire = !wire;
        if (IsKeyPressed(KEY_H)) { heatBone = heatBone > -2 ? -2 : -1; if (heatBone > -2) gb.colours(B.mesh, heatBone); }
        if (heatBone > -2 && IsKeyPressed(KEY_RIGHT_BRACKET)) { heatBone = (heatBone + 2) % (BONES + 1) - 1; gb.colours(B.mesh, heatBone); }
        if (heatBone > -2 && IsKeyPressed(KEY_LEFT_BRACKET)) { heatBone = (heatBone + BONES + 1) % (BONES + 1) - 1; gb.colours(B.mesh, heatBone); }
        if (IsKeyPressed(KEY_UP)) { P.height += 0.05f; rebuild(); }
        if (IsKeyPressed(KEY_DOWN)) { P.height -= 0.05f; rebuild(); }
        if (IsKeyPressed(KEY_RIGHT)) { P.girth += 0.05f; rebuild(); }
        if (IsKeyPressed(KEY_LEFT)) { P.girth -= 0.05f; rebuild(); }
        if (IsKeyPressed(KEY_M)) { P.muscle = P.muscle > 1.3f ? 0.7f : P.muscle + 0.3f; rebuild(); }
        if (IsKeyPressed(KEY_L)) { P.legs = P.legs > 1.05f ? 0.94f : P.legs + 0.06f; rebuild(); }
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) { Vector2 d = GetMouseDelta(); yaw -= d.x * 0.4f; pitch = std::fmin(80, std::fmax(-60, pitch + d.y * 0.3f)); }
        dist = std::fmax(0.6f, dist * (1 - GetMouseWheelMove() * 0.1f));

        Pose pose;
        anim::Bike bike;
        if (mode == "idle") anim::idle(B, pose, t);
        else if (mode == "walk") { anim::walk(B, pose, t, 1.4f); }
        else if (mode == "cycle") {
            bike.crank = t * 9.4f;   // 90 rpm
            bike.seat = {0, 0.915f, -0.193f};
            bike.hoodL = {0.21f, 0.955f, 0.64f};
            bike.hoodR = {-0.21f, 0.955f, 0.64f};
            auto ped = [&](float a, float x) { return V3{x, 0.275f - std::cos(a) * 0.17f, -std::sin(a) * 0.17f}; };
            bike.pedalR = ped(bike.crank, -0.095f);
            bike.pedalL = ped(bike.crank + 3.14159f, 0.095f);
            bike.effort = 0.5f;
            anim::cycle(B, pose, bike);
        }
        Globals G = forward(B.skeleton, pose);
        skin(B, G, pos, nrm);
        gb.update(pos, nrm);

        float H = P.height;
        Vector3 target{0, H * 0.52f, mode == "cycle" ? 0.2f : 0.f};
        float yr = yaw * DEG2RAD, pr = pitch * DEG2RAD;
        Camera3D cam{};
        cam.position = {target.x + std::sin(yr) * std::cos(pr) * dist, target.y + std::sin(pr) * dist, target.z + std::cos(yr) * std::cos(pr) * dist};
        cam.target = target;
        cam.up = {0, 1, 0};
        cam.fovy = 40;
        cam.projection = CAMERA_PERSPECTIVE;
        SetShaderValue(sh, locEye, &cam.position, SHADER_UNIFORM_VEC3);
        float heat = heatBone > -2 ? 1.f : 0.f;
        SetShaderValue(sh, locHeat, &heat, SHADER_UNIFORM_FLOAT);

        BeginDrawing();
        ClearBackground({58, 62, 70, 255});
        BeginMode3D(cam);
        DrawGrid(20, 0.25f);
        if (mode == "cycle") {   // just enough bike to read the pose
            Color bc{40, 40, 44, 255};
            Vector3 bb{0, 0.275f, 0}, seat{0, 0.915f, -0.193f}, bar{0, 0.955f, 0.6f};
            DrawCylinderEx(bb, seat, 0.015f, 0.015f, 8, bc);
            DrawCylinderEx(seat, bar, 0.015f, 0.015f, 8, bc);
            DrawCylinderEx(bb, bar, 0.018f, 0.018f, 8, bc);
            DrawCylinderEx({-0.21f, 0.955f, 0.6f}, {0.21f, 0.955f, 0.6f}, 0.012f, 0.012f, 8, bc);
            DrawCube({0, 0.92f, -0.17f}, 0.12f, 0.03f, 0.26f, bc);
            for (float x : {0.095f, -0.095f}) {
                V3 p = x > 0 ? bike.pedalL : bike.pedalR;
                DrawCylinderEx({x * 0.5f, 0.275f, 0}, rv(p), 0.008f, 0.008f, 6, bc);
                DrawCube(rv(p), 0.07f, 0.012f, 0.09f, bc);
            }
            DrawCircle3D({0, 0.35f, 0.6f}, 0.35f, {0, 1, 0}, 90, bc);
            DrawCircle3D({0, 0.35f, -0.41f}, 0.35f, {0, 1, 0}, 90, bc);
        }
        if (wire) rlEnableWireMode();
        DrawMesh(gb.mesh, mat, MatrixIdentity());
        if (wire) rlDisableWireMode();
        if (showSkel) {
            rlDisableDepthTest();
            for (int b = 1; b < BONES; ++b) {
                Color c = boneSide(b) > 0 ? Color{90, 200, 255, 255} : boneSide(b) < 0 ? Color{255, 140, 90, 255} : Color{240, 230, 120, 255};
                DrawLine3D(rv(G.pos[b]), rv(G.tail(B.skeleton, b)), c);
                DrawSphere(rv(G.pos[b]), 0.008f, c);
            }
            rlEnableDepthTest();
        }
        EndMode3D();
        DrawText(TextFormat("%s   %.2f m  girth %.2f  muscle %.1f  legs %.2f   %d verts / %d tris", mode.c_str(), P.height, P.girth, P.muscle, P.legs,
                            int(B.mesh.pos.size()), int(B.mesh.idx.size() / 3)), 12, 12, 18, RAYWHITE);
        DrawText(heatBone > -2 ? TextFormat("weights: %s  ([ ] to change)", heatBone < 0 ? "dominant bone" : boneName(heatBone)) : "1-4 mode  K skeleton  W wire  H weights  arrows size  M muscle  L legs",
                 12, 36, 16, {200, 200, 210, 255});
        EndDrawing();
        if (frames > 0 && ++frame >= frames) {
            if (!shot.empty()) TakeScreenshot(shot.c_str());
            break;
        }
    }
    UnloadShader(sh);
    CloseWindow();
    return 0;
}
