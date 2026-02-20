#include "World.hpp"
#include <cmath>
#include <numeric>
#include <vector>
#include <iostream>

// ── Perlin noise ──────────────────────────────────────────────────────────────
// Classic Ken Perlin gradient noise (improved 2002 version).
// Produces smooth, continuous, band-limited noise suitable for terrain generation.
// We layer several octaves (fractal Brownian motion) for natural-looking results.
namespace {
    // P[i] is a doubled permutation table of [0,255], used to hash grid cell
    // coordinates into gradient directions. Doubling to 512 entries avoids
    // modulo operations in the hot path.
    int P[512];
    bool perlinInit = false;

    // Shuffle the permutation table with a seed so terrain is deterministic.
    void initPerlin(uint64_t seed) {
        RNG rng(seed);
        std::vector<int> tmp(256);
        std::iota(tmp.begin(), tmp.end(), 0);   // fill 0..255
        // Fisher-Yates shuffle for an unbiased random permutation
        for (int i = 255; i > 0; i--) {
            int j = static_cast<int>(rng.uniform() * (i + 1));
            std::swap(tmp[i], tmp[j]);
        }
        for (int i = 0; i < 512; i++) P[i] = tmp[i & 255];
        perlinInit = true;
    }

    // Quintic fade: smoothstep with zero first AND second derivatives at t=0,1.
    // Using t³(6t²-15t+10) instead of the classic 3t²-2t³ eliminates visible
    // grid-aligned artifacts in the gradient noise.
    float fade(float t)  { return t * t * t * (t * (t * 6 - 15) + 10); }

    float lerp(float t, float a, float b) { return a + t * (b - a); }

    // Map a hash value to a gradient direction, then dot with (x,y,z).
    // The 16 possible gradients (h & 15) are carefully chosen to distribute
    // evenly in 3D so the noise has no directional bias.
    float grad(int hash, float x, float y, float z) {
        int h = hash & 15;
        float u = h < 8 ? x : y;
        float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
        return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
    }

    // Single-octave Perlin noise at point (x,y,z). Result in approximately [-1,1].
    // Integer parts are hashed via the permutation table; fractional parts feed
    // the fade function and gradient dot products.
    float perlin(float x, float y, float z = 0) {
        // Integer cell coordinates (masked to [0,255] for the permutation table)
        int X = (int)std::floor(x) & 255, Y = (int)std::floor(y) & 255, Z = (int)std::floor(z) & 255;
        // Fractional offsets within the cell
        x -= std::floor(x); y -= std::floor(y); z -= std::floor(z);
        // Smooth interpolation weights
        float u = fade(x), v = fade(y), w = fade(z);
        // Hash the eight corners of the unit cube
        int A = P[X]+Y, AA = P[A]+Z, AB = P[A+1]+Z, B = P[X+1]+Y, BA = P[B]+Z, BB = P[B+1]+Z;
        // Trilinear interpolation between the eight gradient dot products
        return lerp(w,
            lerp(v,
                lerp(u, grad(P[AA],x,y,z),   grad(P[BA],x-1,y,z)),
                lerp(u, grad(P[AB],x,y-1,z), grad(P[BB],x-1,y-1,z))),
            lerp(v,
                lerp(u, grad(P[AA+1],x,y,z-1),   grad(P[BA+1],x-1,y,z-1)),
                lerp(u, grad(P[AB+1],x,y-1,z-1), grad(P[BB+1],x-1,y-1,z-1))));
    }

    // Fractional Brownian Motion (fBm): sum several octaves of Perlin noise.
    // octaves -> #iterations/layers
    // lacunarity -> #bumps compared with previous layer
    // Each octave doubles the frequency (lacunarity=2) and halves the amplitude
    // (persistence=0.5), adding progressively finer detail. The result is
    // normalised by the sum of amplitudes so it stays in roughly [-1, 1].
    float fractional_brownian_motion(float x, float z, int octaves = 6, float persistence = 0.5f, float lacunarity = 2.f) {
        float val = 0, amp = 1, freq = 1, max = 0;
        for (int i = 0; i < octaves; i++) {
            val += perlin(x * freq, z * freq) * amp;
            max += amp;
            amp  *= persistence;   // each octave contributes half as much as the previous
            freq *= lacunarity;    // each octave samples twice as finely
        }
        return val / max;
    }
}   // anonymous namespace

// ── World generation ──────────────────────────────────────────────────────────
void World::generate(uint64_t s, int cx, int cz) {
    seed    = s;
    worldCX = cx;
    worldCZ = cz;
    initPerlin(seed);

    chunks.clear();
    chunks.resize(worldCX * worldCZ);

    for (int iz = 0; iz < worldCZ; iz++) {
        for (int ix = 0; ix < worldCX; ix++) {
            Chunk& chunk = chunks[iz * worldCX + ix];
            chunk.cx = ix;
            chunk.cz = iz;
            chunk.dirty = true;

            for (int lz = 0; lz < CHUNK_SIZE; lz++) {
                for (int lx = 0; lx < CHUNK_SIZE; lx++) {
                    // Convert chunk-local coords to world (global) grid coords
                    int gx = ix * CHUNK_SIZE + lx;
                    int gz = iz * CHUNK_SIZE + lz;

                    // Scale world coords to noise space; 0.04 ≈ one feature per 25 cells
                    float wx = gx * 0.04f;
                    float wz = gz * 0.04f;

                    // Three independent noise fields using offset seeds to break correlation
                    float elev = fractional_brownian_motion(wx, wz);            // elevation
                    float temp = fractional_brownian_motion(wx + 100, wz, 4);   // temperature (fewer octaves = broader regions)
                    float hmdt = fractional_brownian_motion(wx + 200, wz, 4);   // humidity

                    // Map noise [-1,1] to a height in metres
                    float height = 3.f + elev * 6.f;

                    auto& col = chunk.cells[lz][lx];
                    col.height = height;

                    // Biome / material assignment based on height, temperature, and humidity
                    if (height <= 0.f) {
                        col.material = 3;   // water (below sea level)
                    } else if (elev > 0.5f) {
                        col.material = (temp > 0.2f) ? 4 : 1;   // high elevation: snow (warm) or rock (cold)
                    } else if (hmdt > 0.3f && temp < 0.4f) {
                        col.material = 0;   // moist and cool: grass
                    } else {
                        col.material = 2;   // otherwise: sand / arid dirt
                    }
                }
            }
        }
    }

    // Seed initial plant population: one plant per ~8 cells on non-water terrain
    RNG rng(seed + 1);
    int wGX = worldCX * CHUNK_SIZE;
    int wGZ = worldCZ * CHUNK_SIZE;
    for (int i = 0; i < wGX * wGZ / 8; i++) {
        float px = rng.range(0, (float)wGX);
        float pz = rng.range(0, (float)wGZ);
        if (!isWater(px, pz))
            spawnPlant(snapToSurface(px, pz));
    }
    std::cout << "Spawned " << wGX * wGZ / 8 << " plants";

    // Spawn initial creature population
    auto spawnN = [&](int n, bool herb) {
        for (int i = 0; i < n; i++) {
            float cx2 = rng.range(2.f, (float)wGX - 2.f);
            float cz2 = rng.range(2.f, (float)wGZ - 2.f);
            Vec3 sp   = snapToSurface(cx2, cz2);
            Genome g  = herb ? Genome::randomHerbivore(rng) : Genome::randomCarnivore(rng);
            spawnCreature(g, sp);
        }
    };
    spawnN(initial_herbivores, true);
    spawnN(initial_carnivores, false);
}

void World::reset() {
    creatures.clear();
    idToIndex.clear();
    plants.clear();
    species.clear();
    nextID       = 1;
    nextSpeciesID= 1;
    simTime      = 0.f;
    generate(seed, worldCX, worldCZ);
}