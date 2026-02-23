# Memory management 
- The `ComPtr` smart pointer
  - DirectX APIs are based on COM (Component Object Model), which use reference counting
    - ComPtr is used with DirectX 11 objects 
      - to automatically manage the reference counting of COM interfaces
        - thereby preventing memory leaks and simplifying resource management.

# Graphics
## GPU
- CPU runs on C++ code: one instruction at a time (roughly), smart,
flexible, handles complex logic. It has maybe 8-16 cores.
- GPU is completely different: it has *thousands* of tiny silly cores that
all run the SAME simple program simultaneously. 
  - That program is called a "shader". The GPU is fast at doing the same math on millions of things at
once (like computing the colour of every pixel on screen in parallel).
- The problem: CPU and GPU are separate chips with separate memory.
To draw anything, one must:
  1. Describe your geometry to the GPU (upload vertex positions into a buffer)
  2. Tell the GPU what program to run on that geometry (compile + set shaders)
  3. Tell the GPU how to interpret your data (input layout)
  4. Issue a draw call ("GPU, go draw 6000 triangles using those buffers")
- D3D11 (DirectX 11) is Microsoft's graphics API for talking to the GPU.
Every weirdly-named thing in this file is just a step in that pipeline.

## The rendering pipeline ────────────────────────────────────
- HLSL (High-Level Shader Language) 
  - it's a C-like language that compiles to GPU instructions
- When Draw() is called, the GPU runs this pipeline automatically:
  - The vertex buffer
    - list of XYZ positions
  - Vertex Shader
    - runs once per vertex, on the GPU, in parallel 
    - Transforms 3D world position → 2D screen position. 
    - Written in HLSL
  - Rasterisation
    - GPU figures out which pixels each triangle covers
  - Pixel Shader
    - Runs once per pixel, on the GPU, in parallel 
    - Decides the final colour of each pixel 
    - Written in HLSL
  - Back buffer
    - A texture in GPU memory that becomes the seen image

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