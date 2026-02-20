#include "Renderer.hpp"
#include <cstring>
#include <cmath>
#include <algorithm>

#include "World/World.hpp"

// ── Renderer_Frame.cpp ────────────────────────────────────────────────────────
// Covers: updateFrameConstants, render.
// This is the main per-frame orchestrator — it calls the sub-renderers in order.

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
// Given timeOfDay in [0,1) (0=midnight, 0.25=dawn, 0.5=noon, 0.75=dusk),
// fills lightDir, sunColor (rgb + timeOfDay in w), and ambientColor (rgb).
//
// Sun arc:
//   elevation = -cos(t × 2π)   →  -1 at midnight, +1 at noon
//   lightDir.y = -elevation     →  negative when sun is above (points downward from sun)
//   lightDir.x = sin(t × 2π)   →  east→west sweep across the sky
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

    // ── Sun colour ────────────────────────────────────────────────────────────
    // Reference colours (sRGB-ish):
    //   Night   : no direct sun
    //   Dawn/Dusk: warm orange-red
    //   Day     : bright warm white

    static const float colNight[3]    = {0.00f, 0.00f, 0.00f};
    static const float colHorizon[3]  = {1.00f, 0.45f, 0.10f}; // orange glow
    static const float colDay[3]      = {1.00f, 0.95f, 0.80f}; // warm white

    float aboveHorizon = std::max(0.f, elevation);          // 0 at night, 1 at noon
    float horizonBlend = smoothStep(-0.15f, 0.25f, elevation); // 0=night, 1=fully day
    float dayBlend     = smoothStep( 0.15f, 0.55f, elevation); // 0=horizon, 1=full day

    float midSun[3];
    lerpColor(colNight, colHorizon, horizonBlend, midSun);
    float finalSun[3];
    lerpColor(midSun, colDay, dayBlend, finalSun);

    // Scale brightness by elevation so the sun doesn't light underground
    sunColor[0] = finalSun[0] * aboveHorizon;
    sunColor[1] = finalSun[1] * aboveHorizon;
    sunColor[2] = finalSun[2] * aboveHorizon;
    sunColor[3] = timeOfDay;   // w = timeOfDay for the creature brightness shader

    // ── Ambient (sky) colour ──────────────────────────────────────────────────
    // Night: deep blue  /  Dawn-Dusk: purple-pink  /  Day: cool pale blue sky

    static const float ambNight[3]   = {0.03f, 0.04f, 0.12f};
    static const float ambHorizon[3] = {0.20f, 0.14f, 0.20f}; // lavender dusk
    static const float ambDay[3]     = {0.28f, 0.35f, 0.48f}; // sky blue

    float midAmb[3];
    lerpColor(ambNight, ambHorizon, horizonBlend, midAmb);
    float finalAmb[3];
    lerpColor(midAmb, ambDay, dayBlend, finalAmb);

    ambientColor[0] = finalAmb[0];
    ambientColor[1] = finalAmb[1];
    ambientColor[2] = finalAmb[2];
    // ── ambientColor.w = simTime (seconds) ────────────────────────────────────
    // Consumed by WaterVSMain in SIMPLE_HLSL to drive the wave animation.
    // Value is set by the caller (updateFrameConstants) from world.simTime.
    // Left at 0 here; overwritten below.
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
            fc->fowData[2] = pc.pos.z; fc->fowData[3] = fogRadius;
        } else { fc->fowData[3] = 0.f; }
    } else { fc->fowData[3] = 0.f; }

    ctx->Unmap(cbFrame, 0);

    // Bind to shader register b0 in both the VS and PS stages
    ctx->VSSetConstantBuffers(0, 1, &cbFrame);
    ctx->PSSetConstantBuffers(0, 1, &cbFrame);
}

// ── render ────────────────────────────────────────────────────────────────────
// Main per-frame draw sequence. Called once per frame after the scene is updated.
// The GPU is a state machine: each ctx->Set*() call changes a setting that
// persists until overridden. The sub-renderers each restore any state they change.
void Renderer::render(const World& world, float aspectRatio) {
    // Rebuild any terrain chunk meshes that have been flagged dirty
    for (int cz = 0; cz < world.worldCZ; cz++) {
        for (int cx2 = 0; cx2 < world.worldCX; cx2++) {
            int i2 = cz * world.worldCX + cx2;
            const Chunk& ch = world.chunks[i2];
            bool needsBuild = ch.dirty || (i2 >= (int)chunkMeshes.size()) || !chunkMeshes[i2].built;
            if (needsBuild) buildChunkMesh(world, cx2, cz);
            const_cast<Chunk&>(ch).dirty = false;
        }
    }

    updateFrameConstants(world, aspectRatio);

    // Draw order matters: opaque first, then transparent overlays on top
    ctx->RSSetState(wireframe ? rsWireframe : rsSolid);
    ctx->OMSetDepthStencilState(dssDepth, 0);
    renderTerrain(world);                           // 1. opaque terrain

    if (showWater   && !wireframe) renderWater(world);     // 2. animated water
    if (showFOVCone && !wireframe) renderFOVCone(world);   // 3. FOV overlay
    renderCreatures(world);                         // 4. creature billboards
}
