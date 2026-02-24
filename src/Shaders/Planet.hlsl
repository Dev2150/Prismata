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
// PLANET_HLSL now supports 16 texture slots (4 biomes × 4 maps each):
//   t0–t3   Colour    (sRGB)
//   t4–t7   NormalGL  (linear, OpenGL Y-up convention)
//   t8–t11  AO        (linear)
//   t12–t15 Roughness (linear)
//
// Triplanar mapping is used to avoid UV seams on the sphere. The three
// world-axis projections (XY, YZ, XZ) are blended by the absolute normal.
// A textures-loaded flag (texParams.y) lets the shader fall back to the
// original procedural biome colours when no textures are present.

// ── Shared frame constants ────────────────────────────────────────────────────
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
    float4 texParams;         // xyz = unit vector scene→sun, w = elevation [-1..1]
};

// ── Terrain texture slots ─────────────────────────────────────────────────────
// Colour maps are sRGB; everything else is linear.
Texture2D texColorGrass     : register(t0);
Texture2D texColorSand      : register(t1);
Texture2D texColorRock      : register(t2);
Texture2D texColorSnow      : register(t3);

Texture2D texNormalGrass    : register(t4);
Texture2D texNormalSand     : register(t5);
Texture2D texNormalRock     : register(t6);
Texture2D texNormalSnow     : register(t7);

Texture2D texAOGrass        : register(t8);
Texture2D texAOSand         : register(t9);
Texture2D texAORock         : register(t10);
Texture2D texAOSnow         : register(t11);

Texture2D texRoughGrass     : register(t12);
Texture2D texRoughSand      : register(t13);
Texture2D texRoughRock      : register(t14);
Texture2D texRoughSnow      : register(t15);

SamplerState texSampler     : register(s0);

// ── Vertex I/O ────────────────────────────────────────────────────────────────
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

// ── Triplanar sampling helper ─────────────────────────────────────────────────
// Samples a texture three times along world-aligned planes and blends
// by the absolute surface normal.  `scale` controls world-units per tile.
// Returns the blended RGBA sample.
float4 triplanar(Texture2D tex, SamplerState samp,
                 float3 wpos, float3 N, float scale)
{
    float3 blendW = abs(N);
    // Sharpen the blend with a power so only the dominant axis contributes heavily
    blendW = pow(blendW, 4.0f);
    blendW /= (blendW.x + blendW.y + blendW.z + 1e-5f);

    float4 xProj = tex.Sample(samp, wpos.yz * scale);
    float4 yProj = tex.Sample(samp, wpos.zx * scale);
    float4 zProj = tex.Sample(samp, wpos.xy * scale);

    return xProj * blendW.x + yProj * blendW.y + zProj * blendW.z;
}

// ── Triplanar normal sampling ─────────────────────────────────────────────────
// Decodes three tangent-space normal map samples and re-orients each into
// world space using the dominant axis frame, then blends.
//
// Technique: "Whiteout blend" triplanar normal mapping.
//   Each face projects its XY normal components onto the corresponding world axes.
//   Using the whiteout blend (rather than reoriented normal mapping) is simpler
//   and works well enough for geo-scale terrain.
float3 triplanarNormal(Texture2D normTex, SamplerState samp,
                       float3 wpos, float3 N, float scale)
{
    float3 blendW = abs(N);
    blendW = pow(blendW, 4.0f);
    blendW /= (blendW.x + blendW.y + blendW.z + 1e-5f);

    // Sample all three projections; decode from [0,1] to [-1,+1]
    float3 nX = normTex.Sample(samp, wpos.yz * scale).rgb * 2.0f - 1.0f;
    float3 nY = normTex.Sample(samp, wpos.zx * scale).rgb * 2.0f - 1.0f;
    float3 nZ = normTex.Sample(samp, wpos.xy * scale).rgb * 2.0f - 1.0f;

    // Reorient each tangent-space normal into world space, accounting for negative axes
    float3 wsX = float3(nX.z * sign(N.x), nX.x * sign(N.x), nX.y);
    float3 wsY = float3(nY.y, nY.z * sign(N.y), nY.x * sign(N.y));
    float3 wsZ = float3(nZ.x * sign(N.z), nZ.y, nZ.z * sign(N.z));

    return normalize(wsX * blendW.x + wsY * blendW.y + wsZ * blendW.z);
}

// ── Procedural biome colour fallback ─────────────────────────────────────────
// Kept from the original shader; used when textures are not loaded.
float3 biomeColor(float h) {
    // Colour keyframes: [height, r, g, b]
    float3 deepOcean   = float3(0.02f, 0.06f, 0.22f);
    float3 shallowSea  = float3(0.05f, 0.20f, 0.48f);
    float3 beach       = float3(0.76f, 0.70f, 0.50f);
    float3 lowland     = float3(0.22f, 0.48f, 0.14f);
    float3 highland    = float3(0.35f, 0.30f, 0.22f);
    float3 rock        = float3(0.45f, 0.42f, 0.40f);
    float3 snow        = float3(0.90f, 0.92f, 0.95f);

    const float seaH   = 0.23f, beachH = 0.27f, lowH  = 0.32f;
    const float highH  = 0.56f, rockH  = 0.75f, snowH = 0.85f;

    if (h < seaH)        return lerp(deepOcean, shallowSea, smoothstep(0.0f, seaH, h));
    else if (h < beachH) return lerp(shallowSea, beach,      saturate((h - seaH)   / (beachH - seaH)));
    else if (h < lowH)   return lerp(beach,      lowland,    saturate((h - beachH) / (lowH   - beachH)));
    else if (h < highH)  return lerp(lowland,    highland,   saturate((h - lowH)   / (highH  - lowH)));
    else if (h < rockH)  return lerp(highland,   rock,       saturate((h - highH)  / (rockH  - highH)));
    else if (h < snowH)  return lerp(rock,       snow,       saturate((h - rockH)  / (snowH  - rockH)));
    else                 return snow;
}

// ── Biome weight computation ──────────────────────────────────────────────────
// Returns a float4 of blend weights for [grass, sand, rock, snow].
// Transitions are soft (8% blend bands) to hide texture seams at biome edges.
float4 biomeWeights(float h) {
    // Height thresholds (same as biomeColor for visual consistency)
    // Sand/beach:  0.23 – 0.30
    // Grass:       0.30 – 0.55
    // Rock:        0.55 – 0.85
    // Snow:        0.85 – 1.0
    // Below 0.23 = ocean — rendered with procedural colour only (no texture blend)

    const float bandW = 0.06f;   // blend band width

    float wGrass = saturate((h - 0.26f) / bandW) * saturate((0.58f - h) / bandW);
    float wSand  = saturate((h - 0.23f) / bandW) * saturate((0.32f - h) / bandW);
    float wRock  = saturate((h - 0.52f) / bandW) * saturate((0.88f - h) / bandW);
    float wSnow  = saturate((h - 0.80f) / bandW);

    // Normalise so weights always sum to 1 (avoids dark seams)
    float total = wGrass + wSand + wRock + wSnow + 1e-5f;
    return float4(wGrass, wSand, wRock, wSnow) / total;
}

// ── PS ────────────────────────────────────────────────────────────────────────
float4 PSMain(VOut v) : SV_TARGET {
    float3 N   = normalize(v.nrm);

    // Sun direction: lightDir points FROM the sun TOWARD the scene.
    // Negate it to get the direction FROM the surface TOWARD the sun.
    float3 L = normalize(-lightDir.xyz);

    bool useTextures = (texParams.y > 0.5f);
    float scale = texParams.x;

    // Calculate the procedural color first.
    // This ensures we have the correct "Water" color available to blend against.
    float3 procCol = biomeColor(v.height);

    float3 baseCol;
    float  roughness = 0.7f;
    float  ao        = 1.0f;
    float3 shadingN  = N;   // normal used for lighting (may be replaced by normal map)

    // Check if it is high enough to potentially have land textures
    if (useTextures && v.height >= 0.23f) {
        // ── Compute per-biome triplanar samples ───────────────────────────────
        float4 W = biomeWeights(v.height);

        // Colour
        float3 colGrass = triplanar(texColorGrass, texSampler, v.wpos, N, scale).rgb;
        float3 colSand  = triplanar(texColorSand,  texSampler, v.wpos, N, scale).rgb;
        float3 colRock  = triplanar(texColorRock,  texSampler, v.wpos, N, scale).rgb;
        float3 colSnow  = triplanar(texColorSnow,  texSampler, v.wpos, N, scale).rgb;
        // Blend colour
        float3 texCol   = colGrass * W.x + colSand * W.y + colRock * W.z + colSnow * W.w;

        // AO
        float aoGrass = triplanar(texAOGrass, texSampler, v.wpos, N, scale).r;
        float aoSand  = triplanar(texAOSand,  texSampler, v.wpos, N, scale).r;
        float aoRock  = triplanar(texAORock,  texSampler, v.wpos, N, scale).r;
        float aoSnow  = triplanar(texAOSnow,  texSampler, v.wpos, N, scale).r;
        float texAO   = aoGrass * W.x + aoSand * W.y + aoRock * W.z + aoSnow * W.w;

        // Roughness
        float roughGrass = triplanar(texRoughGrass, texSampler, v.wpos, N, scale).r;
        float roughSand  = triplanar(texRoughSand,  texSampler, v.wpos, N, scale).r;
        float roughRock  = triplanar(texRoughRock,  texSampler, v.wpos, N, scale).r;
        float roughSnow  = triplanar(texRoughSnow,  texSampler, v.wpos, N, scale).r;
        float texRough   = roughGrass * W.x + roughSand * W.y + roughRock * W.z + roughSnow * W.w;

        // Normal map — blend in world space then combine with vertex normal
        float3 nmGrass = triplanarNormal(texNormalGrass, texSampler, v.wpos, N, scale);
        float3 nmSand  = triplanarNormal(texNormalSand,  texSampler, v.wpos, N, scale);
        float3 nmRock  = triplanarNormal(texNormalRock,  texSampler, v.wpos, N, scale);
        float3 nmSnow  = triplanarNormal(texNormalSnow,  texSampler, v.wpos, N, scale);
        float3 texNorm = normalize(nmGrass * W.x + nmSand * W.y + nmRock * W.z + nmSnow * W.w);

        // BLEND PROCEDURAL WATER WITH TEXTURED LAND ---

        // Create a blend factor that is 0.0 at height 0.22 and 1.0 at height 0.24
        // This creates a smooth transition zone at the shoreline.
        float shoreBlend = smoothstep(0.23f, 0.27f, v.height);

        // Lerp everything based on this shoreBlend
        baseCol   = lerp(procCol, texCol,   shoreBlend);
        ao        = lerp(1.0f,    texAO,    shoreBlend);

        // Water is glossy (0.15), Land is rough (texRough)
        roughness = lerp(0.15f,   texRough, shoreBlend);

        // Blend normals: Water uses geometry normal (N), Land uses normal map (texNorm)
        // We also keep your existing NM_STRENGTH logic for the land part
        float3 landN = normalize(lerp(N, texNorm, 0.6f));
        shadingN     = normalize(lerp(N, landN,   shoreBlend));

    } else {
        // Fallback: Deep water or no textures loaded
        baseCol   = procCol;

        // Procedural roughness logic
        roughness = (v.height < 0.23f) ? 0.15f   // water = glossy
                  : (v.height > 0.80f) ? 0.85f   // snow  = matte
                  : 0.65f;
    }

    // ── Lighting ──────────────────────────────────────────────────────────────
    float NdL = saturate(dot(shadingN, L));

    float3 nightAmbient = float3(0.02f, 0.025f, 0.04f);
    float3 ambient = (ambientColor.rgb + nightAmbient) * ao;

    float3 lit = baseCol * (ambient + sunColor.rgb * NdL);

    // ── Specular ──────────────────────────────────────────────────────────────
    // Specular exponent: roughness 0 (mirror) → exponent 512; roughness 1 (matte) → 2
    if (NdL > 0.f) {
        float3 V   = normalize(camPos.xyz - v.wpos);
        float3 H   = normalize(L + V);
        float  NdH = saturate(dot(shadingN, H));

        // Map roughness [0,1] → exponent [512, 2] (rough = diffuse, smooth = specular)
        float specExp = exp2(lerp(9.f, 1.f, roughness));  // 2^9=512 … 2^1=2
        float spec    = pow(NdH, specExp);

        // Water and polished rock get stronger specular; rough terrain gets almost none
        float specMask = (1.0f - roughness) * (1.0f - roughness);
        lit += sunColor.rgb * spec * specMask * 0.5f;
    }

    // ── Atmosphere haze ───────────────────────────────────────────────────────
    float atmThick = atmosphereColor.w;
    if (atmThick > 1.f) {
        float fogFactor = 1.f - exp(-v.camDist / atmThick);
        // Haze colour is tinted by sunlight on the lit side, dark on the shadow side
        float3 hazeCol = atmosphereColor.rgb * (ambient + sunColor.rgb * 0.4f * NdL);
        lit = lerp(lit, hazeCol, fogFactor * 0.55f);
    }

    // ── Fog of war ────────────────────────────────────────────────────────────
    if (fowData.w > 0.f) {
        float3 toPixel = v.wpos - fowData.xyz;
        float d = length(toPixel);
        bool inFOV = false;

        if (d <= fowData.w) {
            if (d < 0.1f) { inFOV = true; }
            else {
                float cosA = dot(normalize(toPixel), fowFacing.xyz);
                if (cosA >= fowFacing.w) inFOV = true;
            }
        }
        if (!inFOV) lit = float3(0, 0, 0);
    }

    return float4(lit, 1.0f);
}