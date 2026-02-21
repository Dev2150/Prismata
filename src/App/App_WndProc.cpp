#include "App_Globals.hpp"
#include "imgui.hpp"
#include "Core/Math.hpp"
#include <algorithm>
#include <cmath>

// Win32 window procedure: receives all window messages for our HWND.
// ImGui_ImplWin32_WndProcHandler is called first; it returns true if ImGui
// consumed the message (e.g. a mouse click on an ImGui panel) so we don't
// also process it as a game input.

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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
            // Always forward to renderer for camera movement (regardless of ImGui focus)
            g_renderer.onKey((int)wParam, msg == WM_KEYDOWN);

            if (msg == WM_KEYDOWN) {
                // ── Space bar: toggle pause ── always active ──────────────────────
                if (wParam == VK_SPACE)
                    g_world.cfg.paused = !g_world.cfg.paused;

                // ── +/= key or numpad +: increase simulation speed ─────────────────
                if (wParam == VK_OEM_PLUS || wParam == VK_ADD)
                    g_world.cfg.simSpeed = std::min(20.f, g_world.cfg.simSpeed * 1.25f);

                // ── - key or numpad -: decrease simulation speed ───────────────────
                if (wParam == VK_OEM_MINUS || wParam == VK_SUBTRACT)
                    g_world.cfg.simSpeed = std::max(0.1f, g_world.cfg.simSpeed / 1.25f);
            }
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

        // ── Mouse wheel: zoom in/out (radial movement along planet normal) ──────
        // WHEEL_DELTA = 120 per notch. It's normalized to notch count (±1 per notch).
        // Positive = scroll up = move away from planet (zoom out).
        case WM_MOUSEWHEEL: {
            if (!ImGui::GetIO().WantCaptureMouse) {
                float notches = (float)(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA;
                g_renderer.onMouseScroll(notches);
            }
            return 0;
        }

        case WM_LBUTTONDOWN: {
            if (ImGui::GetIO().WantCaptureMouse) break;  // click was on an ImGui panel

            RECT rc; ::GetClientRect(hWnd, &rc);
            float W = (float)(rc.right - rc.left);
            float H = (float)(rc.bottom - rc.top);
            float mx = (short)LOWORD(lParam);
            float my = (short)HIWORD(lParam);

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

            // ── WM_CHAR: single-press actions ─────────────────────────────────────────
        case WM_CHAR:
            if (ImGui::GetIO().WantCaptureKeyboard) return 0;

            // ── P: possess a random creature ────────────────────────────────────
            if (wParam == 'p' || wParam == 'P') {
                EntityID toPos = g_world.findRandomLivingCreature();
                if (toPos != INVALID_ID) {
                    g_renderer.playerID = toPos;
                    g_ui.selectedID     = toPos;
                }
            }
            // ── T: toggle possession of selected creature ───────────────────────
            else if (wParam == 't' || wParam == 'T') {
                if (g_renderer.playerID != INVALID_ID) {
                    // Release possession
                    g_renderer.playerID      = INVALID_ID;
                    g_renderer.hasPossessOffset = false;
                    g_renderer.showFogOfWar  = false;
                } else if (g_ui.selectedID != INVALID_ID) {
                    g_renderer.playerID = g_ui.selectedID;
                    }
                }
            // ── J: toggle hiding objects outside FOV ────────────────────────────
            else if (wParam == 'j' || wParam == 'J') {
                g_renderer.hideOutsideFOV = !g_renderer.hideOutsideFOV;
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