#pragma once
#include "Genome.hpp"
#include <array>
#include <algorithm>

// ── Drive enum ────────────────────────────────────────────────────────────────
// Drives are the creature's internal motivational states (analogous to animal
// instincts). Each drive has a "urgency" in [0,1]: 0 = fully satisfied, 1 = critical.
// The active drive determines which behaviour state the creature enters.
enum class Drive : int {
    Health = 0,
    Hunger,  // Need for food (plant or meat depending on diet)
    Thirst,      // Need for water; critical thirst causes death
    Sleep,       // Fatigue; sleeping recharges energy faster than resting
    Libido,      // Mating urge; triggers SeekMate behaviour when high enough
    Fear,        // Threat response; overrides all other drives above a threshold
    Social,      // Herding instinct (latent – only matters if socialRate gene is high)
    COUNT        // Sentinel for array sizing
};

constexpr int DRIVE_COUNT = static_cast<int>(Drive::COUNT);

inline const char* driveName(Drive d) {
    switch (d) {
        case Drive::Health: return "Health";
        case Drive::Hunger: return "Hunger";
        case Drive::Thirst: return "Thirst";
        case Drive::Sleep:  return "Sleep";
        case Drive::Libido: return "Libido";
        case Drive::Fear:   return "Fear";
        case Drive::Social: return "Social";
        default:            return "??";
    }
}

// ── Needs ─────────────────────────────────────────────────────────────────────
struct Needs {
    // Current urgency of each drive: 0 (satisfied) → 1 (critical)
    std::array<float, DRIVE_COUNT> urgency{};

    // How fast each drive's urgency rises per real simulated second.
    // Derived from the genome; remains constant for the creature's lifetime.
    std::array<float, DRIVE_COUNT> craveRate{};

    // Multiplier for how much the creature desires to satisfy this need.
    std::array<float, DRIVE_COUNT> desireMult{};

    // Initialise crave rates from genome genes; also randomises starting
    // drive levels so creatures aren't all perfectly fed at spawn.
    void initFromGenome(const Genome& g) {
        craveRate[(int)Drive::Hunger] = g.hungerRate();
        craveRate[(int)Drive::Thirst] = g.thirstRate();
        craveRate[(int)Drive::Sleep]  = g.sleepRate();
        craveRate[(int)Drive::Libido] = g.libidoRate();
        craveRate[(int)Drive::Fear]   = 0.f;   // Fear is driven externally by perception, not by a constant rate
        craveRate[(int)Drive::Social] = g.socialRate();
        craveRate[(int)Drive::Health] = 0.f;

        desireMult[(int)Drive::Health] = g.desireHealth();
        desireMult[(int)Drive::Hunger] = g.desireHunger();
        desireMult[(int)Drive::Thirst] = g.desireThirst();
        desireMult[(int)Drive::Sleep]  = g.desireSleep();
        desireMult[(int)Drive::Libido] = g.desireLibido();
        desireMult[(int)Drive::Fear]   = g.desireFear();
        desireMult[(int)Drive::Social] = g.desireSocial();

        // Stagger starting levels so not all creatures share the same hunger spike
        RNG& rng = globalRNG();
        for (int i = 0; i < DRIVE_COUNT; i++)
            urgency[i] = (i == (int)Drive::Fear || i == (int)Drive::Health) ? 0.f : rng.range(0.1f, 0.5f);
    }

    // Advance all drives by dt seconds. Fear is excluded here because
    // it's updated externally by perception (raiseFear / decayFear).
    void tick(float dt) {
        for (int i = 0; i < DRIVE_COUNT; i++) {
            if (i == (int)Drive::Fear || i == (int)Drive::Health) continue;
            urgency[i] = std::min(1.f, urgency[i] + craveRate[i] * dt);
        }

        // Health impairment from high hunger or thirst
        if (urgency[(int)Drive::Hunger] > 0.8f) {
            urgency[(int)Drive::Health] = std::min(1.f, urgency[(int)Drive::Health] + 0.02f * dt);
        }
        if (urgency[(int)Drive::Thirst] > 0.8f) {
            urgency[(int)Drive::Health] = std::min(1.f, urgency[(int)Drive::Health] + 0.04f * dt);
        }
    }

    // Reduce a drive urgency by `amount` (e.g. after eating, drinking, sleeping).
    // Clamped at 0 so it never goes negative.
    void satisfy(Drive d, float amount) {
        int i = static_cast<int>(d);
        urgency[i] = std::max(0.f, urgency[i] - amount);
    }

    // Raise Fear based on how close a predator is.
    // distNorm = (predator_distance / vision_range): 0 = adjacent, 1 = at edge of sight.
    // A closer predator produces a stronger stimulus. The sensitivity gene scales
    // overall reactivity; fearful creatures will flee at longer distances.
    void raiseFear(float distNorm, float sensitivity, float dt) {
        float stimulus = (1.f - distNorm) * sensitivity;  // stronger when closer
        urgency[(int)Drive::Fear] = std::min(1.f,
            urgency[(int)Drive::Fear] + stimulus * dt * 2.f);
    }

    // Fear decays at a fixed rate (0.3/s) when no predator is visible,
    // so creatures calm down after escaping a threat.
    void decayFear(float dt) {
        urgency[(int)Drive::Fear] = std::max(0.f,
            urgency[(int)Drive::Fear] - dt * 0.3f);
    }

    // ── Priority resolution ───────────────────────────────────────────────────
    // Determines which drive currently governs behaviour.
    //
    // Priority rules:
    //  1. Fear > 0.5 → always flee (hard survival override)
    //  2. Otherwise → highest-urgency drive whose craveRate > 0
    //     (craveRate ≤ 0 means the drive is "latent" / not yet evolved)
    Drive activeDrive() const {
        // Fear hard-overrides everything when the creature is sufficiently scared
        if (urgency[(int)Drive::Fear] > 0.5f)
            return Drive::Fear;

        // Walk all drives and pick the most urgent non-fear, non-latent drive
        Drive best  = Drive::Hunger;  // fallback if everything else is latent
        float bLvl  = -1.f;
        for (int i = 0; i < DRIVE_COUNT; i++) {
            if (i == (int)Drive::Fear) continue;
            if (craveRate[i] <= 1e-5f) continue;   // latent drive – skip
            if (urgency[i] > bLvl) {
                bLvl = urgency[i];
                best = static_cast<Drive>(i);
            }
        }
        return best;
    }

    // Used to trigger death (e.g. critical Thirst → die).
    bool isCritical(Drive d) const {
        return urgency[static_cast<int>(d)] >= 1.0f;
    }

    // Urgency of the currently active drive in [0,1].
    // Used to blend movement speed: more urgent → run faster.
    float dominant_urgency() const {
        Drive d = activeDrive();
        return urgency[static_cast<int>(d)];
    }
};
