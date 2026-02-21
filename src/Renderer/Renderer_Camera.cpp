#include "Renderer.hpp"
#include "World/World.hpp"
#include "World/World_Planet.hpp"
#include <cmath>
#include <algorithm>

// ── tickCamera ────────────────────────────────────────────────────────────────
// Two modes:
//   POSSESS: camera translates rigidly with the creature; angle and distance
//            are frozen at whatever they were when possession began.
//   FREE:    WASD/QE keyboard movement in the camera's local frame.
//            W/S  = forward/backward along tangent plane
//            A/D  = strafe left/right along tangent plane
//            F    = radial out (away from planet)
//            R    = radial in  (toward planet)
//            Q/E  = yaw left/right (rotate camera heading)
//            Mouse wheel = zoom (radial move)
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

        // ── Orbit-follow: keep the same offset in planet-normal space ─────────
        // Reproject the stored offset into the creature's current local frame
        // (normal, east, north) so the camera orbits with the creature as it
        // moves across the sphere rather than flying off in a straight line.

        // Planet normal at creature position (= "up" on the sphere)
        Vec3 creatureNormal = g_planet_surface.normalAt(creature.pos);

        // Reconstruct an orthonormal frame (normal, east, north)
        Vec3 arb = (std::abs(creatureNormal.y) < 0.9f)
                 ? Vec3{0.f, 1.f, 0.f} : Vec3{1.f, 0.f, 0.f};
        Vec3 east = Vec3{
            creatureNormal.y * arb.z - creatureNormal.z * arb.y,
            creatureNormal.z * arb.x - creatureNormal.x * arb.z,
            creatureNormal.x * arb.y - creatureNormal.y * arb.x
        }.normalised();
        Vec3 north = Vec3{
            creatureNormal.y * east.z - creatureNormal.z * east.y,
            creatureNormal.z * east.x - creatureNormal.x * east.z,
            creatureNormal.x * east.y - creatureNormal.y * east.x
        }.normalised();

        // Decompose stored offset into the local frame components
        Vec3 off = { possessOffset.x, possessOffset.y, possessOffset.z };
        float dNormal = off.dot(creatureNormal);
        float dEast   = off.dot(east);
        float dNorth  = off.dot(north);

        // Recompose in the creature's current frame
        Float3 target = {
            creature.pos.x + creatureNormal.x * dNormal + east.x * dEast + north.x * dNorth,
            creature.pos.y + creatureNormal.y * dNormal + east.y * dEast + north.y * dNorth,
            creature.pos.z + creatureNormal.z * dNormal + east.z * dEast + north.z * dNorth,
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

        // ── Spherical free-cam ────────────────────────────────────────────────
        Vec3 camPos3 = { camera.pos.x, camera.pos.y, camera.pos.z };
        Vec3 camNormal = g_planet_surface.normalAt(camPos3);

        // ── Q/E yaw rotation ─────────────────────────────────────────────────
        // Rotate camera.yaw around the planet surface normal.
        // This lets the player change the "forward" direction smoothly without
        // any gimbal lock issues because we keep yaw/pitch as canonical state.
        float yawInput = moveKeys[6] - moveKeys[7];  // E - Q
        if (std::abs(yawInput) > 1e-4f) {
            camera.yaw += yawInput * dt * 1.5f;
            // Keep yaw in [-pi, pi] range to prevent float drift
            const float PI = 3.14159265f;
            while (camera.yaw >  PI) camera.yaw -= 2.f * PI;
            while (camera.yaw < -PI) camera.yaw += 2.f * PI;
        }

        // ── Mouse wheel zoom (radial movement) ───────────────────────────────
        if (std::abs(scrollDelta) > 1e-4f) {
            Vec3 radialDir = camNormal;  // outward from planet centre
            float zoomSpd  = camera.translation_speed * 0.3f;
            camera.pos.x  += radialDir.x * scrollDelta * zoomSpd;
            camera.pos.y  += radialDir.y * scrollDelta * zoomSpd;
            camera.pos.z  += radialDir.z * scrollDelta * zoomSpd;
            scrollDelta = 0.f;
        }

        // Camera forward projected onto the tangent plane
        Float3 fwd3  = camera.forward();
        Vec3   fwdV  = { fwd3.x, fwd3.y, fwd3.z };

        float fdotn  = fwdV.dot(camNormal);
        Vec3 tangFwd = Vec3{ fwdV.x - camNormal.x * fdotn,
                             fwdV.y - camNormal.y * fdotn,
                             fwdV.z - camNormal.z * fdotn }.normalised();

        // Tangential right = tangFwd × normal
        Vec3 tangRight = Vec3{
            tangFwd.y * camNormal.z - tangFwd.z * camNormal.y,
            tangFwd.z * camNormal.x - tangFwd.x * camNormal.z,
            tangFwd.x * camNormal.y - tangFwd.y * camNormal.x
        }.normalised();

        float spd = camera.translation_speed * dt;
        Float3 f  = camera.forward();
        // Right vector: perpendicular to forward in the XZ plane (no vertical component)
        Float3 r  = { f.z, 0.f, -f.x };
        float  rl = std::sqrt(r.x*r.x + r.z*r.z);
        if (rl > 1e-6f) { r.x/=rl; r.z/=rl; }

        // W/R  = forward/backward along the sphere surface (tangential)
        // A/S  = strafe left/right                         (tangential)
        // F/Q  = move away from / toward the planet centre (radial)
        float fwdInput   = moveKeys[0] - moveKeys[1];   // W - R
        float strafeInput= moveKeys[3] - moveKeys[2];   // A - S
        float radialInput= moveKeys[4] - moveKeys[5];   // F - Q

        bool moving = (std::abs(fwdInput) > 1e-4f) ||
                      (std::abs(strafeInput) > 1e-4f);

        // Store camera normal before movement for curvature correction
        Vec3 camNormalBefore = camNormal;

        camera.pos.x += (tangFwd.x   * fwdInput   +
                         tangRight.x * strafeInput +
                         camNormalBefore.x * radialInput) * spd;
        camera.pos.y += (tangFwd.y   * fwdInput   +
                         tangRight.y * strafeInput +
                         camNormalBefore.y * radialInput) * spd;
        camera.pos.z += (tangFwd.z   * fwdInput   +
                         tangRight.z * strafeInput +
                         camNormalBefore.z * radialInput) * spd;

        // ── Auto-correct orientation for sphere curvature ─────────────────────
        // When the camera moves along the sphere surface, the local "up" rotates.
        // We apply the same rotation to the camera's forward direction so the
        // horizon stays level without the player having to compensate manually.
        //
        // Instead of extracting yaw/pitch from the rotated forward vector
        // (which causes gimbal lock and spinning near Z=0), we maintain yaw/pitch
        // as the canonical state and only apply the curvature correction as a delta
        // to the existing yaw angle. This is stable everywhere on the sphere.
        if (moving) {
            Vec3 newCamPos   = { camera.pos.x, camera.pos.y, camera.pos.z };
            Vec3 camNormalAfter = g_planet_surface.normalAt(newCamPos);

            // Rotation axis = cross(N_before, N_after)
            Vec3 axis = {
                camNormalBefore.y * camNormalAfter.z - camNormalBefore.z * camNormalAfter.y,
                camNormalBefore.z * camNormalAfter.x - camNormalBefore.x * camNormalAfter.z,
                camNormalBefore.x * camNormalAfter.y - camNormalBefore.y * camNormalAfter.x
            };
            float sinA = axis.len();
            float cosA = camNormalBefore.dot(camNormalAfter);

            if (sinA > 1e-6f) {
                axis = axis * (1.f / sinA);  // normalise

                // Project axis onto the current surface normal to get the
                // yaw-equivalent rotation component. This tells us how much the
                // "north" direction has rotated as seen from the local frame,
                // which is exactly what we need to add to camera.yaw.
                //
                // The signed angle of rotation projected onto the normal axis:
                //   yaw_delta = atan2(sinA * dot(axis, normal), cosA)
                // But since sinA is small per-frame, we can use the small-angle
                // approximation: yaw_delta ≈ sinA * dot(axis, normal_before)
                float yawDelta = sinA * axis.dot(camNormalBefore);
                camera.yaw += yawDelta;

                // Keep yaw in [-pi, pi]
                const float PI = 3.14159265f;
                while (camera.yaw >  PI) camera.yaw -= 2.f * PI;
                while (camera.yaw < -PI) camera.yaw += 2.f * PI;

                // Pitch is unchanged: moving along the surface doesn't tilt
                // the camera up or down relative to the local horizon.
            }
        }
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
    camera.pitch  = std::max(-1.5f, std::min(1.5f, camera.pitch));

    // Keep yaw in [-pi, pi]
    const float PI = 3.14159265f;
    while (camera.yaw >  PI) camera.yaw -= 2.f * PI;
    while (camera.yaw < -PI) camera.yaw += 2.f * PI;
}

// ── onMouseScroll ─────────────────────────────────────────────────────────────
// Mouse wheel scrolls radially (zoom in/out relative to planet surface).
// Positive delta = scroll up = move away from planet.
void Renderer::onMouseScroll(float delta) {
    scrollDelta += delta;
}

// ── onKey ─────────────────────────────────────────────────────────────────────
// Stores key state (1=down, 0=up) for movement keys.
// W/S = forward/backward  A/D = strafe  F/R = radial  Q/E = yaw rotation
void Renderer::onKey(int vk, bool down) {
    float v = down ? 1.f : 0.f;
    switch (vk) {
        case 'W': moveKeys[0] = v; break;  // forward  (tangential)
        case 'R': moveKeys[1] = v; break;  // backward (tangential)
        case 'A': moveKeys[2] = v; break;  // strafe right
        case 'S': moveKeys[3] = v; break;  // strafe left
        case 'Z': moveKeys[4] = v; break;  // radial out (away from planet)
        case 'X': moveKeys[5] = v; break;  // radial in  (toward planet)
        case 'Q': moveKeys[6] = v; break;  // yaw right (rotate heading clockwise)
        case 'F': moveKeys[7] = v; break;  // yaw left  (rotate heading counter-clockwise)
    }
}

// ── screenToTerrain  ─────────────────────────────────────────
// Shoots a ray from the camera through the screen pixel. Intersects with a
// sphere of radius (planet_radius + max_height_scale) to find the approximate
// surface. We then binary-search along the ray for the exact displaced surface.
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

    // Ray–sphere intersection with the outer bounding sphere
    // (planet radius + max height scale, with a small margin).
    const Vec3& pc = g_planet_surface.center;
    float pr = g_planet_surface.radius + g_planet_surface.heightScale + 10.f;

    float ocx = near4.x - pc.x, ocy = near4.y - pc.y, ocz = near4.z - pc.z;
    float b   = ocx*dx + ocy*dy + ocz*dz;
    float c2  = ocx*ocx + ocy*ocy + ocz*ocz - pr*pr;
    float disc = b*b - c2;
    if (disc < 0.f) return false;   // ray misses the planet entirely

    float sqrtDisc = std::sqrt(disc);
    float t0 = -b - sqrtDisc;
    float t1 = -b + sqrtDisc;
    // We want the first positive intersection (entry point)
    float tHit = (t0 > 0.f) ? t0 : t1;
    if (tHit < 0.f) return false;

    // Binary search along the ray between tHit and tHit + 2*height_scale
    // to find where the ray first dips below the displaced surface.
    float lo = std::max(0.f, tHit - g_planet_surface.heightScale);
    float hi = tHit + g_planet_surface.heightScale * 2.f;

    for (int iter = 0; iter < 24; iter++) {
        float mid = (lo + hi) * 0.5f;
        float rx = near4.x + dx*mid;
        float ry = near4.y + dy*mid;
        float rz = near4.z + dz*mid;
        Vec3  rpos = {rx, ry, rz};

        // Check if this point is inside the displaced surface
        Vec3 dir = (rpos - pc).normalised();
        float surfR = g_planet_surface.radius
                    + g_planet_surface.noiseHeight(rpos);
        float rayR  = (rpos - pc).len();

        if (rayR < surfR)
            hi = mid;   // inside surface: hit is earlier
        else
            lo = mid;   // outside: hit is later
    }
    float fx = near4.x + dx * hi;
    float fy = near4.y + dy * hi;
    float fz = near4.z + dz * hi;
    outPos = {fx, fy, fz};

    // Material: use biome colour from noise height
    float h = g_planet_surface.noiseHeight(outPos);
    float normH = (h + g_planet_surface.heightScale * 0.3f)
                / (g_planet_surface.heightScale * 1.3f);
    normH = std::max(0.f, std::min(1.f, normH));
    if      (normH < 0.23f) outMat = 3;   // water
    else if (normH < 0.26f) outMat = 2;   // sand/beach
    else if (normH < 0.56f) outMat = 0;   // grass
    else if (normH < 0.75f) outMat = 1;   // rock
    else                    outMat = 4;   // snow

    return true;
}