#include "Renderer.hpp"
#include <cmath>
#include <vector>
#include <algorithm>

// ── Renderer_Overlays.cpp ─────────────────────────────────────────────────────
// Covers: renderFOVCone.
// Overlay geometry is drawn after opaque objects, with depth-test-only (no write)
// and alpha blending so it appears as a transparent tint over the terrain.

// ── renderFOVCone ─────────────────────────────────────────────────────────────
// Draws a translucent yellow sector on the terrain surface showing the selected
// creature's vision field of view.
//
// The sector is a triangle fan:
//   - centre: the creature's position
//   - arc: FOV_CONE_SEGS points spaced evenly across the FOV angle at visionRange
// Each pair of adjacent arc points forms one triangle with the centre.
// Arc points are snapped to the terrain surface so the cone drapes naturally.
void Renderer::renderFOVCone(const World& world) {
    EntityID id = (selectedID != INVALID_ID) ? selectedID : playerID;
    if (id == INVALID_ID) return;

    auto it = world.idToIndex.find(id);
    if (it == world.idToIndex.end()) return;
    const Creature& c = world.creatures[it->second];
    if (!c.alive) return;

    float range   = c.genome.visionRange();
    float halfFOV = c.genome.visionFOV() * 3.14159265f / 360.f;  // half-angle in radians
    float cx      = c.pos.x;
    float cz      = c.pos.z;
    float cy      = c.pos.y;
    float startAng = c.yaw - halfFOV;
    float endAng   = c.yaw + halfFOV;
    float maxW     = (float)(world.worldCX * CHUNK_SIZE - 1);
    float maxH     = (float)(world.worldCZ * CHUNK_SIZE - 1);

    std::vector<SimpleVertex> verts;
    verts.reserve(FOV_CONE_SEGS * 3);

    for (int i = 0; i < FOV_CONE_SEGS; i++) {
        float a0 = startAng + (endAng - startAng) * (float)i       / FOV_CONE_SEGS;
        float a1 = startAng + (endAng - startAng) * (float)(i + 1) / FOV_CONE_SEGS;

        // Polar → Cartesian: sin/cos convert angle+radius to X/Z offsets
        float x0 = std::clamp(cx + std::sin(a0) * range, 0.f, maxW);
        float z0 = std::clamp(cz + std::cos(a0) * range, 0.f, maxH);
        float x1 = std::clamp(cx + std::sin(a1) * range, 0.f, maxW);
        float z1 = std::clamp(cz + std::cos(a1) * range, 0.f, maxH);

        // Snap arc points to terrain height, lifted slightly so the cone is visible
        float y0 = world.heightAt(x0, z0) + 0.12f;
        float y1 = world.heightAt(x1, z1) + 0.12f;

        verts.push_back({cx, cy + 0.12f, cz});  // centre
        verts.push_back({x0, y0,         z0});   // arc point i
        verts.push_back({x1, y1,         z1});   // arc point i+1
    }

    // Upload CPU-built vertices to the dynamic GPU buffer
    D3D11_MAPPED_SUBRESOURCE ms{};
    ctx->Map(fovConeVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    memcpy(ms.pData, verts.data(),
           std::min(verts.size() * sizeof(SimpleVertex),
                    (size_t)(FOV_CONE_MAX_VERTS * sizeof(SimpleVertex))));
    ctx->Unmap(fovConeVB, 0);

    // rsSolidNoCull: cone must be visible from below (no back-face culling)
    ctx->RSSetState(rsSolidNoCull);
    ctx->IASetInputLayout(simpleLayout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(simpleVS, nullptr, 0);
    ctx->PSSetShader(fovPS,    nullptr, 0);

    float bf[4] = {};
    ctx->OMSetBlendState(bsAlpha, bf, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(dssNoDepthWrite, 0);  // overlay — no depth write

    UINT stride = sizeof(SimpleVertex), offset = 0;
    ctx->IASetVertexBuffers(0, 1, &fovConeVB, &stride, &offset);
    ctx->Draw((UINT)verts.size(), 0);

    ctx->OMSetBlendState(nullptr, bf, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(dssDepth, 0);
    ctx->RSSetState(rsSolid);
}
