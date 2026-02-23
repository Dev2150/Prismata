#pragma once
// ── PlanetRenderer.hpp ────────────────────────────────────────────────────────
// High-level D3D11 renderer for the procedural planet.
// Integrates into the existing KyberPlanet render loop alongside the world terrain.

#include <d3d11.h>
#include "PlanetQuadTree.hpp"
#include "Renderer/Renderer.hpp"

struct PlanetRenderer {
    // ── D3D11 resources ───────────────────────────────────────────────────────
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> ctx;

    // Shaders
    ComPtr<ID3D11VertexShader> terrainVS;
    ComPtr<ID3D11PixelShader> terrainPS;
    ComPtr<ID3D11VertexShader> atmoVS;
    ComPtr<ID3D11PixelShader> atmoPS;

    // Input layout (matches PlanetVertex struct)
    ComPtr<ID3D11InputLayout> layout;

    // Sun shaders + resources
    ComPtr<ID3D11VertexShader> sunVS;
    ComPtr<ID3D11PixelShader> sunPS;
    ComPtr<ID3D11InputLayout> sunLayout;
    ComPtr<ID3D11Buffer> sunQuadVB; // 4 corners of a unit quad

    // Star shaders
    ComPtr<ID3D11VertexShader> starVS;
    ComPtr<ID3D11PixelShader> starPS;

    // Constant buffers
    ComPtr<ID3D11Buffer> cbFrame; // shared layout with world renderer
    ComPtr<ID3D11Buffer> cbPlanet; // planet-specific per-draw data

    // Atmosphere shell: a slightly inflated sphere mesh
    ComPtr<ID3D11Buffer> atmoVB;
    ComPtr<ID3D11Buffer> atmoIB;
    int                  atmoIdxCount = 0;

    // Render states
    ComPtr<ID3D11RasterizerState> rsSolid;
    ComPtr<ID3D11RasterizerState> rsSolidNoCull; // atmosphere (no back-face cull)
    ComPtr<ID3D11DepthStencilState> dssDepth;
    ComPtr<ID3D11DepthStencilState> dssNoWrite; // depth test, no write (atmo)
    ComPtr<ID3D11DepthStencilState> dssNoDepth; // no depth test, no write (sun)
    ComPtr<ID3D11BlendState> bsAlpha;
    ComPtr<ID3D11BlendState> bsAdditive; // additive blend for sun glow
    ComPtr<ID3D11BlendState> bsOpaque;

    // ── Quadtree ──────────────────────────────────────────────────────────────
    PlanetConfig              cfg;
    PlanetQuadTree* tree = nullptr;

    // ── Debug / UI state ──────────────────────────────────────────────────────
    bool  showAtmosphere = true;
    bool  showSun        = true;
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
    void renderStars();

};

// ── Global instance (declared in PlanetRenderer.cpp, extern here) ─────────────
// Match the pattern of g_world / g_renderer etc. in App_Globals.
// Add "extern PlanetRenderer g_planet;" to App_Globals.hpp after integration.
