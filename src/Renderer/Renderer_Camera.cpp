#include "Renderer.hpp"
#include <cmath>
#include <algorithm>

// ── Renderer_Camera.cpp ───────────────────────────────────────────────────────
// Covers: tickCamera, onMouseMove, onKey, screenToTerrain.

// ── tickCamera ────────────────────────────────────────────────────────────────
// Two modes:
//   POSSESS: camera translates rigidly with the creature; angle and distance
//            are frozen at whatever they were when possession began. The offset
//            (camera world-pos minus creature world-pos) is recorded on the
//            first tick after playerID is set and held constant thereafter.
//   FREE:    WASD/QF keyboard movement in the camera's local frame.
void Renderer::tickCamera(float dt, const World& world) {

    if (playerID != INVALID_ID) {
        auto it = world.idToIndex.find(playerID);
        if (it == world.idToIndex.end() || !world.creatures[it->second].alive) {
            // Creature died — drop back to free cam and forget the offset.
            playerID        = INVALID_ID;
            hasPossessOffset = false;
            return;
        }
        const Creature& creature = world.creatures[it->second];

        // ── Record the fixed offset the first time we follow this creature ──────
        // We capture whatever angle and distance the player was at so possession
        // feels like "grabbing onto" the creature rather than snapping to a preset.
        if (!hasPossessOffset) {
            possessOffset.x = camera.pos.x - creature.pos.x;
            possessOffset.y = camera.pos.y - creature.pos.y;
            possessOffset.z = camera.pos.z - creature.pos.z;
            hasPossessOffset = true;
        }

        // ── Translate camera to maintain the fixed offset ─────────────────────
        // Target is simply the creature's current position + the frozen offset.
        // Smooth approach (exponential lag) avoids jarring teleports if the
        // creature moves very fast or we just started following it.
        Float3 target = {
            creature.pos.x + possessOffset.x,
            creature.pos.y + possessOffset.y,
            creature.pos.z + possessOffset.z,
        };

        // Exponential smooth approach: blend = 1 - e^(-speed*dt)
        // Frame-rate independent — gives the same visual speed at any FPS.
        float blend = 1.0f - std::exp(-dt * camera.follow_speed);
        camera.pos.x += (target.x - camera.pos.x) * blend;
        camera.pos.y += (target.y - camera.pos.y) * blend;
        camera.pos.z += (target.z - camera.pos.z) * blend;

    }
    else {
        // Clear offset state so the next possession starts fresh.
        hasPossessOffset = false;

        // Free-look: WASD moves in the camera's local XZ plane; QF moves vertically
        float spd = camera.translation_speed * dt;
        Float3 f  = camera.forward();
        // Right vector: perpendicular to forward in the XZ plane (no vertical component)
        Float3 r  = { f.z, 0.f, -f.x };
        float  rl = std::sqrt(r.x*r.x + r.z*r.z);
        if (rl > 1e-6f) { r.x/=rl; r.z/=rl; }

        // moveKeys[]: [0]=W(fwd) [1]=R(back) [2]=S(left) [3]=A(right) [4]=F(up) [5]=Q(down)
        camera.pos.x += (f.x*(moveKeys[0]-moveKeys[1]) + r.x*(moveKeys[3]-moveKeys[2])) * spd;
        camera.pos.y += (moveKeys[4] - moveKeys[5]) * spd;
        camera.pos.z += (f.z*(moveKeys[0]-moveKeys[1]) + r.z*(moveKeys[3]-moveKeys[2])) * spd;
    }
}

// ── onMouseMove ───────────────────────────────────────────────────────────────
// Right-button drag rotates the camera (yaw = left/right, pitch = up/down).
// Works in both free-cam and possess mode so the player can look around a possessed
// creature without the camera auto-correcting the angle.
void Renderer::onMouseMove(int dx, int dy, bool rightDown) {
    if (!rightDown) return;
    camera.yaw   += dx * 0.003f;
    camera.pitch += dy * 0.003f;
    camera.pitch  = std::max(-1.5f, std::min(0.2f, camera.pitch));  // clamp to avoid gimbal flip
}

// ── onKey ─────────────────────────────────────────────────────────────────────
// Stores key state (1=down, 0=up) for the six movement keys.
void Renderer::onKey(int vk, bool down) {
    float v = down ? 1.f : 0.f;
    switch (vk) {
        case 'W': moveKeys[0]=v; break;  // forward
        case 'R': moveKeys[1]=v; break;  // backward
        case 'S': moveKeys[2]=v; break;  // strafe left
        case 'A': moveKeys[3]=v; break;  // strafe right
        case 'F': moveKeys[4]=v; break;  // up
        case 'Q': moveKeys[5]=v; break;  // down
    }
}

// ── screenToTerrain ───────────────────────────────────────────────────────────
// Converts a 2D screen pixel (mx, my) into a 3D world position on the terrain.
//
// Steps:
//   1. Convert pixel → NDC (Normalised Device Coordinates, range [-1,1])
//   2. Unproject two NDC points (at near and far clip planes) → world space
//      using the inverse view-projection matrix
//   3. Build a ray from near → far
//   4. March along the ray until it drops below the terrain height field
//   5. Binary-search the last step for a more precise hit point
bool Renderer::screenToTerrain(float mx, float my, float W, float H,
                               const World& world, Vec3& outPos, uint8_t& outMat) const {
    if (W < 1.f || H < 1.f) return false;

    // Screen pixel → NDC: X in [-1,1], Y flipped (screen Y down, NDC Y up)
    float ndcX =  (mx / W) * 2.f - 1.f;
    float ndcY = -(my / H) * 2.f + 1.f;

    // Invert view*projection to unproject from clip space back to world space
    Mat4 vp    = camera.viewMatrix() * camera.projMatrix(W / H);
    Mat4 vpInv = vp.inversed();

    auto unproject = [&](float z) -> Vec4 {
        Vec4 clip = {ndcX, ndcY, z, 1.f};
        Vec4 w    = vpInv.transform(clip);
        // Perspective divide: divide XYZ by W to recover Cartesian world position
        float iw  = (std::abs(w.w) > 1e-7f) ? 1.f / w.w : 0.f;
        return {w.x*iw, w.y*iw, w.z*iw, 1.f};
    };

    Vec4 near4 = unproject(0.f);   // ray origin (at near clip plane)
    Vec4 far4  = unproject(1.f);   // ray end    (at far clip plane)

    float dx = far4.x - near4.x, dy = far4.y - near4.y, dz = far4.z - near4.z;
    float dl = std::sqrt(dx*dx + dy*dy + dz*dz);
    if (dl < 1e-6f) return false;
    dx /= dl; dy /= dl; dz /= dl;  // normalise ray direction

    // Coarse ray march: walk 1 metre at a time until ray dips below terrain
    float maxT = 800.f, step = 1.0f, prevT = 0.f;
    float worldMaxX = (float)(world.worldCX * CHUNK_SIZE - 1);
    float worldMaxZ = (float)(world.worldCZ * CHUNK_SIZE - 1);

    for (float t = step; t < maxT; t += step) {
        float rx = near4.x + dx * t;
        float ry = near4.y + dy * t;
        float rz = near4.z + dz * t;

        if (rx < 0.f || rx > worldMaxX || rz < 0.f || rz > worldMaxZ) {
            prevT = t; continue;
        }

        if (ry <= world.heightAt(rx, rz)) {
            // Binary search between prevT and t for a ~1/256 m precision hit
            float lo = prevT, hi = t;
            for (int iter = 0; iter < 8; iter++) {
                float mid = (lo + hi) * 0.5f;
                float mry = near4.y + dy * mid;
                float mth = world.heightAt(near4.x + dx*mid, near4.z + dz*mid);
                if (mry <= mth) hi = mid;
                else             lo = mid;
            }
            float fx = near4.x + dx * hi;
            float fz = near4.z + dz * hi;
            outPos = {fx, world.heightAt(fx, fz), fz};
            outMat = world.materialAt(fx, fz);
            return true;
        }
        prevT = t;
    }
    return false;  // ray aimed at sky
}
