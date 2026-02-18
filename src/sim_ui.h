// SimUI.h
#pragma once
#include "world.h"
#include "data_recorder.h"
#include "renderer.h"

struct sim_ui {
    // ── State ──────────────────────────────────────────────────────────────────
    EntityID   selectedID      = INVALID_ID;
    int        chartGeneIdx    = GENE_MAX_SPEED;
    bool       showDemoWindow  = false;
    char       savePathBuf[256]= "world.evosave";
    char       csvPathBuf[256] = "export.csv";

    // Histogram buffers
    std::vector<float> histX, histY;

    // ── Entry point ───────────────────────────────────────────────────────────
    void draw(world& world, data_recorder& rec, renderer& rend);

private:
    void drawMainMenuBar(world& world, data_recorder& rec, renderer& rend);
    void drawSimControls(world& world, renderer& rend);
    void drawPopStats(const world& world, const data_recorder& rec);
    void drawEntityInspector(const world& world);
    void drawSpeciesPanel(const world& world);
    void drawGeneCharts(const world& world, const data_recorder& rec);
    void drawPlayerPanel(world& world, renderer& rend);
};
