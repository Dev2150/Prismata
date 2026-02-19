#pragma once
#include "Genome.h"
#include "Needs.h"
#include "Math.h"
#include <cstdint>
#include <vector>

struct World;

using EntityID = uint32_t;
constexpr EntityID INVALID_ID = 0;  // Sentinel: "no entity" / "not set"

// ── Behaviour state machine ───────────────────────────────────────────────────
// Each creature is always in exactly one of these states.
// The state is driven by the Needs priority resolver (activeDrive) plus
// availability of targets (food, water, mates, etc.).
enum class BehaviorState {
    Idle,        // Wandering randomly; no urgent drive or missing target
    SeekFood,    // Moving toward the nearest visible food source
    SeekWater,   // Moving toward the nearest visible water tile
    Sleeping,    // Stationary; recharging energy and satisfying Sleep drive
    SeekMate,    // Approaching the nearest compatible creature
    Fleeing,     // Running away from the nearest predator
    Hunting,     // Chasing and biting a prey creature
    Mating,      // Gestation in progress; waiting for gestTimer to reach zero
};

// Convenience distance function between two 3D points (XYZ Euclidean)
inline float dist(const Vec3& a, const Vec3& b) { return (a - b).len(); }

// ── Creature ──────────────────────────────────────────────────────────────────
struct Creature {
    // ── Identity ──────────────────────────────────────────────────────────────
    EntityID  id         = INVALID_ID;  // Unique ID assigned at spawn; never reused
    EntityID  parentA    = INVALID_ID;  // First parent's ID (INVALID_ID = initial generation)
    EntityID  parentB    = INVALID_ID;  // Second parent's ID
    uint32_t  generation = 0;           // Depth from the seed population (0 = first gen)
    uint32_t  speciesID  = 0;           // Index into World::species; assigned by classifySpecies()

    // ── Spatial state ─────────────────────────────────────────────────────────
    Vec3  pos     {};       // World-space position (Y snapped to terrain surface)
    Vec3  vel     {};       // Current velocity (XZ only; Y is always terrain-driven)
    float yaw     = 0.f;   // Heading in radians, measured in the XZ plane (atan2 of vel)

    // ── Biological state ──────────────────────────────────────────────────────
    Genome  genome;
    Needs   needs;
    float   energy      = 100.f;  // Current energy; drops to 0 → death
    float   maxEnergy   = 150.f;  // Cap; scales with body size so large creatures store more
    float   age         = 0.f;    // Seconds since spawn
    float   lifespan    = 180.f;  // Seconds until old-age death; randomised at birth
    float   mass        = 1.f;    // Derived from bodySize gene; scales energy costs
    bool    alive       = true;   // Set to false to mark for removal next tick

    // ── Reproduction ─────────────────────────────────────────────────────────
    BehaviorState  behavior    = BehaviorState::Idle;
    float          gestTimer   = 0.f;           // Countdown (seconds) until offspring are born
    EntityID       mateTarget  = INVALID_ID;    // ID of the partner during gestation

    // ── Perception cache ──────────────────────────────────────────────────────
    // Updated once per tick by World::perceive(). Storing results here avoids
    // repeated spatial queries inside the behaviour state machine.
    EntityID nearestPredator = INVALID_ID;
    float    nearestPredDist = 1e9f;    // Distance to nearest predator (1e9 = "none seen")
    EntityID nearestPrey     = INVALID_ID;
    float    nearestPreyDist = 1e9f;
    EntityID nearestMate     = INVALID_ID;
    float    nearestMateDist = 1e9f;
    Vec3     nearestFood     {};        // Position of nearest alive plant
    float    nearestFoodDist = 1e9f;
    Vec3     nearestWater    {};        // Position of nearest water tile
    float    nearestWaterDist= 1e9f;

    // ── Lifecycle ─────────────────────────────────────────────────────────────
    // Called once after the genome is set to derive all genome-dependent stats.
    void initFromGenome(const Vec3& spawnPos) {
        pos      = spawnPos;
        mass     = genome.bodySize();
        maxEnergy= 80.f + mass * 40.f;       // larger body → bigger energy tank
        energy   = maxEnergy * 0.7f;          // start at 70% so newborns still need food
        lifespan = 120.f + globalRNG().normal(0.f, 20.f);  // add randomness to lifespan
        needs.initFromGenome(genome);
    }

    // Main per-frame update: advances needs, runs the behaviour FSM, moves the
    // creature, consumes energy, and checks death conditions. Returns energy spent.
    float tick(float dt, World& world);

    // ── Physics / steering ────────────────────────────────────────────────────

    // Steers toward a world-space target by blending current velocity toward
    // the desired velocity. Uses a first-order lag (exponential smoothing)
    // with a time constant of 1/8 s, so turns feel natural rather than instant.
    // Slows down ("proportional braking") when close to the target.
    void steerToward(const Vec3& target, float maxSpd, float dt) {
        Vec3 dir = (target - pos);
        dir.y = 0;                          // ignore height difference for steering
        float d = dir.len();
        if (d < 0.1f) return;              // already at target
        dir = dir * (1.f / d);             // normalise
        float spd = std::min(maxSpd, d * 5.f);  // proportional slow-down near goal
        Vec3 desired = dir * spd;
        // Blend current velocity toward desired: faster dt or larger factor = snappier
        vel.x += (desired.x - vel.x) * std::min(1.f, dt * 8.f);
        vel.z += (desired.z - vel.z) * std::min(1.f, dt * 8.f);
        yaw    = std::atan2(vel.x, vel.z);  // update heading to match movement direction
    }

    // Steers directly away from a threat. Uses a stronger time constant (10×)
    // than steerToward so fleeing creatures react faster than pursuing ones.
    void steerAway(const Vec3& threat, float maxSpd, float dt) {
        Vec3 dir = pos - threat;
        dir.y = 0;
        float d = dir.len();
        if (d < 0.1f) dir = Vec3{1,0,0};  // degenerate case: pick arbitrary direction
        else dir = dir * (1.f / d);
        Vec3 desired = dir * maxSpd;
        vel.x += (desired.x - vel.x) * std::min(1.f, dt * 10.f);
        vel.z += (desired.z - vel.z) * std::min(1.f, dt * 10.f);
        yaw    = std::atan2(vel.x, vel.z);
    }

    // ── Energy model ──────────────────────────────────────────────────────────
    // Three-term energy cost per frame:
    //   Basal:  resting metabolic rate (always paid, scales with mass)
    //   Move:   quadratic in speed so fast movement is disproportionately costly
    //           (real muscle energetics are ≈ cubic, but quadratic is close enough)
    //   Climb:  extra cost proportional to the terrain slope being traversed
    float energyCost(float speed, float slopeSin, float dt) const {
        const float kBasal = 0.008f;   // energy/kg/s at rest
        const float kMove  = 0.04f;    // energy/kg/(m/s)² – quadratic locomotion cost
        const float kClimb = 0.025f;   // energy/kg per unit sin(slope) – hill-climbing penalty
        return (kBasal * mass
              + kMove  * speed * speed * mass
              + kClimb * slopeSin * mass) * dt;
    }

    // ── Helpers ───────────────────────────────────────────────────────────────
    bool  isHerbivore() const { return genome.isHerbivore(); }
    bool  isCarnivore() const { return genome.isCarnivore(); }

    // Effective top speed, throttled by energy level.
    // An energy-depleted creature can still move (min 10% speed) but can't outrun
    // a healthy predator, creating meaningful survival pressure around starvation.
    float speedCap()    const {
        float eFrac = energy / maxEnergy;   // 0 (empty) → 1 (full)
        return genome.maxSpeed() * std::max(0.1f, eFrac);
    }
};

// ── Lineage helper ────────────────────────────────────────────────────────────
// Kept separate from Creature to avoid bloating the hot struct with a vector.
// Allows genealogy queries (who are a creature's descendants?) without paying
// the memory cost per creature during normal simulation.
struct Lineage {
    EntityID        id;
    EntityID        parentA, parentB;
    uint32_t        generation;
    std::vector<EntityID> children;  // IDs of all offspring ever born
};
