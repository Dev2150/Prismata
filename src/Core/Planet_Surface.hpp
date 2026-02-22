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

struct PlanetSurface {
    Vec3  center      = {0.f, -1800.f, 0.f};
    float radius      = 1000.f;
    float heightScale = 120.f;
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
        float h = PlanetNoise::sampleHeight(d.x, d.y, d.z, heightScale);
        return std::max(h, 0.0f); // Clamp to sea level
    }


    // Snap a world-space position back onto the displaced sphere surface.
    Vec3 snapToSurface(Vec3 worldPos) const {
        return surfacePos(worldPos - center);
    }

    // Noise-based height above the sphere's base radius (negative = below).
    float noiseHeight(Vec3 worldPos) const {
        Vec3 d = (worldPos - center).normalised();
        return PlanetNoise::sampleHeight(d.x, d.y, d.z, heightScale);
    }

    // Distance from center to the displaced surface along this direction.
    float radiusAt(Vec3 worldPos) const {
        return radius + noiseHeight(worldPos);
    }

    // Is this surface point below sea level?
    bool isOcean(Vec3 worldPos) const {
        Vec3 d = (worldPos - center).normalised();
        return isOceanFast(d.x, d.y, d.z);  // 4X faster
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

        const float eps = 1.f;   // world-unit step for finite difference
        Vec3 p1 = snapToSurface(worldPos + t1 * eps);
        Vec3 p2 = snapToSurface(worldPos - t1 * eps);
        Vec3 p3 = snapToSurface(worldPos + t2 * eps);
        Vec3 p4 = snapToSurface(worldPos - t2 * eps);

        // Height difference along the normal direction
        float dh1 = (p1 - p2).dot(n);
        float dh2 = (p3 - p4).dot(n);
        float grad = std::sqrt(dh1*dh1 + dh2*dh2) / (2.f * eps);
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

        Vec3 t1 = {n.y*arb.z - n.z*arb.y,
                   n.z*arb.x - n.x*arb.z,
                   n.x*arb.y - n.y*arb.x};
        t1 = t1.normalised();
        Vec3 t2 = {n.y*t1.z - n.z*t1.y,
                   n.z*t1.x - n.x*t1.z,
                   n.x*t1.y - n.y*t1.x};

        const float step  = 10.f;
        int         steps = (int)(searchRadius / step) + 1;
        float       bestD = 1e9f;
        bool        found = false;

        for (int dz = -steps; dz <= steps; ++dz) {
            for (int dx = -steps; dx <= steps; ++dx) {
                Vec3 cand = from + t1 * (dx * step) + t2 * (dz * step);
                cand      = snapToSurface(cand);
                if (isOcean(cand)) {
                    float d = (cand - from).len();
                    if (d < bestD && d < searchRadius) {
                        bestD  = d;
                        outPos = cand;
                        found  = true;
                    }
                }
            }
        }
        return found;
    }

    // ── Movement helpers ──────────────────────────────────────────────────────

    // Project a velocity vector onto the tangent plane at `worldPos`.
    // Use this every physics tick so creatures don't drift off the sphere.
    Vec3 projectToTangent(Vec3 worldPos, Vec3 velocity) const {
        Vec3  n     = normalAt(worldPos);
        float vdotn = velocity.dot(n);
        return velocity - n * vdotn;
    }
};