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
    float4   sunColor;      // rgb=sun tint, w=timeOfDay [0,1]
    float4   ambientColor;  // rgb=sky/ambient, w=simTime
};

// ── Planet-specific per-draw constants ────────────────────────────────────────
cbuffer PlanetConstants : register(b1) {
    float4 planetCenter;   // xyz = world-space planet centre, w = radius
    float4 atmosphereColor;// rgb = atmosphere tint, w = atmosphere thickness (world units)
    float4 planetParams;   // x = seaLevel (world Y), y = snowLine fraction, zw = unused
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

// ── Biome colour from height ──────────────────────────────────────────────────
// Maps normalised height [0,1] to a surface colour.
// Uses smooth transitions so biome boundaries aren't hard edges.
float3 biomeColor(float h) {
    // Colour keyframes: [height, r, g, b]
    float3 deepOcean   = float3(0.02f, 0.07f, 0.25f);
    float3 shallowSea  = float3(0.06f, 0.25f, 0.55f);
    float3 beach       = float3(0.76f, 0.70f, 0.50f);
    float3 lowland     = float3(0.22f, 0.48f, 0.14f);
    float3 highland    = float3(0.35f, 0.30f, 0.22f);
    float3 rock        = float3(0.45f, 0.42f, 0.40f);
    float3 snow        = float3(0.90f, 0.92f, 0.95f);

    // Sea level sits at h ≈ 0.23 (sea floor fraction 0.3, normalised)
    const float seaH    = 0.23f;
    const float beachH  = 0.26f;
    const float lowH    = 0.32f;
    const float highH   = 0.56f;
    const float rockH   = 0.75f;
    const float snowH   = 0.85f;

    float3 col;
    if      (h < seaH)   col = lerp(deepOcean,  shallowSea, saturate((h)              / seaH));
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
    float3 L   = normalize(-lightDir.xyz);   // direction FROM surface TOWARD sun
    float  NdL = saturate(dot(N, L));

    // Biome base colour
    float3 baseCol = biomeColor(v.height);

    // Lambertian + ambient lighting (same as world terrain)
    float3 lit = baseCol * (ambientColor.rgb + sunColor.rgb * NdL);

    // ── Atmosphere haze ───────────────────────────────────────────────────────
    // Exponential depth fog in the atmosphere colour.
    // At large distances (space view) the planet surface blends into the
    // atmospheric limb colour. atmosphereColor.w = effective thickness in world units.
    float atmThick = atmosphereColor.w;
    if (atmThick > 1.f) {
        float fogFactor = 1.f - exp(-v.camDist / atmThick);
        lit = lerp(lit, atmosphereColor.rgb * (ambientColor.rgb + sunColor.rgb * 0.4f),
                   fogFactor * 0.55f);
    }

    // ── Specular (ocean only) ─────────────────────────────────────────────────
    // Add a simple Blinn-Phong specular highlight on ocean tiles so they
    // glitter in the sunlight.
    if (v.height < 0.25f) {
        float3 V   = normalize(camPos.xyz - v.wpos);
        float3 H   = normalize(L + V);
        float  spec= pow(saturate(dot(N, H)), 64.f);
        lit += sunColor.rgb * spec * 0.6f * (0.25f - v.height) / 0.25f;
    }

    // ── Fog of war (identical to world terrain) ───────────────────────────────
    if (fowData.w > 0.f) {
        float d = length(v.wpos.xz - fowData.xz);
        float f = saturate((d - fowData.w * 0.8f) / (fowData.w * 0.2f + 0.001f));
        float3 dark = float3(0.01f, 0.01f, 0.04f);
        lit = lerp(lit, dark, f * f);
    }

    return float4(lit, 1.0f);
}

)HLSL";

// ── PLANET_ATMO_HLSL ──────────────────────────────────────────────────────────
// Optional atmosphere shell: a slightly larger sphere rendered with additive
// blending to produce a glowing limb effect when viewing from space.
// Drawn AFTER the planet surface, blended additively.

static const char* PLANET_ATMO_HLSL = R"HLSL(

cbuffer FrameConstants : register(b0) {
    float4x4 viewProj;
    float4   camPos;
    float4   lightDir;
    float4   fowData;
    float4   sunColor;
    float4   ambientColor;
};

cbuffer PlanetConstants : register(b1) {
    float4 planetCenter;
    float4 atmosphereColor;
    float4 planetParams;
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
    float  NdL = saturate(dot(v.nrm, L)) * 0.6f + 0.2f;

    float3 atmoCol = atmosphereColor.rgb * NdL;
    float  alpha   = fresnel * 0.55f;

    return float4(atmoCol, alpha);
}

)HLSL";
