#include <cstdint>

#include "World.hpp"


// ── Species registry ──────────────────────────────────────────────────────────
// Implements a simple nearest-centroid clustering algorithm for species.
// A creature belongs to the species whose centroid genome is closest (RMS distance).
// If the closest centroid is still farther than speciesEpsilon, a new species
// is formed. This produces speciation events when lineages diverge enough.
uint32_t World::classifySpecies(const Genome& g) {
    float bestDist = 1e9f;
    uint32_t bestID = 0;

    // Find the nearest existing (non-extinct) species centroid
    for (auto& sp : species) {
        if (sp.count == 0) continue;   // skip extinct species
        float d = g.distanceTo(sp.centroid);
        if (d < bestDist) { bestDist = d; bestID = sp.id; }
    }

    // If no species is close enough (or the registry is empty), form a new species
    if (bestDist > cfg.speciesEpsilon || species.empty()) {
        SpeciesInfo sp;
        sp.id      = nextSpeciesID++;
        sp.centroid= g;
        sp.count   = 1;
        sp.allTime = 1;

        // Derive a display colour from the genome hue (6-sector HSV approximation)
        float h = g.hue() / 60.f;
        int   hi = (int)h;
        float f  = h - hi;
        float p  = 0.3f, q = 0.3f + 0.7f * (1-f), tv = 0.3f + 0.7f * f;
        float rgb[6][3] = {
            {1,tv,p},{q,1,p},{p,1,tv},{p,q,1},{tv,p,1},{1,p,q}
        };
        sp.color[0] = rgb[hi%6][0];
        sp.color[1] = rgb[hi%6][1];
        sp.color[2] = rgb[hi%6][2];

        // Simple procedural name from a small syllable list + numeric suffix
        const char* parts[] = {"Azel","Brix","Calu","Dorn","Evon","Fyx","Gorn","Hexa"};
        sp.name = std::string(parts[sp.id % 8]) + std::to_string(sp.id);

        species.push_back(sp);
        return sp.id;
    }

    // Increment population count of the matched species
    auto it = std::find_if(species.begin(), species.end(),
                           [&](const SpeciesInfo& s){ return s.id == bestID; });
    if (it != species.end()) { it->count++; it->allTime++; }
    return bestID;
}

// Recompute each species' centroid genome by averaging all living members' raw genes.
// Also resets and recounts species populations. Called every 5 simulated seconds
// (not every tick) to amortise the O(creatures + species) cost.
void World::updateSpeciesCentroids() {
    // Zero all counts and centroid accumulators
    for (auto& sp : species) {
        sp.count = 0;
        sp.centroid = Genome{};   // zeroed raw array
    }
    // Sum genome values per species
    for (const auto& c : creatures) {
        if (!c.alive) continue;
        auto it = std::find_if(species.begin(), species.end(),
                               [&](const SpeciesInfo& s){ return s.id == c.speciesID; });
        if (it == species.end()) continue;
        it->count++;
        for (int i = 0; i < GENOME_SIZE; i++)
            it->centroid.raw[i] += c.genome.raw[i];
    }
    // Divide by count to get the mean genome per species
    for (auto& sp : species) {
        if (sp.count == 0) continue;
        for (int i = 0; i < GENOME_SIZE; i++)
            sp.centroid.raw[i] /= sp.count;
    }
}

const SpeciesInfo* World::getSpecies(uint32_t id) const {
    for (const auto& sp : species)
        if (sp.id == id) return &sp;
    return nullptr;
}

// ── Genetic distance helper ───────────────────────────────────────────────────
// Used by handleReproduction to enforce the biological species concept:
// two creatures can only interbreed if their genomes are within `epsilon` distance.
bool sameSpecies(const Genome& a, const Genome& b, float epsilon) {
    return a.distanceTo(b) < epsilon;
}
