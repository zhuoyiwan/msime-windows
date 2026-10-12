#pragma once
#include <cstdint>

struct WallpaperPalette
{
    std::uint32_t accent;
    std::uint32_t surface;
    std::uint32_t card;
    std::uint32_t border;
    std::uint32_t text;
    std::uint32_t secondary;
    std::uint32_t selected;
};

WallpaperPalette MakeWallpaperPalette(std::uint32_t seed, bool light);
// 只解码缩小后的壁纸；路径/修改时间没变时复用色板，检查间隔十秒。
WallpaperPalette GetWallpaperPalette(bool light);
std::uint32_t GetWallpaperSeed();
