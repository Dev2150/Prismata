#include "Renderer.h"
#include <d3dcompiler.h>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <vector>

// ── HLSL shaders ──────────────────────────────────────────────────────────────
static const char* TERRAIN_HLSL = R"HLSL(
cbuffer FrameConstants : register(b0) {
    float4x4 viewProj;
    float4   camPos;
    float4   lightDir;
    float4   fowData;   // xyz=center, w=radius (0=off)
};
struct VIn  { float3 pos : POSITION; float3 nrm : NORMAL; float4 col : COLOR; };
struct VOut { float4 sv  : SV_POSITION; float3 wpos : TEXCOORD0;
              float3 nrm : TEXCOORD1;   float4 col  : TEXCOORD2; };
VOut VSMain(VIn v) {
    VOut o;
    o.sv   = mul(float4(v.pos, 1.0f), viewProj);
    o.wpos = v.pos;
    o.nrm  = v.nrm;
    o.col  = v.col;
    return o;
}
float4 PSMain(VOut v) : SV_TARGET {
    float3 L   = normalize(-lightDir.xyz);
    float  ndl = saturate(dot(normalize(v.nrm), L));
    float3 lit = v.col.rgb * (0.25f + 0.75f * ndl);
    if (fowData.w > 0.0f) {
        float d = length(v.wpos.xz - fowData.xz);
        float f = saturate((d - fowData.w * 0.8f) / (fowData.w * 0.2f + 0.001f));
        lit = lerp(lit, float3(0.02f, 0.02f, 0.05f), f * f);
    }
    return float4(lit, 1.0f);
}
)HLSL";

static const char* CREATURE_HLSL = R"HLSL(
cbuffer FrameConstants : register(b0) {
    float4x4 viewProj;
    float4   camPos;
    float4   lightDir;
    float4   fowData;
};
struct VIn {
    float2 quadPos  : POSITION;
    float3 worldPos : INST_POS;
    float  yaw      : INST_YAW;
    float4 color    : INST_COLOR;
    float  size     : INST_SIZE;
    float3 pad      : INST_PAD;
};
struct VOut { float4 sv : SV_POSITION; float4 col : COLOR; };
VOut VSMain(VIn v) {
    float3 toCam = normalize(camPos.xyz - v.worldPos);
    float3 right = normalize(cross(float3(0,1,0), toCam));
    float3 up    = cross(toCam, right);
    float3 wpos  = v.worldPos + right * v.quadPos.x * v.size
                              + up    * v.quadPos.y * v.size;
    VOut o;
    o.sv  = mul(float4(wpos, 1.0f), viewProj);
    o.col = v.color;
    return o;
}
float4 PSMain(VOut v) : SV_TARGET { return v.col; }
)HLSL";

// ── Shader compile helper ─────────────────────────────────────────────────────
static ID3DBlob* compileShader(const char* src, const char* entry, const char* target) {
    ID3DBlob* blob = nullptr, *err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &blob, &err);
    if (FAILED(hr)) {
        if (err) { OutputDebugStringA((char*)err->GetBufferPointer()); err->Release(); }
        return nullptr;
    }
    if (err) err->Release();
    return blob;
}

// ── Material colour ───────────────────────────────────────────────────────────
static void materialColor(uint8_t mat, float slope, float out[4]) {
    // water, rock, sand, snow, grass
    static const float cols[5][4] = {
        {0.10f,0.30f,0.70f,1}, {0.50f,0.50f,0.50f,1},
        {0.70f,0.60f,0.40f,1}, {0.90f,0.95f,1.00f,1},
        {0.25f,0.55f,0.15f,1}
    };
    if (slope > 0.6f && mat != 3) { for(int i=0;i<4;i++) out[i]=cols[1][i]; return; }
    int idx = (mat >= 5) ? 0 : mat;
    for (int i = 0; i < 4; i++) out[i] = cols[idx][i];
}

// ── Init ──────────────────────────────────────────────────────────────────────
bool Renderer::init(ID3D11Device* dev, ID3D11DeviceContext* c, int w, int h) {
    device = dev; ctx = c; winW = w; winH = h;
    if (!createShaders())     return false;
    if (!createBuffers(w, h)) return false;

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_BACK;
    rd.FrontCounterClockwise = FALSE; rd.DepthClipEnable = TRUE;
    device->CreateRasterizerState(&rd, &rsSolid);
    rd.FillMode = D3D11_FILL_WIREFRAME; rd.CullMode = D3D11_CULL_NONE;
    device->CreateRasterizerState(&rd, &rsWireframe);

    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE; dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS;
    device->CreateDepthStencilState(&dsd, &dssDepth);

    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA; rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE; rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bd, &bsAlpha);
    return true;
}

bool Renderer::createShaders() {
    // Terrain
    ID3DBlob* tvs = compileShader(TERRAIN_HLSL, "VSMain", "vs_5_0");
    ID3DBlob* tps = compileShader(TERRAIN_HLSL, "PSMain", "ps_5_0");
    if (!tvs || !tps) { if(tvs)tvs->Release(); if(tps)tps->Release(); return false; }
    device->CreateVertexShader(tvs->GetBufferPointer(), tvs->GetBufferSize(), nullptr, &terrainVS);
    device->CreatePixelShader (tps->GetBufferPointer(), tps->GetBufferSize(), nullptr, &terrainPS);

    D3D11_INPUT_ELEMENT_DESC td[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device->CreateInputLayout(td, 3, tvs->GetBufferPointer(), tvs->GetBufferSize(), &terrainLayout);
    tvs->Release(); tps->Release();

    // Creatures
    ID3DBlob* cvs = compileShader(CREATURE_HLSL, "VSMain", "vs_5_0");
    ID3DBlob* cps = compileShader(CREATURE_HLSL, "PSMain", "ps_5_0");
    if (!cvs || !cps) { if(cvs)cvs->Release(); if(cps)cps->Release(); return false; }
    device->CreateVertexShader(cvs->GetBufferPointer(), cvs->GetBufferSize(), nullptr, &creatureVS);
    device->CreatePixelShader (cps->GetBufferPointer(), cps->GetBufferSize(), nullptr, &creaturePS);

    D3D11_INPUT_ELEMENT_DESC cd[] = {
        {"POSITION",  0, DXGI_FORMAT_R32G32_FLOAT,       0,  0, D3D11_INPUT_PER_VERTEX_DATA,   0},
        {"INST_POS",  0, DXGI_FORMAT_R32G32B32_FLOAT,    1,  0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INST_YAW",  0, DXGI_FORMAT_R32_FLOAT,          1, 12, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INST_COLOR",0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INST_SIZE", 0, DXGI_FORMAT_R32_FLOAT,          1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INST_PAD",  0, DXGI_FORMAT_R32G32B32_FLOAT,    1, 36, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    device->CreateInputLayout(cd, 6, cvs->GetBufferPointer(), cvs->GetBufferSize(), &creatureLayout);
    cvs->Release(); cps->Release();
    return true;
}

bool Renderer::createBuffers(int w, int h) {
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(FrameConstants); bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&bd, nullptr, &cbFrame))) {
        OutputDebugStringA("Error: Failed to create frame constant buffer!\n");
        return false; // Stop initialization
    }

    float quad[] = {-0.5f,-0.5f, 0.5f,-0.5f, -0.5f,0.5f, 0.5f,0.5f};
    bd.ByteWidth = sizeof(quad); bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER; bd.CPUAccessFlags = 0;
    D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = quad;
    device->CreateBuffer(&bd, &sd, &creatureQuadVB);

    bd.ByteWidth = (UINT)(sizeof(CreatureInstance) * 4096);
    bd.Usage = D3D11_USAGE_DYNAMIC; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&bd, nullptr, &creatureInstanceVB);

    return createDepthBuffer(w, h);
}

bool Renderer::createDepthBuffer(int w, int h) {
    safeRelease(depthTex); safeRelease(depthDSV);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_D32_FLOAT; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT; td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device->CreateTexture2D(&td, nullptr, &depthTex))) return false;
    if (FAILED(device->CreateDepthStencilView(depthTex, nullptr, &depthDSV))) return false;
    return true;
}

void Renderer::resize(int w, int h) { winW = w; winH = h; createDepthBuffer(w, h); }

// ── Chunk mesh ────────────────────────────────────────────────────────────────
void Renderer::buildChunkMesh(const World& world, int cx, int cz) {
    int idx = cz * world.worldCX + cx;
    if ((int)chunkMeshes.size() <= idx) chunkMeshes.resize(world.worldCX * world.worldCZ);
    ChunkMesh& cm = chunkMeshes[idx];
    safeRelease(cm.vb); safeRelease(cm.ib); cm.idxCount = 0; cm.built = true;

    const Chunk* chunk = world.chunkAtPublic(cx, cz);
    if (!chunk) return;

    std::vector<TerrainVertex> verts;
    std::vector<uint32_t> idxs;
    verts.reserve(CHUNK_SIZE * CHUNK_SIZE * 4);
    idxs.reserve(CHUNK_SIZE * CHUNK_SIZE * 6);

    for (int lz = 0; lz < CHUNK_SIZE - 1; lz++) {
        for (int lx = 0; lx < CHUNK_SIZE - 1; lx++) {
            float wx0 = (float)(cx * CHUNK_SIZE + lx);
            float wx1 = wx0 + 1.f;
            float wz0 = (float)(cz * CHUNK_SIZE + lz);
            float wz1 = wz0 + 1.f;

            uint8_t mat = chunk->cells[lz][lx].material;

            auto makeVert = [&](float wx, float wz) -> TerrainVertex {
                float h  = world.heightAt(wx, wz);
                float dx = (world.heightAt(wx+0.5f,wz) - world.heightAt(wx-0.5f,wz));
                float dz = (world.heightAt(wx,wz+0.5f) - world.heightAt(wx,wz-0.5f));
                float slope = std::sqrt(dx*dx + dz*dz);
                Float3 n = normalise3(-dx, 1.f, -dz);
                float col[4]; materialColor(mat, slope, col);
                TerrainVertex v;
                v.pos[0]=wx; v.pos[1]=h;    v.pos[2]=wz;
                v.nrm[0]=n.x; v.nrm[1]=n.y; v.nrm[2]=n.z;
                v.col[0]=col[0]; v.col[1]=col[1]; v.col[2]=col[2]; v.col[3]=col[3];
                return v;
            };

            uint32_t base = (uint32_t)verts.size();
            verts.push_back(makeVert(wx0, wz0));
            verts.push_back(makeVert(wx1, wz0));
            verts.push_back(makeVert(wx0, wz1));
            verts.push_back(makeVert(wx1, wz1));
            idxs.push_back(base+0); idxs.push_back(base+1); idxs.push_back(base+2);
            idxs.push_back(base+1); idxs.push_back(base+3); idxs.push_back(base+2);
        }
    }
    if (verts.empty()) return;

    D3D11_BUFFER_DESC bd{}; D3D11_SUBRESOURCE_DATA sd{};
    bd.ByteWidth = (UINT)(verts.size() * sizeof(TerrainVertex));
    bd.Usage = D3D11_USAGE_IMMUTABLE; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    sd.pSysMem = verts.data();
    device->CreateBuffer(&bd, &sd, &cm.vb);

    bd.ByteWidth = (UINT)(idxs.size() * sizeof(uint32_t));
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    sd.pSysMem = idxs.data();
    device->CreateBuffer(&bd, &sd, &cm.ib);
    cm.idxCount = (int)idxs.size();
}

// ── Frame constants ───────────────────────────────────────────────────────────
void Renderer::updateFrameConstants(const World& world, float aspect) {
    Mat4 view = camera.viewMatrix();
    Mat4 proj = camera.projMatrix(aspect);
    // Combine and transpose for HLSL row_major cbuffer
    Mat4 vp   = (view * proj).transposed();

    D3D11_MAPPED_SUBRESOURCE ms{};
    ctx->Map(cbFrame, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    auto* fc = (FrameConstants*)ms.pData;
    memcpy(fc->viewProj, vp.m, sizeof(vp.m));
    fc->camPos[0] = camera.pos.x; fc->camPos[1] = camera.pos.y;
    fc->camPos[2] = camera.pos.z; fc->camPos[3] = 0.f;
    fc->lightDir[0]=0.4f; fc->lightDir[1]=-0.8f; fc->lightDir[2]=0.3f; fc->lightDir[3]=0.f;

    if (showFogOfWar && playerID != INVALID_ID) {
        auto it = world.idToIndex.find(playerID);
        if (it != world.idToIndex.end()) {
            const Creature& pc = world.creatures[it->second];
            fc->fowCenter[0]=pc.pos.x; fc->fowCenter[1]=pc.pos.y;
            fc->fowCenter[2]=pc.pos.z; fc->fowCenter[3]=fogRadius;
        } else { fc->fowCenter[3]=0.f; }
    } else { fc->fowCenter[3]=0.f; }
    ctx->Unmap(cbFrame, 0);
    ctx->VSSetConstantBuffers(0, 1, &cbFrame);
    ctx->PSSetConstantBuffers(0, 1, &cbFrame);
}

// ── Render ────────────────────────────────────────────────────────────────────
void Renderer::render(const World& world, float aspectRatio) {
    for (int cz = 0; cz < world.worldCZ; cz++) {
        for (int cx2 = 0; cx2 < world.worldCX; cx2++) {
            const Chunk& ch = world.chunks[cz * world.worldCX + cx2];
            int i2 = cz * world.worldCX + cx2;
            bool needsBuild = ch.dirty || (i2 >= (int)chunkMeshes.size()) || !chunkMeshes[i2].built;
            if (needsBuild) buildChunkMesh(world, cx2, cz);
            const_cast<Chunk&>(ch).dirty = false;
        }
    }
    updateFrameConstants(world, aspectRatio);
    ctx->RSSetState(wireframe ? rsWireframe : rsSolid);
    ctx->OMSetDepthStencilState(dssDepth, 0);
    renderTerrain(world);
    renderCreatures(world);
}

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

// Genome hue (0-360) → RGB
static void hueToRGB(float hue, float out[3]) {
    // 6 sectors, each 60°
    const float rgb6[6][3] = {
        {1.f,0.f,0.f},{1.f,1.f,0.f},{0.f,1.f,0.f},
        {0.f,1.f,1.f},{0.f,0.f,1.f},{1.f,0.f,1.f}
    };
    hue = std::fmod(hue, 360.f);
    float sector = hue / 60.f;
    int   hi = (int)sector % 6;
    float f  = sector - (int)sector;
    int   hi2 = (hi + 1) % 6;
    for (int i = 0; i < 3; i++)
        out[i] = 0.3f + 0.7f * (rgb6[hi][i] * (1.f-f) + rgb6[hi2][i] * f);
}

void Renderer::renderCreatures(const World& world) {
    D3D11_MAPPED_SUBRESOURCE ms{};
    ctx->Map(creatureInstanceVB, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    auto* inst = (CreatureInstance*)ms.pData;
    int count = 0;
    for (const auto& c : world.creatures) {
        if (!c.alive || count >= 4096) continue;
        float rgb[3]; hueToRGB(c.genome.hue(), rgb);
        inst[count].pos[0] = c.pos.x;
        inst[count].pos[1] = c.pos.y + c.genome.bodySize() * 0.5f;
        inst[count].pos[2] = c.pos.z;
        inst[count].yaw    = c.yaw;
        inst[count].color[0]=rgb[0]; inst[count].color[1]=rgb[1];
        inst[count].color[2]=rgb[2]; inst[count].color[3]=0.9f;
        inst[count].size   = c.genome.bodySize() * 0.8f;
        inst[count].pad[0] = inst[count].pad[1] = inst[count].pad[2] = 0.f;
        count++;
    }
    ctx->Unmap(creatureInstanceVB, 0);
    if (count == 0) return;

    ctx->IASetInputLayout(creatureLayout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(creatureVS, nullptr, 0);
    ctx->PSSetShader(creaturePS, nullptr, 0);
    float bf[4]={};
    ctx->OMSetBlendState(bsAlpha, bf, 0xFFFFFFFF);
    UINT strides[2]={sizeof(float)*2, sizeof(CreatureInstance)}, offsets[2]={0,0};
    ID3D11Buffer* vbs[2]={creatureQuadVB, creatureInstanceVB};
    ctx->IASetVertexBuffers(0, 2, vbs, strides, offsets);
    ctx->DrawInstanced(4, (UINT)count, 0, 0);
    ctx->OMSetBlendState(nullptr, bf, 0xFFFFFFFF);
}

// ── Camera movement ───────────────────────────────────────────────────────────
void Renderer::tickCamera(float dt, bool playerMode) {
    if (playerMode) return;
    float spd = 15.f * dt;
    Float3 f = camera.forward();
    // Right = cross(forward, up)
    Float3 r = {f.z, 0.f, -f.x};  // simplified for y-up world
    float rl = std::sqrt(r.x*r.x + r.z*r.z);
    if (rl > 1e-6f) { r.x/=rl; r.z/=rl; }

    camera.pos.x += (f.x * (moveKeys[0]-moveKeys[1]) + r.x * (moveKeys[3]-moveKeys[2])) * spd;
    camera.pos.y += (moveKeys[4] - moveKeys[5]) * spd;
    camera.pos.z += (f.z * (moveKeys[0]-moveKeys[1]) + r.z * (moveKeys[3]-moveKeys[2])) * spd;
}

void Renderer::onMouseMove(int dx, int dy, bool rightDown) {
    if (!rightDown) return;
    camera.yaw   += dx * 0.003f;
    camera.pitch += dy * 0.003f;
    camera.pitch  = std::max(-1.5f, std::min(0.2f, camera.pitch));
}

void Renderer::onKey(int vk, bool down) {
    float v = down ? 1.f : 0.f;
    switch (vk) {
        case 'W': moveKeys[0]=v; break; case 'S': moveKeys[1]=v; break;
        case 'A': moveKeys[2]=v; break; case 'D': moveKeys[3]=v; break;
        case 'Q': moveKeys[4]=v; break; case 'E': moveKeys[5]=v; break;
    }
}

// ── Shutdown ──────────────────────────────────────────────────────────────────
void Renderer::shutdown() {
    for (auto& cm : chunkMeshes) { safeRelease(cm.vb); safeRelease(cm.ib); }
    safeRelease(terrainVS);  safeRelease(terrainPS);
    safeRelease(creatureVS); safeRelease(creaturePS);
    safeRelease(terrainLayout); safeRelease(creatureLayout);
    safeRelease(cbFrame); safeRelease(creatureQuadVB); safeRelease(creatureInstanceVB);
    safeRelease(rsWireframe); safeRelease(rsSolid);
    safeRelease(dssDepth); safeRelease(bsAlpha);
    safeRelease(depthTex); safeRelease(depthDSV);
}
