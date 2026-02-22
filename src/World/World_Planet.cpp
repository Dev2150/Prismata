#include "World_Planet.hpp"
#include "Renderer/Planet/PlanetNoise.hpp"

// Global planet surface. Uses the centralized constexpr defaults.
PlanetSurface g_planet_surface;

void initPlanetNoise(uint64_t seed) {
    PlanetNoise::init(seed);
}