#include "World.hpp"
#include "World_Planet.hpp"

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

    // Build the creature's local facing vector.
    // yaw is measured relative to the planet's XZ plane (atan2 of velocity).
    // On the sphere top hemisphere this is a good enough approximation.
    Vec3 facing = {std::sin(c.yaw), 0.f, std::cos(c.yaw)};
    // Project onto the tangent plane at this creature's position and renormalise.
    facing = g_planet_surface.projectToTangent(c.pos, facing).normalised();

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
        Vec3 toO = o.pos - c.pos;
        float d = toO.len();
        if (d > 0.1f) {
            // FOV cone check in 3-D
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
        if (g_planet_surface.findOcean(c.pos, range, wp)) {
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