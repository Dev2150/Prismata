#include "World.hpp"
#include "World_Planet.hpp"
#include "tracy/Tracy.hpp"

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
void World::perceive(Creature& c, float dt) {
    ZoneScoped;
    float range  = c.genome.visionRange();
    // Half-angle of the FOV cone in radians; creatures behind are invisible
    float fovRad = c.genome.visionFOV() * 3.14159f / 180.f;

    // Reset all perception caches to "nothing found" sentinel values
    c.nearestPredator  = INVALID_ID; c.nearestPredDist = 1e9f;
    c.nearestPrey      = INVALID_ID; c.nearestPreyDist = 1e9f;
    c.nearestMate      = INVALID_ID; c.nearestMateDist = 1e9f;
    c.nearestConspecific = INVALID_ID; c.nearestConspecificDist = 1e9f;
    c.nearestFoodDist  = 1e9f;
    c.nearestFoodIdx   = -1;

    // Build the creature's local facing vector.
    // yaw is measured relative to the planet's XZ plane (atan2 of velocity).
    // On the sphere top hemisphere this is a good enough approximation.
    Vec3 facing = {std::sin(c.yaw), 0.f, std::cos(c.yaw)};
    // Project onto the tangent plane at this creature's position and renormalise.
    facing = g_planet_surface.projectToTangent(c.pos, facing).normalised();

    {
        ZoneScopedN("perceive_creatures");
        static thread_local std::vector<uint32_t> nearby; // Reused capacity
        queryRadius(c.pos, range, nearby);

        float nearestPredDist2 = 1e18f;
        float nearestPreyDist2 = 1e18f;
        float nearestMateDist2 = 1e18f;
        float nearestConspecificDist2 = 1e18f;

        float cosHalfFov = std::cos(fovRad * 0.5f);
        float cosHalfFov2 = cosHalfFov * cosHalfFov;

        for (uint32_t oIdx : nearby) {
            const Creature& o = creatures[oIdx];
            if (o.id == c.id) continue;   // skip self
            if (!o.alive) continue;

            Vec3 toO = o.pos - c.pos;
            float d2 = toO.len2();

            // FOV cone check: project the direction to the other creature onto the
            // facing vector. If the cosine is below cos(halfFOV), the target is outside
            // the cone and is treated as invisible.
            if (d2 > 0.01f) {
                float dotA = toO.dot(facing);
                if (cosHalfFov >= 0.f) {
                    if (dotA < 0.f || dotA * dotA < d2 * cosHalfFov2) continue;
                } else {
                    if (dotA < 0.f && dotA * dotA > d2 * cosHalfFov2) continue;
                }
            }

            bool oIsPredator = o.genome.carnEfficiency() > 0.5f && o.genome.bodySize() > c.genome.bodySize() * 1.1f;
            bool oIsPrey     = c.genome.carnEfficiency() > 0.5f && c.genome.bodySize() > o.genome.bodySize() * 1.1f;
            bool oIsMate     = (o.speciesID == c.speciesID) && (o.needs.urgency[(int)Drive::Libido] > 0.5f);
            bool oIsConspecific = (o.speciesID == c.speciesID);

            if (oIsPredator && d2 < nearestPredDist2) {
                nearestPredDist2 = d2; c.nearestPredator = o.id;
            }
            if (oIsPrey && d2 < nearestPreyDist2) {
                nearestPreyDist2 = d2; c.nearestPrey = o.id;
            }
            if (oIsMate && d2 < nearestMateDist2) {
                nearestMateDist2 = d2; c.nearestMate = o.id;
            }
            if (oIsConspecific && d2 < nearestConspecificDist2) {
                nearestConspecificDist2 = d2; c.nearestConspecific = o.id;
            }
        }
        if (c.nearestPredator != INVALID_ID) c.nearestPredDist = std::sqrt(nearestPredDist2);
        if (c.nearestPrey != INVALID_ID) c.nearestPreyDist = std::sqrt(nearestPreyDist2);
        if (c.nearestMate != INVALID_ID) c.nearestMateDist = std::sqrt(nearestMateDist2);
        if (c.nearestConspecific != INVALID_ID) c.nearestConspecificDist = std::sqrt(nearestConspecificDist2);
    }

    {
        ZoneScopedN("perceive_plants");
        float bestDist2 = range * range;
        bool found = false;

        int r = (int)std::ceil(range / plantHash.cellSize);
        int cx0 = (int)std::floor(c.pos.x / plantHash.cellSize) + PlantSpatialHash::GRID_OFFSET;
        int cz0 = (int)std::floor(c.pos.z / plantHash.cellSize) + PlantSpatialHash::GRID_OFFSET;

        for (int dz = -r; dz <= r; dz++) {
            int cz = cz0 + dz;
            if (cz < 0 || cz >= PlantSpatialHash::GRID_SIZE) continue;
            for (int dx = -r; dx <= r; dx++) {
                int cx = cx0 + dx;
                if (cx < 0 || cx >= PlantSpatialHash::GRID_SIZE) continue;

                int cellIdx = cz * PlantSpatialHash::GRID_SIZE + cx;
                int idx = plantHash.head[cellIdx];
                while (idx != -1) {
                    uint32_t pIdx = plantHash.plantIndices[idx];
                    idx = plantHash.next[idx];

                    const Plant& p = plants[pIdx];
                    float d2 = (c.pos - p.pos).len2();
                    if (d2 < bestDist2) {
                        bestDist2 = d2;
                        c.nearestFood     = p.pos;
                        c.nearestFoodIdx = pIdx;
                        found = true;
                    }
                }
            }
        }

        if (found) {
            c.nearestFoodDist = std::sqrt(bestDist2);
        }
    }

    {
        ZoneScopedN("perceive_water");
        // Water search: only run if the cache timer has expired
        c.waterCacheTimer -= dt;

        if (c.waterCacheTimer <= 0.f) {
            c.waterCacheTimer = 2.0f + globalRNG().range(0.0f, 1.0f); // Stagger
            Vec3 waterPos;
            if (g_planet_surface.findOcean(c.pos, range, waterPos)) {
                c.nearestWater = waterPos;
                c.nearestWaterDist = (waterPos - c.pos).len();
            } else {
                c.nearestWaterDist = 1e9f;
                }
        } else if (c.nearestWaterDist < 1e9f) {
            // We already have a cached water position, just update the distance to it
            c.nearestWaterDist = (c.nearestWater - c.pos).len();
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