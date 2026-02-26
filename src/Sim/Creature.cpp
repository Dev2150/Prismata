#include "Creature.hpp"

#include "tracy/Tracy.hpp"
#include "World/World.hpp"
#include "World/World_Planet.hpp"

void Creature::steerToward(const Vec3& target, float maxSpd, float dt) {
    Vec3 dir = g_planet_surface.projectToTangent(pos, target - pos);
    float d = dir.len();
    if (d < 0.1f) return;
    dir = dir * (1.f / d);
    float spd = std::min(maxSpd, d * 5.f);
    Vec3 desired = dir * spd;
    vel.x += (desired.x - vel.x) * std::min(1.f, dt * 8.f);
    vel.y += (desired.y - vel.y) * std::min(1.f, dt * 8.f);
    vel.z += (desired.z - vel.z) * std::min(1.f, dt * 8.f);
}

void Creature::steerAway(const Vec3& threat, float maxSpd, float dt) {
    Vec3 dir = g_planet_surface.projectToTangent(pos, pos - threat);
    float d = dir.len();
    if (d < 0.1f) {
        Vec3 east, north;
        g_planet_surface.localBasis(pos, east, north);
        dir = east;
    } else {
        dir = dir * (1.f / d);
    }
    Vec3 desired = dir * maxSpd;
    vel.x += (desired.x - vel.x) * std::min(1.f, dt * 10.f);
    vel.y += (desired.y - vel.y) * std::min(1.f, dt * 10.f);
    vel.z += (desired.z - vel.z) * std::min(1.f, dt * 10.f);
}

void Creature::wander(float spd, float dt) {
    behavior = BehaviorState::Idle;
    Vec3 east, north;
    g_planet_surface.localBasis(pos, east, north);

    float angle = std::sin(age * 0.5f + id) * 3.14159f + std::sin(age * 0.2f + id * 2.0f) * 3.14159f;
    Vec3 w = east * std::cos(angle) + north * std::sin(angle);
    steerToward(pos + w * 500.f, spd * 0.3f, dt);
}

float Creature::tick(float dt, World& world) {
    ZoneScoped;
    if (!alive) return 0.f;

    age += dt;
    needs.tick(dt);  // accumulate all drives by their crave rates

    // Old-age penalty: energy drains faster after 80% of lifespan
    float ageFrac = age / lifespan;
    if (ageFrac > 0.8f)
        energy -= 0.02f * mass * dt;

    // Connect hunger directly to the lack of energy
    needs.urgency[(int)Drive::Hunger] = 1.0f - std::clamp(energy / maxEnergy, 0.0f, 1.0f);

    Drive active = needs.activeDrive();  // which drive governs behaviour this frame
    float spd    = speedCap();           // energy-throttled top speed

    slopeTimer -= dt;
    if (slopeTimer <= 0.f) {
        cachedSlope = world.slopeAt3D(pos);
        slopeTimer = 0.5f + globalRNG().range(0.0f, 0.2f); // stagger updates
    }
    float slope = cachedSlope;

    // ── Behaviour state machine ───────────────────────────────────────────────
    // Each case sets `behavior`, then either steers the creature or modifies
    // its state (e.g. sleeping, eating). Multiple drives can share a case via
    // fall-through to idle when a target is unavailable.
    switch (active) {

        // HEALTH: rest to recover health
        case Drive::Health:
            behavior = BehaviorState::Healing;
            vel = {0, 0, 0};
            if (needs.urgency[(int)Drive::Hunger] < 0.8f && needs.urgency[(int)Drive::Thirst] < 0.8f) {
                needs.satisfy(Drive::Health, 0.01f * dt);
            }
            break;

            // FLEE: highest-priority survival response. Overrides all other drives.
        case Drive::Fear:
            if (nearestPredator != INVALID_ID && world.idToIndex.count(nearestPredator)) {
                behavior = BehaviorState::Fleeing;
                if (world.idToIndex.count(nearestPredator)) {
                    const Creature& pred = world.creatures[world.idToIndex.at(nearestPredator)];
                    steerAway(pred.pos, spd, dt);
                } else {
                    wander(spd, dt);
                }
                break;

                // HUNGER: seek food (plants for herbivores, prey for carnivores)
                case Drive::Hunger:
                if (genome.carnEfficiency() + 0.1f > genome.herbEfficiency() && nearestPrey != INVALID_ID) {
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
                } else if (genome.carnEfficiency() < genome.herbEfficiency() + 0.1f && nearestFoodDist < genome.visionRange()) {
                    behavior = BehaviorState::SeekFood;
                    steerToward(nearestFood, spd, dt);
                    if (nearestFoodDist < 120.f) {
                        // Graze: consume up to 15*herbEff nutrition per second from the nearest plant
                        if (nearestFoodIdx != -1 && nearestFoodIdx < (int)world.plants.size()) {
                            Plant& p = world.plants[nearestFoodIdx];
                            if (p.alive && (pos - p.pos).len2() < 1.44f) { // 1.2 * 1.2 = 1.44
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
                    wander(spd, dt);
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
            if (age >= lifespan) { alive = false; std::string msg = "Death: Aging"; TracyMessageC(msg.c_str(), msg.size(), 0x004400); }
            else if (needs.isCritical(Drive::Health)) {
                alive = false;
                std::string msg;
                if (needs.isCritical(Drive::Hunger))  msg = "Death: Lack of food";
                else if(needs.isCritical(Drive::Thirst))  msg = "Death: Lack of water";
                else msg = "Death: Lack of health";
                TracyMessageC(msg.c_str(), msg.size(), 0x440000);
            }

            return cost;
       }
}