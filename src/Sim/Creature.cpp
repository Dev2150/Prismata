#include "Creature.hpp"
#include "World/World.hpp"

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