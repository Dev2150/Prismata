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
    float4 texParams;
};

struct SVIn  { float2 quadPos : POSITION; };
struct SVOut {
    float4 sv        : SV_POSITION;
    float2 uv        : TEXCOORD0;   // -1..1 within the billboard
};

SVOut SunVS(SVIn v) {
    // Place the sun very far away in the direction toward it
    static const float SUN_DIST = 500000.0f;
    static const float SUN_SIZE = 320000.0f;   // world-unit radius of the billboard

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