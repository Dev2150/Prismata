#include <d3d11.h>
#include "Renderer/Renderer.hpp"
#include "Sim/DataRecorder.hpp"
#include "UI/SimUI.hpp"
#include "World/World.hpp"
#include "Renderer/Planet/PlanetRenderer.hpp"

// ── D3D11 globals ─────────────────────────────────────────────────────────────
// Kept as file-scope statics so helper functions (CreateDeviceD3D, WndProc, etc.)
// can access them without passing them around everywhere.
ID3D11Device*           g_pd3dDevice          = nullptr;  // logical GPU interface; used to create resources (buffers, shaders, states)
ID3D11DeviceContext*    g_pd3dDeviceContext    = nullptr;  // records and submits draw calls / state changes
IDXGISwapChain*         g_pSwapChain           = nullptr;  // manages front/back buffers and Present()
bool                    g_SwapChainOccluded    = false;    // true when the window is minimised/covered; we skip rendering
UINT                    g_ResizeWidth          = 0;        // pending resize dimensions written in WM_SIZE,
UINT                    g_ResizeHeight         = 0;        //   applied at the start of the next frame to avoid mid-frame resize
ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;  // view into the swap chain's back buffer; bound as the output render target

// ── Simulation objects ────────────────────────────────────────────────────────
// All four objects live for the entire duration of the program.
World        g_world;     // terrain + creatures + plants + species registry
DataRecorder g_recorder;  // samples population statistics at 1 Hz for graphing
Renderer     g_renderer;  // D3D11 draw calls, camera, chunk mesh cache
PlanetRenderer g_planet;  //
SimUI        g_ui;        // all ImGui panels; owns selectedID / showDemoWindow etc.
