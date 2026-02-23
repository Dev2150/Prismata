#include "Renderer.hpp"
#include "Shaders_HLSL.hpp"
#include <d3dcompiler.h>

// ── Renderer_Init.cpp ─────────────────────────────────────────────────────────
// Covers: init, createShaders, createBuffers, createDepthBuffer, resize, shutdown.
// Everything that runs once at startup (or on resize) rather than every frame.

// ── compileShader ─────────────────────────────────────────────────────────────
// Compiles an HLSL source string into GPU bytecode (a "blob").
// entry  = which function in the HLSL is the entry point
// target = shader stage + version, e.g. "vs_5_0" or "ps_5_0"
static ComPtr<ID3DBlob> compileShader(const char* src, const char* entry, const char* target) {
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> err;
    HRESULT hr = D3DCompile(src, strlen(src), nullptr, nullptr, nullptr,
                            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0,
                            blob.GetAddressOf(), err.GetAddressOf());
    if (FAILED(hr)) {
        if (err) { OutputDebugStringA((char*)err->GetBufferPointer()); }
        return nullptr;
    }
    return blob;
}

// ── init ──────────────────────────────────────────────────────────────────────
bool Renderer::init(ID3D11Device* dev, ID3D11DeviceContext* c, int w, int h) {
    device = dev;
    ctx    = c;
    winW = w; winH = h;

    char buf[128];
    sprintf_s(buf, "Renderer::init device=%p ctx=%p\n", dev, c);
    OutputDebugStringA(buf);

    if (!createShaders()) {
        OutputDebugStringA("createShaders FAILED\n");
        return false;
    }
    if (!createBuffers(w, h)) {
        OutputDebugStringA("createBuffers FAILED\n");
        return false;
    }

    // ── Rasterizer states ─────────────────────────────────────────────────────
    // Controls how triangles are converted to pixels: filled vs wireframe,
    // and whether back-facing triangles are skipped (culled).
    D3D11_RASTERIZER_DESC rd{};
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable       = TRUE;

    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_BACK;
    device->CreateRasterizerState(&rd, rsSolid.GetAddressOf());

    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;  // both sides — for FOV cone
    device->CreateRasterizerState(&rd, rsSolidNoCull.GetAddressOf());

    // ── Depth-stencil states ──────────────────────────────────────────────────
    // The depth buffer tracks "closest drawn pixel" per screen pixel so near
    // objects correctly occlude far ones.
    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable    = TRUE;
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsd.DepthFunc      = D3D11_COMPARISON_LESS;
    device->CreateDepthStencilState(&dsd, dssDepth.GetAddressOf());

    // No depth write: used for FOV cone overlay (depth test ON, write OFF).
    dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    device->CreateDepthStencilState(&dsd, dssNoDepthWrite.GetAddressOf());

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
    device->CreateBlendState(&bd, bsAlpha.GetAddressOf());

    return true;
}

bool Renderer::createShaders() {
    // ── Creature billboards (instanced) ───────────────────────────────────────
    // Two buffer slots:
    //   slot 0: per-vertex quad corners (PER_VERTEX_DATA)
    //   slot 1: per-creature data — pos, yaw, colour, size (PER_INSTANCE_DATA)
    auto cvs = compileShader(CREATURE_HLSL, "VSMain", "vs_5_0");
    auto cps = compileShader(CREATURE_HLSL, "PSMain", "ps_5_0");
    if (!cvs || !cps) return false;

    device->CreateVertexShader(cvs->GetBufferPointer(), cvs->GetBufferSize(), nullptr, creatureVS.GetAddressOf());
    device->CreatePixelShader (cps->GetBufferPointer(), cps->GetBufferSize(), nullptr, creaturePS.GetAddressOf());

    D3D11_INPUT_ELEMENT_DESC cd[] = {
        {"POSITION",  0, DXGI_FORMAT_R32G32_FLOAT,       0,  0, D3D11_INPUT_PER_VERTEX_DATA,   0},
        {"INST_POS",  0, DXGI_FORMAT_R32G32B32_FLOAT,    1,  0, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INST_YAW",  0, DXGI_FORMAT_R32_FLOAT,          1, 12, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INST_COLOR",0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INST_SIZE", 0, DXGI_FORMAT_R32_FLOAT,          1, 32, D3D11_INPUT_PER_INSTANCE_DATA, 1},
        {"INST_PAD",  0, DXGI_FORMAT_R32G32B32_FLOAT,    1, 36, D3D11_INPUT_PER_INSTANCE_DATA, 1},
    };
    device->CreateInputLayout(cd, 6, cvs->GetBufferPointer(), cvs->GetBufferSize(), creatureLayout.GetAddressOf());

    // ── Simple / FOV shaders ──────────────────────────────────────────
    // Compile three distinct shader objects from the same SIMPLE_HLSL source:
    //   simpleVS  – plain passthrough VS → used by the FOV cone
    //   fovPS     – translucent yellow PS
    auto svs = compileShader(SIMPLE_HLSL, "VSMain",  "vs_5_0");
    auto fps = compileShader(SIMPLE_HLSL, "FovPS",   "ps_5_0");
    if (!svs || !fps) return false;
    device->CreateVertexShader(svs->GetBufferPointer(), svs->GetBufferSize(), nullptr, simpleVS.GetAddressOf());
    device->CreatePixelShader (fps->GetBufferPointer(), fps->GetBufferSize(), nullptr, fovPS.GetAddressOf());

    // Input layout is the same for all simple variants (position-only)
    D3D11_INPUT_ELEMENT_DESC sd[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    // Use svs bytecode for layout creation (any of the VS blobs would work since
    // they share the same input signature)
    device->CreateInputLayout(sd, 1, svs->GetBufferPointer(), svs->GetBufferSize(), simpleLayout.GetAddressOf());

    return true;
}

// ── createBuffers ─────────────────────────────────────────────────────────────
bool Renderer::createBuffers(int w, int h) {

    if (!device.Get()) {
        OutputDebugStringA("createBuffers: device is NULL!\n");
        return false;
    }

    D3D11_BUFFER_DESC bd{};

    // Per-frame constant buffer: written by CPU every frame, read by both shaders.
    // Must be a multiple of 16 bytes — GPU alignment requirement.
    bd.ByteWidth      = sizeof(FrameConstants);
    bd.Usage          = D3D11_USAGE_DYNAMIC;        // CPU writes every frame
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device->CreateBuffer(&bd, nullptr, cbFrame.GetAddressOf()))) return false;

    // Creature quad: 4 corners of a unit quad [-0.5,0.5].
    // IMMUTABLE = never changes; fastest GPU read mode.
    // Laid out as TRIANGLE_STRIP: two triangles.
    float quad[] = {-0.5f,0.5f, 0.5f,0.5f, -0.5f,-0.5f, 0.5f,-0.5f}; // TL,TR,BL,BR
    bd.ByteWidth      = sizeof(quad);
    bd.Usage          = D3D11_USAGE_IMMUTABLE;
    bd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = 0;
    D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = quad;
    if (FAILED(device->CreateBuffer(&bd, &sd, creatureQuadVB.GetAddressOf()))) return false;

    // Creature instance buffer: one entry per living creature, rebuilt every frame.
    // Pre-allocated for up to MAX_CREATURES creatures (increase if needed).
    bd.ByteWidth      = (UINT)(sizeof(CreatureInstance) * MAX_CREATURES);
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    HRESULT hr = device->CreateBuffer(&bd, nullptr, cbFrame.GetAddressOf());
    if (FAILED(hr)) {
        char buf[128];
        sprintf_s(buf, "CreateBuffer cbFrame FAILED: hr=0x%08X sizeof=%zu\n",
                  (unsigned)hr, sizeof(FrameConstants));
        OutputDebugStringA(buf);
        return false;
    }
    OutputDebugStringA("cbFrame created OK\n");

    if (FAILED(device->CreateBuffer(&bd, nullptr, creatureInstanceVB.GetAddressOf()))) return false;

    // FOV cone: also dynamic — rebuilt each frame as selected creature moves.
    bd.ByteWidth = (UINT)(sizeof(SimpleVertex) * FOV_CONE_MAX_VERTS);
    if (FAILED(device->CreateBuffer(&bd, nullptr, fovConeVB.GetAddressOf()))) return false;

    return createDepthBuffer(w, h);
}

// ── createDepthBuffer ─────────────────────────────────────────────────────────
// A D32_FLOAT texture the same size as the screen.
// Stores the depth of the closest drawn pixel at each screen position.
// Must be recreated whenever the window is resized.
bool Renderer::createDepthBuffer(int w, int h) {
    depthTex.Reset();
    depthDSV.Reset();

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = (UINT)w;
    td.Height           = (UINT)h;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_D32_FLOAT;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(device->CreateTexture2D(&td, nullptr, depthTex.GetAddressOf()))) return false;
    if (FAILED(device->CreateDepthStencilView(depthTex.Get(), nullptr, depthDSV.GetAddressOf()))) return false;
    return true;
}

void Renderer::resize(int w, int h) {
    winW = w; winH = h;
    createDepthBuffer(w, h);
}

// ── shutdown ──────────────────────────────────────────────────────────────────
// D3D11 objects use COM reference counting. Release() decrements the count;
// the object is freed when it reaches zero. safeRelease() also nulls the pointer.
void Renderer::shutdown() { }
