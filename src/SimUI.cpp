#include "SimUI.h"
#include "imgui.h"
#include "implot.h"
#include <cstdio>
#include <algorithm>

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
    drawMainMenuBar(world, rec, rend);
    drawSimControls(world, rend);
    drawPopStats(world, rec);
    drawEntityInspector(world);
    drawSpeciesPanel(world);
    drawGeneCharts(world, rec);
    drawPlayerPanel(world, rend);

    if (showDemoWindow) {
        ImGui::ShowDemoWindow(&showDemoWindow);
        ImPlot::ShowDemoWindow();
    }
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
        ImGui::Checkbox("ImGui Demo",  &showDemoWindow);
        ImGui::EndMenu();
    }

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
        if (ImGui::Button("▶ Play"))  world.cfg.paused = false;
    } else {
        if (ImGui::Button("⏸ Pause")) world.cfg.paused = true;
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

            // Behavior
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
            ImGui::Text("Genome (raw values):");
            const char* gNames[] = {
                "BodySize","MaxSpeed","MaxSlope","VisionRange","VisionFOV",
                "HerbEff","CarnEff","HungerRate","ThirstRate","SleepRate",
                "LibidoRate","FearSens","SocialRate","TerritRate",
                "GestTime","LitterBias","MutRate","MutStd","Hue","Pattern"
            };
            for (int i = 0; i < GENOME_SIZE; i++) {
                ImGui::ProgressBar(c.genome.raw[i], ImVec2(120, 12), "");
                ImGui::SameLine();
                ImGui::Text("%s", gNames[i]);
            }

            ImGui::Separator();
            ImGui::Text("Parents: %u, %u   Children: (tracked in lineage)",
                c.parentA, c.parentB);
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

            if (ImGui::Button("Release (back to free cam)")) {
                rend.playerID     = INVALID_ID;
                rend.showFogOfWar = false;
            }
        }
    }

    ImGui::End();
}
