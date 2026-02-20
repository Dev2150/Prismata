#include "Renderer.h"
#include <cmath>
#include <vector>
#include <algorithm>

// ── Renderer_Terrain.cpp ──────────────────────────────────────────────────────
// Covers: materialColor, buildChunkMesh, buildWaterMesh, renderTerrain, renderWater.

// ── materialColor ─────────────────────────────────────────────────────────────
// Maps a terrain material index (0-4) to an RGBA colour.
// Steep slopes override the base material with bare rock.
static void materialColor(uint8_t mat, float slope, float out[4]) {
    static const float cols[5][4] = {
        {0.20f,0.55f,0.15f,1},  // 0: grass  (green)
        {0.50f,0.50f,0.50f,1},  // 1: rock   (grey)
        {0.70f,0.60f,0.40f,1},  // 2: sand   (tan)
        {0.10f,0.30f,0.70f,1},  // 3: water  (blue)
        {0.90f,0.95f,1.00f,1},  // 4: snow   (white)
    };
    if (slope > 0.6f && mat != 3) { for(int i=0;i<4;i++) out[i]=cols[1][i]; return; }
    int idx = (mat >= 5) ? 0 : mat;
    for (int i = 0; i < 4; i++) out[i] = cols[idx][i];
}

// ── buildChunkMesh ────────────────────────────────────────────────────────────
// Converts one chunk's height + material data into a GPU vertex/index buffer pair.
//
// Each grid cell becomes a quad (4 vertices, 6 indices / 2 triangles):
//   TL──TR       Triangle 1: TL→TR→BL
//   |  / |       Triangle 2: TR→BR→BL
//   BL──BR
//
// The normal at each vertex is computed by finite differences on the height field:
//   dh/dx ≈ (h(x+0.5) - h(x-0.5))  →  surface normal = normalise(-dhdx, 1, -dhdz)
void Renderer::buildChunkMesh(const World& world, int cx, int cz) {
    int idx = cz * world.worldCX + cx;
    if ((int)chunkMeshes.size() <= idx)
        chunkMeshes.resize(world.worldCX * world.worldCZ);

    ChunkMesh& cm = chunkMeshes[idx];
    safeRelease(cm.vb); safeRelease(cm.ib);
    cm.idxCount = 0; cm.built = true;

    const Chunk* chunk = world.chunkAtPublic(cx, cz);
    if (!chunk) return;

    std::vector<TerrainVertex> verts;
    std::vector<uint32_t>      idxs;
    verts.reserve(CHUNK_SIZE * CHUNK_SIZE * 4);
    idxs.reserve(CHUNK_SIZE * CHUNK_SIZE * 6);

    for (int lz = 0; lz < CHUNK_SIZE - 1; lz++) {
        for (int lx = 0; lx < CHUNK_SIZE - 1; lx++) {
            float wx0 = (float)(cx * CHUNK_SIZE + lx);
            float wx1 = wx0 + 1.f;
            float wz0 = (float)(cz * CHUNK_SIZE + lz);
            float wz1 = wz0 + 1.f;
            uint8_t mat = chunk->cells[lz][lx].material;

            // Build one vertex at world position (wx, wz) with height-derived normal
            auto makeVert = [&](float wx, float wz) -> TerrainVertex {
                float h  = world.heightAt(wx, wz);
                float dx = world.heightAt(wx+0.5f,wz) - world.heightAt(wx-0.5f,wz);
                float dz = world.heightAt(wx,wz+0.5f) - world.heightAt(wx,wz-0.5f);
                float slope = std::sqrt(dx*dx + dz*dz);
                Float3 n = normalise3(-dx, 1.f, -dz);
                float col[4]; materialColor(mat, slope, col);
                TerrainVertex v;
                v.pos[0]=wx; v.pos[1]=h;  v.pos[2]=wz;
                v.nrm[0]=n.x; v.nrm[1]=n.y; v.nrm[2]=n.z;
                v.col[0]=col[0]; v.col[1]=col[1]; v.col[2]=col[2]; v.col[3]=col[3];
                return v;
            };

            uint32_t base = (uint32_t)verts.size();
            verts.push_back(makeVert(wx0, wz0));  // TL
            verts.push_back(makeVert(wx1, wz0));  // TR
            verts.push_back(makeVert(wx0, wz1));  // BL
            verts.push_back(makeVert(wx1, wz1));  // BR
            idxs.push_back(base+0); idxs.push_back(base+1); idxs.push_back(base+2); // TL→TR→BL
            idxs.push_back(base+1); idxs.push_back(base+3); idxs.push_back(base+2); // TR→BR→BL
        }
    }
    if (verts.empty()) return;

    // Upload to IMMUTABLE GPU buffers (terrain never changes at runtime)
    D3D11_BUFFER_DESC bd{}; D3D11_SUBRESOURCE_DATA sd{};

    bd.ByteWidth = (UINT)(verts.size() * sizeof(TerrainVertex));
    bd.Usage     = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    sd.pSysMem   = verts.data();
    device->CreateBuffer(&bd, &sd, &cm.vb);

    bd.ByteWidth = (UINT)(idxs.size() * sizeof(uint32_t));
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem   = idxs.data();
    device->CreateBuffer(&bd, &sd, &cm.ib);

    cm.idxCount = (int)idxs.size();
}

// ── buildWaterMesh ────────────────────────────────────────────────────────────
// A flat quad (6 vertices, no index buffer) covering the whole world at waterLevel.
void Renderer::buildWaterMesh(const World& world) {
    float maxX = (float)(world.worldCX * CHUNK_SIZE);
    float maxZ = (float)(world.worldCZ * CHUNK_SIZE);
    float wy   = waterLevel;

    SimpleVertex verts[] = {
        {0.f,  wy, 0.f}, {maxX, wy, 0.f}, {0.f,  wy, maxZ},
        {maxX, wy, 0.f}, {maxX, wy, maxZ},{0.f,  wy, maxZ},
    };

    safeRelease(waterVB);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(verts); bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = verts;
    device->CreateBuffer(&bd, &sd, &waterVB);
    waterBuilt = true;
}

// ── renderTerrain ─────────────────────────────────────────────────────────────
// Draws all chunk meshes as indexed triangle lists.
void Renderer::renderTerrain(const World& world) {
    ctx->IASetInputLayout(terrainLayout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(terrainVS, nullptr, 0);
    ctx->PSSetShader(terrainPS, nullptr, 0);

    UINT stride = sizeof(TerrainVertex), offset = 0;
    for (int cz = 0; cz < world.worldCZ; cz++) {
        for (int cx2 = 0; cx2 < world.worldCX; cx2++) {
            int i2 = cz * world.worldCX + cx2;
            if (i2 >= (int)chunkMeshes.size()) continue;
            const ChunkMesh& cm = chunkMeshes[i2];
            if (!cm.vb || !cm.ib || cm.idxCount == 0) continue;
            ctx->IASetVertexBuffers(0, 1, &cm.vb, &stride, &offset);
            ctx->IASetIndexBuffer(cm.ib, DXGI_FORMAT_R32_UINT, 0);
            ctx->DrawIndexed((UINT)cm.idxCount, 0, 0);
        }
    }
}

// ── renderWater ───────────────────────────────────────────────────────────────
// Draws the water plane with alpha blending and depth-test-only (no depth write),
// so transparent water doesn't block creatures drawn afterward.
void Renderer::renderWater(const World& world) {
    if (!waterBuilt) buildWaterMesh(world);
    if (!waterVB) return;

    ctx->RSSetState(rsSolid);
    ctx->IASetInputLayout(simpleLayout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(simpleVS, nullptr, 0);
    ctx->PSSetShader(waterPS,  nullptr, 0);

    float bf[4] = {};
    ctx->OMSetBlendState(bsAlpha, bf, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(dssNoDepthWrite, 0);

    UINT stride = sizeof(SimpleVertex), offset = 0;
    ctx->IASetVertexBuffers(0, 1, &waterVB, &stride, &offset);
    ctx->Draw(6, 0);

    ctx->OMSetBlendState(nullptr, bf, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(dssDepth, 0);
}
