#pragma once

// ── Renderer_Shaders.h ────────────────────────────────────────────────────────
// All HLSL shader source strings live here so the .cpp files stay readable.
// HLSL is a C-like language that compiles to GPU bytecode at runtime via
// D3DCompile(). Each string may contain multiple entry points (functions).
// cbuffer FrameConstants must exactly match the C++ struct in Renderer.hpp.

// ── Terrain shader ────────────────────────────────────────────────────────────
// Draws the terrain mesh with Lambertian (diffuse) lighting and fog-of-war.
static const char* TERRAIN_HLSL = R"HLSL(
cbuffer FrameConstants : register(b0) {
    float4x4 viewProj;   // combined View*Projection matrix — projects 3D → 2D screen
    float4   camPos;     // camera world position (xyz)
    float4   lightDir;      // FROM sun TOWARD scene (shader negates for dot product)
    float4   fowData;       // xyz=player pos, w=radius (0=disabled)
    float4   sunColor;      // rgb=sun tint, w=timeOfDay [0,1]
    float4   ambientColor;  // rgb=sky/ambient, w=unused
};

struct VIn {
    float3 pos : POSITION;
    float3 nrm : NORMAL;
    float4 col : COLOR;
};
struct VOut {
    float4 sv   : SV_POSITION;  // required: final clip-space position
    float3 wpos : TEXCOORD0;    // world-space position (for fog calc)
    float3 nrm  : TEXCOORD1;    // surface normal (for lighting)
    float4 col  : TEXCOORD2;    // vertex colour (interpolated across triangle)
};

VOut VSMain(VIn v) {
    VOut o;
    o.sv   = mul(float4(v.pos, 1.0f), viewProj);
    o.wpos = v.pos;
    o.nrm  = v.nrm;
    o.col  = v.col;
    return o;
}

float4 PSMain(VOut v) : SV_TARGET {
    // Lambertian lighting: brightness = max(0, dot(normal, light_direction))
    // L = direction FROM surface TOWARD sun
    float3 L   = normalize(-lightDir.xyz);
    float  ndl = saturate(dot(normalize(v.nrm), L));

    // Two-term lighting: ambient (sky) + sun (directional)
    float3 lit = v.col.rgb * (ambientColor.rgb + sunColor.rgb * ndl);

    // Fog of war: darken terrain outside the player's vision radius
    if (fowData.w > 0.0f) {
        float d = length(v.wpos.xz - fowData.xz);
        float f = saturate((d - fowData.w * 0.8f) / (fowData.w * 0.2f + 0.001f));
        // At night blend toward darker tone; during day blend toward grey
        float3 dark = lerp(float3(0.01f, 0.01f, 0.04f),
                           float3(0.05f, 0.05f, 0.08f),
                           saturate(sunColor.w * 2.0f - 0.5f));
        lit = lerp(lit, dark, f * f);
    }
    return float4(lit, 1.0f);
}
)HLSL";

// ── Creature billboard shader ──────────────────────────────────────────────────
// Draws creatures as camera-facing quads using GPU instancing.
// Slot 0 = per-vertex quad corners; slot 1 = per-creature position/colour/size.
static const char* CREATURE_HLSL = R"HLSL(
cbuffer FrameConstants : register(b0) {
    float4x4 viewProj;
    float4   camPos;
    float4   lightDir;
    float4   fowData;
    float4   sunColor;
    float4   ambientColor;
};

struct VIn {
    float2 quadPos  : POSITION;    // [-0.5, 0.5] local quad corner (per vertex)
    float3 worldPos : INST_POS;    // creature world position (per instance)
    float  yaw      : INST_YAW;    // creature heading (unused in shader)
    float4 color    : INST_COLOR;  // RGBA colour (per instance)
    float  size     : INST_SIZE;   // billboard world size (per instance)
    float3 pad      : INST_PAD;    // alignment padding
};
struct VOut { float4 sv : SV_POSITION; float4 col : COLOR; };

VOut VSMain(VIn v) {
    // Build a camera-facing coordinate frame at the creature's position.
    // right = cross(worldUp, toCam), up = cross(toCam, right).
    // Expanding the 2D quad corner along these vectors makes the billboard
    // always face the camera regardless of camera angle.
    float3 toCam = camPos.xyz - v.worldPos;
    float  camDist = length(toCam);
    if (camDist < 0.001f) { VOut o; o.sv=float4(0,0,2,1); o.col=float4(0,0,0,0); return o; }
    toCam /= camDist;

    float3 worldUp = float3(0,1,0);
    float3 right = cross(worldUp, toCam);
    float  rLen  = length(right);
    if (rLen < 0.01f) right = float3(1,0,0);
    else               right /= rLen;
    float3 up = cross(toCam, right);

    float3 wpos = v.worldPos
                + right * v.quadPos.x * v.size
                + up    * v.quadPos.y * v.size;
    VOut o;
    o.sv  = mul(float4(wpos, 1.0f), viewProj);

    // Scale creature colour by time-of-day brightness so they darken at night.
    // Use a minimum of ambientColor to avoid completely black billboards.
    float brightness = saturate(dot(ambientColor.rgb, float3(0.299f, 0.587f, 0.114f)) * 3.0f + 0.15f);
    o.col = float4(v.color.rgb * brightness, v.color.a);
    return o;
}
float4 PSMain(VOut v) : SV_TARGET { return v.col; }
)HLSL";

// ── Simple / Water / FOV shader ────────────────────────────────────────────────
// Shared cbuffer and input layout. Two VS entry points:
//   VSMain     – plain passthrough, used for FOV cone.
//   WaterVSMain – adds a multi-octave sine wave in Y, used for the water plane.
// Two PS entry points:
//   WaterPS – translucent blue with depth-fade toward the camera.
//   FovPS   – translucent yellow.
static const char* SIMPLE_HLSL = R"HLSL(
cbuffer FrameConstants : register(b0) {
    float4x4 viewProj;
    float4   camPos;
    float4   lightDir;
    float4   fowData;
    float4   sunColor;
    float4   ambientColor;  // w = simTime in seconds (used for water wave animation)
};
struct VIn  { float3 pos : POSITION; };
struct VOut { float4 sv  : SV_POSITION; float3 wpos : TEXCOORD0; };

// ── Plain VS (FOV cone) ───────────────────────────────────────────────────────
VOut VSMain(VIn v) {
    VOut o;
    o.sv = mul(float4(v.pos, 1.0f), viewProj);
    o.wpos = v.pos;
    return o;
}

// ── Water VS ──────────────────────────────────────────────────────────────────
// Adds three overlapping sine waves to create a gentle, organic water surface.
// Using two spatial frequencies + one diagonal wave breaks the flat-plane look
// even with the sparse vertex count of the water quad.
VOut WaterVSMain(VIn v) {
    float t   = ambientColor.w;   // simTime in seconds
    float x   = v.pos.x;
    float z   = v.pos.z;

    // Primary swell: long, slow wave travelling diagonally
    float wave  = sin(x * 0.07f + z * 0.05f + t * 0.8f) * 0.18f;
    // Secondary ripple: shorter, faster, perpendicular direction
    wave       += sin(x * 0.13f - z * 0.09f + t * 1.3f) * 0.09f;
    // Tertiary micro-chop: high frequency, low amplitude
    wave       += sin(x * 0.27f + z * 0.22f - t * 1.9f) * 0.04f;

    float3 wpos = float3(v.pos.x, v.pos.y + wave, v.pos.z);
    VOut o;
    o.sv   = mul(float4(wpos, 1.0f), viewProj);
    o.wpos = wpos;
    return o;
}

// ── Pixel shaders ─────────────────────────────────────────────────────────────
float4 WaterPS(VOut v) : SV_TARGET {
    // Subtle brightness based on time of day so water darkens at night
    float dayBrightness = saturate(sunColor.w * 1.6f + 0.2f);
    float3 waterCol = float3(0.08f, 0.35f, 0.72f) * (0.4f + 0.6f * dayBrightness);
    return float4(waterCol, 0.72f);
}

float4 FovPS(VOut v) : SV_TARGET {
    return float4(1.0f, 0.95f, 0.2f, 0.18f);   // translucent yellow
}
)HLSL";
