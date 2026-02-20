#include "World.hpp"
#include <cmath>
#include <algorithm>
#include <cassert>
#include <fstream>
#include <sstream>
#include <numeric>
#include <vector>
#include <random>

// ── Perlin noise ──────────────────────────────────────────────────────────────
// Classic Ken Perlin gradient noise (improved 2002 version).
// Produces smooth, continuous, band-limited noise suitable for terrain generation.
// We layer several octaves (fractal Brownian motion) for natural-looking results.
namespace {

// P[i] is a doubled permutation table of [0,255], used to hash grid cell
// coordinates into gradient directions. Doubling to 512 entries avoids
// modulo operations in the hot path.
static int P[512];
static bool perlinInit = false;

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
// Each octave doubles the frequency (lacunarity=2) and halves the amplitude
// (persistence=0.5), adding progressively finer detail. The result is
// normalised by the sum of amplitudes so it stays in roughly [-1, 1].
float octaveNoise(float x, float z, int octaves = 6, float persistence = 0.5f, float lacunarity = 2.f) {
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
                    float h  = octaveNoise(wx, wz);            // elevation
                    float t  = octaveNoise(wx + 100, wz, 4);   // temperature (fewer octaves = broader regions)
                    float hm = octaveNoise(wx + 200, wz, 4);   // humidity

                    // Map noise [-1,1] to a height in metres
                    float height = 8.f + h * 12.f;

                    auto& col = chunk.cells[lz][lx];
                    col.height = std::max(0.f, height);   // clamp so nothing is below sea level

                    // Biome / material assignment based on height, temperature, and humidity
                    if (height < 1.f) {
                        col.material = 3;   // water (below sea level)
                    } else if (h > 0.5f) {
                        col.material = (t > 0.2f) ? 4 : 1;   // high elevation: snow (warm) or rock (cold)
                    } else if (hm > 0.3f && t < 0.4f) {
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

// ── Terrain helpers ───────────────────────────────────────────────────────────
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

EntityID World::findRandomLivingCreature() const {
    std::vector<EntityID> livingCreatures;
    for (const auto& creature : creatures) {
        if (creature.alive) {
            livingCreatures.push_back(creature.id);
        }
    }
    if (livingCreatures.empty()) return INVALID_ID;

    // Use a separate mt19937 seeded from hardware entropy so "possess" key picks
    // feel random each press, independent of the deterministic simulation RNG.
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<size_t> dist(0, livingCreatures.size() - 1);
    return livingCreatures[dist(rng)];
}

// ── Spatial hash ──────────────────────────────────────────────────────────────
// Divides the world into a grid of cells (cellSize × cellSize metres).
// Each cell stores the IDs of creatures whose position falls within it.
// This turns O(n²) pair-wise distance checks into O(n × k) where k is the
// average number of creatures per query region — typically much smaller than n.
void World::rebuildSpatialHash() {
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

// ── Species registry ──────────────────────────────────────────────────────────
// Implements a simple nearest-centroid clustering algorithm for species.
// A creature belongs to the species whose centroid genome is closest (RMS distance).
// If the closest centroid is still farther than speciesEpsilon, a new species
// is formed. This produces speciation events when lineages diverge enough.
uint32_t World::classifySpecies(const Genome& g) {
    float bestDist = 1e9f;
    uint32_t bestID = 0;

    // Find the nearest existing (non-extinct) species centroid
    for (auto& sp : species) {
        if (sp.count == 0) continue;   // skip extinct species
        float d = g.distanceTo(sp.centroid);
        if (d < bestDist) { bestDist = d; bestID = sp.id; }
    }

    // If no species is close enough (or the registry is empty), form a new species
    if (bestDist > cfg.speciesEpsilon || species.empty()) {
        SpeciesInfo sp;
        sp.id      = nextSpeciesID++;
        sp.centroid= g;
        sp.count   = 1;
        sp.allTime = 1;

        // Derive a display colour from the genome hue (6-sector HSV approximation)
        float h = g.hue() / 60.f;
        int   hi = (int)h;
        float f  = h - hi;
        float p  = 0.3f, q = 0.3f + 0.7f * (1-f), tv = 0.3f + 0.7f * f;
        float rgb[6][3] = {
            {1,tv,p},{q,1,p},{p,1,tv},{p,q,1},{tv,p,1},{1,p,q}
        };
        sp.color[0] = rgb[hi%6][0];
        sp.color[1] = rgb[hi%6][1];
        sp.color[2] = rgb[hi%6][2];

        // Simple procedural name from a small syllable list + numeric suffix
        const char* parts[] = {"Azel","Brix","Calu","Dorn","Evon","Fyx","Gorn","Hexa"};
        sp.name = std::string(parts[sp.id % 8]) + std::to_string(sp.id);

        species.push_back(sp);
        return sp.id;
    }

    // Increment population count of the matched species
    auto it = std::find_if(species.begin(), species.end(),
                           [&](const SpeciesInfo& s){ return s.id == bestID; });
    if (it != species.end()) { it->count++; it->allTime++; }
    return bestID;
}

// Recompute each species' centroid genome by averaging all living members' raw genes.
// Also resets and recounts species populations. Called every 5 simulated seconds
// (not every tick) to amortise the O(creatures + species) cost.
void World::updateSpeciesCentroids() {
    // Zero all counts and centroid accumulators
    for (auto& sp : species) {
        sp.count = 0;
        sp.centroid = Genome{};   // zeroed raw array
    }
    // Sum genome values per species
    for (const auto& c : creatures) {
        if (!c.alive) continue;
        auto it = std::find_if(species.begin(), species.end(),
                               [&](const SpeciesInfo& s){ return s.id == c.speciesID; });
        if (it == species.end()) continue;
        it->count++;
        for (int i = 0; i < GENOME_SIZE; i++)
            it->centroid.raw[i] += c.genome.raw[i];
    }
    // Divide by count to get the mean genome per species
    for (auto& sp : species) {
        if (sp.count == 0) continue;
        for (int i = 0; i < GENOME_SIZE; i++)
            sp.centroid.raw[i] /= sp.count;
    }
}

const SpeciesInfo* World::getSpecies(uint32_t id) const {
    for (const auto& sp : species)
        if (sp.id == id) return &sp;
    return nullptr;
}

// ── Perception ────────────────────────────────────────────────────────────────
// Updates a creature's perception cache with the nearest predator, prey, mate,
// food, and water visible within its vision cone.
//
// Perception pipeline:
//  1. Reset all cached values to "nothing seen"
//  2. Query the spatial hash for all creatures within visionRange
//  3. For each candidate, check the FOV cone (dot product with facing direction)
//  4. Classify the candidate as predator / prey / mate and update the nearest cache
//  5. Scan all plants for the nearest visible food source
//  6. Search nearby tiles for the nearest water source
//  7. Update the Fear drive based on predator proximity
void World::perceive(Creature& c) {
    float range  = c.genome.visionRange();
    // Half-angle of the FOV cone in radians; creatures behind are invisible
    float fovRad = c.genome.visionFOV() * 3.14159f / 180.f;

    // Reset all perception caches to "nothing found" sentinel values
    c.nearestPredator  = INVALID_ID; c.nearestPredDist = 1e9f;
    c.nearestPrey      = INVALID_ID; c.nearestPreyDist = 1e9f;
    c.nearestMate      = INVALID_ID; c.nearestMateDist = 1e9f;
    c.nearestFoodDist  = 1e9f;
    c.nearestWaterDist = 1e9f;

    auto nearby = queryRadius(c.pos, range);
    for (EntityID oid : nearby) {
        if (oid == c.id) continue;   // skip self
        auto it = idToIndex.find(oid);
        if (it == idToIndex.end()) continue;
        const Creature& o = creatures[it->second];
        if (!o.alive) continue;

        // FOV cone check: project the direction to the other creature onto the
        // facing vector. If the cosine is below cos(halfFOV), the target is outside
        // the cone and is treated as invisible.
        Vec3 toO = o.pos - c.pos; toO.y = 0;
        Vec3 facing = {std::sin(c.yaw), 0, std::cos(c.yaw)};
        float d = toO.len();
        if (d > 0.1f) {
            float cosA = toO.normalised().dot(facing);
            if (cosA < std::cos(fovRad * 0.5f)) continue;   // outside FOV cone
        }

        // Determine the ecological relationship between c and o
        bool oIsPredator = o.isCarnivore() && c.isHerbivore();   // o hunts c
        bool oIsPrey     = c.isCarnivore() && o.isHerbivore();   // c can hunt o
        // A potential mate must be the same species AND have a high libido drive
        bool oIsMate     = (o.speciesID == c.speciesID)
                        && (o.needs.level[(int)Drive::Libido] > 0.5f);

        if (oIsPredator && d < c.nearestPredDist) {
            c.nearestPredDist = d; c.nearestPredator = oid;
        }
        if (oIsPrey && d < c.nearestPreyDist) {
            c.nearestPreyDist = d; c.nearestPrey = oid;
        }
        if (oIsMate && d < c.nearestMateDist) {
            c.nearestMateDist = d; c.nearestMate = oid;
        }
    }

    // Scan all plants: O(plants) but plants are generally sparse relative to
    // vision range so this is fast in practice. A spatial hash for plants would
    // help at very high plant counts.
    for (const auto& p : plants) {
        if (!p.alive) continue;
        float d = dist(c.pos, p.pos);
        if (d < range && d < c.nearestFoodDist) {
            c.nearestFoodDist = d;
            c.nearestFood     = p.pos;
        }
    }

    // Water search: only run if no water has been cached within range yet
    if (c.nearestWaterDist > range) {
        Vec3 wp;
        if (findWater(c.pos, range, wp)) {
            c.nearestWaterDist = dist(c.pos, wp);
            c.nearestWater     = wp;
        }
    }

    // Update Fear drive based on predator visibility
    if (c.nearestPredator != INVALID_ID) {
        // distNorm: 0 = predator is adjacent, 1 = predator is at the edge of vision
        float distNorm = c.nearestPredDist / range;
        c.needs.raiseFear(distNorm, c.genome.fearSensitivity(), 1.f/60.f);
    } else {
        // No predator in sight: fear gradually decays back toward 0
        c.needs.decayFear(1.f/60.f);
    }
}

// ── Plant growth ──────────────────────────────────────────────────────────────
void World::growPlants(float dt) {
    // Regrow eaten (dead) plants after a fixed 30-second timer
    for (auto& p : plants) {
        if (p.alive) continue;
        p.growTimer += dt;
        if (p.growTimer > 30.f) {
            p.alive     = true;
            p.nutrition = 20.f + p.type * 10.f;
            p.growTimer = 0.f;
        }
    }

    // Spontaneous new plant spawning up to a density cap.
    // The fractional part of (plantGrowRate * dt) is treated as a probability
    // so the expected spawn rate is exactly plantGrowRate plants/second on average.
    int alive = 0;
    for (const auto& p : plants) alive += p.alive ? 1 : 0;
    int cap = worldCX * worldCZ * CHUNK_SIZE / 4;   // roughly one plant per 4 cells
    if (alive < cap) {
        // Integer portion always spawns; fractional part spawns with its probability
        int toSpawn = (int)(cfg.plantGrowRate * dt)
                    + (globalRNG().chance(cfg.plantGrowRate * dt
                                         - (int)(cfg.plantGrowRate * dt)) ? 1 : 0);
        RNG& rng = globalRNG();
        for (int i = 0; i < toSpawn; i++) {
            float px = rng.range(0.f, (float)(worldCX * CHUNK_SIZE));
            float pz = rng.range(0.f, (float)(worldCZ * CHUNK_SIZE));
            if (!isWater(px, pz))
                spawnPlant(snapToSurface(px, pz));
        }
    }

    // Evict plant entries that have been dead for over 60 seconds to cap vector growth
    plants.erase(
        std::remove_if(plants.begin(), plants.end(),
                       [](const Plant& p){ return !p.alive && p.growTimer > 60.f; }),
        plants.end());
}

// ── Creature tick (defined in World.cpp because it accesses World internals) ──
float Creature::tick(float dt, World& world) {
    if (!alive) return 0.f;

    age += dt;
    needs.tick(dt);  // accumulate all drives by their crave rates

    // Old-age penalty: energy drains faster after 80% of lifespan
    float ageFrac = age / lifespan;
    if (ageFrac > 0.8f)
        energy -= 0.02f * mass * dt;

    Drive active = needs.activeDrive();  // which drive governs behaviour this frame
    float spd    = speedCap();           // energy-throttled top speed
    float slope  = world.slopeAt(pos.x, pos.z);

    // ── Behaviour state machine ───────────────────────────────────────────────
    // Each case sets `behavior`, then either steers the creature or modifies
    // its state (e.g. sleeping, eating). Multiple drives can share a case via
    // fall-through to idle when a target is unavailable.
    switch (active) {

    // FLEE: highest-priority survival response. Overrides all other drives.
    case Drive::Fear:
        behavior = BehaviorState::Fleeing;
        if (world.idToIndex.count(nearestPredator)) {
            const Creature& pred = world.creatures[world.idToIndex.at(nearestPredator)];
            steerAway(pred.pos, spd, dt);
        }
        break;

    // HUNGER: seek food (plants for herbivores, prey for carnivores)
    case Drive::Hunger:
        if (isCarnivore() && nearestPrey != INVALID_ID) {
            behavior = BehaviorState::Hunting;
            const Creature& prey = world.creatures[world.idToIndex.at(nearestPrey)];
            steerToward(prey.pos, spd, dt);
            // Bite if close enough (within 1.2 m, approximately melee range)
            if (nearestPreyDist < 1.2f) {
                Creature& prey2 = world.creatures[world.idToIndex.at(nearestPrey)];
                float bite = 20.f * genome.carnEfficiency() * dt;  // damage per second
                prey2.energy -= bite;
                energy = std::min(maxEnergy, energy + bite * 0.7f);  // 70% energy transfer efficiency
                if (prey2.energy <= 0) prey2.alive = false;
                needs.satisfy(Drive::Hunger, bite / 50.f);
            }
        } else if (isHerbivore() && nearestFoodDist < genome.visionRange()) {
            behavior = BehaviorState::SeekFood;
            steerToward(nearestFood, spd, dt);
            if (nearestFoodDist < 1.2f) {
                // Graze: consume up to 15*herbEff nutrition per second from the nearest plant
                for (auto& p : world.plants) {
                    if (!p.alive) continue;
                    if (dist(pos, p.pos) < 1.2f) {
                        float eaten = std::min(p.nutrition, 15.f * genome.herbEfficiency() * dt);
                        p.nutrition -= eaten;
                        if (p.nutrition <= 0) p.alive = false;
                        energy = std::min(maxEnergy, energy + eaten);
                        needs.satisfy(Drive::Hunger, eaten / 30.f);
                        break;
                    }
                }
            }
        } else {
            // No food visible: wander randomly (Gaussian step so direction varies smoothly)
            behavior = BehaviorState::Idle;
            Vec3 wander = {globalRNG().normal(0,1), 0, globalRNG().normal(0,1)};
            steerToward(pos + wander * 5.f, spd * 0.3f, dt);
        }
        break;

    // THIRST: navigate to water and drink on arrival
    case Drive::Thirst:
        behavior = BehaviorState::SeekWater;
        if (nearestWaterDist < genome.visionRange()) {
            steerToward(nearestWater, spd, dt);
            if (nearestWaterDist < 1.5f) {
                needs.satisfy(Drive::Thirst, 0.5f * dt);   // drink at 0.5 units/sec
            }
        }
        break;

    // SLEEP: stop moving, recover energy rapidly, and reduce sleep need
    case Drive::Sleep:
        behavior = BehaviorState::Sleeping;
        vel = {0, 0, 0};
        energy = std::min(maxEnergy, energy + 5.f * dt);    // 5 energy/sec while sleeping
        needs.satisfy(Drive::Sleep, 0.3f * dt);
        break;

    // LIBIDO: approach the nearest compatible mate; mating itself is handled by
    // World::handleReproduction() once both partners are adjacent and willing
    case Drive::Libido:
        if (nearestMate != INVALID_ID) {
            behavior = BehaviorState::SeekMate;
            const Creature& mate = world.creatures[world.idToIndex.at(nearestMate)];
            steerToward(mate.pos, spd * 0.6f, dt);   // approach at 60% speed (less urgent than hunger)
        }
        break;

    default:
        behavior = BehaviorState::Idle;
        break;
    }

    // ── Apply movement ────────────────────────────────────────────────────────
    if (vel.len2() > 0.001f) {
        // Only move if the slope is navigable OR if movement reduces altitude
        // (i.e., the creature is moving downhill; dot product check lets them slide down)
        if (slope * (180.f / 3.14159f) < genome.maxSlope()
            || vel.dot(pos - world.snapToSurface(pos.x + vel.x, pos.z + vel.z)) < 0) {
            pos.x += vel.x * dt;
            pos.z += vel.z * dt;
        }
        // Keep Y snapped to the terrain surface at all times
        pos.y = world.heightAt(pos.x, pos.z);
    }

    // ── World boundary clamp ──────────────────────────────────────────────────
    float maxX = (float)(world.worldCX * CHUNK_SIZE - 1);
    float maxZ = (float)(world.worldCZ * CHUNK_SIZE - 1);
    pos.x = std::clamp(pos.x, 0.f, maxX);
    pos.z = std::clamp(pos.z, 0.f, maxZ);

    // ── Energy consumption ────────────────────────────────────────────────────
    float spd2  = vel.len();     // actual achieved speed this frame
    float cost  = energyCost(spd2, slope, dt);
    energy     -= cost;

    // ── Death conditions ──────────────────────────────────────────────────────
    if (energy <= 0.f                       // starved to death
     || age >= lifespan                     // died of old age
     || needs.isCritical(Drive::Thirst))    // severe dehydration
        alive = false;

    return cost;
}

// ── Reproduction ──────────────────────────────────────────────────────────────
void World::handleReproduction(float dt) {
    // Phase 1: advance gestation timers; spawn offspring when timer expires
    for (auto& c : creatures) {
        if (!c.alive || c.behavior != BehaviorState::Mating) continue;
        c.gestTimer -= dt;
        if (c.gestTimer <= 0.f) {
            auto it = idToIndex.find(c.mateTarget);
            if (it == idToIndex.end()) { c.behavior = BehaviorState::Idle; continue; }
            Creature& mate = creatures[it->second];
            if (!mate.alive) { c.behavior = BehaviorState::Idle; continue; }

            // Crossover + mutate to produce each offspring's genome
            int litter = c.genome.litterSize();
            for (int i = 0; i < litter; i++) {
                Genome child = Genome::crossover(c.genome, mate.genome, globalRNG());
                child.mutate(globalRNG());
                // Scatter offspring around the mother's position
                Vec3 birthPos = c.pos;
                birthPos.x += globalRNG().range(-1.f, 1.f);
                birthPos.z += globalRNG().range(-1.f, 1.f);
                birthPos.y  = heightAt(birthPos.x, birthPos.z);

                if ((int)creatures.size() < cfg.maxPopulation)
                    spawnCreature(child, birthPos, c.id, mate.id,
                                  std::max(c.generation, mate.generation) + 1);
            }
            // Post-birth: reset libido, leave mating state, pay birth energy cost
            c.needs.satisfy(Drive::Libido, 1.f);
            c.behavior  = BehaviorState::Idle;
            c.mateTarget= INVALID_ID;
            c.energy   -= 20.f * c.genome.bodySize();   // giving birth is energetically expensive
        }
    }

    // Phase 2: initiate new mating pairs from willing, adjacent creatures.
    // Conditions: libido > 0.7, a mate is visible and within 1.5 m, same species.
    for (auto& c : creatures) {
        if (!c.alive) continue;
        if (c.behavior == BehaviorState::Mating) continue;
        if (c.needs.level[(int)Drive::Libido] < 0.7f) continue;
        if (c.nearestMate == INVALID_ID) continue;
        if (c.nearestMateDist > 1.5f) continue;

        auto it = idToIndex.find(c.nearestMate);
        if (it == idToIndex.end()) continue;
        Creature& mate = creatures[it->second];
        if (!mate.alive) continue;
        if (mate.behavior == BehaviorState::Mating) continue;

        // Final genetic gate: genomes must be within the species epsilon to reproduce
        if (!sameSpecies(c.genome, mate.genome, cfg.speciesEpsilon)) continue;

        // Begin gestation; only the mother (c) tracks the timer
        c.behavior   = BehaviorState::Mating;
        c.mateTarget = mate.id;
        c.gestTimer  = c.genome.gestationTime();
    }
}

// ── Main tick ─────────────────────────────────────────────────────────────────
void World::tick(float dt) {
    if (cfg.paused) return;
    dt *= cfg.simSpeed;   // apply time-scale multiplier

    simTime += dt;

    growPlants(dt);
    rebuildSpatialHash();  // must happen before perceive() queries

    // Two-pass update: perceive first (read-only world scan), then act (writes).
    // Separating the passes ensures a creature can't react to changes made by
    // another creature in the same tick (fair simultaneous update semantics).
    for (auto& c : creatures)
        if (c.alive) perceive(c);

    for (auto& c : creatures)
        if (c.alive) c.tick(dt, *this);

    handleReproduction(dt);
    removeDeadCreatures();

    // Update species centroids periodically (not every tick for performance)
    static float spTimer = 0.f;
    spTimer += dt;
    if (spTimer > 5.f) {
        updateSpeciesCentroids();
        spTimer = 0.f;
    }
}

// ── Genetic distance helper ───────────────────────────────────────────────────
// Used by handleReproduction to enforce the biological species concept:
// two creatures can only interbreed if their genomes are within `epsilon` distance.
bool sameSpecies(const Genome& a, const Genome& b, float epsilon) {
    return a.distanceTo(b) < epsilon;
}

// ── Serialisation stubs ───────────────────────────────────────────────────────
bool World::saveToFile(const char* path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    // TODO: implement a full save format (e.g. JSON or custom binary)
    // Minimal stub: write simTime + creature count as a proof-of-concept
    f.write(reinterpret_cast<const char*>(&simTime), sizeof(simTime));
    uint32_t cnt = (uint32_t)creatures.size();
    f.write(reinterpret_cast<const char*>(&cnt), sizeof(cnt));
    return true;
}

bool World::loadFromFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.read(reinterpret_cast<char*>(&simTime), sizeof(simTime));
    uint32_t cnt;
    f.read(reinterpret_cast<char*>(&cnt), sizeof(cnt));
    return true;
}

void World::exportCSV(const char* path) const {
    std::ofstream f(path);
    if (!f) return;
    f << "id,species,x,y,z,age,energy,speed,herbEff,carnEff\n";
    for (const auto& c : creatures) {
        f << c.id << ',' << c.speciesID << ','
          << c.pos.x << ',' << c.pos.y << ',' << c.pos.z << ','
          << c.age << ',' << c.energy << ','
          << c.genome.maxSpeed() << ','
          << c.genome.herbEfficiency() << ','
          << c.genome.carnEfficiency() << '\n';
    }
}
