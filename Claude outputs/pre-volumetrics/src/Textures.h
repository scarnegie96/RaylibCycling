#pragma once
// Procedural, tileable textures generated at start-up (no image files): grass blades,
// asphalt aggregate, gravel pebbles, bedded rock, sand / silt, bare soil, and soft sprites for rain, snow, splashes and glows.
#include "raylib.h"

namespace tex {
struct Set {
    Texture2D grass{}, asphalt{}, gravel{};   // rgb = albedo, a = height
    Texture2D rock{}, sand{}, soil{};         // terrain layers (bedded rock, river sediment, bare earth)
    Texture2D soft{}, streak{}, ring{};       // sprites (white, alpha)
};
Set generate(int quality);
void unload(Set& s);
}  // namespace tex
