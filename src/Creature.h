#pragma once
#include "Genome.h"
#include "Needs.h"
#include <cstdint>
#include <vector>

// ── Forward declarations ──────────────────────────────────────────────────────
struct World;

using EntityID = uint32_t;
constexpr EntityID INVALID_ID = 0;

// ── Creature state machine ────────────────────────────────────────────────────
enum class BehaviorState {
    Idle,
    SeekFood,
    SeekWater,
    Sleeping,
    SeekMate,
    Fleeing,
    Hunting,
    Mating,     // gestation in progress
};

// ── 3D position / velocity (Y = up) ─────────────────────────────────────────
struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3 operator+(const Vec3& o) const { return {x+o.x, y+o.y, z+o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x-o.x, y-o.y, z-o.z}; }
    Vec3 operator*(float s)       const { return {x*s,   y*s,   z*s  }; }
    float dot(const Vec3& o)      const { return x*o.x + y*o.y + z*o.z; }
    float len2()                  const { return x*x + y*y + z*z; }
    float len()                   const { return std::sqrt(len2()); }
    Vec3  normalised()            const { float l=len(); return l>1e-6f?(*this)*(1.f/l):Vec3{}; }
    Vec3& operator+=(const Vec3& o){ x+=o.x; y+=o.y; z+=o.z; return *this; }
};

inline float dist(const Vec3& a, const Vec3& b) { return (a - b).len(); }

// ── Creature ──────────────────────────────────────────────────────────────────
struct Creature {
    // Identity
    EntityID  id         = INVALID_ID;
    EntityID  parentA    = INVALID_ID;
    EntityID  parentB    = INVALID_ID;
    uint32_t  generation = 0;
    uint32_t  speciesID  = 0;

    // Spatial state
    Vec3  pos     {};
    Vec3  vel     {};      // current velocity (world-space, horizontal)
    float yaw     = 0.f;  // heading in radians (XZ plane)

    // Biological state
    Genome  genome;
    Needs   needs;
    float   energy      = 100.f;   // current energy
    float   maxEnergy   = 150.f;   // cap
    float   age         = 0.f;     // seconds
    float   lifespan    = 180.f;   // derived from genome + randomness
    float   mass        = 1.f;     // derived from bodySize
    bool    alive       = true;

    // Reproduction
    BehaviorState  behavior    = BehaviorState::Idle;
    float          gestTimer   = 0.f;   // countdown to birth
    EntityID       mateTarget  = INVALID_ID;

    // Perception cache (updated each tick)
    EntityID nearestPredator = INVALID_ID;
    float    nearestPredDist = 1e9f;
    EntityID nearestPrey     = INVALID_ID;
    float    nearestPreyDist = 1e9f;
    EntityID nearestMate     = INVALID_ID;
    float    nearestMateDist = 1e9f;
    Vec3     nearestFood     {};   // plant position
    float    nearestFoodDist = 1e9f;
    Vec3     nearestWater    {};
    float    nearestWaterDist= 1e9f;

    // ── Lifecycle ────────────────────────────────────────────────────────────

    void initFromGenome(const Vec3& spawnPos) {
        pos      = spawnPos;
        mass     = genome.bodySize();
        maxEnergy= 80.f + mass * 40.f;
        energy   = maxEnergy * 0.7f;
        lifespan = 120.f + globalRNG().normal(0.f, 20.f);
        needs.initFromGenome(genome);
    }

    // Returns joules consumed this frame
    float tick(float dt, World& world);

    // ── Physics / steering ───────────────────────────────────────────────────

    // Steer toward a world-space target at up to maxSpeed
    void steerToward(const Vec3& target, float maxSpd, float dt) {
        Vec3 dir = (target - pos);
        dir.y = 0;
        float d = dir.len();
        if (d < 0.1f) return;
        dir = dir * (1.f / d);
        float spd = std::min(maxSpd, d * 5.f);  // slow down near target
        Vec3 desired = dir * spd;
        // Simple first-order velocity tracking
        vel.x += (desired.x - vel.x) * std::min(1.f, dt * 8.f);
        vel.z += (desired.z - vel.z) * std::min(1.f, dt * 8.f);
        yaw    = std::atan2(vel.x, vel.z);
    }

    void steerAway(const Vec3& threat, float maxSpd, float dt) {
        Vec3 dir = pos - threat;
        dir.y = 0;
        float d = dir.len();
        if (d < 0.1f) dir = Vec3{1,0,0};
        else dir = dir * (1.f / d);
        Vec3 desired = dir * maxSpd;
        vel.x += (desired.x - vel.x) * std::min(1.f, dt * 10.f);
        vel.z += (desired.z - vel.z) * std::min(1.f, dt * 10.f);
        yaw    = std::atan2(vel.x, vel.z);
    }

    // ── Energy model ─────────────────────────────────────────────────────────

    // Quadratic speed cost + basal + slope surcharge
    float energyCost(float speed, float slopeSin, float dt) const {
        const float kBasal = 0.008f;
        const float kMove  = 0.04f;
        const float kClimb = 0.025f;
        return (kBasal * mass
              + kMove  * speed * speed * mass
              + kClimb * slopeSin * mass) * dt;
    }

    // ── Helpers ───────────────────────────────────────────────────────────────
    bool  isHerbivore() const { return genome.isHerbivore(); }
    bool  isCarnivore() const { return genome.isCarnivore(); }
    float speedCap()    const {
        // Energy shortage throttles speed
        float eFrac = energy / maxEnergy;
        return genome.maxSpeed() * std::max(0.1f, eFrac);
    }
};

// ── Children list helper ──────────────────────────────────────────────────────
// Stored separately to avoid bloating the hot Creature struct
struct Lineage {
    EntityID        id;
    EntityID        parentA, parentB;
    uint32_t        generation;
    std::vector<EntityID> children;
};
