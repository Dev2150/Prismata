#pragma once
#include "../Core/RNG.hpp"
#include <array>
#include <cmath>
#include <algorithm>
#include <string>

// ── Gene indices ──────────────────────────────────────────────────────────────
// The genome is a fixed-length array of floats, each clamped to [0, 1].
// Each gene's raw value is linearly mapped to a meaningful biological range
// by the accessor functions below (e.g. raw 0.0 → min speed, 1.0 → max speed).
//
// Design note: keeping everything in [0,1] makes crossover, mutation, and
// genetic-distance calculations uniform without needing per-gene normalisation.
//
// To add a gene: insert a new enum value before GENOME_SIZE, add an accessor,
// and update the initialisation helpers (randomHerbivore / randomCarnivore).
enum GeneIdx : int {
    // ── Morphology ────────────────────────────────────────────────────────────
    GENE_BODY_SIZE = 0,     // [0.5, 3.0] relative volume – affects mass, energy cap, attack damage
    GENE_MAX_SPEED,         // [0.5, 12.0] m/s – top running speed before energy throttling
    GENE_MAX_SLOPE,         // [5, 65] degrees – steepest terrain the creature can climb
    GENE_VISION_RANGE,      // [2, 50] m – radius of the circular perception zone
    GENE_VISION_FOV,        // [30, 340] degrees – forward-facing cone within the vision radius

    // ── Diet ─────────────────────────────────────────────────────────────────
    GENE_HERB_EFFICIENCY,   // [0, 1] – fraction of plant nutrition converted to energy
    GENE_CARN_EFFICIENCY,   // [0, 1] – fraction of meat energy absorbed per bite

    // ── Drives (need accumulation rates, units: need/second) ──────────────────
    GENE_HUNGER_RATE,       // [0.005, 0.04] – how quickly hunger rises
    GENE_THIRST_RATE,       // [0.003, 0.03] – how quickly thirst rises
    GENE_SLEEP_RATE,        // [0.001, 0.01] – how quickly sleep need rises
    GENE_LIBIDO_RATE,       // [0.002, 0.02] – how quickly the mating urge rises
    GENE_FEAR_SENSITIVITY,  // [0, 1] – scales how strongly nearby predators raise Fear

    // ── Emergent / latent drives ──────────────────────────────────────────────
    // These start near 0 in the initial population and only become significant
    // if natural selection actively favours them.
    GENE_SOCIAL_RATE,       // [0, 0.015] herding instinct (unused drive for now)
    GENE_TERRITORIAL_RATE,  // [0, 0.01]  territory-marking drive (reserved for future use)

    // ── Desires (multipliers for needs) ───────────────────────────────────────
    GENE_DESIRE_HEALTH,
    GENE_DESIRE_HUNGER,
    GENE_DESIRE_THIRST,
    GENE_DESIRE_SLEEP,
    GENE_DESIRE_LIBIDO,
    GENE_DESIRE_FEAR,
    GENE_DESIRE_SOCIAL,

    // ── Reproduction ─────────────────────────────────────────────────────────
    GENE_GESTATION_TIME,    // [5, 60] seconds – countdown from mating to birth
    GENE_LITTER_BIAS,       // [0, 1] – raw value maps to litter size 1–3

    // ── Evolvability ─────────────────────────────────────────────────────────
    // These genes control mutation itself, allowing mutation rate to evolve.
    GENE_MUTATION_RATE,     // [0.005, 0.08] probability that any single gene mutates
    GENE_MUTATION_STD,      // [0.01,  0.12] std-dev of the Gaussian mutation step

    // ── Appearance (rendering + species classification) ───────────────────────
    GENE_HUE,               // [0, 360] HSV hue – creature colour in the viewport
    GENE_PATTERN,           // [0, 1]   future use: pattern overlay intensity

    GENOME_SIZE             // Sentinel – always last; equals the total number of genes
};

// ── Genome ────────────────────────────────────────────────────────────────────
struct Genome {
    // Raw gene values, all in [0, 1]. Index with GeneIdx enum.
    std::array<float, GENOME_SIZE> raw{};

    // ── Accessors (raw gene → biological value) ───────────────────────────────
    // Each accessor applies a linear map:  biological = lo + raw * (hi - lo)
    float bodySize()        const { return map(GENE_BODY_SIZE,        50.f,  300.f); }
    float maxSpeed()        const { return map(GENE_MAX_SPEED,        50.f, 1200.f); }
    float visionRange()     const { return map(GENE_VISION_RANGE,    200.f, 5000.f); }
    float maxSlope()        const { return map(GENE_MAX_SLOPE,       5.0f, 65.0f); }
    float visionFOV()       const { return map(GENE_VISION_FOV,     30.0f,340.0f); }

    // Diet efficiencies are already in [0,1] so no remapping needed
    float herbEfficiency()  const { return raw[GENE_HERB_EFFICIENCY]; }
    float carnEfficiency()  const { return raw[GENE_CARN_EFFICIENCY]; }

    // Diet classification: a creature is an herbivore if it's good at eating
    // plants AND bad at digesting meat; vice-versa for carnivores.
    // Omnivores (both > 0.4 or both < 0.6) fall through both checks.
    bool  isHerbivore()     const { return herbEfficiency() > 0.6f && carnEfficiency() < 0.4f; }
    bool  isCarnivore()     const { return carnEfficiency() > 0.6f && herbEfficiency() < 0.4f; }

    float hungerRate()      const { return map(GENE_HUNGER_RATE,     0.005f, 0.04f); }
    float thirstRate()      const { return map(GENE_THIRST_RATE,     0.003f, 0.03f); }
    float sleepRate()       const { return map(GENE_SLEEP_RATE,      0.001f, 0.01f); }
    float libidoRate()      const { return map(GENE_LIBIDO_RATE,     0.002f, 0.02f); }
    float fearSensitivity() const { return raw[GENE_FEAR_SENSITIVITY]; }
    float socialRate()      const { return map(GENE_SOCIAL_RATE,     0.0f,  0.015f); }

    float desireHealth()    const { return map(GENE_DESIRE_HEALTH,   0.1f, 5.0f); }
    float desireHunger()    const { return map(GENE_DESIRE_HUNGER,   0.1f, 5.0f); }
    float desireThirst()    const { return map(GENE_DESIRE_THIRST,   0.1f, 5.0f); }
    float desireSleep()     const { return map(GENE_DESIRE_SLEEP,    0.1f, 5.0f); }
    float desireLibido()    const { return map(GENE_DESIRE_LIBIDO,   0.1f, 5.0f); }
    float desireFear()      const { return map(GENE_DESIRE_FEAR,     0.1f, 10.0f); }
    float desireSocial()    const { return map(GENE_DESIRE_SOCIAL,   0.0f, 5.0f); }

    float gestationTime()   const { return map(GENE_GESTATION_TIME,  5.0f,  60.0f); }
    // Litter size: raw 0 → 1 offspring, raw 1 → 3 offspring (integer result)
    int   litterSize()      const { return 1 + static_cast<int>(raw[GENE_LITTER_BIAS] * 2.5f); }

    float mutationRate()    const { return map(GENE_MUTATION_RATE,   0.005f, 0.08f); }
    float mutationStd()     const { return map(GENE_MUTATION_STD,    0.01f,  0.12f); }

    float hue()             const { return map(GENE_HUE,             0.0f, 360.0f); }

    // ── Genetics ──────────────────────────────────────────────────────────────

    // Uniform crossover: for each gene independently, pick it from parent A or B
    // with equal probability. This produces a child with a random mix of both
    // parents' traits and avoids linkage disequilibrium effects.
    static Genome crossover(const Genome& a, const Genome& b, RNG& rng) {
        Genome child;
        for (int i = 0; i < GENOME_SIZE; i++)
            child.raw[i] = rng.chance(0.5f) ? a.raw[i] : b.raw[i];
        return child;
    }

    // Per-gene Gaussian mutation. Each gene mutates independently with
    // probability = mutationRate(). The step size is drawn from N(0, mutationStd).
    // Clamping to [0,1] keeps all genes in the valid normalised range.
    void mutate(RNG& rng) {
        float rate = mutationRate();
        float std  = mutationStd();
        for (int i = 0; i < GENOME_SIZE; i++) {
            if (rng.chance(rate)) {
                raw[i] += rng.normal(0.f, std);
                raw[i]  = std::clamp(raw[i], 0.f, 1.f);
            }
        }
    }

    // Normalised RMS distance between two genomes, result in [0, 1].
    // Used for species classification: if distance > epsilon, a new species forms.
    // Dividing by GENOME_SIZE normalises so distance is independent of genome length.
    float distanceTo(const Genome& o) const {
        float sum = 0;
        for (int i = 0; i < GENOME_SIZE; i++) {
            float d = raw[i] - o.raw[i];
            sum += d * d;
        }
        return std::sqrt(sum / (float)GENOME_SIZE);
    }

    // ── Construction helpers ──────────────────────────────────────────────────

    // Generates a fully random genome biased toward herbivory.
    // Most genes are uniformly random; diet genes are force-set after the fact
    // so the creature will actually eat plants. Latent social/territorial drives
    // start low so they don't dominate behaviour from generation 0.
    static Genome randomHerbivore(RNG& rng) {
        Genome g;
        for (auto& v : g.raw) v = rng.uniform();
        g.raw[GENE_HERB_EFFICIENCY] = rng.range(0.6f, 1.0f);  // good at plants
        g.raw[GENE_CARN_EFFICIENCY] = rng.range(0.0f, 0.3f);  // bad at meat
        // Latent drives near-zero so they don't distort early evolution
        g.raw[GENE_SOCIAL_RATE]      = rng.range(0.0f, 0.1f);
        g.raw[GENE_TERRITORIAL_RATE] = rng.range(0.0f, 0.05f);

        g.raw[GENE_DESIRE_HEALTH] = rng.range(0.4f, 0.6f);
        g.raw[GENE_DESIRE_HUNGER] = rng.range(0.4f, 0.6f);
        g.raw[GENE_DESIRE_THIRST] = rng.range(0.4f, 0.6f);
        g.raw[GENE_DESIRE_SLEEP]  = rng.range(0.4f, 0.6f);
        g.raw[GENE_DESIRE_LIBIDO] = rng.range(0.4f, 0.6f);
        g.raw[GENE_DESIRE_FEAR]   = rng.range(0.6f, 0.8f);
        g.raw[GENE_DESIRE_SOCIAL] = rng.range(0.0f, 0.2f);
        return g;
    }

    static Genome randomCarnivore(RNG& rng) {
        Genome g;
        for (auto& v : g.raw) v = rng.uniform();
        g.raw[GENE_HERB_EFFICIENCY] = rng.range(0.0f, 0.3f);  // bad at plants
        g.raw[GENE_CARN_EFFICIENCY] = rng.range(0.6f, 1.0f);  // good at meat
        g.raw[GENE_SOCIAL_RATE]      = rng.range(0.0f, 0.1f);
        g.raw[GENE_TERRITORIAL_RATE] = rng.range(0.0f, 0.05f);

        g.raw[GENE_DESIRE_HEALTH] = rng.range(0.4f, 0.6f);
        g.raw[GENE_DESIRE_HUNGER] = rng.range(0.4f, 0.6f);
        g.raw[GENE_DESIRE_THIRST] = rng.range(0.4f, 0.6f);
        g.raw[GENE_DESIRE_SLEEP]  = rng.range(0.4f, 0.6f);
        g.raw[GENE_DESIRE_LIBIDO] = rng.range(0.4f, 0.6f);
        g.raw[GENE_DESIRE_FEAR]   = rng.range(0.6f, 0.8f);
        g.raw[GENE_DESIRE_SOCIAL] = rng.range(0.0f, 0.2f);
        return g;
    }

private:
    // Linear remapping from the normalised [0,1] gene space to [lo, hi]
    float map(int idx, float lo, float hi) const {
        return lo + raw[idx] * (hi - lo);
    }
};
