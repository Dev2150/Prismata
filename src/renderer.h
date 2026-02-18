#pragma once
#include <d3d11.h>
#include <DirectXMath.h>
#include "world.h"

using namespace DirectX;

// ── Camera ────────────────────────────────────────────────────────────────────
struct Camera {
    XMFLOAT3 pos    = {64.f, 40.f, 64.f};
    float    yaw    = 0.f;    // radians
    float    pitch  = -0.6f;  // radians (negative = looking down)
    float    fovY   = 60.f;

    // Returns view matrix
    XMMATRIX viewMatrix() const {
        XMVECTOR eye = XMLoadFloat3(&pos);
        XMVECTOR fwd = XMVector3Transform(
            XMVectorSet(0, 0, 1, 0),
            XMMatrixRotationRollPitchYaw(pitch, yaw, 0));
        return XMMatrixLookAtRH(eye, XMVectorAdd(eye, fwd), XMVectorSet(0,1,0,0));
    }

    XMMATRIX projMatrix(float aspect) const {
        return XMMatrixPerspectiveFovRH(
            XMConvertToRadians(fovY), aspect, 0.1f, 1000.f);
    }

    void processInput(float dt);  // WASD + mouse, implemented in .cpp
};

// ── Renderer ──────────────────────────────────────────────────────────────────
struct renderer {
    // D3D11 device + context (borrowed from main, not owned here)
    ID3D11Device*        device  = nullptr;
    ID3D11DeviceContext* ctx     = nullptr;

    // ── Resources ────────────────────────────────────────────────────────────
    ID3D11VertexShader*   terrainVS         = nullptr;
    ID3D11PixelShader*    terrainPS         = nullptr;
    ID3D11VertexShader*   creatureVS        = nullptr;
    ID3D11PixelShader*    creaturePS        = nullptr;
    ID3D11InputLayout*    terrainLayout     = nullptr;
    ID3D11InputLayout*    creatureLayout    = nullptr;

    ID3D11Buffer*         cbFrame           = nullptr;  // per-frame constants
    ID3D11Buffer*         terrainVB         = nullptr;  // rebuilt per dirty chunk
    ID3D11Buffer*         terrainIB         = nullptr;
    ID3D11Buffer*         creatureInstanceVB= nullptr;  // instanced quads
    ID3D11Buffer*         creatureQuadVB    = nullptr;  // unit quad

    ID3D11RasterizerState*  rsWireframe     = nullptr;
    ID3D11RasterizerState*  rsSolid         = nullptr;
    ID3D11DepthStencilState* dssDepth       = nullptr;
    ID3D11BlendState*        bsAlpha        = nullptr;

    // Shared depth/stencil for the 3D scene (public so main can bind)
    ID3D11Texture2D*         depthTex       = nullptr;
    ID3D11DepthStencilView*  depthDSV       = nullptr;

    // ── State ─────────────────────────────────────────────────────────────────
    Camera   camera;
    bool     wireframe       = false;
    bool     showFogOfWar    = false;
    float    fogRadius       = 30.f;
    XMFLOAT3 fowCenter       = {};       // player creature position

    // Player mode
    EntityID playerID        = INVALID_ID;

    // ── Per-frame constants (matches cbuffer in HLSL) ────────────────────────
    struct alignas(16) FrameConstants {
        XMFLOAT4X4 viewProj;
        XMFLOAT4   camPos;      // w unused
        XMFLOAT4   lightDir;    // w = fog radius
        XMFLOAT4   fowCenter;   // xyz = center, w = enabled (0/1)
    };

    // ── Terrain vertex ────────────────────────────────────────────────────────
    struct TerrainVertex {
        XMFLOAT3 pos;
        XMFLOAT3 normal;
        XMFLOAT4 color;
    };

    // ── Creature instance data ────────────────────────────────────────────────
    struct CreatureInstance {
        XMFLOAT3 pos;
        float    yaw;
        XMFLOAT4 color;   // hue from genome + alpha = energy fraction
        float    size;
        float    pad[3];
    };

    // Per-chunk GPU meshes
    struct ChunkMesh {
        ID3D11Buffer* vb      = nullptr;
        ID3D11Buffer* ib      = nullptr;
        int           idxCount= 0;
        bool          built   = false;
    };
    std::vector<ChunkMesh> chunkMeshes;

    // ── Public API ────────────────────────────────────────────────────────────
    bool  init(ID3D11Device* dev, ID3D11DeviceContext* ctx,
               int width, int height);
    void  resize(int width, int height);
    void  render(const world& world, float aspectRatio);
    void  shutdown();

    // Input: call from WndProc
    void  onMouseMove(int dx, int dy, bool rightDown);
    void  onKey(int vk, bool down);
    void  tickCamera(float dt, bool playerMode);

private:
    bool  createDepthBuffer(int w, int h);
    bool createShaders();
    bool createBuffers(int width, int height);
    void buildChunkMesh(const world& world, int cx, int cz);
    void uploadCreatureInstances(const world& world);
    void updateFrameConstants(const world& world, float aspect);
    void renderTerrain(const world& world);
    void renderCreatures(const world& world);

    int    winW = 1280, winH = 800;
    float  moveKeys[6] = {};   // W,A,S,D,Q,E

    // Safe release helper
    template<typename T> static void safeRelease(T*& p) {
        if (p) { p->Release(); p = nullptr; }
    }
};
