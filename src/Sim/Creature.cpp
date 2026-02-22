#include "Creature.hpp"

#include "tracy/Tracy.hpp"
#include "World/World.hpp"
#include "World/World_Planet.hpp"

float Creature::tick(float dt, World& world) {
    ZoneScoped;
    if (!alive) return 0.f;

    age += dt;
    needs.tick(dt);  // accumulate all drives by their crave rates

    // Old-age penalty: energy drains faster after 80% of lifespan
    float ageFrac = age / lifespan;
    if (ageFrac > 0.8f)
        energy -= 0.02f * mass * dt;

    Drive active = needs.activeDrive();  // which drive governs behaviour this frame
    float spd    = speedCap();           // energy-throttled top speed
    // Use 3-D slope from planet surface
    float slope = world.slopeAt3D(pos);

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
            if (nearestPreyDist < 120.f) {
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
            if (nearestFoodDist < 120.f) {
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
            // Random wander on the planet: generate a random tangent-plane direction
            Vec3 n = world.normalAt(pos);
            Vec3 arb = (std::abs(n.y) < 0.9f) ? Vec3{0.f, 1.f, 0.f} : Vec3{1.f, 0.f, 0.f};
            Vec3 t1 = Vec3{n.y*arb.z - n.z*arb.y,
                           n.z*arb.x - n.x*arb.z,
                           n.x*arb.y - n.y*arb.x}.normalised();
            Vec3 t2 = Vec3{n.y*t1.z - n.z*t1.y,
                           n.z*t1.x - n.x*t1.z,
                           n.x*t1.y - n.y*t1.x};
            float rx = globalRNG().normal(0,1);
            float rz = globalRNG().normal(0,1);
            Vec3 wander = t1 * rx + t2 * rz;
            steerToward(pos + wander * 500.f, spd * 0.3f, dt);
        }
        break;

    // THIRST: navigate to water and drink on arrival
    case Drive::Thirst:
        behavior = BehaviorState::SeekWater;
        if (nearestWaterDist < genome.visionRange()) {
            steerToward(nearestWater, spd, dt);
            if (nearestWaterDist < 150.f) {
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

    // ── Planet-surface movement ───────────────────────────────────────────────
    if (vel.len2() > 0.001f) {
        // Only traverse uphill if slope is within the genome's limit
        bool canMove = (slope * (180.f / 3.14159f) < genome.maxSlope());

        if (canMove) {
            // Project velocity onto the tangent plane at current position so the
            // creature slides along the sphere rather than drifting through it.
            Vec3 tangentVel = g_planet_surface.projectToTangent(pos, vel);

            // Integrate position
            pos.x += tangentVel.x * dt;
            pos.y += tangentVel.y * dt;
            pos.z += tangentVel.z * dt;
        }

        // Always snap back to the displaced sphere surface (corrects floating/sinking).
        pos = g_planet_surface.snapToSurface(pos);

        // Update yaw: project the velocity onto the local tangent plane and
        // compute the heading as an angle relative to an arbitrary "north" direction.
        // We use the XZ component as an approximation (works well near the equator
        // and top of the sphere where most creatures live).
        float vxz = std::sqrt(vel.x*vel.x + vel.z*vel.z);
        if (vxz > 0.01f)
            yaw = std::atan2(vel.x, vel.z);
    }

    // ── Energy consumption ────────────────────────────────────────────────────
    float spd2 = vel.len();
    float cost  = energyCost(spd2, slope, dt);
    energy     -= cost;

    // ── Death ─────────────────────────────────────────────────────────────────
    if (energy <= 0.f || age >= lifespan || needs.isCritical(Drive::Thirst))
        alive = false;

    return cost;
}