// ── PlanetRenderer.cpp ───────────────────────────────────────────────────────
// D3D11 initialisation, frame constant upload, and draw calls for the planet.

#include "PlanetRenderer.hpp"
#include "PlanetNoise.hpp"
#include "PlanetTextureLoader.hpp"
#include <d3dcompiler.h>
#include <cmath>
#include <vector>
#include <wrl/client.h>

#include "imgui.hpp"
#include "Core/file_management.hpp"
#include "Renderer/Renderer.hpp"
#include "World/World.hpp"
#include "World/World_Planet.hpp"

using Microsoft::WRL::ComPtr;

// ── Shader compilation helper ────────────────────────────────────────────────
static ComPtr<ID3DBlob> compileShaderFile(const wchar_t *filename, const char *entry, const char *target) {
    ComPtr<ID3DBlob> blob, err;

    std::wstring name(filename);

    // Check multiple directory levels to handle different IDE working directories
    std::vector candidates = {
        name,
        L"Shaders/" + name,
        L"../Shaders/" + name,
        L"../../Shaders/" + name,
        L"../../../Shaders/" + name
    };

    std::wstring finalPath = L"";

    for (const auto& p : candidates) {
        if (fileExists(p)) {
            finalPath = p;
            break;
        }
    }

    if (finalPath.empty()) {
        // Convert wchar_t* to char* for debug output
        char buf[512];
        size_t converted;
        wcstombs_s(&converted, buf, filename, 512);

        std::string msg = "FATAL: Shader file not found: " + std::string(buf) + "\n";
        OutputDebugStringA(msg.c_str());
        return nullptr;
    }

    HRESULT hr = D3DCompileFromFile(finalPath.c_str(), nullptr, nullptr, entry, target,
                                        D3DCOMPILE_OPTIMIZATION_LEVEL1, 0,
                                        blob.GetAddressOf(), err.GetAddressOf());
    if (FAILED(hr)) {
        if (err)
            OutputDebugStringA((const char *) err->GetBufferPointer());
        else
            OutputDebugStringA("Unknown D3DCompileFromFile error.\n");
        return nullptr;
    }
    return blob;
}

// ── init ─────────────────────────────────────────────────────────────────────
bool PlanetRenderer::init(ID3D11Device *dev, ID3D11DeviceContext *c,
                          const PlanetConfig &config) {
    device = dev;
    ctx = c;
    cfg = config;

    // Build the quadtree (allocates 6 root nodes, no GPU buffers yet)
    tree = new PlanetQuadTree(cfg);

    if (!compileShaders()) return false;
    if (!createBuffers()) return false;
    if (!createAtmosphere()) return false;
    if (!createSunQuad()) return false;
    if (!createRenderStates()) return false;
    if (!createTextureSampler()) return false;

    // Attempt to load textures from the executable's working directory.
    // If the files aren't present the renderer falls back to procedural colours.
    loadTextures(L"../textures/");

    return true;
}

// ── loadTextures ──────────────────────────────────────────────────────────────
// Expected file naming convention: {dir}{Terrain}_1K-JPG_{Type}.jpg
// E.g. "Textures/Grass_1K-JPG_Color.jpg"
//
// Biome–slot mapping:
//   Grass → t0/t4/t8/t12
//   Sand  → t1/t5/t9/t13
//   Rock  → t2/t6/t10/t14
//   Snow  → t3/t7/t11/t15
bool PlanetRenderer::loadTextures(const wchar_t *dir) {
    // CoInitializeEx is idempotent on the same thread; safe to call multiple times.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    struct TexEntry {
        int slot;
        bool isColor; // true → sRGB; false → linear
        const wchar_t *file;
    };

    const TexEntry entries[] = {
        // ── Colour (sRGB) ───────────────────────────────────────────────────
        {0, true, L"Grass_1K-JPG_Color.jpg"},
        {1, true, L"Sand_1K-JPG_Color.jpg"},
        {2, true, L"Rock_1K-JPG_Color.jpg"},
        {3, true, L"Snow_1K-JPG_Color.jpg"},
        // ── Normal GL (linear, Y-up) ────────────────────────────────────────
        {4, false, L"Grass_1K-JPG_NormalGL.jpg"},
        {5, false, L"Sand_1K-JPG_NormalGL.jpg"},
        {6, false, L"Rock_1K-JPG_NormalGL.jpg"},
        {7, false, L"Snow_1K-JPG_NormalGL.jpg"},
        // ── AO (linear) ─────────────────────────────────────────────────────
        {8, false, L"Grass_1K-JPG_AmbientOcclusion.jpg"},
        {9, false, L"Sand_1K-JPG_AmbientOcclusion.jpg"},
        {10, false, L"Rock_1K-JPG_AmbientOcclusion.jpg"},
        {11, false, L"Snow_1K-JPG_AmbientOcclusion.jpg"},
        // ── Roughness (linear) ──────────────────────────────────────────────
        {12, false, L"Grass_1K-JPG_Roughness.jpg"},
        {13, false, L"Sand_1K-JPG_Roughness.jpg"},
        {14, false, L"Rock_1K-JPG_Roughness.jpg"},
        {15, false, L"Snow_1K-JPG_Roughness.jpg"},
    };

    int loaded = 0;
    for (const auto &e: entries) {
        // Build full path
        std::wstring fullPath = std::wstring(dir) + e.file;

        bool ok;
        if (e.isColor)
            ok = LoadColorTextureFromFile(device.Get(), ctx.Get(),
                                          fullPath.c_str(), texSRVs[e.slot]);
        else
            ok = LoadTextureFromFile(device.Get(), ctx.Get(),
                                     fullPath.c_str(), texSRVs[e.slot]);

        if (ok) {
            loaded++;
        } else {
            char buf[256];
            sprintf_s(buf, "PlanetRenderer: failed to load texture slot %d: %S\n",
                      e.slot, fullPath.c_str());
            OutputDebugStringA(buf);
        }
    }

    // Consider textures "loaded" if at least the 4 colour maps are present
    texturesLoaded = (texSRVs[0] && texSRVs[1] && texSRVs[2] && texSRVs[3]);

    char buf[64];
    sprintf_s(buf, "PlanetRenderer: %d/%d textures loaded\n", loaded, (int) std::size(entries));
    OutputDebugStringA(buf);

    return texturesLoaded;
}

// ── createTextureSampler ──────────────────────────────────────────────────────
// Creates an anisotropic (up to 16×) wrap sampler for the terrain textures.
// Anisotropic filtering is important on terrain viewed at grazing angles.
bool PlanetRenderer::createTextureSampler() {
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_ANISOTROPIC;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxAnisotropy = 16;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD = 0.f;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    return SUCCEEDED(device->CreateSamplerState(&sd, texSampler.GetAddressOf()));
}

// ── bindTerrainTextures / unbindTerrainTextures ───────────────────────────────
void PlanetRenderer::bindTerrainTextures() {
    // Build a flat array of raw SRV pointers for a single SetShaderResources call
    ID3D11ShaderResourceView *srvs[TEX_COUNT] = {};
    for (int i = 0; i < TEX_COUNT; i++)
        srvs[i] = texSRVs[i].Get(); // nullptr if not loaded (HLSL reads 0)

    ctx->PSSetShaderResources(0, TEX_COUNT, srvs);

    // Bind the anisotropic sampler to s0
    ctx->PSSetSamplers(0, 1, texSampler.GetAddressOf());
}

void PlanetRenderer::unbindTerrainTextures() {
    // Unbind all 16 slots so other draw passes don't accidentally read stale SRVs
    ID3D11ShaderResourceView *nullSRVs[TEX_COUNT] = {};
    ctx->PSSetShaderResources(0, TEX_COUNT, nullSRVs);
}

// ── compileShaders ────────────────────────────────────────────────────────────
bool PlanetRenderer::compileShaders() {
    // ── Planet terrain shader ─────────────────────────────────────────────────
    auto tvs = compileShaderFile(L"Planet.hlsl", "VSMain", "vs_5_0");
    auto tps = compileShaderFile(L"Planet.hlsl", "PSMain", "ps_5_0");
    if (!tvs || !tps) return false;

    device->CreateVertexShader(tvs->GetBufferPointer(), tvs->GetBufferSize(), nullptr, terrainVS.GetAddressOf());
    device->CreatePixelShader(tps->GetBufferPointer(), tps->GetBufferSize(), nullptr, terrainPS.GetAddressOf());

    // Input layout matching PlanetVertex: pos(3), nrm(3), uv(2), height(1), pad(1)
    D3D11_INPUT_ELEMENT_DESC ld[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 2, DXGI_FORMAT_R32_FLOAT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device->CreateInputLayout(ld, 5, tvs->GetBufferPointer(), tvs->GetBufferSize(), layout.GetAddressOf());

    // ── Atmosphere shell shader ───────────────────────────────────────────────
    auto avs = compileShaderFile(L"PlanetAtmo.hlsl", "VSAtmo", "vs_5_0");
    auto aps = compileShaderFile(L"PlanetAtmo.hlsl", "PSAtmo", "ps_5_0");
    if (!avs || !aps) return false;

    device->CreateVertexShader(avs->GetBufferPointer(), avs->GetBufferSize(), nullptr, atmoVS.GetAddressOf());
    device->CreatePixelShader(aps->GetBufferPointer(), aps->GetBufferSize(), nullptr, atmoPS.GetAddressOf());

    // ── Sun billboard ─────────────────────────────────────────────────────────
    auto svs = compileShaderFile(L"Sun.hlsl", "SunVS", "vs_5_0");
    auto sps = compileShaderFile(L"Sun.hlsl", "SunPS", "ps_5_0");
    if (!svs || !sps) return false;

    device->CreateVertexShader(svs->GetBufferPointer(), svs->GetBufferSize(), nullptr, sunVS.GetAddressOf());
    device->CreatePixelShader(sps->GetBufferPointer(), sps->GetBufferSize(), nullptr, sunPS.GetAddressOf());

    D3D11_INPUT_ELEMENT_DESC sunLD[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device->CreateInputLayout(sunLD, 1, svs->GetBufferPointer(), svs->GetBufferSize(), sunLayout.GetAddressOf());

    // ── Star shader ───────────────────────────────────────────────────────────
    auto stvs = compileShaderFile(L"Star.hlsl", "StarVS", "vs_5_0");
    auto stps = compileShaderFile(L"Star.hlsl", "StarPS", "ps_5_0");
    if (!stvs || !stps) return false;
    device->CreateVertexShader(stvs->GetBufferPointer(), stvs->GetBufferSize(), nullptr, starVS.GetAddressOf());
    device->CreatePixelShader(stps->GetBufferPointer(), stps->GetBufferSize(), nullptr, starPS.GetAddressOf());


    return true;
}

// ── createBuffers ─────────────────────────────────────────────────────────────
bool PlanetRenderer::createBuffers() {
    D3D11_BUFFER_DESC bd{};

    // Frame constants (b0) — same layout as the world renderer's FrameConstants
    bd.ByteWidth = sizeof(FrameConstants);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&bd, nullptr, cbFrame.GetAddressOf()))) return false;

    // Planet constants (b1) — per-draw planet-specific data
    bd.ByteWidth = sizeof(PlanetConstants);
    if (FAILED(device->CreateBuffer(&bd, nullptr, cbPlanet.GetAddressOf()))) return false;

    return true;
}

// ── createAtmosphere ─────────────────────────────────────────────────────────
// Generates a UV-sphere slightly larger than the planet to act as the
// atmospheric shell. Simple Blinn-Phong / Fresnel in the atmosphere shader
// gives a convincing glowing limb effect when viewed from space.
bool PlanetRenderer::createAtmosphere() {
    const int stacks = 32, slices = 48;
    float radius_atmosphere = cfg.radius * 1.3f;

    std::vector<float> verts;
    verts.reserve(stacks * slices * 3);

    for (int i = 0; i <= stacks; i++) {
        float phi = 3.14159265f * (float) i / stacks; // 0 → π  (pole to pole)
        for (int j = 0; j <= slices; j++) {
            float theta = 2.f * 3.14159265f * (float) j / slices;
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
            idxs.push_back(TL);
            idxs.push_back(TR);
            idxs.push_back(BL);
            idxs.push_back(TR);
            idxs.push_back(BR);
            idxs.push_back(BL);
        }
    }

    D3D11_BUFFER_DESC bd{};
    D3D11_SUBRESOURCE_DATA sd{};
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.ByteWidth = (UINT) (verts.size() * sizeof(float));
    sd.pSysMem = verts.data();
    if (FAILED(device->CreateBuffer(&bd, &sd, atmoVB.GetAddressOf()))) return false;

    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd.ByteWidth = (UINT) (idxs.size() * sizeof(uint32_t));
    sd.pSysMem = idxs.data();
    if (FAILED(device->CreateBuffer(&bd, &sd, atmoIB.GetAddressOf()))) return false;

    atmoIdxCount = (int) idxs.size();
    return true;
}

// ── createSunQuad ─────────────────────────────────────────────────────────────
// Simple 4-corner quad: TL, TR, BL, BR  (TRIANGLE_STRIP winding)
bool PlanetRenderer::createSunQuad() {
    float quad[] = {-0.5f, 0.5f, 0.5f, 0.5f, -0.5f, -0.5f, 0.5f, -0.5f};
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(quad);
    bd.Usage = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA sd{quad};
    return SUCCEEDED(device->CreateBuffer(&bd, &sd, sunQuadVB.GetAddressOf()));
}

// ── createRenderStates ────────────────────────────────────────────────────────
bool PlanetRenderer::createRenderStates() {
    D3D11_RASTERIZER_DESC rd{};
    rd.DepthClipEnable = TRUE;

    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    device->CreateRasterizerState(&rd, rsSolid.GetAddressOf());

    rd.CullMode = D3D11_CULL_NONE;
    device->CreateRasterizerState(&rd, rsSolidNoCull.GetAddressOf());

    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc = D3D11_COMPARISON_LESS;
    device->CreateDepthStencilState(&dsd, dssDepth.GetAddressOf());

    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    device->CreateDepthStencilState(&dsd, dssNoWrite.GetAddressOf());

    // Sun: skip depth test entirely so it always appears in the sky
    dsd.DepthEnable = FALSE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    device->CreateDepthStencilState(&dsd, dssNoDepth.GetAddressOf());

    // Standard alpha blend for atmosphere
    D3D11_BLEND_DESC bd{};
    auto &rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bd, bsAlpha.GetAddressOf());

    // Additive blend: src + dst  (great for glowing sun disc)
    rt.SrcBlend = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend = D3D11_BLEND_ONE;
    device->CreateBlendState(&bd, bsAdditive.GetAddressOf());

    rt.BlendEnable = FALSE;
    device->CreateBlendState(&bd, bsOpaque.GetAddressOf());

    return true;
}

// ── update ────────────────────────────────────────────────────────────────────
// Drives the quadtree LOD split/merge. Must run before render().
void PlanetRenderer::update(const Camera &cam) {
    Vec3 cp = {cam.pos.x, cam.pos.y, cam.pos.z};
    tree->update(cp, device.Get(), ctx.Get());
    totalNodes = tree->totalNodes();
    totalLeaves = tree->totalLeaves();
}

// ── uploadFrameConstants ──────────────────────────────────────────────────────
// Populates b0 with camera, lighting, and fog data.
// Uses a simple sun model (same as world terrain) so the planet matches lighting.
void PlanetRenderer::uploadFrameConstants(const World &world, const Renderer &rend, float aspect) {
    Mat4 view = rend.camera.viewMatrix();

    // Use a planet-specific projection with a large far plane.
    // The planet centre is ~1840 units below the camera; the far side is ~2840 units.
    // The standard camera far plane of 1000 clips most of the sphere away.
    // near=10 is fine here because the planet surface is never closer than ~800 units.
    float planetFar = cfg.radius * 4.f + 1000.f; // e.g. 5000 for radius=1000
    Mat4 proj = rend.camera.projMatrix(aspect);

    Mat4 vp = (view * proj).transposed();

    D3D11_MAPPED_SUBRESOURCE ms{};
    ctx->Map(cbFrame.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    auto *fc = (FrameConstants *) ms.pData;

    memcpy(fc->viewProj, vp.m, sizeof(vp.m));
    fc->camPos[0] = rend.camera.pos.x;
    fc->camPos[1] = rend.camera.pos.y;
    fc->camPos[2] = rend.camera.pos.z;
    fc->camPos[3] = 0.f;

    // Sun position / colour (simplified version of the world's computeDayNightLighting)
    const float PI = 3.14159265f;
    float phase = world.timeOfDay() * 2.f * PI;
    float elev = -std::cos(phase);

    fc->lightDir[0] = std::sin(phase) * 0.6f;
    fc->lightDir[1] = -elev;
    fc->lightDir[2] = 0.3f;
    fc->lightDir[3] = 0.f;
    float ll = std::sqrt(fc->lightDir[0] * fc->lightDir[0]
                         + fc->lightDir[1] * fc->lightDir[1]
                         + fc->lightDir[2] * fc->lightDir[2]);
    if (ll > 1e-6f) {
        fc->lightDir[0] /= ll;
        fc->lightDir[1] /= ll;
        fc->lightDir[2] /= ll;
    }

    fc->sunColor[0] = 1.00f;
    fc->sunColor[1] = 0.95f;
    fc->sunColor[2] = 0.80f;
    fc->sunColor[3] = world.timeOfDay();

    fc->ambientColor[0] = 0.05f;
    fc->ambientColor[1] = 0.05f;
    fc->ambientColor[2] = 0.08f;
    fc->ambientColor[3] = world.simTime;

    fc->planetCenter[0] = cfg.center.x;
    fc->planetCenter[1] = cfg.center.y;
    fc->planetCenter[2] = cfg.center.z;
    fc->planetCenter[3] = cfg.radius;

    if (rend.showFogOfWar && rend.playerID != INVALID_ID) {
        auto it = world.idToIndex.find(rend.playerID);
        if (it != world.idToIndex.end()) {
            const Creature &pc = world.creatures[it->second];
            fc->fowData[0] = pc.pos.x;
            fc->fowData[1] = pc.pos.y;
            fc->fowData[2] = pc.pos.z;
            fc->fowData[3] = pc.genome.visionRange();

            Vec3 facing = {std::sin(pc.yaw), 0.f, std::cos(pc.yaw)};
            facing = g_planet_surface.projectToTangent(pc.pos, facing).normalised();
            fc->fowFacing[0] = facing.x;
            fc->fowFacing[1] = facing.y;
            fc->fowFacing[2] = facing.z;
            fc->fowFacing[3] = std::cos(pc.genome.visionFOV() * PI / 180.f * 0.5f);
        } else { fc->fowData[3] = 0.f; }
    } else { fc->fowData[3] = 0.f; }

    ctx->Unmap(cbFrame.Get(), 0);

    ctx->VSSetConstantBuffers(0, 1, cbFrame.GetAddressOf());
    ctx->PSSetConstantBuffers(0, 1, cbFrame.GetAddressOf());
}

// ── uploadPlanetConstants ─────────────────────────────────────────────────────
void PlanetRenderer::uploadPlanetConstants(float timeOfDay) {
    D3D11_MAPPED_SUBRESOURCE ms{};
    ctx->Map(cbPlanet.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &ms);
    auto *pc = (PlanetConstants *) ms.pData;

    // Atmosphere: pale blue tint, thickness ~8% of planet radius
    pc->atmosphereColor[0] = 0.35f;
    pc->atmosphereColor[1] = 0.58f;
    pc->atmosphereColor[2] = 0.92f;
    pc->atmosphereColor[3] = cfg.radius * 0.08f; // fog thickness

    pc->planetParams[0] = cfg.center.y; // sea level Y
    pc->planetParams[1] = cfg.snowLine;
    pc->planetParams[2] = 0.f;
    pc->planetParams[3] = 0.f;

    // Pass triplanar scale and texture-loaded flag to the shader
    pc->texParams[0] = triplanarScale;
    pc->texParams[1] = texturesLoaded ? 1.f : 0.f;
    pc->texParams[2] = 0.f;
    pc->texParams[3] = 0.f;

    ctx->Unmap(cbPlanet.Get(), 0);

    ctx->VSSetConstantBuffers(1, 1, cbPlanet.GetAddressOf());
    ctx->PSSetConstantBuffers(1, 1, cbPlanet.GetAddressOf());
}

// ── renderPatches ─────────────────────────────────────────────────────────────
// Issues one DrawIndexed call per visible leaf node.
// All leaves share the same shader and input layout — only the VB/IB differ.
void PlanetRenderer::renderPatches() {
    ctx->RSSetState(wireframe ? nullptr : rsSolid.Get());
    ctx->IASetInputLayout(layout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(terrainVS.Get(), nullptr, 0);
    ctx->PSSetShader(terrainPS.Get(), nullptr, 0);

    ctx->OMSetDepthStencilState(dssDepth.Get(), 0);
    float bf[4] = {};
    ctx->OMSetBlendState(bsOpaque.Get(), bf, 0xFFFFFFFF);

    if (wireframe) {
        // Use wireframe rasterizer state
        D3D11_RASTERIZER_DESC wrd{};
        wrd.FillMode = D3D11_FILL_WIREFRAME;
        wrd.CullMode = D3D11_CULL_NONE;
        wrd.DepthClipEnable = TRUE;
        ComPtr<ID3D11RasterizerState> rsWire;
        device->CreateRasterizerState(&wrd, rsWire.GetAddressOf());
        if (rsWire) { ctx->RSSetState(rsWire.Get()); }
    } else {
        ctx->RSSetState(rsSolid.Get());
    }

    // Bind textures + sampler before the draw calls
    bindTerrainTextures();

    std::vector<PlanetNode *> leaves;
    tree->collectLeaves(leaves);

    UINT stride = sizeof(PlanetVertex), offset = 0;
    for (PlanetNode *leaf: leaves) {
        if (!leaf->vb || !leaf->ib || leaf->idxCount == 0) continue;
        ctx->IASetVertexBuffers(0, 1, &leaf->vb, &stride, &offset);
        ctx->IASetIndexBuffer(leaf->ib, DXGI_FORMAT_R32_UINT, 0);
        ctx->DrawIndexed((UINT) leaf->idxCount, 0, 0);
    }

    // Unbind textures so subsequent passes don't read stale slots
    unbindTerrainTextures();
}

// ── renderAtmosphere ─────────────────────────────────────────────────────────
// Draws the transparent atmospheric shell last (after opaque planet surface)
// so additive blending composites correctly over the terrain.
void PlanetRenderer::renderAtmosphere(const Camera & /*cam*/) {
    if (!showAtmosphere || !atmoVB.Get() || wireframe) return;

    ctx->RSSetState(rsSolidNoCull.Get());
    ctx->OMSetDepthStencilState(dssNoWrite.Get(), 0); // depth test but no write (overlay)
    float bf[4] = {};
    ctx->OMSetBlendState(bsAlpha.Get(), bf, 0xFFFFFFFF);

    ctx->IASetInputLayout(layout.Get()); // pos-only, reuse planet layout (first element)
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(atmoVS.Get(), nullptr, 0);
    ctx->PSSetShader(atmoPS.Get(), nullptr, 0);

    UINT stride = sizeof(float) * 3, offset = 0;
    ctx->IASetVertexBuffers(0, 1, atmoVB.GetAddressOf(), &stride, &offset);
    ctx->IASetIndexBuffer(atmoIB.Get(), DXGI_FORMAT_R32_UINT, 0);
    ctx->DrawIndexed((UINT) atmoIdxCount, 0, 0);

    ctx->OMSetBlendState(bsOpaque.Get(), bf, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(dssDepth.Get(), 0);
    ctx->RSSetState(rsSolid.Get());
}

// ── renderSun ────────────────────────────────────────────────────────────────
// Draws a camera-facing billboard at the sun's position in the sky.
// Rendered with additive blending and no depth write so it composites cleanly
// over the space-black background without blocking anything.
void PlanetRenderer::renderSun() {
    if (!showSun || !sunQuadVB.Get() || wireframe) return;

    ctx->RSSetState(rsSolidNoCull.Get());
    ctx->OMSetDepthStencilState(dssNoWrite.Get(), 0); // test against terrain, don't write
    float bf[4] = {};
    ctx->OMSetBlendState(bsAdditive.Get(), bf, 0xFFFFFFFF); // additive for the glow

    ctx->IASetInputLayout(sunLayout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ctx->VSSetShader(sunVS.Get(), nullptr, 0);
    ctx->PSSetShader(sunPS.Get(), nullptr, 0);

    UINT stride = sizeof(float) * 2, offset = 0;
    ctx->IASetVertexBuffers(0, 1, sunQuadVB.GetAddressOf(), &stride, &offset);
    ctx->Draw(4, 0);

    // Restore states
    ctx->OMSetBlendState(bsOpaque.Get(), bf, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(dssDepth.Get(), 0);
    ctx->RSSetState(rsSolid.Get());
}

// ── renderStars ───────────────────────────────────────────────────────────────
void PlanetRenderer::renderStars() {
    if (wireframe) return;

    ctx->RSSetState(rsSolidNoCull.Get());
    ctx->OMSetDepthStencilState(dssNoWrite.Get(), 0); // depth test, no write
    float bf[4] = {};
    ctx->OMSetBlendState(bsAdditive.Get(), bf, 0xFFFFFFFF); // additive blend

    ctx->IASetInputLayout(layout.Get()); // reuse planet layout (pos is first)
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(starVS.Get(), nullptr, 0);
    ctx->PSSetShader(starPS.Get(), nullptr, 0);

    UINT stride = sizeof(float) * 3, offset = 0;
    ctx->IASetVertexBuffers(0, 1, atmoVB.GetAddressOf(), &stride, &offset);
    ctx->IASetIndexBuffer(atmoIB.Get(), DXGI_FORMAT_R32_UINT, 0);
    ctx->DrawIndexed((UINT) atmoIdxCount, 0, 0);

    // Restore states
    ctx->OMSetBlendState(bsOpaque.Get(), bf, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(dssDepth.Get(), 0);
    ctx->RSSetState(rsSolid.Get());
}

// ── render ────────────────────────────────────────────────────────────────────
void PlanetRenderer::render(const World &world, const Renderer &rend, float aspect) {
    uploadFrameConstants(world, rend, aspect);
    uploadPlanetConstants(world.timeOfDay());

    renderPatches(); // opaque terrain
    renderAtmosphere(rend.camera); // transparent atmosphere shell
    renderSun(); // additive sun disc (always last so glow is on top)
    renderStars(); // stars in the background
}

// ── drawDebugUI ───────────────────────────────────────────────────────────────
// Call this inside an existing ImGui::Begin() / End() block.
void PlanetRenderer::drawDebugUI() {
    ImGui::SeparatorText("Planet QuadTree");
    ImGui::Text("Nodes (total / leaves): %d / %d", totalNodes, totalLeaves);

    ImGui::Checkbox("Wireframe##planet", &wireframe);
    ImGui::Checkbox("Atmosphere##planet", &showAtmosphere);
    ImGui::Checkbox("Sun##planet", &showSun);

    ImGui::SeparatorText("Terrain Textures");
    if (texturesLoaded) {
        ImGui::TextColored({0.3f, 1.f, 0.3f, 1.f}, "Textures: loaded");
    } else {
        ImGui::TextColored({1.f, 0.6f, 0.2f, 1.f}, "Textures: not found (procedural fallback)");
        ImGui::TextDisabled("Place files in Textures/ beside the EXE.");
        ImGui::TextDisabled("E.g. Textures/Grass_1K-JPG_Color.jpg");
    }

    // Expose the triplanar scale so artists can tweak the tiling
    ImGui::SliderFloat("Triplanar Scale##planet", &triplanarScale, 0.00001f, 0.001f, "%.6f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Controls texture tile repeat size.\n"
            "Smaller = larger tiles (fewer repeats per km).\n"
            "Default 0.00015 gives ~1 tile per ~6.7 km.");

    // Reload textures at runtime (useful for hot-reloading during development)
    if (ImGui::Button("Reload Textures"))
        loadTextures(L"Textures/");

    ImGui::SeparatorText("LOD");
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
    if (tree) {
        tree->shutdown();
        delete tree;
        tree = nullptr;
    }
}
