#include "Renderer.hpp"
#include "Renderer_Shaders.hpp"
#include <d3dcompiler.h>

// ── Renderer_Init.cpp ─────────────────────────────────────────────────────────
// Covers: init, createShaders, createBuffers, createDepthBuffer, resize, shutdown.
// Everything that runs once at startup (or on resize) rather than every frame.

// ── compileShader ─────────────────────────────────────────────────────────────
// Compiles an HLSL source string into GPU bytecode (a "blob").
// entry  = which function in the HLSL is the entry point
// target = shader stage + version, e.g. "vs_5_0" or "ps_5_0"
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

// ── init ──────────────────────────────────────────────────────────────────────
bool Renderer::init(ID3D11Device* dev, ID3D11DeviceContext* c, int w, int h) {
    device = dev;
    ctx    = c;
    winW = w; winH = h;

    if (!createShaders())     return false;
    if (!createBuffers(w, h)) return false;

    // ── Rasterizer states ─────────────────────────────────────────────────────
    // Controls how triangles are converted to pixels: filled vs wireframe,
    // and whether back-facing triangles are skipped (culled).
    D3D11_RASTERIZER_DESC rd{};
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable       = TRUE;

    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_BACK;
    device->CreateRasterizerState(&rd, &rsSolid);

    rd.FillMode = D3D11_FILL_WIREFRAME; rd.CullMode = D3D11_CULL_NONE;
    device->CreateRasterizerState(&rd, &rsWireframe);

    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;  // both sides — for FOV cone
    device->CreateRasterizerState(&rd, &rsSolidNoCull);

    // ── Depth-stencil states ──────────────────────────────────────────────────
    // The depth buffer tracks "closest drawn pixel" per screen pixel so near
    // objects correctly occlude far ones.
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable    = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc      = D3D11_COMPARISON_LESS;
    device->CreateDepthStencilState(&dsd, &dssDepth);

    // Test-only (no write): used for transparent overlays (water, FOV cone).
    // They can be hidden by terrain but won't block each other.
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    device->CreateDepthStencilState(&dsd, &dssNoDepthWrite);

    // ── Alpha blend state ─────────────────────────────────────────────────────
    // output = src.rgb * src.alpha + dst.rgb * (1 - src.alpha)
    // Standard "paint over" transparency formula.
    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable           = TRUE;
    rt.SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    rt.DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp               = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha         = D3D11_BLEND_ONE;
    rt.DestBlendAlpha        = D3D11_BLEND_ZERO;
    rt.BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    device->CreateBlendState(&bd, &bsAlpha);

    return true;
}

// ── createShaders ─────────────────────────────────────────────────────────────
// Compiles all HLSL shaders and creates the input layouts that tell the GPU
// how to read vertex data from buffers (which bytes = position, normal, etc.).
bool Renderer::createShaders() {

    // ── Terrain ───────────────────────────────────────────────────────────────
    ID3DBlob* tvs = compileShader(TERRAIN_HLSL, "VSMain", "vs_5_0");
    ID3DBlob* tps = compileShader(TERRAIN_HLSL, "PSMain", "ps_5_0");
    if (!tvs || !tps) { if(tvs)tvs->Release(); if(tps)tps->Release(); return false; }
    device->CreateVertexShader(tvs->GetBufferPointer(), tvs->GetBufferSize(), nullptr, &terrainVS);
    device->CreatePixelShader (tps->GetBufferPointer(), tps->GetBufferSize(), nullptr, &terrainPS);

    // Input layout: maps TerrainVertex struct fields → HLSL VIn semantics.
    // Format R32G32B32_FLOAT = 3 floats = 12 bytes (XYZ).
    // AlignedByteOffset: byte position of this field within the struct.
    D3D11_INPUT_ELEMENT_DESC td[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device->CreateInputLayout(td, 3, tvs->GetBufferPointer(), tvs->GetBufferSize(), &terrainLayout);
    tvs->Release(); tps->Release();

    // ── Creature billboards (instanced) ───────────────────────────────────────
    // Two buffer slots:
    //   slot 0: per-vertex quad corners (PER_VERTEX_DATA)
    //   slot 1: per-creature data — pos, yaw, colour, size (PER_INSTANCE_DATA)
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

    // ── Simple position-only (water + FOV cone) ───────────────────────────────
    ID3DBlob* svs = compileShader(SIMPLE_HLSL, "VSMain",  "vs_5_0");
    ID3DBlob* wps = compileShader(SIMPLE_HLSL, "WaterPS", "ps_5_0");
    ID3DBlob* fps = compileShader(SIMPLE_HLSL, "FovPS",   "ps_5_0");
    if (!svs || !wps || !fps) {
        if(svs)svs->Release(); if(wps)wps->Release(); if(fps)fps->Release();
        return false;
    }
    device->CreateVertexShader(svs->GetBufferPointer(), svs->GetBufferSize(), nullptr, &simpleVS);
    device->CreatePixelShader (wps->GetBufferPointer(), wps->GetBufferSize(), nullptr, &waterPS);
    device->CreatePixelShader (fps->GetBufferPointer(), fps->GetBufferSize(), nullptr, &fovPS);

    D3D11_INPUT_ELEMENT_DESC sd[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    device->CreateInputLayout(sd, 1, svs->GetBufferPointer(), svs->GetBufferSize(), &simpleLayout);
    svs->Release(); wps->Release(); fps->Release();

    return true;
}

// ── createBuffers ─────────────────────────────────────────────────────────────
bool Renderer::createBuffers(int w, int h) {
    D3D11_BUFFER_DESC bd{};

    // Per-frame constant buffer: written by CPU every frame, read by both shaders.
    // Must be a multiple of 16 bytes — GPU alignment requirement.
    bd.ByteWidth      = sizeof(FrameConstants);
    bd.Usage          = D3D11_USAGE_DYNAMIC;        // CPU writes every frame
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&bd, nullptr, &cbFrame))) {
        OutputDebugStringA("Error: Failed to create frame constant buffer!\n");
        return false;
    }

    // Creature quad: 4 corners of a unit quad [-0.5,0.5].
    // IMMUTABLE = never changes; fastest GPU read mode.
    // Laid out as TRIANGLE_STRIP: BL, BR, TL, TR → two triangles.
    float quad[] = {-0.5f,0.5f, 0.5f,0.5f, -0.5f,-0.5f, 0.5f,-0.5f}; // TL,TR,BL,BR
    bd.ByteWidth      = sizeof(quad);
    bd.Usage          = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = 0;
    D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = quad;
    device->CreateBuffer(&bd, &sd, &creatureQuadVB);

    // Creature instance buffer: one entry per living creature, rebuilt every frame.
    // Pre-allocated for up to MAX_CREATURES creatures (increase if needed).
    bd.ByteWidth      = (UINT)(sizeof(CreatureInstance) * MAX_CREATURES);
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    device->CreateBuffer(&bd, nullptr, &creatureInstanceVB);

    // FOV cone: also dynamic — rebuilt each frame as selected creature moves.
    bd.ByteWidth = (UINT)(sizeof(SimpleVertex) * FOV_CONE_MAX_VERTS);
    device->CreateBuffer(&bd, nullptr, &fovConeVB);

    return createDepthBuffer(w, h);
}

// ── createDepthBuffer ─────────────────────────────────────────────────────────
// A D32_FLOAT texture the same size as the screen.
// Stores the depth of the closest drawn pixel at each screen position.
// Must be recreated whenever the window is resized.
bool Renderer::createDepthBuffer(int w, int h) {
    safeRelease(depthTex);
    safeRelease(depthDSV);

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = (UINT)w;
    td.Height           = (UINT)h;
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

void Renderer::resize(int w, int h) {
    winW = w; winH = h;
    createDepthBuffer(w, h);
}

// ── shutdown ──────────────────────────────────────────────────────────────────
// D3D11 objects use COM reference counting. Release() decrements the count;
// the object is freed when it reaches zero. safeRelease() also nulls the pointer.
void Renderer::shutdown() {
    for (auto& cm : chunkMeshes) { safeRelease(cm.vb); safeRelease(cm.ib); }
    safeRelease(terrainVS);  safeRelease(terrainPS);
    safeRelease(creatureVS); safeRelease(creaturePS);
    safeRelease(simpleVS);   safeRelease(waterPS);   safeRelease(fovPS);
    safeRelease(terrainLayout); safeRelease(creatureLayout); safeRelease(simpleLayout);
    safeRelease(cbFrame);
    safeRelease(creatureQuadVB); safeRelease(creatureInstanceVB);
    safeRelease(waterVB);    safeRelease(fovConeVB);
    safeRelease(rsWireframe); safeRelease(rsSolid); safeRelease(rsSolidNoCull);
    safeRelease(dssDepth);   safeRelease(dssNoDepthWrite);
    safeRelease(bsAlpha);
    safeRelease(depthTex);   safeRelease(depthDSV);
}
