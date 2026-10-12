#include "utils/wallpaper_palette.h"

#include <Windows.h>
#include <ShObjIdl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <mutex>
#include <string>

namespace
{
using Microsoft::WRL::ComPtr;
struct Hsl
{
    double hue;
    double saturation;
    double lightness;
};

Hsl ToHsl(std::uint32_t rgb)
{
    const double r = ((rgb >> 16) & 255) / 255.0;
    const double g = ((rgb >> 8) & 255) / 255.0;
    const double b = (rgb & 255) / 255.0;
    const double high = (std::max)({r, g, b});
    const double low = (std::min)({r, g, b});
    const double delta = high - low;
    Hsl value{0, 0, (high + low) / 2};
    if (delta > 0.0001)
    {
        value.saturation = delta / (1 - std::abs(2 * value.lightness - 1));
        value.hue = high == r   ? std::fmod((g - b) / delta + 6, 6)
                    : high == g ? (b - r) / delta + 2
                                : (r - g) / delta + 4;
        value.hue /= 6;
    }
    return value;
}

std::uint32_t Tone(double hue, double saturation, double lightness)
{
    const double chroma = (1 - std::abs(2 * lightness - 1)) * saturation;
    const double x = chroma * (1 - std::abs(std::fmod(hue * 6, 2) - 1));
    const double offset = lightness - chroma / 2;
    const int sector = static_cast<int>(hue * 6) % 6;
    const std::array<std::array<double, 3>, 6> channels = {
        {{chroma, x, 0}, {x, chroma, 0}, {0, chroma, x}, {0, x, chroma}, {x, 0, chroma}, {chroma, 0, x}}};
    const auto byte = [&](double value) { return static_cast<std::uint32_t>(std::lround((value + offset) * 255)); };
    return (byte(channels[sector][0]) << 16) | (byte(channels[sector][1]) << 8) | byte(channels[sector][2]);
}

struct ComScope
{
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComScope()
    {
        if (SUCCEEDED(result))
            CoUninitialize();
    }
};

std::wstring WallpaperPath()
{
    ComPtr<IDesktopWallpaper> desktop;
    if (SUCCEEDED(CoCreateInstance(CLSID_DesktopWallpaper, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&desktop))))
    {
        LPWSTR monitor = nullptr;
        LPWSTR path = nullptr;
        // 首个显示器是统一主题的取色来源，避免跨屏时键盘与设置各用一套色。
        desktop->GetMonitorDevicePathAt(0, &monitor);
        desktop->GetWallpaper(monitor, &path);
        const std::wstring result = path ? path : L"";
        CoTaskMemFree(monitor);
        CoTaskMemFree(path);
        if (!result.empty())
            return result;
    }
    std::array<wchar_t, 32768> path{};
    if (SystemParametersInfoW(SPI_GETDESKWALLPAPER, static_cast<UINT>(path.size()), path.data(), 0))
        return path.data();
    return {};
}

std::uint32_t ReadSeed(const std::wstring &path)
{
    constexpr std::uint32_t fallback = 0x6079A8;
    if (path.empty())
        return fallback;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                  &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(frame.Get(), 64, 64, WICBitmapInterpolationModeFant)) ||
        FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)))
        return fallback;
    std::array<BYTE, 64 * 64 * 4> pixels{};
    if (FAILED(converter->CopyPixels(nullptr, 64 * 4, static_cast<UINT>(pixels.size()), pixels.data())))
        return fallback;
    struct Bucket
    {
        double score = 0;
        unsigned r = 0, g = 0, b = 0, count = 0;
    };
    std::array<Bucket, 4096> buckets{};
    for (size_t index = 0; index < pixels.size(); index += 4)
    {
        const unsigned b = pixels[index], g = pixels[index + 1], r = pixels[index + 2];
        if (pixels[index + 3] < 128)
            continue;
        const Hsl hsl = ToHsl((r << 16) | (g << 8) | b);
        if (hsl.lightness < 0.08 || hsl.lightness > 0.92)
            continue;
        auto &bucket = buckets[((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4)];
        bucket.score += 0.15 + hsl.saturation;
        bucket.r += r;
        bucket.g += g;
        bucket.b += b;
        ++bucket.count;
    }
    const auto best = std::max_element(buckets.begin(), buckets.end(),
                                       [](const Bucket &a, const Bucket &b) { return a.score < b.score; });
    return best->count ? ((best->r / best->count) << 16) | ((best->g / best->count) << 8) | (best->b / best->count)
                       : fallback;
}
} // namespace

WallpaperPalette MakeWallpaperPalette(std::uint32_t seed, bool light)
{
    const Hsl hsl = ToHsl(seed);
    const double saturation = (std::clamp)(hsl.saturation, 0.18, 0.50);
    const auto tone = [&](double s, double l) { return Tone(hsl.hue, s, l); };
    return {tone(saturation, light ? 0.34 : 0.76),
            tone(0.10, light ? 0.97 : 0.08),
            tone(0.10, light ? 0.94 : 0.12),
            tone(0.10, light ? 0.78 : 0.28),
            tone(0.08, light ? 0.12 : 0.93),
            tone(0.08, light ? 0.36 : 0.72),
            tone(saturation * 0.5, light ? 0.87 : 0.23)};
}

std::uint32_t GetWallpaperSeed()
{
    static std::mutex mutex;
    static ULONGLONG checked = 0;
    static std::wstring previousPath;
    static std::filesystem::file_time_type previousTime{};
    static std::uint32_t seed = 0x6079A8;
    std::lock_guard lock(mutex);
    const ULONGLONG now = GetTickCount64();
    if (checked && now - checked < 10000)
        return seed;
    checked = now;
    ComScope com;
    const std::wstring path = WallpaperPath();
    std::error_code error;
    const auto modified = std::filesystem::last_write_time(path, error);
    if (path != previousPath || (!error && modified != previousTime))
    {
        previousPath = path;
        previousTime = modified;
        seed = ReadSeed(path);
    }
    return seed;
}

WallpaperPalette GetWallpaperPalette(bool light)
{
    return MakeWallpaperPalette(GetWallpaperSeed(), light);
}
