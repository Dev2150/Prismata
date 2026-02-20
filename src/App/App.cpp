#include "imgui.hpp"
#include "imgui_impl_win32.hpp"
#include "imgui_impl_dx11.hpp"
#include "implot.hpp"
#include <chrono>
#include <algorithm>
#include <cmath>

#include "App_Globals.hpp"

int RunApplication()
{
    // Make the process DPI-aware so the window and fonts render sharply on
    // high-DPI monitors. Must be called before any window is created.
    ImGui_ImplWin32_EnableDpiAwareness();

    // Query the DPI scale of the primary monitor so we can scale the window
    // size and ImGui style dimensions to match (e.g. 1.5 on a 144-DPI screen)
    float dpi = ImGui_ImplWin32_GetDpiScaleForMonitor(
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
    int winW = (int)(1600 * dpi), winH = (int)(900 * dpi);
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

    // ── Start maximised ───────────────────────────────────────────────────────
    ::ShowWindow(hwnd, SW_SHOWMAXIMIZED);
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
    style.ScaleAllSizes(dpi);
    style.FontScaleDpi = dpi;
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
    RECT initialRc; ::GetClientRect(hwnd, &initialRc);
    int initW = initialRc.right  - initialRc.left;
    int initH = initialRc.bottom - initialRc.top;

    if (!g_renderer.init(g_pd3dDevice, g_pd3dDeviceContext, initW, initH))
    {
        OutputDebugStringA("FATAL: Renderer initialization failed!\n");
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    PlanetConfig pcfg;
    pcfg.radius          = 1000.f;
    pcfg.center          = {0.f, 1800.f, 0.f};   // planet below the flat world
    pcfg.heightScale     = 120.f;     // max terrain height above sea level
    pcfg.maxDepth        = 16;        // deepest LOD level (~1.5m patches at max)
    pcfg.patchRes        = 17;        // 17×17 vertices per patch (16×16 quads)
    pcfg.splitThreshold  = 1.2f;      // tune for quality vs performance

    if (!g_planet.init(g_pd3dDevice, g_pd3dDeviceContext, pcfg)) {
        OutputDebugStringA("FATAL: Planet init failed!\n");
        // handle error
    }

    // ── Auto-load default settings ────────────────────────────────────────────
    // Attempt to load "default.json" on startup; silently ignore if missing.
    g_ui.loadSettingsFromFile("default.json", g_world, g_renderer);

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

        // Sync selectedID so renderer can draw FOV cone
        g_renderer.selectedID = g_ui.selectedID;

        // tickCamera must run before render so the view matrix is fresh.
        g_renderer.tickCamera(dt, g_world);
        // update quadtree LOD
        g_planet.update(g_renderer.camera);
        // World::tick() advances all creatures, plants, and reproduction
        g_world.tick(dt);
        // DataRecorder::tick() may or may not fire depending on its 1-Hz timer.
        g_recorder.tick(dt, g_world);

        // Sky colour: deep blue at night → orange at dawn/dusk → pale sky blue at noon.
        // Matches the ambient/sun colours computed in Renderer_Frame.cpp so the
        // horizon blends naturally into the background.
        {
            float time_of_day = g_world.timeOfDay();   // [0,1)
            float elev = -std::cos(time_of_day * 2.f * 3.14159265f); // -1=night, +1=noon

            // Night sky colour
            float skyNight[3]   = {0.01f, 0.01f, 0.04f};
            // Dawn/dusk horizon colour
            float skyHorizon[3] = {0.30f, 0.15f, 0.08f};
            // Daytime sky colour
            float skyDay[3]     = {0.38f, 0.58f, 0.82f};

            auto sStep = [](float lo, float hi, float x) {
                float t = std::max(0.f, std::min(1.f, (x-lo)/(hi-lo)));
                return t*t*(3.f-2.f*t);
            };
            float h1 = sStep(-0.15f, 0.20f, elev);  // night→horizon
            float h2 = sStep( 0.15f, 0.55f, elev);  // horizon→day

            float r = skyNight[0] + (skyHorizon[0]-skyNight[0])*h1 + (skyDay[0]-skyHorizon[0])*h2;
            float g = skyNight[1] + (skyHorizon[1]-skyNight[1])*h1 + (skyDay[1]-skyHorizon[1])*h2;
            float b = skyNight[2] + (skyHorizon[2]-skyNight[2])*h1 + (skyDay[2]-skyHorizon[2])*h2;

            const float cc[4] = {r, g, b, 1.f};
            g_pd3dDeviceContext->ClearRenderTargetView(g_mainRenderTargetView, cc);
        }


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

        if (g_renderer.depthDSV)
            g_pd3dDeviceContext->ClearDepthStencilView(
                g_renderer.depthDSV, D3D11_CLEAR_DEPTH, 1.f, 0);

        g_planet.render(g_renderer.camera, aspect,
                g_world.timeOfDay(), g_world.simTime);

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

        // Pass window dimensions to UI so it can do terrain hover raycasting
        g_ui.windowW = (int)vp.Width;
        g_ui.windowH = (int)vp.Height;

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
    g_planet.shutdown();
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

