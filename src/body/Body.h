#pragma once
// A proportioned, rigged human male body (grey box), built from scratch.
//
// Shape: ~50 anatomical primitives (ellipsoids and round cones: skull, jaw, ribcage, pecs,
// lats, glutes, deltoids, biceps, quads, calves ...) placed from anthropometric ratios of the
// height, each owned by a bone and smoothly blended (per limb into the torso, never limb into
// limb) into one signed-distance field. The surface is extracted with surface nets, every
// vertex projected onto the exact surface, normals from the field's gradient.
//
// Rig: 26 bones in a hierarchy (root, pelvis, spine, chest, neck, head, clavicles, arms,
// hands with a finger block and thumb, legs, feet, toes). Skin weights come from the same
// primitives: a vertex follows the bones whose shapes it lies on, blended across joints.
// Posing is by local rotations (FK), with two-bone IK, aim and global-rotation helpers.
//
// Everything scales from Params (height, shoulders, hips, girth, muscle, leg / arm length),
// and the primitives stay available, so clothing can later be the same field pushed out.
// Conventions: metres, y up, +z forward, +x = the body's LEFT; A-pose bind, feet on y = 0.
// Bone frames: y runs head -> tail; a positive rotation about a bone's local x "flexes"
// it forward (hips, knees backwards: negative; feet: toes up).
// Pure C++ (no raylib), so it can be tested and used anywhere.
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace body {

// ---------------------------------------------------------------- maths
struct V3 { float x = 0, y = 0, z = 0; };
inline V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 operator-(V3 a) { return {-a.x, -a.y, -a.z}; }
inline V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline V3 operator*(float s, V3 a) { return a * s; }
inline float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float len(V3 a) { return std::sqrt(dot(a, a)); }
inline V3 norm(V3 a) { float l = len(a); return l > 1e-12f ? a * (1.f / l) : V3{0, 1, 0}; }
inline V3 lerp(V3 a, V3 b, float t) { return a + (b - a) * t; }

struct Quat { float x = 0, y = 0, z = 0, w = 1; };
Quat qmul(Quat a, Quat b);
Quat qconj(Quat q);
Quat qnorm(Quat q);
V3 qrot(Quat q, V3 v);
Quat qaxis(V3 axis, float angle);
Quat qbetween(V3 from, V3 to);           // shortest rotation taking unit `from` to unit `to`
Quat qbasis(V3 x, V3 y, V3 z);           // rotation whose columns are the given orthonormal axes
Quat qslerp(Quat a, Quat b, float t);

// ---------------------------------------------------------------- skeleton
enum Bone : int {
    Root, Pelvis, Spine, Chest, Neck, Head,
    ClavicleL, UpperArmL, ForearmL, HandL, FingersL, ThumbL,
    ClavicleR, UpperArmR, ForearmR, HandR, FingersR, ThumbR,
    ThighL, ShinL, FootL, ToesL,
    ThighR, ShinR, FootR, ToesR,
    BONES
};
const char* boneName(int b);
int boneParent(int b);
int boneSide(int b);     // +1 left, -1 right, 0 centre
int boneMirror(int b);   // left <-> right

struct BoneRest {
    int parent = -1;
    V3 head, tail;       // rest (bind) positions, model space
    Quat rot;            // rest frame: local y along the bone, z towards the front (or up, for feet)
    float length = 0;
    Quat localRot;       // rest rotation relative to the parent's rest frame
    V3 localOffset;      // head relative to the parent's head, in the parent's rest frame
};
struct Skeleton { std::array<BoneRest, BONES> bones; };

// ---------------------------------------------------------------- shape + mesh
struct Params {
    float height = 1.78f;    // m
    float shoulders = 1.f;   // shoulder breadth
    float hips = 1.f;        // hip breadth
    float girth = 1.f;       // overall thickness (lean 0.85 .. heavy 1.25)
    float muscle = 1.f;      // muscle definition (0 smooth mannequin .. 1.6 very athletic)
    float legs = 1.f;        // leg length relative to the torso (height kept)
    float arms = 1.f;        // arm length
    float cell = 0.0075f;    // surface resolution (m); coarsened automatically to stay under maxVerts
    int maxVerts = 60000;    // (16-bit index meshes)
};

struct Prim {
    enum Type { Ellipsoid, RoundCone } type = Ellipsoid;
    int bone = 0, group = 0;     // group: 0 torso + head, 1 arm L, 2 arm R, 3 leg L, 4 leg R
    float k = 0.f;               // blend radius into its group
    V3 a, b;                     // ellipsoid: centre in a; round cone: a -> b
    float ra = 0, rb = 0;        // round cone radii
    V3 r;                        // ellipsoid radii (along ax, ay, az)
    V3 ax{1, 0, 0}, ay{0, 1, 0}, az{0, 0, 1};
    V3 bc; float br = 0;         // bounding sphere
};

struct Mesh {
    std::vector<V3> pos, nrm;
    std::vector<uint32_t> idx;                     // triangles
    std::vector<std::array<uint8_t, 4>> bones;     // up to 4 influences
    std::vector<std::array<float, 4>> weights;     // normalised
};

struct Body {
    Params params;
    Skeleton skeleton;
    std::vector<Prim> prims;
    float groupK[5] = {};        // how far each limb blends into the torso
    Mesh mesh;
    float sdf(V3 p) const;       // signed distance to the (rest pose) body surface
    float primDist(const Prim& pr, V3 p) const;
};

Body build(const Params& p = {});

// ---------------------------------------------------------------- posing
struct Pose {
    V3 root;                                   // translation added to the root bone
    std::array<Quat, BONES> local{};           // rotation of each bone in its own rest frame
};
struct Globals {
    std::array<Quat, BONES> rot;               // posed bone frames
    std::array<V3, BONES> pos;                 // posed heads
    V3 tail(const Skeleton& s, int b) const { return pos[b] + qrot(rot[b], V3{0, s.bones[b].length, 0}); }
};

Globals forward(const Skeleton& s, const Pose& p);
// skin the mesh into out arrays (same size as mesh.pos)
void skin(const Body& b, const Globals& g, std::vector<V3>& pos, std::vector<V3>& nrm);

// rigging helpers (each runs FK on the current pose itself)
void setGlobalRot(const Skeleton& s, Pose& p, int bone, Quat global);
// point a bone along `dir`; if `zRef` is non-zero its local z is turned towards it (twist)
void aim(const Skeleton& s, Pose& p, int bone, V3 dir, V3 zRef = {});
// place the end of `lower` at `target`; the joint between them bulges towards `pole`.
// zToPole: +1 when the bone's local z faces the way the joint bulges (knees), -1 (elbows).
// Returns false if the target was out of reach (the chain is then straightened towards it).
bool twoBoneIK(const Skeleton& s, Pose& p, int upper, int lower, V3 target, V3 pole, float zToPole);
// local rotation about the bone's own axes (radians)
Quat flex(float x, float y = 0, float z = 0);

// ---------------------------------------------------------------- procedural animation
namespace anim {
void rest(Pose& p);                                    // A-pose bind
void idle(const Body& b, Pose& p, float t);            // relaxed standing, breathing, weight shift
void walk(const Body& b, Pose& p, float t, float speed = 1.4f);   // in place (a treadmill walk)
struct Bike {
    V3 seat;                 // where the sit bones rest on the saddle
    V3 hoodL, hoodR;         // where the hands grip
    V3 pedalL, pedalR;       // pedal spindles
    float crank = 0;         // radians
    float stand = 0;         // 0 seated .. 1 out of the saddle
    float effort = 0;        // 0 .. 1 body rock
    float tuck = 0;          // 0 hoods .. 1 drops (lower, flatter back)
};
void cycle(const Body& b, Pose& p, const Bike& bike);
}  // namespace anim

}  // namespace body
