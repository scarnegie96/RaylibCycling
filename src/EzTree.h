#pragma once
// Native C++ port of ez-tree (https://github.com/dgreenheck/ez-tree, MIT, (c) 2024 Daniel
// Greenheck): procedural trees grown from a handful of parameters. The skeleton growth
// (gnarliness, growth force, twist, stratified child branches and leaves) and the meshing
// (tapered rings, double-billboard leaves with rounded normals, LOD strides) follow
// src/lib/tree.js closely, including its seeded RNG and three.js's XYZ Euler / quaternion
// maths, so a preset + seed grows the same tree. Trellis support is left out.
#include <cstdint>
#include <vector>

namespace eztree {

struct V3 { float x = 0, y = 0, z = 0; };

struct Options {
    const char* name = "";
    uint32_t seed = 0;
    bool evergreen = false;
    int levels = 3;
    float angle[4], children[4];
    V3 forceDir{0, 1, 0};
    float forceStrength = 0.01f;
    float gnarliness[4], length[4], radius[4], sections[4], segments[4], start[4], taper[4], twist[4];
    int leafCount = 1;
    float leafAngle = 10, leafStart = 0, leafSize = 2.5f, leafSizeVariance = 0.7f;
    float barkScaleX = 1;
    bool doubleBillboard = true;
};

// the presets shipped with ez-tree (bark / leaf texture names aside)
const Options* preset(const char* name);

struct Section { V3 origin; float qx, qy, qz, qw; float radius; float along; };   // along: metres from the branch base
struct BranchSk { std::vector<Section> sections; int segmentCount; float baseRadius; int level; };
struct LeafSk { V3 origin; float qx, qy, qz, qw; float size; };
struct Skeleton { std::vector<BranchSk> branches; std::vector<LeafSk> leaves; };

Skeleton grow(const Options& o);

// ez-tree's LODDetail
struct Detail { int sectionStride = 1; float segmentFactor = 1; int leafStride = 1; float leafScale = 1; int billboard = -1; /* -1 options, 0 single, 1 double */ };

struct Vertex { V3 p, n; float u, v; int branchLevel; };   // branchLevel -1 for leaves
struct Geometry {
    std::vector<Vertex> branchVerts, leafVerts;
    std::vector<uint32_t> branchIdx, leafIdx;
    std::vector<V3> leafOrigins;   // per leaf vertex: where its leaf grows (for canopy shading)
};
Geometry mesh(const Options& o, const Skeleton& s, const Detail& d);

}  // namespace eztree
