// Prismata – Evolution Simulation
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "implot.h"
#include <d3d11.h>
#include <tchar.h>
#include <chrono>
#include <algorithm>
#include <cmath>

#include "World.h"
#include "Renderer.h"
#include "DataRecorder.h"
#include "SimUI.h"
#include "Math.h"

// ── D3D11 globals ─────────────────────────────────────────────────────────────
static ID3D11Device*           g_pd3dDevice          = nullptr;
static ID3D11DeviceContext*    g_pd3dDeviceContext    = nullptr;
static IDXGISwapChain*         g_pSwapChain           = nullptr;
static bool                    g_SwapChainOccluded    = false;
static UINT                    g_ResizeWidth          = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;

// ── Simulation globals ────────────────────────────────────────────────────────
static World        g_world;
static DataRecorder g_recorder;
static Renderer     g_renderer;
static SimUI        g_ui;

bool    CreateDeviceD3D(HWND hWnd);
void    CleanupDeviceD3D();
void    CreateRenderTarget();
void    CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ── Main ──────────────────────────────────────────────────────────────────────
int main(int, char**)
{
    ImGui_ImplWin32_EnableDpiAwareness();
    float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(
        ::MonitorFromPoint(POINT{0,0}, MONITOR_DEFAULTTOPRIMARY));

    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L,
                       GetModuleHandle(nullptr), nullptr, nullptr, nullptr, nullptr,
                       L"Prismata", nullptr };
    ::RegisterClassExW(&wc);
    int winW = (int)(1600 * main_scale), winH = (int)(900 * main_scale);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Prismata – Evolution Simulation",
                                WS_OVERLAPPEDWINDOW, 100, 100, winW, winH,
                                nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;
    io.ConfigDpiScaleFonts     = true;
    io.ConfigDpiScaleViewports = true;
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    g_world.generate(42, 16, 16);
    if (!g_renderer.init(g_pd3dDevice, g_pd3dDeviceContext, winW, winH))
    {
        // If init fails, we must stop.
        OutputDebugStringA("FATAL: Renderer initialization failed!\n");
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    using Clock = std::chrono::high_resolution_clock;
    auto lastTime = Clock::now();
    bool done = false;

    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg); ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_SwapChainOccluded &&
            g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            ::Sleep(10); continue;
        }
        g_SwapChainOccluded = false;

        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
            RECT rc; ::GetClientRect(hwnd, &rc);
            g_renderer.resize(rc.right - rc.left, rc.bottom - rc.top);
        }

        auto now = Clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        dt = std::min(dt, 0.05f);

        g_renderer.tickCamera(dt, g_world);
        g_world.tick(dt);
        g_recorder.tick(dt, g_world);

        // Clear + bind RTV
        const float cc[4] = {0.08f, 0.10f, 0.12f, 1.f};
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, cc);
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, g_renderer.depthDSV);
        if (g_renderer.depthDSV)
            g_pd3dDeviceContext->ClearDepthStencilView(g_renderer.depthDSV,
                D3D11_CLEAR_DEPTH, 1.f, 0);

        RECT rc; ::GetClientRect(hwnd, &rc);
        D3D11_VIEWPORT vp{};
        vp.Width    = (float)(rc.right  - rc.left);
        vp.Height   = (float)(rc.bottom - rc.top);
        vp.MaxDepth = 1.f;
        g_pd3dDeviceContext->RSSetViewports(1, &vp);

        float aspect = vp.Width / std::max(vp.Height, 1.f);
        g_renderer.render(g_world, aspect);

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
            ImGuiDockNodeFlags_PassthruCentralNode);

        g_ui.draw(g_world, g_recorder, g_renderer);

        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }
        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    g_renderer.shutdown();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

// ── WndProc ───────────────────────────────────────────────────────────────────
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;

    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_ResizeWidth  = LOWORD(lParam);
        g_ResizeHeight = HIWORD(lParam);
        return 0;

    case WM_KEYDOWN:
    case WM_KEYUP:
        if (!ImGui::GetIO().WantCaptureKeyboard)
            g_renderer.onKey((int)wParam, msg == WM_KEYDOWN);
        return 0;

    case WM_RBUTTONDOWN: SetCapture(hWnd);  return 0;
    case WM_RBUTTONUP:   ReleaseCapture();  return 0;

    case WM_MOUSEMOVE: {
        static int lastMX = 0, lastMY = 0;
        int mx = (short)LOWORD(lParam), my = (short)HIWORD(lParam);
        if (!ImGui::GetIO().WantCaptureMouse)
            g_renderer.onMouseMove(mx - lastMX, my - lastMY, (wParam & MK_RBUTTON) != 0);
        lastMX = mx; lastMY = my;
        return 0;
    }

    // ── Ray-pick creature on left click ──────────────────────────────────────
    case WM_LBUTTONDOWN: {
        if (ImGui::GetIO().WantCaptureMouse) break;

        RECT rc; ::GetClientRect(hWnd, &rc);
        float W = (float)(rc.right - rc.left);
        float H = (float)(rc.bottom - rc.top);
        float mx = (float)(short)LOWORD(lParam);
        float my = (float)(short)HIWORD(lParam);

        // NDC mouse position
        float ndcX =  (mx / W) * 2.f - 1.f;
        float ndcY = -(my / H) * 2.f + 1.f;

        // Build combined VP matrix and invert it (no DirectXMath needed)
        Mat4 vp    = g_renderer.camera.viewMatrix() * g_renderer.camera.projMatrix(W / H);
        Mat4 vpInv = vp.inversed();

        // Unproject two NDC points
        auto unproject = [&](float z) -> Vec4 {
            Vec4 clip = {ndcX, ndcY, z, 1.f};
            Vec4 world = vpInv.transform(clip);
            float invW = (std::abs(world.w) > 1e-7f) ? 1.f / world.w : 0.f;
            return {world.x * invW, world.y * invW, world.z * invW, 1.f};
        };

        Vec4 near4 = unproject(0.f);
        Vec4 far4  = unproject(1.f);
        // Ray direction
        float dx = far4.x - near4.x, dy = far4.y - near4.y, dz = far4.z - near4.z;
        float dl = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (dl < 1e-6f) break;
        dx /= dl; dy /= dl; dz /= dl;

        // Find closest creature to ray (within 3m of ray)
        float    bestDist = 3.f;
        EntityID bestID   = INVALID_ID;
        for (const auto& c : g_world.creatures) {
            if (!c.alive) continue;
            float ocx = c.pos.x - near4.x, ocy = c.pos.y - near4.y, ocz = c.pos.z - near4.z;
            float t   = ocx*dx + ocy*dy + ocz*dz;
            if (t < 0.f) continue;
            float cx2 = near4.x + dx*t - c.pos.x;
            float cy2 = near4.y + dy*t - c.pos.y;
            float cz2 = near4.z + dz*t - c.pos.z;
            float d   = std::sqrt(cx2*cx2 + cy2*cy2 + cz2*cz2);
            if (d < bestDist) { bestDist = d; bestID = c.id; }
        }
        g_ui.selectedID = bestID;
        return 0;
    }
    case WM_CHAR: // better for single-press actions
        if (wParam == 'p' || wParam == 'P') {
            if (!ImGui::GetIO().WantCaptureKeyboard) {
                // Find a random living creature and possess it
                EntityID randomID = g_world.findRandomLivingCreature();
                if (randomID != INVALID_ID) {
                    g_renderer.playerID = randomID;
                    g_ui.selectedID = randomID; // Also select it in the UI
                }
            }
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ── D3D11 helpers ─────────────────────────────────────────────────────────────
bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator=60; sd.BufferDesc.RefreshRate.Denominator=1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL fl; const D3D_FEATURE_LEVEL fla[] = {D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE,
        nullptr, createDeviceFlags, fla, 1, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
        &g_pd3dDevice, &fl, &g_pd3dDeviceContext);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP,
            nullptr, createDeviceFlags, fla, 1, D3D11_SDK_VERSION, &sd, &g_pSwapChain,
            &g_pd3dDevice, &fl, &g_pd3dDeviceContext);
    if (FAILED(hr)) return false;

    IDXGIFactory* f;
    if (SUCCEEDED(g_pSwapChain->GetParent(IID_PPV_ARGS(&f)))) {
        f->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER); f->Release();
    }
    CreateRenderTarget();
    return true;
}
void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain=nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext=nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice=nullptr; }
}
void CreateRenderTarget() {
    ID3D11Texture2D* buf;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&buf));
    g_pd3dDevice->CreateRenderTargetView(buf, nullptr, &g_mainRenderTargetView);
    buf->Release();
}
void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView=nullptr; }
}
