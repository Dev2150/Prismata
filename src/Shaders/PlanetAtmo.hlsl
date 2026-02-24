// ── PLANET_ATMO_HLSL ──────────────────────────────────────────────────────────
// Atmosphere shell: a slightly larger transparent sphere with limb brightening.
// Also uses the per-fragment N·L so the atmosphere glows on the sunlit side
// and fades to near-black on the shadow side.

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
    float3 viewDir = normalize(v.wpos - camPos.xyz);
    float3 L   = normalize(-lightDir.xyz);

    float planetRadius = planetCenter.w;
    float atmoRadius = planetRadius * 1.3f;
    float camDist = length(camPos.xyz - planetCenter.xyz);

    // --- Space View ---
    float3 rayOrigin = camPos.xyz;
    float3 toCenter = planetCenter.xyz - rayOrigin;
    float tClosest = dot(toCenter, viewDir);
    float3 closestPoint = rayOrigin + viewDir * tClosest;
    float distToCenter = length(closestPoint - planetCenter.xyz);

    float spaceAlpha = 0.0f;
    if (distToCenter < planetRadius) {
        float r = distToCenter / planetRadius;
        spaceAlpha = lerp(0.1f, 1.0f, pow(r, 4.0f));
    } else {
        float haloThickness = atmoRadius - planetRadius;
        float r = saturate((distToCenter - planetRadius) / haloThickness);
        spaceAlpha = pow(1.0f - r, 2.0f);
    }

    float3 effectiveNrm = normalize(closestPoint - planetCenter.xyz);
    float NdL_space = saturate(dot(effectiveNrm, L));
    float litFactor = NdL_space * 0.8f + 0.05f;
    float3 spaceCol = atmosphereColor.rgb * litFactor;
    float darkFade = smoothstep(-0.2f, 0.2f, dot(effectiveNrm, L));
    spaceAlpha *= darkFade;

    // --- Ground View ---
    float3 localUp = normalize(camPos.xyz - planetCenter.xyz);
    float VdUp = dot(viewDir, localUp);
    float sunElev = dot(localUp, L);
    float VdL = dot(viewDir, L);

    float3 zenithCol = atmosphereColor.rgb * 0.4f;
    float3 horizonCol = atmosphereColor.rgb * 1.2f;
    float3 sunsetCol = float3(1.0f, 0.5f, 0.2f);

    float horizonBlend = 1.0f - saturate(VdUp);
    horizonBlend = pow(horizonBlend, 3.0f);

    float3 skyCol = lerp(zenithCol, horizonCol, horizonBlend);

    float sunsetBlend = smoothstep(-0.2f, 0.2f, sunElev) * smoothstep(0.4f, 0.0f, sunElev);
    float sunDirBlend = pow(saturate(VdL * 0.5f + 0.5f), 4.0f);

    skyCol = lerp(skyCol, sunsetCol, sunsetBlend * sunDirBlend * horizonBlend);

    float dayBrightness = smoothstep(-0.35f, 0.05f, sunElev);
    float3 groundCol = skyCol * dayBrightness;
    float groundAlpha = lerp(0.6f, 0.98f, horizonBlend) * dayBrightness;

    float glare = pow(saturate(VdL), 16.0f);
    groundCol += sunColor.rgb * glare * dayBrightness * 0.5f;

    // --- Blend ---
    float spaceBlend = smoothstep(planetRadius * 1.05f, planetRadius * 1.25f, camDist);

    float3 finalCol = lerp(groundCol, spaceCol, spaceBlend);
    float finalAlpha = lerp(groundAlpha, spaceAlpha, spaceBlend);

    return float4(finalCol, finalAlpha);
}