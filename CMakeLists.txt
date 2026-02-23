cmake_minimum_required(VERSION 3.20)
project(KyberPlanet LANGUAGES CXX)

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
    src/UI/Notifications.cpp
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
    src/Renderer/Renderer_planets.cpp
    src/Renderer/Renderer_Init.cpp
    src/Renderer/Renderer_Overlays.cpp
    src/Renderer/Planet/PlanetQuadTree.cpp
    src/Renderer/Planet/PlanetRenderer.cpp
    src/Renderer/Planet/PlanetTextureLoader.hpp

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
)

add_executable(KyberPlanet WIN32 ${SOURCES})

target_include_directories(KyberPlanet PRIVATE
    src/
    imgui/
    imgui/backends/
    implot/
)

# ── Windows / DirectX definitions ─────────────────────────────────────────────
target_compile_definitions(KyberPlanet PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    _WIN32_WINNT=0x0601
    WIN32_LEAN_AND_MEAN
    TRACY_ENABLE
    TRACY_ON_DEMAND
    TRACY_CALLSTACK=8 # TRACY_NO_SYSTEM_TRACING
)

# ── DirectX 11 + Win32 libs ───────────────────────────────────────────────────
target_link_libraries(KyberPlanet PRIVATE
    d3d11
    dxgi
    d3dcompiler
    dwmapi
)

add_library(TracyClient tracy/public/TracyClient.cpp)
target_include_directories(TracyClient PUBLIC tracy/public)
target_compile_definitions(TracyClient PUBLIC
        TRACY_ENABLE
        TRACY_ON_DEMAND
        TRACY_CALLSTACK=8 # TRACY_NO_SYSTEM_TRACING
)
target_link_libraries(TracyClient PUBLIC ws2_32 dbghelp secur32)

target_link_libraries(KyberPlanet PRIVATE TracyClient)


if(MINGW)
    target_link_libraries(KyberPlanet PRIVATE dxguid uuid)
    target_link_options(KyberPlanet PRIVATE -static-libgcc -static-libstdc++ -static)
endif()

# ── Compiler flags ────────────────────────────────────────────────────────────
if(MSVC)
    target_compile_options(KyberPlanet PRIVATE
            /W4
            /WX-
            $<$<CONFIG:Release>:/O2 /fp:fast /Zi /Gy /Oy-> # Added /Oy- (Disable Frame Pointer Omission)
            $<$<CONFIG:Debug>:/Zi /Ob0 /Od /RTC1>
    )
    # Required to emit the PDB file in Release and enable profiling
    target_link_options(KyberPlanet PRIVATE
            $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>
            $<$<CONFIG:Debug>:/DEBUG>
    )

    # Apply the same logic to TracyClient so it has proper symbols
    target_compile_options(TracyClient PRIVATE /Zi /Oy-)
else()
    # GCC / MinGW (CLion default)
    target_compile_options(KyberPlanet PRIVATE
        -Wall
        $<$<CONFIG:Release>:-O3 -march=native -ffast-math -g -fno-omit-frame-pointer>
        $<$<CONFIG:Debug>:-g -Og>
    )
endif()
