#include "World.hpp"

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
