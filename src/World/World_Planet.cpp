// ── World_Planet.cpp ──────────────────────────────────────────────────────────
// Defines the single shared PlanetSurface used by the whole simulation.

#include "World_Planet.hpp"
#include "Renderer/Planet/PlanetNoise.hpp"

// Global planet surface. Parameters match PlanetConfig in App.cpp:
//   radius=1000, center=(0,1800,0) below flat origin, heightScale=120.
// NOTE: center.y is POSITIVE here because the planet sits 1800 units *below*
// the old flat-world origin. Creatures stand on the *top* of the sphere,
// so the camera starts slightly above center + radius.
PlanetSurface g_planet_surface = []() {
    PlanetSurface ps;
    ps.center      = {0.f, -180000.f, 0.f};
    ps.radius      = 100000.f;
    ps.heightScale = 20000.f;
    ps.seaLevel    = 0.f;   // noise height below 0 = ocean
    return ps;
}();

void initPlanetNoise(uint64_t seed) {
    PlanetNoise::init(seed);
}
