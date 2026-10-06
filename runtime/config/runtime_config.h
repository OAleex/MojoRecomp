#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace mojorecomp::config {

enum class DisplayMode {
    Windowed,
    Fullscreen,
};

enum class AspectRatio {
    Native16x9,
    Ultrawide21x9,
    SuperUltrawide32x9,
    Ratio16x10,
    Ratio4x3,
};

enum class AntiAliasing {
    Off,
    Fxaa,
    FxaaExtreme,
};

struct RuntimeConfig {
    uint32_t schemaVersion = 1;
    std::string localizationProfile = "en";
    uint32_t xboxLanguage = 1;
    DisplayMode displayMode = DisplayMode::Windowed;
    std::string monitor = "primary";
    std::string outputResolution = "desktop";
    uint32_t resolutionScale = 1;
    AspectRatio aspectRatio = AspectRatio::Native16x9;
    // Preserve the current timing-safe COT baseline when no launcher config exists.
    bool vsync = false;
    AntiAliasing antiAliasing = AntiAliasing::FxaaExtreme;
    // 0 means Default. Other accepted values are 1, 2, 4, 8 and 16.
    uint32_t textureFiltering = 8;
};

struct RuntimeConfigOverrides {
    std::optional<DisplayMode> displayMode;
    std::optional<uint32_t> resolutionScale;
    std::optional<AspectRatio> aspectRatio;
    std::optional<bool> vsync;
    std::optional<AntiAliasing> antiAliasing;
    std::optional<uint32_t> textureFiltering;
};

bool LoadFile(const std::filesystem::path& path, RuntimeConfig& out, std::string& error);
bool ApplyEnvironmentOverrides(RuntimeConfig& config, std::string& error);
void ApplyOverrides(RuntimeConfig& config, const RuntimeConfigOverrides& overrides);

void Set(const RuntimeConfig& config);
const RuntimeConfig& Get();

bool ParseDisplayMode(const std::string& value, DisplayMode& out);
bool ParseAspectRatio(const std::string& value, AspectRatio& out);
bool ParseAntiAliasing(const std::string& value, AntiAliasing& out);
bool ParseTextureFiltering(const std::string& value, uint32_t& out);
bool ParseBool(const std::string& value, bool& out);
bool ParseLocalizationProfile(const std::string& value, std::string& profile,
                              uint32_t& xboxLanguage);

const char* ToString(DisplayMode value);
const char* ToString(AspectRatio value);
const char* ToString(AntiAliasing value);
const char* TextureFilteringToString(uint32_t value);

} // namespace mojorecomp::config
