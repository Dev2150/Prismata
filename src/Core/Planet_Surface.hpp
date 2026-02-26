#pragma once

// Shared planet surface query utility used by both World (simulation) and
// PlanetRenderer (rendering).
// No D3D / renderer dependencies — only Math.hpp, RNG.hpp, PlanetNoise.hpp.
//
// All creature / plant positions in "planet mode" are 3D world-space points
// on the displaced sphere surface.

#include "Core/Math.hpp"
#include "Core/RNG.hpp"
#include "Renderer/Planet/PlanetNoise.hpp"
#include <cmath>

// ── Centralized Planet Constants ──────────────────────────────────────────────
constexpr float PLANET_RADIUS       = 100000.f;
constexpr float PLANET_CENTER_Y     = -180000.f;
constexpr float PLANET_HEIGHT_SCALE = 20000.f;

struct PlanetSurface {
    Vec3  center      = {0.f, PLANET_CENTER_Y, 0.f};
    float radius      = PLANET_RADIUS;
    float heightScale = PLANET_HEIGHT_SCALE;
    float seaLevel    = 0.f;   // noise height below this = ocean

    // ── Geometry ──────────────────────────────────────────────────────────────

    // Displaced surface position for a direction from the planet center.
    Vec3 surfacePos(Vec3 dir) const {
        dir = dir.normalised();
        float h = PlanetNoise::sampleHeight(dir.x, dir.y, dir.z, heightScale);
        h = std::max(h, 0.0f); // Clamp to sea level
        float r = radius + h;
        return {center.x + dir.x * r,
                center.y + dir.y * r,
                center.z + dir.z * r};
    }

    // Outward surface normal at a world-space position (unit vector away from center).
    Vec3 normalAt(Vec3 worldPos) const {
        return (worldPos - center).normalised();
    }

    // Noise-based height above the sphere's base radius (negative = below).
    float noiseHeight(Vec3 worldPos) const {
        Vec3 d = (worldPos - center).normalised();
        return PlanetNoise::sampleHeight(d.x, d.y, d.z, heightScale, 0.3f, 0);
    }


    // Snap a world-space position back onto the displaced sphere surface.
    Vec3 snapToSurface(Vec3 worldPos) const {
        return surfacePos(worldPos - center);
    }

    // Distance from center to the displaced surface along this direction.
    float radiusAt(Vec3 worldPos) const {
        return radius + noiseHeight(worldPos);
    }

    // Is this surface point below sea level?
    bool isOcean(Vec3 worldPos) const {
        return noiseHeight(worldPos) <= 0.0f;
    }

    // ── Terrain queries (sphere-surface analogues of flat-world methods) ───────

    // Slope (sin of angle from horizontal) at a surface position.
    // Approximated by finite differences in the tangent plane.
    float slopeAt(Vec3 worldPos) const {
        Vec3 n   = normalAt(worldPos);
        Vec3 arb = (std::abs(n.y) < 0.9f)
                 ? Vec3{0.f, 1.f, 0.f}
                 : Vec3{1.f, 0.f, 0.f};

        // Tangent vectors via cross products
        Vec3 t1 = {n.y*arb.z - n.z*arb.y,
                   n.z*arb.x - n.x*arb.z,
                   n.x*arb.y - n.y*arb.x};
        t1 = t1.normalised();
        Vec3 t2 = {n.y*t1.z - n.z*t1.y,
                   n.z*t1.x - n.x*t1.z,
                   n.x*t1.y - n.y*t1.x};

        const float eps_step = 100.f;   // world-unit step for finite difference
        Vec3 p1 = snapToSurface(worldPos + t1 * eps_step);
        Vec3 p2 = snapToSurface(worldPos - t1 * eps_step);
        Vec3 p3 = snapToSurface(worldPos + t2 * eps_step);
        Vec3 p4 = snapToSurface(worldPos - t2 * eps_step);

        // Height difference along the normal direction
        float dh1 = (p1 - p2).dot(n);
        float dh2 = (p3 - p4).dot(n);
        float grad = std::sqrt(dh1*dh1 + dh2*dh2) / (2.f * eps_step);
        return std::sin(std::atan(grad));
    }

    // ── Spawn helpers ─────────────────────────────────────────────────────────

    // Random non-ocean surface position (uniform over the sphere).
    Vec3 randomLandPos(RNG& rng) const {
        for (int i = 0; i < 300; ++i) {
            // Marsaglia (1972) method: uniform point on unit sphere
            float a = rng.range(0.f, 6.2831853f);
            float z = rng.range(-1.f, 1.f);
            float s = std::sqrt(1.f - z * z);
            Vec3 dir = {s * std::cos(a), z, s * std::sin(a)};
            Vec3 pos = surfacePos(dir);
            if (!isOcean(pos)) return pos;
        }
        // Fallback: top of planet (usually above sea)
        return surfacePos({0.f, 1.f, 0.f});
    }

    // ── Water search ─────────────────────────────────────────────────────────
    // Scans a tangent-plane grid around `from`, returns the nearest ocean point.
    bool findOcean(const Vec3& from, float searchRadius, Vec3& outPos) const {
        Vec3 n   = normalAt(from);
        Vec3 arb = (std::abs(n.y) < 0.9f)
                 ? Vec3{0.f, 1.f, 0.f}
        : Vec3{1.f, 0.f, 0.f};

        Vec3 east = {n.y*arb.z - n.z*arb.y,
                   n.z*arb.x - n.x*arb.z,
                   n.x*arb.y - n.y*arb.x};
        east = east.normalised();
        Vec3 north = {n.y*east.z - n.z*east.y,
                   n.z*east.x - n.x*east.z,
                   n.x*east.y - n.y*east.x};

        // Adaptive step size to prevent millions of iterations on large vision ranges
        const float step  = std::max(50.f, searchRadius / 16.f);
        int         steps = (int)(searchRadius / step) + 1;
        float       bestD = 1e9f;
        bool        found = false;

        for (int dz = -steps; dz <= steps; ++dz) {
            for (int dx = -steps; dx <= steps; ++dx) {
                float distSq = (dx*step)*(dx*step) + (dz*step)*(dz*step);
                if (distSq > searchRadius*searchRadius) continue;

                Vec3 cand = from + east * (dx * step) + north * (dz * step);
                Vec3 dir = (cand - center).normalised();

                // Fast check first (2 octaves instead of 8)
                if (PlanetNoise::isOceanFast(dir.x, dir.y, dir.z)) {
                    cand = snapToSurface(cand);
                    if (isOcean(cand)) {
                        float d = (cand - from).len();
                        if (d < bestD) {
                            bestD  = d;
                            outPos = cand;
                            found  = true;
                        }
                    }
                }
            }
        }
        return found;
    }

    // ── Movement helpers ──────────────────────────────────────────────────────

    void localBasis(Vec3 worldPos, Vec3& outEast, Vec3& outNorth) const {
        Vec3 n   = normalAt(worldPos);
        Vec3 arb = (std::abs(n.y) < 0.9f)
                 ? Vec3{0.f, 1.f, 0.f}
        : Vec3{1.f, 0.f, 0.f};

        outEast = {n.y*arb.z - n.z*arb.y,
                   n.z*arb.x - n.x*arb.z,
                   n.x*arb.y - n.y*arb.x};
        outEast = outEast.normalised();
        outNorth = {n.y*outEast.z - n.z*outEast.y,
                    n.z*outEast.x - n.x*outEast.z,
                    n.x*outEast.y - n.y*outEast.x};
        outNorth = outNorth.normalised();
    }

    Vec3 facingDir(Vec3 worldPos, float yaw) const {
        Vec3 east, north;
        localBasis(worldPos, east, north);
        return east * std::sin(yaw) + north * std::cos(yaw);
    }

    // Project a velocity vector onto the tangent plane at `worldPos`.
    // Use this every physics tick so creatures don't drift off the sphere.
    Vec3 projectToTangent(Vec3 worldPos, Vec3 velocity) const {
        Vec3  n     = normalAt(worldPos);
        float vdotn = velocity.dot(n);
        return velocity - n * vdotn;
    }
};