#pragma once
#include <d3d11.h>
#include "../Core/Math.hpp"
#include "Sim/Creature.hpp"
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;

// ── Camera ────────────────────────────────────────────────────────────────────
struct Camera {
    Float3 pos    = {64.f, 40.f, 64.f};
    Float3 fwd    = {0.f, 0.f, 1.f};
    Float3 up     = {0.f, 1.f, 0.f};
    float  fovY   = 60.f;   // degrees
    float  translation_speed = 200.f;
    float  zoom_speed_coefficient = 0.1f;
    float  follow_dist    = 8000.f;   // How far behind the creature
    float  follow_height  = 500.f;   // How high above the creature
    float  follow_speed   = 250.f;   // How quickly the camera snaps to position

    Float3 forward() const {
        return fwd;
    }

    Mat4 viewMatrix() const {
        return Mat4::lookAtRH(
            pos.x, pos.y, pos.z,
            pos.x + fwd.x, pos.y + fwd.y, pos.z + fwd.z,
            up.x, up.y, up.z);
    }

    Mat4 projMatrix(float aspect) const {
        return Mat4::perspectiveRH(
            fovY * 3.14159265f / 180.f, aspect, 1.f, 600000.f);
    }
};

// ── Renderer ──────────────────────────────────────────────────────────────────
struct Renderer {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;

    // ── Shaders & layouts ─────────────────────────────────────────────────────
    ComPtr<ID3D11VertexShader> terrainVS;
    ComPtr<ID3D11PixelShader> terrainPS;
    ComPtr<ID3D11InputLayout> terrainLayout;

    // ── Creature shaders & layout ─────────────────────────────────────────────
    ComPtr<ID3D11VertexShader> creatureVS;
    ComPtr<ID3D11PixelShader> creaturePS;
    ComPtr<ID3D11InputLayout> creatureLayout;

    // FOV cone shaders (position-only)
    ComPtr<ID3D11VertexShader> simpleVS;
    ComPtr<ID3D11PixelShader> fovPS;
    ComPtr<ID3D11InputLayout> simpleLayout;

    // ── Buffers ────────────────────────────────────────────────────────────────
    ComPtr<ID3D11Buffer> cbFrame;
    ComPtr<ID3D11Buffer> creatureInstanceVB;
    ComPtr<ID3D11Buffer> creatureQuadVB;
    ComPtr<ID3D11Buffer> fovConeVB; // dynamic: updated each frame

    // ── States ─────────────────────────────────────────────────────────────────
    ComPtr<ID3D11RasterizerState> rsSolid;
    ComPtr<ID3D11RasterizerState> rsSolidNoCull; // for FOV cone (double-sided)
    ComPtr<ID3D11DepthStencilState> dssDepth;
    ComPtr<ID3D11DepthStencilState> dssNoDepthWrite;
    ComPtr<ID3D11BlendState> bsAlpha;

    static constexpr int MAX_CREATURES = 4096;

    // ── Depth buffer ──────────────────────────────────────────────────────────
    ComPtr<ID3D11Texture2D> depthTex;
    ComPtr<ID3D11DepthStencilView> depthDSV;

    // ── Camera & rendering state ───────────────────────────────────────────────
    Camera   camera;
    bool     wireframe    = false;
    bool     showFogOfWar = false;
    float    fogRadius    = 3000.f;
    EntityID playerID     = INVALID_ID;

    // ── Rendering features ────────────────────────────────────────────────
    EntityID selectedID   = INVALID_ID;  // creature whose FOV cone to draw
    bool     showFOVCone  = true;        // toggle FOV cone overlay
    bool     lockYawFollow= false;       // when true, following a creature won't rotate the camera
    bool     hideOutsideFOV = false;     // hide entities outside possessed creature's FOV

    // ── Creature possession: translation-only follow ───────────────────────────
    // When a creature is possessed we record the camera→creature offset at the
    // moment possession begins and maintain that fixed offset for the duration.
    // This means the camera never rotates or zooms — it just translates in lockstep.
    // ambientColor.w carries simTime for the water wave animation.
    Float3 possessOffset    = {0.f, 0.f, 0.f};  // camera pos - creature pos at start
    bool   hasPossessOffset = false;             // true once offset has been captured

    // ── Per-frame constant buffer layout (matches HLSL cbuffer) ───────────────
    // IMPORTANT: must remain 16-byte aligned throughout; add fields in float4 blocks.
    struct alignas(16) FrameConstants {
        float viewProj[4][4];   // 64 bytes – row-major View*Projection
        float camPos[4];        // 16 bytes – camera world position (w unused)
        float lightDir[4];      // 16 bytes – sun direction (FROM sun TOWARD scene, w unused)
        float fowData[4];       // 16 bytes – fog of war: xyz=player pos, w=radius (0=off)
        float fowFacing[4];     // 16 bytes - fog of war: xyz=facing dir, w=cos(FOV/2)
        float sunColor[4];      // 16 bytes – rgb=sun light tint, w=timeOfDay [0,1]
        float ambientColor[4];  // 16 bytes – rgb=sky/ambient light, w=simTime (seconds)
        float planetCenter[4];  // 16 bytes - xyz=planet center, w=planet radius
    };

    // ── Vertex layouts ─────────────────────────────────────────────────────────
    struct TerrainVertex {
        float pos[3];
        float nrm[3];
        float col[4];
    };

    struct CreatureInstance {
        float pos[3];
        float yaw;
        float color[4];
        float size;
        float pad[3];
    };

    // Simple float3 vertex used for water plane and FOV cone
    struct SimpleVertex { float x, y, z; };

    // ── Per-chunk GPU meshes ───────────────────────────────────────────────────
    struct ChunkMesh {
        ID3D11Buffer* vb       = nullptr;
        ID3D11Buffer* ib       = nullptr;
        int           idxCount = 0;
        bool          built    = false;
    };
    std::vector<ChunkMesh> chunkMeshes;

    // ── Public API ─────────────────────────────────────────────────────────────
    bool init(ID3D11Device* dev, ID3D11DeviceContext* ctx, int width, int height);
    void resize(int width, int height);
    void render(const World& world, float aspectRatio);
    void shutdown();
    void tickCamera(float dt, const World& world);
    void onMouseMove(int dx, int dy, bool rightDown);
    void onMouseScroll(float delta);
    void onKey(int vk, bool down);

    // Terrain raycast for hover tooltip: returns true and sets outPos/outMat
    // if the mouse ray hits the terrain. mx/my are window-space pixel coords.
    bool screenToTerrain(float mx, float my, float W, float H,
                         const World& world, Vec3& outPos, uint8_t& outMat) const;

private:
    bool createShaders();
    bool createBuffers(int w, int h);
    bool createDepthBuffer(int w, int h);
    void updateFrameConstants(const World& world, float aspect);
    void renderCreatures(const World& world);
    void renderPlants(const World& world);       // ← NEW
    void renderFOVCone(const World& world);

    static constexpr int FOV_CONE_SEGS = 64;
    static constexpr int FOV_CONE_MAX_VERTS = FOV_CONE_SEGS * 3;

    int   winW = 1280, winH = 800;

    // Movement keys: [0]=W [1]=S [2]=A [3]=D [4]=Z [5]=X [6]=E [7]=Q
    float moveKeys[8] = {};

    // Mouse wheel accumulator: positive = scroll up = zoom out (move away from planet)
    float scrollDelta = 0.f;

};
