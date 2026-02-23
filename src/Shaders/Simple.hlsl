// ── Simple / Water / FOV shader ────────────────────────────────────────────────
// Shared cbuffer and input layout. Two VS entry points:
//   VSMain     – plain passthrough, used for FOV cone.
//   WaterVSMain – adds a multi-octave sine wave in Y, used for the water plane.
// Two PS entry points:
//   WaterPS – translucent blue with depth-fade toward the camera.
//   FovPS   – translucent yellow.

cbuffer FrameConstants : register(b0) {
    float4x4 viewProj;
    float4   camPos;
    float4   lightDir;
    float4   fowData;
    float4   fowFacing;
    float4   sunColor;
    float4   ambientColor;  // w = simTime in seconds (used for water wave animation)
    float4   planetCenter;
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
