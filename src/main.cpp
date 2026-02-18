// EvoSim – Evolution Simulation
// Built on: Dear ImGui DirectX11 backend (windows example)

#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include "implot.h"
#include <d3d11.h>
#include <tchar.h>
#include <chrono>
#include <algorithm>

#include "world.h"
#include "renderer.h"
#include "data_recorder.h"
#include "sim_ui.h"

// ── D3D11 globals (from original example) ────────────────────────────────────
static ID3D11Device*            g_pd3dDevice           = nullptr;
static ID3D11DeviceContext*     g_pd3dDeviceContext     = nullptr;
static IDXGISwapChain*          g_pSwapChain            = nullptr;
static bool                     g_SwapChainOccluded     = false;
static UINT                     g_ResizeWidth           = 0, g_ResizeHeight = 0;
static ID3D11RenderTargetView*  g_mainRenderTargetView  = nullptr;

// ── Simulation globals ────────────────────────────────────────────────────────
static world        g_world;
static data_recorder g_recorder;
static renderer     g_renderer;
static sim_ui        g_ui;

// Forward declarations
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
                       L"EvoSim", nullptr };
    ::RegisterClassExW(&wc);
    int winW = (int)(1600 * main_scale);
    int winH = (int)(900  * main_scale);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"EvoSim – Evolution Simulation",
                                WS_OVERLAPPEDWINDOW, 100, 100, winW, winH,
                                nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // ── ImGui + ImPlot setup ─────────────────────────────────────────────────
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
    style.FontScaleDpi  = main_scale;
    io.ConfigDpiScaleFonts    = true;
    io.ConfigDpiScaleViewports= true;

    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding        = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // ── Simulation init ───────────────────────────────────────────────────────
    g_world.generate(/*seed=*/42, /*chunksX=*/16, /*chunksZ=*/16);
    g_renderer.init(g_pd3dDevice, g_pd3dDeviceContext, winW, winH);

    // ── Timing ───────────────────────────────────────────────────────────────
    using Clock = std::chrono::high_resolution_clock;
    auto lastTime = Clock::now();

    // ── Main loop ─────────────────────────────────────────────────────────────
    ImVec4 clearColor = {0.08f, 0.10f, 0.12f, 1.f};
    bool done = false;

    while (!done)
    {
        // ── Win32 messages ────────────────────────────────────────────────────
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // ── Swap chain occlusion ──────────────────────────────────────────────
        if (g_SwapChainOccluded &&
            g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        // ── Resize ────────────────────────────────────────────────────────────
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight,
                                        DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
            g_renderer.resize((int)g_ResizeWidth, (int)g_ResizeHeight);
        }

        // ── Delta time ────────────────────────────────────────────────────────
        auto now  = Clock::now();
        float dt  = std::chrono::duration<float>(now - lastTime).count();
        lastTime  = now;
        dt = std::min(dt, 0.05f);   // cap at 50 ms to avoid spiral-of-death

        // ── Camera movement ───────────────────────────────────────────────────
        g_renderer.tickCamera(dt, g_renderer.playerID != INVALID_ID);

        // ── Simulation tick ───────────────────────────────────────────────────
        g_world.tick(dt);
        g_recorder.tick(dt, g_world);

        // ── Render 3D scene ───────────────────────────────────────────────────
        // Clear back buffer
        const float cc[4] = {clearColor.x, clearColor.y, clearColor.z, clearColor.w};
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, cc);

        // Set RTV + depth from renderer
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView,
                                                g_renderer.depthDSV);
        if (g_renderer.depthDSV)
            g_pd3dDeviceContext->ClearDepthStencilView(g_renderer.depthDSV,
                D3D11_CLEAR_DEPTH, 1.f, 0);

        // Viewport
        D3D11_VIEWPORT vp{};
        RECT rc; ::GetClientRect(hwnd, &rc);
        vp.Width    = (float)(rc.right - rc.left);
        vp.Height   = (float)(rc.bottom - rc.top);
        vp.MaxDepth = 1.f;
        g_pd3dDeviceContext->RSSetViewports(1, &vp);

        float aspect = vp.Width / std::max(vp.Height, 1.f);
        g_renderer.render(g_world, aspect);

        // ── ImGui frame ───────────────────────────────────────────────────────
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // DockSpace over the whole window
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
            ImGuiDockNodeFlags_PassthruCentralNode);

        // Draw all simulation panels
        g_ui.draw(g_world, g_recorder, g_renderer);

        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }

        // ── Present ───────────────────────────────────────────────────────────
        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
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
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_ResizeWidth  = LOWORD(lParam);
        g_ResizeHeight = HIWORD(lParam);
        return 0;

    case WM_KEYDOWN:
    case WM_KEYUP:
        // Forward camera keys to renderer (only when ImGui is not capturing)
        if (!ImGui::GetIO().WantCaptureKeyboard)
            g_renderer.onKey((int)wParam, msg == WM_KEYDOWN);
        return 0;

    case WM_RBUTTONDOWN:
        SetCapture(hWnd);
        return 0;
    case WM_RBUTTONUP:
        ReleaseCapture();
        return 0;

    case WM_MOUSEMOVE: {
        static int lastMX = 0, lastMY = 0;
        int mx = LOWORD(lParam), my = HIWORD(lParam);
        bool rightDown = (wParam & MK_RBUTTON) != 0;
        if (!ImGui::GetIO().WantCaptureMouse)
            g_renderer.onMouseMove(mx - lastMX, my - lastMY, rightDown);
        lastMX = mx; lastMY = my;
        return 0;
    }

    case WM_LBUTTONDOWN: {
        // Ray-pick nearest creature
        if (!ImGui::GetIO().WantCaptureMouse) {
            RECT rc; ::GetClientRect(hWnd, &rc);
            float winW2 = (float)(rc.right - rc.left);
            float winH2 = (float)(rc.bottom - rc.top);
            float mx    = (float)LOWORD(lParam);
            float my    = (float)HIWORD(lParam);

            // Unproject mouse ray
            float ndcX =  (mx / winW2) * 2.f - 1.f;
            float ndcY = -(my / winH2) * 2.f + 1.f;

            using namespace DirectX;
            XMMATRIX vp = g_renderer.camera.viewMatrix()
                        * g_renderer.camera.projMatrix(winW2 / winH2);
            XMMATRIX vpInv = XMMatrixInverse(nullptr, vp);

            XMVECTOR near4 = XMVector4Transform(XMVectorSet(ndcX, ndcY, 0, 1), vpInv);
            XMVECTOR far4  = XMVector4Transform(XMVectorSet(ndcX, ndcY, 1, 1), vpInv);
            near4 = XMVectorScale(near4, 1.f / XMVectorGetW(near4));
            far4  = XMVectorScale(far4,  1.f / XMVectorGetW(far4));
            XMVECTOR dir = XMVector3Normalize(XMVectorSubtract(far4, near4));

            // Find closest creature to ray
            float    bestDist = 3.f;
            EntityID bestID   = INVALID_ID;
            for (const auto& c : g_world.creatures) {
                if (!c.alive) continue;
                XMVECTOR toC = XMVectorSet(c.pos.x, c.pos.y, c.pos.z, 0)
                             - XMVectorSet(XMVectorGetX(near4), XMVectorGetY(near4),
                                           XMVectorGetZ(near4), 0);
                float t    = XMVectorGetX(XMVector3Dot(toC, dir));
                XMVECTOR closest = near4 + dir * t;
                XMVECTOR diff    = closest - XMVectorSet(c.pos.x, c.pos.y, c.pos.z, 0);
                float    d       = XMVectorGetX(XMVector3Length(diff));
                if (d < bestDist && t > 0) {
                    bestDist = d;
                    bestID   = c.id;
                }
            }
            g_ui.selectedID = bestID;
        }
        return 0;
    }

    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ── D3D11 helpers (unchanged from original example) ───────────────────────────
bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount                        = 2;
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags         = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage   = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow  = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed      = TRUE;
    sd.SwapEffect    = DXGI_SWAP_EFFECT_DISCARD;

    UINT createDeviceFlags = 0;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE,
                    nullptr, createDeviceFlags, featureLevelArray, 2,
                    D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice,
                    &featureLevel, &g_pd3dDeviceContext);
    if (res == DXGI_ERROR_UNSUPPORTED)
        res = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP,
                    nullptr, createDeviceFlags, featureLevelArray, 2,
                    D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice,
                    &featureLevel, &g_pd3dDeviceContext);
    if (res != S_OK) return false;

    IDXGIFactory* pFactory;
    if (SUCCEEDED(g_pSwapChain->GetParent(IID_PPV_ARGS(&pFactory)))) {
        pFactory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);
        pFactory->Release();
    }

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain)        { g_pSwapChain->Release();        g_pSwapChain        = nullptr; }
    if (g_pd3dDeviceContext) { g_pd3dDeviceContext->Release(); g_pd3dDeviceContext = nullptr; }
    if (g_pd3dDevice)        { g_pd3dDevice->Release();        g_pd3dDevice        = nullptr; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBack;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBack));
    g_pd3dDevice->CreateRenderTargetView(pBack, nullptr, &g_mainRenderTargetView);
    pBack->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = nullptr; }
}
