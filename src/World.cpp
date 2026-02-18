#include "World.h"
#include <cmath>
#include <algorithm>
#include <cassert>
#include <fstream>
#include <sstream>
#include <numeric>

// ── Perlin noise (single-header implementation) ───────────────────────────────
namespace {

// Quintic fade + permutation table for Perlin noise
static int P[512];
static bool perlinInit = false;

void initPerlin(uint64_t seed) {
    RNG rng(seed);
    std::vector<int> tmp(256);
    std::iota(tmp.begin(), tmp.end(), 0);
    for (int i = 255; i > 0; i--) {
        int j = static_cast<int>(rng.uniform() * (i + 1));
        std::swap(tmp[i], tmp[j]);
    }
    for (int i = 0; i < 512; i++) P[i] = tmp[i & 255];
    perlinInit = true;
}

float fade(float t)  { return t * t * t * (t * (t * 6 - 15) + 10); }
float lerp(float t, float a, float b) { return a + t * (b - a); }

float grad(int hash, float x, float y, float z) {
    int h = hash & 15;
    float u = h < 8 ? x : y;
    float v = h < 4 ? y : (h == 12 || h == 14 ? x : z);
    return ((h & 1) ? -u : u) + ((h & 2) ? -v : v);
}

float perlin(float x, float y, float z = 0) {
    int   X = (int)std::floor(x) & 255, Y = (int)std::floor(y) & 255, Z = (int)std::floor(z) & 255;
    x -= std::floor(x); y -= std::floor(y); z -= std::floor(z);
    float u = fade(x), v = fade(y), w = fade(z);
    int A = P[X]+Y, AA = P[A]+Z, AB = P[A+1]+Z, B = P[X+1]+Y, BA = P[B]+Z, BB = P[B+1]+Z;
    return lerp(w,
        lerp(v,
            lerp(u, grad(P[AA],x,y,z),   grad(P[BA],x-1,y,z)),
            lerp(u, grad(P[AB],x,y-1,z), grad(P[BB],x-1,y-1,z))),
        lerp(v,
            lerp(u, grad(P[AA+1],x,y,z-1),   grad(P[BA+1],x-1,y,z-1)),
            lerp(u, grad(P[AB+1],x,y-1,z-1), grad(P[BB+1],x-1,y-1,z-1))));
}

float octaveNoise(float x, float z, int octaves = 6, float persistence = 0.5f, float lacunarity = 2.f) {
    float val = 0, amp = 1, freq = 1, max = 0;
    for (int i = 0; i < octaves; i++) {
        val += perlin(x * freq, z * freq) * amp;
        max += amp;
        amp  *= persistence;
        freq *= lacunarity;
    }
    return val / max;   // [-1, 1]
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
                    int gx = ix * CHUNK_SIZE + lx;
                    int gz = iz * CHUNK_SIZE + lz;

                    float wx = gx * 0.04f;
                    float wz = gz * 0.04f;

                    float h  = octaveNoise(wx, wz);            // terrain
                    float t  = octaveNoise(wx + 100, wz, 4);   // temperature
                    float hm = octaveNoise(wx + 200, wz, 4);   // humidity

                    float height = 8.f + h * 12.f;

                    auto& col = chunk.cells[lz][lx];
                    col.height = std::max(0.f, height);

                    if (height < 1.f) {
                        col.material = 3; // water
                    } else if (h > 0.5f) {
                        col.material = (t > 0.2f) ? 4 : 1; // snow or rock
                    } else if (hm > 0.3f && t < 0.4f) {
                        col.material = 0; // grass
                    } else {
                        col.material = 2; // sand/dirt
                    }
                }
            }
        }
    }

    // Spawn initial plants
    RNG rng(seed + 1);
    int wGX = worldCX * CHUNK_SIZE;
    int wGZ = worldCZ * CHUNK_SIZE;
    for (int i = 0; i < wGX * wGZ / 8; i++) {
        float px = rng.range(0, (float)wGX);
        float pz = rng.range(0, (float)wGZ);
        if (!isWater(px, pz))
            spawnPlant(snapToSurface(px, pz));
    }

    // Spawn initial creatures
    auto spawnN = [&](int n, bool herb) {
        for (int i = 0; i < n; i++) {
            float cx2 = rng.range(2.f, (float)wGX - 2.f);
            float cz2 = rng.range(2.f, (float)wGZ - 2.f);
            Vec3 sp   = snapToSurface(cx2, cz2);
            Genome g  = herb ? Genome::randomHerbivore(rng) : Genome::randomCarnivore(rng);
            spawnCreature(g, sp);
        }
    };
    spawnN(80, true);
    spawnN(20, false);
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

// Bilinear interpolation of height
float World::heightAt(float x, float z) const {
    int x0 = (int)std::floor(x), z0 = (int)std::floor(z);
    float fx = x - x0, fz = z - z0;

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

    return (h00 * (1-fx) + h10 * fx) * (1-fz)
         + (h01 * (1-fx) + h11 * fx) *    fz;
}

float World::slopeAt(float x, float z) const {
    // Finite difference gradient
    const float d = 0.5f;
    float dhdx = (heightAt(x+d, z) - heightAt(x-d, z)) / (2*d);
    float dhdz = (heightAt(x, z+d) - heightAt(x, z-d)) / (2*d);
    float gradMag = std::sqrt(dhdx*dhdx + dhdz*dhdz);
    return std::sin(std::atan(gradMag));   // sin of angle from horizontal
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

bool World::findWater(const Vec3& from, float radius, Vec3& outPos) const {
    // Grid scan over water tiles within radius
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
    c.speciesID  = classifySpecies(g);
    c.initFromGenome(pos);

    idToIndex[c.id] = creatures.size() - 1;
    return c;
}

Plant& World::spawnPlant(const Vec3& pos, uint8_t type) {
    plants.emplace_back();
    Plant& p  = plants.back();
    p.pos      = pos;
    p.type     = type;
    p.nutrition= 20.f + type * 10.f;
    return p;
}

void World::removeDeadCreatures() {
    creatures.erase(
        std::remove_if(creatures.begin(), creatures.end(),
                       [](const Creature& c){ return !c.alive; }),
        creatures.end());
    // Rebuild index
    idToIndex.clear();
    for (size_t i = 0; i < creatures.size(); i++)
        idToIndex[creatures[i].id] = i;
}

// ── Spatial hash ─────────────────────────────────────────────────────────────
void World::rebuildSpatialHash() {
    spatialHash.cells.clear();
    for (const auto& c : creatures) {
        if (!c.alive) continue;
        int cx = (int)(c.pos.x / spatialHash.cellSize);
        int cz = (int)(c.pos.z / spatialHash.cellSize);
        spatialHash.cells[spatialHash.key(cx, cz)].push_back(c.id);
    }
}

std::vector<EntityID> World::queryRadius(const Vec3& center, float radius) const {
    std::vector<EntityID> result;
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
uint32_t World::classifySpecies(const Genome& g) {
    float bestDist = 1e9f;
    uint32_t bestID = 0;

    for (auto& sp : species) {
        if (sp.count == 0) continue;
        float d = g.distanceTo(sp.centroid);
        if (d < bestDist) { bestDist = d; bestID = sp.id; }
    }

    if (bestDist > cfg.speciesEpsilon || species.empty()) {
        // New species
        SpeciesInfo sp;
        sp.id      = nextSpeciesID++;
        sp.centroid= g;
        sp.count   = 1;
        sp.allTime = 1;
        // Generate a colour from the genome hue
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

        // Generate a name from id
        const char* parts[] = {"Azel","Brix","Calu","Dorn","Evon","Fyx","Gorn","Hexa"};
        sp.name = std::string(parts[sp.id % 8]) + std::to_string(sp.id);

        species.push_back(sp);
        return sp.id;
    }

    auto it = std::find_if(species.begin(), species.end(),
                           [&](const SpeciesInfo& s){ return s.id == bestID; });
    if (it != species.end()) { it->count++; it->allTime++; }
    return bestID;
}

void World::updateSpeciesCentroids() {
    // Zero counts
    for (auto& sp : species) {
        sp.count = 0;
        sp.centroid = Genome{};
    }
    // Accumulate
    for (const auto& c : creatures) {
        if (!c.alive) continue;
        auto it = std::find_if(species.begin(), species.end(),
                               [&](const SpeciesInfo& s){ return s.id == c.speciesID; });
        if (it == species.end()) continue;
        it->count++;
        for (int i = 0; i < GENOME_SIZE; i++)
            it->centroid.raw[i] += c.genome.raw[i];
    }
    // Average
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
void World::perceive(Creature& c) {
    float range  = c.genome.visionRange();
    float fovRad = c.genome.visionFOV() * 3.14159f / 180.f;

    // Reset cache
    c.nearestPredator  = INVALID_ID; c.nearestPredDist = 1e9f;
    c.nearestPrey      = INVALID_ID; c.nearestPreyDist = 1e9f;
    c.nearestMate      = INVALID_ID; c.nearestMateDist = 1e9f;
    c.nearestFoodDist  = 1e9f;
    c.nearestWaterDist = 1e9f;

    auto nearby = queryRadius(c.pos, range);
    for (EntityID oid : nearby) {
        if (oid == c.id) continue;
        auto it = idToIndex.find(oid);
        if (it == idToIndex.end()) continue;
        const Creature& o = creatures[it->second];
        if (!o.alive) continue;

        // FOV check (simple dot product)
        Vec3 toO = o.pos - c.pos; toO.y = 0;
        Vec3 facing = {std::sin(c.yaw), 0, std::cos(c.yaw)};
        float d = toO.len();
        if (d > 0.1f) {
            float cosA = toO.normalised().dot(facing);
            if (cosA < std::cos(fovRad * 0.5f)) continue;
        }

        // Classify relationship
        bool oIsPredator = o.isCarnivore() && c.isHerbivore();
        bool oIsPrey     = c.isCarnivore() && o.isHerbivore();
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

    // Plants
    for (const auto& p : plants) {
        if (!p.alive) continue;
        float d = dist(c.pos, p.pos);
        if (d < range && d < c.nearestFoodDist) {
            c.nearestFoodDist = d;
            c.nearestFood     = p.pos;
        }
    }

    // Water
    if (c.nearestWaterDist > range) {
        Vec3 wp;
        if (findWater(c.pos, range, wp)) {
            c.nearestWaterDist = dist(c.pos, wp);
            c.nearestWater     = wp;
        }
    }

    // Fear update
    if (c.nearestPredator != INVALID_ID) {
        float distNorm = c.nearestPredDist / range;
        c.needs.raiseFear(distNorm, c.genome.fearSensitivity(), 1.f/60.f);
    } else {
        c.needs.decayFear(1.f/60.f);
    }
}

// ── Plant growth ──────────────────────────────────────────────────────────────
void World::growPlants(float dt) {
    // Regrow eaten plants
    for (auto& p : plants) {
        if (p.alive) continue;
        p.growTimer += dt;
        if (p.growTimer > 30.f) {
            p.alive     = true;
            p.nutrition = 20.f + p.type * 10.f;
            p.growTimer = 0.f;
        }
    }

    // Spontaneous new plants (if below cap)
    int alive = 0;
    for (const auto& p : plants) alive += p.alive ? 1 : 0;
    int cap = worldCX * worldCZ * CHUNK_SIZE / 4;
    if (alive < cap) {
        int toSpawn = (int)(cfg.plantGrowRate * dt) + (globalRNG().chance(cfg.plantGrowRate * dt - (int)(cfg.plantGrowRate * dt)) ? 1 : 0);
        RNG& rng = globalRNG();
        for (int i = 0; i < toSpawn; i++) {
            float px = rng.range(0.f, (float)(worldCX * CHUNK_SIZE));
            float pz = rng.range(0.f, (float)(worldCZ * CHUNK_SIZE));
            if (!isWater(px, pz))
                spawnPlant(snapToSurface(px, pz));
        }
    }

    // Remove dead plants that have been gone a long time (keep vector trim)
    plants.erase(
        std::remove_if(plants.begin(), plants.end(),
                       [](const Plant& p){ return !p.alive && p.growTimer > 60.f; }),
        plants.end());
}

// ── Creature tick ─────────────────────────────────────────────────────────────
float Creature::tick(float dt, World& world) {
    if (!alive) return 0.f;

    age += dt;
    needs.tick(dt);

    // Age-related energy reduction
    float ageFrac = age / lifespan;
    if (ageFrac > 0.8f)
        energy -= 0.02f * mass * dt;   // old age penalty

    Drive active = needs.activeDrive();
    float spd    = speedCap();
    float slope  = world.slopeAt(pos.x, pos.z);

    // ── Behaviour state machine ──────────────────────────────────────────────
    switch (active) {

    case Drive::Fear:
        behavior = BehaviorState::Fleeing;
        if (world.idToIndex.count(nearestPredator)) {
            const Creature& pred = world.creatures[world.idToIndex.at(nearestPredator)];
            steerAway(pred.pos, spd, dt);
        }
        break;

    case Drive::Hunger:
        if (isCarnivore() && nearestPrey != INVALID_ID) {
            behavior = BehaviorState::Hunting;
            const Creature& prey = world.creatures[world.idToIndex.at(nearestPrey)];
            steerToward(prey.pos, spd, dt);
            // Attack if adjacent
            if (nearestPreyDist < 1.2f) {
                Creature& prey2 = world.creatures[world.idToIndex.at(nearestPrey)];
                float bite = 20.f * genome.carnEfficiency() * dt;
                prey2.energy -= bite;
                energy = std::min(maxEnergy, energy + bite * 0.7f);
                if (prey2.energy <= 0) prey2.alive = false;
                needs.satisfy(Drive::Hunger, bite / 50.f);
            }
        } else if (isHerbivore() && nearestFoodDist < genome.visionRange()) {
            behavior = BehaviorState::SeekFood;
            steerToward(nearestFood, spd, dt);
            // Eat if adjacent
            if (nearestFoodDist < 1.2f) {
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
            // Wander
            behavior = BehaviorState::Idle;
            Vec3 wander = {globalRNG().normal(0,1), 0, globalRNG().normal(0,1)};
            steerToward(pos + wander * 5.f, spd * 0.3f, dt);
        }
        break;

    case Drive::Thirst:
        behavior = BehaviorState::SeekWater;
        if (nearestWaterDist < genome.visionRange()) {
            steerToward(nearestWater, spd, dt);
            if (nearestWaterDist < 1.5f) {
                needs.satisfy(Drive::Thirst, 0.5f * dt);
            }
        }
        break;

    case Drive::Sleep:
        behavior = BehaviorState::Sleeping;
        vel = {0, 0, 0};    // stop moving
        energy = std::min(maxEnergy, energy + 5.f * dt);
        needs.satisfy(Drive::Sleep, 0.3f * dt);
        break;

    case Drive::Libido:
        if (nearestMate != INVALID_ID) {
            behavior = BehaviorState::SeekMate;
            const Creature& mate = world.creatures[world.idToIndex.at(nearestMate)];
            steerToward(mate.pos, spd * 0.6f, dt);
        }
        break;

    default:
        behavior = BehaviorState::Idle;
        break;
    }

    // ── Apply movement to terrain ────────────────────────────────────────────
    // Project velocity onto terrain plane
    if (vel.len2() > 0.001f) {
        // Check slope is climbable
        if (slope * (180.f / 3.14159f) < genome.maxSlope() || vel.dot(pos - world.snapToSurface(pos.x + vel.x, pos.z + vel.z)) < 0) {
            pos.x += vel.x * dt;
            pos.z += vel.z * dt;
        }
        // Snap Y to terrain
        pos.y = world.heightAt(pos.x, pos.z);
    }

    // ── Boundary clamp ───────────────────────────────────────────────────────
    float maxX = (float)(world.worldCX * CHUNK_SIZE - 1);
    float maxZ = (float)(world.worldCZ * CHUNK_SIZE - 1);
    pos.x = std::clamp(pos.x, 0.f, maxX);
    pos.z = std::clamp(pos.z, 0.f, maxZ);

    // ── Energy consumption ───────────────────────────────────────────────────
    float spd2  = vel.len();
    float cost  = energyCost(spd2, slope, dt);
    energy     -= cost;

    // ── Death conditions ─────────────────────────────────────────────────────
    if (energy <= 0.f || age >= lifespan || needs.isCritical(Drive::Thirst))
        alive = false;

    return cost;
}

// ── Reproduction ──────────────────────────────────────────────────────────────
void World::handleReproduction(float dt) {
    // Update gestation
    for (auto& c : creatures) {
        if (!c.alive || c.behavior != BehaviorState::Mating) continue;
        c.gestTimer -= dt;
        if (c.gestTimer <= 0.f) {
            // Birth
            auto it = idToIndex.find(c.mateTarget);
            if (it == idToIndex.end()) { c.behavior = BehaviorState::Idle; continue; }
            Creature& mate = creatures[it->second];
            if (!mate.alive) { c.behavior = BehaviorState::Idle; continue; }

            int litter = c.genome.litterSize();
            for (int i = 0; i < litter; i++) {
                Genome child = Genome::crossover(c.genome, mate.genome, globalRNG());
                child.mutate(globalRNG());
                Vec3 birthPos = c.pos;
                birthPos.x += globalRNG().range(-1.f, 1.f);
                birthPos.z += globalRNG().range(-1.f, 1.f);
                birthPos.y  = heightAt(birthPos.x, birthPos.z);

                if ((int)creatures.size() < cfg.maxPopulation)
                    spawnCreature(child, birthPos, c.id, mate.id,
                                  std::max(c.generation, mate.generation) + 1);
            }
            c.needs.satisfy(Drive::Libido, 1.f);
            c.behavior  = BehaviorState::Idle;
            c.mateTarget= INVALID_ID;
            c.energy   -= 20.f * c.genome.bodySize();   // birth energy cost
        }
    }

    // Initiate mating between adjacent willing pairs
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

        // Species gate
        if (!sameSpecies(c.genome, mate.genome, cfg.speciesEpsilon)) continue;

        c.behavior   = BehaviorState::Mating;
        c.mateTarget = mate.id;
        c.gestTimer  = c.genome.gestationTime();
    }
}

// ── Main tick ─────────────────────────────────────────────────────────────────
void World::tick(float dt) {
    if (cfg.paused) return;
    dt *= cfg.simSpeed;

    simTime += dt;

    growPlants(dt);
    rebuildSpatialHash();

    // Perceive + tick creatures
    for (auto& c : creatures)
        if (c.alive) perceive(c);

    for (auto& c : creatures)
        if (c.alive) c.tick(dt, *this);

    handleReproduction(dt);
    removeDeadCreatures();

    // Update species every 5 seconds
    static float spTimer = 0.f;
    spTimer += dt;
    if (spTimer > 5.f) {
        updateSpeciesCentroids();
        spTimer = 0.f;
    }
}

// Genetic distance helper (used by World::handleReproduction)
bool sameSpecies(const Genome& a, const Genome& b, float epsilon) {
    return a.distanceTo(b) < epsilon;
}

// ── Serialisation stubs ───────────────────────────────────────────────────────
bool World::saveToFile(const char* path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    // TODO: use nlohmann/json or a binary format for full save
    // Minimal stub: write simTime + creature count
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
