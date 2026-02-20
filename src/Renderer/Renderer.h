#pragma once
#include <d3d11.h>
#include "../Math.h"
#include "../World.h"

// ── Camera ────────────────────────────────────────────────────────────────────
struct Camera {
    Float3 pos    = {64.f, 40.f, 64.f};
    float  yaw    = 0.f;    // radians
    float  pitch  = -0.6f;  // radians (negative = looking down)
    float  fovY   = 60.f;   // degrees
    float  translation_speed = 120.f;
    float  follow_dist    = 16.f;   // How far behind the creature
    float  follow_height  = 10.f;   // How high above the creature
    float  follow_speed   = 5.f;   // How quickly the camera snaps to position

    // Forward vector from yaw+pitch
    Float3 forward() const {
        return {
            std::sin(yaw) * std::cos(pitch),
            std::sin(pitch),
            std::cos(yaw) * std::cos(pitch)
        };
    }

    Mat4 viewMatrix() const {
        Float3 f = forward();
        return Mat4::lookAtRH(
            pos.x, pos.y, pos.z,
            pos.x + f.x, pos.y + f.y, pos.z + f.z,
            0.f, 1.f, 0.f);
    }

    Mat4 projMatrix(float aspect) const {
        return Mat4::perspectiveRH(
            fovY * 3.14159265f / 180.f, aspect, 0.1f, 1000.f);
    }
};

// ── Renderer ──────────────────────────────────────────────────────────────────
struct Renderer {
    ID3D11Device*        device = nullptr;
    ID3D11DeviceContext* ctx    = nullptr;

    // ── Shaders & layout ──────────────────────────────────────────────────────
    ID3D11VertexShader*   terrainVS          = nullptr;
    ID3D11PixelShader*    terrainPS          = nullptr;
    ID3D11InputLayout*    terrainLayout = nullptr;

    // ── Creature shaders & layout ─────────────────────────────────────────────
    ID3D11VertexShader*   creatureVS         = nullptr;
    ID3D11PixelShader*    creaturePS         = nullptr;
    ID3D11InputLayout*    creatureLayout     = nullptr;

    // ── Simple (position-only) shaders – shared by water + FOV cone ──────────
    // simpleVS: position-only VS that just multiplies by viewProj.
    // waterPS : renders translucent blue.
    // fovPS   : renders translucent yellow.
    ID3D11VertexShader*   simpleVS   = nullptr;
    ID3D11PixelShader*    waterPS    = nullptr;
    ID3D11PixelShader*    fovPS      = nullptr;
    ID3D11InputLayout*    simpleLayout = nullptr;

    // ── Buffers ────────────────────────────────────────────────────────────────
    ID3D11Buffer*         cbFrame            = nullptr;
    ID3D11Buffer*         creatureInstanceVB = nullptr;
    ID3D11Buffer*         creatureQuadVB     = nullptr;
    ID3D11Buffer*         waterVB            = nullptr;   // two triangles covering the world
    ID3D11Buffer*         fovConeVB          = nullptr;   // dynamic: updated each frame

    // ── States ─────────────────────────────────────────────────────────────────
    ID3D11RasterizerState*   rsWireframe = nullptr;
    ID3D11RasterizerState*   rsSolid     = nullptr;
    ID3D11RasterizerState*   rsSolidNoCull  = nullptr;  // for FOV cone (double-sided)
    ID3D11DepthStencilState* dssDepth    = nullptr;
    ID3D11DepthStencilState* dssNoDepthWrite= nullptr;  // depth test ON, write OFF (water)
    ID3D11BlendState*        bsAlpha     = nullptr;
    bool waterBuilt = false;                // set after buildWaterMesh()
    size_t MAX_CREATURES = 4096;

    // ── Depth buffer (public so main can bind it) ─────────────────────────────
    ID3D11Texture2D*        depthTex = nullptr;
    ID3D11DepthStencilView* depthDSV = nullptr;

    // ── Camera & rendering state ───────────────────────────────────────────────
    Camera   camera;
    bool     wireframe    = false;
    bool     showFogOfWar = false;
    float    fogRadius    = 30.f;
    Float3   fowCenter    = {};
    EntityID playerID     = INVALID_ID;

    // ── New rendering features ────────────────────────────────────────────────
    EntityID selectedID   = INVALID_ID;  // creature whose FOV cone to draw
    bool     showFOVCone  = true;        // toggle FOV cone overlay
    bool     showWater    = true;        // toggle water plane
    float    waterLevel   = 0.45f;       // Y-height of the water plane
    bool     lockYawFollow= false;       // when true, following a creature won't rotate the camera

    // ── Per-frame constant buffer layout (matches HLSL cbuffer) ───────────────
    struct alignas(16) FrameConstants {
        float viewProj[4][4];   // row-major
        float camPos[4];        // w unused
        float lightDir[4];      // w unused
        float fowCenter[4];     // xyz=center, w=radius (0 = disabled)
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
    void onKey(int vk, bool down);

    // Terrain raycast for hover tooltip: returns true and sets outPos/outMat
    // if the mouse ray hits the terrain. mx/my are window-space pixel coords.
    bool screenToTerrain(float mx, float my, float W, float H,
                         const World& world, Vec3& outPos, uint8_t& outMat) const;

private:
    bool createShaders();
    bool createBuffers(int w, int h);
    bool createDepthBuffer(int w, int h);
    void buildChunkMesh(const World& world, int cx, int cz);
    void buildWaterMesh(const World& world);
    void updateFrameConstants(const World& world, float aspect);
    void renderTerrain(const World& world);
    void renderCreatures(const World& world);
    void renderWater(const World& world);
    void renderFOVCone(const World& world);

    static constexpr int FOV_CONE_SEGS = 64;
    static constexpr int FOV_CONE_MAX_VERTS = FOV_CONE_SEGS * 3;

    int   winW = 1280, winH = 800;
    float moveKeys[6] = {};   // W S A D Q E

    template<typename T> static void safeRelease(T*& p) {
        if (p) { p->Release(); p = nullptr; }
    }
};
