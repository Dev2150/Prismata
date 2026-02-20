#include "SimUI.hpp"
#include "imgui.h"
#include "implot.h"
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <string>
#include <cmath>

// Helper: hue → ImVec4 colour
static ImVec4 hueColor(float hue, float alpha = 1.f) {
    float h = hue / 60.f;
    int   hi = (int)h;
    float f  = h - hi;
    float p  = 0.3f, q = 1.f - 0.7f * f, tv = 0.3f + 0.7f * f;
    float rgb[6][3] = {{1,tv,p},{q,1,p},{p,1,tv},{p,q,1},{tv,p,1},{1,p,q}};
    return {rgb[hi%6][0], rgb[hi%6][1], rgb[hi%6][2], alpha};
}

// ── Top-level draw ────────────────────────────────────────────────────────────
void SimUI::draw(World& world, DataRecorder& rec, Renderer& rend) {
    // Update hover each frame before drawing anything
    updateTerrainHover(rend, world);

    drawMainMenuBar(world, rec, rend);
    drawSimControls(world, rend);
    drawPopStats(world, rec);
    drawEntityInspector(world);
    drawSpeciesPanel(world);
    drawGeneCharts(world, rec);
    drawPlayerPanel(world, rend);

    if (showSettings) drawSettingsWindow(world, rend);

    drawTerrainHoverTooltip();

    if (showDemoWindow) {
        ImGui::ShowDemoWindow(&showDemoWindow);
        ImPlot::ShowDemoWindow();
    }
}

// ── Terrain hover ─────────────────────────────────────────────────────────────
void SimUI::updateTerrainHover(const Renderer& rend, const World& world) {
    terrainHitValid = false;
    // Only raycast when mouse is over the 3D viewport (not over any ImGui window)
    if (ImGui::GetIO().WantCaptureMouse) return;

    ImVec2 mp = ImGui::GetIO().MousePos;
    Vec3 pos; uint8_t mat;
    if (rend.screenToTerrain(mp.x, mp.y, (float)windowW, (float)windowH, world, pos, mat)) {
        terrainHitValid = true;
        terrainHitPos   = pos;
        terrainHitMat   = mat;
    }
}

void SimUI::drawTerrainHoverTooltip() {
    if (!terrainHitValid) return;

    ImGui::SetNextWindowPos(
        ImVec2(ImGui::GetIO().MousePos.x + 16.f, ImGui::GetIO().MousePos.y + 8.f),
        ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.78f);
    ImGui::SetNextWindowSize(ImVec2(0, 0));   // auto-size
    ImGui::Begin("##TerrainHover",
                 nullptr,
                 ImGuiWindowFlags_NoDecoration |
                 ImGuiWindowFlags_NoInputs     |
                 ImGuiWindowFlags_NoNav         |
                 ImGuiWindowFlags_NoMove        |
                 ImGuiWindowFlags_AlwaysAutoResize);

    // Material colour swatch + name
    static const ImVec4 matColors[] = {
        {0.25f,0.55f,0.15f,1}, // Grass – green
        {0.50f,0.50f,0.50f,1}, // Rock  – grey
        {0.70f,0.60f,0.40f,1}, // Sand  – tan
        {0.08f,0.35f,0.72f,1}, // Water – blue
        {0.90f,0.95f,1.00f,1}, // Snow  – white
    };
    uint8_t m = std::min(terrainHitMat, (uint8_t)4);
    ImGui::TextColored(matColors[m], "  %s", World::materialName(terrainHitMat));
    ImGui::Text("Height : %.2f m", terrainHitPos.y);
    ImGui::Text("Pos    : (%.1f, %.1f)", terrainHitPos.x, terrainHitPos.z);

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
        ImGui::Checkbox("Wireframe",   &rend.wireframe);
        ImGui::Checkbox("Water Plane", &rend.showWater);
        ImGui::Checkbox("FOV Cone",    &rend.showFOVCone);
        ImGui::Separator();
        ImGui::Checkbox("ImGui Demo",  &showDemoWindow);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Settings")) {
        if (ImGui::MenuItem("Open Settings")) showSettings = true;
        ImGui::EndMenu();
    }

    // Pause indicator (Space to toggle hint)
    if (world.cfg.paused)
        ImGui::TextColored({1.f,0.4f,0.1f,1.f}, "  ⏸ PAUSED (Space)");
    else
        ImGui::Text("  ▶");

    ImGui::Text("  |  t=%.1fs  Pop=%d  Species=%d",
        world.simTime,
        (int)world.creatures.size(),
        (int)std::count_if(world.species.begin(), world.species.end(),
                           [](const SpeciesInfo& s){ return s.count > 0; }));

    ImGui::EndMainMenuBar();
}

// ── Sim controls ──────────────────────────────────────────────────────────────
void SimUI::drawSimControls(World& world, Renderer& rend) {
    ImGui::Begin("Simulation Controls");

    // Pause / play buttons
    if (world.cfg.paused) {
        if (ImGui::Button("▶ Play (Space)"))  world.cfg.paused = false;
    } else {
        if (ImGui::Button("⏸ Pause (Space)")) world.cfg.paused = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) world.reset();

    ImGui::Separator();

    ImGui::SliderFloat("Sim Speed",     &world.cfg.simSpeed,     0.1f, 20.f);
    ImGui::SliderFloat("Mutation Scale",&world.cfg.mutationRateScale, 0.1f, 5.f);
    ImGui::SliderFloat("Species ε",     &world.cfg.speciesEpsilon,   0.05f, 0.5f);
    ImGui::SliderFloat("Plant Grow Rate",&world.cfg.plantGrowRate,   0.f, 5.f);
    ImGui::SliderInt  ("Max Population",&world.cfg.maxPopulation, 100, 5000);

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
            float px = globalRNG().range(2.f, (float)(world.worldCX * CHUNK_SIZE - 2));
            float pz = globalRNG().range(2.f, (float)(world.worldCZ * CHUNK_SIZE - 2));
            world.spawnCreature(Genome::randomHerbivore(globalRNG()),
                                world.snapToSurface(px, pz));
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Spawn Carnivores")) {
        for (int i = 0; i < nCarn; i++) {
            float px = globalRNG().range(2.f, (float)(world.worldCX * CHUNK_SIZE - 2));
            float pz = globalRNG().range(2.f, (float)(world.worldCZ * CHUNK_SIZE - 2));
            world.spawnCreature(Genome::randomCarnivore(globalRNG()),
                                world.snapToSurface(px, pz));
        }
    }

    ImGui::End();
}

// ── Population stats ──────────────────────────────────────────────────────────
void SimUI::drawPopStats(const World& world, const DataRecorder& rec) {
    ImGui::Begin("Population Statistics");
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

// ── Entity inspector ──────────────────────────────────────────────────────────
void SimUI::drawEntityInspector(const World& world) {
    ImGui::Begin("Entity Inspector");

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
            ImGui::Text("Age: %.1f / %.1f s   Energy: %.1f / %.1f",
                c.age, c.lifespan, c.energy, c.maxEnergy);
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
                                     "SeekMate","Flee","Hunt","Mating"};
            ImGui::Text("Behavior: %s", bhNames[(int)c.behavior]);

            ImGui::Separator();
            ImGui::Text("Needs:");
            for (int i = 0; i < DRIVE_COUNT; i++) {
                float lvl = c.needs.level[i];
                ImVec4 col = {lvl, 1.f - lvl, 0.2f, 1.f};
                ImGui::TextColored(col, "  %-10s %.2f", driveName((Drive)i), lvl);
                ImGui::SameLine();
                ImGui::ProgressBar(lvl, ImVec2(120, 0));
            }

            ImGui::Separator();
            ImGui::Text("Genome (raw [0,1]):");
            const char* gNames[] = {
                "BodySize","MaxSpeed","MaxSlope","VisionRange","VisionFOV",
                "HerbEff","CarnEff","HungerRate","ThirstRate","SleepRate",
                "LibidoRate","FearSens","SocialRate","TerritRate",
                "GestTime","LitterBias","MutRate","MutStd","Hue","Pattern"
            };
            for (int i = 0; i < GENOME_SIZE; i++) {
                ImGui::ProgressBar(c.genome.raw[i], ImVec2(120, 12), "");
                ImGui::SameLine();
                ImGui::Text("%s  %.3f", gNames[i], c.genome.raw[i]);
            }

            ImGui::Separator();
            ImGui::Text("Parents: %u, %u",  c.parentA, c.parentB);
        }
    }

    ImGui::End();
}

// ── Species panel ─────────────────────────────────────────────────────────────
void SimUI::drawSpeciesPanel(const World& world) {
    ImGui::Begin("Species");

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
    ImGui::Begin("Gene Evolution");
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
    ImGui::Begin("Player Mode");

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
            ImGui::SliderFloat("Fog Radius", &rend.fogRadius, 5.f, 80.f);
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

    ImGui::SeparatorText("Simulation");
    ImGui::SliderFloat("Sim Speed##s",          &world.cfg.simSpeed,           0.1f, 20.f);
    ImGui::SliderFloat("Mutation Rate Scale##s",&world.cfg.mutationRateScale,  0.1f,  5.f);
    ImGui::SliderFloat("Species Epsilon##s",    &world.cfg.speciesEpsilon,     0.05f, 0.5f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Genetic distance threshold for new species.\n"
                          "A newborn whose genome differs by more\n"
                          "than this from all species centroids\n"
                          "will trigger a speciation event (100%%).");
    ImGui::SliderFloat("Plant Grow Rate##s",    &world.cfg.plantGrowRate,      0.f,   5.f);
    ImGui::SliderInt  ("Max Population##s",     &world.cfg.maxPopulation,      100, 5000);

    ImGui::SeparatorText("Camera");
    ImGui::SliderFloat("FOV##s",             &rend.camera.fovY,                30.f, 120.f);
    ImGui::SliderFloat("Move Speed##s",      &rend.camera.translation_speed,   10.f, 400.f);
    ImGui::SliderFloat("Follow Distance##s", &rend.camera.follow_dist,          2.f,  40.f);
    ImGui::SliderFloat("Follow Speed##s",    &rend.camera.follow_speed,         1.f,  20.f);
    ImGui::Checkbox("Lock Yaw When Following##s", &rend.lockYawFollow);

    ImGui::SeparatorText("Rendering");
    ImGui::Checkbox("Show Water##s",    &rend.showWater);
    ImGui::SliderFloat("Water Level##s",&rend.waterLevel, 0.1f, 3.f);
    if (ImGui::IsItemEdited()) {
        // Rebuild water mesh at new height next frame
        rend.waterBuilt = false;
    }
    ImGui::Checkbox("Show FOV Cone##s",&rend.showFOVCone);
    ImGui::Checkbox("Wireframe##s",    &rend.wireframe);
    ImGui::Checkbox("Fog of War##s",   &rend.showFogOfWar);
    ImGui::SliderFloat("Fog Radius##s",&rend.fogRadius, 5.f, 80.f);

    ImGui::SeparatorText("Hotkeys");
    ImGui::TextDisabled("Space    – Pause / Resume");
    ImGui::TextDisabled("P        – Possess random creature");
    ImGui::TextDisabled("RMB drag – Rotate camera");
    ImGui::TextDisabled("W/S/A/R  – Move camera (fwd/back/left/right)");
    ImGui::TextDisabled("F/Q      – Move camera up/down");

    ImGui::SeparatorText("Save / Load");
    ImGui::InputText("Path##sjson", settingsPathBuf, sizeof(settingsPathBuf));
    if (ImGui::Button("Save Settings")) saveSettingsToFile(settingsPathBuf, world, rend);
    ImGui::SameLine();
    if (ImGui::Button("Load Settings")) loadSettingsFromFile(settingsPathBuf, world, rend);

    ImGui::End();
}

// ── Settings JSON serialisation ───────────────────────────────────────────────
// Hand-written JSON so we don't need an external library.
void SimUI::saveSettingsToFile(const char* path, const World& world, const Renderer& rend) const {
    std::ofstream f(path);
    if (!f) return;
    f << "{\n";
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
    f << "  \"showWater\": "            << (rend.showWater   ? "true" : "false") << ",\n";
    f << "  \"waterLevel\": "           << rend.waterLevel                 << ",\n";
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
            if      (has("\"simSpeed\""))          world.cfg.simSpeed            = std::stof(val);
            else if (has("\"mutationRateScale\""))  world.cfg.mutationRateScale   = std::stof(val);
            else if (has("\"speciesEpsilon\""))     world.cfg.speciesEpsilon      = std::stof(val);
            else if (has("\"plantGrowRate\""))      world.cfg.plantGrowRate       = std::stof(val);
            else if (has("\"maxPopulation\""))      world.cfg.maxPopulation       = std::stoi(val);
            else if (has("\"cameraFOV\""))          rend.camera.fovY              = std::stof(val);
            else if (has("\"cameraMoveSpeed\""))    rend.camera.translation_speed = std::stof(val);
            else if (has("\"followDist\""))         rend.camera.follow_dist       = std::stof(val);
            else if (has("\"followSpeed\""))        rend.camera.follow_speed      = std::stof(val);
            else if (has("\"lockYawFollow\""))      rend.lockYawFollow            = bval;
            else if (has("\"showWater\""))          rend.showWater                = bval;
            else if (has("\"waterLevel\""))       { rend.waterLevel               = std::stof(val);
                                                    rend.waterBuilt               = false; }
            else if (has("\"showFOVCone\""))        rend.showFOVCone              = bval;
            else if (has("\"fogRadius\""))          rend.fogRadius                = std::stof(val);
        } catch (...) { /* skip malformed lines */ }
    }
}
