#pragma once
#include "world.h"
#include <vector>
#include <deque>
#include <algorithm>

// ── One sample point ──────────────────────────────────────────────────────────
struct DataSample {
    float time         = 0;
    int   totalPop     = 0;
    int   herbPop      = 0;
    int   carnPop      = 0;
    int   speciesCount = 0;
    float avgSpeed     = 0;
    float avgSize      = 0;
    float avgHerbEff   = 0;
    float avgCarnEff   = 0;
    float avgMutRate   = 0;
    float plantCount   = 0;
};

// ── Ring buffer of N seconds of history at 1-Hz resolution ───────────────────
struct data_recorder {
    static constexpr int MAX_SAMPLES = 3600;   // 1 hour at 1 Hz

    std::deque<DataSample> history;

    // Pre-allocated flat arrays for ImPlot (avoids per-frame allocations)
    std::vector<float> t_buf, total_buf, herb_buf, carn_buf,
                       species_buf, speed_buf, size_buf,
                       herbEff_buf, carnEff_buf, plant_buf;

    float sampleTimer = 0.f;
    float sampleInterval = 1.f;   // seconds between samples

    void tick(float dt, const world& world) {
        sampleTimer += dt;
        if (sampleTimer < sampleInterval) return;
        sampleTimer = 0.f;

        DataSample s;
        s.time = world.simTime;

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
        if (s.totalPop > 0) {
            s.avgSpeed   = sumSpeed / s.totalPop;
            s.avgSize    = sumSize  / s.totalPop;
            s.avgHerbEff = sumH     / s.totalPop;
            s.avgCarnEff = sumC     / s.totalPop;
            s.avgMutRate = sumMut   / s.totalPop;
        }
        for (const auto& p : world.plants) s.plantCount += p.alive ? 1.f : 0.f;
        s.speciesCount = (int)std::count_if(world.species.begin(), world.species.end(),
                                            [](const SpeciesInfo& sp){ return sp.count > 0; });

        history.push_back(s);
        if ((int)history.size() > MAX_SAMPLES) history.pop_front();

        rebuildBuffers();
    }

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

    // Gene histogram: collect values of one gene across population
    void geneHistogram(const world& world, GeneIdx gene,
                       int bins, std::vector<float>& outX,
                       std::vector<float>& outY) const {
        outX.assign(bins, 0.f);
        outY.assign(bins, 0.f);
        for (const auto& c : world.creatures) {
            if (!c.alive) continue;
            int b = (int)(c.genome.raw[gene] * (bins - 1));
            outY[b]++;
        }
        for (int i = 0; i < bins; i++)
            outX[i] = (float)i / (bins - 1);
    }
};
