#include "Renderer.hpp"
#include <cstring>
#include <cmath>
#include <algorithm>

#include "World/World.hpp"
#include "World/World_Planet.hpp"

// ── Day/night lighting helpers ─────────────────────────────────────────────────
// Smooth step in [0,1]: 0 when x<=lo, 1 when x>=hi, smooth cubic between.
static float smoothStep(float lo, float hi, float x) {
    float t = std::max(0.f, std::min(1.f, (x - lo) / (hi - lo)));
    return t * t * (3.f - 2.f * t);
}

// Linear interpolate between two 3-component colours.
static void lerpColor(const float a[3], const float b[3], float t, float out[3]) {
    out[0] = a[0] + (b[0]-a[0]) * t;
    out[1] = a[1] + (b[1]-a[1]) * t;
    out[2] = a[2] + (b[2]-a[2]) * t;
}

// ── computeDayNightLighting ────────────────────────────────────────────────────
// Given timeOfDay in [0,1), fills lightDir, sunColor, and ambientColor.
static void computeDayNightLighting(float timeOfDay,
                                    float lightDir[4],
                                    float sunColor[4],
                                    float ambientColor[4])
{
    const float PI = 3.14159265f;
    float phase     = timeOfDay * 2.f * PI;

    // Sun elevation: +1 at noon, -1 at midnight
    float elevation = -std::cos(phase);

    // Light direction: FROM the sun TOWARD the scene (shader negates it)
    // x component produces the east→west sweep; fixed z gives a slight south tilt.
    lightDir[0] =  std::sin(phase) * 0.6f;
    lightDir[1] = -elevation;                    // negative = sun above horizon
    lightDir[2] =  0.3f;
    lightDir[3] =  0.f;

    // Normalise so the shader's saturate(dot(N, L)) gives correct results.
    float len = std::sqrt(lightDir[0]*lightDir[0]
                        + lightDir[1]*lightDir[1]
                        + lightDir[2]*lightDir[2]);
    if (len > 1e-6f) { lightDir[0]/=len; lightDir[1]/=len; lightDir[2]/=len; }

    // On a spherical planet, the sun doesn't turn off. It's always shining.
    // The local day/night is handled by NdL in the shaders.
    sunColor[0] = 1.00f;
    sunColor[1] = 0.95f;
    sunColor[2] = 0.80f;
    sunColor[3] = timeOfDay;

    ambientColor[0] = 0.05f;
    ambientColor[1] = 0.05f;
    ambientColor[2] = 0.08f;
    ambientColor[3] = 0.f;
}

// ── updateFrameConstants ──────────────────────────────────────────────────────
// Writes camera, lighting, and fog data into the GPU constant buffer once per frame.
// Both vertex and pixel shaders read from this buffer via register(b0).
//
// The view*projection matrix is transposed before upload because:
//   - Our Mat4 stores data row-major (row 0 in m[0][0..3])
//   - HLSL float4x4 in a cbuffer expects column-major layout by default
//   - Transposing swaps the two conventions without changing the math
void Renderer::updateFrameConstants(const World& world, float aspect) {
    Mat4 view = camera.viewMatrix();        // positions + orients the "camera lens"
    Mat4 proj = camera.projMatrix(aspect);  // applies perspective (far = small)
    Mat4 vp   = (view * proj).transposed(); // combined transform, transposed for HLSL

    // Map/Unmap: the only safe way to write to a DYNAMIC GPU buffer.
    // MAP_WRITE_DISCARD = discard old contents; no GPU sync required (fast).
    D3D11_MAPPED_SUBRESOURCE ms{};
    ctx->Map(cbFrame, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    auto* fc = (FrameConstants*)ms.pData;

    memcpy(fc->viewProj, vp.m, sizeof(vp.m));
    fc->camPos[0] = camera.pos.x; fc->camPos[1] = camera.pos.y;
    fc->camPos[2] = camera.pos.z; fc->camPos[3] = 0.f;

    // ── Day/night lighting ────────────────────────────────────────────────────
    computeDayNightLighting(world.timeOfDay(),
                            fc->lightDir,
                            fc->sunColor,
                            fc->ambientColor);

    // ── Pass simTime for water wave animation ─────────────────────────────────
    // WaterVSMain reads ambientColor.w to phase the sine waves each frame.
    fc->ambientColor[3] = world.simTime;

    // ── Fog of war ────────────────────────────────────────────────────────────
    // w component acts as enable flag (0 = disabled, >0 = radius)
    if (showFogOfWar && playerID != INVALID_ID) {
        auto it = world.idToIndex.find(playerID);
        if (it != world.idToIndex.end()) {
            const Creature& pc = world.creatures[it->second];
            fc->fowData[0] = pc.pos.x; fc->fowData[1] = pc.pos.y;
            fc->fowData[2] = pc.pos.z; fc->fowData[3] = pc.genome.visionRange();

            Vec3 facing = {std::sin(pc.yaw), 0.f, std::cos(pc.yaw)};
            facing = g_planet_surface.projectToTangent(pc.pos, facing).normalised();
            fc->fowFacing[0] = facing.x;
            fc->fowFacing[1] = facing.y;
            fc->fowFacing[2] = facing.z;
            fc->fowFacing[3] = std::cos(pc.genome.visionFOV() * 3.14159265f / 180.f * 0.5f);
        } else { fc->fowData[3] = 0.f; }
    } else { fc->fowData[3] = 0.f; }

    ctx->Unmap(cbFrame, 0);

    // Bind to shader register b0 in both the VS and PS stages
    ctx->VSSetConstantBuffers(0, 1, &cbFrame);
    ctx->PSSetConstantBuffers(0, 1, &cbFrame);
}

// ── render ────────────────────────────────────────────────────────────────────
// Planet mode: skip terrain + water; only draw FOV cone + creature billboards.
void Renderer::render(const World& world, float aspectRatio) {
    updateFrameConstants(world, aspectRatio);

    // Draw order matters: opaque first, then transparent overlays on top
    ctx->RSSetState(rsSolid);
    ctx->OMSetDepthStencilState(dssDepth, 0);

    if (showFOVCone)
        renderFOVCone(world);

    // Plants first (behind creatures)
    renderPlants(world);

    // Creature billboards on top
    renderCreatures(world);
}
