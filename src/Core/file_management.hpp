#pragma once
#include <filesystem>

inline bool fileExists(const std::wstring& filename) {
    return std::filesystem::exists(filename);
}