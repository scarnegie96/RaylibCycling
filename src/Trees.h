#pragma once
// Procedural vegetation: trees (six species, two variants each) and bushes grown with an
// ez-tree port, at a near (full) and far (fewer, bigger leaf clumps, no twigs) level of
// detail, plus faceted rocks.
#include <cstdint>

#include "raylib.h"

namespace trees {

enum Species { Oak, Beech, Birch, Poplar, Spruce, Pine, SPECIES };
constexpr int VARIANTS = 2;
constexpr int TYPES = SPECIES * VARIANTS;

struct Built {
    Mesh hi{}, lo{};
    Mesh shadow{};   // far shadow cascade only: trunk, main limbs, a few big clumps
    float height = 10;
};

constexpr int BUSHES = 3;
constexpr int ROCKS = 4;

// Trees and bushes are grown at start-up with a port of ez-tree (EzTree.h) from its presets;
// each session draws fresh seeds, so the variants differ from launch to launch.
void setSeed(uint32_t seed);
Texture2D leafAtlas(int quality);   // Zwift-style painted leaf clusters (2 x 2 tiles)
Built build(int type, int quality);
Built buildBush(int i);
Mesh buildRock(int i);              // chunky faceted boulder, ~1 m, base at y = 0
// species for a spot: altitude01 = 0 valley .. 1 high; r = random 0..1; forest = density noise
int pickType(float altitude01, float forest, float r1, float r2);
// generated worlds: the biome gives the share of conifers; broadleaves by altitude
int pickTypeWorld(float altitude01, float conifer, float r1, float r2);
bool isConifer(int type);

}  // namespace trees
