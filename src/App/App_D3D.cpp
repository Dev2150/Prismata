#include <d3d11.h>
#include "App_Globals.hpp"

// ── D3D11 device and swap chain creation ──────────────────────────────────────
// Attempts hardware acceleration first; falls back to WARP (software rasteriser)
// if no compatible GPU is present. WARP is slow but correct, useful for CI/VMs.
bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount                        = 2; // double-buffering (front + back)
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;  // standard 8-bit RGBA back buffer
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags        = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;  // allow full-screen toggle
    sd.BufferUsage  = DXGI_USAGE_RENDER_TARGET_OUTPUT;         // back buffer is a render target
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;    // no MSAA (anti-aliasing handled by billboards + terrain mesh density)
    sd.Windowed     = TRUE;
    sd.SwapEffect   = DXGI_SWAP_EFFECT_DISCARD;  // discard back buffer contents after present (fastest)

    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;  // enable D3D validation layer in debug builds
#endif

    // Try hardware (GPU) first; fall back to WARP software rasteriser if it fails
    D3D_FEATURE_LEVEL fl;
    const D3D_FEATURE_LEVEL fla[] = {D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        createDeviceFlags, fla, 1, D3D11_SDK_VERSION,
        &sd, &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dDeviceContext);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
            createDeviceFlags, fla, 1, D3D11_SDK_VERSION,
            &sd, &g_pSwapChain, &g_pd3dDevice, &fl, &g_pd3dDeviceContext);
    if (FAILED(hr)) return false;

    // Disable DXGI's built-in Alt+Enter full-screen handling (we manage this ourselves)
    IDXGIFactory* f;
    if (SUCCEEDED(g_pSwapChain->GetParent(IID_PPV_ARGS(&f)))) {
        f->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);
        f->Release();
    }

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain=nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext=nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice=nullptr; }
}

// Obtain a Render Target View (RTV) from the swap chain's back buffer texture.
// An RTV is what you bind to OMSetRenderTargets; it tells D3D which texture to write to.
void CreateRenderTarget() {
    ID3D11Texture2D* buf;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&buf));  // index 0 = back buffer
    g_pd3dDevice->CreateRenderTargetView(buf, nullptr, &g_mainRenderTargetView);
    buf->Release();  // RTV holds its own reference; we can release ours
}

void CleanupRenderTarget()
{
    if (g_mainRenderTargetView) {
        g_mainRenderTargetView->Release(); g_mainRenderTargetView=nullptr;
    }
}
