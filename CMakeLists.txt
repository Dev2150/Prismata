cmake_minimum_required(VERSION 3.20)
project(Prismata LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Sources ────────────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/App/App.cpp
    src/App/App_Globals.cpp
    src/App/App_D3D.cpp
    src/App/App_WndProc.cpp
    src/Core/RNG.hpp
    src/Sim/Creature.cpp
    src/UI/SimUI.cpp
    src/World/World_Species.cpp
    src/World/World_Tick.cpp
    src/World/World_IO.cpp
    src/World/World_Planet.cpp
    src/World/World_Gen.cpp
    src/World/World_Terrain.cpp
    src/World/World_Entities.cpp
    src/World/World_Perceive.cpp
    src/Renderer/Renderer_Camera.cpp
    src/Renderer/Renderer_Creatures.cpp
    src/Renderer/Renderer_Frame.cpp
    src/Renderer/Renderer_Init.cpp
    src/Renderer/Renderer_Overlays.cpp
    src/Renderer/Planet/PlanetQuadTree.cpp
    src/Renderer/Planet/PlanetRenderer.cpp

    # ImGui – place the imgui/ folder at the project root
    imgui/imgui.cpp
    imgui/imgui_draw.cpp
    imgui/imgui_tables.cpp
    imgui/imgui_widgets.cpp
    imgui/imgui_demo.cpp
    imgui/backends/imgui_impl_win32.cpp
    imgui/backends/imgui_impl_dx11.cpp

    # ImPlot – place the implot/ folder at the project root
    implot/implot.cpp
    implot/implot_items.cpp
    implot/implot_demo.cpp
        src/Renderer/Renderer_planets.cpp
)

add_executable(Prismata WIN32 ${SOURCES})

target_include_directories(Prismata PRIVATE
    src/
    imgui/
    imgui/backends/
    implot/
)

# ── Windows / DirectX definitions ─────────────────────────────────────────────
target_compile_definitions(Prismata PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    _WIN32_WINNT=0x0601
    WIN32_LEAN_AND_MEAN
)

# ── DirectX 11 + Win32 libs ───────────────────────────────────────────────────
target_link_libraries(Prismata PRIVATE
    d3d11
    dxgi
    d3dcompiler
    dwmapi
)

if(MINGW)
    target_link_libraries(Prismata PRIVATE dxguid uuid)
endif()

# ── Compiler flags ────────────────────────────────────────────────────────────
if(MSVC)
    target_compile_options(Prismata PRIVATE
        $<$<CONFIG:Release>:/O2 /fp:fast>
        $<$<CONFIG:Debug>:/W4 /WX->
    )
else()
    # GCC / MinGW (CLion default)
    target_compile_options(Prismata PRIVATE
        -Wall
        $<$<CONFIG:Release>:-O2 -ffast-math>
        $<$<CONFIG:Debug>:-g -Og>
    )
endif()
