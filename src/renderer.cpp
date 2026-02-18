#include "renderer.h"
#include <d3dcompiler.h>
#include <stdexcept>
#include <cassert>
#include <windows.h>

// ── HLSL shaders (compiled at runtime) ────────────────────────────────────────

static const char* TERRAIN_HLSL = R"HLSL(

cbuffer FrameConstants : register(b0)
{
    float4x4 viewProj;
    float4   camPos;
    float4   lightDir;   // w = unused
    float4   fowCenter;  // xyz = center, w = fowEnabled
};

struct VIn  { float3 pos : POSITION; float3 nrm : NORMAL; float4 col : COLOR; };
struct VOut { float4 sv  : SV_POSITION; float3 wpos : TEXCOORD0;
              float3 nrm : TEXCOORD1;   float4 col  : TEXCOORD2; };

VOut VSMain(VIn v)
{
    VOut o;
    o.sv   = mul(float4(v.pos, 1), viewProj);
    o.wpos = v.pos;
    o.nrm  = v.nrm;
    o.col  = v.col;
    return o;
}

float4 PSMain(VOut v) : SV_TARGET
{
    // Diffuse + ambient lighting
    float3 L   = normalize(-lightDir.xyz);
    float  ndl = saturate(dot(normalize(v.nrm), L));
    float3 lit = v.col.rgb * (0.25f + 0.75f * ndl);

    // Fog of war
    if (fowCenter.w > 0.5f)
    {
        float d = length(v.wpos.xz - fowCenter.xz);
        float f = saturate((d - fowCenter.w * 0.8f) / (fowCenter.w * 0.2f));
        lit = lerp(lit, float3(0.02f, 0.02f, 0.05f), f * f);
    }

    return float4(lit, 1.0f);
}
)HLSL";

static const char* CREATURE_HLSL = R"HLSL(

cbuffer FrameConstants : register(b0)
{
    float4x4 viewProj;
    float4   camPos;
    float4   lightDir;
    float4   fowCenter;
};

// Per-vertex (billboard quad)
struct VIn {
    float2 quadPos   : POSITION;
    // Instance data
    float3 worldPos  : INST_POS;
    float  yaw       : INST_YAW;
    float4 color     : INST_COLOR;
    float  size      : INST_SIZE;
    float3 pad       : INST_PAD;
};
struct VOut { float4 sv : SV_POSITION; float4 col : COLOR; };

VOut VSMain(VIn v)
{
    // Spherical billboard: always face camera
    float3 toCam = normalize(camPos.xyz - v.worldPos);
    float3 right = normalize(cross(float3(0,1,0), toCam));
    float3 up    = cross(toCam, right);

    float3 wpos  = v.worldPos
                 + right * v.quadPos.x * v.size
                 + up    * v.quadPos.y * v.size;

    VOut o;
    o.sv  = mul(float4(wpos, 1), viewProj);
    o.col = v.color;
    return o;
}

float4 PSMain(VOut v) : SV_TARGET
{
    return v.col;
}
)HLSL";

// ── Helpers ───────────────────────────────────────────────────────────────────
static ID3DBlob* compileShader(const char* src, const char* entry, const char* target) {
    ID3DBlob* blob = nullptr, *err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &err);
    if (FAILED(hr)) {
        if (err) {
            OutputDebugStringA((char*)err->GetBufferPointer());
            err->Release();
        }
        return nullptr;
    }
    if (err) err->Release();
    return blob;
}

// Material → color
static XMFLOAT4 materialColor(uint8_t mat, float height, float slope) {
    // water, rock, sand/dirt, snow, grass
    const XMFLOAT4 cols[] = {
        {0.1f, 0.3f, 0.7f, 1},  // water
        {0.5f, 0.5f, 0.5f, 1},  // rock
        {0.7f, 0.6f, 0.4f, 1},  // sand
        {0.9f, 0.95f,1.0f, 1},  // snow
        {0.25f,0.55f,0.15f,1},  // grass
    };
    if (slope > 0.6f && mat != 3)
        return cols[1];  // steep = rock
    if (mat >= 5) return cols[0];
    return cols[mat];
}

// ── Init ──────────────────────────────────────────────────────────────────────
bool renderer::init(ID3D11Device* dev, ID3D11DeviceContext* c, int w, int h) {
    device = dev;
    ctx    = c;
    winW   = w;
    winH   = h;

    if (!createShaders())         return false;
    if (!createBuffers(w, h))     return false;

    // Rasteriser states
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_BACK;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable       = TRUE;
    device->CreateRasterizerState(&rd, &rsSolid);

    rd.FillMode = D3D11_FILL_WIREFRAME;
    rd.CullMode = D3D11_CULL_NONE;
    device->CreateRasterizerState(&rd, &rsWireframe);

    // Depth stencil
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable    = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc      = D3D11_COMPARISON_LESS;
    device->CreateDepthStencilState(&dsd, &dssDepth);

    // Alpha blend (for creatures)
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable           = TRUE;
    bd.RenderTarget[0].SrcBlend             = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend            = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp              = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha        = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha       = D3D11_BLEND_ZERO;
    bd.RenderTarget[0].BlendOpAlpha         = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask= D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bd, &bsAlpha);

    return true;
}

bool renderer::createShaders() {
    // ── Terrain ──────────────────────────────────────────────────────────────
    ID3DBlob* tvs = compileShader(TERRAIN_HLSL, "VSMain", "vs_5_0");
    ID3DBlob* tps = compileShader(TERRAIN_HLSL, "PSMain", "ps_5_0");
    if (!tvs || !tps) return false;

    device->CreateVertexShader(tvs->GetBufferPointer(), tvs->GetBufferSize(), nullptr, &terrainVS);
    device->CreatePixelShader (tps->GetBufferPointer(), tps->GetBufferSize(), nullptr, &terrainPS);

    D3D11_INPUT_ELEMENT_DESC terrainDesc[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT,0,24,D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device->CreateInputLayout(terrainDesc, 3,
        tvs->GetBufferPointer(), tvs->GetBufferSize(), &terrainLayout);
    tvs->Release(); tps->Release();

    // ── Creatures ─────────────────────────────────────────────────────────────
    ID3DBlob* cvs = compileShader(CREATURE_HLSL, "VSMain", "vs_5_0");
    ID3DBlob* cps = compileShader(CREATURE_HLSL, "PSMain", "ps_5_0");
    if (!cvs || !cps) return false;

    device->CreateVertexShader(cvs->GetBufferPointer(), cvs->GetBufferSize(), nullptr, &creatureVS);
    device->CreatePixelShader (cps->GetBufferPointer(), cps->GetBufferSize(), nullptr, &creaturePS);

    D3D11_INPUT_ELEMENT_DESC creatureDesc[] = {
        // Per-vertex
        {"POSITION",   0, DXGI_FORMAT_R32G32_FLOAT,       0,  0, D3D11_INPUT_PER_VERTEX_DATA,   0},
        // Per-instance (slot 1)
        {"INST_POS",   0, DXGI_FORMAT_R32G32B32_FLOAT,    1,  0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INST_YAW",   0, DXGI_FORMAT_R32_FLOAT,          1, 12, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INST_COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INST_SIZE",  0, DXGI_FORMAT_R32_FLOAT,          1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INST_PAD",   0, DXGI_FORMAT_R32G32B32_FLOAT,    1, 36, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    device->CreateInputLayout(creatureDesc, 6,
        cvs->GetBufferPointer(), cvs->GetBufferSize(), &creatureLayout);
    cvs->Release(); cps->Release();

    return true;
}

bool renderer::createBuffers(int w, int h) {
    // Per-frame constant buffer
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = sizeof(FrameConstants);
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&bd, nullptr, &cbFrame);

    // Unit quad for creature billboards: 4 vertices
    float quadVerts[] = {-0.5f,-0.5f, 0.5f,-0.5f, -0.5f,0.5f, 0.5f,0.5f};
    bd.ByteWidth      = sizeof(quadVerts);
    bd.Usage          = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = 0;
    D3D11_SUBRESOURCE_DATA initData{};
    initData.pSysMem  = quadVerts;
    device->CreateBuffer(&bd, &initData, &creatureQuadVB);

    // Instance buffer (up to 4096 creatures)
    bd.ByteWidth      = sizeof(CreatureInstance) * 4096;
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&bd, nullptr, &creatureInstanceVB);

    // Depth/stencil texture
    return createDepthBuffer(w, h);
}

bool renderer::createDepthBuffer(int w, int h) {
    safeRelease(depthTex);
    safeRelease(depthDSV);

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = w;
    td.Height           = h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &depthTex))) return false;
    if (FAILED(device->CreateDepthStencilView(depthTex, nullptr, &depthDSV))) return false;
    return true;
}

void renderer::resize(int w, int h) {
    winW = w; winH = h;
    createDepthBuffer(w, h);
}

// ── Chunk mesh building ────────────────────────────────────────────────────────
void renderer::buildChunkMesh(const world& world, int cx, int cz) {
    if ((int)chunkMeshes.size() <= cz * world.worldCX + cx)
        chunkMeshes.resize(world.worldCX * world.worldCZ);

    ChunkMesh& cm = chunkMeshes[cz * world.worldCX + cx];
    safeRelease(cm.vb);
    safeRelease(cm.ib);
    cm.idxCount = 0;
    cm.built    = true;

    const Chunk* chunk = world.chunkAt(cx, cz);
    if (!chunk) return;

    std::vector<TerrainVertex> verts;
    std::vector<uint32_t>      idxs;
    verts.reserve(CHUNK_SIZE * CHUNK_SIZE * 4);
    idxs.reserve(CHUNK_SIZE * CHUNK_SIZE * 6);

    auto getH = [&](int lx, int lz) -> float {
        lx = std::clamp(lx, 0, CHUNK_SIZE-1);
        lz = std::clamp(lz, 0, CHUNK_SIZE-1);
        return chunk->cells[lz][lx].height;
    };

    for (int lz = 0; lz < CHUNK_SIZE - 1; lz++) {
        for (int lx = 0; lx < CHUNK_SIZE - 1; lx++) {
            float wx0 = (float)(cx * CHUNK_SIZE + lx);
            float wx1 = wx0 + 1.f;
            float wz0 = (float)(cz * CHUNK_SIZE + lz);
            float wz1 = wz0 + 1.f;

            float h00 = getH(lx,   lz  );
            float h10 = getH(lx+1, lz  );
            float h01 = getH(lx,   lz+1);
            float h11 = getH(lx+1, lz+1);

            uint8_t mat = chunk->cells[lz][lx].material;

            // Emit two triangles
            // Normal from cross product
            auto makeVert = [&](float x, float h, float z) -> TerrainVertex {
                // Finite-diff normal
                float dhdx = (world.heightAt(x+0.5f, z) - world.heightAt(x-0.5f, z));
                float dhdz = (world.heightAt(x, z+0.5f) - world.heightAt(x, z-0.5f));
                XMFLOAT3 nrm;
                XMStoreFloat3(&nrm, XMVector3Normalize(XMVectorSet(-dhdx, 1.f, -dhdz, 0)));
                float slope = std::sqrt(dhdx*dhdx + dhdz*dhdz);
                XMFLOAT4 col = materialColor(mat, h, slope);
                return {{x, h, z}, nrm, col};
            };

            uint32_t base = (uint32_t)verts.size();
            verts.push_back(makeVert(wx0, h00, wz0));
            verts.push_back(makeVert(wx1, h10, wz0));
            verts.push_back(makeVert(wx0, h01, wz1));
            verts.push_back(makeVert(wx1, h11, wz1));

            // Tri A
            idxs.push_back(base+0); idxs.push_back(base+1); idxs.push_back(base+2);
            // Tri B
            idxs.push_back(base+1); idxs.push_back(base+3); idxs.push_back(base+2);
        }
    }

    if (verts.empty()) return;

    D3D11_BUFFER_DESC bd{};
    D3D11_SUBRESOURCE_DATA sd{};

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

// ── Render ────────────────────────────────────────────────────────────────────
void renderer::render(const world& world, float aspectRatio) {
    // Build chunk meshes for dirty chunks
    for (int cz = 0; cz < world.worldCZ; cz++) {
        for (int cx = 0; cx < world.worldCX; cx++) {
            const Chunk& ch = world.chunks[cz * world.worldCX + cx];
            bool needsBuild = ch.dirty;
            if ((int)chunkMeshes.size() > cz * world.worldCX + cx)
                needsBuild |= !chunkMeshes[cz * world.worldCX + cx].built;
            else
                needsBuild = true;
            if (needsBuild)
                buildChunkMesh(world, cx, cz);
            // Note: const_cast here only clears a 'dirty' flag, safe
            const_cast<Chunk&>(ch).dirty = false;
        }
    }

    updateFrameConstants(world, aspectRatio);

    ctx->RSSetState(wireframe ? rsWireframe : rsSolid);
    ctx->OMSetDepthStencilState(dssDepth, 0);

    renderTerrain(world);
    renderCreatures(world);
}

void renderer::updateFrameConstants(const world& world, float aspect) {
    XMMATRIX view = camera.viewMatrix();
    XMMATRIX proj = camera.projMatrix(aspect);

    D3D11_MAPPED_SUBRESOURCE ms{};
    ctx->Map(cbFrame, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    auto* fc = (FrameConstants*)ms.pData;
    XMStoreFloat4x4(&fc->viewProj, XMMatrixTranspose(view * proj));
    fc->camPos   = {camera.pos.x, camera.pos.y, camera.pos.z, 0};
    fc->lightDir = {0.4f, -0.8f, 0.3f, 0};

    if (showFogOfWar && playerID != INVALID_ID) {
        auto it = world.idToIndex.find(playerID);
        if (it != world.idToIndex.end()) {
            const creature& pc = world.creatures[it->second];
            fowCenter = {pc.pos.x, pc.pos.y, pc.pos.z};
            fc->fowCenter = {fowCenter.x, fowCenter.y, fowCenter.z, fogRadius};
        } else {
            fc->fowCenter = {0,0,0,0};
        }
    } else {
        fc->fowCenter = {0,0,0,0};
    }
    ctx->Unmap(cbFrame, 0);
    ctx->VSSetConstantBuffers(0, 1, &cbFrame);
    ctx->PSSetConstantBuffers(0, 1, &cbFrame);
}

void renderer::renderTerrain(const world& world) {
    ctx->IASetInputLayout(terrainLayout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(terrainVS, nullptr, 0);
    ctx->PSSetShader(terrainPS, nullptr, 0);

    UINT stride = sizeof(TerrainVertex), offset = 0;
    for (int cz = 0; cz < world.worldCZ; cz++) {
        for (int cx = 0; cx < world.worldCX; cx++) {
            int idx = cz * world.worldCX + cx;
            if (idx >= (int)chunkMeshes.size()) continue;
            const ChunkMesh& cm = chunkMeshes[idx];
            if (!cm.vb || !cm.ib || cm.idxCount == 0) continue;

            ctx->IASetVertexBuffers(0, 1, &cm.vb, &stride, &offset);
            ctx->IASetIndexBuffer(cm.ib, DXGI_FORMAT_R32_UINT, 0);
            ctx->DrawIndexed((UINT)cm.idxCount, 0, 0);
        }
    }
}

void renderer::renderCreatures(const world& world) {
    // Upload instance data
    D3D11_MAPPED_SUBRESOURCE ms{};
    ctx->Map(creatureInstanceVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    auto* inst = (CreatureInstance*)ms.pData;
    int count  = 0;
    for (const auto& c : world.creatures) {
        if (!c.alive) continue;
        if (count >= 4096) break;

        // Genome → colour (HSV → RGB)
        float h = c.genome.hue() / 60.f;
        int   hi = (int)h;
        float f  = h - hi;
        float p  = 0.3f, q = 1.f - 0.7f * f, tv = 0.3f + 0.7f * f;
        float rgb[3][3] = {{1,tv,p},{q,1,p},{p,1,tv},{p,q,1},{tv,p,1},{1,p,q}};
        float r = rgb[hi%6][0], g = rgb[hi%6][1], b = rgb[hi%6][2];

        inst[count].pos   = {c.pos.x, c.pos.y + c.genome.bodySize() * 0.5f, c.pos.z};
        inst[count].yaw   = c.yaw;
        inst[count].color = {r, g, b, 0.8f + 0.2f * (c.energy / c.maxEnergy)};
        inst[count].size  = c.genome.bodySize() * 0.8f;
        count++;
    }
    ctx->Unmap(creatureInstanceVB, 0);

    if (count == 0) return;

    ctx->IASetInputLayout(creatureLayout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(creatureVS, nullptr, 0);
    ctx->PSSetShader(creaturePS, nullptr, 0);

    float bsf[4] = {};
    ctx->OMSetBlendState(bsAlpha, bsf, 0xFFFFFFFF);

    UINT strides[2] = {sizeof(float)*2, sizeof(CreatureInstance)};
    UINT offsets[2] = {0, 0};
    ID3D11Buffer* vbs[2] = {creatureQuadVB, creatureInstanceVB};
    ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
    ctx->DrawInstanced(4, (UINT)count, 0, 0);

    // Reset blend
    ctx->OMSetBlendState(nullptr, bsf, 0xFFFFFFFF);
}

// ── Camera input ──────────────────────────────────────────────────────────────
void Camera::processInput(float dt) {
    // Implemented by Renderer which tracks key states
}

void renderer::onMouseMove(int dx, int dy, bool rightDown) {
    if (!rightDown) return;
    camera.yaw   += dx * 0.003f;
    camera.pitch += dy * 0.003f;
    camera.pitch = std::clamp(camera.pitch, -1.5f, 0.1f);
}

void renderer::onKey(int vk, bool down) {
    float v = down ? 1.f : 0.f;
    switch (vk) {
        case 'W': moveKeys[0] = v; break;
        case 'S': moveKeys[1] = v; break;
        case 'A': moveKeys[2] = v; break;
        case 'D': moveKeys[3] = v; break;
        case 'Q': moveKeys[4] = v; break;
        case 'E': moveKeys[5] = v; break;
    }
}

void renderer::tickCamera(float dt, bool playerMode) {
    if (playerMode) return;   // camera follows player in player mode

    float spd = 15.f * dt;
    XMVECTOR fwd = XMVector3Transform(XMVectorSet(0,0,1,0),
                    XMMatrixRotationY(camera.yaw));
    XMVECTOR right = XMVector3Transform(XMVectorSet(1,0,0,0),
                    XMMatrixRotationY(camera.yaw));
    XMVECTOR pos  = XMLoadFloat3(&camera.pos);
    pos = XMVectorAdd(pos, XMVectorScale(fwd,   (moveKeys[0] - moveKeys[1]) * spd));
    pos = XMVectorAdd(pos, XMVectorScale(right,  (moveKeys[3] - moveKeys[2]) * spd));
    pos = XMVectorAdd(pos, XMVectorScale(XMVectorSet(0,1,0,0), (moveKeys[4] - moveKeys[5]) * spd));
    XMStoreFloat3(&camera.pos, pos);
}

// ── Shutdown ──────────────────────────────────────────────────────────────────
void renderer::shutdown() {
    for (auto& cm : chunkMeshes) { safeRelease(cm.vb); safeRelease(cm.ib); }
    safeRelease(terrainVS);   safeRelease(terrainPS);
    safeRelease(creatureVS);  safeRelease(creaturePS);
    safeRelease(terrainLayout); safeRelease(creatureLayout);
    safeRelease(cbFrame);
    safeRelease(creatureQuadVB); safeRelease(creatureInstanceVB);
    safeRelease(rsWireframe); safeRelease(rsSolid);
    safeRelease(dssDepth);    safeRelease(bsAlpha);
    safeRelease(depthTex);    safeRelease(depthDSV);
}
