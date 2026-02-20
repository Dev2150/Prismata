#pragma once
// ── PlanetNoise.hpp ───────────────────────────────────────────────────────────
// 3D Perlin + fractal Brownian motion for procedural planet terrain.
// Operates on a 3D direction vector (normalised), so terrain is seamless
// across all cube-sphere face boundaries with no visible seams.
// Intentionally self-contained (no global state) so it can be called from
// any thread during async mesh generation.

#include <cmath>
#include <cstdint>
#include <array>

namespace PlanetNoise {

// ── Internal permutation table ────────────────────────────────────────────────
// Seeded once via init(); stored in a plain array so multiple instances
// can coexist without static collisions with World_Gen.cpp's Perlin state.
struct State {
    int P[512] = {};
    bool ready = false;
};

inline State& state() { static State s; return s; }

inline void init(uint64_t seed) {
    State& s = state();
    // SplitMix64 → linear congruential to fill perm table
    uint64_t x = seed ^ 0x9e3779b97f4a7c15ULL;
    auto next = [&]() -> uint64_t {
        x += 0x9e3779b97f4a7c15ULL;
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    };

    // Fill p[0..255] with 0..255
    for (int i = 0; i < 256; i++) s.P[i] = i;
    // Fisher-Yates shuffle using the PRNG
    for (int i = 255; i > 0; i--) {
        int j = (int)(next() % (i + 1));
        int t = s.P[i]; s.P[i] = s.P[j]; s.P[j] = t;
    }
    // Double the table to avoid bounds checks in the hot path
    for (int i = 0; i < 256; i++) s.P[i + 256] = s.P[i];
    s.ready = true;
}

// ── Quintic fade ──────────────────────────────────────────────────────────────
// Zero first AND second derivatives at t=0,1 → no grid-aligned banding.
inline float fade(float t) { return t * t * t * (t * (t * 6.f - 15.f) + 10.f); }

inline float lerp(float t, float a, float b) { return a + t * (b - a); }

// ── Gradient dot-product (3D) ─────────────────────────────────────────────────
inline float grad3(int hash, float x, float y, float z) {
    int h = hash & 15;
    float u = h < 8 ? x : y;
    float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

// ── Single-octave 3D Perlin noise → result ∈ [-1, 1] ─────────────────────────
inline float perlin3(float x, float y, float z) {
    const int* P = state().P;

    int X = (int)std::floor(x) & 255;
    int Y = (int)std::floor(y) & 255;
    int Z = (int)std::floor(z) & 255;

    x -= std::floor(x);
    y -= std::floor(y);
    z -= std::floor(z);

    float u = fade(x), v = fade(y), w = fade(z);

    int A  = P[X  ] + Y, AA = P[A  ] + Z, AB = P[A+1] + Z;
    int B  = P[X+1] + Y, BA = P[B  ] + Z, BB = P[B+1] + Z;

    return lerp(w,
        lerp(v,
            lerp(u, grad3(P[AA  ], x,   y,   z  ), grad3(P[BA  ], x-1, y,   z  )),
            lerp(u, grad3(P[AB  ], x,   y-1, z  ), grad3(P[BB  ], x-1, y-1, z  ))),
        lerp(v,
            lerp(u, grad3(P[AA+1], x,   y,   z-1), grad3(P[BA+1], x-1, y,   z-1)),
            lerp(u, grad3(P[AB+1], x,   y-1, z-1), grad3(P[BB+1], x-1, y-1, z-1))));
}

// ── Fractal Brownian Motion (fBm) in 3D ───────────────────────────────────────
// Input: normalised direction on the sphere surface.
// The 3D position is scaled by `freq` before sampling so different seeds/scales
// produce independent features without visible repetition.
// Returns a value roughly in [-1, 1].
inline float fbm(float x, float y, float z,
                 int octaves = 8, float freq = 1.f,
                 float persistence = 0.5f, float lacunarity = 2.f)
{
    float val = 0.f, amp = 1.f, maxAmp = 0.f;
    for (int i = 0; i < octaves; i++) {
        val    += perlin3(x * freq, y * freq, z * freq) * amp;
        maxAmp += amp;
        amp    *= persistence;
        freq   *= lacunarity;
    }
    return val / maxAmp;
}

// ── Ridged noise ──────────────────────────────────────────────────────────────
// Creates sharp ridge-line features (like mountain ranges) by folding the
// noise: ridged = 1 - |perlin|. Multiple octaves sharpen the ridges.
inline float ridged(float x, float y, float z,
                    int octaves = 6, float freq = 1.f,
                    float persistence = 0.5f, float lacunarity = 2.f)
{
    float val = 0.f, amp = 1.f, maxAmp = 0.f;
    float prev = 1.f;
    for (int i = 0; i < octaves; i++) {
        float n = 1.f - std::abs(perlin3(x * freq, y * freq, z * freq));
        n *= n;          // sharpen the ridges
        n *= prev;       // cascade: ridge strength modulated by previous octave
        prev = n;
        val    += n * amp;
        maxAmp += amp;
        amp    *= persistence;
        freq   *= lacunarity;
    }
    return val / maxAmp;
}

// ── Continent mask ────────────────────────────────────────────────────────────
// Low-frequency noise that controls what's land vs ocean.
// Smoothstep applied so there are broad flat continents and clear coastlines.
inline float continentMask(float x, float y, float z, float freq = 0.4f) {
    float raw = fbm(x, y, z, 4, freq, 0.5f, 2.f);
    // Bias toward land by shifting the centre: -0.1 means ~55% land coverage
    raw = (raw - (-0.1f)) / 0.4f;
    // Smooth clamped to [0,1]
    float t = std::max(0.f, std::min(1.f, raw));
    return t * t * (3.f - 2.f * t);
}

// ── Full planet height sample ─────────────────────────────────────────────────
// Combines continent mask, hill fBm, and ridged mountain noise.
// Returns signed displacement from sphere radius in world units.
// dir must be a normalised 3D vector pointing at the surface point.
//
//  heightScale  – maximum terrain height above sea level (world units)
//  seaFloor     – how deep the ocean floor goes (fraction of heightScale)
inline float sampleHeight(float dx, float dy, float dz,
                          float heightScale = 100.f,
                          float seaFloor   = 0.3f,
                          uint64_t /*seed*/ = 0)
{
    // Low-frequency continent mask [0,1]: 0=deep ocean, 1=high land
    float continent = continentMask(dx, dy, dz, 0.35f);

    // Ocean floor: slight undulation below sea level
    float oceanH = fbm(dx, dy, dz, 3, 0.8f, 0.45f, 2.1f) * 0.15f;

    // Land terrain: blend between rolling hills and sharp mountains
    float hills   = fbm   (dx, dy, dz, 7, 1.2f, 0.52f, 2.f);
    float mounts  = ridged(dx, dy, dz, 5, 1.6f, 0.48f, 2.2f);

    // Mountain presence is gated by continent and a separate noise mask
    float mountMask = fbm(dx + 3.7f, dy + 1.1f, dz + 5.3f, 3, 0.5f);
    mountMask = std::max(0.f, mountMask);

    // Land height: mostly hills, pockets of mountains, clipped at [0,1]
    float landH = (hills * 0.6f + mounts * mountMask * 0.8f);

    // Blend: below coastline → ocean floor; above → land terrain
    float h;
    if (continent < 0.1f) {
        // Deep ocean
        h = -seaFloor + continent * (seaFloor / 0.1f) + oceanH * 0.1f;
    } else {
        // Smooth beach → land transition in continent [0.1, 0.3]
        float landFrac = std::min(1.f, (continent - 0.1f) / 0.2f);
        h = landFrac * landH;
    }

    return h * heightScale;
}

} // namespace PlanetNoise
