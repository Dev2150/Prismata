// SimUI.h
#pragma once
#include "World/World.hpp"
#include "Sim/DataRecorder.hpp"
#include "Renderer/Renderer.hpp"
#include <string>
#include <vector>

#include "imgui.hpp"

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

    struct PerformanceTracker {
        static constexpr int RING = 300;  // ~5 seconds at 60fps

        float fpsSamples[RING] = {};      // each entry = instantaneous FPS for that frame
        float upsSamples[RING] = {};
        int   head = 0;
        int   count = 0;

        // Rolling window accumulators (for 0.5s average display)
        int   fpsFrameCount = 0;
        float fpsAccum      = 0.f;
        int   upsTickCount  = 0;
        float upsAccum      = 0.f;

        float displayFPS    = 0.f;
        float displayUPS    = 0.f;
        float onePctLowFPS  = 0.f;
        float onePctLowUPS  = 0.f;

        void pushFPS(float instantFPS) {
            fpsSamples[head % RING] = instantFPS;
            // UPS slot filled separately; mark as invalid until set
        }
        void pushUPS(float instantUPS) {
            upsSamples[head % RING] = instantUPS;
            head = (head + 1) % RING;
            count = std::min(count + 1, RING);
        }

        // Compute the Nth percentile from an array copy (partial sort)
        static float percentile(float* arr, int n, float pct) {
            if (n <= 0) return 0.f;
            // Copy to temp buffer and partial sort
            static float tmp[RING];
            for (int i = 0; i < n; i++) tmp[i] = arr[i];
            int k = std::max(0, (int)(pct * n / 100.f));
            std::nth_element(tmp, tmp + k, tmp + n);
            return tmp[k];
        }

        void compute1PctLows() {
            if (count == 0) return;
            // 1% lows = bottom 1st percentile = lowest ~1% of frame rates
            // Collect the valid window
            int n = count;
            float fBuf[RING], uBuf[RING];
            for (int i = 0; i < n; i++) {
                fBuf[i] = fpsSamples[i];
                uBuf[i] = upsSamples[i];
            }
            onePctLowFPS = percentile(fBuf, n, 1.f);
            onePctLowUPS = percentile(uBuf, n, 1.f);
        }
    } perf;

    float displayFPS = 0.f;   // kept for SimUI internal use, mirrors perf.displayFPS
    float displayUPS = 0.f;

    float onePctLowFPS = 0.f;   // 1% low FPS (worst 1% of recent frames)
    float onePctLowUPS = 0.f;   // 1% low UPS

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

    ImVec4 get_color_from_term(const char *term);

    const char *get_term_from_term(int total, int count_lower, int count_greater);

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
