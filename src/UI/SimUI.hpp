// SimUI.h
#pragma once
#include "World/World.hpp"
#include "Sim/DataRecorder.hpp"
#include "Renderer/Renderer.hpp"
#include <string>
#include <vector>

enum class NotifSeverity {
    Info,
    Warning,
    Critical,
};

// ── Notification card ─────────────────────────────────────────────────────────
struct Notification {
    std::string   title;
    std::string   message;
    NotifSeverity severity  = NotifSeverity::Info;
    float         gameTime  = 0.f;   // in-game time when event occurred
    float         age       = 0.f;   // real seconds since pushed (for auto-dismiss)
    bool          dismissed = false;
};

// ── SimUI ─────────────────────────────────────────────────────────────────────
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
    EntityID hoveredCreatureID = INVALID_ID;
    int      hoveredPlantIdx   = -1;

    // Window dimensions passed in from main.cpp each frame
    int  windowW = 1280, windowH = 800;

    // ── Performance metrics (set by App.cpp each frame) ───────────────────────
    float displayFPS = 0.f;   // frames per second (rendering)
    float displayUPS = 0.f;   // simulation updates per second

    // ── Notifications ─────────────────────────────────────────────────────────
    // Newest first; the draw function renders them top-to-bottom in this order.
    std::vector<Notification> notifications;

    // State flags for built-in triggers
    bool lowPopNotifFired = false;

    // Push a new notification card onto the stack.
    // title     – short headline shown in the accent colour
    // message   – body text (word-wrapped inside the card)
    // severity  – controls colour and icon
    // gameTime  – world.simTime at the moment of the event
    void pushNotification(const std::string& title,
                          const std::string& message,
                          NotifSeverity       severity = NotifSeverity::Info,
                          float               gameTime = 0.f);

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
    void drawTerrainHoverTooltip(const World& world);

    // Update terrain hover data using the renderer's ray cast
    void updateTerrainHover(const Renderer& rend, const World& world);

    // Notification internals
    void tickNotifications(float dt, const World& world);
    void drawNotifications();
};