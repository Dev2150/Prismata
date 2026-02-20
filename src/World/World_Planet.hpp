#pragma once
// ── World_Planet.hpp ──────────────────────────────────────────────────────────
// Single shared PlanetSurface instance used by World (simulation) and Renderer
// (camera, billboard normals, FOV cone). Defined in World_Planet.cpp.
//
// This replaces the flat heightfield as the ground truth for all spatial queries.

#include "Core/Planet_Surface.hpp"

// The one planet that the whole simulation lives on.
// Initialised once in World::generate(); read-only thereafter except for
// parameter tweaks exposed through the settings UI.
extern PlanetSurface g_planet_surface;

// Convenience: seed the planet noise (calls PlanetNoise::init).
// Must be called before any PlanetNoise::sampleHeight calls.
void initPlanetNoise(uint64_t seed);
