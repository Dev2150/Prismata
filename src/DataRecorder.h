#pragma once
#include "World.h"
#include <vector>
#include <deque>
#include <algorithm>

// ── DataSample ────────────────────────────────────────────────────────────────
// A single snapshot of population-level statistics captured at one sample point.
// All per-creature values are population means, not totals.
struct DataSample {
    float time         = 0;   // Simulation time (seconds) when sample was taken
    int   totalPop     = 0;   // Total living creatures
    int   herbPop      = 0;   // Herbivores (herbEff > 0.6 AND carnEff < 0.4)
    int   carnPop      = 0;   // Carnivores (carnEff > 0.6 AND herbEff < 0.4)
    int   speciesCount = 0;   // Species with at least one living member
    float avgSpeed     = 0;   // Mean maxSpeed gene across all living creatures
    float avgSize      = 0;   // Mean bodySize gene across all living creatures
    float avgHerbEff   = 0;   // Mean herbEfficiency gene (tracks plant-eating evolution)
    float avgCarnEff   = 0;   // Mean carnEfficiency gene (tracks meat-eating evolution)
    float avgMutRate   = 0;   // Mean mutationRate gene (evolving evolvability indicator)
    float plantCount   = 0;   // Number of alive plant entities
};

// ── DataRecorder ──────────────────────────────────────────────────────────────
// Samples the simulation state at a fixed rate (default 1 Hz) and maintains
// a ring buffer of up to MAX_SAMPLES history points.
//
// Pre-allocated flat std::vector buffers mirror the deque for ImPlot, which
// requires contiguous float arrays. These are rebuilt whenever a new sample
// is pushed (rebuildBuffers). A deque is used for history so pop_front is O(1).
struct DataRecorder {
    // 1 hour of 1-Hz data; older samples are discarded automatically
    static constexpr int MAX_SAMPLES = 3600;

    // Ring buffer of samples; oldest at front, newest at back
    std::deque<DataSample> history;

    // Pre-allocated contiguous arrays for ImPlot (avoids per-frame heap allocation).
    // Each buf[i] corresponds to history[i] so they can be passed directly to
    // ImPlot::PlotLine(name, t_buf.data(), y_buf.data(), n).
    std::vector<float> t_buf,       // simulation time axis
                       total_buf,   // total population
                       herb_buf,    // herbivore population
                       carn_buf,    // carnivore population
                       species_buf, // active species count
                       speed_buf,   // average maxSpeed
                       size_buf,    // average bodySize
                       herbEff_buf, // average herbEfficiency
                       carnEff_buf, // average carnEfficiency
                       plant_buf;   // plant count

    float sampleTimer    = 0.f;    // accumulator; fires when it exceeds sampleInterval
    float sampleInterval = 1.f;    // how many simulation seconds between samples

    // Called every frame. Accumulates dt; when the interval is reached, captures
    // a new DataSample from the current world state and refreshes ImPlot buffers.
    void tick(float dt, const World& world) {
        sampleTimer += dt;
        if (sampleTimer < sampleInterval) return;
        sampleTimer = 0.f;

        DataSample s;
        s.time = world.simTime;

        // Single pass over all living creatures to compute counts and trait sums
        float sumSpeed = 0, sumSize = 0, sumH = 0, sumC = 0, sumMut = 0;
        for (const auto& c : world.creatures) {
            if (!c.alive) continue;
            s.totalPop++;
            if (c.isHerbivore()) s.herbPop++;
            else if (c.isCarnivore()) s.carnPop++;
            sumSpeed += c.genome.maxSpeed();
            sumSize  += c.genome.bodySize();
            sumH     += c.genome.herbEfficiency();
            sumC     += c.genome.carnEfficiency();
            sumMut   += c.genome.mutationRate();
        }
        // Compute means only if at least one creature is alive
        if (s.totalPop > 0) {
            s.avgSpeed   = sumSpeed / s.totalPop;
            s.avgSize    = sumSize  / s.totalPop;
            s.avgHerbEff = sumH     / s.totalPop;
            s.avgCarnEff = sumC     / s.totalPop;
            s.avgMutRate = sumMut   / s.totalPop;
        }
        for (const auto& p : world.plants) s.plantCount += p.alive ? 1.f : 0.f;
        // Count only species that have living members
        s.speciesCount = (int)std::count_if(world.species.begin(), world.species.end(),
                                            [](const SpeciesInfo& sp){ return sp.count > 0; });

        history.push_back(s);
        if ((int)history.size() > MAX_SAMPLES) history.pop_front();  // discard oldest

        rebuildBuffers();  // keep ImPlot arrays in sync
    }

    // Synchronise the flat ImPlot buffers with the current deque contents.
    // Must be called after any insertion or removal from `history`.
    void rebuildBuffers() {
        int n = (int)history.size();
        auto resize = [&](std::vector<float>& v){ v.resize(n); };
        resize(t_buf);       resize(total_buf);
        resize(herb_buf);    resize(carn_buf);
        resize(species_buf); resize(speed_buf);
        resize(size_buf);    resize(herbEff_buf);
        resize(carnEff_buf); resize(plant_buf);

        for (int i = 0; i < n; i++) {
            const auto& s  = history[i];
            t_buf[i]       = s.time;
            total_buf[i]   = (float)s.totalPop;
            herb_buf[i]    = (float)s.herbPop;
            carn_buf[i]    = (float)s.carnPop;
            species_buf[i] = (float)s.speciesCount;
            speed_buf[i]   = s.avgSpeed;
            size_buf[i]    = s.avgSize;
            herbEff_buf[i] = s.avgHerbEff;
            carnEff_buf[i] = s.avgCarnEff;
            plant_buf[i]   = s.plantCount;
        }
    }

    int size() const { return (int)history.size(); }

    // Build a histogram of one gene's raw values across the current population.
    // outX[i] = normalised gene value for bin i (0 to 1)
    // outY[i] = count of creatures in that bin
    // The histogram uses equal-width bins over [0,1], the gene's raw range.
    void geneHistogram(const World& world, GeneIdx gene,
                       int bins, std::vector<float>& outX,
                       std::vector<float>& outY) const {
        outX.assign(bins, 0.f);
        outY.assign(bins, 0.f);
        for (const auto& c : world.creatures) {
            if (!c.alive) continue;
            // Map raw gene value [0,1] → bin index [0, bins-1]
            int b = (int)(c.genome.raw[gene] * (bins - 1));
            outY[b]++;
        }
        // Fill X axis with the centre value of each bin
        for (int i = 0; i < bins; i++)
            outX[i] = (float)i / (bins - 1);
    }
};
