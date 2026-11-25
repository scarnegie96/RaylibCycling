#pragma once
// Procedural trees: a leaf atlas (broad leaves, small birch leaves, spruce sprays, pine
// needle tufts) and branching skeletons for six species, two variants each, built at a
// near (full) and far (few big cards, no twigs) level of detail.
#include "raylib.h"

namespace trees {

enum Species { Oak, Beech, Birch, Poplar, Spruce, Pine, SPECIES };
constexpr int VARIANTS = 2;
constexpr int TYPES = SPECIES * VARIANTS;

struct Built {
    Mesh hi{}, lo{};
    float height = 10;
};

Texture2D leafAtlas(int quality);
Built build(int type, int quality);
// species for a spot: altitude01 = 0 valley .. 1 high; r = random 0..1; forest = density noise
int pickType(float altitude01, float forest, float r1, float r2);
bool isConifer(int type);

}  // namespace trees
