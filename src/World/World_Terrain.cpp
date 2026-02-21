#include "World.hpp"
#include "World_Planet.hpp"
#include <cmath>
#include <algorithm>

// ── Chunk accessors ───────────────────────────────────────────────────────────
// Chunks are kept as an empty grid for compatibility with the chunk mesh cache
// path in the renderer (which is itself a no-op in planet mode). The accessors
// are retained so chunkAtPublic() compiles, but no gameplay code calls them.
Chunk* World::chunkAt(int cx, int cz) {
    if (cx < 0 || cz < 0 || cx >= worldCX || cz >= worldCZ) return nullptr;
    return &chunks[cz * worldCX + cx];
}

const Chunk* World::chunkAt(int cx, int cz) const {
    if (cx < 0 || cz < 0 || cx >= worldCX || cz >= worldCZ) return nullptr;
    return &chunks[cz * worldCX + cx];
}

// ── Planet-surface 3D spatial helpers ─────────────────────────────────────────

float World::heightAt3D(const Vec3& worldPos) const {
    return g_planet_surface.noiseHeight(worldPos);
}

Vec3 World::snapToSurface3D(const Vec3& worldPos) const {
    return g_planet_surface.snapToSurface(worldPos);
}

Vec3 World::normalAt(const Vec3& worldPos) const {
    return g_planet_surface.normalAt(worldPos);
}

float World::slopeAt3D(const Vec3& worldPos) const {
    return g_planet_surface.slopeAt(worldPos);
}

bool World::isOcean(const Vec3& worldPos) const {
    return g_planet_surface.isOcean(worldPos);
}

bool World::findOcean(const Vec3& from, float radius, Vec3& outPos) const {
    return g_planet_surface.findOcean(from, radius, outPos);
}