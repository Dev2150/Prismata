#include "Renderer.hpp"
#include "World/World.hpp"
#include "World/World_Planet.hpp"
#include <cmath>
#include <vector>
#include <algorithm>

#include "World/World.hpp"

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
    float halfFOV = c.genome.visionFOV() * 3.14159265f / 360.f;

    // Build the creature's local tangent frame on the sphere surface.
    Vec3 n = g_planet_surface.normalAt(c.pos);  // outward normal = "up"

    // Forward vector in the tangent plane (from yaw)
    Vec3 rawFwd = {std::sin(c.yaw), 0.f, std::cos(c.yaw)};
    Vec3 fwd = g_planet_surface.projectToTangent(c.pos, rawFwd).normalised();

    // Right vector = normal × forward
    Vec3 right = {
        n.y*fwd.z - n.z*fwd.y,
        n.z*fwd.x - n.x*fwd.z,
        n.x*fwd.y - n.y*fwd.x,
    };
    right = right.normalised();

    float startAng = -halfFOV;
    float endAng   =  halfFOV;

    std::vector<SimpleVertex> verts;
    verts.reserve(FOV_CONE_SEGS * 3);

    // Centre point lifted slightly above the surface along the normal
    Vec3 cen = c.pos + n * 0.15f;

    for (int i = 0; i < FOV_CONE_SEGS; i++) {
        float a0 = startAng + (endAng - startAng) * (float)i       / FOV_CONE_SEGS;
        float a1 = startAng + (endAng - startAng) * (float)(i + 1) / FOV_CONE_SEGS;

        // Arc points in the tangent plane, then walk along the sphere surface
        // by taking `range` metres along the great-circle arc.
        auto arcPoint = [&](float ang) -> Vec3 {
            // Direction in tangent plane
            Vec3 dir = fwd * std::cos(ang) + right * std::sin(ang);
            // Walk `range` metres along the sphere surface starting from c.pos
            // using the planet's snapToSurface to stay on the displaced sphere.
            Vec3 walked = c.pos + dir * range;
            Vec3 snapped = g_planet_surface.snapToSurface(walked);
            return snapped + g_planet_surface.normalAt(snapped) * 0.15f;
        };

        Vec3 p0 = arcPoint(a0);
        Vec3 p1 = arcPoint(a1);

        verts.push_back({cen.x, cen.y, cen.z});
        verts.push_back({p0.x,  p0.y,  p0.z });
        verts.push_back({p1.x,  p1.y,  p1.z });
    }

    if (!ctx.Get()) {
        OutputDebugStringA("CRASH IMMINENT: ctx is null in updateFrameConstants\n");
        return;
    }
    if (!fovConeVB.Get()) {
        OutputDebugStringA("CRASH IMMINENT: fovConeVB is null — createBuffers failed or was never called\n");
        return;
    }

    // Upload CPU-built vertices to the dynamic GPU buffer
    D3D11_MAPPED_SUBRESOURCE ms{};
    ctx->Map(fovConeVB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    memcpy(ms.pData, verts.data(),
           std::min(verts.size() * sizeof(SimpleVertex),
                    (size_t)(FOV_CONE_MAX_VERTS * sizeof(SimpleVertex))));
    ctx->Unmap(fovConeVB.Get(), 0);

    // rsSolidNoCull: cone must be visible from below (no back-face culling)
    ctx->RSSetState(rsSolidNoCull.Get());
    ctx->IASetInputLayout(simpleLayout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(simpleVS.Get(), nullptr, 0);
    ctx->PSSetShader(fovPS.Get(),    nullptr, 0);

    float bf[4] = {};
    ctx->OMSetBlendState(bsAlpha.Get(), bf, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(dssNoDepthWrite.Get(), 0);  // overlay — no depth write

    UINT stride = sizeof(SimpleVertex), offset = 0;
    ctx->IASetVertexBuffers(0, 1, fovConeVB.GetAddressOf(), &stride, &offset);
    ctx->Draw((UINT)verts.size(), 0);

    ctx->OMSetBlendState(nullptr, bf, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(dssDepth.Get(), 0);
    ctx->RSSetState(rsSolid.Get());
}
