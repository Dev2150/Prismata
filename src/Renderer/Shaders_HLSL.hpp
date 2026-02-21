#pragma once

// ── Renderer_Shaders.h ────────────────────────────────────────────────────────
// All HLSL shader source strings live here so the .cpp files stay readable.
// HLSL is a C-like language that compiles to GPU bytecode at runtime via
// D3DCompile(). Each string may contain multiple entry points (functions).
// cbuffer FrameConstants must exactly match the C++ struct in Renderer.hpp.

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

float4 FovPS(VOut v) : SV_TARGET {
    return float4(1.0f, 0.95f, 0.2f, 0.18f);
}
)HLSL";
