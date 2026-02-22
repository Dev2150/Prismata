#pragma once
// ── PlanetRenderer.hpp ────────────────────────────────────────────────────────
// High-level D3D11 renderer for the procedural planet.
// Integrates into the existing KyberPlanet render loop alongside the world terrain.
//
// USAGE IN App.cpp / RunApplication():
// ─────────────────────────────────────
//   // After Renderer::init():
//   PlanetConfig pcfg;
//   pcfg.radius       = 1000.f;
//   pcfg.center       = {0, -1500.f, 0};   // planet sits below the flat world
//   pcfg.heightScale  = 120.f;
//   pcfg.maxDepth     = 16;
//   g_planet.init(g_pd3dDevice, g_pd3dDeviceContext, pcfg);
//
//   // In the main render loop (after g_renderer.render()):
//   g_planet.update(g_renderer.camera);
//   g_planet.render(g_renderer.camera, aspect, g_world.timeOfDay(), g_world.simTime);
//
//   // On shutdown (before ImGui / D3D11 cleanup):
//   g_planet.shutdown();

#include <d3d11.h>
#include "PlanetQuadTree.hpp"
#include "Renderer/Renderer.hpp"

struct PlanetRenderer {
    // ── D3D11 resources ───────────────────────────────────────────────────────
    ID3D11Device*        device = nullptr;
    ID3D11DeviceContext* ctx    = nullptr;

    // Shaders
    ID3D11VertexShader*  terrainVS  = nullptr;
    ID3D11PixelShader*   terrainPS  = nullptr;
    ID3D11VertexShader*  atmoVS     = nullptr;
    ID3D11PixelShader*   atmoPS     = nullptr;

    // Input layout (matches PlanetVertex struct)
    ID3D11InputLayout*   layout     = nullptr;

    // Sun shaders + resources
    ID3D11VertexShader*  sunVS      = nullptr;
    ID3D11PixelShader*   sunPS      = nullptr;
    ID3D11InputLayout*   sunLayout  = nullptr;
    ID3D11Buffer*        sunQuadVB  = nullptr; // 4 corners of a unit quad

    // Constant buffers
    ID3D11Buffer*        cbFrame    = nullptr;   // shared layout with world renderer
    ID3D11Buffer*        cbPlanet   = nullptr;   // planet-specific per-draw data

    // Atmosphere shell: a slightly inflated sphere mesh
    ID3D11Buffer*        atmoVB     = nullptr;
    ID3D11Buffer*        atmoIB     = nullptr;
    int                  atmoIdxCount = 0;

    // Render states
    ID3D11RasterizerState*   rsSolid     = nullptr;
    ID3D11RasterizerState*   rsSolidNoCull = nullptr;  // atmosphere (no back-face cull)
    ID3D11DepthStencilState* dssDepth    = nullptr;
    ID3D11DepthStencilState* dssNoWrite    = nullptr;   // depth test, no write (atmo)
    ID3D11DepthStencilState* dssNoDepth    = nullptr;   // no depth test, no write (sun)
    ID3D11BlendState*        bsAlpha       = nullptr;
    ID3D11BlendState*        bsAdditive    = nullptr;   // additive blend for sun glow
    ID3D11BlendState*        bsOpaque    = nullptr;

    // ── Quadtree ──────────────────────────────────────────────────────────────
    PlanetConfig              cfg;
    PlanetQuadTree*           tree    = nullptr;

    // ── Debug / UI state ──────────────────────────────────────────────────────
    bool  showAtmosphere = true;
    bool showSun        = true;
    bool  wireframe      = false;
    int   totalNodes     = 0;
    int   totalLeaves    = 0;

    // ── Per-frame constant buffer layout ─────────────────────────────────────
    // Must be 16-byte aligned; matches HLSL cbuffer PlanetConstants : register(b1)
    struct alignas(16) PlanetConstants {
        float atmosphereColor[4]; // rgb = atmosphere tint, w = thickness
        float planetParams[4];    // x = seaLevel, y = snowLine, zw unused
    };

    // ── Shared frame constants (identical to Renderer::FrameConstants) ────────
    // We upload these to b0 ourselves so the planet shaders can share the same
    // lighting model as the world terrain without duplicating state.
    struct alignas(16) FrameConstants {
        float viewProj[4][4];
        float camPos[4];
        float lightDir[4];
        float fowData[4];
        float fowFacing[4];
        float sunColor[4];
        float ambientColor[4];
        float planetCenter[4];
    };

    // ── Public API ────────────────────────────────────────────────────────────

    // Create shaders, buffers, states. Call once after D3D11 device is ready.
    bool init(ID3D11Device* dev, ID3D11DeviceContext* c, const PlanetConfig& config);

    // Update the quadtree LOD for the current camera position.
    // Must be called before render() each frame.
    void update(const Camera& cam);

    // Render the planet. Uploads fresh frame constants from the camera.
    // timeOfDay in [0,1), simTime in seconds (for future water animation on planet).
    void render(const World& world, const Renderer& rend, float aspect);

    // Release all GPU resources.
    void shutdown();

    // Draw ImGui diagnostics panel (call inside an ImGui window)
    void drawDebugUI();

private:
    bool compileShaders();
    bool createBuffers();
    bool createAtmosphere();
    bool createSunQuad();
    bool createRenderStates();

    void uploadFrameConstants(const World& world, const Renderer& rend, float aspect);
    void uploadPlanetConstants(float timeOfDay);

    void renderPatches();
    void renderAtmosphere(const Camera& cam);
    void renderSun();

    template<typename T>
    static void safeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }
};

// ── Global instance (declared in PlanetRenderer.cpp, extern here) ─────────────
// Match the pattern of g_world / g_renderer etc. in App_Globals.
// Add "extern PlanetRenderer g_planet;" to App_Globals.hpp after integration.
