#include "runtime_config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <type_traits>

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-literal-operator"
#endif
#include <toml++/toml.hpp>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace mojorecomp::config {
namespace {

RuntimeConfig g_config{};
std::mutex g_configMutex;

std::string Lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool ValidateScale(uint32_t value)
{
    return value >= 1 && value <= 3;
}

bool ValidateFiltering(uint32_t value)
{
    return value == 0 || value == 1 || value == 2 || value == 4 ||
           value == 8 || value == 16;
}

template <typename T>
bool ReadString(const toml::table& table, std::string_view key, T& out,
                bool (*parser)(const std::string&, T&), std::string& error)
{
    const auto value = table[key].value<std::string>();
    if (!value)
        return true;
    if (!parser(*value, out))
    {
        error = "Invalid value for '" + std::string(key) + "': " + *value;
        return false;
    }
    return true;
}

bool ApplyEnvString(const char* name, auto parser, auto& target, std::string& error)
{
    const char* value = std::getenv(name);
    if (!value || !*value)
        return true;
    using Value = std::remove_reference_t<decltype(target)>;
    Value parsed{};
    if (!parser(std::string(value), parsed))
    {
        error = std::string("Invalid ") + name + " value: " + value;
        return false;
    }
    target = parsed;
    return true;
}

} // namespace

bool ParseDisplayMode(const std::string& value, DisplayMode& out)
{
    const std::string v = Lower(value);
    if (v == "windowed")
    {
        out = DisplayMode::Windowed;
        return true;
    }
    if (v == "fullscreen" || v == "borderless" || v == "borderless_fullscreen")
    {
        out = DisplayMode::Fullscreen;
        return true;
    }
    return false;
}

bool ParseAspectRatio(const std::string& value, AspectRatio& out)
{
    const std::string v = Lower(value);
    if (v == "16:9" || v == "16x9" || v == "native" || v == "native16:9")
        out = AspectRatio::Native16x9;
    else if (v == "21:9" || v == "21x9" || v == "ultrawide")
        out = AspectRatio::Ultrawide21x9;
    else if (v == "32:9" || v == "32x9" || v == "super_ultrawide" || v == "superultrawide")
        out = AspectRatio::SuperUltrawide32x9;
    else if (v == "16:10" || v == "16x10")
        out = AspectRatio::Ratio16x10;
    else if (v == "4:3" || v == "4x3")
        out = AspectRatio::Ratio4x3;
    else
        return false;
    return true;
}

bool ParseAntiAliasing(const std::string& value, AntiAliasing& out)
{
    const std::string v = Lower(value);
    if (v == "off" || v == "none")
        out = AntiAliasing::Off;
    else if (v == "fxaa")
        out = AntiAliasing::Fxaa;
    else if (v == "fxaa_extreme" || v == "fxaa-extreme" || v == "fxaa extreme")
        out = AntiAliasing::FxaaExtreme;
    else
        return false;
    return true;
}

bool ParseTextureFiltering(const std::string& value, uint32_t& out)
{
    const std::string v = Lower(value);
    if (v == "default")
    {
        out = 0;
        return true;
    }
    std::string numeric = v;
    if (!numeric.empty() && numeric.back() == 'x')
        numeric.pop_back();
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(numeric.c_str(), &end, 10);
    if (end == numeric.c_str() || *end != '\0' || !ValidateFiltering(static_cast<uint32_t>(parsed)))
        return false;
    out = static_cast<uint32_t>(parsed);
    return true;
}

bool ParseBool(const std::string& value, bool& out)
{
    const std::string v = Lower(value);
    if (v == "1" || v == "true" || v == "yes" || v == "on")
    {
        out = true;
        return true;
    }
    if (v == "0" || v == "false" || v == "no" || v == "off")
    {
        out = false;
        return true;
    }
    return false;
}

bool ParseLocalizationProfile(const std::string& value, std::string& profile,
                              uint32_t& xboxLanguage)
{
    const std::string v = Lower(value);
    if (v == "en")
    {
        profile = "en";
        xboxLanguage = 1;
        return true;
    }
    if (v == "de")
    {
        profile = "de";
        xboxLanguage = 3;
        return true;
    }
    if (v == "fr")
    {
        profile = "fr";
        xboxLanguage = 4;
        return true;
    }
    if (v == "es")
    {
        profile = "es";
        xboxLanguage = 5;
        return true;
    }
    if (v == "it")
    {
        profile = "it";
        xboxLanguage = 6;
        return true;
    }
    if (v == "nl")
    {
        profile = "nl";
        xboxLanguage = 16;
        return true;
    }
    if (v == "pt-br" || v == "pt_br")
    {
        profile = "pt-BR";
        // COT's PT-BR localization pack replaces the English text resources while
        // deliberately retaining the English voice/content path.
        xboxLanguage = 1;
        return true;
    }
    return false;
}

bool LoadFile(const std::filesystem::path& path, RuntimeConfig& out, std::string& error)
{
    try
    {
        const toml::table root = toml::parse_file(path.string());
        RuntimeConfig loaded{};
        if (const auto schema = root["schema_version"].value<int64_t>())
        {
            if (*schema != 1)
            {
                error = "Unsupported config schema_version: " + std::to_string(*schema);
                return false;
            }
            loaded.schemaVersion = static_cast<uint32_t>(*schema);
        }

        if (const toml::table* localization = root["localization"].as_table())
        {
            if (const auto profile = (*localization)["profile"].value<std::string>())
            {
                if (!ParseLocalizationProfile(*profile, loaded.localizationProfile,
                                              loaded.xboxLanguage))
                {
                    error = "Unsupported localization.profile: " + *profile;
                    return false;
                }
            }
            if (const auto language = (*localization)["xbox_language"].value<int64_t>())
            {
                if (*language < 1 || *language > 16 ||
                    static_cast<uint32_t>(*language) != loaded.xboxLanguage)
                {
                    error = "localization.xbox_language does not match the selected profile";
                    return false;
                }
            }
        }

        if (const toml::table* display = root["display"].as_table())
        {
            if (!ReadString(*display, "mode", loaded.displayMode, ParseDisplayMode, error) ||
                !ReadString(*display, "aspect_ratio", loaded.aspectRatio, ParseAspectRatio, error))
                return false;
            if (const auto monitor = (*display)["monitor"].value<std::string>())
                loaded.monitor = *monitor;
            if (const auto resolution = (*display)["output_resolution"].value<std::string>())
                loaded.outputResolution = *resolution;
            if (const auto scale = (*display)["resolution_scale"].value<int64_t>())
            {
                if (*scale < 1 || *scale > 3)
                {
                    error = "display.resolution_scale must be 1, 2 or 3";
                    return false;
                }
                loaded.resolutionScale = static_cast<uint32_t>(*scale);
            }
            if (const auto vsync = (*display)["vsync"].value<bool>())
                loaded.vsync = *vsync;
        }

        if (const toml::table* graphics = root["graphics"].as_table())
        {
            if (!ReadString(*graphics, "anti_aliasing", loaded.antiAliasing,
                            ParseAntiAliasing, error))
                return false;
            if (const auto filtering = (*graphics)["texture_filtering"].value<std::string>())
            {
                if (!ParseTextureFiltering(*filtering, loaded.textureFiltering))
                {
                    error = "Invalid graphics.texture_filtering value: " + *filtering;
                    return false;
                }
            }
            else if (const auto filteringInt = (*graphics)["texture_filtering"].value<int64_t>())
            {
                if (*filteringInt < 0 ||
                    !ValidateFiltering(static_cast<uint32_t>(*filteringInt)))
                {
                    error = "graphics.texture_filtering must be default, 1, 2, 4, 8 or 16";
                    return false;
                }
                loaded.textureFiltering = static_cast<uint32_t>(*filteringInt);
            }
        }

        out = std::move(loaded);
        return true;
    }
    catch (const toml::parse_error& parseError)
    {
        error = "Could not parse config TOML: " + std::string(parseError.description());
        return false;
    }
    catch (const std::exception& exception)
    {
        error = "Could not load config: " + std::string(exception.what());
        return false;
    }
}

bool ApplyEnvironmentOverrides(RuntimeConfig& config, std::string& error)
{
    if (!ApplyEnvString("MOJORECOMP_DISPLAY_MODE", ParseDisplayMode, config.displayMode, error) ||
        !ApplyEnvString("MOJORECOMP_ASPECT_RATIO", ParseAspectRatio, config.aspectRatio, error) ||
        !ApplyEnvString("MOJORECOMP_VSYNC", ParseBool, config.vsync, error) ||
        !ApplyEnvString("MOJORECOMP_ANTI_ALIASING", ParseAntiAliasing, config.antiAliasing, error) ||
        !ApplyEnvString("MOJORECOMP_TEXTURE_FILTERING", ParseTextureFiltering,
                        config.textureFiltering, error))
        return false;

    if (const char* value = std::getenv("MOJORECOMP_LOCALIZATION_PROFILE"); value && *value)
    {
        if (!ParseLocalizationProfile(value, config.localizationProfile, config.xboxLanguage))
        {
            error = std::string("Invalid MOJORECOMP_LOCALIZATION_PROFILE value: ") + value;
            return false;
        }
    }

    if (const char* value = std::getenv("MOJORECOMP_RESOLUTION_SCALE"); value && *value)
    {
        char* end = nullptr;
        const unsigned long parsed = std::strtoul(value, &end, 10);
        if (end == value || *end != '\0' || !ValidateScale(static_cast<uint32_t>(parsed)))
        {
            error = std::string("Invalid MOJORECOMP_RESOLUTION_SCALE value: ") + value;
            return false;
        }
        config.resolutionScale = static_cast<uint32_t>(parsed);
    }
    return true;
}

void ApplyOverrides(RuntimeConfig& config, const RuntimeConfigOverrides& overrides)
{
    if (overrides.displayMode) config.displayMode = *overrides.displayMode;
    if (overrides.resolutionScale) config.resolutionScale = *overrides.resolutionScale;
    if (overrides.aspectRatio) config.aspectRatio = *overrides.aspectRatio;
    if (overrides.vsync) config.vsync = *overrides.vsync;
    if (overrides.antiAliasing) config.antiAliasing = *overrides.antiAliasing;
    if (overrides.textureFiltering) config.textureFiltering = *overrides.textureFiltering;
}

void Set(const RuntimeConfig& config)
{
    std::lock_guard lock(g_configMutex);
    g_config = config;
}

const RuntimeConfig& Get()
{
    return g_config;
}

const char* ToString(DisplayMode value)
{
    return value == DisplayMode::Fullscreen ? "fullscreen" : "windowed";
}

const char* ToString(AspectRatio value)
{
    switch (value)
    {
        case AspectRatio::Ultrawide21x9: return "21:9";
        case AspectRatio::SuperUltrawide32x9: return "32:9";
        case AspectRatio::Ratio16x10: return "16:10";
        case AspectRatio::Ratio4x3: return "4:3";
        default: return "16:9";
    }
}

const char* ToString(AntiAliasing value)
{
    switch (value)
    {
        case AntiAliasing::Fxaa: return "fxaa";
        case AntiAliasing::FxaaExtreme: return "fxaa_extreme";
        default: return "off";
    }
}

const char* TextureFilteringToString(uint32_t value)
{
    switch (value)
    {
        case 1: return "1x";
        case 2: return "2x";
        case 4: return "4x";
        case 8: return "8x";
        case 16: return "16x";
        default: return "default";
    }
}

} // namespace mojorecomp::config
