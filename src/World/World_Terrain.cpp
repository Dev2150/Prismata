#include "World.hpp"
#include "World_Planet.hpp"
#include <cmath>
#include <algorithm>

// ── Chunk accessors ───────────────────────────────────────────────────────────
Chunk* World::chunkAt(int cx, int cz) {
    if (cx < 0 || cz < 0 || cx >= worldCX || cz >= worldCZ) return nullptr;
    return &chunks[cz * worldCX + cx];
}

const Chunk* World::chunkAt(int cx, int cz) const {
    if (cx < 0 || cz < 0 || cx >= worldCX || cz >= worldCZ) return nullptr;
    return &chunks[cz * worldCX + cx];
}

VoxelColumn& World::columnAt(int gx, int gz) {
    int cx = gx / CHUNK_SIZE, lx = gx % CHUNK_SIZE;
    int cz = gz / CHUNK_SIZE, lz = gz % CHUNK_SIZE;
    return chunks[cz * worldCX + cx].cells[lz][lx];
}

// ── Planet-mode 3D spatial helpers ───────────────────────────────────────────

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

// ── Legacy flat-world stubs ───────────────────────────────────────────────────
// These are called by Renderer_Camera, Renderer_Overlays, SimUI, etc. that
// haven't been ported to full 3D yet. They work by treating (x, z) as a
// rough direction from the planet center — good enough for the FOV cone and
// terrain hover which operate near the top of the sphere (y > 0 side).

float World::heightAt(float x, float z) const {
    // Build an approximate 3-D world position on the top hemisphere and query.
    // Offset by planet center so the direction points away from center.
    Vec3 approx = {
        g_planet_surface.center.x + x,
        g_planet_surface.center.y + g_planet_surface.radius,   // top of sphere
        g_planet_surface.center.z + z,
    };
    // Snap to actual surface along this direction from center, return Y.
    Vec3 snapped = g_planet_surface.snapToSurface(approx);
    return snapped.y;
}

float World::slopeAt(float x, float z) const {
    Vec3 approx = {
        g_planet_surface.center.x + x,
        g_planet_surface.center.y + g_planet_surface.radius,
        g_planet_surface.center.z + z,
    };
    Vec3 snapped = g_planet_surface.snapToSurface(approx);
    return g_planet_surface.slopeAt(snapped);
}

bool World::isWater(float x, float z) const {
    Vec3 approx = {
        g_planet_surface.center.x + x,
        g_planet_surface.center.y + g_planet_surface.radius,
        g_planet_surface.center.z + z,
    };
    return g_planet_surface.isOcean(g_planet_surface.snapToSurface(approx));
}

bool World::findWater(const Vec3& from, float radius, Vec3& outPos) const {
    return g_planet_surface.findOcean(from, radius, outPos);
}

Vec3 World::snapToSurface(float x, float z) const {
    Vec3 approx = {
        g_planet_surface.center.x + x,
        g_planet_surface.center.y + g_planet_surface.radius,
        g_planet_surface.center.z + z,
    };
    return g_planet_surface.snapToSurface(approx);
}