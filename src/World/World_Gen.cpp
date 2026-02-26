#include "World.hpp"
#include <vector>
#include "World_Planet.hpp"

void World::generate(uint64_t s, int cx, int cz) {
    seed    = s;
    worldCX = cx;
    worldCZ = cz;

    // Seed 3-D Perlin noise used by PlanetNoise (shared with PlanetRenderer).
    initPlanetNoise(seed);

    // Build chunk grid for material storage (renderer still uses it for the
    // flat-world chunk mesh cache; we zero it out since planet mode doesn't
    // use flat chunk meshes for terrain).
    chunks.clear();
    chunks.resize(worldCX * worldCZ);

    for (int iz = 0; iz < worldCZ; iz++) {
        for (int ix = 0; ix < worldCX; ix++) {
            Chunk& chunk = chunks[iz * worldCX + ix];
            chunk.cx = ix;
            chunk.cz = iz;
            chunk.dirty = false;  // flat chunk meshes not used in planet mode
        }
    }

    // Seed initial plant population: one plant per ~8 cells on non-water terrain
    RNG rng(seed + 1);

    // ── Plant population ──────────────────────────────────────────────────────
    // Seed ~2000 plants on random land positions.
    constexpr int numPlants = 2000;
    for (int i = 0; i < numPlants; i++) {
        Vec3 pos = g_planet_surface.randomLandPos(rng);
        spawnPlant(pos, (uint8_t)(rng.uniform() * 3));
    }

    // ── Creature population ───────────────────────────────────────────────────
    auto spawnN = [&](int n, bool herb) {
        for (int i = 0; i < n; i++) {
            Vec3   sp = g_planet_surface.randomLandPos(rng);
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