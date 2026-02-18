cmake_minimum_required(VERSION 3.26)
project(CritterCode)

set(CMAKE_CXX_STANDARD 20)

set(SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
include_directories(${SRC_DIR})

add_executable(CritterCode
        ${SRC_DIR}/main.cpp
        ${SRC_DIR}/imgui.cpp
        ${SRC_DIR}/imgui_demo.cpp
        ${SRC_DIR}/imgui_draw.cpp
        ${SRC_DIR}/imgui_tables.cpp
        ${SRC_DIR}/imgui_widgets.cpp
        ${SRC_DIR}/imgui_impl_win32.cpp
        ${SRC_DIR}/imgui_impl_dx11.cpp
)

# Link the DirectX 11 Libraries
# d3d11: The main DX11 library
# d3dcompiler: Needed for ImGui to compile its shaders
# dxgi: Needed for the swap chain (handling the window display)
target_link_libraries(CritterCode d3d11 d3dcompiler dxgi dwmapi)