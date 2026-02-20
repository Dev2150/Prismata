#pragma once
#include <d3d11.h>
#include <Windows.h>
#include "World/World.hpp"
#include "Sim/DataRecorder.hpp"
#include "Renderer/Renderer.hpp"
#include "UI/SimUI.hpp"
#include "Renderer/Planet/PlanetRenderer.hpp"


// ── D3D11 device objects ──────────────────────────────────────────────────────
// Defined in App_Globals.cpp; shared by App.cpp, App_D3D.cpp, App_WndProc.cpp.
extern ID3D11Device*           g_pd3dDevice;
extern ID3D11DeviceContext*    g_pd3dDeviceContext;
extern IDXGISwapChain*         g_pSwapChain;
extern bool                    g_SwapChainOccluded;   // true → window is covered; skip rendering
extern UINT                    g_ResizeWidth;          // pending resize; applied at frame start
extern UINT                    g_ResizeHeight;
extern ID3D11RenderTargetView* g_mainRenderTargetView;

// ── Simulation objects ────────────────────────────────────────────────────────
extern World            g_world;
extern DataRecorder     g_recorder;
extern Renderer         g_renderer;
extern PlanetRenderer   g_planet;
extern SimUI            g_ui;

// ── D3D11 helpers (implemented in App_D3D.cpp) ────────────────────────────────
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();

// ── Window procedure (implemented in App_WndProc.cpp) ─────────────────────────
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
