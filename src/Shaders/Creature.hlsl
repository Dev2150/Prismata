// ── Creature billboard shader ──────────────────────────────────────────────────
// Draws creatures as camera-facing quads using GPU instancing.
// Slot 0 = per-vertex quad corners; slot 1 = per-creature position/colour/size.

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

struct VIn {
    float2 quadPos  : POSITION;    // [-0.5, 0.5] local quad corner (per vertex)
    float3 worldPos : INST_POS;    // creature world position (per instance)
    float  yaw      : INST_YAW;    // creature heading (unused in shader)
    float4 color    : INST_COLOR;  // RGBA colour (per instance)
    float  size     : INST_SIZE;   // billboard world size (per instance)
    float3 pad      : INST_PAD;    // alignment padding
};
struct VOut {
    float4 sv      : SV_POSITION;
    float4 col     : COLOR;
    float3 wpos    : TEXCOORD0;
    float3 nrm     : TEXCOORD1;   // surface normal at creature position (outward from planet)
};

// Planet centre — hardcoded to match App.cpp PlanetConfig.
// (Can't pass it via a separate cbuffer in this shader without a bigger refactor,
//  so it's embedded. Change if planet centre changes.)
static const float3 PLANET_CENTER = float3(0.f, -1800.f, 0.f);

VOut VSMain(VIn v) {
    // Build a camera-facing coordinate frame at the creature's position.
    // right = cross(worldUp, toCam), up = cross(toCam, right).
    // Expanding the 2D quad corner along these vectors makes the billboard
    // always face the camera regardless of camera angle.
    float3 toCam = camPos.xyz - v.worldPos;
    float  camDist = length(toCam);
    if (camDist < 0.001f) {
        VOut o; o.sv=float4(0,0,2,1); o.col=float4(0,0,0,0);
        o.wpos=float3(0,0,0); o.nrm=float3(0,1,0);
        return o;
    }
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

    // Surface normal at this creature's position = direction from planet centre outward
    float3 surfNormal = normalize(v.worldPos - PLANET_CENTER);

    VOut o;
    o.sv  = mul(float4(wpos, 1.0f), viewProj);
    o.wpos = wpos;
    o.nrm  = surfNormal;

    // Per-position brightness: dot of outward normal with sun direction.
    // This replicates the planet terrain lighting so creatures match the surface.
    float3 L   = normalize(-lightDir.xyz);
    float  NdL = saturate(dot(surfNormal, L));

    // Ambient floor so creatures on the night side aren't invisible
    float3 ambient = ambientColor.rgb + float3(0.04f, 0.04f, 0.06f);
    float3 litColor = v.color.rgb * (ambient + sunColor.rgb * NdL);

    o.col = float4(litColor, v.color.a);
    return o;
}
float4 PSMain(VOut v) : SV_TARGET { return v.col; }