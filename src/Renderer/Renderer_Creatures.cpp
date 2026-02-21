#include "Renderer.hpp"
#include <cmath>
#include <algorithm>

#include "World/World.hpp"
#include "World/World_Planet.hpp"

// ── Renderer_Creatures.cpp ────────────────────────────────────────────────────
// Covers: hueToRGB, renderCreatures.
// All creatures are drawn in a single instanced draw call.

// ── hueToRGB ──────────────────────────────────────────────────────────────────
// Converts a hue angle (0–360°) to an RGB colour.
// The colour wheel is split into 6 sectors of 60°. At each sector boundary you
// get a pure primary/secondary colour; between boundaries two components blend.
// Output is remapped from [0,1] to [0.3, 1.0] so nothing is too dark to see.
static void hueToRGB(float hue, float out[3]) {
    static const float rgb6[6][3] = {
        {1,0,0},{1,1,0},{0,1,0},   // red, yellow, green
        {0,1,1},{0,0,1},{1,0,1},   // cyan, blue, magenta
    };
    hue          = std::fmod(hue, 360.f);
    float sector = hue / 60.f;
    int   hi     = (int)sector % 6;
    float f      = sector - (int)sector;    // fractional position within this sector
    int   hi2    = (hi + 1) % 6;
    for (int i = 0; i < 3; i++)
        out[i] = 0.3f + 0.7f * (rgb6[hi][i] * (1.f-f) + rgb6[hi2][i] * f);
}

// ── isVisibleFromCamera ───────────────────────────────────────────────────────
// A surface point P is visible from camera C if and only if the angle ∠POC
// (at the planet centre O) is less than the geometric horizon angle ∠TOC,
// where T is the tangent point from C to the sphere.
//
// In the right triangle OTC (right angle at T):
//   cos(∠TOC) = OT / OC  =  R / camDist
//
// So P is visible when:
//   dot( (P-O)/|P-O|,  (C-O)/|C-O| )  >  R / |C-O|
//
// We add a small negative bias (-0.02) to cull a thin band just before the
// true horizon, which avoids pop-in on entities at the extreme edge of sight.
static bool isVisibleFromCamera(const Vec3& worldPos, const Float3& camPos) {
    const Vec3& pc = g_planet_surface.center;

    // Direction from planet centre to the entity (= outward surface normal)
    Vec3 toEntity = (worldPos - pc).normalised();

    Vec3 toCamVec = {
        camPos.x - pc.x,
        camPos.y - pc.y,
        camPos.z - pc.z
    };
    float camDist = toCamVec.len();
    if (camDist < 1e-3f) return true;

    Vec3 toCam = toCamVec * (1.f / camDist);
    float dotVal = toEntity.dot(toCam);

    // Correct geometric horizon threshold: cos(horizon angle at planet centre)
    float R = g_planet_surface.radius;
    float horizonDot = (camDist > R) ? (R / camDist) : 1.f;

    // Small margin so entities right at the edge don't flicker
    return dotVal > horizonDot - 0.02f;
}

// ── renderCreatures ───────────────────────────────────────────────────────────
// Fills the instance buffer (one entry per living creature) then issues a single
// DrawInstanced call that renders all of them as camera-facing billboards.
//
// How instancing works:
//   - creatureQuadVB has 4 vertices (the corners of one billboard)
//   - creatureInstanceVB has N rows (one per creature) with position/colour/size
//   - The GPU runs the vertex shader 4*N times, feeding each group of 4 vertices
//     the same instance row, producing one billboard per creature.
void Renderer::renderCreatures(const World& world) {
    // Lock the instance buffer so the CPU can write new creature data into it.
    // MAP_WRITE_DISCARD = discard old contents (no GPU sync needed).
    D3D11_MAPPED_SUBRESOURCE ms{};
    ctx->Map(creatureInstanceVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    auto* inst  = (CreatureInstance*)ms.pData;
    int   count = 0;

    for (const auto& c : world.creatures) {
        if (!c.alive || count >= MAX_CREATURES) continue;

        // Cull creatures on the far side of the planet
        if (!isVisibleFromCamera(c.pos, camera.pos)) {
            continue;
        }

        float rgb[3]; hueToRGB(c.genome.hue(), rgb);
        bool isSelected = (c.id == selectedID || c.id == playerID);

        // Lift the billboard centre above the terrain so it sits visually on top.
        float bSize    = std::max(1.5f, c.genome.bodySize() * 2.0f);
        inst[count].pos[0] = c.pos.x;
        inst[count].pos[1] = c.pos.y + bSize * 0.5f;
        inst[count].pos[2] = c.pos.z;
        inst[count].yaw    = c.yaw;

        if (isSelected) {
            inst[count].color[0] = std::min(1.f, rgb[0] * 1.4f + 0.2f);
            inst[count].color[1] = std::min(1.f, rgb[1] * 1.4f + 0.2f);
            inst[count].color[2] = std::min(1.f, rgb[2] * 1.4f + 0.2f);
            inst[count].color[3] = 1.0f;
            inst[count].size     = bSize * 1.35f;  // slightly larger when selected
        } else {
            inst[count].color[0] = rgb[0];
            inst[count].color[1] = rgb[1];
            inst[count].color[2] = rgb[2];
            inst[count].color[3] = 0.95f;
            inst[count].size     = bSize;
        }
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

    ctx->OMSetDepthStencilState(dssDepth, 0);  // depth test + write (creatures occlude each other)
    float bf[4] = {};
    ctx->OMSetBlendState(bsAlpha, bf, 0xFFFFFFFF);

    // Bind two vertex buffer slots simultaneously:
    //   slot 0: creatureQuadVB — 4 quad corners, advances per vertex
    //   slot 1: creatureInstanceVB — per-creature data, advances per instance
    UINT strides[2] = { sizeof(float)*2, sizeof(CreatureInstance) };
    UINT offsets[2] = { 0, 0 };
    ID3D11Buffer* vbs[2] = { creatureQuadVB, creatureInstanceVB };
    ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);

    // 4 vertices per billboard × count creatures = all creatures in one draw call
    ctx->DrawInstanced(4, (UINT)count, 0, 0);
    ctx->OMSetBlendState(nullptr, bf, 0xFFFFFFFF);
}
