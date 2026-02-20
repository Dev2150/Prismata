#include "Renderer.h"
#include <cstring>
#include <algorithm>

// ── Renderer_Frame.cpp ────────────────────────────────────────────────────────
// Covers: updateFrameConstants, render.
// This is the main per-frame orchestrator — it calls the sub-renderers in order.

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
    fc->lightDir[0] = 0.4f; fc->lightDir[1] = -0.8f;
    fc->lightDir[2] = 0.3f; fc->lightDir[3] =  0.f;

    // Fog of war: w component acts as enable flag (0 = disabled, >0 = radius)
    if (showFogOfWar && playerID != INVALID_ID) {
        auto it = world.idToIndex.find(playerID);
        if (it != world.idToIndex.end()) {
            const Creature& pc = world.creatures[it->second];
            fc->fowCenter[0] = pc.pos.x; fc->fowCenter[1] = pc.pos.y;
            fc->fowCenter[2] = pc.pos.z; fc->fowCenter[3] = fogRadius;
        } else { fc->fowCenter[3] = 0.f; }
    } else { fc->fowCenter[3] = 0.f; }

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

    if (showWater   && !wireframe) renderWater(world);     // 2. translucent water
    if (showFOVCone && !wireframe) renderFOVCone(world);   // 3. translucent FOV overlay
    renderCreatures(world);                         // 4. creature billboards
}
