// ── Renderer_Plants.cpp ───────────────────────────────────────────────────────
// Renders all alive plants as small instanced billboards, similar to creatures
// but using a different colour and smaller size.
//
// IMPORTANT: This file adds renderPlants() as a free function.
// Call it from Renderer::render() BEFORE renderCreatures so creatures
// draw on top of plants.
//
// To wire this up, add to Renderer.hpp (private section):
//   void renderPlants(const World& world);
// And add one line to Renderer_Frame.cpp::render():
//   renderPlants(world);
//
// Plant billboard colour: biome-mapped green shades driven by plant type:
//   type 0 = grass  → light green
//   type 1 = bush   → mid green
//   type 2 = tree   → dark green / brown-green

#include "Renderer.hpp"
#include "World/World.hpp"
#include "World/World_Planet.hpp"
#include <cmath>
#include <algorithm>

// Plant types → RGBA colour
static const float PLANT_COLORS[3][4] = {
    { 0.45f, 0.78f, 0.25f, 0.90f },   // grass  – bright green
    { 0.22f, 0.60f, 0.18f, 0.92f },   // bush   – mid green
    { 0.15f, 0.42f, 0.12f, 0.95f },   // tree   – dark green
};

static const float PLANT_SIZES[3] = { 0.6f, 1.2f, 2.0f };

// ── plantVisibleFromCamera ────────────────────────────────────────────────────
// Identical geometric test to isVisibleFromCamera in Renderer_Creatures.cpp.
// A surface point P is visible only when the angle ∠POC < arccos(R / camDist).
// Threshold dot product = R / camDist  (cosine of the horizon angle at O).
static bool plantVisibleFromCamera(const Vec3& worldPos, const Float3& camPos) {
    const Vec3& pc = g_planet_surface.center;
    Vec3 toEntity = (worldPos - pc).normalised();
    Vec3 toCamVec = { camPos.x - pc.x, camPos.y - pc.y, camPos.z - pc.z };
    float camDist = toCamVec.len();
    if (camDist < 1e-3f) return true;

    Vec3 toCam   = toCamVec * (1.f / camDist);
    float dotVal = toEntity.dot(toCam);
    float R = g_planet_surface.radius;
    float horizonDot = (camDist > R) ? (R / camDist) : 1.f;

    return dotVal > horizonDot - 0.02f;
}

void Renderer::renderPlants(const World& world) {
    // Re-use the creature instance buffer. We do a separate Map/draw pass.
    D3D11_MAPPED_SUBRESOURCE ms{};
    ctx->Map(creatureInstanceVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    auto* inst  = (CreatureInstance*)ms.pData;
    int   count = 0;

    for (const auto& p : world.plants) {
        if (!p.alive || count >= (int)MAX_CREATURES) continue;

        // Cull plants on the far side of the planet
        if (!plantVisibleFromCamera(p.pos, camera.pos)) continue;

        uint8_t t = std::min((uint8_t)2, p.type);
        float sz  = PLANT_SIZES[t];

        inst[count].pos[0] = p.pos.x;
        inst[count].pos[1] = p.pos.y + sz * 0.5f;   // lift above surface
        inst[count].pos[2] = p.pos.z;
        inst[count].yaw    = 0.f;

        // Tint by remaining nutrition: fully eaten plants look dull/yellow
        float nutFrac = std::min(1.f, p.nutrition / 30.f);
        inst[count].color[0] = PLANT_COLORS[t][0] * (0.5f + 0.5f * nutFrac);
        inst[count].color[1] = PLANT_COLORS[t][1] * (0.6f + 0.4f * nutFrac);
        inst[count].color[2] = PLANT_COLORS[t][2] * nutFrac;
        inst[count].color[3] = PLANT_COLORS[t][3];
        inst[count].size     = sz;
        inst[count].pad[0] = inst[count].pad[1] = inst[count].pad[2] = 0.f;
        count++;
    }
    ctx->Unmap(creatureInstanceVB, 0);

    if (count == 0) return;

    ctx->RSSetState(rsSolid);
    ctx->IASetInputLayout(creatureLayout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(creatureVS, nullptr, 0);
    ctx->PSSetShader(creaturePS, nullptr, 0);
    ctx->OMSetDepthStencilState(dssDepth, 0);

    float bf[4] = {};
    ctx->OMSetBlendState(bsAlpha, bf, 0xFFFFFFFF);

    UINT strides[2] = { sizeof(float)*2, sizeof(CreatureInstance) };
    UINT offsets[2] = { 0, 0 };
    ID3D11Buffer* vbs[2] = { creatureQuadVB, creatureInstanceVB };
    ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);

    ctx->DrawInstanced(4, (UINT)count, 0, 0);
    ctx->OMSetBlendState(nullptr, bf, 0xFFFFFFFF);
}