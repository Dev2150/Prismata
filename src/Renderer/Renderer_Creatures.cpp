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
// Returns false if `worldPos` is on the far side of the planet relative to the
// camera. We compare the dot product of the surface normal at `worldPos` with
// the vector from the planet centre to the camera. If the angle between them
// is greater than ~90° (plus a small over-the-horizon margin), the point is
// occluded by the planet body and should be culled.
static bool isVisibleFromCamera(const Vec3& worldPos, const Float3& camPos) {
    const Vec3& pc = g_planet_surface.center;

    // Direction from planet centre to the entity (= outward surface normal)
    Vec3 toEntity = (worldPos - pc).normalised();

    // Direction from planet centre to the camera
    Vec3 toCam = {
        camPos.x - pc.x,
        camPos.y - pc.y,
        camPos.z - pc.z
    };
    float camDist = toCam.len();
    if (camDist < 1e-3f) return true;
    toCam = toCam * (1.f / camDist);

    float dotVal = toEntity.dot(toCam);

    // The geometric horizon angle: cos(asin(R / camDist))
    // Points beyond this are behind the planet limb.
    // We subtract a small bias (0.05) to cull slightly before the true horizon,
    // avoiding pop-in artefacts on entities right at the edge.
    float R = g_planet_surface.radius;
    float sinHorizon = (camDist > R) ? (R / camDist) : 1.f;
    float cosHorizon = -std::sqrt(std::max(0.f, 1.f - sinHorizon * sinHorizon));

    return dotVal > cosHorizon - 0.05f;
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
