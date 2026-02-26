#include "UI/SimUI.hpp"
#include "imgui.hpp"
#include "implot.hpp"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <cmath>

#include "App/App_Globals.hpp"
#include "World/World_Planet.hpp"

// ── Helper: format simTime as "Day D  HH:MM" ─────────────────────────────────
// One in-game day = World::DAY_DURATION simulated seconds.
// Returns a human-readable string: "Day 1  06:30" etc.
static std::string formatGameTime(float simTime) {
    float dayDur  = World::DAY_DURATION;              // 300 s per day
    int   day     = (int)(simTime / dayDur) + 1;      // 1-based day counter
    float inDay   = std::fmod(simTime, dayDur);       // seconds elapsed this day
    float dayFrac = inDay / dayDur;                   // [0,1) fraction of day

    int hour = (int)(dayFrac * 24.f);
    int min  = (int)(std::fmod(dayFrac * 24.f, 1.f) * 60.f);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "Day %-3d  %02d:%02d", day, hour, min);
    return buf;
}

// ── Optionally add a small moon/sun icon based on time ────────────────────────
// Returns a UTF-8 icon character for the current time-of-day period.
static const char* timeIcon(float timeOfDay) {
    if (timeOfDay < 0.1f || timeOfDay > 0.9f) return "🌙"; // midnight
    if (timeOfDay < 0.3f)                      return "🌅"; // dawn
    if (timeOfDay < 0.7f)                      return "☀";  // day
    return "🌇";                                             // dusk
}

// ── Top-level draw ────────────────────────────────────────────────────────────
void SimUI::draw(World& world, DataRecorder& rec, Renderer& rend) {
    updateTerrainHover(rend, world);

    // Advance notification timers and fire built-in triggers
    // Use real dt; for simplicity derive it from ImGui's delta time.
    float realDt = ImGui::GetIO().DeltaTime;
    tickNotifications(realDt, world);

    // ── Snapshot window-open state BEFORE drawing ─────────────────────────
    // Any change (menu-bar checkbox OR the × close button) will be caught
    // by the comparison below and trigger an immediate auto-save.
    struct WinFlags {
        bool panels, simControls, popStats, inspector, species,
             geneCharts, playerPanel, planetDebug, settings;
        bool operator==(const WinFlags& o) const {
            return panels==o.panels && simControls==o.simControls &&
                   popStats==o.popStats && inspector==o.inspector &&
                   species==o.species && geneCharts==o.geneCharts &&
                   playerPanel==o.playerPanel && planetDebug==o.planetDebug &&
                   settings==o.settings;
        }
    };
    auto captureFlags = [&]() -> WinFlags {
        return { showPanels, showSimControls, showPopStats, showInspector,
                 showSpecies, showGeneCharts, showPlayerPanel,
                 showPlanetDebug, showSettings };
    };
    WinFlags before = captureFlags();

    // ── Normal draw ───────────────────────────────────────────────────────
    drawMainMenuBar(world, rec, rend);
    
    if (showPanels) {
        if (showSimControls) drawSimControls(world, rend);
        if (showPopStats)    drawPopStats(world, rec);
        if (showInspector)   drawEntityInspector(world);
        if (showSpecies)     drawSpeciesPanel(world);
        if (showGeneCharts)  drawGeneCharts(world, rec);
        if (showPlayerPanel) drawPlayerPanel(world, rend);

        if (showPlanetDebug) {
            if (ImGui::Begin("Planet Debug", &showPlanetDebug))
                g_planet.drawDebugUI();
            ImGui::End();
        }

        if (showSettings) drawSettingsWindow(world, rend);
    }

    drawTerrainHoverTooltip(world);

    // Notification overlay drawn last so it sits on top of everything
    drawNotifications();

    if (showDemoWindow) {
        ImGui::ShowDemoWindow(&showDemoWindow);
        ImPlot::ShowDemoWindow();
    }

    // ── Auto-save if any window was opened or closed this frame ──────────
    if (!(captureFlags() == before))
        saveSettingsToFile(settingsPathBuf, world, rend);
}

// ── Terrain hover ─────────────────────────────────────────────────────────────
void SimUI::updateTerrainHover(const Renderer& rend, const World& world) {
    terrainHitValid = false;
    hoveredCreatureID = INVALID_ID;
    hoveredPlantIdx = -1;

    // Only raycast when mouse is over the 3D viewport (not over any ImGui window)
    if (ImGui::GetIO().WantCaptureMouse) return;

    ImVec2 mp = ImGui::GetIO().MousePos;

    // Check entities (creatures and plants)
    float ndcX =  (mp.x / windowW) * 2.f - 1.f;
    float ndcY = -(mp.y / windowH) * 2.f + 1.f;
    Mat4 vp    = rend.camera.viewMatrix() * rend.camera.projMatrix((float)windowW / windowH);
    Mat4 vpInv = vp.inversed();

    auto unproject = [&](float z) -> Vec4 {
        Vec4 clip = {ndcX, ndcY, z, 1.f};
        Vec4 w = vpInv.transform(clip);
        float iw = (std::abs(w.w) > 1e-7f) ? 1.f / w.w : 0.f;
        return {w.x*iw, w.y*iw, w.z*iw, 1.f};
    };

    Vec4 near4 = unproject(0.f);
    Vec4 far4  = unproject(1.f);
    float dx = far4.x - near4.x, dy = far4.y - near4.y, dz = far4.z - near4.z;
    float dl = std::sqrt(dx*dx + dy*dy + dz*dz);

    if (dl > 1e-6f) {
        dx /= dl; dy /= dl; dz /= dl;
        float bestDist = 300.f; // m selection radius

        // Check creatures
        for (const auto& c : world.creatures) {
            if (!c.alive) continue;
            float ocx = c.pos.x - near4.x, ocy = c.pos.y - near4.y, ocz = c.pos.z - near4.z;
            float t = ocx*dx + ocy*dy + ocz*dz;
            if (t < 0.f) continue;
            float cx2 = near4.x + dx*t - c.pos.x;
            float cy2 = near4.y + dy*t - c.pos.y;
            float cz2 = near4.z + dz*t - c.pos.z;
            float d = std::sqrt(cx2*cx2 + cy2*cy2 + cz2*cz2);
            if (d < bestDist) {
                bestDist = d;
                hoveredCreatureID = c.id;
                hoveredPlantIdx = -1;
            }
        }

        // Check plants
        for (int i = 0; i < (int)world.plants.size(); i++) {
            const auto& p = world.plants[i];
            if (!p.alive) continue;
            float ocx = p.pos.x - near4.x, ocy = p.pos.y - near4.y, ocz = p.pos.z - near4.z;
            float t = ocx*dx + ocy*dy + ocz*dz;
            if (t < 0.f) continue;
            float cx2 = near4.x + dx*t - p.pos.x;
            float cy2 = near4.y + dy*t - p.pos.y;
            float cz2 = near4.z + dz*t - p.pos.z;
            float d = std::sqrt(cx2*cx2 + cy2*cy2 + cz2*cz2);
            if (d < bestDist) {
                bestDist = d;
                hoveredPlantIdx = i;
                hoveredCreatureID = INVALID_ID;
            }
        }
    }

    // Check terrain
    Vec3 pos; uint8_t mat;
    if (rend.screenToTerrain(mp.x, mp.y, (float)windowW, (float)windowH, world, pos, mat)) {
        terrainHitValid = true;
        terrainHitPos   = pos;
        terrainHitMat   = mat;
    }
}

void SimUI::drawTerrainHoverTooltip(const World& world) {
    if (!terrainHitValid && hoveredCreatureID == INVALID_ID && hoveredPlantIdx == -1) return;

    ImGui::SetNextWindowPos(
        ImVec2(ImGui::GetIO().MousePos.x + 16.f, ImGui::GetIO().MousePos.y + 8.f),
        ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::SetNextWindowSize(ImVec2(0, 0));   // auto-size
    ImGui::Begin("##TerrainHover",
                 nullptr,
                 ImGuiWindowFlags_NoDecoration |
                 ImGuiWindowFlags_NoInputs     |
                 ImGuiWindowFlags_NoNav         |
                 ImGuiWindowFlags_NoMove        |
                 ImGuiWindowFlags_AlwaysAutoResize);

    if (hoveredCreatureID != INVALID_ID) {
        auto it = world.idToIndex.find(hoveredCreatureID);
        if (it != world.idToIndex.end()) {
            const Creature& c = world.creatures[it->second];
            const SpeciesInfo* sp = world.getSpecies(c.speciesID);
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f), "Creature #%u", c.id);
            ImGui::Text("Species: %s", sp ? sp->name.c_str() : "?");
            ImGui::Text("Diet: %s", c.isHerbivore() ? "Herbivore" : c.isCarnivore() ? "Carnivore" : "Omnivore");
            ImGui::Text("Energy: %.1f / %.1f", c.energy, c.maxEnergy);
            ImGui::Text("Age: %.1f / %.1f", c.age, c.lifespan);

            const char* bhNames[] = {"Idle","SeekFood","SeekWater","Sleep",
                                     "SeekMate","Flee","Hunt","Mating","Healing"};
            ImGui::Text("Action: %s", bhNames[(int)c.behavior]);
        } else {
            ImGui::Text("Creature died.");
        }
    } else if (hoveredPlantIdx != -1) {
        const Plant& p = world.plants[hoveredPlantIdx];
        const char* pType = p.type == 0 ? "Grass" : p.type == 1 ? "Bush" : "Tree";
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f), "Plant: %s", pType);
        ImGui::Text("Nutrition: %.1f", p.nutrition);
    } else if (terrainHitValid) {
        // Material colour swatch + name
        static const ImVec4 matColors[] = {
            {0.25f,0.55f,0.15f,1}, // Grass – green
            {0.50f,0.50f,0.50f,1}, // Rock  – grey
            {0.70f,0.60f,0.40f,1}, // Sand  – tan
            {0.08f,0.35f,0.72f,1}, // Water – blue
            {0.90f,0.95f,1.00f,1}, // Snow  – white
        };
        uint8_t m = std::min(terrainHitMat, (uint8_t)4);
        ImGui::TextColored(matColors[m], "Terrain: %s", World::materialName(terrainHitMat));
        // Height above sea level: noise displacement from the planet's base radius.
        // terrainHitPos.y is world-space Y (planet center is at Y=-180000) so we
        // use noiseHeight() which gives metres above the base sphere radius.
        float heightAboveSea = g_planet_surface.noiseHeight(terrainHitPos);
        ImGui::Text("Height : %.1f m", heightAboveSea);
        ImGui::Text("Pos    : (%.1f, %.1f, %.1f)",
                    terrainHitPos.x, terrainHitPos.y, terrainHitPos.z);
    }

    ImGui::End();
}

// ── Menu bar ──────────────────────────────────────────────────────────────────
void SimUI::drawMainMenuBar(World& world, DataRecorder& rec, Renderer& rend) {
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("File")) {
        ImGui::InputText("##savepath", savePathBuf, sizeof(savePathBuf));
        ImGui::SameLine();
        if (ImGui::MenuItem("Save"))
            world.saveToFile(savePathBuf);
        if (ImGui::MenuItem("Load"))
            world.loadFromFile(savePathBuf);
        ImGui::Separator();
        ImGui::InputText("##csvpath", csvPathBuf, sizeof(csvPathBuf));
        ImGui::SameLine();
        if (ImGui::MenuItem("Export CSV"))
            world.exportCSV(csvPathBuf);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset World"))
            world.reset();
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("View")) {
        ImGui::Checkbox("Show UI Panels (Master)", &showPanels);
        ImGui::Separator();
        ImGui::Checkbox("Simulation Controls", &showSimControls);
        ImGui::Checkbox("Population Statistics", &showPopStats);
        ImGui::Checkbox("Entity Inspector", &showInspector);
        ImGui::Checkbox("Species", &showSpecies);
        ImGui::Checkbox("Gene Evolution", &showGeneCharts);
        ImGui::Checkbox("Player Mode", &showPlayerPanel);
        ImGui::Checkbox("Planet Debug", &showPlanetDebug);
        ImGui::Checkbox("Settings", &showSettings);
        ImGui::Separator();
        ImGui::Checkbox("Wireframe",   &rend.wireframe);
        ImGui::Checkbox("FOV Cone",    &rend.showFOVCone);
        ImGui::Separator();
        ImGui::Checkbox("ImGui Demo",  &showDemoWindow);
        ImGui::EndMenu();
    }

    // Pause indicator (Space to toggle hint)
    if (world.cfg.paused)
        ImGui::TextColored({1.f,0.4f,0.1f,1.f}, "  ⏸ PAUSED (Space)");
    else
        ImGui::Text("  ▶");

    // ── In-game time display ──────────────────────────────────────────────────
    std::string gt = formatGameTime(world.simTime);
    ImGui::Text("  |  %s  %s", gt.c_str(), timeIcon(world.timeOfDay()));

    // ── Simulation stats ──────────────────────────────────────────────────────
    ImGui::Text("  |  Pop=%d  Species=%d",
        (int)world.creatures.size(),
        (int)std::count_if(world.species.begin(), world.species.end(),
                           [](const SpeciesInfo& s){ return s.count > 0; }));

    // ── Sim speed indicator ───────────────────────────────────────────────────
    ImGui::TextColored({0.6f,1.f,0.6f,1.f}, "  |  ×%.1f  (-/+)", world.cfg.simSpeed);

    // ── FPS / UPS display ─────────────────────────────────────────────────────
    // FPS = render frames per second  (how fast the GPU is presenting)
    // UPS = simulation updates per second (world.tick calls per second,
    //       weighted by simSpeed so it reflects actual sim throughput)
    //
    // Colour coding:
    //   >= 60 FPS → green    (smooth)
    //   >= 30 FPS → yellow   (acceptable)
    //    < 30 FPS → red      (slow)
    {
        ImVec4 fpsCol;
        if      (displayFPS >= 60.f) fpsCol = {0.3f, 1.0f, 0.3f, 1.f};
        else if (displayFPS >= 30.f) fpsCol = {1.0f, 0.9f, 0.2f, 1.f};
        else                         fpsCol = {1.0f, 0.3f, 0.2f, 1.f};

        // FPS with 1% low in parentheses
        ImGui::TextColored(fpsCol, "  |  FPS: %4.0f", displayFPS);
        ImGui::SameLine(0.f, 0.f);
        ImGui::TextColored(ImVec4(fpsCol.x * 0.7f, fpsCol.y * 0.7f, fpsCol.z * 0.7f, 1.f),
            " (%3.0f)", onePctLowFPS);
        ImGui::SameLine(0.f, 0.f);

        // UPS with 1% low
        ImVec4 upsCol    = {0.6f, 0.85f, 1.0f, 1.f};
        ImVec4 upsDimCol = {0.42f, 0.60f, 0.70f, 1.f};
        ImGui::TextColored(upsCol, "  UPS: %4.0f", displayUPS);
        ImGui::SameLine(0.f, 0.f);
        ImGui::TextColored(upsDimCol, " (%3.0f)", onePctLowUPS);
    }

    // ── Controls hint (right-aligned) ─────────────────────────────────────────
    // Show a quick hotkey reminder at the far right of the menu bar.
    {
        const char* hint = "WASD=move  Q/E=turn  F/R=alt  Wheel=zoom  RMB=look  P=possess";
        float hintW = ImGui::CalcTextSize(hint).x;
        float avail = ImGui::GetContentRegionAvail().x;
        if (avail > hintW + 8.f) {
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - hintW - 4.f);
            ImGui::TextDisabled("%s", hint);
        }
    }

    ImGui::EndMainMenuBar();
}

// ── Sim controls ──────────────────────────────────────────────────────────────
void SimUI::drawSimControls(World& world, Renderer& rend) {
    if (!ImGui::Begin("Simulation Controls", &showSimControls)) { ImGui::End(); return; }

    // Pause / play buttons
    if (world.cfg.paused) {
        if (ImGui::Button("▶ Play (Space)"))  world.cfg.paused = false;
    } else {
        if (ImGui::Button("⏸ Pause (Space)")) world.cfg.paused = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) world.reset();

    ImGui::Separator();

    // ── In-game time ──────────────────────────────────────────────────────────
    {
        float dayDur   = World::DAY_DURATION;
        float inDay    = std::fmod(world.simTime, dayDur);
        float progress = inDay / dayDur;           // fraction through current day

        // Time label
        std::string gt = formatGameTime(world.simTime);
        ImGui::Text("%s %s", timeIcon(world.timeOfDay()), gt.c_str());

        // Progress bar showing position within the current day
        // Colour shifts from dark blue (night) → orange (dawn) → white (day) → orange (dusk)
        float t = world.timeOfDay();
        float r, g, b;
        if      (t < 0.25f) { float f=t/0.25f;      r=f;      g=f*0.45f;  b=0.12f+f*0.5f; } // dawn
        else if (t < 0.50f) { float f=(t-0.25f)/0.25f; r=1.f;     g=0.45f+f*0.5f; b=0.62f+f*0.18f; } // morning
        else if (t < 0.75f) { float f=(t-0.50f)/0.25f; r=1.f; g=0.95f;  b=0.80f-f*0.68f; } // afternoon
        else                 { float f=(t-0.75f)/0.25f; r=1.f-f;   g=0.95f-f*0.9f; b=0.12f-f*0.09f; } // dusk→night

        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(r, g, b, 1.f));
        char overlay[32];
        int h=(int)(t*24), mn=(int)(std::fmod(t*24,1.f)*60);
        std::snprintf(overlay, sizeof(overlay), "%02d:%02d", h, mn);
        ImGui::ProgressBar(progress, ImVec2(-1, 8), overlay);
        ImGui::PopStyleColor();

        ImGui::TextDisabled("1 day = %.0f real seconds  (×%.1f speed)",
            World::DAY_DURATION, world.cfg.simSpeed);
    }

    ImGui::Separator();
    ImGui::SliderFloat("Mutation Scale",&world.cfg.mutationRateScale, 0.1f, 5.f);
    ImGui::SliderFloat("Species Epsilon", &world.cfg.speciesEpsilon,    0.05f, 0.5f);
    ImGui::SliderFloat("Plant Grow Rate",&world.cfg.plantGrowRate,   0.f, 5.f);
    ImGui::SliderInt  ("Max Population",&world.cfg.maxPopulation, 100, Renderer::MAX_CREATURES);

    ImGui::Separator();
    ImGui::Text("Camera");
    ImGui::SliderFloat("FOV", &rend.camera.fovY, 30.f, 120.f);

    ImGui::Separator();
    ImGui::Text("Spawn");
    static int nHerb = 10, nCarn = 5;
    ImGui::InputInt("Herbivores##sp", &nHerb);
    ImGui::InputInt("Carnivores##sp", &nCarn);
    if (ImGui::Button("Spawn Herbivores")) {
        for (int i = 0; i < nHerb; i++) {
            Vec3 pos = g_planet_surface.randomLandPos(globalRNG());
            world.spawnCreature(Genome::randomHerbivore(globalRNG()), pos);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Spawn Carnivores")) {
        for (int i = 0; i < nCarn; i++) {
            Vec3 pos = g_planet_surface.randomLandPos(globalRNG());
            world.spawnCreature(Genome::randomCarnivore(globalRNG()), pos);
        }
    }

    ImGui::End();
}

// ── Population stats ──────────────────────────────────────────────────────────
void SimUI::drawPopStats(const World& world, const DataRecorder& rec) {
    if (!ImGui::Begin("Population Statistics", &showPopStats)) { ImGui::End(); return; }
    int n = rec.size();

    if (n > 1 && ImPlot::BeginPlot("Population", ImVec2(-1, 180))) {
        ImPlot::SetupAxes("Time (s)", "Count");
        ImPlot::PlotLine("Total",     rec.t_buf.data(), rec.total_buf.data(), n);
        ImPlot::PlotLine("Herbivore", rec.t_buf.data(), rec.herb_buf.data(),  n);
        ImPlot::PlotLine("Carnivore", rec.t_buf.data(), rec.carn_buf.data(),  n);
        ImPlot::PlotLine("Plants",    rec.t_buf.data(), rec.plant_buf.data(), n);
        ImPlot::EndPlot();
    }

    if (n > 1 && ImPlot::BeginPlot("Species Count", ImVec2(-1, 140))) {
        ImPlot::SetupAxes("Time (s)", "Species");
        ImPlot::PlotLine("Active Species", rec.t_buf.data(), rec.species_buf.data(), n);
        ImPlot::EndPlot();
    }

    ImGui::End();
}

ImVec4 SimUI::get_color_from_term(const char *term) {
    ImVec4 color;
    if (strcmp(term, "Lowest") == 0)
        color = ImVec4(1.0f, 0.0f, 0.0f, 1.0f); // Red
    else if (strcmp(term, "Vestigial") == 0)
        color = ImVec4(1.0f, 0.25f, 0.25f, 1.0f); //
    else if (strcmp(term, "Minimal") == 0)
        color = ImVec4(1.0f, 0.50f, 0.50f, 1.0f); //
    else if (strcmp(term, "Reduced") == 0)
        color = ImVec4(1.0f, 0.75f, 0.75f, 1.0f); //
    else if (strcmp(term, "Average") == 0)
        color = ImVec4(1.0f, 1.0f, 1.0f, 1.0f); // White
    else if (strcmp(term, "Elevated") == 0)
        color = ImVec4(0.75f, 1.0f, 0.75f, 1.0f); //
    else if (strcmp(term, "Significant") == 0)
        color = ImVec4(0.5f, 1.0f, 0.5f, 1.0f); //
    else if (strcmp(term, "Extreme") == 0)
        color = ImVec4(0.25f, 1.0f, 0.25f, 1.0f); //
    else if (strcmp(term, "Highest") == 0)
        color = ImVec4(0.0f, 1.0f, 0.0f, 1.0f); // Green
    else // Error
        throw std::invalid_argument("Undefined term");
    return color;
}

const char * SimUI::get_term_from_term(int total, int count_lower, int count_greater) {
    const char* term = "";
    if (total <= 1 || (count_lower == 0 && count_greater == 0)) {
        term = "Average";
    } else if (count_lower == 0) {
        term = "Lowest";
    } else if (count_greater == 0) {
        term = "Highest";
    } else {
        float pct = (float)count_lower / (total - 1);
        if (pct < 0.05f) term = "Vestigial";
        else if (pct < 0.25f) term = "Minimal";
        else if (pct < 0.40f) term = "Reduced";
        else if (pct < 0.60f) term = "Average";
        else if (pct < 0.75f) term = "Elevated";
        else if (pct < 0.95f) term = "Significant";
        else term = "Extreme";
    }
    return term;
}

// ── Entity inspector ──────────────────────────────────────────────────────────
void SimUI::drawEntityInspector(const World& world) {
    if (!ImGui::Begin("Entity Inspector", &showInspector)) { ImGui::End(); return; }

    if (selectedID == INVALID_ID) {
        ImGui::TextDisabled("Click a creature to inspect.");
    } else {
        auto it = world.idToIndex.find(selectedID);
        if (it == world.idToIndex.end()) {
            ImGui::TextDisabled("Entity no longer exists.");
            selectedID = INVALID_ID;
        } else {
            const Creature& c = world.creatures[it->second];
            const SpeciesInfo* sp = world.getSpecies(c.speciesID);

            ImGui::Text("ID: %u  Gen: %u  Species: %s",
                c.id, c.generation, sp ? sp->name.c_str() : "?");
            ImGui::Text("Diet: %s",
                c.isHerbivore() ? "Herbivore" : c.isCarnivore() ? "Carnivore" : "Omnivore");

            // ── Age as a progress bar ─────────────────────────────────────────
            // The bar fills from left (newborn) to right (lifespan reached).
            // Colour shifts green → yellow → red as the creature ages.
            ImGui::Separator();
            {
                float ageFrac = std::min(c.age / c.lifespan, 1.f);
                ImVec4 ageCol;
                if      (ageFrac < 0.4f) ageCol = {0.2f, 0.85f, 0.2f, 1.f};  // young: green
                else if (ageFrac < 0.75f) ageCol = {0.9f, 0.75f, 0.1f, 1.f};  // middle: yellow
                else                      ageCol = {1.0f, 0.25f, 0.15f, 1.f};  // old: red

                char ageBuf[48];
                std::snprintf(ageBuf, sizeof(ageBuf), "%.0f / %.0f s", c.age, c.lifespan);

                ImGui::Text("Age:");
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ageCol);
                ImGui::ProgressBar(ageFrac, ImVec2(-1.f, 0.f), ageBuf);
                ImGui::PopStyleColor();
            }

            // ── Energy bar ────────────────────────────────────────────────────
            {
                float eFrac = std::min(c.energy / c.maxEnergy, 1.f);
                ImVec4 eCol = {1.f - eFrac, eFrac * 0.8f, 0.1f, 1.f};
                char eBuf[48];
                std::snprintf(eBuf, sizeof(eBuf), "%.1f / %.1f", c.energy, c.maxEnergy);
                ImGui::Text("Energy:");
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, eCol);
                ImGui::ProgressBar(eFrac, ImVec2(-1.f, 0.f), eBuf);
                ImGui::PopStyleColor();
            }

            ImGui::Text("Pos: (%.1f, %.1f, %.1f)  Speed: %.2f m/s",
                c.pos.x, c.pos.y, c.pos.z, c.vel.len());

            // ── Genetic distance from species centroid ────────────────────────
            // Shows how diverged this creature's genome is from its species average.
            // At 100%, a newly born creature with this genome would form a new species.
            ImGui::Separator();
            if (sp) {
                float rawDist  = c.genome.distanceTo(sp->centroid);
                float epsilon  = world.cfg.speciesEpsilon;
                float distPct  = rawDist / epsilon;          // 0 = identical, 1 = speciation threshold
                float barVal   = std::min(distPct, 1.f);

                ImVec4 barCol;
                if (distPct < 0.5f)       barCol = {0.2f, 0.8f, 0.2f, 1.f};   // green: well within species
                else if (distPct < 0.85f)  barCol = {0.9f, 0.7f, 0.1f, 1.f};   // yellow: diverging
                else                       barCol = {1.0f, 0.2f, 0.2f, 1.f};   // red: near speciation

                char overlayBuf[32];
                std::snprintf(overlayBuf, sizeof(overlayBuf), "%.1f%%", distPct * 100.f);

                ImGui::Text("Genetic Distance:");
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, barCol);
                ImGui::ProgressBar(barVal, ImVec2(160.f, 0.f), overlayBuf);
                ImGui::PopStyleColor();

                if (distPct >= 1.f)
                    ImGui::TextColored({1.f,0.3f,0.3f,1.f},
                        "  ⚠ Genome has diverged beyond species threshold!");
                else
                    ImGui::TextDisabled("  (%.3f / %.3f epsilon)", rawDist, epsilon);
            }
            ImGui::Separator();

            // Behaviour
            const char* bhNames[] = {"Idle","SeekFood","SeekWater","Sleep",
                                     "SeekMate","Flee","Hunt","Mating","Healing"};
            ImGui::Text("Behavior: %s", bhNames[(int)c.behavior]);

            ImGui::Separator();
            ImGui::Text("Needs:");
            if (ImGui::BeginTable("NeedsTable", 3, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn("Drive", ImGuiTableColumnFlags_WidthFixed, 60.f);
                ImGui::TableSetupColumn("Want", ImGuiTableColumnFlags_None);
                ImGui::TableSetupColumn("Need", ImGuiTableColumnFlags_None);
                ImGui::TableHeadersRow();

                for (int i = 0; i < DRIVE_COUNT; i++) {
                    ImGui::TableNextRow();
                    float lvl = c.needs.urgency[i];
                    float des = c.needs.desireMult[i];
                    float want = lvl * des;

                    // Smooth transition from Green (0%)  to Yellow (33%) to Red (66%) to Black (100%)
                    constexpr ImVec4 col_green  = {0.1f, 1.0f, 0.1f, 1.0f};
                    constexpr ImVec4 col_yellow = {1.0f, 1.0f, 0.1f, 1.0f};
                    constexpr ImVec4 col_red    = {1.0f, 0.1f, 0.1f, 1.0f};
                    constexpr ImVec4 col_black  = {0.1f, 0.1f, 0.1f, 1.0f};

                    ImVec4 col;
                    if (lvl < 0.33f) { col = lerp_im_vec4(col_green, col_yellow, lvl / 0.33f); }
                    else if (lvl < 0.66f) { col = lerp_im_vec4(col_yellow, col_red, (lvl - 0.33f) / 0.33f); }
                    else { col = lerp_im_vec4(col_red, col_black, (lvl - 0.66f) / 0.34f); }

                    ImGui::TableSetColumnIndex(0);
                    ImGui::Text("%s", driveName((Drive)i));

                    ImGui::TableSetColumnIndex(1);
                    char wantBuf[32];
                    std::snprintf(wantBuf, sizeof(wantBuf), "%.2f", want);
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(0.2f, 0.6f, 1.0f, 1.f));
                    ImGui::ProgressBar(std::min(want / 5.0f, 1.0f), ImVec2(-FLT_MIN, 0), wantBuf);
                    ImGui::PopStyleColor();

                    ImGui::TableSetColumnIndex(2);
                    char needBuf[32];
                    std::snprintf(needBuf, sizeof(needBuf), "%d%%", (int)(lvl * 100));
                    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, col);
                    ImGui::ProgressBar(lvl, ImVec2(-FLT_MIN, 0), needBuf);
                    ImGui::PopStyleColor();
                }
                ImGui::EndTable();
            }

            ImGui::Separator();
            ImGui::Text("Genome (raw [0,1]):");
            const char* gNames[] = {
                "BodySize","MaxSpeed","MaxSlope","VisionRange","VisionFOV",
                "HerbEff","CarnEff","HungerRate","ThirstRate","SleepRate",
                "LibidoRate","FearSens","SocialRate","TerritRate",
                "DesireHealth","DesireHunger","DesireThirst","DesireSleep",
                "DesireLibido","DesireFear","DesireSocial",
                "GestTime","LitterBias","MutRate","MutStd","Hue","Pattern"
            };
            for (int i = 0; i < GENOME_SIZE; i++) {
                int count_lower = 0;
                int count_greater = 0;
                int total = 0;
                float my_val = c.genome.raw[i];
                for (const auto& other : world.creatures) {
                    if (!other.alive) continue;
                    total++;
                    if (other.genome.raw[i] < my_val) count_lower++;
                    else if (other.genome.raw[i] > my_val) count_greater++;
                }

                const char *term = get_term_from_term(total, count_lower, count_greater);
                ImVec4 color = get_color_from_term(term);

                ImGui::ProgressBar(c.genome.raw[i], ImVec2(120, 12), "");
                ImGui::SameLine();
                ImGui::Text("%s  %.3f ", gNames[i], c.genome.raw[i]);
                ImGui::SameLine();
                ImGui::TextColored(color, "(%s)", term);
            }
        }
    }

    ImGui::End();
}

// ── Species panel ─────────────────────────────────────────────────────────────
void SimUI::drawSpeciesPanel(const World& world) {
    if (!ImGui::Begin("Species", &showSpecies)) { ImGui::End(); return; }

    // Count active
    int activeSp = 0;
    for (const auto& sp : world.species)
        if (sp.count > 0) activeSp++;
    ImGui::Text("%d active species", activeSp);

    if (ImGui::BeginTable("SpeciesTable", 5,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
        ImVec2(0, 300))) {

        ImGui::TableSetupColumn("Name");
        ImGui::TableSetupColumn("Count");
        ImGui::TableSetupColumn("All-time");
        ImGui::TableSetupColumn("AvgSpeed");
        ImGui::TableSetupColumn("Diet");
        ImGui::TableHeadersRow();

        for (const auto& sp : world.species) {
            if (sp.count == 0) continue;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored({sp.color[0], sp.color[1], sp.color[2], 1.f},
                               "%s", sp.name.c_str());
            ImGui::TableSetColumnIndex(1); ImGui::Text("%d", sp.count);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%d", sp.allTime);
            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%.1f", sp.centroid.maxSpeed());
            ImGui::TableSetColumnIndex(4);
            bool isHerb = sp.centroid.herbEfficiency() > 0.5f;
            bool isCarn = sp.centroid.carnEfficiency() > 0.5f;
            ImGui::Text("%s", isHerb && isCarn ? "Omni" : isHerb ? "Herb" : "Carn");
        }
        ImGui::EndTable();
    }

    ImGui::End();
}

// ── Gene charts ───────────────────────────────────────────────────────────────
void SimUI::drawGeneCharts(const World& world, const DataRecorder& rec) {
    if (!ImGui::Begin("Gene Evolution", &showGeneCharts)) { ImGui::End(); return; }
    int n = rec.size();

    if (n > 1 && ImPlot::BeginPlot("Average Traits Over Time", ImVec2(-1, 200))) {
        ImPlot::SetupAxes("Time (s)", "Value");
        ImPlot::PlotLine("Avg Speed",   rec.t_buf.data(), rec.speed_buf.data(),   n);
        ImPlot::PlotLine("Avg Size",    rec.t_buf.data(), rec.size_buf.data(),    n);
        ImPlot::PlotLine("Herb Eff",    rec.t_buf.data(), rec.herbEff_buf.data(), n);
        ImPlot::PlotLine("Carn Eff",    rec.t_buf.data(), rec.carnEff_buf.data(), n);
        ImPlot::EndPlot();
    }

    ImGui::Text("Gene Histogram:");
    const char* geneNames[] = {
        "BodySize","MaxSpeed","MaxSlope","VisionRange","VisionFOV",
        "HerbEff","CarnEff","HungerRate","ThirstRate","SleepRate",
        "LibidoRate","FearSens","SocialRate","TerritRate",
        "GestTime","LitterBias","MutRate","MutStd","Hue","Pattern"
    };
    ImGui::Combo("Gene", &chartGeneIdx, geneNames, GENOME_SIZE);

    rec.geneHistogram(world, (GeneIdx)chartGeneIdx, 20, histX, histY);
    if (!histX.empty() && ImPlot::BeginPlot("##GeneHist", ImVec2(-1, 160))) {
        ImPlot::SetupAxes("Gene value [0,1]", "Count");
        ImPlot::PlotBars("##bars", histX.data(), histY.data(),
                         (int)histX.size(), 0.04f);
        ImPlot::EndPlot();
    }

    ImGui::End();
}

// ── Player panel ──────────────────────────────────────────────────────────────
void SimUI::drawPlayerPanel(World& world, Renderer& rend) {
    if (!ImGui::Begin("Player Mode", &showPlayerPanel)) { ImGui::End(); return; }

    if (rend.playerID == INVALID_ID) {
        ImGui::TextWrapped("Select a creature in the inspector, then possess it.");
        ImGui::TextDisabled("Or press P to possess a random creature.");
        if (selectedID != INVALID_ID) {
            if (ImGui::Button("Possess Selected")) {
                rend.playerID     = selectedID;
                rend.showFogOfWar = true;
            }
        }
    } else {
        auto it = world.idToIndex.find(rend.playerID);
        if (it == world.idToIndex.end()) {
            ImGui::TextDisabled("Controlled creature died.");
            rend.playerID     = INVALID_ID;
            rend.showFogOfWar = false;
        } else {
            const Creature& c = world.creatures[it->second];
            ImGui::Text("Controlling: #%u", rend.playerID);
            ImGui::Text("Energy: %.1f   Age: %.1fs", c.energy, c.age);
            ImGui::Text("Active Drive: %s",
                        driveName(c.needs.activeDrive()));

            ImGui::Checkbox("Fog of War", &rend.showFogOfWar);
            ImGui::SliderFloat("Fog Radius", &rend.fogRadius, 500.f, 8000.f);
            ImGui::Checkbox("Lock Yaw Follow", &rend.lockYawFollow);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("When enabled, following a creature\nwon't rotate the camera.");

            if (ImGui::Button("Release (back to free cam)")) {
                rend.playerID     = INVALID_ID;
                rend.showFogOfWar = false;
            }
        }
    }

    ImGui::End();
}

// ── Settings window ───────────────────────────────────────────────────────────
void SimUI::drawSettingsWindow(World& world, Renderer& rend) {
    if (!ImGui::Begin("Settings", &showSettings,
                      ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    bool changed = false;   // set to true by any widget that was edited this frame

    // Convenience macros so every slider only needs one extra line.
    // IsItemDeactivatedAfterEdit() fires once when the user releases the slider,
    // not every frame while dragging — avoids hammering the filesystem.
#define SLIDER_F(label, var, lo, hi) \
    ImGui::SliderFloat(label, &(var), lo, hi); \
    changed |= ImGui::IsItemDeactivatedAfterEdit();

#define SLIDER_I(label, var, lo, hi) \
    ImGui::SliderInt(label, &(var), lo, hi); \
    changed |= ImGui::IsItemDeactivatedAfterEdit();

#define CHECK(label, var) \
    changed |= ImGui::Checkbox(label, &(var));

    // ── Simulation ────────────────────────────────────────────────────────────
    ImGui::SeparatorText("Simulation");
    SLIDER_F("Sim Speed##s",           world.cfg.simSpeed,           0.1f, 20.f)
    ImGui::TextDisabled("(- / + keys also adjust speed)");
    SLIDER_F("Mutation Rate Scale##s", world.cfg.mutationRateScale,  0.1f,  5.f)
    SLIDER_F("Species Epsilon##s",     world.cfg.speciesEpsilon,     0.05f, 0.5f)
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Genetic distance threshold for new species.\n"
                          "A newborn whose genome differs by more\n"
                          "than this from all species centroids\n"
                          "will trigger a speciation event (100%%).");
    SLIDER_F("Plant Grow Rate##s",     world.cfg.plantGrowRate,      0.f,   5.f)
    SLIDER_I("Max Population##s",      world.cfg.maxPopulation,      100, Renderer::MAX_CREATURES)

    // ── Camera ────────────────────────────────────────────────────────────────
    ImGui::SeparatorText("Camera");
    SLIDER_F("FOV##s",              rend.camera.fovY,               30.f,   120.f)
    SLIDER_F("Move Speed##s",       rend.camera.translation_speed,  1000.f, 40000.f)
    SLIDER_F("Follow Distance##s",  rend.camera.follow_dist,        200.f,  4000.f)
    SLIDER_F("Follow Speed##s",     rend.camera.follow_speed,       1.f,    20.f)
    CHECK("Lock Yaw When Following##s", rend.lockYawFollow)

    ImGui::SeparatorText("Rendering");
    CHECK("Show FOV Cone##s", rend.showFOVCone)
    CHECK("Wireframe##s",     rend.wireframe)
    CHECK("Fog of War##s",    rend.showFogOfWar)
    SLIDER_F("Fog Radius##s", rend.fogRadius, 500.f, 8000.f)

#undef SLIDER_F
#undef SLIDER_I
#undef CHECK

    // ── Hotkeys (read-only) ───────────────────────────────────────────────────
    ImGui::SeparatorText("Hotkeys");
    ImGui::TextDisabled("Space    – Pause / Resume");
    ImGui::TextDisabled("- / +    – Decrease / Increase sim speed (1.25×)");
    ImGui::TextDisabled("P        – Possess random creature");
    ImGui::TextDisabled("T        – Toggle possession of selected");
    ImGui::TextDisabled("J        – Toggle hide outside FOV");
    ImGui::TextDisabled("RMB drag – Rotate camera");
    ImGui::TextDisabled("W/S/A/D  – Move camera (fwd/back/left/right)");
    ImGui::TextDisabled("F/Q      – Move camera up/down");

    // ── Save / Load ───────────────────────────────────────────────────────────
    ImGui::SeparatorText("Save / Load");

    ImGui::InputText("Path##sjson", settingsPathBuf, sizeof(settingsPathBuf));
    ImGui::SameLine(); if (ImGui::Button("Load"))      loadSettingsFromFile(settingsPathBuf, world, rend);
    ImGui::SameLine(); if (ImGui::Button("Save"))      saveSettingsToFile(settingsPathBuf, world, rend);

    // Show a brief "Saved!" confirmation for 2 seconds after any auto-save
    static float savedMsgTimer = 0.f;
    savedMsgTimer -= ImGui::GetIO().DeltaTime;
    if (savedMsgTimer > 0.f)
        ImGui::TextColored({0.3f, 1.f, 0.4f, 1.f}, "Auto-saved to %s", settingsPathBuf);
    else
        ImGui::TextColored({0.3f, 1.f, 0.4f, 1.f}, "", settingsPathBuf);

    // ── Auto-save ─────────────────────────────────────────────────────────────
    if (changed) {
        saveSettingsToFile(settingsPathBuf, world, rend);
        savedMsgTimer = 2.f;
    }

    ImGui::End();
}

// ── Settings JSON serialisation ───────────────────────────────────────────────
// Hand-written JSON so we don't need an external library.
void SimUI::saveSettingsToFile(const char* path, const World& world, const Renderer& rend) const {
    std::ofstream f(path);
    if (!f) return;
    f << "{\n";
    // UI Panels
    f << "  \"showPanels\": "       << (showPanels ? "true" : "false") << ",\n";
    f << "  \"showSimControls\": "  << (showSimControls ? "true" : "false") << ",\n";
    f << "  \"showPopStats\": "     << (showPopStats ? "true" : "false") << ",\n";
    f << "  \"showInspector\": "    << (showInspector ? "true" : "false") << ",\n";
    f << "  \"showSpecies\": "      << (showSpecies ? "true" : "false") << ",\n";
    f << "  \"showGeneCharts\": "   << (showGeneCharts ? "true" : "false") << ",\n";
    f << "  \"showPlayerPanel\": "  << (showPlayerPanel ? "true" : "false") << ",\n";
    f << "  \"showPlanetDebug\": "  << (showPlanetDebug ? "true" : "false") << ",\n";
    f << "  \"showSettings\": "     << (showSettings ? "true" : "false") << ",\n";
    // Simulation
    f << "  \"simSpeed\": "             << world.cfg.simSpeed             << ",\n";
    f << "  \"mutationRateScale\": "    << world.cfg.mutationRateScale    << ",\n";
    f << "  \"speciesEpsilon\": "       << world.cfg.speciesEpsilon       << ",\n";
    f << "  \"plantGrowRate\": "        << world.cfg.plantGrowRate        << ",\n";
    f << "  \"maxPopulation\": "        << world.cfg.maxPopulation        << ",\n";
    // Camera
    f << "  \"cameraFOV\": "            << rend.camera.fovY               << ",\n";
    f << "  \"cameraMoveSpeed\": "      << rend.camera.translation_speed  << ",\n";
    f << "  \"followDist\": "           << rend.camera.follow_dist        << ",\n";
    f << "  \"followSpeed\": "          << rend.camera.follow_speed       << ",\n";
    f << "  \"lockYawFollow\": "        << (rend.lockYawFollow ? "true" : "false") << ",\n";
    // Rendering
    f << "  \"showFOVCone\": "          << (rend.showFOVCone ? "true" : "false") << ",\n";
    f << "  \"fogRadius\": "            << rend.fogRadius                  << "\n";
    f << "}\n";
}

void SimUI::loadSettingsFromFile(const char* path, World& world, Renderer& rend) {
    std::ifstream f(path);
    if (!f) return;

    // Parse simple key: value lines from the JSON.
    // Strips whitespace, handles both float/int and bool values.
    auto getVal = [](const std::string& line, std::string& valStr) -> bool {
        size_t colon = line.find(':');
        if (colon == std::string::npos) return false;
        valStr = line.substr(colon + 1);
        // Strip trailing comma and whitespace
        while (!valStr.empty() && (valStr.back() == ',' ||
               valStr.back() == '\r' || valStr.back() == '\n' ||
               valStr.back() == ' ')) valStr.pop_back();
        while (!valStr.empty() && (valStr.front() == ' ')) valStr.erase(valStr.begin());
        return !valStr.empty();
    };

    std::string line;
    while (std::getline(f, line)) {
        std::string val;
        if (!getVal(line, val)) continue;

        auto has = [&](const char* key){ return line.find(key) != std::string::npos; };
        bool bval = (val == "true");

        try {
            if      (has("\"showPanels\""))         showPanels                    = bval;
            else if (has("\"showSimControls\""))    showSimControls               = bval;
            else if (has("\"showPopStats\""))       showPopStats                  = bval;
            else if (has("\"showInspector\""))      showInspector                 = bval;
            else if (has("\"showSpecies\""))        showSpecies                   = bval;
            else if (has("\"showGeneCharts\""))     showGeneCharts                = bval;
            else if (has("\"showPlayerPanel\""))    showPlayerPanel               = bval;
            else if (has("\"showPlanetDebug\""))    showPlanetDebug               = bval;
            else if (has("\"showSettings\""))       showSettings                  = bval;
            else if (has("\"simSpeed\""))           world.cfg.simSpeed            = std::stof(val);
            else if (has("\"mutationRateScale\""))  world.cfg.mutationRateScale   = std::stof(val);
            else if (has("\"speciesEpsilon\""))     world.cfg.speciesEpsilon      = std::stof(val);
            else if (has("\"plantGrowRate\""))      world.cfg.plantGrowRate       = std::stof(val);
            else if (has("\"maxPopulation\""))      world.cfg.maxPopulation       = std::stoi(val);
            else if (has("\"cameraFOV\""))          rend.camera.fovY              = std::stof(val);
            else if (has("\"cameraMoveSpeed\""))    rend.camera.translation_speed = std::stof(val);
            else if (has("\"followDist\""))         rend.camera.follow_dist       = std::stof(val);
            else if (has("\"followSpeed\""))        rend.camera.follow_speed      = std::stof(val);
            else if (has("\"lockYawFollow\""))      rend.lockYawFollow            = bval;
            else if (has("\"showFOVCone\""))        rend.showFOVCone              = bval;
            else if (has("\"fogRadius\""))          rend.fogRadius                = std::stof(val);
        } catch (...) { /* skip malformed lines */ }
    }
}
