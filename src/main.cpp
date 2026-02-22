// KyberPlanet – Evolution Simulation
// Entry point: delegates everything to App.cpp / RunApplication().
#include <windows.h>
#include "App/App.hpp"

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nShowCmd)
{
    return RunApplication();
}
