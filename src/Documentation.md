### PLANET_HLSL
- cbuffer FrameConstants, PlanetConstants
- struct VIn, VOut
- VOut VSMain(VIn v)
- float3 biomeColor(float h)
- float4 PSMain(VOut v) : SV_TARGET

### Planet_ATMO_HSLS
- cbuffer FrameConstants, PlanetConstants
- struct VIn, VOut
- VOut VSAtmo(VIn v)
- float4 PSAtmo(VOUt v): SV_TARGET

### SUN_HSLS
- cbuffer FrameConstants, PlanetConstants
- struct SVIn, SVOut
- SVOut SunVS(SVIn v)
- float4 SunPS(SVOut v) : SV_TARGET