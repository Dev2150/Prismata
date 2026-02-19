// Prismata – Evolution Simulation
// ─────────────────────────────────────────────────────────────────────────────
// Entry point and application shell. Responsibilities:
//   1. Create and manage the Win32 window and D3D11 device/swap chain
//   2. Initialise Dear ImGui + ImPlot for the UI layer
//   3. Run the main loop: poll messages → tick sim → render 3D → render UI → present
//   4. Forward input events to the renderer (camera) and UI (creature selection)
//
// Architecture overview:
//   World      – simulation state (terrain, creatures, plants, species)
//   Renderer   – D3D11 rendering of terrain + creature billboards + camera
//   DataRecorder – 1-Hz population statistics ring buffer (feeds ImPlot graphs)
//   SimUI      – all Dear ImGui panels (controls, inspector, charts, etc.)
// ─────────────────────────────────────────────────────────────────────────────
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
// Kept as file-scope statics so helper functions (CreateDeviceD3D, WndProc, etc.)
// can access them without passing them around everywhere.
static ID3D11Device*           g_pd3dDevice          = nullptr;  // logical GPU interface; used to create resources (buffers, shaders, states)
static ID3D11DeviceContext*    g_pd3dDeviceContext    = nullptr;  // records and submits draw calls / state changes
static IDXGISwapChain*         g_pSwapChain           = nullptr;  // manages front/back buffers and Present()
static bool                    g_SwapChainOccluded    = false;    // true when the window is minimised/covered; we skip rendering
static UINT                    g_ResizeWidth          = 0,        // pending resize dimensions written in WM_SIZE,
                               g_ResizeHeight         = 0;        //   applied at the start of the next frame to avoid mid-frame resize
static ID3D11RenderTargetView* g_mainRenderTargetView = nullptr;  // view into the swap chain's back buffer; bound as the output render target

// ── Simulation globals ────────────────────────────────────────────────────────
// All four objects live for the entire duration of the program.
static World        g_world;     // terrain + creatures + plants + species registry
static DataRecorder g_recorder;  // samples population statistics at 1 Hz for graphing
static Renderer     g_renderer;  // D3D11 draw calls, camera, chunk mesh cache
static SimUI        g_ui;        // all ImGui panels; owns selectedID / showDemoWindow etc.

// ── Forward declarations ──────────────────────────────────────────────────────
bool    CreateDeviceD3D(HWND hWnd);
void    CleanupDeviceD3D();
void    CreateRenderTarget();
void    CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ── main ──────────────────────────────────────────────────────────────────────
int main(int, char**)
{
    // Make the process DPI-aware so the window and fonts render sharply on
    // high-DPI monitors. Must be called before any window is created.
    ImGui_ImplWin32_EnableDpiAwareness();

    // Query the DPI scale of the primary monitor so we can scale the window
    // size and ImGui style dimensions to match (e.g. 1.5 on a 144-DPI screen)
    float main_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(
        ::MonitorFromPoint(POINT{0,0}, MONITOR_DEFAULTTOPRIMARY));

    // ── Win32 window creation ─────────────────────────────────────────────────
    WNDCLASSEXW wc = {
        sizeof(wc),         // cbSize
        CS_CLASSDC,         // style: one DC shared across all windows of this class
        WndProc,            // lpfnWndProc
        0L, 0L,             // cbClsExtra, cbWndExtra
        GetModuleHandle(nullptr),  // hInstance
        nullptr, nullptr, nullptr, nullptr,
        L"Prismata",        // lpszClassName (must match DestroyWindow call)
        nullptr
    };
    ::RegisterClassExW(&wc);

    // Scale the logical 1600×900 design size by the DPI factor
    int winW = (int)(1600 * main_scale), winH = (int)(900 * main_scale);
    HWND hwnd = ::CreateWindowW(
        wc.lpszClassName, L"Prismata – Evolution Simulation",
        WS_OVERLAPPEDWINDOW,   // standard resizable/minimisable/maximisable window
        100, 100, winW, winH,  // initial position and size
        nullptr, nullptr, wc.hInstance, nullptr);

    // ── D3D11 device + swap chain ─────────────────────────────────────────────
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    ::ShowWindow(hwnd, SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    // ── Dear ImGui + ImPlot setup ─────────────────────────────────────────────
    // ImGui and ImPlot each maintain their own context (allocations, state, style).
    // Both must be created before any ImGui/ImPlot calls and destroyed at shutdown.
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // allow keyboard navigation of widgets
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;      // allow panels to be docked into each other
    io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;    // allow panels to be torn off into OS windows

    ImGui::StyleColorsDark();

    // Scale all ImGui padding, rounding, and spacing by the DPI factor so the
    // UI looks the same physical size on all monitors
    ImGuiStyle& style = ImGui::GetStyle();
    style.ScaleAllSizes(main_scale);
    style.FontScaleDpi = main_scale;
    io.ConfigDpiScaleFonts     = true;
    io.ConfigDpiScaleViewports = true;

    // When viewports are enabled (torn-off windows), the platform windows need
    // sharp square corners and a fully opaque background to blend with the OS
    if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        style.WindowRounding = 0.0f;
        style.Colors[ImGuiCol_WindowBg].w = 1.0f;
    }

    // Hook ImGui up to Win32 message handling and the D3D11 device/context
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dDeviceContext);

    // ── Simulation init ───────────────────────────────────────────────────────
    // generate() seeds the Perlin noise, creates the terrain chunks, and spawns
    // the initial creature population. 42 = world seed, 16×16 = chunk grid.
    g_world.generate(42, 16, 16);

    // init() compiles HLSL shaders, creates GPU buffers, and builds the depth
    // buffer. Returns false on any D3D failure (e.g. driver doesn't support SM5).
    if (!g_renderer.init(g_pd3dDevice, g_pd3dDeviceContext, winW, winH))
    {
        OutputDebugStringA("FATAL: Renderer initialization failed!\n");
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // ── Main loop ─────────────────────────────────────────────────────────────
    // Uses std::chrono::high_resolution_clock for sub-millisecond frame timing.
    // dt is capped at 50 ms (20 FPS minimum) to prevent the simulation from
    // making enormous jumps if the window is dragged, the system stalls, etc.
    using Clock = std::chrono::high_resolution_clock;
    auto lastTime = Clock::now();
    bool done = false;

    while (!done)
    {
        // ── Drain the Win32 message queue ─────────────────────────────────
        // PeekMessage with PM_REMOVE retrieves and removes one message at a time.
        // We loop until the queue is empty so we don't fall behind on input.
        // WM_QUIT sets done=true to cleanly exit the loop.
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            ::TranslateMessage(&msg);   // translate VK_* to WM_CHAR for text input
            ::DispatchMessage(&msg);    // route to WndProc
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        // ── Handle occlusion (window minimised / covered by another window) ─
        // DXGI_PRESENT_TEST checks if presentation would succeed without actually
        // presenting. While occluded, we sleep and skip rendering to avoid wasting
        // GPU time on invisible frames.
        if (g_SwapChainOccluded &&
            g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) {
            ::Sleep(10); continue;
        }
        g_SwapChainOccluded = false;

        // ── Handle window resize ───────────────────────────────────────────
        // Resize is deferred from WM_SIZE to here because D3D11 buffers cannot
        // be resized while they are bound as render targets. The sequence is:
        //   a) Release the RTV (unbinds the back buffer)
        //   b) ResizeBuffers (resizes the swap chain)
        //   c) Recreate the RTV against the new back buffer
        //   d) Tell the renderer to rebuild its depth buffer at the new size
        if (g_ResizeWidth != 0 && g_ResizeHeight != 0) {
            CleanupRenderTarget();
            g_pSwapChain->ResizeBuffers(0, g_ResizeWidth, g_ResizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_ResizeWidth = g_ResizeHeight = 0;
            CreateRenderTarget();
            RECT rc; ::GetClientRect(hwnd, &rc);
            g_renderer.resize(rc.right - rc.left, rc.bottom - rc.top);
        }

        // ── Compute delta time ─────────────────────────────────────────────
        auto now = Clock::now();
        float dt = std::chrono::duration<float>(now - lastTime).count();
        lastTime = now;
        dt = std::min(dt, 0.05f);  // cap at 50 ms to prevent huge simulation steps

        // ── Update simulation and recording ────────────────────────────────
        // tickCamera must run before render so the view matrix is fresh.
        // World::tick() advances all creatures, plants, and reproduction.
        // DataRecorder::tick() may or may not fire depending on its 1-Hz timer.
        g_renderer.tickCamera(dt, g_world);
        g_world.tick(dt);
        g_recorder.tick(dt, g_world);

        // ── Clear render targets ───────────────────────────────────────────
        // Clear the back buffer to a dark blue-grey (matches the sky/ambient tone).
        // Then bind it as the output RTV together with the depth-stencil view so
        // the renderer's draw calls write colour and depth correctly.
        const float cc[4] = {0.08f, 0.10f, 0.12f, 1.f};
        g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, cc);
        g_pd3dDeviceContext->OMSetRenderTargets(1, &g_mainRenderTargetView, g_renderer.depthDSV);
        if (g_renderer.depthDSV)
            g_pd3dDeviceContext->ClearDepthStencilView(
                g_renderer.depthDSV, D3D11_CLEAR_DEPTH, 1.f, 0);  // 1.0 = far plane (everything fails initially)

        // ── Set viewport ───────────────────────────────────────────────────
        // The viewport maps NDC coordinates to pixel coordinates on the back buffer.
        // MinDepth=0 and MaxDepth=1 (default) map clip-space Z to [0,1] depth buffer range.
        RECT rc; ::GetClientRect(hwnd, &rc);
        D3D11_VIEWPORT vp{};
        vp.Width    = (float)(rc.right  - rc.left);
        vp.Height   = (float)(rc.bottom - rc.top);
        vp.MaxDepth = 1.f;
        g_pd3dDeviceContext->RSSetViewports(1, &vp);

        // ── 3D render pass ─────────────────────────────────────────────────
        // Renders terrain (indexed triangle lists) then creature billboards
        // (instanced triangle strips with alpha blending).
        float aspect = vp.Width / std::max(vp.Height, 1.f);
        g_renderer.render(g_world, aspect);

        // ── ImGui / ImPlot UI render pass ──────────────────────────────────
        // NewFrame() must be called after the platform back-ends have processed
        // input (ImGui_ImplWin32_NewFrame reads mouse/keyboard state from Win32)
        // and before any ImGui:: draw calls.
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // DockSpaceOverViewport creates an invisible fullscreen docking host so
        // all ImGui panels can be docked anywhere on screen.
        // PassthruCentralNode = the 3D viewport shows through the empty central area.
        ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport(),
            ImGuiDockNodeFlags_PassthruCentralNode);

        // Draw all simulation UI panels (controls, inspector, charts, species, etc.)
        g_ui.draw(g_world, g_recorder, g_renderer);

        // Render() finalises the ImGui draw lists into indexed vertex buffers.
        // RenderDrawData() uploads them to the GPU and issues draw calls.
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // If viewports are enabled, update and render any torn-off ImGui windows
        // that live in their own OS windows (separate HWNDs).
        if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
            ImGui::UpdatePlatformWindows();
            ImGui::RenderPlatformWindowsDefault();
        }

        // ── Present ───────────────────────────────────────────────────────
        // Present(1, 0) = sync to VBlank (vsync on). Swap the back and front buffers.
        // Returns DXGI_STATUS_OCCLUDED if the window became covered this frame;
        // we store that and skip rendering next frame until it's uncovered.
        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
    }

    // ── Shutdown ──────────────────────────────────────────────────────────────
    // Release everything in reverse initialisation order to avoid dangling references.
    g_renderer.shutdown();          // release D3D buffers, shaders, states
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    CleanupDeviceD3D();             // release device, device context, swap chain, RTV
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}

// ── WndProc ───────────────────────────────────────────────────────────────────
// Win32 window procedure: receives all window messages for our HWND.
// ImGui_ImplWin32_WndProcHandler is called first; it returns true if ImGui
// consumed the message (e.g. a mouse click on an ImGui panel) so we don't
// also process it as a game input.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;

    switch (msg) {

    // WM_SIZE fires whenever the window is resized (including minimise/restore).
    // We defer the actual buffer resize to the main loop (see step 3 above)
    // because we can't safely resize D3D resources from inside WndProc.
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;  // ignore minimise (width/height = 0)
        g_ResizeWidth  = LOWORD(lParam);
        g_ResizeHeight = HIWORD(lParam);
        return 0;

    // Forward keyboard events to the renderer for camera movement.
    // WantCaptureKeyboard is true when ImGui has a text field focused,
    // so we don't move the camera while the user types in a file path etc.
    case WM_KEYDOWN:
    case WM_KEYUP:
        if (!ImGui::GetIO().WantCaptureKeyboard)
            g_renderer.onKey((int)wParam, msg == WM_KEYDOWN);
        return 0;

    // Capture/release the mouse on right-click so we can read WM_MOUSEMOVE
    // delta even when the cursor leaves the window boundary during a drag.
    case WM_RBUTTONDOWN: SetCapture(hWnd);  return 0;
    case WM_RBUTTONUP:   ReleaseCapture();  return 0;

    // Mouse movement: compute delta from the previous position and forward to
    // the renderer for camera yaw/pitch when right-button is held.
    // Static variables persist across calls to track the last known position.
    case WM_MOUSEMOVE: {
        static int lastMX = 0, lastMY = 0;
        int mx = (short)LOWORD(lParam), my = (short)HIWORD(lParam);
        if (!ImGui::GetIO().WantCaptureMouse)
            g_renderer.onMouseMove(mx - lastMX, my - lastMY, (wParam & MK_RBUTTON) != 0);
        lastMX = mx; lastMY = my;
        return 0;
    }

    // ── Ray-picking: select a creature by left-clicking on the viewport ───────
    // Converts the 2D screen click into a 3D world-space ray, then finds the
    // living creature whose position is closest to that ray (within 3 m).
    case WM_LBUTTONDOWN: {
        if (ImGui::GetIO().WantCaptureMouse) break;  // click was on an ImGui panel

        RECT rc; ::GetClientRect(hWnd, &rc);
        float W = (float)(rc.right - rc.left);
        float H = (float)(rc.bottom - rc.top);
        float mx = (float)(short)LOWORD(lParam);
        float my = (float)(short)HIWORD(lParam);

        // Convert pixel coordinates to Normalised Device Coordinates (NDC):
        //   NDC X: -1 (left edge) to +1 (right edge)
        //   NDC Y: +1 (top edge)  to -1 (bottom edge)  ← note Y flip
        float ndcX =  (mx / W) * 2.f - 1.f;
        float ndcY = -(my / H) * 2.f + 1.f;

        // Compute the inverse of the combined View×Projection matrix.
        // This lets us unproject from clip space back to world space.
        Mat4 vp    = g_renderer.camera.viewMatrix() * g_renderer.camera.projMatrix(W / H);
        Mat4 vpInv = vp.inversed();

        // Unproject two points at different clip-space depths:
        //   z=0 → near plane in NDC (maps to the near clip plane in world space)
        //   z=1 → far plane in NDC  (maps to the far clip plane in world space)
        // Together they define the start and end of the pick ray.
        auto unproject = [&](float z) -> Vec4 {
            Vec4 clip = {ndcX, ndcY, z, 1.f};
            Vec4 world = vpInv.transform(clip);
            // Perspective divide: divide XYZ by W to convert from homogeneous
            // coordinates back to Cartesian world-space coordinates
            float invW = (std::abs(world.w) > 1e-7f) ? 1.f / world.w : 0.f;
            return {world.x * invW, world.y * invW, world.z * invW, 1.f};
        };

        Vec4 near4 = unproject(0.f);
        Vec4 far4  = unproject(1.f);

        // Normalise the ray direction vector
        float dx = far4.x - near4.x, dy = far4.y - near4.y, dz = far4.z - near4.z;
        float dl = std::sqrt(dx*dx + dy*dy + dz*dz);
        if (dl < 1e-6f) break;
        dx /= dl; dy /= dl; dz /= dl;

        // Find the living creature whose position is within 3 m of the ray.
        // For each creature, compute the perpendicular distance from its centre
        // to the ray using the formula: d = |OC - (OC·d)d| where OC is the
        // vector from the ray origin to the creature centre and d is the ray direction.
        float    bestDist = 3.f;   // selection radius: 3 m from the ray
        EntityID bestID   = INVALID_ID;
        for (const auto& c : g_world.creatures) {
            if (!c.alive) continue;
            // Vector from ray origin (near4) to creature centre
            float ocx = c.pos.x - near4.x, ocy = c.pos.y - near4.y, ocz = c.pos.z - near4.z;
            // Scalar projection of OC onto the ray direction (how far along the ray)
            float t   = ocx*dx + ocy*dy + ocz*dz;
            if (t < 0.f) continue;  // creature is behind the camera
            // Closest point on ray to creature centre
            float cx2 = near4.x + dx*t - c.pos.x;
            float cy2 = near4.y + dy*t - c.pos.y;
            float cz2 = near4.z + dz*t - c.pos.z;
            float d   = std::sqrt(cx2*cx2 + cy2*cy2 + cz2*cz2);  // perpendicular distance
            if (d < bestDist) { bestDist = d; bestID = c.id; }
        }
        // Store the selected creature ID in the UI; the inspector panel reads this
        g_ui.selectedID = bestID;
        return 0;
    }

    // WM_CHAR is better than WM_KEYDOWN for single-press actions because it
    // fires once per key press (WM_KEYDOWN repeats while held).
    case WM_CHAR:
        // ── possess a random living creature ─────────────────────────────
        if (wParam == 'p' || wParam == 'P') {
            if (!ImGui::GetIO().WantCaptureKeyboard) {
                EntityID randomID = g_world.findRandomLivingCreature();
                if (randomID != INVALID_ID) {
                    g_renderer.playerID = randomID;  // camera follows this creature
                    g_ui.selectedID = randomID;      // also select it in the inspector
                }
            }
        }
        return 0;

    // Suppress the default Alt+Enter full-screen toggle that DXGI would otherwise
    // intercept. We don't support full-screen so this prevents a broken state.
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;

    case WM_DESTROY:
        ::PostQuitMessage(0);  // posts WM_QUIT to the message queue, causing the main loop to exit
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ── D3D11 device and swap chain creation ──────────────────────────────────────
// Attempts hardware acceleration first; falls back to WARP (software rasteriser)
// if no compatible GPU is present. WARP is slow but correct, useful for CI/VMs.
bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;                                  // double-buffering (front + back)
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;  // standard 8-bit RGBA back buffer
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

void CleanupDeviceD3D() {
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

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView=nullptr; }
}