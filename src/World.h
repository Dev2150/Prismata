#pragma once
#include "Creature.h"
#include <vector>
#include <unordered_map>
#include <functional>
#include <cstdint>

// ── Terrain ───────────────────────────────────────────────────────────────────
struct VoxelColumn {
    float   height   = 0.f;
    uint8_t material = 0;  // 0=grass,1=rock,2=sand,3=water,4=snow
    uint8_t biome    = 0;
};

constexpr int CHUNK_SIZE = 32;

struct Chunk {
    int cx = 0, cz = 0;
    VoxelColumn cells[CHUNK_SIZE][CHUNK_SIZE];
    bool dirty = true;    // GPU mesh needs rebuild
};

// ── Plants ────────────────────────────────────────────────────────────────────
struct Plant {
    Vec3    pos;
    float   nutrition   = 30.f;
    float   growTimer   = 0.f;
    bool    alive       = true;
    uint8_t type        = 0;  // 0=grass,1=bush,2=tree
};

// ── Species registry ──────────────────────────────────────────────────────────
struct SpeciesInfo {
    uint32_t    id;
    std::string name;
    Genome      centroid;        // average genome of living members
    int         count    = 0;
    int         allTime  = 0;    // total ever born
    float       color[3] = {};   // display colour
};

// ── Simulation config (exposed to ImGui sliders) ──────────────────────────────
struct SimConfig {
    float simSpeed          = 1.0f;      // tick multiplier
    float mutationRateScale = 1.0f;      // global multiplier on genome mutation rate
    float speciesEpsilon    = 0.15f;     // genetic distance threshold for new species
    float plantGrowRate     = 0.5f;      // plants per chunk per second
    int   maxPopulation     = 2000;
    bool  paused            = false;
};

// Free function used by World internals and available externally
bool sameSpecies(const Genome& a, const Genome& b, float epsilon = 0.15f);

// ── World ─────────────────────────────────────────────────────────────────────
struct World {
    // ── Config ────────────────────────────────────────────────────────────────
    SimConfig cfg;
    uint64_t  seed     = 0;
    int       worldCX  = 16;   // chunks in X
    int       worldCZ  = 16;   // chunks in Z

    int       initial_herbivores = 200;
    int       initial_carnivores = initial_herbivores / 5;

    // ── Terrain ───────────────────────────────────────────────────────────────
    std::vector<Chunk> chunks;

    // Bilinear height at world-space (x, z)
    float heightAt(float x, float z) const;

    // Surface slope (sin of angle from horizontal) at (x,z)
    float slopeAt(float x, float z) const;

    // True if this column is passable water
    bool  isWater(float x, float z) const;

    // Nearest water position within radius
    bool  findWater(const Vec3& from, float radius, Vec3& outPos) const;

    // Public chunk accessor (for Renderer)
    const Chunk* chunkAtPublic(int cx, int cz) const { return chunkAt(cx, cz); }

    // Snap a world position to the terrain surface
    Vec3  snapToSurface(float x, float z) const;

    // ── Creatures ─────────────────────────────────────────────────────────────
    std::vector<Creature>               creatures;
    std::unordered_map<EntityID, size_t> idToIndex;   // O(1) lookup
    EntityID nextID = 1;

    Creature& spawnCreature(const Genome& g, const Vec3& pos,
                            EntityID parentA = INVALID_ID,
                            EntityID parentB = INVALID_ID,
                            uint32_t generation = 0);

    void removeDeadCreatures();

    EntityID findRandomLivingCreature() const;

    // ── Plants ────────────────────────────────────────────────────────────────
    std::vector<Plant> plants;
    Plant& spawnPlant(const Vec3& pos, uint8_t type = 0);

    // ── Species ───────────────────────────────────────────────────────────────
    std::vector<SpeciesInfo>             species;
    uint32_t nextSpeciesID = 1;

    // Assign or find species for a genome; updates centroid
    uint32_t classifySpecies(const Genome& g);
    void     updateSpeciesCentroids();
    const SpeciesInfo* getSpecies(uint32_t id) const;

    // ── Simulation ────────────────────────────────────────────────────────────
    float simTime = 0.f;
    void  tick(float dt);     // main simulation step

    // ── Initialisation ────────────────────────────────────────────────────────
    void generate(uint64_t seed, int chunksX, int chunksZ);
    void reset();

    // ── Serialisation ────────────────────────────────────────────────────────
    bool saveToFile(const char* path) const;
    bool loadFromFile(const char* path);
    void exportCSV(const char* path) const;

private:
    void  growPlants(float dt);
    void  tickCreatures(float dt);
    void  handleReproduction(float dt);
    void  perceive(Creature& c);      // update perception cache

    Chunk*       chunkAt(int cx, int cz);
    const Chunk* chunkAt(int cx, int cz) const;
    VoxelColumn& columnAt(int gx, int gz);

    // Simple spatial hash for creature proximity queries
    void rebuildSpatialHash();
    std::vector<EntityID> queryRadius(const Vec3& center, float radius) const;

    struct SpatialHash {
        float cellSize = 5.f;
        std::unordered_map<uint64_t, std::vector<EntityID>> cells;
        uint64_t key(int cx, int cz) const {
            return (static_cast<uint64_t>(cx + 30000) << 32)
                 |  static_cast<uint64_t>(cz + 30000);
        }
    } spatialHash;
};
