// ── STAR_HLSL ─────────────────────────────────────────────────────────────────
// Procedural twinkling starfield. Reuses the atmosphere sphere mesh but pushes
// it to the far clip plane.
// Key design:
//   Direction is computed from planet centre → vertex so the star sphere
//     covers the full sky uniformly regardless of where the planet sits in
//     world space (fixes clustering near the sun / south pole).
//   The billboard is centred on the CAMERA, not the planet, so stars don't
//     drift as the player moves around the surface.
//   Twinkling uses simTime; direction is stable so positions never shift.
//   nightFactor fades stars out during the day using the sun elevation angle
//     computed from timeOfDay (sunColor.w).

cbuffer FrameConstants : register(b0) {
    float4x4 viewProj;
    float4   camPos;
    float4   lightDir;
    float4   fowData;
    float4   fowFacing;
    float4   sunColor;      // w = timeOfDay [0,1]
    float4   ambientColor;  // w = simTime (seconds)
    float4   planetCenter;  // xyz = planet centre, w = radius
};

struct VIn { float3 pos : POSITION; };
struct VOut {
    float4 sv  : SV_POSITION;
    float3 dir : TEXCOORD0;
};

VOut StarVS(VIn v) {
    VOut o;
    // Calculate direction from planet center to the vertex.
    // This direction is stable regardless of camera position.
    float3 dir = normalize(v.pos - planetCenter.xyz);

    // THE FIX FOR JITTER:
    // We pass '0.0' as the W component (4th value).
    // In matrix math, this tells the GPU to treat this as a "Direction", not a "Position".
    // This causes the ViewMatrix to ignore the Camera Position (Translation) entirely.
    // We only apply Rotation + Projection.
    // This creates an "Infinite Skybox" that never jitters, regardless of planet size.
    float4 clipPos = mul(float4(dir, 0.0f), viewProj);

    // Force to far plane
    clipPos.z = clipPos.w * 0.999999f;

    o.sv = clipPos;
    o.dir = dir;
    return o;
}

// 2D Hash
float2 hash2(float2 p) {
    float3 p3 = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.xx+p3.yz)*p3.zy);
}

float4 StarPS(VOut v) : SV_TARGET {
    float3 dir = normalize(v.dir);

    // CUBE MAP PROJECTION
    // Instead of a 3D grid (which creates voids/zones), we project the sphere
    // onto a cube. This ensures uniform star coverage everywhere.
    float3 adir = abs(dir);
    float maxAxis = max(max(adir.x, adir.y), adir.z);

    float2 uv;
    if (adir.x == maxAxis)      uv = dir.yz / dir.x;
    else if (adir.y == maxAxis) uv = dir.xz / dir.y;
    else                        uv = dir.xy / dir.z;

    // Grid generation
    float scale = 400.0f;
    float2 grid = uv * scale;
    float2 cell = floor(grid);
    float2 local = frac(grid) - 0.5f;

    // Randomness
    float2 h = hash2(cell);

    float star = 0.0f;
    // Probability of a star in this cell
    if (h.x > -1.0f) {
        // Random offset within cell
        float2 offset = (h - 0.5f) * 0.7f;
        float d = length(local - offset);

        // Draw star
        star = smoothstep(0.12f, 0.0f, d);

        // Brightness variation
        star *= (0.4f + 0.6f * h.y);
    }

    // Twinkling: unique phase per star, driven by simTime.
    // float time    = ambientColor.w * 2.5f;
    // float phase   = h3.y * 6.2831853f;
    // float twinkle = sin(time + phase) * 0.5f + 0.5f;
    // star *= 0.25f + 0.75f * twinkle;

    // Day/night fade based on LOCAL sun elevation (camera's position on the planet)
    float3 L = normalize(-lightDir.xyz);
    float3 camNormal = normalize(camPos.xyz - planetCenter.xyz);
    float sunElev = dot(camNormal, L);

    // <0 = sun below local horizon
    // Transitions from visible to invisible
    float nightFactor = smoothstep(1.f, -1.0f, sunElev);

    // Star tint (blue-white to yellow-white)
    float3 tint = lerp(float3(0.7f, 0.85f, 1.0f), float3(1.0f, 0.9f, 0.7f), h.y);

    float brightness = star * nightFactor;
    return float4(tint * brightness, brightness);
}
