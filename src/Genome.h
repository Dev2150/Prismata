#pragma once
#include "RNG.h"
#include <array>
#include <cmath>
#include <algorithm>
#include <string>

// ── Gene indices ────────────────────────────────────────────────────────────
// Each gene is a float in [0,1], linearly mapped to its biological range.
// Adding a gene is safe as long as GENOME_SIZE is updated.
enum GeneIdx : int {
    // Morphology
    GENE_BODY_SIZE = 0,       // [0.5, 3.0] relative volume
    GENE_MAX_SPEED,           // [0.5, 12.0] m/s
    GENE_MAX_SLOPE,           // [5, 65] degrees climbable
    GENE_VISION_RANGE,        // [2, 50] m
    GENE_VISION_FOV,          // [30, 340] degrees

    // Diet
    GENE_HERB_EFFICIENCY,     // [0, 1]  digestion efficiency for plants
    GENE_CARN_EFFICIENCY,     // [0, 1]  digestion efficiency for meat

    // Drives (crave rates – units of need/sec)
    GENE_HUNGER_RATE,         // [0.005, 0.04]
    GENE_THIRST_RATE,         // [0.003, 0.03]
    GENE_SLEEP_RATE,          // [0.001, 0.01]
    GENE_LIBIDO_RATE,         // [0.002, 0.02]
    GENE_FEAR_SENSITIVITY,    // [0, 1]  scales how fast Fear rises near threats

    // Emergent / latent drives (start near 0 in initial population)
    GENE_SOCIAL_RATE,         // [0, 0.015]  herding instinct
    GENE_TERRITORIAL_RATE,    // [0, 0.01]   unused until selected for

    // Reproduction
    GENE_GESTATION_TIME,      // [5, 60] seconds
    GENE_LITTER_BIAS,         // [0,1] → litter size 1 (low) to 3 (high)

    // Evolvability
    GENE_MUTATION_RATE,       // [0.005, 0.08]  probability per gene
    GENE_MUTATION_STD,        // [0.01, 0.12]   std dev of mutation step

    // Appearance (used for rendering + partially for species ID)
    GENE_HUE,                 // [0, 360]
    GENE_PATTERN,             // [0, 1]

    GENOME_SIZE               // always last – total gene count
};

// ── Genome struct ────────────────────────────────────────────────────────────
struct Genome {
    std::array<float, GENOME_SIZE> raw{};

    // ── Accessors (gene → biological value) ──────────────────────────────────
    float bodySize()        const { return map(GENE_BODY_SIZE,       0.5f,  3.0f); }
    float maxSpeed()        const { return map(GENE_MAX_SPEED,       0.5f, 12.0f); }
    float maxSlope()        const { return map(GENE_MAX_SLOPE,       5.0f, 65.0f); }
    float visionRange()     const { return map(GENE_VISION_RANGE,    2.0f, 50.0f); }
    float visionFOV()       const { return map(GENE_VISION_FOV,     30.0f,340.0f); }

    float herbEfficiency()  const { return raw[GENE_HERB_EFFICIENCY]; }
    float carnEfficiency()  const { return raw[GENE_CARN_EFFICIENCY]; }

    // Is this creature predominantly an herbivore / carnivore / omnivore?
    bool  isHerbivore()     const { return herbEfficiency() > 0.6f && carnEfficiency() < 0.4f; }
    bool  isCarnivore()     const { return carnEfficiency() > 0.6f && herbEfficiency() < 0.4f; }

    float hungerRate()      const { return map(GENE_HUNGER_RATE,     0.005f, 0.04f); }
    float thirstRate()      const { return map(GENE_THIRST_RATE,     0.003f, 0.03f); }
    float sleepRate()       const { return map(GENE_SLEEP_RATE,      0.001f, 0.01f); }
    float libidoRate()      const { return map(GENE_LIBIDO_RATE,     0.002f, 0.02f); }
    float fearSensitivity() const { return raw[GENE_FEAR_SENSITIVITY]; }
    float socialRate()      const { return map(GENE_SOCIAL_RATE,     0.0f,  0.015f); }

    float gestationTime()   const { return map(GENE_GESTATION_TIME,  5.0f,  60.0f); }
    int   litterSize()      const { return 1 + static_cast<int>(raw[GENE_LITTER_BIAS] * 2.5f); }

    float mutationRate()    const { return map(GENE_MUTATION_RATE,   0.005f, 0.08f); }
    float mutationStd()     const { return map(GENE_MUTATION_STD,    0.01f,  0.12f); }

    float hue()             const { return map(GENE_HUE,             0.0f, 360.0f); }

    // ── Genetics ─────────────────────────────────────────────────────────────

    // Uniform crossover: each gene is drawn from one parent at random
    static Genome crossover(const Genome& a, const Genome& b, RNG& rng) {
        Genome child;
        for (int i = 0; i < GENOME_SIZE; i++)
            child.raw[i] = rng.chance(0.5f) ? a.raw[i] : b.raw[i];
        return child;
    }

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

    // Normalised RMS distance ∈ [0, 1]
    float distanceTo(const Genome& o) const {
        float sum = 0;
        for (int i = 0; i < GENOME_SIZE; i++) {
            float d = raw[i] - o.raw[i];
            sum += d * d;
        }
        return std::sqrt(sum / (float)GENOME_SIZE);
    }

    // ── Construction helpers ──────────────────────────────────────────────────

    // Randomise all genes
    static Genome randomHerbivore(RNG& rng) {
        Genome g;
        for (auto& v : g.raw) v = rng.uniform();
        // Bias toward herbivory
        g.raw[GENE_HERB_EFFICIENCY] = rng.range(0.6f, 1.0f);
        g.raw[GENE_CARN_EFFICIENCY] = rng.range(0.0f, 0.3f);
        // Start with low latent drives
        g.raw[GENE_SOCIAL_RATE]      = rng.range(0.0f, 0.1f);
        g.raw[GENE_TERRITORIAL_RATE] = rng.range(0.0f, 0.05f);
        return g;
    }

    static Genome randomCarnivore(RNG& rng) {
        Genome g;
        for (auto& v : g.raw) v = rng.uniform();
        g.raw[GENE_HERB_EFFICIENCY] = rng.range(0.0f, 0.3f);
        g.raw[GENE_CARN_EFFICIENCY] = rng.range(0.6f, 1.0f);
        g.raw[GENE_SOCIAL_RATE]      = rng.range(0.0f, 0.1f);
        g.raw[GENE_TERRITORIAL_RATE] = rng.range(0.0f, 0.05f);
        return g;
    }

private:
    float map(int idx, float lo, float hi) const {
        return lo + raw[idx] * (hi - lo);
    }
};
