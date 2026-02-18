// SimUI.h
#pragma once
#include "World.h"
#include "DataRecorder.h"
#include "Renderer.h"

struct SimUI {
    // ── State ──────────────────────────────────────────────────────────────────
    EntityID   selectedID      = INVALID_ID;
    int        chartGeneIdx    = GENE_MAX_SPEED;
    bool       showDemoWindow  = false;
    char       savePathBuf[256]= "world.evosave";
    char       csvPathBuf[256] = "export.csv";

    // Histogram buffers
    std::vector<float> histX, histY;

    // ── Entry point ───────────────────────────────────────────────────────────
    void draw(World& world, DataRecorder& rec, Renderer& rend);

private:
    void drawMainMenuBar(World& world, DataRecorder& rec, Renderer& rend);
    void drawSimControls(World& world, Renderer& rend);
    void drawPopStats(const World& world, const DataRecorder& rec);
    void drawEntityInspector(const World& world);
    void drawSpeciesPanel(const World& world);
    void drawGeneCharts(const World& world, const DataRecorder& rec);
    void drawPlayerPanel(World& world, Renderer& rend);
};
