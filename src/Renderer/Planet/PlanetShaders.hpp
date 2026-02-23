#pragma once
// ── PlanetShaders.hpp ─────────────────────────────────────────────────────────
// HLSL source strings for planet rendering.
// Shares the same cbuffer layout as the existing FrameConstants so no extra
// constant buffer is needed. Adds a second per-draw cbuffer for planet-specific
// data (planet centre, radius).

// ── PLANET_HLSL ───────────────────────────────────────────────────────────────
// Shared by the main terrain shader. Draws planet patches with:
//  • Lambertian + ambient lighting (same sun model as the world terrain)
//  • Procedural biome colouring driven by the `height` vertex channel:
//      < 0   = deep ocean (dark blue)
//      0-0.1 = shallow / beach (cyan → sand)
//      0.1-0.4 = lowland (green)
//      0.4-0.75 = highland (brown → grey rock)
//      > 0.75 = snow cap (white)
//  • Fog of war (same as world terrain shader, w=0 to disable)
//  • Atmosphere haze (simple depth-based blue haze for space view)

static const char* PLANET_HLSL = R"HLSL(

// ── Shared frame constants (identical layout to the world FrameConstants) ─────
cbuffer FrameConstants : register(b0) {
    float4x4 viewProj;
    float4   camPos;
    float4   lightDir;      // FROM sun TOWARD scene
    float4   fowData;
    float4   fowFacing;
    float4   sunColor;      // rgb=sun tint, w=timeOfDay [0,1]
    float4   ambientColor;  // rgb=sky/ambient, w=simTime
    float4   planetCenter;  // xyz = world-space planet centre, w = radius
};

// ── Planet-specific per-draw constants ────────────────────────────────────────
cbuffer PlanetConstants : register(b1) {
    float4 atmosphereColor;// rgb = atmosphere tint, w = atmosphere thickness (world units)
    float4 planetParams;   // x = seaLevel (world Y), y = snowLine fraction, zw = unused
    float4 sunInfo;         // xyz = unit vector scene→sun, w = elevation [-1..1]
};

struct VIn {
    float3 pos    : POSITION;   // world-space displaced vertex
    float3 nrm    : NORMAL;     // surface normal
    float2 uv     : TEXCOORD0;  // [0,1]² patch UV
    float  height : TEXCOORD1;  // normalised height [0,1]: 0=sea floor, 1=max peak
    float  pad    : TEXCOORD2;
};

struct VOut {
    float4 sv       : SV_POSITION;
    float3 wpos     : TEXCOORD0;   // world position for lighting
    float3 nrm      : TEXCOORD1;   // surface normal
    float  height   : TEXCOORD2;   // normalised height for biome blending
    float  camDist  : TEXCOORD3;   // distance to camera (for atmosphere haze)
};

// ── Vertex shader ─────────────────────────────────────────────────────────────
VOut VSMain(VIn v) {
    VOut o;
    o.sv      = mul(float4(v.pos, 1.0f), viewProj);
    o.wpos    = v.pos;
    o.nrm     = normalize(v.nrm);
    o.height  = v.height;
    o.camDist = length(camPos.xyz - v.pos);
    return o;
}

// ── Biome colour from normalised height ────────────────────────────────────────
float3 biomeColor(float h) {
    // Colour keyframes: [height, r, g, b]
    float3 deepOcean   = float3(0.02f, 0.07f, 0.25f);
    float3 shallowSea  = float3(0.06f, 0.25f, 0.55f);
    float3 beach       = float3(0.76f, 0.70f, 0.50f);
    float3 lowland     = float3(0.22f, 0.48f, 0.14f);
    float3 highland    = float3(0.35f, 0.30f, 0.22f);
    float3 rock        = float3(0.45f, 0.42f, 0.40f);
    float3 snow        = float3(0.90f, 0.92f, 0.95f);

    const float seaH   = 0.23f, beachH = 0.26f, lowH  = 0.32f;
    const float highH  = 0.56f, rockH  = 0.75f, snowH = 0.85f;

    float3 col;
    if      (h < seaH)   col = lerp(deepOcean,  shallowSea, saturate(h / seaH));
    else if (h < beachH) col = lerp(shallowSea, beach,      saturate((h - seaH)       / (beachH - seaH)));
    else if (h < lowH)   col = lerp(beach,      lowland,    saturate((h - beachH)     / (lowH - beachH)));
    else if (h < highH)  col = lerp(lowland,    highland,   saturate((h - lowH)       / (highH - lowH)));
    else if (h < rockH)  col = lerp(highland,   rock,       saturate((h - highH)      / (rockH - highH)));
    else if (h < snowH)  col = lerp(rock,       snow,       saturate((h - rockH)      / (snowH - rockH)));
    else                 col = snow;

    return col;
}

// ── Pixel shader ──────────────────────────────────────────────────────────────
float4 PSMain(VOut v) : SV_TARGET {
    float3 N   = normalize(v.nrm);

    // Sun direction: lightDir points FROM the sun TOWARD the scene.
    // Negate it to get the direction FROM the surface TOWARD the sun.
    float3 L = normalize(-lightDir.xyz);

    // --- KEY CHANGE: per-fragment N·L ---
    // Each surface point independently determines whether it faces the sun.
    // This creates a proper day/night terminator on the sphere.
    float  NdL = saturate(dot(N, L));

    // Biome base colour
    float3 baseCol = biomeColor(v.height);

    // Night-side glow: a tiny amount of ambient so the dark side isn't pitch black.
    // ambientColor.rgb provides the sky/space ambient (already dark at night).
    float3 nightAmbient = float3(0.02f, 0.025f, 0.04f);  // very faint blue-black
    float3 ambient = ambientColor.rgb + nightAmbient;

    // Direct sunlight contribution — zero on the dark hemisphere automatically
    float3 lit = baseCol * (ambient + sunColor.rgb * NdL);

    // ── Atmosphere depth haze ─────────────────────────────────────────────────
    float atmThick = atmosphereColor.w;
    if (atmThick > 1.f) {
        float fogFactor = 1.f - exp(-v.camDist / atmThick);
        // Haze colour is tinted by sunlight on the lit side, dark on the shadow side
        float3 hazeCol = atmosphereColor.rgb * (ambient + sunColor.rgb * 0.4f * NdL);
        lit = lerp(lit, hazeCol, fogFactor * 0.55f);
    }

    // ── Specular highlight on ocean (sun-side only) ────────────────────────────
    if (v.height < 0.25f && NdL > 0.f) {
        float3 V   = normalize(camPos.xyz - v.wpos);
        float3 H   = normalize(L + V);
        float  spec= pow(saturate(dot(N, H)), 64.f);
        lit += sunColor.rgb * spec * 0.6f * (0.25f - v.height) / 0.25f;
    }

    // ── Fog of war (pure blackness outside FOV) ───────────────────────────────
    if (fowData.w > 0.f) {
        float3 toPixel = v.wpos - fowData.xyz;
        float d = length(toPixel);
        bool inFOV = false;

        if (d <= fowData.w) {
            if (d < 0.1f) {
                inFOV = true;
            } else {
                float cosA = dot(normalize(toPixel), fowFacing.xyz);
                if (cosA >= fowFacing.w) {
                    inFOV = true;
                }
            }
        }

        if (!inFOV) {
            lit = float3(0.0f, 0.0f, 0.0f);
        }
    }

    return float4(lit, 1.0f);
}

)HLSL";

// ── PLANET_ATMO_HLSL ──────────────────────────────────────────────────────────
// Atmosphere shell: a slightly larger transparent sphere with limb brightening.
// Also uses the per-fragment N·L so the atmosphere glows on the sunlit side
// and fades to near-black on the shadow side.

static const char* PLANET_ATMO_HLSL = R"HLSL(

cbuffer FrameConstants : register(b0) {
    float4x4 viewProj;
    float4   camPos;
    float4   lightDir;
    float4   fowData;
    float4   fowFacing;
    float4   sunColor;
    float4   ambientColor;
    float4   planetCenter;
};

cbuffer PlanetConstants : register(b1) {
    float4 atmosphereColor;
    float4 planetParams;
    float4 sunInfo;
};

struct VIn  { float3 pos : POSITION; };
struct VOut {
    float4 sv      : SV_POSITION;
    float3 wpos    : TEXCOORD0;
    float3 nrm     : TEXCOORD1;   // direction from planet centre
};

VOut VSAtmo(VIn v) {
    VOut o;
    o.sv   = mul(float4(v.pos, 1.0f), viewProj);
    o.wpos = v.pos;
    // Normal = direction from planet centre outward
    o.nrm  = normalize(v.pos - planetCenter.xyz);
    return o;
}

float4 PSAtmo(VOut v) : SV_TARGET {
    // Fresnel-like limb brightening: brightest at grazing angle
    float3 V = normalize(camPos.xyz - v.wpos);
    float  fresnel = 1.f - saturate(dot(v.nrm, V));
    fresnel = pow(fresnel, 3.f);  // tighten to the limb

    // Sun side brightens (scattering): lit side has blue scatter, dark side fades
    float3 L   = normalize(-lightDir.xyz);
    float  NdL = saturate(dot(v.nrm, L));

    // Lit side: blue scatter. Shadow side: nearly black with just a hint of scatter.
    float litFactor = NdL * 0.7f + 0.05f;   // small ambient floor so it's not pure black
    float3 atmoCol  = atmosphereColor.rgb * litFactor;

    float  alpha   = fresnel * 0.55f;

    return float4(atmoCol, alpha);
}
)HLSL";

// ── SUN_HLSL ──────────────────────────────────────────────────────────────────
// The sun billboard is ALWAYS rendered (no CPU-side discard based on elevation).
// Visibility is handled entirely in the pixel shader:
//  - The sun disc itself is always drawn in the direction of sunInfo.xyz
//  - A horizon fade culls it smoothly when it goes below the geometric horizon
//    as seen from the CAMERA (not based on planet-relative elevation).
//    This means the sun correctly disappears when the planet terrain blocks it,
//    regardless of which side of the planet the camera is on.
//
// Geometric horizon from camera:
//   cos(horizon_angle) = planet_radius / camera_dist_from_center
//   sun is above horizon when dot(cam_to_planet_center_dir, sun_dir) < horizon_cos
//
static const char* SUN_HLSL = R"HLSL(

cbuffer FrameConstants : register(b0) {
    float4x4 viewProj;
    float4   camPos;
    float4   lightDir;
    float4   fowData;
    float4   fowFacing;
    float4   sunColor;
    float4   ambientColor;
    float4   planetCenter;
};

cbuffer PlanetConstants : register(b1) {
    float4 atmosphereColor;
    float4 planetParams;
};

struct SVIn  { float2 quadPos : POSITION; };
struct SVOut {
    float4 sv        : SV_POSITION;
    float2 uv        : TEXCOORD0;   // -1..1 within the billboard
};

SVOut SunVS(SVIn v) {
    // Place the sun very far away in the direction toward it
    static const float SUN_DIST = 500000.0f;
    static const float SUN_SIZE = 160000.0f;   // world-unit radius of the billboard

    // USE lightDir INSTEAD OF sunInfo! (lightDir points FROM sun, so it's negated)
    float3 sunDir    = normalize(-lightDir.xyz);         // scene→sun
    float3 sunCenter = camPos.xyz + sunDir * SUN_DIST;

    // Camera-facing billboard frame
    // "up" is world Y unless we're looking nearly straight up/down
    float3 worldUp = (abs(sunDir.y) < 0.95f)
                   ? float3(0, 1, 0)
                   : float3(1, 0, 0);
    float3 right = normalize(cross(worldUp, sunDir));
    float3 up    = cross(sunDir, right);

    float3 wpos = sunCenter
                + right * v.quadPos.x * SUN_SIZE
                + up    * v.quadPos.y * SUN_SIZE;

    SVOut o;
    o.sv        = mul(float4(wpos, 1.0f), viewProj);
    o.sv.z      = o.sv.w * 0.9999f; // Push to far plane so terrain always occludes it
    o.uv        = v.quadPos * 2.0f;   // remap [-0.5,0.5] → [-1,1]
    return o;
}

float4 SunPS(SVOut v) : SV_TARGET {
    float d = length(v.uv);
    if (d > 1.0f) discard;

    // Sun disc and corona
    float core = 1.0f - smoothstep(0.0f, 0.12f, d);
    // Soft corona falloff
    float corona = pow(1.0f - d, 4.0f);
    // Wide outer glow
    float glow   = pow(1.0f - d, 2.0f) * 0.4f;

    // Colour: white-hot core, warm orange halo
    float3 coreCol   = float3(1.0f, 0.98f, 0.88f);
    float3 coronaCol = lerp(float3(1.0f, 0.65f, 0.15f),
                            float3(1.0f, 0.90f, 0.60f), core);

    float3 col   = coreCol * core + coronaCol * (corona + glow);
    float  alpha = saturate(core + corona * 0.8f + glow);

    return float4(col, alpha);
}
)HLSL";

// ── STAR_HLSL ─────────────────────────────────────────────────────────────────
// Procedural twinkling starfield. Reuses the atmosphere sphere mesh but pushes
// it to the far clip plane.
static const char* STAR_HLSL = R"HLSL(
cbuffer FrameConstants : register(b0) {
    float4x4 viewProj;
    float4   camPos;
    float4   lightDir;
    float4   fowData;
    float4   fowFacing;
    float4   sunColor;
    float4   ambientColor;
    float4   planetCenter;
};

struct VIn { float3 pos : POSITION; };
struct VOut {
    float4 sv  : SV_POSITION;
    float3 dir : TEXCOORD0;
};

VOut StarVS(VIn v) {
    VOut o;
    // Center the sphere on the camera
    float3 wpos = camPos.xyz + normalize(v.pos) * 1000.0f; 
    o.sv = mul(float4(wpos, 1.0f), viewProj);
    o.sv.z = o.sv.w * 0.9999f; // push to far plane
    o.dir = normalize(v.pos);
    return o;
}

float hash(float3 p) {
    p = frac(p * float3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return frac((p.x + p.y) * p.z);
}

float4 StarPS(VOut v) : SV_TARGET {
    float3 dir = normalize(v.dir);
    float h = hash(dir * 800.0);
    float star = smoothstep(0.99, 1.0, h);
    
    float time = ambientColor.w * 5.0;
    float twinkle = sin(time + hash(dir * 100.0) * 6.28) * 0.5 + 0.5;
    star *= 0.2 + 0.8 * twinkle;
    
    float elev = -cos(sunColor.w * 6.2831853);
    float nightFactor = smoothstep(0.1, -0.1, elev); // 1 at night, 0 during day
    float3 tint = lerp(float3(0.8, 0.9, 1.0), float3(1.0, 0.9, 0.8), hash(dir * 200.0));
    
    return float4(star * tint * nightFactor, star * nightFactor);
}
)HLSL";
