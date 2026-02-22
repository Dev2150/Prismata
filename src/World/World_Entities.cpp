#include <random>
#include "World.hpp"
#include "World_Planet.hpp"
#include "Sim/Creature.hpp"
#include "tracy/Tracy.hpp"

// ── Entity management ─────────────────────────────────────────────────────────
Creature& World::spawnCreature(const Genome& g, const Vec3& pos,
                               EntityID pA, EntityID pB, uint32_t gen) {
    creatures.emplace_back();
    Creature& c  = creatures.back();
    c.id         = nextID++;
    c.parentA    = pA;
    c.parentB    = pB;
    c.generation = gen;
    c.genome     = g;
    c.speciesID  = classifySpecies(g);  // assign to nearest existing species or create new one
    c.initFromGenome(pos);

    idToIndex[c.id] = creatures.size() - 1;
    return c;
}

Plant& World::spawnPlant(const Vec3& pos, uint8_t type) {
    plants.emplace_back();
    Plant& p  = plants.back();
    p.pos      = pos;
    p.type     = type;
    p.nutrition= 20.f + type * 10.f;   // bushes and trees are more nutritious than grass
    return p;
}

// Compact the creatures vector, removing all dead entries.
// After removal, rebuilds idToIndex so all EntityID→index mappings are valid.
// Called once per tick after all creature updates so we never read stale indices
// during the tick itself.
void World::removeDeadCreatures() {
    creatures.erase(
        std::remove_if(creatures.begin(), creatures.end(),
                       [](const Creature& c){ return !c.alive; }),
        creatures.end());
    idToIndex.clear();
    for (size_t i = 0; i < creatures.size(); i++)
        idToIndex[creatures[i].id] = i;
}

// ── Spatial hash ──────────────────────────────────────────────────────────────
// Divides the world into a grid of cells (cellSize × cellSize metres).
// Each cell stores the IDs of creatures whose position falls within it.
// This turns O(n²) pair-wise distance checks into O(n × k) where k is the
// average number of creatures per query region — typically much smaller than n.
void World::rebuildSpatialHash() {
    ZoneScoped;
    spatialHash.cells.clear();
    for (const auto& c : creatures) {
        if (!c.alive) continue;
        // Integer cell address
        int cx = (int)(c.pos.x / spatialHash.cellSize);
        int cz = (int)(c.pos.z / spatialHash.cellSize);
        spatialHash.cells[spatialHash.key(cx, cz)].push_back(c.id);
    }
}

// Return all creature IDs within `radius` metres of `center`.
// Checks every grid cell that overlaps the query circle (a square ring of cells),
// then filters by actual Euclidean distance to avoid returning corners of the
// bounding square.
std::vector<EntityID> World::queryRadius(const Vec3& center, float radius) const {
    std::vector<EntityID> result;
    // Number of cells to check in each direction (add 1 for safety)
    int r = (int)std::ceil(radius / spatialHash.cellSize) + 1;
    int cx0 = (int)(center.x / spatialHash.cellSize);
    int cz0 = (int)(center.z / spatialHash.cellSize);
    for (int dz = -r; dz <= r; dz++) {
        for (int dx = -r; dx <= r; dx++) {
            auto it = spatialHash.cells.find(spatialHash.key(cx0+dx, cz0+dz));
            if (it == spatialHash.cells.end()) continue;
            for (EntityID id : it->second) {
                auto ii = idToIndex.find(id);
                if (ii == idToIndex.end()) continue;
                const Creature& c2 = creatures[ii->second];
                if (dist(c2.pos, center) <= radius)
                    result.push_back(id);
            }
        }
    }
    return result;
}

EntityID World::findRandomLivingCreature() const {
    std::vector<EntityID> livingCreatures;
    for (const auto& creature : creatures)
        if (creature.alive) livingCreatures.push_back(creature.id);
    if (livingCreatures.empty()) return INVALID_ID;

    // Use a separate mt19937 seeded from hardware entropy so "possess" key picks
    // feel random each press, independent of the deterministic simulation RNG.
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> dist(0, livingCreatures.size() - 1);
    return livingCreatures[dist(rng)];
}

// ── Plant growth ──────────────────────────────────────────────────────────────
void World::growPlants(float dt) {
    // Regrow eaten (dead) plants after a fixed 30-second timer
    ZoneScoped;
    for (auto& p : plants) {
        if (p.alive) continue;
        p.growTimer += dt;
        if (p.growTimer > 30.f) {
            p.alive     = true;
            p.nutrition = 20.f + p.type * 10.f;
            p.growTimer = 0.f;
        }
    }

    // Spontaneous new plants on land
    int alive = 0;
    for (const auto& p : plants) alive += p.alive ? 1 : 0;
    const int cap = 3000;
    if (alive < cap) {
        // Integer portion always spawns; fractional part spawns with its probability
        int toSpawn = (int)(cfg.plantGrowRate * dt)
                    + (globalRNG().chance(cfg.plantGrowRate * dt
                                         - (int)(cfg.plantGrowRate * dt)) ? 1 : 0);
        RNG& rng = globalRNG();
        for (int i = 0; i < toSpawn; i++) {
            Vec3 pos = g_planet_surface.randomLandPos(globalRNG());
            spawnPlant(pos);
        }
    }

    // Evict plant entries that have been dead for over 60 seconds to cap vector growth
    plants.erase(
        std::remove_if(plants.begin(), plants.end(),
                       [](const Plant& p){ return !p.alive && p.growTimer > 60.f; }),
        plants.end());
}