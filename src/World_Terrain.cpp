#include "World.hpp"
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

// Bilinear interpolation of height at a continuous world-space point.
// Sampling four integer-grid corners and blending avoids harsh "stair-step"
// transitions when a creature moves between cells.
float World::heightAt(float x, float z) const {
    int x0 = (int)std::floor(x), z0 = (int)std::floor(z);
    float fx = x - x0, fz = z - z0;   // fractional offsets in [0,1)

    auto getH = [&](int gx, int gz) -> float {
        gx = std::clamp(gx, 0, worldCX * CHUNK_SIZE - 1);
        gz = std::clamp(gz, 0, worldCZ * CHUNK_SIZE - 1);
        int cx = gx / CHUNK_SIZE, lx = gx % CHUNK_SIZE;
        int cz = gz / CHUNK_SIZE, lz = gz % CHUNK_SIZE;
        const Chunk* ch = chunkAt(cx, cz);
        return ch ? ch->cells[lz][lx].height : 0.f;
    };

    float h00 = getH(x0,   z0  );
    float h10 = getH(x0+1, z0  );
    float h01 = getH(x0,   z0+1);
    float h11 = getH(x0+1, z0+1);

    // Standard bilinear blend: lerp along X, then lerp those results along Z
    return (h00 * (1-fx) + h10 * fx) * (1-fz)
         + (h01 * (1-fx) + h11 * fx) *    fz;
}

// Estimate terrain slope at (x,z) as sin(angle_from_horizontal).
// Uses a central finite difference on the height field.
// sin(atan(gradMag)) ≈ gradMag for small angles; gives true sin for steep slopes.
// Used by the energy model (climbing cost) and movement gating (max slope).
float World::slopeAt(float x, float z) const {
    const float d = 0.5f;
    float dhdx = (heightAt(x+d, z) - heightAt(x-d, z)) / (2*d);
    float dhdz = (heightAt(x, z+d) - heightAt(x, z-d)) / (2*d);
    float gradMag = std::sqrt(dhdx*dhdx + dhdz*dhdz);  // magnitude of gradient vector
    return std::sin(std::atan(gradMag));                 // convert rise/run to sin(angle)
}

bool World::isWater(float x, float z) const {
    int gx = (int)x, gz = (int)z;
    gx = std::clamp(gx, 0, worldCX * CHUNK_SIZE - 1);
    gz = std::clamp(gz, 0, worldCZ * CHUNK_SIZE - 1);
    int cx = gx / CHUNK_SIZE, lx = gx % CHUNK_SIZE;
    int cz2 = gz / CHUNK_SIZE, lz = gz % CHUNK_SIZE;
    const Chunk* ch = chunkAt(cx, cz2);
    return ch && ch->cells[lz][lx].material == 3;
}

// Scan a grid of cells within `radius` of `from` to find the nearest water tile.
// Returns false if no water is found within range.
// Linear scan is acceptable because this is called once per creature per tick
// and the search radius is typically small (< 50 tiles).
bool World::findWater(const Vec3& from, float radius, Vec3& outPos) const {
    int r = (int)std::ceil(radius);
    int gx0 = (int)from.x - r, gz0 = (int)from.z - r;
    float bestDist = 1e9f;
    bool  found    = false;
    for (int dz = -r; dz <= r; dz++) {
        for (int dx = -r; dx <= r; dx++) {
            int gx = gx0 + dx, gz = gz0 + dz;
            if (gx < 0 || gz < 0) continue;
            if (gx >= worldCX * CHUNK_SIZE || gz >= worldCZ * CHUNK_SIZE) continue;
            if (!isWater((float)gx, (float)gz)) continue;
            Vec3 wPos = snapToSurface((float)gx, (float)gz);
            float d   = dist(from, wPos);
            if (d < bestDist && d < radius) {
                bestDist = d; outPos = wPos; found = true;
            }
        }
    }
    return found;
}

Vec3 World::snapToSurface(float x, float z) const {
    return {x, heightAt(x, z), z};
}