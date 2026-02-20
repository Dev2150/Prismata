#pragma once

// ── Renderer_Shaders.h ────────────────────────────────────────────────────────
// All HLSL shader source strings live here so the .cpp files stay readable.
// HLSL is a C-like language that compiles to GPU bytecode at runtime via
// D3DCompile(). Each string may contain multiple entry points (functions).

// ── Terrain shader ────────────────────────────────────────────────────────────
// Draws the terrain mesh with Lambertian (diffuse) lighting and fog-of-war.
static const char* TERRAIN_HLSL = R"HLSL(
cbuffer FrameConstants : register(b0) {
    float4x4 viewProj;   // combined View*Projection matrix — projects 3D → 2D screen
    float4   camPos;     // camera world position (xyz)
    float4   lightDir;   // sun direction vector (xyz)
    float4   fowData;    // fog of war: xyz=player pos, w=radius (0=disabled)
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
    float3 L   = normalize(-lightDir.xyz);
    float  ndl = saturate(dot(normalize(v.nrm), L));
    float3 lit = v.col.rgb * (0.25f + 0.75f * ndl);  // 0.25 ambient + 0.75 diffuse

    // Fog of war: darken terrain outside the player's vision radius
    if (fowData.w > 0.0f) {
        float d = length(v.wpos.xz - fowData.xz);
        float f = saturate((d - fowData.w * 0.8f) / (fowData.w * 0.2f + 0.001f));
        lit = lerp(lit, float3(0.02f, 0.02f, 0.05f), f * f);
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
    o.col = v.color;
    return o;
}
float4 PSMain(VOut v) : SV_TARGET { return v.col; }
)HLSL";

// ── Simple position-only shader ────────────────────────────────────────────────
// Shared by the water plane and the FOV cone.
// One vertex shader, two pixel shaders (chosen by the CPU at draw time).
static const char* SIMPLE_HLSL = R"HLSL(
cbuffer FrameConstants : register(b0) {
    float4x4 viewProj;
    float4   camPos;
    float4   lightDir;
    float4   fowData;
};
struct VIn  { float3 pos : POSITION; };
struct VOut { float4 sv  : SV_POSITION; };

VOut VSMain(VIn v) {
    VOut o;
    o.sv = mul(float4(v.pos, 1.0f), viewProj);
    return o;
}
float4 WaterPS(VOut v) : SV_TARGET { return float4(0.08f, 0.35f, 0.72f, 0.78f); }  // translucent blue
float4 FovPS  (VOut v) : SV_TARGET { return float4(1.0f,  0.95f, 0.2f,  0.18f); }  // translucent yellow
)HLSL";
