#pragma once
#include "genome.h"
#include <array>
#include <algorithm>

// ── Drive enum ───────────────────────────────────────────────────────────────
enum class Drive : int {
    Hunger = 0,
    Thirst,
    Sleep,
    Libido,
    Fear,
    Social,
    COUNT
};

constexpr int DRIVE_COUNT = static_cast<int>(Drive::COUNT);

inline const char* driveName(Drive d) {
    switch (d) {
        case Drive::Hunger: return "Hunger";
        case Drive::Thirst: return "Thirst";
        case Drive::Sleep:  return "Sleep";
        case Drive::Libido: return "Libido";
        case Drive::Fear:   return "Fear";
        case Drive::Social: return "Social";
        default:            return "??";
    }
}

// ── Needs state ───────────────────────────────────────────────────────────────
struct needs {
    // level ∈ [0,1]: 0 = fully satisfied, 1 = critical
    std::array<float, DRIVE_COUNT> level{};

    // Crave rates: how fast each need rises per second
    std::array<float, DRIVE_COUNT> craveRate{};

    void initFromGenome(const genome& g) {
        craveRate[(int)Drive::Hunger] = g.hungerRate();
        craveRate[(int)Drive::Thirst] = g.thirstRate();
        craveRate[(int)Drive::Sleep]  = g.sleepRate();
        craveRate[(int)Drive::Libido] = g.libidoRate();
        craveRate[(int)Drive::Fear]   = 0.f;   // driven externally by perception
        craveRate[(int)Drive::Social] = g.socialRate();

        // Start creatures at moderate need levels (not freshly born and full)
        rng& rng = globalRNG();
        for (int i = 0; i < DRIVE_COUNT; i++)
            level[i] = (i == (int)Drive::Fear) ? 0.f : rng.range(0.1f, 0.5f);
    }

    void tick(float dt) {
        for (int i = 0; i < DRIVE_COUNT; i++) {
            if (i == (int)Drive::Fear) continue;    // fear is set externally
            level[i] = std::min(1.f, level[i] + craveRate[i] * dt);
        }
    }

    // Satisfy a drive by some amount
    void satisfy(Drive d, float amount) {
        int i = static_cast<int>(d);
        level[i] = std::max(0.f, level[i] - amount);
    }

    // Raise fear based on proximity to a predator
    // distNorm: 0 = right on top, 1 = at vision edge
    void raiseFear(float distNorm, float sensitivity, float dt) {
        float stimulus = (1.f - distNorm) * sensitivity;
        level[(int)Drive::Fear] = std::min(1.f,
            level[(int)Drive::Fear] + stimulus * dt * 2.f);
    }

    // Fear decays when no threats are perceived
    void decayFear(float dt) {
        level[(int)Drive::Fear] = std::max(0.f,
            level[(int)Drive::Fear] - dt * 0.3f);
    }

    // ── Priority resolution ───────────────────────────────────────────────────
    // Fear hard-overrides everything above a threshold.
    // Below that, we pick the highest level drive.
    // A drive is eligible only if its crave rate > 0 (latent drives ignored).
    Drive activeDrive() const {
        if (level[(int)Drive::Fear] > 0.5f)
            return Drive::Fear;

        Drive best  = Drive::Hunger;
        float bLvl  = -1.f;
        for (int i = 0; i < DRIVE_COUNT; i++) {
            if (i == (int)Drive::Fear) continue;
            if (craveRate[i] <= 1e-5f) continue;    // latent – skip
            if (level[i] > bLvl) {
                bLvl = level[i];
                best = static_cast<Drive>(i);
            }
        }
        return best;
    }

    // True if creature is in crisis for this drive
    bool isCritical(Drive d) const {
        return level[static_cast<int>(d)] > 0.85f;
    }

    // Urgency of the active drive (0-1), useful for blending speeds
    float urgency() const {
        Drive d = activeDrive();
        return level[static_cast<int>(d)];
    }
};
