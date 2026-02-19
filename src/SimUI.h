// SimUI.h
#pragma once
#include "World.h"
#include "DataRecorder.h"
#include "Renderer.h"

struct SimUI {
    // ── State ──────────────────────────────────────────────────────────────────
    EntityID   selectedID      = INVALID_ID;

    // ── Gene chart state ──────────────────────────────────────────────────────
    int        chartGeneIdx    = GENE_MAX_SPEED;
    bool       showDemoWindow  = false;

    // ── File path buffers ──────────────────────────────────────────────────────
    char       savePathBuf[256]= "world.evosave";
    char       csvPathBuf[256] = "export.csv";
    char       settingsPathBuf[256] = "settings.json";

    // ── Settings window ───────────────────────────────────────────────────────
    bool       showSettings   = false;

    // ── Histogram buffers ─────────────────────────────────────────────────────
    std::vector<float> histX, histY;

    // ── Terrain hover ─────────────────────────────────────────────────────────
    // Updated each frame from SimUI::draw() via Renderer::screenToTerrain().
    bool    terrainHitValid  = false;   // did the hover ray hit terrain this frame?
    Vec3    terrainHitPos    = {};      // world-space hit position
    uint8_t terrainHitMat    = 0;       // material at hit position

    // Window dimensions passed in from main.cpp each frame
    int  windowW = 1280, windowH = 800;

    // ── Entry point ───────────────────────────────────────────────────────────
    void draw(World& world, DataRecorder& rec, Renderer& rend);

    // ── Settings serialisation ────────────────────────────────────────────────
    void saveSettingsToFile(const char* path, const World& world, const Renderer& rend) const;
    void loadSettingsFromFile(const char* path, World& world, Renderer& rend);

private:
    void drawMainMenuBar(World& world, DataRecorder& rec, Renderer& rend);
    void drawSimControls(World& world, Renderer& rend);
    void drawPopStats(const World& world, const DataRecorder& rec);
    void drawEntityInspector(const World& world);
    void drawSpeciesPanel(const World& world);
    void drawGeneCharts(const World& world, const DataRecorder& rec);
    void drawPlayerPanel(World& world, Renderer& rend);
    void drawSettingsWindow(World& world, Renderer& rend);
    void drawTerrainHoverTooltip();

    // Update terrain hover data using the renderer's ray cast
    void updateTerrainHover(const Renderer& rend, const World& world);
};
