// ── PlanetRenderer.cpp ───────────────────────────────────────────────────────
// D3D11 initialisation, frame constant upload, and draw calls for the planet.

#include "PlanetRenderer.hpp"
#include "PlanetShaders.hpp"
#include "PlanetNoise.hpp"
#include <d3dcompiler.h>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

#include "imgui.hpp"   // for drawDebugUI
#include "Renderer/Renderer.hpp"

// ── Shader compilation helper ────────────────────────────────────────────────
static ID3DBlob* compileShader(const char* src, const char* entry, const char* target) {
    ID3DBlob *blob = nullptr, *err = nullptr;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &blob, &err);
    if (FAILED(hr)) {
        if (err) {
            OutputDebugStringA((const char*)err->GetBufferPointer());
            err->Release();
        }
        return nullptr;
    }
    if (err) err->Release();
    return blob;
}

// ── init ─────────────────────────────────────────────────────────────────────
bool PlanetRenderer::init(ID3D11Device* dev, ID3D11DeviceContext* c,
                          const PlanetConfig& config) {
    device = dev;
    ctx    = c;
    cfg    = config;

    // Seed the 3D noise (uses planet seed derived from cfg)
    PlanetNoise::init(42ULL);

    // Build the quadtree (allocates 6 root nodes, no GPU buffers yet)
    tree = new PlanetQuadTree(cfg);

    if (!compileShaders())     return false;
    if (!createBuffers())      return false;
    if (!createAtmosphere())   return false;
    if (!createSunQuad())      return false;
    if (!createRenderStates()) return false;
    return true;
}

// ── compileShaders ────────────────────────────────────────────────────────────
bool PlanetRenderer::compileShaders() {
    // ── Planet terrain shader ─────────────────────────────────────────────────
    ID3DBlob* tvs = compileShader(PLANET_HLSL, "VSMain", "vs_5_0");
    ID3DBlob* tps = compileShader(PLANET_HLSL, "PSMain", "ps_5_0");
    if (!tvs || !tps) {
        if (tvs) tvs->Release(); if (tps) tps->Release();
        return false;
    }
    device->CreateVertexShader(tvs->GetBufferPointer(), tvs->GetBufferSize(), nullptr, &terrainVS);
    device->CreatePixelShader (tps->GetBufferPointer(), tps->GetBufferSize(), nullptr, &terrainPS);

    // Input layout matching PlanetVertex: pos(3), nrm(3), uv(2), height(1), pad(1)
    D3D11_INPUT_ELEMENT_DESC ld[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT,          0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT,          0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device->CreateInputLayout(ld, 5, tvs->GetBufferPointer(), tvs->GetBufferSize(), &layout);
    tvs->Release(); tps->Release();

    // ── Atmosphere shell shader ───────────────────────────────────────────────
    ID3DBlob* avs = compileShader(PLANET_ATMO_HLSL, "VSAtmo", "vs_5_0");
    ID3DBlob* aps = compileShader(PLANET_ATMO_HLSL, "PSAtmo", "ps_5_0");
    if (!avs || !aps) {
        if (avs) avs->Release(); if (aps) aps->Release();
        return false;
    }
    device->CreateVertexShader(avs->GetBufferPointer(), avs->GetBufferSize(), nullptr, &atmoVS);
    device->CreatePixelShader (aps->GetBufferPointer(), aps->GetBufferSize(), nullptr, &atmoPS);
    avs->Release(); aps->Release();

    // ── Sun billboard ─────────────────────────────────────────────────────────
    ID3DBlob* svs = compileShader(SUN_HLSL, "SunVS", "vs_5_0");
    ID3DBlob* sps = compileShader(SUN_HLSL, "SunPS", "ps_5_0");
    if (!svs || !sps) { if(svs) svs->Release(); if(sps) sps->Release(); return false; }
    device->CreateVertexShader(svs->GetBufferPointer(), svs->GetBufferSize(), nullptr, &sunVS);
    device->CreatePixelShader (sps->GetBufferPointer(), sps->GetBufferSize(), nullptr, &sunPS);

    D3D11_INPUT_ELEMENT_DESC sunLD[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device->CreateInputLayout(sunLD, 1, svs->GetBufferPointer(), svs->GetBufferSize(), &sunLayout);
    svs->Release(); sps->Release();

    return true;
}

// ── createBuffers ─────────────────────────────────────────────────────────────
bool PlanetRenderer::createBuffers() {
    D3D11_BUFFER_DESC bd{};

    // Frame constants (b0) — same layout as the world renderer's FrameConstants
    bd.ByteWidth      = sizeof(FrameConstants);
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&bd, nullptr, &cbFrame))) return false;

    // Planet constants (b1) — per-draw planet-specific data
    bd.ByteWidth = sizeof(PlanetConstants);
    if (FAILED(device->CreateBuffer(&bd, nullptr, &cbPlanet))) return false;

    return true;
}

// ── createAtmosphere ─────────────────────────────────────────────────────────
// Generates a UV-sphere slightly larger than the planet to act as the
// atmospheric shell. Simple Blinn-Phong / Fresnel in the atmosphere shader
// gives a convincing glowing limb effect when viewed from space.
bool PlanetRenderer::createAtmosphere() {
    const int stacks = 32, slices = 48;
    float radius_atmosphere = cfg.radius * 1.1f;

    std::vector<float> verts;
    verts.reserve(stacks * slices * 3);

    for (int i = 0; i <= stacks; i++) {
        float phi = 3.14159265f * (float)i / stacks;   // 0 → π  (pole to pole)
        for (int j = 0; j <= slices; j++) {
            float theta = 2.f * 3.14159265f * (float)j / slices;
            float x = std::sin(phi) * std::cos(theta);
            float y = std::cos(phi);
            float z = std::sin(phi) * std::sin(theta);
            verts.push_back(cfg.center.x + x * radius_atmosphere);
            verts.push_back(cfg.center.y + y * radius_atmosphere);
            verts.push_back(cfg.center.z + z * radius_atmosphere);
        }
    }

    std::vector<uint32_t> idxs;
    int rowLen = slices + 1;
    for (int i = 0; i < stacks; i++) {
        for (int j = 0; j < slices; j++) {
            uint32_t TL = i * rowLen + j;
            uint32_t TR = TL + 1;
            uint32_t BL = TL + rowLen;
            uint32_t BR = BL + 1;
            idxs.push_back(TL); idxs.push_back(TR); idxs.push_back(BL);
            idxs.push_back(TR); idxs.push_back(BR); idxs.push_back(BL);
        }
    }

    D3D11_BUFFER_DESC bd{}; D3D11_SUBRESOURCE_DATA sd{};
    bd.Usage     = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.ByteWidth = (UINT)(verts.size() * sizeof(float));
    sd.pSysMem   = verts.data();
    if (FAILED(device->CreateBuffer(&bd, &sd, &atmoVB))) return false;

    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.ByteWidth = (UINT)(idxs.size() * sizeof(uint32_t));
    sd.pSysMem   = idxs.data();
    if (FAILED(device->CreateBuffer(&bd, &sd, &atmoIB))) return false;

    atmoIdxCount = (int)idxs.size();
    return true;
}

// ── createSunQuad ─────────────────────────────────────────────────────────────
// Simple 4-corner quad: TL, TR, BL, BR  (TRIANGLE_STRIP winding)
bool PlanetRenderer::createSunQuad() {
    float quad[] = { -0.5f, 0.5f,   0.5f, 0.5f,   -0.5f, -0.5f,   0.5f, -0.5f };
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = sizeof(quad);
    bd.Usage          = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{ quad };
    return SUCCEEDED(device->CreateBuffer(&bd, &sd, &sunQuadVB));
}

// ── createRenderStates ────────────────────────────────────────────────────────
bool PlanetRenderer::createRenderStates() {
    D3D11_RASTERIZER_DESC rd{};
    rd.DepthClipEnable = TRUE;

    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
    device->CreateRasterizerState(&rd, &rsSolid);

    rd.CullMode = D3D11_CULL_NONE;
    device->CreateRasterizerState(&rd, &rsSolidNoCull);

    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable    = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc      = D3D11_COMPARISON_LESS;
    device->CreateDepthStencilState(&dsd, &dssDepth);

    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    device->CreateDepthStencilState(&dsd, &dssNoWrite);

    // Sun: skip depth test entirely so it always appears in the sky
    dsd.DepthEnable    = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    device->CreateDepthStencilState(&dsd, &dssNoDepth);

    // Standard alpha blend for atmosphere
    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable    = TRUE;
    rt.SrcBlend       = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp        = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha  = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha   = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bd, &bsAlpha);

    // Additive blend: src + dst  (great for glowing sun disc)
    rt.SrcBlend  = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_ONE;
    device->CreateBlendState(&bd, &bsAdditive);

    rt.BlendEnable = FALSE;
    device->CreateBlendState(&bd, &bsOpaque);

    return true;
}

// ── update ────────────────────────────────────────────────────────────────────
// Drives the quadtree LOD split/merge. Must run before render().
void PlanetRenderer::update(const Camera& cam) {
    Vec3 cp = {cam.pos.x, cam.pos.y, cam.pos.z};
    tree->update(cp, device, ctx);
    totalNodes  = tree->totalNodes();
    totalLeaves = tree->totalLeaves();
}

// ── uploadFrameConstants ──────────────────────────────────────────────────────
// Populates b0 with camera, lighting, and fog data.
// Uses a simple sun model (same as world terrain) so the planet matches lighting.
void PlanetRenderer::uploadFrameConstants(const Camera& cam, float aspect,
                                          float timeOfDay, float simTime) {
    Mat4 view = cam.viewMatrix();

    // Use a planet-specific projection with a large far plane.
    // The planet centre is ~1840 units below the camera; the far side is ~2840 units.
    // The standard camera far plane of 1000 clips most of the sphere away.
    // near=10 is fine here because the planet surface is never closer than ~800 units.
    float planetFar = cfg.radius * 4.f + 1000.f;   // e.g. 5000 for radius=1000
    Mat4 proj = Mat4::perspectiveRH(
        cam.fovY * 3.14159265f / 180.f, aspect, 10.f, planetFar);

    Mat4 vp   = (view * proj).transposed();

    D3D11_MAPPED_SUBRESOURCE ms{};
    ctx->Map(cbFrame, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    auto* fc = (FrameConstants*)ms.pData;

    memcpy(fc->viewProj, vp.m, sizeof(vp.m));
    fc->camPos[0] = cam.pos.x; fc->camPos[1] = cam.pos.y;
    fc->camPos[2] = cam.pos.z; fc->camPos[3] = 0.f;

    // Sun position / colour (simplified version of the world's computeDayNightLighting)
    const float PI = 3.14159265f;
    float phase    = timeOfDay * 2.f * PI;
    float elev     = -std::cos(phase);

    fc->lightDir[0] = std::sin(phase) * 0.6f;
    fc->lightDir[1] = -elev;
    fc->lightDir[2] = 0.3f; fc->lightDir[3] = 0.f;
    float ll = std::sqrt(fc->lightDir[0]*fc->lightDir[0]
                       + fc->lightDir[1]*fc->lightDir[1]
                       + fc->lightDir[2]*fc->lightDir[2]);
    if (ll > 1e-6f) {
        fc->lightDir[0]/=ll; fc->lightDir[1]/=ll; fc->lightDir[2]/=ll;
    }

    float sunStr  = std::max(0.f, elev);
    fc->sunColor[0] = 1.f * sunStr;
    fc->sunColor[1] = 0.92f * sunStr;
    fc->sunColor[2] = 0.75f * sunStr;
    fc->sunColor[3] = timeOfDay;

    float ambStr = 0.08f + sunStr * 0.25f;
    fc->ambientColor[0] = 0.15f * ambStr + 0.05f;
    fc->ambientColor[1] = 0.22f * ambStr + 0.05f;
    fc->ambientColor[2] = 0.35f * ambStr + 0.08f;
    fc->ambientColor[3] = simTime;

    fc->fowData[3] = 0.f;  // disable fog of war on planet (not meaningful)

    ctx->Unmap(cbFrame, 0);

    ctx->VSSetConstantBuffers(0, 1, &cbFrame);
    ctx->PSSetConstantBuffers(0, 1, &cbFrame);
}

// ── uploadPlanetConstants ─────────────────────────────────────────────────────
void PlanetRenderer::uploadPlanetConstants(float timeOfDay) {
    const float PI    = 3.14159265f;
    float phase       = timeOfDay * 2.f * PI;
    float elevation   = -std::cos(phase);   // [-1, 1], positive = sun above horizon

    // Sun direction FROM the scene TOWARD the sun = -lightDir (normalised)
    float ldx = std::sin(phase) * 0.6f;
    float ldy = -elevation;
    float ldz = 0.3f;
    float ll  = std::sqrt(ldx*ldx + ldy*ldy + ldz*ldz);
    if (ll > 1e-6f) { ldx/=ll; ldy/=ll; ldz/=ll; }
    float sdx = -ldx, sdy = -ldy, sdz = -ldz;  // scene→sun

    D3D11_MAPPED_SUBRESOURCE ms{};
    ctx->Map(cbPlanet, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    auto* pc = (PlanetConstants*)ms.pData;

    pc->planetCenter[0] = cfg.center.x;
    pc->planetCenter[1] = cfg.center.y;
    pc->planetCenter[2] = cfg.center.z;
    pc->planetCenter[3] = cfg.radius;

    // Atmosphere: pale blue tint, thickness ~8% of planet radius
    pc->atmosphereColor[0] = 0.35f;
    pc->atmosphereColor[1] = 0.58f;
    pc->atmosphereColor[2] = 0.92f;
    pc->atmosphereColor[3] = cfg.radius * 0.08f;  // fog thickness

    pc->planetParams[0] = cfg.center.y;    // sea level Y
    pc->planetParams[1] = cfg.snowLine;
    pc->planetParams[2] = 0.f;
    pc->planetParams[3] = 0.f;

    pc->sunInfo[0] = sdx;
    pc->sunInfo[1] = sdy;
    pc->sunInfo[2] = sdz;
    pc->sunInfo[3] = elevation;   // [-1, 1]

    ctx->Unmap(cbPlanet, 0);

    ctx->VSSetConstantBuffers(1, 1, &cbPlanet);
    ctx->PSSetConstantBuffers(1, 1, &cbPlanet);
}

// ── renderPatches ─────────────────────────────────────────────────────────────
// Issues one DrawIndexed call per visible leaf node.
// All leaves share the same shader and input layout — only the VB/IB differ.
void PlanetRenderer::renderPatches() {
    ctx->RSSetState(wireframe ? nullptr : rsSolid);
    ctx->IASetInputLayout(layout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(terrainVS, nullptr, 0);
    ctx->PSSetShader(terrainPS, nullptr, 0);

    ctx->OMSetDepthStencilState(dssDepth, 0);
    float bf[4] = {};
    ctx->OMSetBlendState(bsOpaque, bf, 0xFFFFFFFF);

    if (wireframe) {
        // Use wireframe rasterizer state
        D3D11_RASTERIZER_DESC wrd{};
        wrd.FillMode = D3D11_FILL_WIREFRAME;
        wrd.CullMode = D3D11_CULL_NONE;
        wrd.DepthClipEnable = TRUE;
        ID3D11RasterizerState* rsWire = nullptr;
        device->CreateRasterizerState(&wrd, &rsWire);
        if (rsWire) { ctx->RSSetState(rsWire); rsWire->Release(); }
    } else {
        ctx->RSSetState(rsSolid);
    }

    std::vector<PlanetNode*> leaves;
    tree->collectLeaves(leaves);

    UINT stride = sizeof(PlanetVertex), offset = 0;
    for (PlanetNode* leaf : leaves) {
        if (!leaf->vb || !leaf->ib || leaf->idxCount == 0) continue;
        ctx->IASetVertexBuffers(0, 1, &leaf->vb, &stride, &offset);
        ctx->IASetIndexBuffer(leaf->ib, DXGI_FORMAT_R32_UINT, 0);
        ctx->DrawIndexed((UINT)leaf->idxCount, 0, 0);
    }
}

// ── renderAtmosphere ─────────────────────────────────────────────────────────
// Draws the transparent atmospheric shell last (after opaque planet surface)
// so additive blending composites correctly over the terrain.
void PlanetRenderer::renderAtmosphere(const Camera& /*cam*/) {
    if (!showAtmosphere || !atmoVB || wireframe) return;

    ctx->RSSetState(rsSolidNoCull);
    ctx->OMSetDepthStencilState(dssNoWrite, 0);  // depth test but no write (overlay)
    float bf[4] = {};
    ctx->OMSetBlendState(bsAlpha, bf, 0xFFFFFFFF);

    ctx->IASetInputLayout(layout);          // pos-only, reuse planet layout (first element)
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(atmoVS, nullptr, 0);
    ctx->PSSetShader(atmoPS, nullptr, 0);

    UINT stride = sizeof(float) * 3, offset = 0;
    ctx->IASetVertexBuffers(0, 1, &atmoVB, &stride, &offset);
    ctx->IASetIndexBuffer(atmoIB, DXGI_FORMAT_R32_UINT, 0);
    ctx->DrawIndexed((UINT)atmoIdxCount, 0, 0);

    ctx->OMSetBlendState(bsOpaque, bf, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(dssDepth, 0);
    ctx->RSSetState(rsSolid);
}

// ── renderSun ────────────────────────────────────────────────────────────────
// Draws a camera-facing billboard at the sun's position in the sky.
// Rendered with additive blending and no depth write so it composites cleanly
// over the space-black background without blocking anything.
void PlanetRenderer::renderSun() {
    if (!showSun || !sunQuadVB || wireframe) return;

    ctx->RSSetState(rsSolidNoCull);
    ctx->OMSetDepthStencilState(dssNoDepth, 0);      // always on top of depth
    float bf[4] = {};
    ctx->OMSetBlendState(bsAdditive, bf, 0xFFFFFFFF); // additive for the glow

    ctx->IASetInputLayout(sunLayout);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(sunVS, nullptr, 0);
    ctx->PSSetShader(sunPS, nullptr, 0);

    UINT stride = sizeof(float) * 2, offset = 0;
    ctx->IASetVertexBuffers(0, 1, &sunQuadVB, &stride, &offset);
    ctx->Draw(4, 0);

    // Restore states
    ctx->OMSetBlendState(bsOpaque, bf, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(dssDepth, 0);
    ctx->RSSetState(rsSolid);
}

// ── render ────────────────────────────────────────────────────────────────────
void PlanetRenderer::render(const Camera& cam, float aspect,
                            float timeOfDay, float simTime) {
    uploadFrameConstants(cam, aspect, timeOfDay, simTime);
    uploadPlanetConstants(timeOfDay);

    renderPatches();       // opaque terrain
    renderAtmosphere(cam); // transparent atmosphere shell
    renderSun();           // additive sun disc (always last so glow is on top)
}

// ── drawDebugUI ───────────────────────────────────────────────────────────────
// Call this inside an existing ImGui::Begin() / End() block.
void PlanetRenderer::drawDebugUI() {
    ImGui::SeparatorText("Planet QuadTree");
    ImGui::Text("Nodes (total / leaves): %d / %d", totalNodes, totalLeaves);

    ImGui::Checkbox("Wireframe##planet",     &wireframe);
    ImGui::Checkbox("Atmosphere##planet",    &showAtmosphere);
    ImGui::Checkbox("Sun##planet",        &showSun);

    ImGui::SliderFloat("Split Threshold##planet", &cfg.splitThreshold, 0.3f, 3.f);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Lower = finer LOD (more nodes, higher quality).\n"
                          "Higher = coarser LOD (fewer nodes, faster rendering).");

    ImGui::SliderInt("Max Depth##planet", &cfg.maxDepth, 4, 22);
    ImGui::SliderFloat("Height Scale##planet", &cfg.heightScale, 0.f, cfg.radius * 0.2f);
    ImGui::SliderFloat("Noise Frequency##planet", &cfg.noiseFrequency, 0.1f, 5.f);

    ImGui::TextDisabled("Planet radius: %.0f  Centre: (%.0f, %.0f, %.0f)",
        cfg.radius, cfg.center.x, cfg.center.y, cfg.center.z);
}

// ── shutdown ──────────────────────────────────────────────────────────────────
void PlanetRenderer::shutdown() {
    if (tree) { tree->shutdown(); delete tree; tree = nullptr; }

    safeRelease(terrainVS); safeRelease(terrainPS);
    safeRelease(atmoVS);    safeRelease(atmoPS);
    safeRelease(sunVS);     safeRelease(sunPS);
    safeRelease(layout);    safeRelease(sunLayout);
    safeRelease(cbFrame);   safeRelease(cbPlanet);
    safeRelease(atmoVB);    safeRelease(atmoIB);
    safeRelease(sunQuadVB);
    safeRelease(rsSolid);   safeRelease(rsSolidNoCull);
    safeRelease(dssDepth);  safeRelease(dssNoWrite);  safeRelease(dssNoDepth);
    safeRelease(bsAlpha);   safeRelease(bsAdditive);  safeRelease(bsOpaque);
}
